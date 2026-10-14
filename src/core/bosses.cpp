// Remnant x MHW mashup core: Remnant II bosses roaming MHW's field zones. GPL-3.0-or-later (see LICENSE).
//
// Each boss is a row of sheets/bosses.json with its attacks in sheets/boss_attacks.json, both written by
// tools/author_bosses.py from the player's own Remnant II install. The mod moves the boss on MHW's ground
// (world_ray), runs a Remnant-style AI (roam, roar, chase, melee / area / ranged attacks with hit windows measured
// from the animations, flinch, stagger, death), scales it to the zone's rank (low / high / master, read from the
// zone and its MHW monsters) and keeps the strongest bosses out of low-rank zones. It takes damage from the Remnant
// guns and skills (as a pseudo monster) and from MHW's weapons (melee: attack input + reach; bows and bowguns: the
// shot's line), and both build its stagger. Drawing is render3d.cpp with Remnant II's own model and textures.
#include "bosses.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "imgui.h"
#include "model.h"
#include "render3d.h"

namespace rh {

ImFont* ui_font(int which);  // ui.cpp: 0 body, 1 bold, 2 title

namespace {

constexpr float kPi = 3.14159265f;
constexpr int kMax = 4;
constexpr int kProj = 24;

enum St { ST_LOADING, ST_IDLE, ST_WALK, ST_ROAR, ST_CHASE, ST_TURN, ST_ATTACK, ST_FLINCH, ST_STUN, ST_DEAD };
enum Rank { RANK_LR, RANK_HR, RANK_MR };
const char* const kRankName[] = {"LOW RANK", "HIGH RANK", "MASTER RANK"};

// A boss's model in its own frame: forward / right (model xy) and its extent along, across and up (cm, scaled).
struct Frame {
    bool ok = false;
    float f[2] = {0, 1}, r[2] = {-1, 0};
    float aMin = -200, aMax = 200, cMin = -100, cMax = 100, zMin = 0, zMax = 300;
};

// One sphere of a boss's whole-model hitbox: world position and radius (cm), and whether it is on the head.
struct WSph { Vec3 c; float r; bool head; };

struct Boss {
    bool used = false;
    bool entrance = false;     // just spawned: it roars at the hunter before it engages
    std::vector<WSph> sph;     // the posed hitbox spheres, placed once per tick (empty until the model is posed)
    int row = -1;
    St st = ST_LOADING;
    Vec3 pos{}, home{};
    float yaw = 0;
    float hp = 0, maxHp = 0, dmgMult = 1, meleeHit = 100;
    int rank = RANK_LR;
    double stT0 = 0;
    std::string anim, prev;  // full asset paths
    float at = 0, pt = 0, fade = 0;
    bool loop = true, ploop = true;
    float root[3] = {};
    int attack = -1;     // boss_attacks row
    int windowsDone = 0; // bits
    bool fired = false;  // ranged attack released
    double nextAttack = 0;
    std::map<int, double> cd;  // boss_attacks row -> ready again
    Vec3 goal{};
    double goalUntil = 0;
    float flinchAcc = 0, staggerAcc = 0, staggerNeed = 0;
    int staggers = 0, phases = 0;
    bool aggro = false, roared = false, looted = false;
    double lastSeen = 0, lastStep = 0, lastWallCheck = 0;
    float turnFrom = 0, turnBy = 0;
    float sink = 0;
    Frame fr;
    // How it fights (bosses.json style) and the difficulty it was made at.
    int diff = 1;
    float tempo = 1;           // the difficulty's pace, faster again when enraged
    bool enraged = false;
    int orbitDir = 1;          // which way it circles
    float reachMax = 0;        // its longest attack, cm
    double nextFlip = 0, retreatUntil = 0;
    // A turf war with an MHW monster: it takes the hunter's place as the target.
    uint8_t* foe = nullptr;
    double foeUntil = 0, foeNext = 0, foeHitAt = 0, foeCheck = 0, lastHunterHit = -100;
    float foeReach = 0, foeLost = 0, bossLost = 0, hunterDmg = 0;  // shares of max health lost in it
};

struct Proj {
    bool live = false;
    Vec3 p{}, v{};
    float dmg = 0, radius = 0;
    double until = 0;
    uint32_t color = 0;
    int status = 0;
    float statusTotal = 0;
    uint8_t* foe = nullptr;  // aimed at an MHW monster in a turf war
    int boss = -1;
    float attackDmg = 0;
};

// Per bosses.json row, in the model's bind pose: the bounding sphere of the vertices skinned mostly to each bone.
struct LSph { float c[3]; float r; int bone; bool head; };
std::map<int, std::vector<LSph>> g_hitbox;  // game thread

Boss g_boss[kMax];
uint8_t g_token[kMax];  // the bosses' pseudo uEnemy pointers: never dereferenced, only compared
Proj g_proj[kProj];
bool g_rh = false;       // MHW's world is right-handed (from its projection)
int g_zone = -1, g_zoneRank = RANK_LR;
double g_rollAt = 0;
int g_rolls = 0;
int g_nextDebugRow = 0;
bool g_keyWas = false, g_lmbWas = false, g_rmbWas = false;
double g_swingAt = 0, g_dodgeAt = -10;
int g_swingWeapon = -1;
const GameSnapshot* g_snap = nullptr;
std::map<const uint8_t*, double> g_foeCd;  // MHW monster -> no new turf war with a boss before
int g_difficulty = 1;  // difficulties row, from the save (chosen at Ward 13)

// Shared with the render thread.
struct DrawBoss {
    const Model* m;
    int row;
    std::string a, b;
    float at, bt, blend;
    float world[16];
};
struct Number { Vec3 at; int amount; bool crit, weak; double t; };
struct HudBoss { int row; float hp, maxHp; bool show, dead; double deadAt; int rank; bool staggered; uint32_t status; };
// Remnant-style bars for everything else: MHW's monsters, and Remnant creatures below boss tier.
// tier 0 = small creature (thin, no name), 1 = large monster / elite (name over it).
struct Bar { const void* key; int tier; std::string name; float hp, maxHp; Vec3 anchor; uint32_t status; };
struct Docked { const void* key; std::string name; float hp, maxHp; uint32_t status; };
struct Aff { Vec3 c; float r; uint32_t bits; };  // afflicted monsters, for the status visuals
struct Mark { Vec3 pos; int row; bool aggro; };  // Remnant creatures for the tracker
struct Glow { Vec3 p; float radius; uint32_t color; };       // projectiles
struct Ring { Vec3 c; float radius, k; uint32_t color; };     // area attacks winding up
std::mutex g_lock;
std::vector<DrawBoss> g_draw;
std::vector<HudBoss> g_hud;
std::vector<Bar> g_bars, g_creatureBars;
std::vector<Mark> g_marks;
std::vector<Aff> g_affs;
uint32_t g_meBits = 0;
bool g_spaceWas = false;
std::vector<Glow> g_glows;
std::vector<Ring> g_rings;
Docked g_focus{};
bool g_hasFocus = false;
std::map<const void*, float> g_barTrail;  // render thread
struct Seen { bool vis; double at; };
std::map<const void*, Seen> g_los;  // game thread
std::set<std::string> g_kindsMet;   // game thread
Number g_numbers[32];
int g_nextNumber = 0;
double g_hurtAt = -10;
float g_trail[kMax] = {};

float frand() { return (float)rand() / (float)RAND_MAX; }
float wrap(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}
Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 mul(Vec3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float hlen(Vec3 a) { return sqrtf(a.x * a.x + a.z * a.z); }
float len3(Vec3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
Vec3 fwd_of(float yaw) { return {sinf(yaw), 0, cosf(yaw)}; }
Vec3 right_of(float yaw) { return g_rh ? Vec3{-cosf(yaw), 0, sinf(yaw)} : Vec3{cosf(yaw), 0, -sinf(yaw)}; }
float yaw_to(Vec3 from, Vec3 to) { return atan2f(to.x - from.x, to.z - from.z); }
bool has(const char* s) { return s && *s && strcmp(s, "none") != 0; }
uint32_t parse_color(const char* hex) {
    unsigned v = 0xFFD27A;
    if (hex && hex[0] == '#') sscanf(hex + 1, "%x", &v);
    return IM_COL32((v >> 16) & 255, (v >> 8) & 255, v & 255, 255);
}

const Bosses& B(const Boss& b) { return kBosses[b.row]; }
std::string display_name(int row) {
    const std::string& n = r2_text(kBosses[row].name);
    return n.empty() ? std::string(kBosses[row].title) : n;
}
std::string anim_path(int row, const char* name) {
    return has(name) ? std::string(kBosses[row].anim_dir) + name + ".uasset" : std::string();
}
const Model* model_of(int row, bool* failed = nullptr) { return model_get(kBosses[row].mesh, failed); }
const Anim* anim_of(int row, const std::string& path, bool* failed = nullptr) {
    if (path.empty()) return nullptr;
    const Model* m = model_of(row);
    return m ? anim_get(path.c_str(), kBosses[row].skeleton, m, failed) : nullptr;
}
float anim_len(const Boss& b, const std::string& path) {
    const Anim* a = anim_of(b.row, path);
    return a ? a->length() : 0.0f;
}

// The boss's frame from its model and the sheet's facing.
void make_frame(Boss& b) {
    const Model* m = model_of(b.row);
    if (!m) return;
    Frame& f = b.fr;
    switch (B(b).forward) {
        case BOSSES_FORWARD_NY: f.f[0] = 0; f.f[1] = -1; f.r[0] = 1; f.r[1] = 0; break;
        case BOSSES_FORWARD_PX: f.f[0] = 1; f.f[1] = 0; f.r[0] = 0; f.r[1] = 1; break;
        case BOSSES_FORWARD_NX: f.f[0] = -1; f.f[1] = 0; f.r[0] = 0; f.r[1] = -1; break;
        default: f.f[0] = 0; f.f[1] = 1; f.r[0] = -1; f.r[1] = 0; break;  // +y, like most Remnant creatures
    }
    float s = B(b).scale;
    f.aMin = f.cMin = 1e30f;
    f.aMax = f.cMax = -1e30f;
    for (float x : {m->boundsMin[0], m->boundsMax[0]})
        for (float y : {m->boundsMin[1], m->boundsMax[1]}) {
            float a = (x * f.f[0] + y * f.f[1]) * s, c = (x * f.r[0] + y * f.r[1]) * s;
            f.aMin = std::min(f.aMin, a); f.aMax = std::max(f.aMax, a);
            f.cMin = std::min(f.cMin, c); f.cMax = std::max(f.cMax, c);
        }
    f.zMin = m->boundsMin[2] * s;
    f.zMax = m->boundsMax[2] * s;
    f.ok = true;
}
Vec3 head_point(const Boss& b) { return add(b.pos, mul(fwd_of(b.yaw), std::max(100.0f, b.fr.aMax * 0.8f))); }
float height_of(const Boss& b) { return std::max(150.0f, b.fr.zMax - std::max(0.0f, b.fr.zMin)); }

// Weak spots are the head's bones (matched by name).
bool head_bone(const std::string& n) {
    std::string l = n;
    for (char& c : l) c = (char)tolower((unsigned char)c);
    return l.find("head") != std::string::npos || l.find("jaw") != std::string::npos || l.find("skull") != std::string::npos;
}

// The whole-model hitbox of a row: one bounding sphere per bone that has enough skinned vertices (each vertex goes to
// its strongest bone), in the bind pose. Radius capped at 150 cm, bones with under 20 vertices or under 8 cm left out.
const std::vector<LSph>& hitbox_of(int row, const Model& m) {
    auto it = g_hitbox.find(row);
    if (it != g_hitbox.end()) return it->second;
    size_t nb = m.bones.size();
    std::vector<float> lo(nb * 3, 1e30f), hi(nb * 3, -1e30f);
    std::vector<int> cnt(nb, 0);
    std::vector<int> owner(m.vertices.size(), -1);
    for (size_t i = 0; i < m.vertices.size(); i++) {
        const ModelVertex& v = m.vertices[i];
        int best = -1, bw = 0;
        for (int k = 0; k < 8; k++)
            if (v.weights[k] > bw && v.bones[k] < nb) { bw = v.weights[k]; best = v.bones[k]; }
        if (best < 0) continue;
        owner[i] = best;
        cnt[best]++;
        for (int c = 0; c < 3; c++) {
            lo[best * 3 + c] = std::min(lo[best * 3 + c], v.pos[c]);
            hi[best * 3 + c] = std::max(hi[best * 3 + c], v.pos[c]);
        }
    }
    std::vector<LSph> out;
    std::vector<float> rad2(nb, 0.0f);
    for (size_t i = 0; i < m.vertices.size(); i++) {
        int b = owner[i];
        if (b < 0 || cnt[b] < 20) continue;
        float d2 = 0;
        for (int c = 0; c < 3; c++) {
            float d = m.vertices[i].pos[c] - (lo[b * 3 + c] + hi[b * 3 + c]) * 0.5f;
            d2 += d * d;
        }
        rad2[b] = std::max(rad2[b], d2);
    }
    float s = kBosses[row].scale;
    for (size_t b = 0; b < nb; b++) {
        if (cnt[b] < 20) continue;
        float r = std::min(sqrtf(rad2[b]), 150.0f / std::max(0.01f, s));
        if (r * s < 8.0f) continue;
        LSph l;
        for (int c = 0; c < 3; c++) l.c[c] = (lo[b * 3 + c] + hi[b * 3 + c]) * 0.5f;
        l.r = r;
        l.bone = (int)b;
        l.head = head_bone(m.bones[b].name);
        out.push_back(l);
    }
    log("bosses: %s hitbox from %d bones", kBosses[row].id, (int)out.size());
    return g_hitbox[row] = std::move(out);
}

// Places a boss's spheres: the posed bone palette (as render3d draws it) times the boss's world matrix.
void place_hitbox(Boss& b, const std::vector<float>& pal, const float w[16]) {
    const Model* m = model_of(b.row);
    b.sph.clear();
    if (!m || pal.size() < m->bones.size() * 12) return;
    const std::vector<LSph>& hb = hitbox_of(b.row, *m);
    float s = B(b).scale;
    for (const LSph& l : hb) {
        const float* q = &pal[(size_t)l.bone * 12];
        float x = q[0] * l.c[0] + q[1] * l.c[1] + q[2] * l.c[2] + q[3];
        float y = q[4] * l.c[0] + q[5] * l.c[1] + q[6] * l.c[2] + q[7];
        float z = q[8] * l.c[0] + q[9] * l.c[1] + q[10] * l.c[2] + q[11];
        Vec3 c = {x * w[0] + y * w[4] + z * w[8] + w[12], x * w[1] + y * w[5] + z * w[9] + w[13],
                  x * w[2] + y * w[6] + z * w[10] + w[14]};
        b.sph.push_back({c, l.r * s, l.head});
    }
}

// The nearest sphere a ray passes within the radius of (dir is a unit vector); false when it misses them all.
bool ray_spheres(const Boss& b, Vec3 o, Vec3 dir, float maxT, float* tHit, bool* head) {
    bool any = false;
    float best = maxT;
    for (const WSph& s : b.sph) {
        Vec3 oc = sub(s.c, o);
        float tc = oc.x * dir.x + oc.y * dir.y + oc.z * dir.z;
        float d2 = oc.x * oc.x + oc.y * oc.y + oc.z * oc.z - tc * tc, r2 = s.r * s.r;
        if (d2 > r2) continue;
        float half = sqrtf(r2 - d2);
        if (tc + half < 0) continue;
        float t0 = std::max(0.0f, tc - half);
        if (t0 >= best) continue;
        best = t0;
        *head = s.head;
        any = true;
    }
    if (any) *tHit = best;
    return any;
}

void play(const char* event, Vec3 at, float loud) {
    if (!has(event) || !g_snap) return;
    float d = hlen(sub(at, g_snap->pos));
    float v = std::max(0.0f, std::min(1.0f, loud * (1.25f - d / 6000.0f)));
    if (v > 0.03f) audio_play(event, v);
}

void set_anim(Boss& b, const std::string& path, bool loop) {
    if (path.empty() || path == b.anim) { b.loop = loop; return; }
    b.prev = b.anim;
    b.pt = b.at;
    b.ploop = b.loop;
    b.fade = b.prev.empty() ? 0.0f : 1.0f;
    b.anim = path;
    b.at = 0;
    b.loop = loop;
    const Anim* a = anim_of(b.row, path);
    if (a) anim_root(*a, 0, false, b.root);
    else b.root[0] = b.root[1] = b.root[2] = 0;
}
void go(Boss& b, St st, double t) {
    b.st = st;
    b.stT0 = t;
    const Bosses& r = B(b);
    switch (st) {
        case ST_IDLE: set_anim(b, anim_path(b.row, r.anim_idle), true); break;
        case ST_WALK: set_anim(b, anim_path(b.row, has(r.anim_walk) ? r.anim_walk : r.anim_idle), true); break;
        case ST_ROAR:
            // no roar animation: it stands in its idle for a moment (think() times it)
            set_anim(b, anim_path(b.row, has(r.anim_roar) ? r.anim_roar : r.anim_idle), !has(r.anim_roar));
            play(r.snd_roar, b.pos, 1.2f);
            break;
        case ST_FLINCH: set_anim(b, anim_path(b.row, r.anim_flinch), false); play(r.snd_pain, b.pos, 1.0f); break;
        case ST_STUN:
            set_anim(b, anim_path(b.row, has(r.anim_stun) ? r.anim_stun : r.anim_flinch), false);
            play(r.snd_pain, b.pos, 1.15f);
            break;
        case ST_DEAD: set_anim(b, anim_path(b.row, r.anim_death), false); play(r.snd_death, b.pos, 1.2f); break;
        default: break;
    }
}
// How long a state's animation runs (a state without one holds briefly).
float state_len(const Boss& b, float fallback) {
    float l = anim_len(b, b.anim);
    return l > 0.05f ? l : fallback;
}

bool ground(Vec3 p, float* y) {
    Vec3 hit;
    if (world_ray({p.x, p.y + 400, p.z}, {p.x, p.y - 1600, p.z}, &hit)) { *y = hit.y; return true; }
    return false;
}

// Moves along the ground; false when a wall is in the way.
bool move(Boss& b, Vec3 d, double t) {
    if (hlen(d) < 0.01f) return true;
    Vec3 to = add(b.pos, d);
    if (t - b.lastWallCheck > 0.15) {
        b.lastWallCheck = t;
        Vec3 dir = mul(d, 1.0f / hlen(d));
        Vec3 from = {b.pos.x, b.pos.y + std::min(300.0f, height_of(b) * 0.5f), b.pos.z};
        Vec3 ahead = add(from, mul(dir, std::max(250.0f, b.fr.aMax * 0.6f)));
        Vec3 hit;
        if (world_ray(from, ahead, &hit)) return false;
    }
    float y;
    if (ground(to, &y)) {
        if (y - b.pos.y > 350) return false;  // a cliff up
        to.y = y;
    }
    b.pos = to;
    return true;
}

void turn_towards(Boss& b, float target, float dps, float dt) {
    float d = wrap(target - b.yaw), step = dps * kPi / 180.0f * dt;
    b.yaw = wrap(b.yaw + std::max(-step, std::min(step, d)));
}

// ---------------------------------------------------------------- rank

// The zone's rank: Hoarfrost Reach and the Guiding Lands are master rank; elsewhere the HP of the zone's largest
// MHW monster tells (MHW scales its monsters' HP with the quest's rank).
int area_rank(const GameSnapshot& g, float* largest) {
    float big = 0;
    for (const Monster& m : g.monsters)
        if (m.large && !is_boss(m.em)) big = std::max(big, m.maxHp);
    if (largest) *largest = big;
    if (g.zone == 108 || g.zone == 109) return RANK_MR;
    if (big >= tune(TUNING_RANK_HP_MR)) return RANK_MR;
    if (big >= tune(TUNING_RANK_HP_HR)) return RANK_HR;
    return RANK_LR;
}
float rank_hp(int rank) {
    return tune(rank == RANK_MR ? TUNING_BOSS_HP_MR : rank == RANK_HR ? TUNING_BOSS_HP_HR : TUNING_BOSS_HP_LR);
}
float rank_dmg(int rank) {
    return tune(rank == RANK_MR ? TUNING_BOSS_DMG_MR : rank == RANK_HR ? TUNING_BOSS_DMG_HR : TUNING_BOSS_DMG_LR);
}
int min_rank(int row) {
    int m = kBosses[row].min_rank;
    return m == BOSSES_MIN_RANK_MR ? RANK_MR : m == BOSSES_MIN_RANK_HR ? RANK_HR : RANK_LR;
}

std::vector<int> attacks_of(int row) {
    static std::map<int, std::vector<int>> cache;
    auto it = cache.find(row);
    if (it != cache.end()) return it->second;
    std::vector<int> v;
    for (int i = 0; i < kBoss_attacksCount; i++)
        if (kBoss_attacks[i].boss == row) v.push_back(i);
    cache[row] = v;
    return v;
}

void spawn(Live& l, int row, Vec3 at, float yaw, double t) {
    int slot = -1;
    for (int i = 0; i < kMax; i++)
        if (!g_boss[i].used) { slot = i; break; }
    if (slot < 0) return;
    Boss& b = g_boss[slot];
    b = Boss();
    b.used = true;
    b.row = row;
    b.pos = b.home = at;
    b.yaw = yaw;
    b.entrance = true;
    if (hlen(sub(l.snap.pos, at)) > 1) b.yaw = yaw_to(at, l.snap.pos);
    float largest = 0;
    b.rank = area_rank(l.snap, &largest);
    const Bosses& r = kBosses[row];
    b.maxHp = b.hp = std::max(rank_hp(b.rank) * r.hp_mult, 0.9f * largest);
    b.dmgMult = rank_dmg(b.rank) * (0.85f + 0.06f * r.threat);
    b.meleeHit = rank_hp(b.rank) / std::max(20.0f, r.melee_hits);
    // Remnant II's difficulty (chosen at Ward 13): health, damage, pace, how hard it staggers.
    b.diff = std::max(0, std::min(kDifficultiesCount - 1, g_difficulty));
    const Difficulties& d = kDifficulties[b.diff];
    b.maxHp = b.hp = b.maxHp * d.hp_mult;
    b.dmgMult *= d.dmg_mult;
    b.tempo = d.tempo;
    b.staggerNeed = r.stagger_frac * b.maxHp * d.stagger_mult * (r.style == BOSSES_STYLE_JUGGERNAUT ? 1.4f : 1.0f);
    b.orbitDir = rand() % 2 ? 1 : -1;
    for (int k : attacks_of(row)) b.reachMax = std::max(b.reachMax, kBoss_attacks[k].max_m * 100.0f * r.scale);
    b.st = ST_LOADING;
    b.stT0 = t;
    // Ask r2read for everything now, so the first attack does not wait.
    model_of(row);
    for (const char* a : {r.anim_idle, r.anim_walk, r.anim_run, r.anim_turn_l, r.anim_turn_r, r.anim_roar, r.anim_flinch,
                          r.anim_stun, r.anim_death})
        anim_of(row, anim_path(row, a));
    for (int k : attacks_of(row)) {
        anim_of(row, anim_path(row, kBoss_attacks[k].anim));
        if (has(kBoss_attacks[k].snd)) audio_prefetch(kBoss_attacks[k].snd);
    }
    for (const char* s : {r.snd_roar, r.snd_pain, r.snd_death, r.snd_step})
        if (has(s)) audio_prefetch(s);
    r2_text(r.name);  // resolved now, so the bar never waits for it
    log("bosses: %s spawning at (%.0f, %.0f, %.0f), %s, %s, %.0f hp, damage x%.2f", r.id, at.x, at.y, at.z,
        kRankName[b.rank], d.label, b.maxHp, b.dmgMult);
}

// A place on the ground at a distance from the hunter, in a direction; false when there is none.
bool place_near(const GameSnapshot& g, float yaw, float dist, Vec3* out) {
    Vec3 p = add(g.pos, mul(fwd_of(yaw), dist));
    p.y = g.pos.y;
    float y;
    if (!ground({p.x, p.y + 600, p.z}, &y)) return false;
    if (fabsf(y - g.pos.y) > 1500) return false;
    *out = {p.x, y, p.z};
    return true;
}

bool zone_listed(const Bosses& r, int zone) {
    std::string z = r.zones, me = std::to_string(zone);
    size_t i = 0;
    while (i <= z.size()) {
        size_t j = z.find(',', i);
        if (j == std::string::npos) j = z.size();
        std::string tok = z.substr(i, j - i);
        if (tok == "any" || tok == me) return true;
        i = j + 1;
    }
    return false;
}

// Loot: one Remnant item from the boss's list that the hunter does not own yet.
void loot(SaveData& s, Live& l, const Boss& b) {
    static const struct { const char* sheet; Cat cat; } kSheets[] = {
        {"weapons", CAT_WEAPON}, {"mods", CAT_MOD}, {"mutators", CAT_MUTATOR}, {"armor", CAT_ARMOR},
        {"archetypes", CAT_ARCHETYPE}, {"rings", CAT_RING}, {"amulets", CAT_AMULET}, {"relics", CAT_RELIC},
        {"fragments", CAT_FRAGMENT}, {"consumables", CAT_CONSUMABLE}, {"traits", CAT_TRAIT}, {"prisms", CAT_PRISM}};
    std::vector<std::pair<Cat, int>> pool;
    std::string all = B(b).loot;
    size_t i = 0;
    while (i < all.size()) {
        size_t j = all.find(',', i);
        if (j == std::string::npos) j = all.size();
        std::string e = all.substr(i, j - i);
        size_t dot = e.find('.');
        if (dot != std::string::npos)
            for (const auto& k : kSheets)
                if (e.compare(0, dot, k.sheet) == 0) {
                    int n = cat_count(k.cat);
                    for (int row = 0; row < n; row++)
                        if (e.compare(dot + 1, std::string::npos, item(k.cat, row).id) == 0 && !s.has(k.cat, row))
                            pool.push_back({k.cat, row});
                }
        i = j + 1;
    }
    std::string name = display_name(b.row);
    double t = now_s();
    const Difficulties& d = kDifficulties[b.diff];
    award_xp(s, l, b.maxHp / d.hp_mult * d.xp_mult * tune(TUNING_XP_PER_HP) * 1.5f, t);
    // Remnant II's other rewards: the first boss brings a prism (Voracity, as a world boss's first kill does), and
    // bosses may teach a trait the hunter does not know yet.
    int voracity = prisms_index("voracity");
    bool anyPrism = false;
    for (int i = 0; i < kPrismsCount; i++) anyPrism |= s.has(CAT_PRISM, i);
    std::string extra;
    if (voracity >= 0 && !anyPrism) {
        s.owned[CAT_PRISM][voracity] = 1;
        if (s.prismOn < 0) s.prismOn = voracity;
        extra = ", and a prism (choose its fragments at Ward 13)";
    } else if (frand() < tune(TUNING_LOOT_TRAIT_CHANCE)) {
        std::vector<int> unknown;
        for (int i = 0; i < kTraitsCount; i++)
            if (kTraits[i].kind == TRAITS_KIND_TRAIT && !s.has(CAT_TRAIT, i)) unknown.push_back(i);
        if (!unknown.empty()) {
            int tr = unknown[rand() % unknown.size()];
            s.owned[CAT_TRAIT][tr] = 1;
            extra = ", and the " + r2_text(kTraits[tr].name) + " trait";
        }
    }
    l.loadoutChanged = true;
    if (pool.empty()) {
        toast(l, "%s defeated%s", name.c_str(), extra.c_str());
        return;
    }
    auto pick = pool[rand() % pool.size()];
    if (pick.first == CAT_CONSUMABLE) s.stock[pick.second]++;  // consumables are counted in the stock, not owned
    else s.owned[pick.first][pick.second] = 1;
    l.dirty = true;
    ItemView v = item(pick.first, pick.second);
    const std::string& got = v.name ? r2_text(*v.name) : std::string();
    toast(l, "%s defeated: %s added to your Remnant gear%s", name.c_str(), got.empty() ? v.id : got.c_str(), extra.c_str());
    log("bosses: %s defeated, loot %s", B(b).id, v.id);
}

// The hunter is inside an attack's reach and arc.
bool in_reach(const Boss& b, const Boss_attacks& a, Vec3 hunter) {
    if (fabsf(hunter.y - b.pos.y) > std::max(500.0f, height_of(b))) return false;
    float reach = a.reach_m * 100.0f * B(b).scale + 80.0f;
    Vec3 d = sub(hunter, b.pos);
    if (hlen(d) > reach) return false;
    if (a.arc_deg >= 359) return true;
    float ang = fabsf(wrap(yaw_to(b.pos, hunter) - b.yaw)) * 180.0f / kPi;
    return ang <= a.arc_deg * 0.5f || hlen(d) < 250.0f * B(b).scale;
}

bool window(const char* hits, int i, float* s, float* e) {
    const char* p = hits;
    for (int k = 0; k < i && p; k++) {
        p = strchr(p, ',');
        if (p) p++;
    }
    return p && sscanf(p, "%f-%f", s, e) == 2;
}

// Damage to the hunter from a boss (dodging through it is Remnant's evade).
float status_dur(int st) { return st == SS_BURNING ? 6.0f : 10.0f; }
int hurt_hunter(Live& l, float dmg, int status, float statusTotal, double t) {
    const GameSnapshot& g = l.snap;
    if (t - g_dodgeAt < 0.45) return BOSS_EV_EVADE;
    if (status > 0) inflict_player(l, (Status)status, statusTotal, status_dur(status), t);
    reactions_note_own_damage(t);
    if (g.playerHealth && add_hp(g.playerHealth, -dmg)) {
        if (l.maxHp > 0) reactions_flinch(dmg / l.maxHp, t);  // MHW's own flinch / knock-back for the hit
        std::lock_guard<std::mutex> lk(g_lock);
        g_hurtAt = t;
        return BOSS_EV_HURT;
    }
    return 0;
}

// What a boss's status costs the hunter: a share of a hunter's health, more in higher ranks.
float status_total(const Boss& b) {
    float hp = 150.0f;
    if (g_snap && g_snap->playerHealth && mem_readable(g_snap->playerHealth + 0x60, 4)) hp = *(float*)(g_snap->playerHealth + 0x60);
    return hp * tune(TUNING_BOSS_STATUS_FRAC) * sqrtf(std::max(1.0f, b.dmgMult));
}

// ---------------------------------------------------------------- turf wars with MHW monsters (turf.cpp has the
// MHW monster's side)
const Monster* find_mon(const GameSnapshot& g, const uint8_t* em) {
    for (const Monster& m : g.monsters)
        if (m.em == em) return &m;
    return nullptr;
}
Vec3 body_center(const Boss& b) {
    return add(add(b.pos, mul(fwd_of(b.yaw), (b.fr.aMin + b.fr.aMax) * 0.5f)),
               mul(right_of(b.yaw), (b.fr.cMin + b.fr.cMax) * 0.5f));
}
float avg_attack(int row) {
    float sum = 0;
    int n = 0;
    for (int k : attacks_of(row)) { sum += kBoss_attacks[k].damage; n++; }
    return n ? sum / n : 1.0f;
}
// A boss's blow on the MHW monster it fights: a share of the monster's health, more for its heavier attacks.
void hurt_foe(Boss& b, const Monster& m, float attackDmg) {
    float k = std::max(0.5f, std::min(2.0f, attackDmg / std::max(1.0f, avg_attack(b.row))));
    float share = tune(TUNING_TURF_HIT_FRAC) * k * (0.85f + 0.3f * frand());
    turf_hurt(m, share * m.maxHp);
    b.foeLost += share;
    if (k >= 0.9f || frand() < 0.4f) turf_flinch(m);
}

void fire_projectile(const Boss& b, const Boss_attacks& a, Vec3 target, double t, const Monster* foe = nullptr) {
    for (Proj& p : g_proj) {
        if (p.live) continue;
        Vec3 o = add(add(b.pos, mul(fwd_of(b.yaw), std::max(80.0f, b.fr.aMax * 0.5f))), {0, height_of(b) * 0.62f, 0});
        Vec3 d = sub(target, o);
        float n = std::max(1.0f, len3(d));
        p.live = true;
        p.p = o;
        p.v = mul(d, tune(TUNING_BOSS_PROJ_SPEED) / n);
        p.dmg = a.damage * b.dmgMult;
        p.radius = 150.0f;
        p.until = t + std::min(5.0, (a.max_m * 100.0 + 800.0) / tune(TUNING_BOSS_PROJ_SPEED));
        p.color = parse_color(B(b).fx_color);
        p.status = a.inflict;
        p.statusTotal = status_total(b);
        p.foe = foe ? foe->em : nullptr;
        p.boss = (int)(&b - g_boss);
        p.attackDmg = a.damage;
        return;
    }
}

int projectiles(Live& l, double t, float dt) {
    const GameSnapshot& g = l.snap;
    int ev = 0;
    Vec3 chest = add(g.pos, {0, 110, 0});
    for (Proj& p : g_proj) {
        if (!p.live) continue;
        Vec3 next = add(p.p, mul(p.v, dt));
        Vec3 hit;
        if (t > p.until || world_ray(p.p, next, &hit)) { p.live = false; continue; }
        p.p = next;
        if (p.foe && g_snap && p.boss >= 0 && g_boss[p.boss].used) {
            const Monster* m = find_mon(*g_snap, p.foe);
            if (m && len3(sub(p.p, turf_closest(*m, p.p))) < p.radius + 60) {
                hurt_foe(g_boss[p.boss], *m, p.attackDmg);
                p.live = false;
                continue;
            }
        }
        if (len3(sub(p.p, chest)) < p.radius) {
            ev |= hurt_hunter(l, p.dmg, p.status, p.statusTotal, t);
            p.live = false;
        }
    }
    return ev;
}

// One tick of a living boss.
int think(Boss& b, Live& l, double t, float dt) {
    const GameSnapshot& g = l.snap;
    const Bosses& r = B(b);
    int ev = 0;
    const Monster* foe = b.foe ? find_mon(g, b.foe) : nullptr;  // a turf war: the MHW monster is the target
    Vec3 hunter = foe ? turf_closest(*foe, head_point(b)) : g.pos;
    float dist = hlen(sub(hunter, head_point(b)));
    float toHunter = yaw_to(b.pos, hunter);
    float off = wrap(toHunter - b.yaw);
    bool mobile = r.walk_mps > 0.05f && has(r.anim_walk);
    bool sees = foe != nullptr;
    if (!foe && dist < r.aggro_m * 100.0f) {
        Vec3 hit, eye = add(head_point(b), {0, std::min(300.0f, height_of(b) * 0.6f), 0});
        sees = !world_ray(eye, add(hunter, {0, 120, 0}), &hit);
        if (dist < r.aggro_m * 50.0f) sees = true;  // close by: a Remnant boss notices the hunter whatever the sight ray says
    }
    if (sees) b.lastSeen = t;
    if (!b.aggro && (sees || b.hp < b.maxHp)) b.aggro = true;
    if (b.aggro && dist > r.leash_m * 100.0f && t - b.lastSeen > 15) {
        b.aggro = b.roared = false;
        b.home = b.pos;
        go(b, ST_IDLE, t);
    }
    b.staggerAcc = std::max(0.0f, b.staggerAcc - b.staggerNeed * 0.02f * dt);
    // Enraged below a share of its health that grows with the difficulty: a roar, then a faster pace.
    const Difficulties& diff = kDifficulties[b.diff];
    if (!b.enraged && diff.enrage_frac > 0 && b.hp < diff.enrage_frac * b.maxHp &&
        (b.st == ST_CHASE || b.st == ST_IDLE || b.st == ST_WALK)) {
        b.enraged = true;
        b.tempo *= 1.25f;
        log("bosses: %s is enraged", r.id);
        if (has(r.anim_roar)) { go(b, ST_ROAR, t); return ev; }
    }
    float pace = std::sqrt(b.tempo);  // turning and moving: a little faster with the pace

    switch (b.st) {
        case ST_IDLE:
        case ST_WALK: {
            if (b.aggro) {
                if (!b.roared && has(r.anim_roar)) go(b, ST_ROAR, t);
                else { b.roared = true; go(b, ST_CHASE, t); }
                break;
            }
            if (t > b.goalUntil) {
                if (b.st == ST_IDLE && mobile) {
                    float a = frand() * 2 * kPi, d = 800 + frand() * 2200;
                    b.goal = add(b.home, mul(fwd_of(a), d));
                    b.goalUntil = t + 12;
                    go(b, ST_WALK, t);
                } else {
                    b.goalUntil = t + 3 + frand() * 4;
                    go(b, ST_IDLE, t);
                }
            }
            if (b.st == ST_WALK) {
                turn_towards(b, yaw_to(b.pos, b.goal), r.turn_dps * 0.6f, dt);
                if (fabsf(wrap(yaw_to(b.pos, b.goal) - b.yaw)) < 0.6f &&
                    !move(b, mul(fwd_of(b.yaw), r.walk_mps * 100.0f * B(b).scale * dt), t))
                    b.goalUntil = 0;  // a wall: pick somewhere else
                if (hlen(sub(b.goal, b.pos)) < 300) b.goalUntil = 0;
            }
            break;
        }
        case ST_ROAR: {
            turn_towards(b, toHunter, r.turn_dps, dt);
            if (has(r.anim_roar) ? b.at >= state_len(b, 1.5f) : t - b.stT0 >= 1.5) {
                b.roared = true;
                if (!b.aggro) { b.aggro = true; b.lastSeen = t; }  // the entrance is over: now it engages
                b.nextAttack = t + 0.4 / b.tempo;
                go(b, ST_CHASE, t);
            }
            break;
        }
        case ST_CHASE: {
            // Where it wants to be: each style keeps its own distance, and moves between attacks in its own way.
            int style = r.style;
            float prefer = std::min(r.prefer_m * 100.0f, b.reachMax * 0.75f);  // never further than its attacks reach
            bool ready = t >= b.nextAttack && t >= b.retreatUntil;
            if (t > b.nextFlip) {
                b.nextFlip = t + 4 + frand() * 4;
                if (frand() < 0.5f) b.orbitDir = -b.orbitDir;
            }
            Vec3 goal = hunter;
            bool approach = true;  // heading for the target (else repositioning)
            if (!mobile || style == BOSSES_STYLE_SENTINEL) {
                approach = false;
                goal = b.pos;
            } else if (style == BOSSES_STYLE_RANGED || style == BOSSES_STYLE_CASTER) {
                Vec3 away = sub(b.pos, hunter);
                float n = std::max(1.0f, hlen(away));
                if (dist < prefer * 0.55f) {  // pressed: back off
                    goal = add(hunter, mul(Vec3{away.x / n, 0, away.z / n}, prefer));
                    approach = false;
                } else if (dist < prefer * 1.35f) {  // in its band: circle slowly
                    float a = yaw_to(hunter, b.pos) + b.orbitDir * 0.5f;
                    goal = add(hunter, mul(fwd_of(a), prefer));
                    approach = false;
                }
            } else if (!ready && (style == BOSSES_STYLE_HUNTER || style == BOSSES_STYLE_DUELIST)) {
                // between attacks: circle the target, the hunter further out than the duelist
                float a = yaw_to(hunter, b.pos) + b.orbitDir * (style == BOSSES_STYLE_HUNTER ? 0.9f : 0.6f);
                goal = add(hunter, mul(fwd_of(a), std::max(prefer, 300.0f)));
                approach = false;
            }
            bool turnAnims = has(r.anim_turn_l) && has(r.anim_turn_r);
            if (approach && fabsf(off) > 1.75f && dist < 2000 && turnAnims) {
                b.turnFrom = b.yaw;
                b.turnBy = off > 0 ? std::min(off, kPi / 2) : std::max(off, -kPi / 2);
                bool toRight = (off > 0) != g_rh;  // positive yaw turns towards +x
                b.st = ST_TURN;
                b.stT0 = t;
                set_anim(b, anim_path(b.row, toRight ? r.anim_turn_r : r.anim_turn_l), false);
                break;
            }
            if (ready) {
                turn_towards(b, toHunter, r.turn_dps * pace, dt);
                float total = 0;
                int pick = -1;
                for (int k : attacks_of(b.row)) {
                    const Boss_attacks& a = kBoss_attacks[k];
                    bool facing = a.arc_deg >= 359 || fabsf(off) < 0.6f || a.min_m > 10;
                    float range = dist / 100.0f / std::max(0.3f, B(b).scale);
                    auto cd = b.cd.find(k);
                    if ((cd != b.cd.end() && t < cd->second) || !facing || range < a.min_m || range > a.max_m) continue;
                    if (!anim_of(b.row, anim_path(b.row, a.anim))) continue;
                    // each style leans on its own kind of attack
                    float w = a.weight;
                    bool ranged = a.kind == BOSS_ATTACKS_KIND_RANGED, aoe = a.kind == BOSS_ATTACKS_KIND_AOE;
                    if (style == BOSSES_STYLE_RANGED) w *= ranged ? 2.5f : aoe ? 1.2f : (dist < 600 ? 1.0f : 0.3f);
                    else if (style == BOSSES_STYLE_CASTER) w *= aoe ? 2.5f : ranged ? 1.8f : (dist < 600 ? 1.0f : 0.4f);
                    else if (style == BOSSES_STYLE_BRUTE || style == BOSSES_STYLE_JUGGERNAUT) w *= ranged ? 0.6f : aoe ? 1.4f : 1.5f;
                    else if (style == BOSSES_STYLE_HUNTER) w *= (a.max_m > 8 && dist > 600) ? 2.0f : 1.0f;  // pounces
                    else if (style == BOSSES_STYLE_DUELIST) w *= ranged ? 0.7f : 1.3f;
                    total += w;
                    if (frand() * total <= w) pick = k;
                }
                if (pick >= 0) {
                    const Boss_attacks& a = kBoss_attacks[pick];
                    b.attack = pick;
                    b.windowsDone = 0;
                    b.fired = false;
                    b.st = ST_ATTACK;
                    b.stT0 = t;
                    set_anim(b, anim_path(b.row, a.anim), false);
                    b.cd[pick] = t + a.cooldown_s / b.tempo;
                    play(a.snd, b.pos, 1.0f);
                    break;
                }
            }
            if (!mobile) {
                set_anim(b, anim_path(b.row, r.anim_idle), true);
                if (!ready) turn_towards(b, toHunter, r.turn_dps * pace, dt);
                break;
            }
            float toGoal = hlen(sub(goal, b.pos));
            float stopAt = approach ? 350.0f : 250.0f;
            bool going = approach ? dist > stopAt : toGoal > stopAt;
            if (!going) {  // there: face the target and wait for the next attack
                turn_towards(b, toHunter, r.turn_dps * pace, dt);
                set_anim(b, anim_path(b.row, r.anim_idle), true);
                break;
            }
            float want = approach ? toHunter : yaw_to(b.pos, goal);
            turn_towards(b, want, r.turn_dps * pace, dt);
            bool distant = (approach ? dist : toGoal) > 1400 && has(r.anim_run);
            bool hurry = distant || (style == BOSSES_STYLE_HUNTER && has(r.anim_run));
            set_anim(b, anim_path(b.row, hurry ? r.anim_run : r.anim_walk), true);
            if (fabsf(wrap(want - b.yaw)) < 1.0f)
                move(b, mul(fwd_of(b.yaw), (hurry ? r.run_mps : r.walk_mps) * 100.0f * B(b).scale * pace * dt), t);
            break;
        }
        case ST_TURN: {
            float len = std::max(0.1f, anim_len(b, b.anim));
            float k = std::min(1.0f, b.at / len);
            b.yaw = wrap(b.turnFrom + b.turnBy * (k * k * (3 - 2 * k)));
            if (b.at >= len) go(b, ST_CHASE, t);
            break;
        }
        case ST_ATTACK: {
            const Boss_attacks& a = kBoss_attacks[b.attack];
            const Anim* an = anim_of(b.row, b.anim);
            float len = an ? an->length() : 0;
            float frac = len > 0 ? b.at / len : 1;
            float first = 1, firstEnd = 1;
            window(a.hits, 0, &first, &firstEnd);
            if (frac < first) turn_towards(b, toHunter, r.turn_dps * a.track, dt);
            // The animation's own motion moves the creature (lunges, leaps, charges).
            if (an) {
                float rt[3];
                anim_root(*an, std::min(b.at, len), false, rt);
                float dx = rt[0] - b.root[0], dy = rt[1] - b.root[1];
                memcpy(b.root, rt, sizeof(rt));
                float along = (dx * b.fr.f[0] + dy * b.fr.f[1]) * B(b).scale;
                float across = (dx * b.fr.r[0] + dy * b.fr.r[1]) * B(b).scale;
                if (mobile || fabsf(along) + fabsf(across) < 400) move(b, add(mul(fwd_of(b.yaw), along), mul(right_of(b.yaw), across)), t);
            }
            if (a.kind == BOSS_ATTACKS_KIND_RANGED) {
                if (!b.fired && frac >= first) {
                    b.fired = true;
                    fire_projectile(b, a, add(hunter, {0, foe ? 60.0f : 110.0f, 0}), t, foe);
                }
            } else {
                for (int w = 0; w < 8; w++) {
                    float ws, we;
                    if (!window(a.hits, w, &ws, &we)) break;
                    if ((b.windowsDone & (1 << w)) || frac < ws || frac > we) continue;
                    // the hunter standing in a blow meant for a monster is hit too
                    bool onFoe = foe && in_reach(b, a, hunter), onHunter = in_reach(b, a, g.pos);
                    if (!onFoe && !onHunter) continue;
                    b.windowsDone |= 1 << w;
                    if (onFoe) hurt_foe(b, *foe, a.damage);
                    if (onHunter)
                        ev |= hurt_hunter(l, a.damage * b.dmgMult,
                                          frand() < (a.kind == BOSS_ATTACKS_KIND_AOE ? 0.8f : 0.4f) ? a.inflict : 0,
                                          status_total(b) * kDifficulties[b.diff].status_mult, t);
                }
            }
            if (b.at >= len) {
                b.attack = -1;
                // a chained attack, or a pause (hunters and duelists step around the target in it)
                float combo = r.combo + kDifficulties[b.diff].combo_bonus + (b.enraged ? 0.1f : 0.0f);
                if (frand() < combo) {
                    b.nextAttack = t + 0.12;
                } else {
                    b.nextAttack = t + (0.5 + frand() * 1.2) / b.tempo;
                    if (r.style == BOSSES_STYLE_HUNTER || r.style == BOSSES_STYLE_DUELIST)
                        b.retreatUntil = t + (1.0 + frand() * 1.2) / b.tempo;
                }
                go(b, ST_CHASE, t);
            }
            break;
        }
        case ST_FLINCH:
        case ST_STUN: {
            if (b.at >= state_len(b, b.st == ST_STUN ? 2.5f : 0.8f)) {
                b.nextAttack = t + (b.st == ST_STUN ? 0.8 : 0.3);
                go(b, b.aggro ? ST_CHASE : ST_IDLE, t);
            }
            break;
        }
        default: break;
    }
    // Footsteps while moving.
    if ((b.st == ST_WALK || (b.st == ST_CHASE && mobile)) && has(r.snd_step)) {
        bool running = b.anim == anim_path(b.row, r.anim_run);
        if (t - b.lastStep > (running ? 0.32 : 0.55)) {
            b.lastStep = t;
            play(r.snd_step, b.pos, running ? 0.55f : 0.35f);
        }
    }
    return ev;
}

// The hunter's distance to a boss's body, and the closest point of it.
float gap_to(const Boss& b, Vec3 p, Vec3* closest) {
    Vec3 d = sub(p, b.pos);
    Vec3 f = fwd_of(b.yaw), rt = right_of(b.yaw);
    float along = d.x * f.x + d.z * f.z, across = d.x * rt.x + d.z * rt.z;
    float ca = std::max(b.fr.aMin, std::min(b.fr.aMax, along)), cx = std::max(b.fr.cMin, std::min(b.fr.cMax, across));
    if (closest) *closest = add(b.pos, add(mul(f, ca), mul(rt, cx)));
    return sqrtf((along - ca) * (along - ca) + (across - cx) * (across - cx));
}

int boss_index(const uint8_t* em) { return (int)(em - g_token); }
void hit_boss(int i, float dmg, const Vec3& at, bool crit, bool weak, float stagger, bool hunter = true);

// MHW's weapon damage per hit, relative to an average hit (motion values, roughly), and how much it staggers.
struct WeaponFeel { float dmg, stagger; };
WeaponFeel weapon_feel(int type) {
    switch (type) {
        case 0: return {2.4f, 1.5f};   // great sword
        case 1: return {0.6f, 1.0f};   // sword and shield
        case 2: return {0.45f, 0.8f};  // dual blades
        case 3: return {1.0f, 1.0f};   // long sword
        case 4: return {1.5f, 1.9f};   // hammer
        case 5: return {1.3f, 1.8f};   // hunting horn
        case 6: return {0.9f, 1.0f};   // lance
        case 7: return {1.0f, 1.1f};   // gunlance
        case 8: return {1.1f, 1.2f};   // switch axe
        case 9: return {1.2f, 1.6f};   // charge blade
        case 10: return {0.8f, 0.9f};  // insect glaive
        case 11: return {0.8f, 0.7f};  // bow
        case 12: return {1.1f, 1.0f};  // heavy bowgun
        case 13: return {0.7f, 0.7f};  // light bowgun
        default: return {1.0f, 1.0f};
    }
}

// MHW's weapons against the bosses: an attack press lands a moment later where MHW's own hitboxes cannot (they only
// know MHW's monsters). Melee weapons: within reach of the body. Bows and bowguns: the shot's line from the camera.
void mhw_weapons(Live& l, double t) {
    bool lmb = key_down(VK_LBUTTON), rmb = key_down(VK_RBUTTON);
    bool press = (lmb && !g_lmbWas) || (rmb && !g_rmbWas && player_weapon_type() < 11);
    g_lmbWas = lmb;
    g_rmbWas = rmb;
    if (press && !l.gunMode && g_swingAt == 0) {
        g_swingWeapon = player_weapon_type();
        g_swingAt = t + (g_swingWeapon >= 11 ? 0.08 : g_swingWeapon == 0 ? 0.55 : 0.28);
    }
    if (g_swingAt == 0 || t < g_swingAt) return;
    g_swingAt = 0;
    const GameSnapshot& g = l.snap;
    WeaponFeel wf = weapon_feel(g_swingWeapon);
    Vec3 look = g.view.valid ? sub(g.view.camTarget, g.view.camPos) : Vec3{0, 0, 1};
    if (g_swingWeapon >= 11 && g.view.valid) {  // a shot along the camera's line
        Vec3 dir = mul(look, 1.0f / std::max(1.0f, len3(look)));
        float best = 7000;
        int bi = -1;
        for (const Monster& m : g.monsters) {
            if (!is_boss(m.em)) continue;
            const Boss& sb = g_boss[boss_index(m.em)];
            if (!sb.sph.empty()) {  // the whole-model hitbox: the nearest bone sphere the line passes through
                float th;
                bool head;
                if (ray_spheres(sb, g.view.camPos, dir, best, &th, &head)) { best = th; bi = boss_index(m.em); }
                continue;
            }
            Vec3 rel = sub(g.view.camPos, m.center);  // fallback before the model is posed: the old volume
            float t0 = 0, t1 = 1e9f;
            bool ok = true;
            float h[3] = {m.half.x, m.half.y, m.half.z};
            for (int k = 0; k < 3 && ok; k++) {
                Vec3 ax = {m.rot[k][0], m.rot[k][1], m.rot[k][2]};
                float o = rel.x * ax.x + rel.y * ax.y + rel.z * ax.z, dd = dir.x * ax.x + dir.y * ax.y + dir.z * ax.z;
                if (fabsf(dd) < 1e-6f) { ok = fabsf(o) <= h[k]; continue; }
                float a = (-h[k] - o) / dd, c = (h[k] - o) / dd;
                if (a > c) std::swap(a, c);
                t0 = std::max(t0, a);
                t1 = std::min(t1, c);
                ok = t0 <= t1;
            }
            if (ok && t0 < best) { best = t0; bi = boss_index(m.em); }
        }
        if (bi >= 0) {
            Vec3 at = add(g.view.camPos, mul(dir, best));
            Vec3 wall;
            if (!(world_ray(g.view.camPos, at, &wall) && len3(sub(wall, g.view.camPos)) + 50 < best))
                hit_boss(bi, g_boss[bi].meleeHit * wf.dmg * (0.85f + 0.3f * frand()), at, false, false, wf.stagger);
        }
        return;
    }
    for (int i = 0; i < kMax; i++) {
        Boss& b = g_boss[i];
        if (!b.used || b.st == ST_LOADING || b.st == ST_DEAD) continue;
        Vec3 to;
        float gap;
        bool headHit = false;
        if (!b.sph.empty()) {  // the reach point (380 cm from the hunter's chest) inside any bone sphere
            Vec3 chest = add(g.pos, {0, 110, 0});
            float bestGap = 1e30f;
            for (const WSph& s : b.sph) {
                Vec3 d = sub(s.c, chest);
                float dl = len3(d), gp = std::max(0.0f, dl - s.r);
                if (gp >= bestGap) continue;
                Vec3 at = dl > 0.01f ? add(chest, mul(d, gp / dl)) : chest;
                bestGap = gp; to = at; headHit = s.head;
            }
            gap = bestGap;
            if (gap > 380) continue;
            Vec3 dir = sub(to, chest);
            float facing = (dir.x * look.x + dir.z * look.z) / std::max(1.0f, hlen(dir) * hlen(look));
            if (gap > 120 && facing < 0.1f) continue;
        } else {  // fallback before the model is posed: the old volume
            gap = gap_to(b, g.pos, &to);
            float dy = g.pos.y - b.pos.y;
            if (gap > 380 || dy < -250 || dy > b.fr.zMax + 200) continue;
            Vec3 dir = sub(to, g.pos);
            float facing = (dir.x * look.x + dir.z * look.z) / std::max(1.0f, hlen(dir) * hlen(look));
            if (gap > 120 && facing < 0.1f) continue;
        }
        // a hit high on the body counts as a weak spot when it lands at the head end (or on a head bone)
        Vec3 f = fwd_of(b.yaw);
        float along = (to.x - b.pos.x) * f.x + (to.z - b.pos.z) * f.z;
        bool weak = headHit || along > b.fr.aMax * 0.6f;
        hit_boss(i, b.meleeHit * wf.dmg * (weak ? 1.3f : 1.0f) * (0.85f + 0.3f * frand()),
                 {to.x, b.sph.empty() ? g.pos.y + 120 : to.y, to.z}, false, weak, wf.stagger);
        break;
    }
}

void world_matrix(const Boss& b, float w[16]) {
    float s = B(b).scale;
    Vec3 f = fwd_of(b.yaw), r = right_of(b.yaw);
    const Frame& fr = b.fr;
    // model x and y go to forward / right by the boss's facing; model z is up
    Vec3 ix = add(mul(f, fr.f[0]), mul(r, fr.r[0])), iy = add(mul(f, fr.f[1]), mul(r, fr.r[1]));
    float rows[4][4] = {{ix.x * s, ix.y * s, ix.z * s, 0},
                        {iy.x * s, iy.y * s, iy.z * s, 0},
                        {0, s, 0, 0},
                        {b.pos.x, b.pos.y - b.sink, b.pos.z, 1}};
    memcpy(w, rows, 64);
}

std::string mhw_name(const char* kind) {
    if (!kind || !kind[0]) return {};
    int i = mhw_monsters_index(kind);
    return i >= 0 ? kMhw_monsters[i].name : std::string();
}

// MHW's monsters: the one being fought gets a docked bar; the others nearby, in sight, a bar over them.
void mhw_bars(const Live& l, double t) {
    const GameSnapshot& g = l.snap;
    std::vector<Bar> bars;
    Docked focus{};
    bool hasFocus = false;
    const Monster* f = nullptr;
    for (const Monster& m : g.monsters)
        if (m.em == l.target && m.large) f = &m;
    if (!f) {
        float best = 3000.0f * 3000.0f;
        for (const Monster& m : g.monsters) {
            if (!m.large || is_boss(m.em)) continue;
            Vec3 d = sub(m.center, g.pos);
            float dd = d.x * d.x + d.y * d.y + d.z * d.z;
            if (dd < best) { best = dd; f = &m; }
        }
    }
    if (f) {
        focus = {f->em, mhw_name(f->kind), f->hp, f->maxHp, status_bits(f->em)};
        hasFocus = true;
    }
    if (g_los.size() > 96) g_los.clear();
    for (const Monster& m : g.monsters) {
        if (m.kind[0] && g_kindsMet.insert(m.kind).second) {
            std::string n = mhw_name(m.kind);
            log("monsters: met %s (%s)", m.kind, n.empty() ? (m.large ? "no name in sheets/mhw_monsters.json" : "small") : n.c_str());
        }
        if (is_boss(m.em) || !m.obbOk || m.maxHp <= 0 || &m == f) continue;
        float h[3] = {m.half.x, m.half.y, m.half.z}, up = 0;
        for (int k = 0; k < 3; k++) up += fabsf(m.rot[k][1]) * h[k];
        Vec3 anchor = {m.center.x, m.center.y + up + 60, m.center.z};
        float d = hlen(sub(anchor, g.pos));
        bool hurt = m.hp < m.maxHp - 0.5f;
        float range = m.large ? 6000.0f : 2500.0f;
        if (d > (hurt ? range * 1.6f : range)) continue;
        if (!m.large && !hurt && d > 1500) continue;
        Seen& s = g_los[m.em];
        if (t - s.at > 0.25) {
            Vec3 hit;
            s.vis = !g.view.valid || !world_ray(g.view.camPos, anchor, &hit);
            s.at = t;
        }
        if (!s.vis) continue;
        bars.push_back({m.em, m.large ? 1 : 0, m.large ? mhw_name(m.kind) : std::string(), m.hp, m.maxHp, anchor,
                        status_bits(m.em)});
    }
    std::lock_guard<std::mutex> lk(g_lock);
    g_bars.swap(bars);
    g_focus = focus;
    g_hasFocus = hasFocus;
}


bool project(const GameView& v, const Vec3& p, ImVec2 vpPos, ImVec2 vpSize, ImVec2* out) {
    float in[4] = {p.x, p.y, p.z, 1}, a[4], c[4];
    for (int j = 0; j < 4; j++) a[j] = in[0] * v.view[j] + in[1] * v.view[4 + j] + in[2] * v.view[8 + j] + in[3] * v.view[12 + j];
    for (int j = 0; j < 4; j++) c[j] = a[0] * v.proj[j] + a[1] * v.proj[4 + j] + a[2] * v.proj[8 + j] + a[3] * v.proj[12 + j];
    if (c[3] <= 1.0f) return false;
    out->x = vpPos.x + (c[0] / c[3] * 0.5f + 0.5f) * vpSize.x;
    out->y = vpPos.y + (0.5f - c[1] / c[3] * 0.5f) * vpSize.y;
    return true;
}


void hit_boss(int i, float dmg, const Vec3& at, bool crit, bool weak, float stagger, bool hunter) {
    if (i < 0 || i >= kMax) return;
    Boss& b = g_boss[i];
    if (!b.used || b.st == ST_LOADING || b.st == ST_DEAD || dmg <= 0) return;
    double t = now_s();
    b.hp -= dmg;
    b.aggro = true;
    b.flinchAcc += dmg;
    b.staggerAcc += dmg * stagger * (weak ? 2.0f : 1.0f);
    if (hunter) {  // a monster's blow in a turf war shows no number and does not draw the boss to the hunter
        b.lastSeen = t;
        b.lastHunterHit = t;
        if (b.foe) b.hunterDmg += dmg;
        std::lock_guard<std::mutex> lk(g_lock);
        g_numbers[g_nextNumber++ % 32] = {at, (int)lroundf(dmg), crit, weak, t};
    }
    const Bosses& r = B(b);
    if (b.hp <= 0) {
        b.hp = 0;
        go(b, ST_DEAD, t);
        return;
    }
    if (b.st == ST_ROAR || b.st == ST_STUN) return;
    // Stagger: the meter (weak spots and heavy weapons fill it faster) or a third of its health gone; each stagger
    // makes the next one harder, as Remnant's and MHW's bosses both do.
    int phase = b.hp < b.maxHp / 3 ? 2 : b.hp < 2 * b.maxHp / 3 ? 1 : 0;
    bool phaseBreak = phase > 0 && !(b.phases & (1 << phase));
    if (b.staggerAcc >= b.staggerNeed || phaseBreak) {
        if (phaseBreak) b.phases |= 1 << phase;
        b.staggerAcc = 0;
        b.flinchAcc = 0;
        b.staggers++;
        b.staggerNeed *= 1.45f;
        b.attack = -1;
        go(b, ST_STUN, t);
        log("bosses: %s staggered (%d)", r.id, b.staggers);
    } else if (b.flinchAcc >= r.flinch_frac * b.maxHp * (r.style == BOSSES_STYLE_JUGGERNAUT ? 2.0f : 1.0f) &&
               has(r.anim_flinch)) {
        b.flinchAcc = 0;
        b.attack = -1;
        go(b, ST_FLINCH, t);
    }
}

void end_turf(Boss& b, const Monster* foe, const char* why, double t) {
    log("bosses: %s's turf war with %s ends (%s): boss -%.0f%%, monster -%.0f%%", B(b).id, foe ? foe->kind : "?", why,
        b.bossLost * 100, b.foeLost * 100);
    bool decided = foe && b.st != ST_DEAD && (!strcmp(why, "beaten") || !strcmp(why, "time"));
    if (decided) {  // the one hurt worse goes down
        if (b.bossLost > b.foeLost) {
            b.attack = -1;
            go(b, ST_STUN, t);
        } else {
            turf_flinch(*foe);
        }
    }
    turf_claim(b.foe, false);
    g_foeCd[b.foe] = t + tune(TUNING_TURF_COOLDOWN_S);
    b.foe = nullptr;
    b.foeHitAt = 0;
    b.nextAttack = t + 1.0;
    if (g_snap && hlen(sub(g_snap->pos, b.pos)) > B(b).aggro_m * 100.0f) {  // the hunter is not around: back to roaming
        b.aggro = b.roared = false;
        b.home = b.pos;
        if (b.st != ST_STUN && b.st != ST_DEAD) go(b, ST_IDLE, t);
    }
}

// A boss meeting a large MHW monster may fight it (MHW's turf wars, with a Remnant side). The boss's blows come from
// think(); the monster's are played here through turf.cpp.
void boss_turf(Boss& b, int i, Live& l, double t, float dt) {
    const GameSnapshot& g = l.snap;
    const Bosses& r = B(b);
    if (b.foe) {
        const Monster* foe = find_mon(g, b.foe);
        const char* why = !foe                                        ? "the monster left"
                          : t > b.foeUntil                            ? "time"
                          : b.foeLost >= 0.25f || b.bossLost >= 0.25f ? "beaten"
                          : b.hunterDmg >= 0.10f * b.maxHp            ? "the hunter joined in"
                          : hlen(sub(foe->center, b.pos)) > 7000      ? "apart"
                                                                      : nullptr;
        if (why) return end_turf(b, foe, why, t);
        Vec3 me = body_center(b);
        if (b.foeHitAt > 0) {
            if (t < b.foeHitAt) return;
            b.foeHitAt = 0;
            b.foeNext = t + 1.4 + frand() * 1.8;
            Vec3 c = turf_closest(*foe, me);
            if (gap_to(b, c, nullptr) > b.foeReach + 150 || b.st == ST_DEAD) return;
            float size = std::max(0.75f, std::min(1.3f, powf(foe->maxHp / std::max(1.0f, rank_hp(b.rank)), 0.3f)));
            float dmg = b.maxHp * tune(TUNING_TURF_HIT_FRAC) * size * (0.8f + 0.45f * frand());
            b.bossLost += dmg / b.maxHp;
            hit_boss(i, dmg, add(c, {0, height_of(b) * 0.4f, 0}), false, false, 1.2f, false);
            return;
        }
        turf_face(*foe, me, dt);
        if (t < b.foeNext) return;
        float gap = gap_to(b, turf_closest(*foe, me), nullptr);
        if (gap > 1500) { b.foeNext = t + 0.5; return; }  // out of its reach: the boss closes in
        b.foeHitAt = t + turf_attack(*foe, gap, &b.foeReach);
        return;
    }
    if (!turf_enabled() || t < b.foeCheck || g.inBase) return;
    b.foeCheck = t + 1.0;
    if (b.st != ST_IDLE && b.st != ST_WALK && b.st != ST_CHASE) return;
    if ((b.aggro && hlen(sub(g.pos, b.pos)) < 1500) || t - b.lastHunterHit < 10) return;  // busy with the hunter
    const Monster* best = nullptr;
    float bestD = std::max(2500.0f, r.aggro_m * 120.0f);
    for (const Monster& m : g.monsters) {
        if (!m.large || !m.health || !m.obbOk || is_boss(m.em) || turf_busy(m.em)) continue;
        bool taken = false;
        for (const Boss& o : g_boss) taken |= o.used && o.foe == m.em;
        auto cd = g_foeCd.find(m.em);
        if (taken || (cd != g_foeCd.end() && t < cd->second)) continue;
        float d = hlen(sub(turf_closest(m, b.pos), b.pos));
        if (d < bestD) { bestD = d; best = &m; }
    }
    if (!best) return;
    g_foeCd[best->em] = t + tune(TUNING_TURF_COOLDOWN_S);  // one roll per meeting
    if (frand() > tune(TUNING_TURF_BOSS_CHANCE)) return;
    b.foe = best->em;
    b.foeUntil = t + tune(TUNING_TURF_WAR_S) * (0.8f + 0.4f * frand());
    b.foeNext = t + 2.5;
    b.foeHitAt = 0;
    b.foeLost = b.bossLost = b.hunterDmg = 0;
    b.aggro = true;
    b.roared = false;
    b.attack = -1;
    go(b, ST_IDLE, t);
    turf_claim(best->em, true);
    std::string them = mhw_name(best->kind);
    toast(l, "Turf war: %s vs %s", display_name(b.row).c_str(), them.empty() ? "a monster" : them.c_str());
    log("bosses: %s starts a turf war with %s", r.id, best->kind);
}

}  // namespace

bool is_boss(const uint8_t* em) { return em >= g_token && em < g_token + kMax; }

bool boss_hit_ex(uint8_t* em, float dmg, const Vec3& at, bool crit, bool weak, float stagger) {
    if (!is_boss(em)) return false;
    hit_boss(boss_index(em), dmg, at, crit, weak, stagger);
    return true;
}

float boss_time_scale(const uint8_t* em) { return (status_bits(em) & (1u << SS_SLOWED)) ? 0.7f : 1.0f; }

bool boss_hit(uint8_t* em, float dmg, const Vec3& at, bool crit, bool weak) {
    if (!is_boss(em)) return false;
    hit_boss(boss_index(em), dmg, at, crit, weak, 1.0f);
    return true;
}

int bosses_tick(SaveData& s, Live& l, double t, float dt) {
    const GameSnapshot& g = l.snap;
    g_snap = &g;
    g_difficulty = s.difficulty;
    if (g.view.valid) g_rh = g.view.proj[11] < 0;
    int ev = 0;
    mhw_bars(l, t);
    bool allowed = g.inGame && g.solo && (!g.inBase || g.zone == 504);
    if (!allowed || g.zone != g_zone) {
        for (Boss& b : g_boss)
            if (b.used) log("bosses: %s leaves (zone change)", B(b).id);
        for (Boss& b : g_boss) b = Boss();
        for (Proj& p : g_proj) p.live = false;
        if (g.zone != g_zone) {
            g_rollAt = t + 40 + frand() * 50;
            g_rolls = 0;
        }
        g_zone = g.zone;
    }
    if (!allowed) {
        std::lock_guard<std::mutex> lk(g_lock);
        g_draw.clear();
        g_hud.clear();
        g_creatureBars.clear();
        g_marks.clear();
        g_glows.clear();
        g_rings.clear();
        g_affs.clear();
        return 0;
    }
    float largest = 0;
    int rank = area_rank(g, &largest);
    if (rank != g_zoneRank) {
        log("bosses: zone %d reads as %s (largest MHW monster %.0f hp)", g.zone, kRankName[rank], largest);
        g_zoneRank = rank;
    }

    // Roaming: a roll per zone visit (fields only), and again a while after a boss is gone. Only bosses whose rank
    // fits the zone (the strongest stay out of low-rank zones).
    bool anyUsed = false;
    for (Boss& b : g_boss) anyUsed |= b.used;
    if (!g.inBase && g_rollAt > 0 && t >= g_rollAt && !anyUsed && g_rolls < 3) {
        g_rolls++;
        g_rollAt = t + 360 + frand() * 240;
        if (frand() < tune(TUNING_BOSS_ZONE_CHANCE)) {
            float total = 0;
            int pick = -1;
            for (int row = 0; row < kBossesCount; row++) {
                const Bosses& r = kBosses[row];
                if (!zone_listed(r, g.zone) || min_rank(row) > rank || r.spawn_weight <= 0) continue;
                total += r.spawn_weight;
                if (frand() * total <= r.spawn_weight) pick = row;
            }
            Vec3 p;
            bool placed = false;
            for (int k = 0; k < 10 && pick >= 0 && !placed; k++)  // 35-45 m out when the ground allows
                placed = place_near(g, frand() * 2 * kPi, 3500 + frand() * 1000, &p);
            for (int k = 0; k < 10 && pick >= 0 && !placed; k++)
                placed = place_near(g, frand() * 2 * kPi, 6000 + frand() * 5000, &p);
            if (placed) spawn(l, pick, p, frand() * 2 * kPi, t);
            else if (pick >= 0) g_rollAt = t + 20;  // try again from another spot
        }
    }
    // Debug / training: the boss key brings the next boss 35-45 m in front of the hunter (any rank, to try them all).
    bool key = key_down(keys().boss);
    if (key && !g_keyWas) log("boss key: view %s, zone %d, rank %d", g.view.valid ? "valid" : "not valid yet", g.zone, rank);
    if (key && !g_keyWas && kBossesCount > 0 && g.view.valid) {
        Vec3 f = sub(g.view.camTarget, g.view.camPos);
        float yaw = atan2f(f.x, f.z);
        Vec3 p;
        if (place_near(g, yaw, 3500 + frand() * 1000, &p) || place_near(g, yaw, 1500, &p) || place_near(g, yaw, 3000, &p) ||
            place_near(g, yaw, 800, &p)) {
            int row = g_nextDebugRow++ % kBossesCount;
            spawn(l, row, p, wrap(yaw + kPi), t);
            toast(l, "%s approaches (%s)", display_name(row).c_str(), kRankName[rank]);
        } else {
            toast(l, "No ground there for a Remnant boss");
        }
    }
    g_keyWas = key;
    // Remnant's evade: once MHW's stamina bar is known, a real dodge is one that spent stamina (keyboard or pad);
    // until then, the dodge key.
    bool space = key_down(VK_SPACE);
    if (stamina_known()) {
        double at = stamina_dodge_at();
        if (at > g_dodgeAt && t - at < 0.1) {
            g_dodgeAt = at;
            status_player_dodge(t);
        }
    } else {
        if (space) g_dodgeAt = t;
        if (space && !g_spaceWas) status_player_dodge(t);
    }
    g_spaceWas = space;

    mhw_weapons(l, t);
    ev |= projectiles(l, t, dt);

    for (int i = 0; i < kMax; i++) {
        Boss& b = g_boss[i];
        if (!b.used) continue;
        const Bosses& r = B(b);
        if (b.st == ST_LOADING) {
            bool failed = false;
            const Model* m = model_of(b.row, &failed);
            bool af = false;
            const Anim* idle = m ? anim_of(b.row, anim_path(b.row, r.anim_idle), &af) : nullptr;
            if (failed || af) {
                toast(l, "Remnant II's %s could not be read", display_name(b.row).c_str());
                log("bosses: %s: model or idle animation unreadable, removed", r.id);
                b = Boss();
                continue;
            }
            if (!idle) {
                if (t - b.stT0 > 120) { log("bosses: %s: still loading after 120 s, removed", r.id); b = Boss(); }
                continue;
            }
            make_frame(b);
            float y;
            if (ground(b.pos, &y)) b.pos.y = y;
            if (b.entrance) {  // the entrance: it turns to the hunter and roars before it engages
                b.entrance = false;
                b.yaw = yaw_to(b.pos, g.pos);
                go(b, ST_ROAR, t);
            } else {
                go(b, ST_IDLE, t);
            }
            b.goalUntil = t + 2;
            log("bosses: %s is out (%.0f hp, %.1f x %.1f x %.1f m)", r.id, b.maxHp, (b.fr.aMax - b.fr.aMin) / 100,
                (b.fr.cMax - b.fr.cMin) / 100, (b.fr.zMax - b.fr.zMin) / 100);
        }
        // Animation clocks.
        float bdt = dt * boss_time_scale(&g_token[i]);  // SLOWED
        b.at += bdt;
        b.pt += bdt;
        b.fade = std::max(0.0f, b.fade - dt / 0.22f);
        if (b.st == ST_DEAD && b.foe) end_turf(b, find_mon(g, b.foe), "the boss fell", t);
        if (b.st == ST_DEAD) {
            if (!b.looted) {
                b.looted = true;
                ev |= BOSS_EV_KILL;
                loot(s, l, b);
                g_rollAt = t + 360 + frand() * 240;
            }
            float hold = has(r.anim_death) ? 14.0f : 1.0f;
            if (t - b.stT0 > hold) b.sink += dt * std::max(120.0f, height_of(b) * 0.4f);
            if (b.sink > height_of(b) + 300) b = Boss();
            continue;
        }
        boss_turf(b, i, l, t, bdt);
        ev |= think(b, l, t, bdt);
        float y;
        if (ground(b.pos, &y) && fabsf(y - b.pos.y) < 400) b.pos.y += (y - b.pos.y) * std::min(1.0f, dt * 12);
    }

    // The bosses as monsters for the rest of the mod (guns, skills, effects, targeting).
    for (int i = 0; i < kMax; i++) {
        Boss& b = g_boss[i];
        if (!b.used || b.st == ST_LOADING || b.st == ST_DEAD) continue;
        const Frame& fr = b.fr;
        Vec3 f = fwd_of(b.yaw), r = right_of(b.yaw), up = {0, 1, 0};
        Monster m{};
        m.em = &g_token[i];
        m.health = nullptr;
        m.hp = b.hp;
        m.maxHp = b.maxHp;
        m.large = true;
        Vec3 axes[3] = {r, up, f};
        for (int k = 0; k < 3; k++) {
            m.rot[k][0] = axes[k].x; m.rot[k][1] = axes[k].y; m.rot[k][2] = axes[k].z; m.rot[k][3] = 0;
        }
        m.center = add(b.pos, add(mul(f, (fr.aMin + fr.aMax) / 2), add(mul(up, (fr.zMin + fr.zMax) / 2), mul(r, (fr.cMin + fr.cMax) / 2))));
        m.half = {(fr.cMax - fr.cMin) * 0.45f, (fr.zMax - fr.zMin) * 0.45f, (fr.aMax - fr.aMin) * 0.45f};
        m.obbOk = true;
        snprintf(m.kind, sizeof(m.kind), "r2boss");
        l.snap.monsters.push_back(m);
    }

    // Hand over what to draw.
    std::lock_guard<std::mutex> lk(g_lock);
    g_draw.clear();
    g_hud.clear();
    g_creatureBars.clear();
    g_marks.clear();
    g_glows.clear();
    g_rings.clear();
    for (const Proj& p : g_proj)
        if (p.live) g_glows.push_back({p.p, p.radius, p.color});
    g_affs.clear();
    for (const Monster& m : g.monsters)
        if (uint32_t bits = status_bits(m.em)) g_affs.push_back({m.center, std::max(m.half.x, std::max(m.half.y, m.half.z)), bits});
    g_meBits = status_bits(nullptr);
    for (int i = 0; i < kMax; i++) {
        Boss& b = g_boss[i];
        if (!b.used || b.st == ST_LOADING) continue;
        const Anim* cur = anim_of(b.row, b.anim);
        const Anim* pre = b.prev.empty() ? nullptr : anim_of(b.row, b.prev);
        DrawBoss d;
        d.m = model_of(b.row);
        d.row = b.row;
        d.a = b.anim;
        d.at = cur ? (b.loop ? b.at : std::min(b.at, std::max(0.0f, cur->length() - 0.001f))) : 0;
        d.b = b.fade > 0 ? b.prev : std::string();
        d.bt = pre ? (b.ploop ? b.pt : std::min(b.pt, std::max(0.0f, pre->length() - 0.001f))) : 0;
        d.blend = b.fade;
        world_matrix(b, d.world);
        g_draw.push_back(d);
        if (d.m) {  // the hitbox follows the pose the renderer will draw
            std::vector<float> pal;
            pose(*d.m, cur, d.at, true, pre, d.bt, pre ? d.blend : 0.0f, &pal);
            place_hitbox(b, pal, d.world);
        }
        // area attacks winding up: their reach on the ground
        if (b.st == ST_ATTACK && b.attack >= 0 && kBoss_attacks[b.attack].kind == BOSS_ATTACKS_KIND_AOE) {
            const Boss_attacks& a = kBoss_attacks[b.attack];
            float first = 1, firstEnd = 1, len = std::max(0.1f, anim_len(b, b.anim));
            window(a.hits, 0, &first, &firstEnd);
            float frac = b.at / len;
            if (frac < firstEnd)
                g_rings.push_back({b.pos, a.reach_m * 100.0f * B(b).scale, std::min(1.0f, frac / std::max(0.05f, first)),
                                   parse_color(B(b).fx_color)});
        }
        float dist = hlen(sub(g.pos, b.pos));
        if (b.st != ST_DEAD) g_marks.push_back({b.pos, b.row, b.aggro});
        if (B(b).tier == BOSSES_TIER_BOSS) {
            g_hud.push_back({b.row, b.hp, b.maxHp, b.aggro || (b.st == ST_DEAD && dist < 8000), b.st == ST_DEAD, b.stT0,
                             b.rank, b.st == ST_STUN, status_bits(&g_token[i])});
        } else if (b.st != ST_DEAD && dist < 6000) {
            std::string nm = display_name(b.row);
            g_creatureBars.push_back({&g_token[i], B(b).tier == BOSSES_TIER_ELITE ? 1 : 0,
                                      B(b).tier == BOSSES_TIER_ELITE ? nm : std::string(), b.hp, b.maxHp,
                                      {b.pos.x, b.pos.y + b.fr.zMax + 60, b.pos.z}, status_bits(&g_token[i])});
        }
    }
    return ev;
}

void bosses_render(void* dev, void* ctx, void* rtv, int w, int h) {
    std::vector<DrawBoss> list;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        list = g_draw;
    }
    GameView v;
    if (!camera_view(&v)) return;
    std::vector<DrawItem> items;
    items.reserve(list.size() + 1);
    static std::set<std::string> keep;  // stable storage for the texture asset strings
    for (const DrawBoss& d : list) {
        if (!d.m) continue;
        const Bosses& r = kBosses[d.row];
        DrawItem it;
        it.model = d.m;
        memcpy(it.world, d.world, 64);
        const Anim* a = anim_get(d.a.c_str(), r.skeleton, d.m);
        const Anim* b = d.b.empty() ? nullptr : anim_get(d.b.c_str(), r.skeleton, d.m);
        // A one-shot animation's time is held at its end by the game thread; looping ones wrap here.
        pose(*d.m, a, d.at, true, b, d.bt, b ? d.blend : 0.0f, &it.palette);
        // Textures per material slot: "slot=asset" entries matched by slot name; "*=asset" for every other slot.
        size_t slots = d.m->materials.size();
        for (const ModelSection& s : d.m->sections) slots = std::max(slots, (size_t)std::max<int16_t>(0, s.material) + 1);
        it.diffuse.assign(slots, nullptr);
        const char* fallback = nullptr;
        std::string all = r.textures;
        size_t i = 0;
        while (i < all.size()) {
            size_t j = all.find(',', i);
            if (j == std::string::npos) j = all.size();
            std::string e = all.substr(i, j - i);
            size_t eq = e.find('=');
            if (eq != std::string::npos) {
                std::string slot = e.substr(0, eq), asset = e.substr(eq + 1);
                const char* kept = keep.insert(asset).first->c_str();
                if (slot == "*") fallback = kept;
                for (size_t k = 0; k < d.m->materials.size() && slot != "*"; k++)
                    if (!it.diffuse[k] && d.m->materials[k].find(slot) != std::string::npos) it.diffuse[k] = kept;
            }
            i = j + 1;
        }
        for (auto& tx : it.diffuse)
            if (!tx) tx = fallback;
        items.push_back(std::move(it));
    }
    gunmodel_append(&items);
    if (items.empty()) return;
    render3d_frame(dev, ctx, rtv, w, h, v.view, v.proj, items);
}

// Remnant II's own status icons, and a colour for each status's visuals.
struct StatusArt { const char* icon; ImU32 color; };
const StatusArt kArt[SS_COUNT] = {
    {nullptr, 0},
    {"Remnant2/Content/UI2/UI_Icons/UI_Icons_Status/T_UI_Icon_Status_Bleed", IM_COL32(210, 30, 30, 255)},
    {"Remnant2/Content/UI2/UI_Icons/UI_Icons_Status/T_UI_Icon_Status_Fire", IM_COL32(255, 140, 30, 255)},
    {"Remnant2/Content/UI2/UI_Icons/UI_Icons_Status/T_UI_Icon_Status_Corrosive", IM_COL32(130, 210, 60, 255)},
    {"Remnant2/Content/UI2/UI_Icons/UI_Icons_Status/T_UI_Icon_Status_Shock", IM_COL32(90, 170, 255, 255)},
    {"Remnant2/Content/UI2/UI_Icons/UI_Icons_Status/T_UI_Icon_Status_Exposed", IM_COL32(240, 220, 90, 255)},
    {"Remnant2/Content/UI2/UI_Icons/UI_Icons_Status/T_UI_Icon_Status_Slow", IM_COL32(150, 150, 230, 255)},
    {"Remnant2/Content/UI2/UI_Icons/UI_Icons_Status/T_UI_Icon_Status_Omen_Bad", IM_COL32(255, 80, 80, 255)},
};

// A row of status icons from x (to the right), centred on y; returns the row's width.
float status_icons(ImDrawList* dl, uint32_t bits, float x, float y, float size) {
    float x0 = x;
    for (int st = 1; st < SS_COUNT; st++) {
        if (!(bits & (1u << st))) continue;
        int w = 0, h = 0;
        void* tex = texture_for(kArt[st].icon, 64, &w, &h);
        ImVec2 a = {x, y - size / 2}, b = {x + size, y + size / 2};
        dl->AddRectFilled(a, b, IM_COL32(10, 8, 8, 170), size * 0.2f);
        if (tex) dl->AddImage((ImTextureID)(uintptr_t)tex, a, b, {0, 0}, {1, 1}, kArt[st].color);
        else dl->AddCircleFilled({x + size / 2, y}, size * 0.32f, kArt[st].color, 12);
        x += size + 2;
    }
    return x - x0;
}

void bosses_hud() {
    std::vector<HudBoss> hud;
    std::vector<Bar> bars;
    std::vector<Mark> marks;
    std::vector<Aff> affs;
    uint32_t meBits = 0;
    std::vector<Glow> glows;
    std::vector<Ring> rings;
    Docked focus;
    bool hasFocus;
    Number nums[32];
    double hurtAt;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        hud = g_hud;
        bars = g_bars;
        bars.insert(bars.end(), g_creatureBars.begin(), g_creatureBars.end());
        focus = g_focus;
        hasFocus = g_hasFocus;
        marks = g_marks;
        affs = g_affs;
        meBits = g_meBits;
        glows = g_glows;
        rings = g_rings;
        memcpy(nums, g_numbers, sizeof(nums));
        hurtAt = g_hurtAt;
    }
    gunmodel_hud();
    double t = now_s();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float sc = vp->Size.y / 1080.0f;
    // Hit flash: a red edge, as Remnant II shows damage taken.
    if (t - hurtAt < 0.45) {
        int a = (int)(110 * (1.0 - (t - hurtAt) / 0.45));
        ImU32 red = IM_COL32(170, 10, 0, a), none = IM_COL32(170, 10, 0, 0);
        float e = 140 * sc;
        ImVec2 p0 = vp->Pos, p1 = {vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y};
        dl->AddRectFilledMultiColor(p0, {p1.x, p0.y + e}, red, red, none, none);
        dl->AddRectFilledMultiColor({p0.x, p1.y - e}, p1, none, none, red, red);
        dl->AddRectFilledMultiColor(p0, {p0.x + e, p1.y}, red, none, none, red);
        dl->AddRectFilledMultiColor({p1.x - e, p0.y}, p1, none, red, red, none);
    }
    GameView v;
    bool viewOk = camera_view(&v);
    // Area attacks winding up: a ring on the ground at their reach. Projectiles: a glow in the boss's colour.
    if (viewOk) {
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        for (const Ring& r : rings) {
            ImVec2 pts[40];
            int n = 0;
            for (int k = 0; k < 40; k++) {
                float a = k * 2 * kPi / 40;
                Vec3 p = {r.c.x + r.radius * cosf(a), r.c.y + 30, r.c.z + r.radius * sinf(a)};
                if (project(v, p, vp->Pos, vp->Size, &pts[n])) n++;
            }
            ImU32 c = (r.color & 0x00FFFFFF) | ((ImU32)(90 + 140 * r.k) << 24);
            if (n > 2) bg->AddPolyline(pts, n, c, n == 40 ? ImDrawFlags_Closed : 0, (2.0f + 3.0f * r.k) * sc);
        }
        for (const Glow& gl : glows) {
            ImVec2 p;
            if (!project(v, gl.p, vp->Pos, vp->Size, &p)) continue;
            float d = std::max(100.0f, len3(sub(gl.p, v.camPos)));
            float rad = std::max(4.0f, std::min(70.0f, gl.radius * 900.0f / d)) * sc;
            ImU32 core = IM_COL32(255, 255, 255, 230), mid = (gl.color & 0x00FFFFFF) | (200u << 24),
                  halo = (gl.color & 0x00FFFFFF) | (60u << 24);
            bg->AddCircleFilled(p, rad * 2.2f, halo, 24);
            bg->AddCircleFilled(p, rad, mid, 20);
            bg->AddCircleFilled(p, rad * 0.45f, core, 12);
        }
    }
    // Tracker: where the Remnant creatures in this zone are, relative to the camera (until MHW's own minimap can
    // carry them). A strip at the top of the screen, half a turn wide.
    float y = vp->Pos.y + 38 * sc;
    if (viewOk && !marks.empty()) {
        float w = std::min(460.0f * sc, vp->Size.x * 0.34f), hgt = 20 * sc;
        float cx = vp->Pos.x + vp->Size.x / 2, y0 = vp->Pos.y + 10 * sc;
        ImU32 clear = IM_COL32(10, 8, 8, 0), dark = IM_COL32(10, 8, 8, 170);
        dl->AddRectFilledMultiColor({cx - w / 2, y0}, {cx, y0 + hgt}, clear, dark, dark, clear);
        dl->AddRectFilledMultiColor({cx, y0}, {cx + w / 2, y0 + hgt}, dark, clear, clear, dark);
        dl->AddLine({cx - w / 2, y0 + hgt}, {cx + w / 2, y0 + hgt}, IM_COL32(176, 146, 84, 110), 1.0f * sc);
        bool rh = v.proj[11] < 0;
        auto bearing = [&](Vec3 to) {  // radians, + = right of the camera's view
            Vec3 d = sub(to, v.camPos);
            float vx = d.x * v.view[0] + d.y * v.view[4] + d.z * v.view[8];
            float vz = d.x * v.view[2] + d.y * v.view[6] + d.z * v.view[10];
            return atan2f(vx, rh ? -vz : vz);
        };
        for (int k = -6; k <= 6; k++) {  // ticks every 15 degrees
            float x = cx + k * (w / 2) / 6;
            dl->AddLine({x, y0 + hgt - (k % 2 ? 4 : 8) * sc}, {x, y0 + hgt}, IM_COL32(220, 210, 190, (int)(150 - 18 * abs(k))), 1.0f * sc);
        }
        for (const Mark& m : marks) {
            float a = bearing(m.pos);
            bool edge = fabsf(a) > kPi / 2;
            float x = cx + std::max(-1.0f, std::min(1.0f, a / (kPi / 2))) * (w / 2 - 8 * sc);
            float my = y0 + hgt / 2, r = 6 * sc;
            ImU32 col = m.aggro ? IM_COL32(226, 48, 32, 255) : IM_COL32(214, 180, 96, 240);
            if (edge) {  // behind: an arrow at the side it is on
                float sgn = a > 0 ? 1.0f : -1.0f;
                dl->AddTriangleFilled({x + sgn * r, my}, {x - sgn * r * 0.6f, my - r}, {x - sgn * r * 0.6f, my + r}, col);
            } else {
                dl->AddQuadFilled({x, my - r}, {x + r, my}, {x, my + r}, {x - r, my}, col);
                dl->AddQuad({x, my - r}, {x + r, my}, {x, my + r}, {x - r, my}, IM_COL32(0, 0, 0, 200), 1.0f * sc);
            }
            char dist[16];
            snprintf(dist, sizeof(dist), "%.0f m", hlen(sub(m.pos, v.camPos)) / 100.0f);
            ImFont* f = ui_font(0);
            float fs = 13 * sc;
            ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, dist);
            dl->AddText(f, fs, {x - ts.x / 2 + 1, y0 + hgt + 2 * sc + 1}, IM_COL32(0, 0, 0, 200), dist);
            dl->AddText(f, fs, {x - ts.x / 2, y0 + hgt + 2 * sc}, IM_COL32(236, 228, 214, 230), dist);
        }
        y = y0 + hgt + 22 * sc;
    }
    // Boss bars, top centre (Remnant II's place for them), framed in MHW's gold.
    int shown = 0;
    for (size_t i = 0; i < hud.size() && shown < 2; i++) {
        const HudBoss& h = hud[i];
        if (!h.show) continue;
        if (h.dead && t - h.deadAt > 6) continue;
        shown++;
        float w = std::min(760.0f * sc, vp->Size.x * 0.5f), bh = 11 * sc;
        float x0 = vp->Pos.x + (vp->Size.x - w) / 2;
        std::string name = display_name(h.row);
        for (char& c : name) c = (char)toupper((unsigned char)c);
        ImFont* title = ui_font(2);
        float fs = 26 * sc;
        ImVec2 ts = title->CalcTextSizeA(fs, FLT_MAX, 0, name.c_str());
        dl->AddText(title, fs, {x0 + 1, y + 1}, IM_COL32(0, 0, 0, 200), name.c_str());
        dl->AddText(title, fs, {x0, y}, IM_COL32(236, 228, 214, 255), name.c_str());
        if (!h.dead) {  // the zone's rank, MHW style
            const char* rk = kRankName[std::max(0, std::min(2, h.rank))];
            ImVec2 rs = ui_font(1)->CalcTextSizeA(14 * sc, FLT_MAX, 0, rk);
            dl->AddText(ui_font(1), 14 * sc, {x0 + w - rs.x, y + fs - rs.y - 2}, IM_COL32(214, 180, 96, 230), rk);
        }
        if (h.dead) {
            const char* sl = "SLAIN";
            ImVec2 ss = ui_font(1)->CalcTextSizeA(16 * sc, FLT_MAX, 0, sl);
            dl->AddText(ui_font(1), 16 * sc, {x0 + w - ss.x, y + fs - ss.y - 2}, IM_COL32(214, 180, 96, 255), sl);
        }
        float by = y + ts.y + 4 * sc;
        float frac = h.maxHp > 0 ? std::max(0.0f, h.hp / h.maxHp) : 0;
        float& trail = g_trail[i % kMax];
        if (trail < frac) trail = frac;
        trail = std::max(frac, trail - ImGui::GetIO().DeltaTime * 0.25f);
        dl->AddRectFilled({x0 - 2, by - 2}, {x0 + w + 2, by + bh + 2}, IM_COL32(8, 6, 6, 210));
        dl->AddRectFilled({x0, by}, {x0 + w * trail, by + bh}, IM_COL32(236, 214, 190, 220));
        dl->AddRectFilledMultiColor({x0, by}, {x0 + w * frac, by + bh}, IM_COL32(196, 34, 24, 255), IM_COL32(196, 34, 24, 255),
                                    IM_COL32(120, 14, 10, 255), IM_COL32(120, 14, 10, 255));
        dl->AddRect({x0 - 2, by - 2}, {x0 + w + 2, by + bh + 2}, IM_COL32(176, 146, 84, 200), 0, 0, 1.2f * sc);
        for (float cx : {x0 - 6 * sc, x0 + w + 6 * sc}) {  // MHW-style end studs
            ImVec2 c = {cx, by + bh / 2};
            float r = 5 * sc;
            dl->AddQuadFilled({c.x, c.y - r}, {c.x + r, c.y}, {c.x, c.y + r}, {c.x - r, c.y}, IM_COL32(196, 160, 90, 230));
        }
        if (h.status && !h.dead) status_icons(dl, h.status, x0, by + bh + 13 * sc, 18 * sc);
        y = by + bh + (h.status ? 30 : 22) * sc;
    }
    if (g_barTrail.size() > 128) g_barTrail.clear();
    auto trail_of = [&](const void* key, float frac) {
        auto it = g_barTrail.find(key);
        float tr = it == g_barTrail.end() ? frac : it->second;
        if (tr < frac) tr = frac;
        tr = std::max(frac, tr - ImGui::GetIO().DeltaTime * 0.3f);
        g_barTrail[key] = tr;
        return tr;
    };
    auto bar = [&](float x0, float by, float w, float bh, float frac, float tr, ImU32 frame, float thick) {
        dl->AddRectFilled({x0 - 1.5f * sc, by - 1.5f * sc}, {x0 + w + 1.5f * sc, by + bh + 1.5f * sc}, IM_COL32(8, 6, 6, 200));
        dl->AddRectFilled({x0, by}, {x0 + w * tr, by + bh}, IM_COL32(236, 214, 190, 210));
        dl->AddRectFilledMultiColor({x0, by}, {x0 + w * frac, by + bh}, IM_COL32(196, 34, 24, 255), IM_COL32(196, 34, 24, 255),
                                    IM_COL32(120, 14, 10, 255), IM_COL32(120, 14, 10, 255));
        dl->AddRect({x0 - 1.5f * sc, by - 1.5f * sc}, {x0 + w + 1.5f * sc, by + bh + 1.5f * sc}, frame, 0, 0, thick);
    };
    // The MHW monster being fought: docked under the boss bars, a size smaller.
    if (hasFocus && focus.maxHp > 0) {
        float w = std::min(520.0f * sc, vp->Size.x * 0.36f), bh = 7 * sc;
        float x0 = vp->Pos.x + (vp->Size.x - w) / 2;
        float frac = std::max(0.0f, std::min(1.0f, focus.hp / focus.maxHp));
        if (!focus.name.empty()) {
            std::string name = focus.name;
            for (char& c : name) c = (char)toupper((unsigned char)c);
            ImFont* f = ui_font(1);
            float fs = 18 * sc;
            dl->AddText(f, fs, {x0 + 1, y + 1}, IM_COL32(0, 0, 0, 200), name.c_str());
            dl->AddText(f, fs, {x0, y}, IM_COL32(230, 222, 206, 255), name.c_str());
            y += fs + 3 * sc;
        }
        bar(x0, y, w, bh, frac, trail_of(focus.key, frac), IM_COL32(176, 146, 84, 170), 1.0f * sc);
        if (focus.status) {
            status_icons(dl, focus.status, x0, y + bh + 12 * sc, 16 * sc);
            y += 18 * sc;
        }
        y += bh + 18 * sc;
    }
    if (!viewOk) return;
    // Bars over the other creatures: large monsters (and Remnant elites) wider with a name, small ones thin.
    for (const Bar& b : bars) {
        if (b.maxHp <= 0) continue;
        ImVec2 p;
        if (!project(v, b.anchor, vp->Pos, vp->Size, &p)) continue;
        if (p.x < vp->Pos.x - 50 || p.x > vp->Pos.x + vp->Size.x + 50 || p.y < vp->Pos.y || p.y > vp->Pos.y + vp->Size.y) continue;
        float frac = std::max(0.0f, std::min(1.0f, b.hp / b.maxHp));
        float w = (b.tier == 1 ? 150.0f : 70.0f) * sc, bh = (b.tier == 1 ? 6.0f : 4.0f) * sc;
        float x0 = p.x - w / 2;
        if (b.tier == 1 && !b.name.empty()) {
            ImFont* f = ui_font(0);
            float fs = 15 * sc;
            ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, b.name.c_str());
            dl->AddText(f, fs, {p.x - ts.x / 2 + 1, p.y - bh - ts.y - 2 * sc + 1}, IM_COL32(0, 0, 0, 190), b.name.c_str());
            dl->AddText(f, fs, {p.x - ts.x / 2, p.y - bh - ts.y - 2 * sc}, IM_COL32(236, 228, 214, 240), b.name.c_str());
        }
        if (b.status) status_icons(dl, b.status, x0, p.y + 9 * sc, (b.tier == 1 ? 14 : 11) * sc);
        bar(x0, p.y - bh, w, bh, frac, trail_of(b.key, frac), b.tier == 1 ? IM_COL32(176, 146, 84, 150) : IM_COL32(0, 0, 0, 160),
            1.0f * sc);
    }
    // Statuses on monsters: embers, blood, acid, sparks rising around them in the status's colour.
    {
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        for (const Aff& a : affs) {
            ImVec2 c;
            if (!project(v, a.c, vp->Pos, vp->Size, &c)) continue;
            float d = std::max(300.0f, len3(sub(a.c, v.camPos)));
            float rad = std::min(260.0f, a.r * 1000.0f / d) * sc;
            int k = 0;
            for (int st = SS_BLEEDING; st <= SS_EXPOSED; st++) {
                if (!(a.bits & (1u << st))) continue;
                for (int n = 0; n < 7; n++, k++) {
                    float ph = fmodf((float)t * (st == SS_OVERLOADED ? 2.2f : 0.9f) + k * 0.137f, 1.0f);
                    float ang = k * 2.39996f;
                    float rr = rad * (0.35f + 0.6f * fmodf(k * 0.618f, 1.0f));
                    ImVec2 q = {c.x + cosf(ang) * rr, c.y + sinf(ang) * rr * 0.5f + (st == SS_BLEEDING ? ph : -ph) * rad * 0.6f};
                    int al = (int)(220 * (1.0f - ph));
                    ImU32 col = (kArt[st].color & 0x00FFFFFF) | ((ImU32)al << 24);
                    float sz = (st == SS_OVERLOADED ? 2.0f : 3.0f + 3.0f * (1.0f - ph)) * sc;
                    if (st == SS_OVERLOADED)
                        bg->AddLine(q, {q.x + 8 * sc * cosf(ang * 3), q.y + 8 * sc * sinf(ang * 3)}, col, 2.0f * sc);
                    else
                        bg->AddCircleFilled(q, sz, col, 8);
                }
            }
        }
    }
    // The hunter's own statuses: Remnant's icons under MHW's health bar, and the screen edge in their colour.
    if (meBits) {
        float x = vp->Pos.x + 34 * sc, y0 = vp->Pos.y + 150 * sc;
        status_icons(dl, meBits, x, y0, 30 * sc);
        int st = SS_BLEEDING;
        while (st < SS_COUNT && !(meBits & (1u << st))) st++;
        if (st <= SS_OVERLOADED) {
            int al = (int)(55 + 35 * sinf((float)t * 5.0f));
            ImU32 col = (kArt[st].color & 0x00FFFFFF) | ((ImU32)al << 24), none = kArt[st].color & 0x00FFFFFF;
            float e = 90 * sc;
            ImVec2 p0 = vp->Pos, p1 = {vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y};
            ImDrawList* bg = ImGui::GetBackgroundDrawList();
            bg->AddRectFilledMultiColor({p0.x, p1.y - e}, p1, none, none, col, col);
            bg->AddRectFilledMultiColor(p0, {p0.x + e, p1.y}, col, none, none, col);
            bg->AddRectFilledMultiColor({p1.x - e, p0.y}, p1, none, col, col, none);
        }
    }
    // Damage numbers at the hit.
    for (const Number& n : nums) {
        double age = t - n.t;
        if (n.amount <= 0 || age < 0 || age > 0.95) continue;
        ImVec2 p;
        if (!project(v, n.at, vp->Pos, vp->Size, &p)) continue;
        char txt[16];
        snprintf(txt, sizeof(txt), "%d", n.amount);
        float fs = (n.crit ? 30 : 24) * sc;
        int a = (int)(255 * std::min(1.0, (0.95 - age) / 0.3));
        ImU32 col = n.weak ? IM_COL32(255, 128, 64, a) : n.crit ? IM_COL32(255, 214, 90, a) : IM_COL32(245, 245, 240, a);
        ImFont* f = ui_font(1);
        ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, txt);
        ImVec2 at = {p.x - ts.x / 2, p.y - ts.y / 2 - (float)age * 60 * sc};
        dl->AddText(f, fs, {at.x + 1.5f, at.y + 1.5f}, IM_COL32(0, 0, 0, a * 3 / 4), txt);
        dl->AddText(f, fs, at, col, txt);
    }
}


}  // namespace rh
