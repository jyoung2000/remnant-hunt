// Remnant x MHW mashup core: hidden host World monsters under the Remnant bosses (see host.h). GPL-3.0-or-later.
//
// Writes, all to the host monster's own data, approved by Jalon on 2026-10-14:
//   uEnemy +0x160  position (x, y, z), checked against the monster's hitbox centre before the first write
//   uEnemy +0xA50  collision position (SPL Teleport writes it with +0x160), only when it reads within 5 m of +0x160
//   uEnemy +0x170  rotation quaternion (turf.cpp's facing write)
//   uEnemy +0x314  transparency (UNVERIFIED, 15.11 DTI dump): 0 while bound when it reads as an alpha, restored on release
//   uEnemy +0x180  scale ([host] scale_offset; hide=scale, testing only: it shrinks the hit capsules the claw needs)
//   uEnemy +0x14   cUnit flags, bit 0 cleared (frozen) for 30 s after the boss dies, then set again
//   [[AI +0xAA8] +0x5D0]  the host's target, nulled every tick while bound (SPL SetTarget; AI = [uEnemy +0x12278])
//   cpHealthManager +0x64  current HP: held at max while bound and while sunk (it never dies, so it drops nothing)
// MHW's own ActionController::DoAction (reactions.cpp's mhw_do_action) puts a host that starts an attack near the hunter
// back into its idle action, and with [host] mode=spawn the game's SpawnMonsterInit at [host] spawn_fn is called (RVA
// inside .text, a function prologue, under SEH). Read only: the forced action +0x18938 and the parts table +0x1D058.
// remnant_hunt.ini [host]: enabled, mode, hide, disarm, target_base, scale_offset, spawn_em, spawn_fn, spawn_flag.
#include "host.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rh {

// ---------------------------------------------------------------- pure parts

HostQuat host_quat_from_yaw(float yaw, int fwd) {
    float a = fwd < 0 ? yaw + 3.14159265f : yaw;  // a rotation about the world's up axis by a takes +z to (sin a, 0, cos a)
    return {0.0f, sinf(a * 0.5f), 0.0f, cosf(a * 0.5f)};
}

float host_yaw_of_quat(const HostQuat& q, int fwd) {
    float s = fwd < 0 ? -1.0f : 1.0f;
    // q applied to (0, 0, s)
    float fx = s * 2.0f * (q.x * q.z + q.w * q.y);
    float fz = s * (1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    return atan2f(fx, fz);
}

bool host_claw_family(int clawSet, int clawId, int set, int id) {
    return clawSet >= 0 && set == clawSet && id >= clawId - 4 && id <= clawId + 4;
}

int HostLatch::step(double t, float gap, float above, int set, int id, bool press, bool hurt) {
    int ev = 0;
    if (press) pressAt = t;
    bool valid = set >= 0 && set <= 15 && id >= 0 && id < 4096;
    bool changed = valid && prevSet >= 0 && (set != prevSet || id != prevId);
    if (!latched) {
        if (changed && gap <= kHostLatchCm) {
            // learning: the first action the hunter enters at a host within the press window is the claw's
            if (clawSet < 0 && t - pressAt <= kHostPressWindow) {
                clawSet = set;
                clawId = id;
                ev |= HL_LEARNED;
            }
            if (host_claw_family(clawSet, clawId, set, id)) {
                latched = true;
                lastHitAt = t;
                ev |= HL_LATCH | HL_CLAW_HIT;
            }
        }
    } else if (!valid || !host_claw_family(clawSet, clawId, set, id) || gap > kHostLatchCm * 2.5f) {
        latched = false;
        ev |= HL_UNLATCH;
    } else if (hurt && t - lastHitAt >= 0.8) {  // a claw attack / wound on the host while hanging on
        lastHitAt = t;
        ev |= HL_CLAW_HIT;
    }
    // a mount: on top of the host (not hanging from the claw) for a second
    if (!latched && gap < 150.0f && above > 200.0f) {
        if (highSince < 0) highSince = t;
        if (t - highSince >= 1.0 && t - mountAt >= 20.0) {
            mountAt = t;
            ev |= HL_MOUNT;
        }
    } else {
        highSince = -1;
    }
    if (valid) {
        prevSet = set;
        prevId = id;
    }
    return ev;
}


int host_alpha_class(uint32_t bits) {
    float f;
    memcpy(&f, &bits, 4);
    if (bits == 0) return HOST_ALPHA_FLOAT;  // 0.0f (and a byte 0): already clear
    if (std::isfinite(f) && f >= 1e-4f && f <= 1.0f) return HOST_ALPHA_FLOAT;
    if (bits <= 255) return HOST_ALPHA_BYTE;  // a tiny denormal as a float: a byte alpha with zero bytes after it
    return HOST_ALPHA_NONE;
}

int host_parts_read(const uint8_t* t, size_t len, HostPart* out, int maxParts) {
    if (!t) return 0;
    int n = 0;
    for (; n < maxParts; n++) {
        size_t o = kHostPartsAt + (size_t)n * kHostPartStride;
        if (o + 0x1C > len) break;
        HostPart p;
        memcpy(&p.maxHp, t + o + 0x0C, 4);
        memcpy(&p.hp, t + o + 0x10, 4);
        memcpy(&p.breaks, t + o + 0x18, 4);
        bool ok = std::isfinite(p.maxHp) && p.maxHp > 0 && p.maxHp < 1e6f && std::isfinite(p.hp) && p.hp <= p.maxHp &&
                  p.hp >= -p.maxHp && p.breaks >= 0 && p.breaks <= 99;
        if (!ok) break;
        if (out) out[n] = p;
    }
    return n;
}

int host_part_cluster(int part, const float* radii, int n) {
    if (!radii || n <= 0 || part < 0) return -1;
    std::vector<int> idx(n);
    for (int i = 0; i < n; i++) idx[i] = i;
    std::stable_sort(idx.begin(), idx.end(), [radii](int a, int b) { return radii[a] > radii[b]; });
    return idx[part % n];
}

bool host_parse_em(const char* s, int* id, int* sub) {
    if (!s || (s[0] != 'e' && s[0] != 'E') || (s[1] != 'm' && s[1] != 'M') || s[2] < '0' || s[2] > '9') return false;
    int a = -1, b = -1;
    char tail = 0;
    if (sscanf(s + 2, "%d_%d%c", &a, &b, &tail) != 2 || a <= 0 || a >= 1000 || b < 0 || b >= 100) return false;
    *id = a;
    *sub = b;
    return true;
}

bool host_rva_in_text(const uint8_t* img, size_t len, uint32_t rva) {
    if (!img || len < 0x40 || img[0] != 'M' || img[1] != 'Z') return false;
    uint32_t lfa;
    memcpy(&lfa, img + 0x3C, 4);
    if ((size_t)lfa + 24 > len || memcmp(img + lfa, "PE\0\0", 4) != 0) return false;
    uint16_t ns, so;
    memcpy(&ns, img + lfa + 6, 2);
    memcpy(&so, img + lfa + 20, 2);
    size_t sec = (size_t)lfa + 24 + so;
    for (unsigned i = 0; i < ns; i++) {
        size_t e = sec + (size_t)i * 40;
        if (e + 40 > len) return false;
        char name[9] = {};
        memcpy(name, img + e, 8);
        uint32_t vs, va;
        memcpy(&vs, img + e + 8, 4);
        memcpy(&va, img + e + 12, 4);
        if (strcmp(name, ".text") == 0) return rva >= va && (uint64_t)rva < (uint64_t)va + vs;
    }
    return false;
}

bool host_looks_like_prologue(const uint8_t* b, size_t n) {
    if (!b || n < 5) return false;
    // mov [rsp+disp8], reg (48 89 5C 24 08, 4C 89 44 24 18 ...): the usual home-space spill
    if ((b[0] == 0x48 || b[0] == 0x4C) && b[1] == 0x89 && (b[2] & 0xC7) == 0x44 && b[3] == 0x24) return true;
    // REX.W (sub rsp / mov rax, rsp), REX (push r..), push rbp, push rbx
    return b[0] == 0x48 || b[0] == 0x40 || b[0] == 0x55 || b[0] == 0x53 || b[0] == 0x41;
}

}  // namespace rh

#ifndef RH_HOST_PURE
#include <climits>
#include <deque>
#include <map>
#include <string>

#include "bosses.h"

namespace rh {

namespace {

constexpr size_t kPos = 0x160, kRot = 0x170, kCol = 0xA50, kAlpha = 0x314, kFlags = 0x14;
constexpr size_t kAi = 0x12278, kTgt1 = 0xAA8, kTgt2 = 0x5D0, kForced = 0x18938, kParts = 0x1D058;
constexpr size_t kHpMax = 0x60, kHpCur = 0x64;
constexpr int kAction = 0x61C8 + 0xAC;  // ActionController + current {set, id} (hunter and monsters alike)
constexpr int kMaxPairs = 8;
constexpr float kHideScale = 0.02f, kUnderCm = 300.0f, kDisarmCm = 1500.0f, kColCm = 500.0f, kPartCm = 600.0f;
constexpr float kSpawnNearCm = 10000.0f;
constexpr double kRetireS = 30.0, kSpawnWaitS = 2.0;

enum Mode { MODE_BORROW, MODE_SPAWN };
enum Hide { HIDE_ALPHA, HIDE_SCALE, HIDE_UNDER, HIDE_NONE };
enum Disarm { DIS_ACTION = 1, DIS_TARGET = 2 };
const char* hide_name(int h) {
    return h == HIDE_ALPHA ? "alpha" : h == HIDE_SCALE ? "scale" : h == HIDE_UNDER ? "under" : "none";
}

struct Cfg {
    bool loaded = false;
    int enabled = 1, mode = MODE_BORROW, hide = HIDE_ALPHA, disarm = DIS_ACTION | DIS_TARGET, scaleOff = 0x180;
    int targetAi = 1, spawnFlag = 0, emId = 1, emSub = 0;
    unsigned long long spawnFn = 0;
    char spawnEm[16] = "em001_00";
} g_cfg;

const Cfg& cfg() {
    if (g_cfg.loaded) return g_cfg;
    g_cfg.loaded = true;
    char ini[MAX_PATH], v[32];
    snprintf(ini, sizeof(ini), "%s\\remnant_hunt.ini", mod_dir());
    g_cfg.enabled = GetPrivateProfileIntA("host", "enabled", 1, ini);
    GetPrivateProfileStringA("host", "mode", "borrow", v, sizeof(v), ini);
    g_cfg.mode = _stricmp(v, "spawn") == 0 ? MODE_SPAWN : MODE_BORROW;
    GetPrivateProfileStringA("host", "hide", "alpha", v, sizeof(v), ini);
    g_cfg.hide = _stricmp(v, "scale") == 0 ? HIDE_SCALE : _stricmp(v, "under") == 0 ? HIDE_UNDER
               : _stricmp(v, "none") == 0  ? HIDE_NONE : HIDE_ALPHA;
    GetPrivateProfileStringA("host", "disarm", "both", v, sizeof(v), ini);
    g_cfg.disarm = _stricmp(v, "none") == 0 ? 0 : _stricmp(v, "action") == 0 ? DIS_ACTION
                 : _stricmp(v, "target") == 0 ? DIS_TARGET : DIS_ACTION | DIS_TARGET;
    GetPrivateProfileStringA("host", "target_base", "ai", v, sizeof(v), ini);
    g_cfg.targetAi = _stricmp(v, "monster") != 0;
    GetPrivateProfileStringA("host", "scale_offset", "0x180", v, sizeof(v), ini);
    g_cfg.scaleOff = (int)strtol(v, nullptr, 0);
    GetPrivateProfileStringA("host", "spawn_em", "em001_00", g_cfg.spawnEm, sizeof(g_cfg.spawnEm), ini);
    if (!host_parse_em(g_cfg.spawnEm, &g_cfg.emId, &g_cfg.emSub)) {
        log("host: spawn_em=%s is not an emNNN_SS id: em001_00 used", g_cfg.spawnEm);
        g_cfg.emId = 1;
        g_cfg.emSub = 0;
    }
    GetPrivateProfileStringA("host", "spawn_fn", "0", v, sizeof(v), ini);
    g_cfg.spawnFn = strtoull(v, nullptr, 0);
    if (g_cfg.spawnFn >= 0x140000000ull) g_cfg.spawnFn -= 0x140000000ull;  // an absolute address given: its RVA
    g_cfg.spawnFlag = GetPrivateProfileIntA("host", "spawn_flag", 0, ini);
    log("host: enabled=%d mode=%s hide=%s disarm=%s target_base=%s scale_offset=0x%X spawn_em=%s (%d, %d) spawn_fn=0x%llX",
        g_cfg.enabled, g_cfg.mode == MODE_SPAWN ? "spawn" : "borrow", hide_name(g_cfg.hide),
        g_cfg.disarm == 0 ? "none" : g_cfg.disarm == DIS_ACTION ? "action" : g_cfg.disarm == DIS_TARGET ? "target" : "both",
        g_cfg.targetAi ? "ai" : "monster", g_cfg.scaleOff, g_cfg.spawnEm, g_cfg.emId, g_cfg.emSub, g_cfg.spawnFn);
    if (g_cfg.hide == HIDE_SCALE)
        log("host: hide=scale (testing): scaling shrinks the host's hit capsules, so the clutch claw mostly cannot latch");
    if (g_cfg.hide == HIDE_UNDER)
        log("host: hide=under (testing): the host 3 m below the boss, so the claw latches below ground");
    return g_cfg;
}

struct Pair {
    bool used = false;
    std::string boss;
    Vec3 bpos{};
    float byaw = 0;
    uint8_t* em = nullptr;
    uint8_t* health = nullptr;
    char kind[16] = {};
    bool borrowed = true;
    Vec3 origPos{}, origCol{};
    float origQuat[4] = {0, 0, 0, 1}, origScale[3] = {1, 1, 1};
    bool posOk = false, colOk = false, quatOk = false, scaleOk = false;
    int alpha = HOST_ALPHA_NONE;
    uint32_t origAlpha = 0;
    float origHp = 0, wroteHp = -1;
    int hide = HIDE_ALPHA;
    double nextTry = 0;
    bool hurt = false, spawnWait = false;
    float gap = 1e30f;
    // disarm: time spent in each non-attack action while the hunter is away
    std::map<std::pair<int, int>, float> idleTime;
    int idleSet = -1, idleId = -1;
    double lastDo = 0;
    bool disarmLogged = false, waitLogged = false, tgtOk = true, tgtLogged = false;
    int forced = INT_MIN, forcedLogs = 0;
    // part breaks
    int nParts = 0;
    int breaks[kHostMaxParts] = {};
    // after the boss: sunk and frozen until retireUntil, then let go (borrowed) or parked sunk (spawned)
    double retireUntil = 0;
    bool parked = false, froze = false;
    std::deque<HostEvent> ev;
};
Pair g_pair[kMaxPairs];
std::deque<Pair> g_retired;
HostLatch g_latch;
int g_latchKey = -1;
int g_zone = -1;
bool g_loaded = false, g_vWas = false, g_cWas = false, g_fwdLogged = false, g_colLogged = false, g_spawnOff = false;
double g_vAt = -100, g_now = 0;
std::map<std::string, std::pair<int, int>> g_idle;  // learned idle action per monster kind
const GameSnapshot* g_last = nullptr;  // this tick's snapshot (host_release runs inside bosses_tick)

struct SpawnWait {
    bool pending = false;
    double at = 0;
    Vec3 pos{};
    int key = -1;
    std::vector<const uint8_t*> before;
} g_spawn;

std::string path() {
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s\\learned_host.txt", mod_dir());
    return p;
}

void load_learned() {
    g_loaded = true;
    FILE* f = fopen(path().c_str(), "rb");
    if (!f) return;
    char line[128], k[16];
    int s, i;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "claw %d %d", &s, &i) == 2) { g_latch.clawSet = s; g_latch.clawId = i; }
        else if (sscanf(line, "idle %15s %d %d", k, &s, &i) == 3) g_idle[k] = {s, i};
    }
    fclose(f);
    log("host: learned claw action %d %d, idle actions for %zu monsters", g_latch.clawSet, g_latch.clawId, g_idle.size());
}

void save_learned() {
    FILE* f = fopen(path().c_str(), "wb");
    if (!f) return;
    if (g_latch.clawSet >= 0) fprintf(f, "claw %d %d\n", g_latch.clawSet, g_latch.clawId);
    for (auto& kv : g_idle) fprintf(f, "idle %s %d %d\n", kv.first.c_str(), kv.second.first, kv.second.second);
    fclose(f);
}

const Monster* find(const GameSnapshot& g, const uint8_t* em) {
    for (const Monster& m : g.monsters)
        if (m.em == em) return &m;
    return nullptr;
}

float hdist(Vec3 a, Vec3 b) { return sqrtf((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); }
float dist3(Vec3 a, Vec3 b) { return sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
bool finite3(const float* f) { return std::isfinite(f[0]) && std::isfinite(f[1]) && std::isfinite(f[2]); }

bool read_action(const uint8_t* unit, int* set, int* id) {
    if (!unit || !mem_readable(unit + kAction, 8)) return false;
    *set = *(const int*)(unit + kAction);
    *id = *(const int*)(unit + kAction + 4);
    return true;
}

// The host at a place: +0x160, and +0xA50 with it when that held up.
void put(Pair& p, Vec3 at, Vec3 col) {
    if (!p.posOk) return;
    memcpy(p.em + kPos, &at, 12);
    if (p.colOk) memcpy(p.em + kCol, &col, 12);
}

void refill(Pair& p) {
    if (!p.health || !mem_readable(p.health + kHpMax, 8)) return;
    float mx = *(float*)(p.health + kHpMax);
    if (std::isfinite(mx) && mx > 0) {
        *(float*)(p.health + kHpCur) = mx;
        p.wroteHp = mx;
    }
}

// The transparency: 0 while bound, back to what it read on release. Re-checked before every write.
void write_alpha(Pair& p, bool restore) {
    if (p.alpha == HOST_ALPHA_NONE || !mem_readable(p.em + kAlpha, 4)) return;
    uint32_t now;
    memcpy(&now, p.em + kAlpha, 4);
    if (host_alpha_class(now) == HOST_ALPHA_NONE) {
        log("host: %s transparency +0x314 now reads 0x%08X: no longer written, host shown", p.kind, now);
        p.alpha = HOST_ALPHA_NONE;
        p.hide = HIDE_NONE;
        return;
    }
    if (p.alpha == HOST_ALPHA_FLOAT) {
        uint32_t v = restore ? p.origAlpha : 0u;
        memcpy(p.em + kAlpha, &v, 4);
    } else {
        p.em[kAlpha] = restore ? (uint8_t)p.origAlpha : 0;
    }
}

int read_parts(const uint8_t* em, HostPart* out) {
    if (!mem_readable(em + kParts, 8)) return 0;
    const uint8_t* tb = *(const uint8_t* const*)(em + kParts);
    size_t len = kHostPartsAt + kHostMaxParts * kHostPartStride;
    if (!tb || !mem_readable(tb, len)) return 0;
    return host_parts_read(tb, len, out, kHostMaxParts);
}

// Takes a monster as the pair's host: what it was (to give it back), and which fields hold up.
void take(Pair& p, const Monster& m, bool borrowed) {
    p.em = m.em;
    p.health = m.health;
    memcpy(p.kind, m.kind, sizeof(p.kind));
    p.borrowed = borrowed;
    p.wroteHp = -1;
    p.idleTime.clear();
    p.idleSet = p.idleId = -1;
    p.disarmLogged = p.waitLogged = p.tgtLogged = false;
    p.tgtOk = true;
    p.spawnWait = false;
    auto it = g_idle.find(p.kind);
    if (it != g_idle.end()) { p.idleSet = it->second.first; p.idleId = it->second.second; }
    p.posOk = false;
    if (mem_readable(m.em + kPos, 12)) {
        memcpy(&p.origPos, m.em + kPos, 12);
        // +0x160 must be where the monster is: its feet under its hitbox centre
        float reach = std::max(m.half.x, m.half.z) + 800.0f;
        p.posOk = finite3(&p.origPos.x) && hdist(p.origPos, m.center) < reach && fabsf(p.origPos.y - m.center.y) < m.half.y * 2 + 800.0f;
        if (!p.posOk)
            log("host: %s +0x160 (%.0f, %.0f, %.0f) is not near its hitbox (%.0f, %.0f, %.0f): position not written", m.kind,
                p.origPos.x, p.origPos.y, p.origPos.z, m.center.x, m.center.y, m.center.z);
    }
    // +0xA50, the collision position: written with +0x160 only when it reads close to it
    p.colOk = false;
    if (p.posOk && mem_readable(m.em + kCol, 12)) {
        memcpy(&p.origCol, m.em + kCol, 12);
        p.colOk = finite3(&p.origCol.x) && dist3(p.origCol, p.origPos) < kColCm;
    }
    if (p.posOk && !p.colOk && !g_colLogged) {
        g_colLogged = true;
        log("host: %s collision position +0xA50 (%.0f, %.0f, %.0f) is not within 5 m of +0x160: only +0x160 written",
            m.kind, p.origCol.x, p.origCol.y, p.origCol.z);
    }
    p.quatOk = false;
    if (mem_readable(m.em + kRot, 16)) {
        memcpy(p.origQuat, m.em + kRot, 16);
        float n = p.origQuat[0] * p.origQuat[0] + p.origQuat[1] * p.origQuat[1] + p.origQuat[2] * p.origQuat[2] + p.origQuat[3] * p.origQuat[3];
        p.quatOk = std::isfinite(n) && fabsf(n - 1.0f) < 0.02f;
    }
    p.scaleOk = false;
    int so = cfg().scaleOff;
    if (so > 0 && so < 0x10000 && mem_readable(m.em + so, 12)) {
        memcpy(p.origScale, m.em + so, 12);
        float s = p.origScale[0];
        p.scaleOk = finite3(p.origScale) && s > 0.2f && s < 5.0f && fabsf(p.origScale[1] - s) < 0.01f * s &&
                    fabsf(p.origScale[2] - s) < 0.01f * s;
    }
    p.hide = cfg().hide;
    p.alpha = HOST_ALPHA_NONE;
    if (p.hide == HIDE_ALPHA) {
        uint32_t bits = 0xFFFFFFFFu;
        if (mem_readable(m.em + kAlpha, 4)) memcpy(&bits, m.em + kAlpha, 4);
        p.alpha = host_alpha_class(bits);
        p.origAlpha = p.alpha == HOST_ALPHA_BYTE ? (bits & 0xFF) : bits;
        float f;
        memcpy(&f, &bits, 4);
        if (p.alpha == HOST_ALPHA_FLOAT) log("host: %s transparency +0x314 reads as a float (%.3f): hidden by alpha", m.kind, f);
        else if (p.alpha == HOST_ALPHA_BYTE) log("host: %s transparency +0x314 reads as a byte (%u): hidden by alpha", m.kind, bits);
        else {
            p.hide = HIDE_NONE;
            log("host: transparency field not recognised at +0x314 (value 0x%08X, %g as a float); host shown", bits, f);
        }
    }
    if (p.hide == HIDE_SCALE && !p.scaleOk) {
        p.hide = HIDE_UNDER;
        log("host: %s +0x%X (%.3f, %.3f, %.3f) does not read as a scale: hidden 3 m under the boss instead", m.kind, so,
            p.origScale[0], p.origScale[1], p.origScale[2]);
    }
    // read only: the forced action field (SPL ForceAction)
    p.forced = INT_MIN;
    p.forcedLogs = 0;
    if (mem_readable(m.em + kForced, 4)) {
        p.forced = *(const int*)(m.em + kForced);
        log("host: %s forced action +0x18938 reads %d (read only)", m.kind, p.forced);
    }
    // the parts table: the break counts now, so a rise later is a break
    HostPart parts[kHostMaxParts];
    p.nParts = read_parts(m.em, parts);
    for (int i = 0; i < p.nParts; i++) p.breaks[i] = parts[i].breaks;
    if (p.nParts > 0) {
        char buf[256];
        int o = 0;
        for (int i = 0; i < p.nParts && o < (int)sizeof(buf) - 24; i++)
            o += snprintf(buf + o, sizeof(buf) - o, " %d:%.0f/%.0f x%d", i, parts[i].hp, parts[i].maxHp, parts[i].breaks);
        log("host: %s parts table: %d parts%s (severable parts at +0x1FC8 not read)", m.kind, p.nParts, buf);
    } else {
        log("host: %s parts table [+0x1D058] not recognised: part breaks are skipped", m.kind);
    }
    p.origHp = m.hp;
    turf_claim(m.em, true);  // no turf wars for a host
    log("host: bound %s @%p to %s (%s, pos %s, collision %s, rotation %s, hidden by %s, %.0f/%.0f hp)", m.kind, (void*)m.em,
        p.boss.c_str(), borrowed ? "borrowed" : "spawned", p.posOk ? "ok" : "unchecked", p.colOk ? "ok" : "not written",
        p.quatOk ? "ok" : "unchecked", p.hide == HIDE_ALPHA ? "alpha" : p.hide == HIDE_SCALE ? "scale" : p.hide == HIDE_UNDER ? "depth" : "nothing",
        m.hp, m.maxHp);
}

// SpawnMonsterInit under SEH: a wrong address faults here, not in the game.
typedef void* (*SpawnFn)(void* sEnemy, unsigned long long emId, unsigned long long subId, char flag);
DWORD g_sehCode = 0;
int seh_filter(DWORD code) {
    g_sehCode = code;
    return EXCEPTION_EXECUTE_HANDLER;
}
bool seh_spawn(SpawnFn fn, void* se, int id, int sub, char flag, void** ret) {
    __try {
        *ret = fn(se, (unsigned long long)id, (unsigned long long)sub, flag);
    } __except (seh_filter(GetExceptionCode())) {
        return false;
    }
    return true;
}

void spawn_off(const char* why) {
    g_spawnOff = true;
    log("host: spawn disabled for this session (%s): hosts are borrowed from the zone instead", why);
}

void acquire(Pair& p, const GameSnapshot& g, double t) {
    p.nextTry = t + 2.0;
    if (cfg().mode == MODE_SPAWN && !g_spawnOff) {
        // a spawned host left sunk by an earlier boss first
        for (auto it = g_retired.begin(); it != g_retired.end(); ++it) {
            const Monster* m = it->parked ? find(g, it->em) : nullptr;
            if (!m) continue;
            Vec3 op = it->origPos, oc = it->origCol;
            g_retired.erase(it);
            take(p, *m, false);
            p.origPos = op;
            p.origCol = oc;
            return;
        }
        if (g_spawn.pending || p.spawnWait) return;
        if (host_spawn_request(cfg().emId, cfg().emSub, p.bpos)) {
            p.spawnWait = true;
            g_spawn.key = (int)(&p - g_pair);
            return;
        }
        if (!g_spawnOff) { p.nextTry = t + 30.0; return; }
    }
    const Monster* distant = nullptr, *any = nullptr;
    float farD = 1e30f, anyD = 1e30f;
    for (const Monster& m : g.monsters) {
        if (!m.large || !m.health || !m.obbOk || !m.kind[0] || is_boss(m.em) || host_is(m.em) || turf_busy(m.em)) continue;
        float d = hdist(m.center, p.bpos);
        if (d < anyD) { anyD = d; any = &m; }
        if (hdist(m.center, g.pos) > 6000.0f && d < farD) { farD = d; distant = &m; }
    }
    const Monster* m = distant ? distant : any;
    if (m) take(p, *m, true);
}

// The boss is gone: the host is never killed. Refilled, restored (scale, rotation, transparency), sunk 3 m under its
// own spot and frozen for 30 s (host_tick's retired list lets it go after that).
void retire(Pair& p) {
    int key = (int)(&p - g_pair);
    if (key == g_latchKey) { g_latch.latched = false; g_latchKey = -1; }
    if (g_spawn.pending && g_spawn.key == key) g_spawn.key = -1;
    const Monster* m = p.em && g_last ? find(*g_last, p.em) : nullptr;
    if (!m || !mem_readable(p.em + kRot, 16)) {
        if (p.em) {
            log("host: released %s from %s (already gone)", p.kind, p.boss.c_str());
            turf_claim(p.em, false);
        }
        p = Pair();
        return;
    }
    refill(p);
    if (p.hide == HIDE_SCALE && p.scaleOk) memcpy(p.em + cfg().scaleOff, p.origScale, 12);
    if (p.quatOk) memcpy(p.em + kRot, p.origQuat, 16);
    if (p.hide == HIDE_ALPHA) write_alpha(p, true);
    if (!p.posOk) {  // never moved: nothing to sink, it carries on where it is
        log("host: released %s @%p from %s (refilled, in place)", p.kind, (void*)p.em, p.boss.c_str());
        turf_claim(p.em, false);
        p = Pair();
        return;
    }
    Vec3 under = p.origPos, underCol = p.origCol;
    under.y -= kUnderCm;
    underCol.y -= kUnderCm;
    put(p, under, underCol);
    p.froze = false;
    if (mem_readable(p.em + kFlags, 1)) {
        uint8_t f = p.em[kFlags];
        log("host: %s cUnit +0x14 reads 0x%02X before the freeze%s", p.kind, f, f & 1 ? "" : " (bit 0 already clear: not touched)");
        if (f & 1) {
            p.em[kFlags] = (uint8_t)(f & ~1u);
            p.froze = true;
        }
    }
    p.retireUntil = g_now + kRetireS;
    p.parked = false;
    p.ev.clear();
    log("host: released %s @%p from %s: refilled, sunk 3 m under its own spot%s for %.0f s", p.kind, (void*)p.em,
        p.boss.c_str(), p.froze ? " and frozen" : "", kRetireS);
    g_retired.push_back(std::move(p));
    p = Pair();
}

void retired_tick(const GameSnapshot& g, double t) {
    for (auto it = g_retired.begin(); it != g_retired.end();) {
        Pair& r = *it;
        if (!find(g, r.em) || !mem_readable(r.em + kRot, 16)) {
            log("host: released %s is gone", r.kind);
            turf_claim(r.em, false);
            it = g_retired.erase(it);
            continue;
        }
        refill(r);
        Vec3 under = r.origPos, underCol = r.origCol;
        under.y -= kUnderCm;
        underCol.y -= kUnderCm;
        if (!r.parked && t >= r.retireUntil) {
            if (r.froze && mem_readable(r.em + kFlags, 1)) r.em[kFlags] |= 1;
            r.froze = false;
            if (r.borrowed) {
                put(r, r.origPos, r.origCol);
                log("host: %s unfrozen and back at its own spot", r.kind);
                turf_claim(r.em, false);
                it = g_retired.erase(it);
                continue;
            }
            r.parked = true;
            log("host: spawned %s unfrozen and left sunk under its spot", r.kind);
        }
        put(r, under, underCol);
        ++it;
    }
}

// The host's target slot, nulled every tick (SPL SetTarget): [[base+0xAA8]+0x5D0], base = the AI data (or the monster).
void null_target(Pair& p, const GameSnapshot& g) {
    if (!p.tgtOk) return;
    const char* why = nullptr;
    const uint8_t* base = p.em;
    uint8_t* a = nullptr;
    uint8_t* slot = nullptr;
    if (cfg().targetAi) {
        base = mem_readable(p.em + kAi, 8) ? *(uint8_t* const*)(p.em + kAi) : nullptr;
        if (!base) why = "AI pointer +0x12278 unreadable";
    }
    if (!why) {
        if (!mem_readable(base + kTgt1, 8)) why = "+0xAA8 unreadable";
        else if (!(a = *(uint8_t* const*)(base + kTgt1)) || ((uintptr_t)a & 7) || !mem_readable(a + kTgt2, 8)) why = "[+0xAA8] not a readable pointer";
        else {
            slot = *(uint8_t* const*)(a + kTgt2);
            if (slot && (((uintptr_t)slot & 7) || !mem_readable(slot, 0x100))) why = "the target slot holds neither 0 nor a readable pointer";
        }
    }
    if (why) {
        p.tgtOk = false;
        log("host: target chain not valid for %s (%s): target not nulled", p.kind, why);
        return;
    }
    if (!p.tgtLogged) {
        p.tgtLogged = true;
        log("host: disarm path target: %s's target [[%s+0xAA8]+0x5D0] reads %p%s: nulled every tick", p.kind,
            cfg().targetAi ? "AI" : "monster", (void*)slot, slot == g.player ? " (the hunter)" : find(g, slot) ? " (a monster)" : "");
    }
    if (slot) *(uint8_t**)(a + kTgt2) = nullptr;
}

void disarm(Pair& p, const Monster& m, const GameSnapshot& g, double t, float dt) {
    int mode = cfg().disarm;
    if (!mode) {
        if (!p.disarmLogged) { p.disarmLogged = true; log("host: disarm path none (testing): %s can hurt the hunter", p.kind); }
        return;
    }
    if (mode & DIS_TARGET) null_target(p, g);
    if (!(mode & DIS_ACTION)) return;
    int set, id;
    if (!read_action(m.em, &set, &id) || set < 0 || set > 15 || id < 0 || id >= 4096) return;
    bool attack = turf_is_attack(p.kind, set, id);
    float hunterGap = dist3(turf_closest(m, g.pos), g.pos);
    if (hunterGap > kDisarmCm) {  // the hunter is away: learn what it does when it is not fighting
        if (!attack) p.idleTime[{set, id}] += dt;
        if (p.idleSet < 0) {
            float total = 0, best = 0;
            std::pair<int, int> bk{-1, -1};
            for (auto& kv : p.idleTime) {
                total += kv.second;
                if (kv.second > best) { best = kv.second; bk = kv.first; }
            }
            if (total >= 4.0f && bk.first >= 0) {
                p.idleSet = bk.first;
                p.idleId = bk.second;
                g_idle[p.kind] = bk;
                save_learned();
                log("host: %s idles in action %d %d (%.0f%% of %.1f s)", p.kind, bk.first, bk.second, best * 100 / total, total);
            }
        }
        return;
    }
    if (p.idleSet < 0) {
        if (!p.waitLogged) { p.waitLogged = true; log("host: disarm waiting: no idle action known for %s yet", p.kind); }
        return;
    }
    if (!p.disarmLogged) {
        p.disarmLogged = true;
        log("host: disarm path action: %s back to idle %d %d when it leaves it near the hunter", p.kind, p.idleSet, p.idleId);
    }
    // the idle set: the idle action and anything it spent a quarter of its quiet time in, never a learned attack
    bool inIdle = set == p.idleSet && id == p.idleId;
    if (!inIdle && !attack) {
        float total = 0;
        for (auto& kv : p.idleTime) total += kv.second;
        auto it = p.idleTime.find({set, id});
        inIdle = total > 4.0f && it != p.idleTime.end() && it->second >= 0.25f * total;
    }
    if (inIdle || t - p.lastDo < 0.25) return;
    p.lastDo = t;
    mhw_do_action(m.em, p.idleSet, p.idleId);
}

// A rise in a part's break count while the hunter is on the host (latched) or within 6 m: the boss's part breaks.
void parts_tick(Pair& p, int key, const GameSnapshot& g) {
    if (p.nParts <= 0) return;
    HostPart cur[kHostMaxParts];
    int n = read_parts(p.em, cur);
    if (n != p.nParts) {
        log("host: %s parts table changed (%d -> %d parts): part breaks off", p.kind, p.nParts, n);
        p.nParts = 0;
        return;
    }
    bool close = (g_latch.latched && g_latchKey == key) || p.gap < kPartCm;
    for (int i = 0; i < n; i++) {
        if (cur[i].breaks > p.breaks[i]) {
            if (close) {
                log("host: %s part %d broke (%d breaks)", p.kind, i, cur[i].breaks);
                p.ev.push_back({HOST_EV_PART, i, g.pos});
            }
            p.breaks[i] = cur[i].breaks;
        } else if (cur[i].breaks < p.breaks[i]) {
            p.breaks[i] = cur[i].breaks;
        }
    }
}

// A pending spawn: the new monster near the spot within 2 s becomes the waiting pair's host.
void spawn_tick(const GameSnapshot& g, double t) {
    if (!g_spawn.pending) return;
    const Monster* best = nullptr;
    float bd = 1e30f;
    for (const Monster& m : g.monsters) {
        if (!m.health || !m.kind[0] || is_boss(m.em) || host_is(m.em)) continue;
        if (std::find(g_spawn.before.begin(), g_spawn.before.end(), m.em) != g_spawn.before.end()) continue;
        float d = hdist(m.center, g_spawn.pos);
        if (d < bd) { bd = d; best = &m; }
    }
    if (best && bd <= kSpawnNearCm) {
        g_spawn.pending = false;
        int k = g_spawn.key;
        if (k >= 0 && k < kMaxPairs && g_pair[k].used && !g_pair[k].em) {
            log("host: spawned %s @%p appeared %.0f m from the boss after %.1f s", best->kind, (void*)best->em, bd / 100, t - g_spawn.at);
            take(g_pair[k], *best, false);
        } else {  // its boss is gone already: parked sunk for the next one
            log("host: spawned %s @%p appeared after its boss left: parked", best->kind, (void*)best->em);
            Pair r;
            r.used = true;
            r.em = best->em;
            r.health = best->health;
            memcpy(r.kind, best->kind, sizeof(r.kind));
            r.borrowed = false;
            r.parked = true;
            r.posOk = mem_readable(best->em + kPos, 12);
            if (r.posOk) memcpy(&r.origPos, best->em + kPos, 12);
            turf_claim(best->em, true);
            g_retired.push_back(std::move(r));
        }
        return;
    }
    if (t - g_spawn.at < kSpawnWaitS) return;
    g_spawn.pending = false;
    if (best) log("host: spawn call returned and a new %s appeared %.0f m away: not taken", best->kind, bd / 100);
    else log("host: spawn call returned but no monster appeared");
    int k = g_spawn.key;
    if (k >= 0 && k < kMaxPairs) g_pair[k].spawnWait = false;
    spawn_off("the spawn call made no host");
}

}  // namespace

bool host_enabled() { return cfg().enabled != 0; }

bool host_spawn_request(int emId, int subId, Vec3 pos) {
    if (g_spawnOff || g_spawn.pending || !g_last) return false;
    unsigned long long rva = cfg().spawnFn;
    if (!rva) {
        spawn_off("[host] spawn_fn is 0: set it from the dev_diag 'host diag: call' listing");
        return false;
    }
    uint8_t* base = (uint8_t*)GetModuleHandleA(nullptr);
    if (!base || !mem_readable(base, 0x1000)) {
        spawn_off("the exe's headers are unreadable");
        return false;
    }
    if (rva > 0xFFFFFFFFull || !host_rva_in_text(base, 0x1000, (uint32_t)rva)) {
        log("host: spawn_fn RVA 0x%llX is not inside the exe's .text section", rva);
        spawn_off("spawn_fn outside .text");
        return false;
    }
    uint8_t* fn = base + rva;
    if (!mem_readable(fn, 16) || !host_looks_like_prologue(fn, 16)) {
        if (mem_readable(fn, 8))
            log("host: spawn_fn RVA 0x%llX does not start like a function (%02X %02X %02X %02X %02X %02X %02X %02X)", rva,
                fn[0], fn[1], fn[2], fn[3], fn[4], fn[5], fn[6], fn[7]);
        spawn_off("spawn_fn is not a function prologue");
        return false;
    }
    uint8_t* se = rd_ptr(HOOKV_S_ENEMY);
    if (!se) {
        log("host: spawn: sEnemy is not readable yet");
        return false;
    }
    g_spawn.before.clear();
    for (const Monster& m : g_last->monsters) g_spawn.before.push_back(m.em);
    void* ret = nullptr;
    if (!seh_spawn((SpawnFn)(void*)fn, se, emId, subId, (char)cfg().spawnFlag, &ret)) {
        log("host: spawn call faulted (0x%08lx) at RVA 0x%llX, disabled", (unsigned long)g_sehCode, rva);
        g_spawnOff = true;
        return false;
    }
    log("host: spawn call SpawnMonsterInit(sEnemy %p, %d, %d, %d) at RVA 0x%llX returned %p", (void*)se, emId, subId,
        cfg().spawnFlag, rva, ret);
    g_spawn.pending = true;
    g_spawn.at = g_now;
    g_spawn.pos = pos;
    g_spawn.key = -1;
    return true;
}

void host_bind(int key, const char* bossId, Vec3 pos, float yaw) {
    if (key < 0 || key >= kMaxPairs || !host_enabled()) return;
    Pair& p = g_pair[key];
    if (p.used) retire(p);
    p = Pair();
    p.used = true;
    p.boss = bossId ? bossId : "boss";
    p.bpos = pos;
    p.byaw = yaw;
}

void host_follow(int key, Vec3 pos, float yaw) {
    if (key < 0 || key >= kMaxPairs || !g_pair[key].used) return;
    g_pair[key].bpos = pos;
    g_pair[key].byaw = yaw;
}

void host_release(int key) {
    if (key < 0 || key >= kMaxPairs || !g_pair[key].used) return;
    retire(g_pair[key]);
}

bool host_take(int key, HostEvent* ev) {
    if (key < 0 || key >= kMaxPairs || g_pair[key].ev.empty()) return false;
    *ev = g_pair[key].ev.front();
    g_pair[key].ev.pop_front();
    return true;
}

bool host_is(const uint8_t* em) {
    if (!em) return false;
    for (const Pair& p : g_pair)
        if (p.used && p.em == em) return true;
    for (const Pair& p : g_retired)
        if (p.em == em) return true;
    return false;
}

void host_tick(GameSnapshot& g, double t, float dt) {
    if (!g_loaded) load_learned();
    g_last = &g;
    g_now = t;
    if (g.zone != g_zone || !g.inGame) {  // the zone's monsters are gone: nothing to give back
        for (Pair& p : g_pair)
            if (p.used) {
                if (p.em) log("host: released %s (zone change)", p.kind);
                p = Pair();
            }
        g_retired.clear();
        g_spawn = SpawnWait();
        g_latch.latched = false;
        g_latchKey = -1;
        g_zone = g.zone;
    }
    if (!host_enabled() || !g.inGame) return;
    int fwd = turf_forward();
    if (!fwd && !g_fwdLogged) {
        g_fwdLogged = true;
        log("host: the monsters' forward is not learned yet (turf.cpp): facing written as +z");
    }
    // an aim + claw press (the mod's own injected claw press reads the same)
    bool v = key_down(keys().aimSlinger), c = key_down(keys().clutchClaw);
    if (v && !g_vWas) g_vAt = t;
    bool press = c && !g_cWas && (v || t - g_vAt <= kHostPressWindow);
    g_vWas = v;
    g_cWas = c;

    spawn_tick(g, t);
    retired_tick(g, t);

    int nearest = -1;
    const Monster* nearestM = nullptr;
    float nearestGap = 1e30f;
    for (int k = 0; k < kMaxPairs; k++) {
        Pair& p = g_pair[k];
        if (!p.used) continue;
        if (!p.em) {
            if (t >= p.nextTry) acquire(p, g, t);
            if (!p.em) continue;
        }
        const Monster* m = find(g, p.em);
        if (!m || !p.health || !mem_readable(p.health + kHpMax, 8) || !mem_readable(p.em + kRot, 16)) {
            log("host: %s @%p for %s is gone: looking for another", p.kind, (void*)p.em, p.boss.c_str());
            turf_claim(p.em, false);
            if (k == g_latchKey) { g_latch.latched = false; g_latchKey = -1; }
            p.em = nullptr;
            p.nextTry = t + 5.0;
            continue;
        }
        // HP: any drop since the last write is the hunter's doing (it is held at max)
        float cur = *(float*)(p.health + kHpCur);
        p.hurt = p.wroteHp > 0 && cur < p.wroteHp - 1.0f;
        refill(p);
        // pinned to the boss, facing its way, hidden
        Vec3 at = p.bpos;
        if (p.hide == HIDE_UNDER) at.y -= kUnderCm;
        put(p, at, at);
        if (p.quatOk) {
            HostQuat q = host_quat_from_yaw(p.byaw, fwd);
            memcpy(p.em + kRot, &q, 16);
        }
        if (p.hide == HIDE_ALPHA) write_alpha(p, false);
        if (p.hide == HIDE_SCALE && p.scaleOk) {
            const float s[3] = {kHideScale, kHideScale, kHideScale};
            memcpy(p.em + cfg().scaleOff, s, 12);
        }
        disarm(p, *m, g, t, dt);
        if (p.forced != INT_MIN && p.forcedLogs < 20 && mem_readable(p.em + kForced, 4)) {
            int f = *(const int*)(p.em + kForced);
            if (f != p.forced) {
                if (++p.forcedLogs == 20) log("host: %s forced action: further changes not logged", p.kind);
                else log("host: %s forced action +0x18938 %d -> %d (read only)", p.kind, p.forced, f);
                p.forced = f;
            }
        }
        p.gap = dist3(turf_closest(*m, g.pos), g.pos);
        parts_tick(p, k, g);
        if (p.gap < nearestGap) { nearestGap = p.gap; nearest = k; nearestM = m; }
    }

    // The hunter against the nearest host: claw latch, claw hits, mount.
    int set = -1, id = -1;
    read_action(g.player, &set, &id);
    Pair* np = nearest >= 0 ? &g_pair[nearest] : nullptr;
    if (g_latch.latched && nearest != g_latchKey) g_latch.latched = false;  // another host is closer now
    float above = np ? g.pos.y - np->bpos.y : 0.0f;
    int ev = g_latch.step(t, nearestM ? nearestGap : 1e30f, above, set, id, press, np && np->hurt);
    g_latchKey = g_latch.latched ? nearest : -1;
    if (ev & HL_LEARNED) {
        log("host: learned the claw action %d %d", g_latch.clawSet, g_latch.clawId);
        save_learned();
    }
    if (np) {
        if (ev & HL_LATCH) log("host: claw latch (action %d %d)", set, id);
        if (ev & HL_UNLATCH) log("host: claw let go");
        if (ev & HL_CLAW_HIT) np->ev.push_back({HOST_EV_CLAW, -1, g.pos});
        if (ev & HL_MOUNT) {
            log("host: mount");
            np->ev.push_back({HOST_EV_MOUNT, -1, g.pos});
        }
        while (np->ev.size() > 8) np->ev.pop_front();
    }

    // Nothing else in the mod sees the hosts (no bars, no targets, no turf wars).
    g.monsters.erase(std::remove_if(g.monsters.begin(), g.monsters.end(), [](const Monster& m) { return host_is(m.em); }),
                     g.monsters.end());
}

}  // namespace rh
#endif  // RH_HOST_PURE
