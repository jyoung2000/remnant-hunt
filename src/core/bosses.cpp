// Remnant x MHW mashup core: Remnant II bosses roaming MHW's field zones. GPL-3.0-or-later (see LICENSE).
//
// Each boss is a row of sheets/bosses.json with its attacks in sheets/boss_attacks.json. The mod moves it on MHW's
// ground (world_ray), runs a small Remnant-style AI (roam, roar, chase, attack, flinch, stagger, die), deals damage
// to the hunter in its attacks' hit windows (taken from the animations' limb speeds), and takes damage from the
// Remnant guns and skills (as a pseudo monster) and from MHW's melee weapons (attack input + reach, approximate).
// Drawing is render3d.cpp with Remnant II's own model and textures; the bar and numbers are drawn with ImGui.
#include "bosses.h"

#include <algorithm>
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

enum St { ST_LOADING, ST_IDLE, ST_WALK, ST_ROAR, ST_CHASE, ST_TURN, ST_ATTACK, ST_FLINCH, ST_STUN, ST_DEAD };

struct Boss {
    bool used = false;
    int row = -1;
    St st = ST_LOADING;
    Vec3 pos{}, home{};
    float yaw = 0;
    float hp = 0, maxHp = 0, rankK = 1;
    double stT0 = 0;
    std::string anim, prev;  // full asset paths
    float at = 0, pt = 0, fade = 0;
    bool loop = true, ploop = true;
    float root[3] = {};
    int attack = -1;     // boss_attacks row
    int windowsDone = 0; // bits
    double nextAttack = 0;
    double cd[64] = {};  // per boss_attacks row
    Vec3 goal{};
    double goalUntil = 0;
    float flinchAcc = 0;
    bool stunned = false, aggro = false, roared = false, looted = false;
    double lastSeen = 0, lastStep = 0, lastWallCheck = 0;
    float turnFrom = 0, turnBy = 0;
    float sink = 0;
};

Boss g_boss[kMax];
uint8_t g_token[kMax];  // the bosses' pseudo uEnemy pointers: never dereferenced, only compared
bool g_rh = false;       // MHW's world is right-handed (from its projection)
int g_zone = -1;
double g_rollAt = 0;
int g_rollTries = 0;
int g_nextDebugRow = 0;
bool g_keyWas = false, g_lmbWas = false, g_rmbWas = false;
double g_swingAt = 0, g_dodgeAt = -10;
const GameSnapshot* g_snap = nullptr;

// Shared with the render thread.
struct DrawBoss {
    const Model* m;
    int row;
    std::string a, b;
    float at, bt, blend;
    float world[16];
};
struct Number { Vec3 at; int amount; bool crit, weak; double t; };
struct HudBoss { int row; float hp, maxHp; bool show, dead; double deadAt; };
// Remnant-style bars for everything else: MHW's monsters, and Remnant creatures below boss tier.
// tier 0 = small creature (thin, no name), 1 = large monster / elite (name over it).
struct Bar { const void* key; int tier; std::string name; float hp, maxHp; Vec3 anchor; };
struct Docked { const void* key; std::string name; float hp, maxHp; };
struct Mark { Vec3 pos; int row; bool aggro; };  // Remnant creatures for the tracker
std::mutex g_lock;
std::vector<DrawBoss> g_draw;
std::vector<HudBoss> g_hud;
std::vector<Bar> g_bars, g_creatureBars;
std::vector<Mark> g_marks;
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
Vec3 fwd_of(float yaw) { return {sinf(yaw), 0, cosf(yaw)}; }
Vec3 right_of(float yaw) { return g_rh ? Vec3{-cosf(yaw), 0, sinf(yaw)} : Vec3{cosf(yaw), 0, -sinf(yaw)}; }
float yaw_to(Vec3 from, Vec3 to) { return atan2f(to.x - from.x, to.z - from.z); }

const Bosses& B(const Boss& b) { return kBosses[b.row]; }
std::string anim_path(int row, const char* name) { return std::string(kBosses[row].anim_dir) + name + ".uasset"; }
const Model* model_of(int row, bool* failed = nullptr) { return model_get(kBosses[row].mesh, failed); }
const Anim* anim_of(int row, const std::string& path, bool* failed = nullptr) {
    const Model* m = model_of(row);
    return m ? anim_get(path.c_str(), kBosses[row].skeleton, m, failed) : nullptr;
}
float anim_len(const Boss& b, const std::string& path) {
    const Anim* a = anim_of(b.row, path);
    return a ? a->length() : 0.0f;
}

// The model's extent (Remnant II units, cm): x across (right is -x), y along (head at +y), z up.
void extent(const Boss& b, float mn[3], float mx[3]) {
    const Model* m = model_of(b.row);
    for (int k = 0; k < 3; k++) {
        mn[k] = m ? m->boundsMin[k] * B(b).scale : -200;
        mx[k] = m ? m->boundsMax[k] * B(b).scale : 200;
    }
}
Vec3 head_point(const Boss& b) {
    float mn[3], mx[3];
    extent(b, mn, mx);
    return add(b.pos, mul(fwd_of(b.yaw), mx[1] * 0.8f));
}

std::vector<int> attacks_of(int row) {
    std::vector<int> v;
    for (int i = 0; i < kBoss_attacksCount; i++)
        if (kBoss_attacks[i].boss == row) v.push_back(i);
    return v;
}

void play(const char* event, Vec3 at, float loud) {
    if (!event || !g_snap) return;
    float d = hlen(sub(at, g_snap->pos));
    float v = std::max(0.0f, std::min(1.0f, loud * (1.25f - d / 6000.0f)));
    if (v > 0.03f) audio_play(event, v);
}

void set_anim(Boss& b, const std::string& path, bool loop) {
    if (path == b.anim) { b.loop = loop; return; }
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
        case ST_WALK: set_anim(b, anim_path(b.row, r.anim_walk), true); break;
        case ST_ROAR: set_anim(b, anim_path(b.row, r.anim_roar), false); play(r.snd_roar, b.pos, 1.2f); break;
        case ST_FLINCH: set_anim(b, anim_path(b.row, r.anim_flinch), false); play(r.snd_pain, b.pos, 1.0f); break;
        case ST_STUN: set_anim(b, anim_path(b.row, r.anim_stun), false); play(r.snd_pain, b.pos, 1.1f); break;
        case ST_DEAD: set_anim(b, anim_path(b.row, r.anim_death), false); play(r.snd_death, b.pos, 1.2f); break;
        default: break;
    }
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
        float mn[3], mx[3];
        extent(b, mn, mx);
        Vec3 from = {b.pos.x, b.pos.y + 300, b.pos.z};
        Vec3 ahead = add(from, mul(dir, std::max(250.0f, mx[1] * 0.6f)));
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
    float largest = 0;
    for (const Monster& m : l.snap.monsters)
        if (m.large && !is_boss(m.em)) largest = std::max(largest, m.maxHp);
    const Bosses& r = kBosses[row];
    b.maxHp = b.hp = std::max(r.hp_base, r.hp_vs_zone * largest);
    b.rankK = std::max(1.0f, std::min(tune(TUNING_RANK_SCALE_MAX), b.maxHp / std::max(1.0f, tune(TUNING_RANK_SCALE_HP))));
    b.st = ST_LOADING;
    b.stT0 = t;
    // Ask r2read for everything now, so the first attack does not wait.
    model_of(row);
    for (const char* a : {r.anim_idle, r.anim_walk, r.anim_run, r.anim_turn_l, r.anim_turn_r, r.anim_roar, r.anim_flinch,
                          r.anim_stun, r.anim_death})
        anim_of(row, anim_path(row, a));
    for (int k : attacks_of(row)) {
        anim_of(row, anim_path(row, kBoss_attacks[k].anim));
        if (kBoss_attacks[k].snd) audio_prefetch(kBoss_attacks[k].snd);
    }
    for (const char* s : {r.snd_roar, r.snd_pain, r.snd_death, r.snd_step}) audio_prefetch(s);
    r2_text(r.name);  // resolved now, so the bar never waits for it
    log("bosses: %s spawning at (%.0f, %.0f, %.0f), %.0f hp", r.id, at.x, at.y, at.z, b.maxHp);
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
        {"fragments", CAT_FRAGMENT}, {"consumables", CAT_CONSUMABLE}};
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
    const std::string& name = r2_text(B(b).name);
    if (pool.empty()) {
        toast(l, "%s defeated", name.empty() ? B(b).id : name.c_str());
        return;
    }
    auto pick = pool[rand() % pool.size()];
    s.owned[pick.first][pick.second] = 1;
    l.dirty = true;
    ItemView v = item(pick.first, pick.second);
    const std::string& got = v.name ? r2_text(*v.name) : std::string();
    toast(l, "%s defeated: %s added to your Remnant gear", name.empty() ? B(b).id : name.c_str(),
          got.empty() ? v.id : got.c_str());
    log("bosses: %s defeated, loot %s", B(b).id, v.id);
}

// The hunter is inside an attack's reach and arc.
bool in_reach(const Boss& b, const Boss_attacks& a, Vec3 hunter) {
    if (fabsf(hunter.y - b.pos.y) > 700 * B(b).scale) return false;
    float reach = a.reach_m * 100.0f * B(b).scale + 80.0f;
    Vec3 from = a.arc_deg >= 359 ? b.pos : head_point(b);
    Vec3 d = sub(hunter, from);
    if (hlen(d) > reach) return false;
    if (a.arc_deg >= 359) return true;
    float ang = fabsf(wrap(yaw_to(b.pos, hunter) - b.yaw)) * 180.0f / kPi;
    return ang <= a.arc_deg * 0.5f || hlen(sub(hunter, b.pos)) < 250.0f * B(b).scale;
}

bool window(const char* hits, int i, float* s, float* e) {
    const char* p = hits;
    for (int k = 0; k < i && p; k++) {
        p = strchr(p, ',');
        if (p) p++;
    }
    return p && sscanf(p, "%f-%f", s, e) == 2;
}

// One tick of a living boss.
int think(Boss& b, SaveData& s, Live& l, double t, float dt) {
    (void)s;
    const GameSnapshot& g = l.snap;
    const Bosses& r = B(b);
    int ev = 0;
    Vec3 hunter = g.pos;
    float dist = hlen(sub(hunter, head_point(b)));
    float toHunter = yaw_to(b.pos, hunter);
    float off = wrap(toHunter - b.yaw);
    bool sees = false;
    if (dist < r.aggro_m * 100.0f) {
        Vec3 hit, eye = add(head_point(b), {0, 300, 0});
        sees = !world_ray(eye, add(hunter, {0, 120, 0}), &hit);
    }
    if (sees) b.lastSeen = t;
    if (!b.aggro && (sees || b.hp < b.maxHp)) b.aggro = true;
    if (b.aggro && dist > r.leash_m * 100.0f && t - b.lastSeen > 15) {
        b.aggro = b.roared = false;
        b.home = b.pos;
        go(b, ST_IDLE, t);
    }

    switch (b.st) {
        case ST_IDLE:
        case ST_WALK: {
            if (b.aggro) {
                b.roared ? go(b, ST_CHASE, t) : go(b, ST_ROAR, t);
                break;
            }
            if (t > b.goalUntil) {
                if (b.st == ST_IDLE) {
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
                    !move(b, mul(fwd_of(b.yaw), r.walk_mps * 100.0f * dt), t))
                    b.goalUntil = 0;  // a wall: pick somewhere else
                if (hlen(sub(b.goal, b.pos)) < 300) b.goalUntil = 0;
            }
            break;
        }
        case ST_ROAR: {
            turn_towards(b, toHunter, r.turn_dps, dt);
            float len = anim_len(b, b.anim);
            if (b.at >= len) {
                b.roared = true;
                b.nextAttack = t + 0.4;
                go(b, ST_CHASE, t);
            }
            break;
        }
        case ST_CHASE: {
            if (fabsf(off) > 1.75f && dist < 2000) {
                b.turnFrom = b.yaw;
                b.turnBy = off > 0 ? std::min(off, kPi / 2) : std::max(off, -kPi / 2);
                bool toRight = (off > 0) != g_rh;  // positive yaw turns towards +x
                b.st = ST_TURN;
                b.stT0 = t;
                set_anim(b, anim_path(b.row, toRight ? r.anim_turn_r : r.anim_turn_l), false);
                break;
            }
            turn_towards(b, toHunter, r.turn_dps, dt);
            if (t >= b.nextAttack) {
                float total = 0;
                int pick = -1;
                for (int k : attacks_of(b.row)) {
                    const Boss_attacks& a = kBoss_attacks[k];
                    bool facing = a.arc_deg >= 359 || fabsf(off) < 0.6f || a.min_m > 10;
                    if (t < b.cd[k] || !facing || dist < a.min_m * 100.0f || dist > a.max_m * 100.0f) continue;
                    if (!anim_of(b.row, anim_path(b.row, a.anim))) continue;
                    total += a.weight;
                    if (frand() * total <= a.weight) pick = k;
                }
                if (pick >= 0) {
                    const Boss_attacks& a = kBoss_attacks[pick];
                    b.attack = pick;
                    b.windowsDone = 0;
                    b.st = ST_ATTACK;
                    b.stT0 = t;
                    set_anim(b, anim_path(b.row, a.anim), false);
                    b.cd[pick] = t + a.cooldown_s;
                    play(a.snd, b.pos, 1.0f);
                    break;
                }
            }
            bool distant = dist > 1400;
            set_anim(b, anim_path(b.row, distant ? r.anim_run : r.anim_walk), true);
            if (dist > 350 && fabsf(off) < 1.0f) move(b, mul(fwd_of(b.yaw), (distant ? r.run_mps : r.walk_mps) * 100.0f * dt), t);
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
            // The animation's own forward motion moves the creature (lunges, leaps, charges).
            if (an) {
                float rt[3];
                anim_root(*an, std::min(b.at, len), false, rt);
                float dx = (rt[0] - b.root[0]) * r.scale, dy = (rt[1] - b.root[1]) * r.scale;
                memcpy(b.root, rt, sizeof(rt));
                Vec3 d = add(mul(right_of(b.yaw), -dx), mul(fwd_of(b.yaw), dy));
                move(b, d, t);
            }
            for (int w = 0; w < 8; w++) {
                float ws, we;
                if (!window(a.hits, w, &ws, &we)) break;
                if ((b.windowsDone & (1 << w)) || frac < ws || frac > we) continue;
                if (!in_reach(b, a, hunter)) continue;
                b.windowsDone |= 1 << w;
                if (t - g_dodgeAt < 0.45) {  // a dodge through the hit: Remnant's evade
                    ev |= BOSS_EV_EVADE;
                    continue;
                }
                float dmg = a.damage * sqrtf(b.rankK);
                if (g.playerHealth && add_hp(g.playerHealth, -dmg)) {
                    ev |= BOSS_EV_HURT;
                    std::lock_guard<std::mutex> lk(g_lock);
                    g_hurtAt = t;
                }
            }
            if (b.at >= len) {
                b.attack = -1;
                b.nextAttack = t + 0.5 + frand() * 1.2;
                go(b, ST_CHASE, t);
            }
            break;
        }
        case ST_FLINCH:
        case ST_STUN: {
            if (b.at >= anim_len(b, b.anim)) {
                b.nextAttack = t + 0.3;
                go(b, ST_CHASE, t);
            }
            break;
        }
        default: break;
    }
    // Footsteps while moving.
    if ((b.st == ST_WALK || b.st == ST_CHASE) && r.snd_step) {
        bool running = b.anim == anim_path(b.row, r.anim_run);
        if (t - b.lastStep > (running ? 0.32 : 0.55)) {
            b.lastStep = t;
            play(r.snd_step, b.pos, running ? 0.55f : 0.35f);
        }
    }
    return ev;
}

// MHW's melee weapons: an attack press near the boss lands a hit a moment later (approximate: MHW's own hitboxes
// only know MHW's monsters).
void melee(Live& l, double t) {
    bool lmb = key_down(VK_LBUTTON), rmb = key_down(VK_RBUTTON);
    bool press = (lmb && !g_lmbWas) || (rmb && !g_rmbWas);
    g_lmbWas = lmb;
    g_rmbWas = rmb;
    if (press && !l.gunMode && g_swingAt == 0) g_swingAt = t + 0.28;
    if (g_swingAt == 0 || t < g_swingAt) return;
    g_swingAt = 0;
    const GameSnapshot& g = l.snap;
    Vec3 look = g.view.valid ? sub(g.view.camTarget, g.view.camPos) : Vec3{0, 0, 1};
    for (int i = 0; i < kMax; i++) {
        Boss& b = g_boss[i];
        if (!b.used || b.st == ST_LOADING || b.st == ST_DEAD) continue;
        float mn[3], mx[3];
        extent(b, mn, mx);
        // The hunter's distance to the boss's box, in the boss's frame.
        Vec3 d = sub(g.pos, b.pos);
        Vec3 f = fwd_of(b.yaw), rt = right_of(b.yaw);
        float along = d.x * f.x + d.z * f.z, across = d.x * rt.x + d.z * rt.z;
        float ca = std::max(mn[1], std::min(mx[1], along)), cx = std::max(-mx[0], std::min(-mn[0], across));
        float gap = sqrtf((along - ca) * (along - ca) + (across - cx) * (across - cx));
        if (gap > 380 || d.y < -250 || d.y > mx[2] + 200) continue;
        Vec3 to = add(b.pos, add(mul(f, ca), mul(rt, cx)));
        Vec3 dir = sub(to, g.pos);
        float facing = (dir.x * look.x + dir.z * look.z) / std::max(1.0f, hlen(dir) * hlen(look));
        if (gap > 120 && facing < 0.1f) continue;
        float dmg = b.maxHp / std::max(20.0f, B(b).melee_hits) * (0.85f + 0.3f * frand());
        boss_hit(&g_token[i], dmg, {to.x, g.pos.y + 120, to.z}, false, false);
        break;
    }
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
        focus = {f->em, mhw_name(f->kind), f->hp, f->maxHp};
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
        bars.push_back({m.em, m.large ? 1 : 0, m.large ? mhw_name(m.kind) : std::string(), m.hp, m.maxHp, anchor});
    }
    std::lock_guard<std::mutex> lk(g_lock);
    g_bars.swap(bars);
    g_focus = focus;
    g_hasFocus = hasFocus;
}

void world_matrix(const Boss& b, float w[16]) {
    float s = B(b).scale;
    Vec3 f = fwd_of(b.yaw), r = right_of(b.yaw);
    // model x = -right, model y = forward, model z = up (Remnant II's creatures face +y)
    float rows[4][4] = {{-r.x * s, -r.y * s, -r.z * s, 0},
                        {f.x * s, f.y * s, f.z * s, 0},
                        {0, s, 0, 0},
                        {b.pos.x, b.pos.y - b.sink, b.pos.z, 1}};
    memcpy(w, rows, 64);
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

}  // namespace

bool is_boss(const uint8_t* em) { return em >= g_token && em < g_token + kMax; }

bool boss_hit(uint8_t* em, float dmg, const Vec3& at, bool crit, bool weak) {
    if (!is_boss(em)) return false;
    Boss& b = g_boss[em - g_token];
    if (!b.used || b.st == ST_LOADING || b.st == ST_DEAD || dmg <= 0) return true;
    double t = now_s();
    b.hp -= dmg;
    b.aggro = true;
    b.lastSeen = t;
    b.flinchAcc += dmg;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_numbers[g_nextNumber++ % 32] = {at, (int)lroundf(dmg), crit, weak, t};
    }
    const Bosses& r = B(b);
    if (b.hp <= 0) {
        b.hp = 0;
        go(b, ST_DEAD, t);
        return true;
    }
    if (b.st == ST_ROAR || b.st == ST_STUN) return true;
    if (!b.stunned && r.stun_at > 0 && b.hp < r.stun_at * b.maxHp) {
        b.stunned = true;
        b.flinchAcc = 0;
        go(b, ST_STUN, t);
    } else if (b.flinchAcc >= r.flinch_frac * b.maxHp) {
        b.flinchAcc = 0;
        go(b, ST_FLINCH, t);
    }
    return true;
}

int bosses_tick(SaveData& s, Live& l, double t, float dt) {
    const GameSnapshot& g = l.snap;
    g_snap = &g;
    if (g.view.valid) g_rh = g.view.proj[11] < 0;
    int ev = 0;
    mhw_bars(l, t);
    bool allowed = g.inGame && g.solo && (!g.inBase || g.zone == 504);
    if (!allowed || g.zone != g_zone) {
        for (Boss& b : g_boss)
            if (b.used) log("bosses: %s leaves (zone change)", B(b).id);
        for (Boss& b : g_boss) b = Boss();
        g_zone = g.zone;
        g_rollAt = t + 40 + frand() * 50;
        g_rollTries = 0;
    }
    if (!allowed) {
        std::lock_guard<std::mutex> lk(g_lock);
        g_draw.clear();
        g_hud.clear();
        g_creatureBars.clear();
        g_marks.clear();
        return 0;
    }

    // Roaming: a roll for each zone visit (fields only).
    if (!g.inBase && g_rollAt > 0 && t >= g_rollAt && g_rollTries < 6) {
        g_rollTries++;
        g_rollAt = 0;
        for (int row = 0; row < kBossesCount; row++) {
            if (!zone_listed(kBosses[row], g.zone) || frand() >= kBosses[row].spawn_chance) continue;
            Vec3 p;
            bool placed = false;
            for (int k = 0; k < 10 && !placed; k++) placed = place_near(g, frand() * 2 * kPi, 6000 + frand() * 5000, &p);
            if (placed) spawn(l, row, p, frand() * 2 * kPi, t);
            else g_rollAt = t + 20;  // try again from another spot
            break;
        }
    }
    // Debug / training: the boss key brings the next boss 30 m in front of the hunter.
    bool key = key_down(keys().boss);
    if (key && !g_keyWas && kBossesCount > 0 && g.view.valid) {
        Vec3 f = sub(g.view.camTarget, g.view.camPos);
        float yaw = atan2f(f.x, f.z);
        Vec3 p;
        if (place_near(g, yaw, 3000, &p) || place_near(g, yaw, 1500, &p)) {
            spawn(l, g_nextDebugRow % kBossesCount, p, wrap(yaw + kPi), t);
            g_nextDebugRow++;
        } else {
            toast(l, "No ground there for a Remnant boss");
        }
    }
    g_keyWas = key;
    if (key_down(VK_SPACE)) g_dodgeAt = t;

    melee(l, t);

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
                toast(l, "Remnant II's %s could not be read", r.id);
                log("bosses: %s: model or idle animation unreadable, removed", r.id);
                b = Boss();
                continue;
            }
            if (!idle) {
                if (t - b.stT0 > 90) { log("bosses: %s: still loading after 90 s, removed", r.id); b = Boss(); }
                continue;
            }
            float y;
            if (ground(b.pos, &y)) b.pos.y = y;
            go(b, ST_IDLE, t);
            b.goalUntil = t + 2;
            log("bosses: %s is out (%.0f hp)", r.id, b.maxHp);
        }
        // Animation clocks.
        b.at += dt;
        b.pt += dt;
        b.fade = std::max(0.0f, b.fade - dt / 0.22f);
        if (b.st == ST_DEAD) {
            if (!b.looted) {
                b.looted = true;
                ev |= BOSS_EV_KILL;
                loot(s, l, b);
            }
            if (t - b.stT0 > 14) b.sink += dt * 120.0f;
            if (b.sink > 900) b = Boss();
            continue;
        }
        ev |= think(b, s, l, t, dt);
        float y;
        if (ground(b.pos, &y) && fabsf(y - b.pos.y) < 400) b.pos.y += (y - b.pos.y) * std::min(1.0f, dt * 12);
    }

    // The bosses as monsters for the rest of the mod (guns, skills, effects, targeting).
    for (int i = 0; i < kMax; i++) {
        Boss& b = g_boss[i];
        if (!b.used || b.st == ST_LOADING || b.st == ST_DEAD) continue;
        float mn[3], mx[3];
        extent(b, mn, mx);
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
        m.center = add(b.pos, add(mul(f, (mn[1] + mx[1]) / 2), add(mul(up, (mn[2] + mx[2]) / 2), mul(r, -(mn[0] + mx[0]) / 2))));
        m.half = {(mx[0] - mn[0]) * 0.45f, (mx[2] - mn[2]) * 0.45f, (mx[1] - mn[1]) * 0.45f};
        m.obbOk = true;
        l.snap.monsters.push_back(m);
    }

    // Hand over what to draw.
    std::lock_guard<std::mutex> lk(g_lock);
    g_draw.clear();
    g_hud.clear();
    g_creatureBars.clear();
    g_marks.clear();
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
        float dist = hlen(sub(g.pos, b.pos));
        if (b.st != ST_DEAD) g_marks.push_back({b.pos, b.row, b.aggro});
        if (B(b).tier == BOSSES_TIER_BOSS) {
            g_hud.push_back({b.row, b.hp, b.maxHp, b.aggro || (b.st == ST_DEAD && dist < 8000), b.st == ST_DEAD, b.stT0});
        } else if (b.st != ST_DEAD && dist < 6000) {
            float mn[3], mx[3];
            extent(b, mn, mx);
            const std::string& nm = r2_text(B(b).name);
            g_creatureBars.push_back({&g_token[i], B(b).tier == BOSSES_TIER_ELITE ? 1 : 0,
                                      B(b).tier == BOSSES_TIER_ELITE ? nm : std::string(), b.hp, b.maxHp,
                                      {b.pos.x, b.pos.y + mx[2] + 60, b.pos.z}});
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
    if (list.empty()) return;
    GameView v;
    if (!camera_view(&v)) return;
    std::vector<DrawItem> items;
    items.reserve(list.size());
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
        // Textures per material slot: "slot=asset" entries, matched by slot name.
        static std::set<std::string> keep;  // stable storage for the asset strings
        it.diffuse.assign(d.m->materials.size(), nullptr);
        std::string all = r.textures;
        size_t i = 0;
        while (i < all.size()) {
            size_t j = all.find(',', i);
            if (j == std::string::npos) j = all.size();
            std::string e = all.substr(i, j - i);
            size_t eq = e.find('=');
            if (eq != std::string::npos) {
                std::string slot = e.substr(0, eq), asset = e.substr(eq + 1);
                for (size_t k = 0; k < d.m->materials.size(); k++) {
                    std::string mat = d.m->materials[k];
                    if (it.diffuse[k] || mat.find(slot) == std::string::npos) continue;
                    it.diffuse[k] = keep.insert(asset).first->c_str();
                }
            }
            i = j + 1;
        }
        items.push_back(std::move(it));
    }
    render3d_frame(dev, ctx, rtv, w, h, v.view, v.proj, items);
}

void bosses_hud() {
    std::vector<HudBoss> hud;
    std::vector<Bar> bars;
    std::vector<Mark> marks;
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
        memcpy(nums, g_numbers, sizeof(nums));
        hurtAt = g_hurtAt;
    }
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
        const std::string& nm = r2_text(kBosses[h.row].name);
        std::string name = nm.empty() ? kBosses[h.row].id : nm;
        for (char& c : name) c = (char)toupper((unsigned char)c);
        ImFont* title = ui_font(2);
        float fs = 26 * sc;
        ImVec2 ts = title->CalcTextSizeA(fs, FLT_MAX, 0, name.c_str());
        dl->AddText(title, fs, {x0 + 1, y + 1}, IM_COL32(0, 0, 0, 200), name.c_str());
        dl->AddText(title, fs, {x0, y}, IM_COL32(236, 228, 214, 255), name.c_str());
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
        y = by + bh + 22 * sc;
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
        bar(x0, p.y - bh, w, bh, frac, trail_of(b.key, frac), b.tier == 1 ? IM_COL32(176, 146, 84, 150) : IM_COL32(0, 0, 0, 160),
            1.0f * sc);
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
