// Remnant x MHW mashup core: read-only research while Jalon plays. GPL-3.0-or-later.
//
// Finds, by watching MHW's memory while Jalon plays, what the next features need and nothing documents:
//  - the hunter's joint matrices (Remnant guns in the hunter's hands, Remnant armor pieces on the hunter);
//  - the counters behind MHW's monster flinches (so Remnant gun damage can build MHW flinches and staggers);
//  - the save slot's hunter / master rank fields.
// Nothing is written to the game. Results go to dev_research.txt in the mod folder.
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "state.h"

namespace rh {

namespace {

bool g_on = false, g_checked = false;
std::string g_text;
SRWLOCK g_textLock = SRWLOCK_INIT;  // outf/flush are called from the game thread and the joint worker

void outf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void outf(const char* fmt, ...) {
    char b[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof(b), fmt, a);
    va_end(a);
    AcquireSRWLockExclusive(&g_textLock);
    g_text += b;
    g_text += "\n";
    ReleaseSRWLockExclusive(&g_textLock);
}
void flush() {
    AcquireSRWLockExclusive(&g_textLock);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\dev_research.txt", mod_dir());
    if (FILE* f = fopen(path, "ab")) {
        fwrite(g_text.data(), 1, g_text.size(), f);
        fclose(f);
    }
    g_text.clear();
    ReleaseSRWLockExclusive(&g_textLock);
}

// ---------------------------------------------------------------- the hunter's joints
// Motion-based: a joint is a matrix near the hunter that moves differently from the hunter's root. The pointer web
// under the player object is walked two levels deep, every orthonormal matrix near the hunter becomes a tracker,
// and trackers are scored over time (see joint_tick).

enum Layout { kRowVec = 0, kColVec = 1, k34 = 2 };
const char* const kLayoutName[3] = {"rv", "cv", "34"};
constexpr int kMaxBlocks = 4000, kMaxTrackers = 8000, kMaxEntries = 400;
constexpr int kL1Bytes = 0x2000, kL2Bytes = 0x800, kScanBudgetUs = 5000;
constexpr double kJointSecs = 600, kJointStep = 0.25, kReportEvery = 30, kRescanEvery = 60;
constexpr int kJointLineCap = 200;

struct Block { uintptr_t p; int l1, l2; size_t readable; };  // readable: bytes readable from p (checked once, at build)  // l1/l2: offsets in the player / in the level-1 object (-1 = none)
struct Tracker {
    uintptr_t addr = 0;
    int blk = 0, layout = 0, stride = 0, entry = 0;
    bool hasLast = false;
    int lastTick = -1, score = 0;
    float last[3] = {0, 0, 0};
    double sum[3] = {0, 0, 0}, sum2 = 0;  // local offset sums and sum of squared lengths
};
struct JointRun {
    int zone = -2;
    double start = 0, nextStep = 0, nextReport = 0, nextRescan = 0;
    int tick = 0, lines = 0, cursor = 0;
    bool built = false, framed = false, capped = false;
    Vec3 lastRoot{}, fwd{0, 0, 1};
    bool haveRoot = false, motionYaw = false;
    float rootDelta[3] = {0, 0, 0};
};
JointRun g_run;
std::vector<Block> g_blocks;
std::vector<Tracker> g_trackers;
std::vector<uint64_t> g_seenPtr, g_seenKey;
std::vector<int> g_rank;

// Capped, per-zone log line.
void jout(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void jout(const char* fmt, ...) {
    if (g_run.lines >= kJointLineCap) {
        if (!g_run.capped) { g_run.capped = true; outf("joint search: line cap reached for this zone"); }
        return;
    }
    g_run.lines++;
    char b[400];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof(b), fmt, a);
    va_end(a);
    outf("%s", b);
}

bool seen_before(std::vector<uint64_t>& v, uint64_t k) {  // v is kept sorted
    auto it = std::lower_bound(v.begin(), v.end(), k);
    if (it != v.end() && *it == k) return true;
    v.insert(it, k);
    return false;
}

// Rotation part orthonormal: rows and columns unit length (+-0.05), rows mutually perpendicular. rs = floats per row.
bool rot_ok(const float* m, int rs) {
    float r[3][3];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r[i][j] = m[i * rs + j];
    for (int i = 0; i < 3; i++) {
        float lr = sqrtf(r[i][0] * r[i][0] + r[i][1] * r[i][1] + r[i][2] * r[i][2]);
        float lc = sqrtf(r[0][i] * r[0][i] + r[1][i] * r[1][i] + r[2][i] * r[2][i]);
        if (!(fabsf(lr - 1) < 0.05f) || !(fabsf(lc - 1) < 0.05f)) return false;
    }
    for (int i = 0; i < 3; i++) {
        int k = (i + 1) % 3;
        if (fabsf(r[i][0] * r[k][0] + r[i][1] * r[k][1] + r[i][2] * r[k][2]) > 0.1f) return false;
    }
    return true;
}

// Reads a matrix of the given layout at addr (the caller has established that the memory is readable); true with its translation when it is a clean transform.
bool read_matrix(uintptr_t addr, int layout, float* t) {
    float m[16];
    size_t n = layout == k34 ? 12 : 16;
    memcpy(m, (void*)addr, n * 4);
    for (size_t i = 0; i < n; i++)
        if (!std::isfinite(m[i])) return false;
    if (layout == kRowVec) {
        if (fabsf(m[3]) > 1e-3f || fabsf(m[7]) > 1e-3f || fabsf(m[11]) > 1e-3f || fabsf(m[15] - 1) > 1e-3f) return false;
        if (!rot_ok(m, 4)) return false;
        t[0] = m[12]; t[1] = m[13]; t[2] = m[14];
    } else if (layout == kColVec) {
        if (fabsf(m[12]) > 1e-3f || fabsf(m[13]) > 1e-3f || fabsf(m[14]) > 1e-3f || fabsf(m[15] - 1) > 1e-3f) return false;
        if (!rot_ok(m, 4)) return false;
        t[0] = m[3]; t[1] = m[7]; t[2] = m[11];
    } else {
        if (!rot_ok(m, 3)) return false;
        t[0] = m[9]; t[1] = m[10]; t[2] = m[11];
    }
    return true;
}

// Level 0 = the player object itself, level 1 = what the player points to, level 2 = what those point to.
void build_blocks(uint8_t* pl) {
    g_blocks.clear();
    g_seenPtr.clear();
    g_blocks.push_back({(uintptr_t)pl, -1, -1, 0});
    seen_before(g_seenPtr, (uintptr_t)pl);
    for (int a = 0; a < kL1Bytes && (int)g_blocks.size() < kMaxBlocks; a += 8) {
        uintptr_t p;
        if (!rd((uintptr_t)pl + a, &p)) { a |= 0xFFF; continue; }
        if (p < 0x10000 || (p & 7) || !mem_readable((void*)p, 64) || seen_before(g_seenPtr, p)) continue;
        g_blocks.push_back({p, a, -1, 0});
    }
    size_t l1End = g_blocks.size();
    for (size_t i = 1; i < l1End; i++) {
        for (int b = 0; b < kL2Bytes && (int)g_blocks.size() < kMaxBlocks; b += 8) {
            uintptr_t p;
            if (!rd(g_blocks[i].p + b, &p)) break;
            if (p < 0x10000 || (p & 7) || !mem_readable((void*)p, 64) || seen_before(g_seenPtr, p)) continue;
            g_blocks.push_back({p, g_blocks[i].l1, b, 0});
        }
    }
    // readability of each block's whole scan range, checked once here (page by page), never in the per-sample scan
    const size_t want = (size_t)kMaxEntries * 0x40 + 0x40;
    for (Block& bk : g_blocks) {
        size_t readable = 0;
        while (readable < want && mem_readable((void*)(bk.p + readable), 1)) readable += 0x1000 - ((bk.p + readable) & 0xFFF);
        bk.readable = readable;
    }
    g_run.cursor = 0;
    g_run.built = true;
    jout("joint search: %d candidate blocks (%d from the player, %d from those)", (int)g_blocks.size(), (int)l1End,
         (int)(g_blocks.size() - l1End));
}

// Adds a tracker for every clean matrix in the block that sits 50-250 cm from the hunter right now.
void scan_block(int bi, const Vec3& root) {
    Block& bk = g_blocks[bi];
    if (!mem_readable((const void*)bk.p, 0x40)) { bk.readable = 0; return; }  // freed by a zone load since the build
    const size_t readable = bk.readable;
    for (int layout = 0; layout < 3; layout++) {
        for (int stride = 0x30; stride <= 0x40; stride += 0x10) {
            for (int e = 0; e < kMaxEntries; e++) {
                size_t off = (size_t)e * stride;
                if (off + 0x40 > readable) break;
                float t[3];
                if (!read_matrix(bk.p + off, layout, t)) continue;
                float dx = t[0] - root.x, dy = t[1] - root.y, dz = t[2] - root.z;
                float d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (!(d >= 50 && d <= 250)) continue;
                // the same address in the same layout is one tracker, whatever the stride that found it
                if (seen_before(g_seenKey, ((uint64_t)(bk.p + off) << 2) | (uint64_t)layout)) continue;
                if ((int)g_trackers.size() >= kMaxTrackers) return;
                Tracker tr;
                tr.addr = bk.p + off;
                tr.blk = bi; tr.layout = layout; tr.stride = stride; tr.entry = e;
                g_trackers.push_back(tr);
            }
        }
    }
}

// Hunter local frame: x right, y up, z forward (right vector flipped by the projection like the camera code does).
void local_frame(const float* d, const GameView& v, float* out) {
    Vec3 f = g_run.fwd, r = {f.z, 0, -f.x};
    if (v.proj[11] < 0) r = {-r.x, 0, -r.z};
    out[0] = d[0] * r.x + d[2] * r.z;
    out[1] = d[1];
    out[2] = d[0] * f.x + d[2] * f.z;
}

void report_top() {
    g_rank.clear();
    for (size_t i = 0; i < g_trackers.size(); i++)
        if (g_trackers[i].score > 0) g_rank.push_back((int)i);
    size_t n = std::min<size_t>(10, g_rank.size());
    std::partial_sort(g_rank.begin(), g_rank.begin() + n, g_rank.end(), [](int a, int b) {
        const Tracker &x = g_trackers[a], &y = g_trackers[b];
        return x.score != y.score ? x.score > y.score : a < b;
    });
    jout("joint report at %.0f s: %d trackers, %d scored", g_run.tick * kJointStep, (int)g_trackers.size(),
         (int)g_rank.size());
    for (size_t k = 0; k < n; k++) {
        const Tracker& t = g_trackers[g_rank[k]];
        double c = t.score;
        float mx = (float)(t.sum[0] / c), my = (float)(t.sum[1] / c), mz = (float)(t.sum[2] / c);
        double var = t.sum2 / c - (double)mx * mx - (double)my * my - (double)mz * mz;
        const Block& bk = g_blocks[t.blk];
        bool hand = my >= 60 && my <= 110 && fabsf(mx) >= 20 && fabsf(mx) <= 40;
        jout("joint candidate: base %p +0x%X layout %s stride 0x%X entry %d, score %d, local offset (%.0f,%.0f,%.0f) cm, "
             "spread %.0f cm, chain player+0x%X -> +0x%X%s",
             (void*)bk.p, (unsigned)(t.addr - bk.p), kLayoutName[t.layout], t.stride, t.entry, t.score, mx, my, mz,
             sqrt(var > 0 ? var : 0), bk.l1 < 0 ? 0 : bk.l1, bk.l2 < 0 ? 0 : bk.l2,
             hand ? " [hand-like]" : "");
    }
}

// Inputs copied from the game thread each research tick; the worker reads the copy.
struct JointIn {
    bool inGame = false;
    uint8_t* pl = nullptr;
    Vec3 pos{};
    GameView view{};
    int zone = -1;
};
SRWLOCK g_inLock = SRWLOCK_INIT;
JointIn g_in;
HANDLE g_jthread = nullptr, g_jevent = nullptr;
volatile LONG g_jstop = 0;
bool g_jstarted = false;

double wall_secs() { return (double)GetTickCount64() / 1000.0; }
long long qpc_us() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000000LL / f.QuadPart;
}

// Worker thread, one call per 0.25 s sample.
void joint_step(uint8_t* pl, const JointIn& g, double now) {
    if (pl && !mem_readable(pl, 0x2000)) return;  // the player object went away between samples: skip
    bool hunting = g.inGame && pl && g.view.valid && g.zone >= 0;  // any zone: the arms swing in town too, and the hub is where the search can run unattended
    if (!hunting) { g_run.zone = -2; return; }
    JointRun& r = g_run;
    if (r.zone != g.zone) {  // a new zone: fresh run
        r = JointRun();
        r.zone = g.zone;
        r.start = now;
        r.nextStep = now;
        r.nextReport = now + kReportEvery;
        r.nextRescan = now + kRescanEvery;
        g_blocks.clear();
        g_trackers.clear();
        g_seenKey.clear();
        jout("joint search: zone %d, root = GameSnapshot pos = player+0x%X (world cm), watching %.0f s", g.zone,
             (unsigned)HOOKV_PLAYER_POS, kJointSecs);
    }
    if (now - r.start > kJointSecs || now < r.nextStep) return;
    r.nextStep = now + kJointStep;
    r.tick++;

    // Facing: the direction the root moves in (the snapshot has no yaw); the camera heading until the hunter moves.
    float dr[3] = {0, 0, 0};
    if (r.haveRoot) { dr[0] = g.pos.x - r.lastRoot.x; dr[1] = g.pos.y - r.lastRoot.y; dr[2] = g.pos.z - r.lastRoot.z; }
    float hm = sqrtf(dr[0] * dr[0] + dr[2] * dr[2]);
    if (hm > 8) { r.fwd = {dr[0] / hm, 0, dr[2] / hm}; r.motionYaw = true; }
    else if (!r.motionYaw) {
        float cx = g.view.camTarget.x - g.view.camPos.x, cz = g.view.camTarget.z - g.view.camPos.z;
        float cn = sqrtf(cx * cx + cz * cz);
        if (cn > 1) r.fwd = {cx / cn, 0, cz / cn};
    }
    if (!r.framed && (r.motionYaw || r.tick > 8)) {
        r.framed = true;
        jout("joint search: facing yaw %.0f deg (atan2(x,z)) from %s; local frame x right, y up, z forward",
             atan2f(r.fwd.x, r.fwd.z) * 57.29578f, r.motionYaw ? "root motion" : "the camera heading");
    }
    memcpy(r.rootDelta, dr, sizeof(dr));
    bool havePrev = r.haveRoot;
    r.lastRoot = g.pos;
    r.haveRoot = true;

    if (!r.built || (r.cursor >= (int)g_blocks.size() && now >= r.nextRescan)) {
        if (r.built) r.nextRescan = now + kRescanEvery;
        build_blocks(pl);
    }
    long long t0 = qpc_us();
    while (r.cursor < (int)g_blocks.size() && qpc_us() - t0 < kScanBudgetUs) scan_block(r.cursor++, g.pos);

    // one readability check per block that has trackers, per sample (blocks can be freed between rebuilds)
    static std::vector<char> blockOk;
    blockOk.assign(g_blocks.size(), 0);
    for (Tracker& t : g_trackers) {
        char& ok = blockOk[t.blk];
        if (!ok) ok = mem_readable((void*)g_blocks[t.blk].p, g_blocks[t.blk].readable) ? 1 : 2;
        if (ok != 1) { t.hasLast = false; continue; }
        float tr[3];
        if (!read_matrix(t.addr, t.layout, tr)) { t.hasLast = false; continue; }
        float d[3] = {tr[0] - g.pos.x, tr[1] - g.pos.y, tr[2] - g.pos.z};
        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        bool consecutive = t.hasLast && t.lastTick == r.tick - 1;
        float dj[3] = {tr[0] - t.last[0], tr[1] - t.last[1], tr[2] - t.last[2]};
        memcpy(t.last, tr, sizeof(tr));
        t.hasLast = true;
        t.lastTick = r.tick;
        if (!consecutive || !havePrev || dist < 50 || dist > 250) continue;
        // its change must differ from the root's change (rejects stale copies of the root and the root itself)
        float ex = dj[0] - dr[0], ey = dj[1] - dr[1], ez = dj[2] - dr[2];
        if (sqrtf(ex * ex + ey * ey + ez * ez) < 1.0f) continue;
        float loc[3];
        local_frame(d, g.view, loc);
        t.score++;
        for (int i = 0; i < 3; i++) t.sum[i] += loc[i];
        t.sum2 += (double)loc[0] * loc[0] + (double)loc[1] * loc[1] + (double)loc[2] * loc[2];
    }
    if (now >= r.nextReport) {
        r.nextReport = now + kReportEvery;
        report_top();
    }
    flush();
}

// ---------------------------------------------------------------- MHW monster flinch counters

constexpr int kEmBytes = 0x18000;
struct Region { std::string name; uintptr_t base = 0; int bytes = 0; std::vector<float> prev, peak; std::vector<uint16_t> dec, jump; };
uint8_t* g_em = nullptr;
std::vector<Region> g_regions;
float g_emHp = -1;
double g_watchTime = 0, g_lastSample = 0;
int g_flinchReports = 0;

void flinch_begin(uint8_t* em) {
    g_em = em;
    g_regions.clear();
    g_regions.push_back(Region{"em", (uintptr_t)em, kEmBytes});
    // objects the monster points to whose class name mentions parts or damage
    for (int off = 0; off < kEmBytes && g_regions.size() < 6; off += 8) {
        if (!mem_readable(em + off, 8)) continue;
        uintptr_t p = *(uintptr_t*)(em + off);
        if (p < 0x10000 || (p & 7) || !mem_readable((void*)p, 16)) continue;
        const char* n = dti_name_of((const void*)p);
        if (n && (strstr(n, "Part") || strstr(n, "Damage") || strstr(n, "Parts"))) {
            char nm[96];
            snprintf(nm, sizeof(nm), "[em+0x%X] %s", off, n);
            g_regions.push_back(Region{nm, p, 0x4000});
        }
    }
    for (Region& r : g_regions) {
        size_t n = r.bytes / 4;
        r.prev.assign(n, NAN);
        r.peak.assign(n, 0);
        r.dec.assign(n, 0);
        r.jump.assign(n, 0);
    }
    g_emHp = -1;
    g_watchTime = 0;
}

void flinch_report(const char* why) {
    if (!g_em || g_watchTime < 20) { g_em = nullptr; return; }
    outf("flinch watch (%s, %.0f s of fighting): counters that drop with MHW hits and refill", why, g_watchTime);
    struct C { const Region* r; int i; float score; };
    std::vector<C> cs;
    for (const Region& r : g_regions)
        for (size_t i = 0; i < r.dec.size(); i++)
            if (r.dec[i] >= 3 && r.jump[i] >= 1) cs.push_back({&r, (int)i, (float)r.dec[i] * std::min(5, (int)r.jump[i])});
    std::sort(cs.begin(), cs.end(), [](const C& a, const C& b) { return a.score > b.score; });
    for (size_t k = 0; k < cs.size() && k < 30; k++)
        outf("  %s +0x%X: peak %.0f, drops %d, refills %d", cs[k].r->name.c_str(), cs[k].i * 4, cs[k].r->peak[cs[k].i],
             cs[k].r->dec[cs[k].i], cs[k].r->jump[cs[k].i]);
    if (cs.empty()) outf("  none");
    flush();
    g_em = nullptr;
    g_flinchReports++;
}

void flinch_sample(const Monster& m, double dt) {
    bool hit = g_emHp >= 0 && m.hp < g_emHp - 0.5f;
    g_emHp = m.hp;
    if (hit) g_watchTime += dt;
    std::vector<float> cur;
    for (Region& r : g_regions) {
        size_t n = r.bytes / 4;
        cur.assign(n, NAN);
        for (int pg = 0; pg < r.bytes; pg += 0x1000)
            if (mem_readable((void*)(r.base + pg), 0x1000)) memcpy(&cur[pg / 4], (void*)(r.base + pg), 0x1000);
        for (size_t i = 0; i < n; i++) {
            float a = r.prev[i], b = cur[i];
            if (std::isfinite(b) && b > 1 && b < 100000) {
                r.peak[i] = std::max(r.peak[i], b);
                if (std::isfinite(a)) {
                    if (hit && b < a - 0.01f && r.dec[i] < 60000) r.dec[i]++;
                    if (b > a * 1.3f + 1 && a < 0.6f * r.peak[i] && r.jump[i] < 60000) r.jump[i]++;
                }
            }
            r.prev[i] = b;
        }
    }
}

DWORD WINAPI joint_worker(LPVOID) {
    while (!g_jstop) {
        JointIn in;
        AcquireSRWLockShared(&g_inLock);
        in = g_in;
        ReleaseSRWLockShared(&g_inLock);
        joint_step(in.pl, in, wall_secs());
        WaitForSingleObject(g_jevent, 250);  // Sleep(250) that stop wakes at once
    }
    return 0;
}

}  // namespace

// Called from main.cpp shutdown, before the DLL unloads.
void research_stop() {
    if (!g_jthread) return;
    InterlockedExchange(&g_jstop, 1);
    SetEvent(g_jevent);
    WaitForSingleObject(g_jthread, 5000);
    CloseHandle(g_jthread);
    CloseHandle(g_jevent);
    g_jthread = g_jevent = nullptr;
}

// Game thread, after the snapshot.
void research_tick(const Live& l, double now) {
    if (!g_checked) {
        g_checked = true;
        // On by default (read-only, a few minutes per session), so playing is enough; [research] enabled=0 turns it off.
        char ini[MAX_PATH];
        snprintf(ini, sizeof(ini), "%s\\remnant_hunt.ini", mod_dir());
        g_on = GetPrivateProfileIntA("research", "enabled", 1, ini) != 0;
        if (g_on) { outf("== Remnant Hunt research (read-only) =="); flush(); }
    }
    if (!g_on) return;
    const GameSnapshot& g = l.snap;
    static bool ranks = false;
    if (!ranks && g.inGame) {
        if (uint8_t* u = user_data()) {
            ranks = true;
            std::string line = "save slot ints +0x90..+0xF0:";
            for (int o = 0x90; o < 0xF0; o += 4) {
                char b[24];
                snprintf(b, sizeof(b), " %X:%d", o, mem_readable(u + o, 4) ? *(int*)(u + o) : -1);
                line += b;
            }
            outf("%s", line.c_str());
            outf("(tell Claude your HR and MR so the right fields can be picked)");
            flush();
        }
    }
    uint8_t* pl = player_ptr();
    {
        AcquireSRWLockExclusive(&g_inLock);
        g_in.inGame = g.inGame; g_in.pl = pl; g_in.pos = g.pos; g_in.view = g.view; g_in.zone = g.zone;
        ReleaseSRWLockExclusive(&g_inLock);
        if (!g_jstarted) {
            g_jstarted = true;
            g_blocks.reserve(kMaxBlocks);
            g_trackers.reserve(kMaxTrackers);
            g_seenPtr.reserve(kMaxBlocks);
            g_seenKey.reserve(kMaxTrackers);
            g_rank.reserve(kMaxTrackers);
            g_jevent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            g_jthread = CreateThread(nullptr, 0, joint_worker, nullptr, 0, nullptr);
        }
    }
    // Flinch counters: the large MHW monster being fought, in hunting zones, up to three reports.
    if (g_flinchReports >= 3) return;
    const Monster* target = nullptr;
    for (const Monster& m : g.monsters)
        if (m.large && m.em == l.target && strcmp(m.kind, "r2boss") != 0) target = &m;
    if (g_em && (!g.inGame || g.inBase || !target || target->em != g_em)) {
        bool gone = true;
        for (const Monster& m : g.monsters) gone &= m.em != g_em;
        if (gone || !g.inGame || g.inBase) flinch_report(gone ? "monster gone" : "left the zone");
        else return;  // another monster hit for a moment: keep watching the first
    }
    if (!target || g.inBase) return;
    if (!g_em) { flinch_begin(target->em); outf("flinch watch: %s (%.0f hp)", target->kind, target->maxHp); flush(); }
    if (now - g_lastSample < 0.2) return;
    double dt = g_lastSample > 0 ? std::min(1.0, now - g_lastSample) : 0.2;
    g_lastSample = now;
    flinch_sample(*target, dt);
    if (g_watchTime > 300) flinch_report("5 minutes");
}

}  // namespace rh
