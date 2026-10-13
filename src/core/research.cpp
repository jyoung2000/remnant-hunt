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

void outf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void outf(const char* fmt, ...) {
    char b[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof(b), fmt, a);
    va_end(a);
    g_text += b;
    g_text += "\n";
}
void flush() {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\dev_research.txt", mod_dir());
    if (FILE* f = fopen(path, "ab")) {
        fwrite(g_text.data(), 1, g_text.size(), f);
        fclose(f);
    }
    g_text.clear();
}

// A 4x4 float matrix that looks like a joint's world transform near p (row-vector, translation in the last row).
bool joint_like(const float* m, const Vec3& p, float radius) {
    for (int r = 0; r < 3; r++) {
        float n = m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2];
        if (!(n > 0.6f && n < 1.6f) || fabsf(m[r * 4 + 3]) > 1e-3f) return false;
    }
    if (fabsf(m[15] - 1.0f) > 1e-3f) return false;
    float dx = m[12] - p.x, dy = m[13] - p.y, dz = m[14] - p.z;
    return dx * dx + dy * dy + dz * dz < radius * radius;
}

// ---------------------------------------------------------------- the hunter's joints

struct Joints { uintptr_t base = 0; int playerOff = -1, stride = 0, count = 0, first = 0; };
Joints g_joints;
int g_jointSamples = 0;
double g_jointNext = 0;

void find_joints(uint8_t* pl, const Vec3& pos) {
    std::map<uintptr_t, bool> seen;
    Joints best;
    for (int off = 0; off < 0x20000; off += 8) {
        if (!mem_readable(pl + off, 8)) { off = (off | 0xFFF); continue; }
        uintptr_t p = *(uintptr_t*)(pl + off);
        if (p < 0x10000 || (p & 7) || seen.count(p)) continue;
        seen[p] = true;
        if (!mem_readable((void*)p, 0x200)) continue;
        bool any = false;
        for (int k = 0; k < 0x200 && !any; k += 16) any = joint_like((const float*)(p + k), pos, 400);
        if (!any) continue;
        // count the matrices in the block and the step between them
        std::vector<int> at;
        for (int k = 0; k < 0x10000; k += 16) {
            if ((k & 0xFFF) == 0 && !mem_readable((void*)(p + k), 0x1000)) break;
            if (joint_like((const float*)(p + k), pos, 400)) at.push_back(k);
        }
        if ((int)at.size() < 20) continue;
        std::map<int, int> steps;
        for (size_t i = 1; i < at.size(); i++) steps[at[i] - at[i - 1]]++;
        auto st = std::max_element(steps.begin(), steps.end(), [](auto& a, auto& b) { return a.second < b.second; });
        if ((int)at.size() > best.count) best = {p, off, st->first, (int)at.size(), at[0]};
    }
    g_joints = best;
    if (best.count) outf("joints: [player+0x%X] -> %d matrices from +0x%X, step 0x%X", best.playerOff, best.count,
                         best.first, best.stride);
    else outf("joints: no joint matrices found near the hunter");
}

void sample_joints(uint8_t* pl, const Vec3& pos, const GameView& v) {
    if (!g_joints.count) return;
    uintptr_t p = *(uintptr_t*)(pl + g_joints.playerOff);
    if (p != g_joints.base) { outf("joints: block moved"); g_joints.count = 0; return; }
    // positions relative to the hunter, in the camera's frame (x right, y up, z forward)
    Vec3 f = {v.camTarget.x - v.camPos.x, 0, v.camTarget.z - v.camPos.z};
    float n = sqrtf(f.x * f.x + f.z * f.z);
    if (n < 1) return;
    f.x /= n; f.z /= n;
    Vec3 r = {f.z, 0, -f.x};
    if (v.proj[11] < 0) r = {-r.x, 0, -r.z};
    std::string line = "joints sample:";
    int idx = 0;
    for (int k = g_joints.first; idx < 220; k += g_joints.stride, idx++) {
        if (!mem_readable((void*)(p + k), 64)) break;
        const float* m = (const float*)(p + k);
        if (!joint_like(m, pos, 600)) { line += " -"; continue; }
        float dx = m[12] - pos.x, dy = m[13] - pos.y, dz = m[14] - pos.z;
        char b[48];
        snprintf(b, sizeof(b), " %d:%.0f,%.0f,%.0f", idx, dx * r.x + dz * r.z, dy, dx * f.x + dz * f.z);
        line += b;
    }
    outf("%s", line.c_str());
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

}  // namespace

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
    if (g.inGame && pl && g.view.valid) {
        static bool searched = false;
        if (!searched) { searched = true; find_joints(pl, g.pos); flush(); }
        if (g_joints.count && g_jointSamples < 12 && now >= g_jointNext) {
            g_jointNext = now + 10;
            g_jointSamples++;
            sample_joints(pl, g.pos, g.view);
            flush();
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
