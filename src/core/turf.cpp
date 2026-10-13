// Remnant x MHW mashup core: turf wars. GPL-3.0-or-later.
//
// MHW's own turf wars are paired animations Capcom made for particular pairs of monsters. This adds staged ones:
//  - between two large MHW monsters that meet near the hunter. Pairs MHW already has a turf war for are left to MHW:
//    when two monsters badly hurt each other on their own, the pair is remembered and never staged;
//  - between a Remnant II boss and an MHW monster (bosses.cpp runs the boss's side and asks this file for the MHW
//    monster's: its facing, its blows, its flinches).
// The MHW monsters fight with their own moves. Nothing documents which of a monster's actions are attacks, so the mod
// learns them by watching. The action a monster is in when it hits the hunter is an attack, along with when the hit
// lands in it and how far it reaches. The action it switches to right after a heavy hit is a flinch. What was learned
// is kept in learned_turf.txt in the mod folder. Until a monster's attacks are known, its staged blows land without an
// animation.
// Moves are played through ActionController::DoAction (reactions.cpp; the function is called, never changed). A
// monster turns to face its opponent by having its rotation written. That is data, not code, and is done only once its
// rotation has been checked against how the monster moves. remnant_hunt.ini [turf]: enabled, mhw_vs_mhw, facing.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "bosses.h"

namespace rh {

namespace {

constexpr int kAction = 0x61C8 + 0xAC;  // the unit's ActionController (SharpPluginLoader's Entity) + current {set, id}
constexpr int kRot = 0x170;             // an MT Framework unit's rotation quaternion (x, y, z, w), after its position
constexpr float kPi = 3.14159265f;

Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 mul(Vec3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float len3(Vec3 a) { return sqrtf(dot(a, a)); }
float hlen(Vec3 a) { return sqrtf(a.x * a.x + a.z * a.z); }
float wrap(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}
float frand() { return (float)rand() / (float)RAND_MAX; }

struct Quat { float x, y, z, w; };
Vec3 rotate(const Quat& q, Vec3 v) {
    Vec3 u = {q.x, q.y, q.z};
    Vec3 t = mul(cross(u, v), 2.0f);
    return add(add(v, mul(t, q.w)), cross(u, t));
}
Quat qmul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// ---------------------------------------------------------------- settings and self-checks

int g_cfg[3] = {-1, -1, -1};  // enabled, mhw_vs_mhw, facing
bool cfg(int i) {
    if (g_cfg[0] < 0) {
        char ini[MAX_PATH];
        snprintf(ini, sizeof(ini), "%s\\remnant_hunt.ini", mod_dir());
        g_cfg[0] = GetPrivateProfileIntA("turf", "enabled", 1, ini);
        g_cfg[1] = GetPrivateProfileIntA("turf", "mhw_vs_mhw", 1, ini);
        g_cfg[2] = GetPrivateProfileIntA("turf", "facing", 1, ini);
    }
    return g_cfg[0] != 0 && g_cfg[i] != 0;
}

// Each memory field the turf wars rely on is checked in play before it is used, and the verdict logged once.
struct Check {
    const char* what;
    int good = 0, bad = 0;
    bool ok = false, failed = false;
    void note(bool g, int needGood, int maxBad) {
        if (ok || failed) return;
        (g ? good : bad)++;
        if (good >= needGood && bad * 10 <= good) {
            ok = true;
            log("turf: %s confirmed (%d good, %d bad)", what, good, bad);
        } else if (bad >= maxBad && bad > good) {
            failed = true;
            log("turf: %s does not hold (%d good, %d bad): turned off for this session", what, good, bad);
        }
    }
};
Check g_act{"monster actions (ActionController +0xAC)"}, g_quat{"monster rotation quaternion"}, g_drive{"DoAction on monsters"},
    g_write{"facing writes"};
int g_fwd = 0;  // the monsters' forward: +1 = the rotation's +z, -1 = -z (from how they move)
float g_fwdSum = 0;
int g_fwdN = 0;

// ---------------------------------------------------------------- watching the monsters

struct Watch {
    int set = -1, id = -1, pset = -1, pid = -1;
    double since = 0, psince = 0;
    float hp = -1;
    double hurtAt = -10, hurtBig = 0;  // last hit not from the mod, and its share of max health
    double modAt = -10;                // last damage the mod dealt itself
    Vec3 c0{};
    double c0t = 0;
    double busyUntil = 0;              // a staged move is playing
    int wantSet = -1, wantId = -1;
    double wantAt = 0;                 // a DoAction to confirm
    double wroteAt = 0;
    float wroteYaw = 0;
    double seen = 0;
};
std::map<const uint8_t*, Watch> g_w;

struct Move { int set, id, n; float hitT, reach; };
struct Kind { std::vector<Move> attacks, flinches; };
std::map<std::string, Kind> g_kinds;
std::set<std::string> g_native;  // "emA_00|emB_00", sorted
bool g_loaded = false, g_dirty = false;
double g_saveAt = 0;
float g_hp = -1;

std::string path() {
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s\\learned_turf.txt", mod_dir());
    return p;
}

void load() {
    g_loaded = true;
    FILE* f = fopen(path().c_str(), "rb");
    if (!f) return;
    char line[160], what[16], k[16], k2[16];
    int set, id, n;
    float a, b;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "attack %15s %d %d %d %f %f", k, &set, &id, &n, &a, &b) == 6)
            g_kinds[k].attacks.push_back({set, id, n, a, b});
        else if (sscanf(line, "flinch %15s %d %d %d", k, &set, &id, &n) == 4)
            g_kinds[k].flinches.push_back({set, id, n, 0, 0});
        else if (sscanf(line, "native %15s %15s", k, k2) == 2)
            g_native.insert(std::string(k) + "|" + k2);
        else if (sscanf(line, "%15s %d", what, &n) == 2 && !strcmp(what, "forward") && (n == 1 || n == -1))
            g_fwd = n;
    }
    fclose(f);
    log("turf: learned moves for %zu monsters, %zu native turf-war pairs, forward %+d", g_kinds.size(), g_native.size(),
        g_fwd);
}

void save() {
    FILE* f = fopen(path().c_str(), "wb");
    if (!f) return;
    if (g_fwd) fprintf(f, "forward %d\n", g_fwd);
    for (auto& kv : g_kinds) {
        for (const Move& m : kv.second.attacks)
            fprintf(f, "attack %s %d %d %d %.2f %.0f\n", kv.first.c_str(), m.set, m.id, m.n, m.hitT, m.reach);
        for (const Move& m : kv.second.flinches) fprintf(f, "flinch %s %d %d %d\n", kv.first.c_str(), m.set, m.id, m.n);
    }
    for (const std::string& p : g_native) {
        size_t bar = p.find('|');
        fprintf(f, "native %s %s\n", p.substr(0, bar).c_str(), p.substr(bar + 1).c_str());
    }
    fclose(f);
}

bool real(const Monster& m) { return m.large && m.health && !is_boss(m.em) && m.kind[0] && m.obbOk; }

bool read_action(const uint8_t* em, int* set, int* id) {
    if (!mem_readable(em + kAction, 8)) return false;
    *set = *(const int*)(em + kAction);
    *id = *(const int*)(em + kAction + 4);
    return true;
}
bool plausible_action(int set, int id) { return set >= 0 && set <= 15 && id >= 0 && id < 4096; }

bool read_quat(const uint8_t* em, Quat* q) {
    if (!mem_readable(em + kRot, 16)) return false;
    memcpy(q, em + kRot, 16);
    float n = q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w;
    return std::isfinite(n) && fabsf(n - 1.0f) < 0.02f;
}
float yaw_of(const Quat& q) {
    Vec3 f = rotate(q, {0, 0, (float)g_fwd});
    return atan2f(f.x, f.z);
}

Move* find_move(std::vector<Move>& v, int set, int id) {
    for (Move& m : v)
        if (m.set == set && m.id == id) return &m;
    return nullptr;
}
bool is_attack(const char* kind, int set, int id) {
    auto it = g_kinds.find(kind);
    return it != g_kinds.end() && find_move(it->second.attacks, set, id);
}

void learn_attack(const char* kind, int set, int id, float hitT, float reach) {
    if (!plausible_action(set, id)) return;
    hitT = std::max(0.15f, std::min(4.0f, hitT));
    Kind& k = g_kinds[kind];
    if (Move* f = find_move(k.flinches, set, id)) f->n = std::max(0, f->n - 2);  // it was a blow after all
    Move* m = find_move(k.attacks, set, id);
    if (!m) {
        k.attacks.push_back({set, id, 0, hitT, reach});
        m = &k.attacks.back();
    }
    m->hitT = (m->hitT * m->n + hitT) / (m->n + 1);
    m->reach = (m->reach * m->n + reach) / (m->n + 1);
    m->n = std::min(m->n + 1, 999);
    g_dirty = true;
    log("turf: %s hit the hunter with action %d:%d (%.2f s in, %.0f cm), seen %d times", kind, set, id, hitT, reach, m->n);
}

void learn_flinch(const char* kind, int set, int id) {
    if (!plausible_action(set, id) || is_attack(kind, set, id)) return;
    Kind& k = g_kinds[kind];
    Move* m = find_move(k.flinches, set, id);
    if (!m) {
        k.flinches.push_back({set, id, 0, 0, 0});
        m = &k.flinches.back();
    }
    m->n = std::min(m->n + 1, 999);
    g_dirty = true;
}

// The closest point of a monster's hitbox (OBB) to p.
Vec3 closest(const Monster& m, Vec3 p) {
    Vec3 d = sub(p, m.center), out = m.center;
    const float half[3] = {m.half.x, m.half.y, m.half.z};
    for (int k = 0; k < 3; k++) {
        Vec3 ax = {m.rot[k][0], m.rot[k][1], m.rot[k][2]};
        float n = len3(ax);
        if (n < 1e-4f) continue;
        ax = mul(ax, 1.0f / n);
        float s = std::max(-half[k], std::min(half[k], dot(d, ax)));
        out = add(out, mul(ax, s));
    }
    return out;
}
float gap_between(const Monster& a, const Monster& b) {
    Vec3 pb = closest(b, a.center);
    Vec3 pa = closest(a, pb);
    return hlen(sub(pb, pa));
}

const Monster* find(const GameSnapshot& g, const uint8_t* em) {
    for (const Monster& m : g.monsters)
        if (m.em == em) return &m;
    return nullptr;
}

std::string name_of(const Monster& m) {
    int i = mhw_monsters_index(m.kind);
    return i >= 0 ? std::string(kMhw_monsters[i].name) : std::string(m.kind);
}

bool can_drive() { return g_act.ok && !g_drive.failed; }
bool can_face() { return cfg(2) && g_quat.ok && g_fwd != 0 && !g_write.failed; }

bool drive(const Monster& m, const Move& mv, double t, float busyFor) {
    if (!can_drive() || !mhw_do_action(m.em, mv.set, mv.id)) return false;
    Watch& w = g_w[m.em];
    w.wantSet = mv.set;
    w.wantId = mv.id;
    w.wantAt = t;
    w.busyUntil = t + busyFor;
    return true;
}

const Move* pick(const std::vector<Move>& v, float gap) {
    float total = 0;
    const Move* best = nullptr;
    for (const Move& m : v) {
        if (m.n < 2) continue;
        float wgt = (float)std::min(m.n, 8) * (m.reach <= 0 || m.reach + 150 >= gap ? 1.0f : 0.15f);
        total += wgt;
        if (frand() * total <= wgt) best = &m;
    }
    return best;
}

// ---------------------------------------------------------------- MHW monster against MHW monster

struct War {
    bool on = false;
    const uint8_t* em[2] = {};
    std::string name[2];
    int phase = 0;  // 0 squaring off, 1 trading blows, 2 the loser goes down
    double t0 = 0, next = 0, hitAt = 0;
    int attacker = 0, hits = 0, maxHits = 4, downs = 0;
    float reach = 0, lost[2] = {0, 0};
};
War g_war;
std::map<std::pair<const uint8_t*, const uint8_t*>, double> g_near, g_pairCd;
std::set<const uint8_t*> g_claimed;  // in a turf war with a Remnant boss
double g_nextWarAt = 0, g_scanAt = 0;
int g_zone = -1;

std::pair<const uint8_t*, const uint8_t*> key(const uint8_t* a, const uint8_t* b) { return a < b ? std::make_pair(a, b) : std::make_pair(b, a); }
std::string kinds_key(const char* a, const char* b) {
    return strcmp(a, b) < 0 ? std::string(a) + "|" + b : std::string(b) + "|" + a;
}

void end_war(const char* why, double t) {
    if (!g_war.on) return;
    log("turf: %s vs %s ends (%s): -%.0f%% / -%.0f%%", g_war.name[0].c_str(), g_war.name[1].c_str(), why,
        g_war.lost[0] * 100, g_war.lost[1] * 100);
    g_pairCd[key(g_war.em[0], g_war.em[1])] = t + tune(TUNING_TURF_COOLDOWN_S) * 3;  // once per meeting, as MHW's
    g_nextWarAt = t + tune(TUNING_TURF_COOLDOWN_S) * 0.4;
    g_war = War();
}

float hit_share(const Monster& a, const Monster& d) {
    float size = powf(std::max(1.0f, a.maxHp) / std::max(1.0f, d.maxHp), 0.3f);
    return tune(TUNING_TURF_HIT_FRAC) * (0.8f + 0.45f * frand()) * std::max(0.75f, std::min(1.3f, size));
}

void run_war(Live& l, double t, float dt) {
    const GameSnapshot& g = l.snap;
    const Monster* m[2] = {find(g, g_war.em[0]), find(g, g_war.em[1])};
    if (!m[0] || !m[1]) return end_war("a monster left", t);
    if (gap_between(*m[0], *m[1]) > 4500 || hlen(sub(g.pos, m[0]->center)) > 12000) return end_war("apart", t);
    switch (g_war.phase) {
        case 0:  // squaring off
            turf_face(*m[0], m[1]->center, dt);
            turf_face(*m[1], m[0]->center, dt);
            if (t - g_war.t0 > 1.6) {
                g_war.phase = 1;
                g_war.attacker = m[0]->hp / m[0]->maxHp >= m[1]->hp / m[1]->maxHp ? 0 : 1;
                g_war.next = t;
            }
            break;
        case 1: {  // trading blows
            int a = g_war.attacker, d = 1 - a;
            if (g_war.hitAt == 0) {
                turf_face(*m[d], m[a]->center, dt);
                if (t < g_war.next) break;
                bool facing = turf_face(*m[a], m[d]->center, dt);
                if (!facing && t - g_war.next < 0.8) break;  // a moment to turn
                float gap = gap_between(*m[a], *m[d]);
                g_war.hitAt = t + turf_attack(*m[a], gap, &g_war.reach);
            } else if (t >= g_war.hitAt) {
                g_war.hitAt = 0;
                if (gap_between(*m[a], *m[d]) <= g_war.reach + 400) {  // its hitboxes reach past its body
                    float share = hit_share(*m[a], *m[d]);
                    turf_hurt(*m[d], share * m[d]->maxHp);
                    g_war.lost[d] += share;
                    turf_flinch(*m[d]);
                    g_war.hits++;
                }
                if (frand() > 0.25f) g_war.attacker = d;
                g_war.next = t + 0.9 + frand() * 0.9;
                if (g_war.hits >= g_war.maxHits || t - g_war.t0 > 40) {
                    g_war.phase = 2;
                    g_war.next = t;
                }
            }
            break;
        }
        case 2: {  // the one hurt worse goes down
            int loser = g_war.lost[0] > g_war.lost[1] ? 0 : 1;
            if (t >= g_war.next) {
                turf_flinch(*m[loser]);
                g_war.downs++;
                g_war.next = t + 1.3;
                if (g_war.downs >= 2) {
                    std::string why = g_war.name[1 - loser] + " won";
                    return end_war(why.c_str(), t);
                }
            }
            break;
        }
    }
}

void look_for_war(Live& l, double t) {
    const GameSnapshot& g = l.snap;
    if (t < g_scanAt) return;
    g_scanAt = t + 0.5;
    std::vector<const Monster*> big;
    for (const Monster& m : g.monsters)
        if (real(m) && !g_claimed.count(m.em)) big.push_back(&m);
    std::set<std::pair<const uint8_t*, const uint8_t*>> close;
    for (size_t i = 0; i < big.size(); i++)
        for (size_t j = i + 1; j < big.size(); j++) {
            const Monster& a = *big[i];
            const Monster& b = *big[j];
            if (gap_between(a, b) > tune(TUNING_TURF_RANGE_M) * 100.0f) continue;
            Vec3 mid = mul(add(a.center, b.center), 0.5f);
            if (hlen(sub(g.pos, mid)) > 7000) continue;  // out where the hunter would not see it
            auto k = key(a.em, b.em);
            close.insert(k);
            if (!g_near.count(k)) g_near[k] = t;
            auto cd = g_pairCd.find(k);
            if (g_native.count(kinds_key(a.kind, b.kind)) || (cd != g_pairCd.end() && t < cd->second)) continue;
            if (t - g_near[k] < 4.0 || t < g_nextWarAt) continue;  // MHW's own turf war, if it has one, starts first
            g_pairCd[k] = t + tune(TUNING_TURF_COOLDOWN_S);   // one roll per meeting
            if (frand() > tune(TUNING_TURF_MHW_CHANCE)) continue;
            g_war = War();
            g_war.on = true;
            g_war.em[0] = a.em;
            g_war.em[1] = b.em;
            g_war.name[0] = name_of(a);
            g_war.name[1] = name_of(b);
            g_war.t0 = t;
            g_war.maxHits = 3 + rand() % 3;
            toast(l, "Turf war: %s vs %s", g_war.name[0].c_str(), g_war.name[1].c_str());
            log("turf: %s (%s) vs %s (%s), %d blows", g_war.name[0].c_str(), a.kind, g_war.name[1].c_str(), b.kind,
                g_war.maxHits);
            return;
        }
    for (auto it = g_near.begin(); it != g_near.end();) it = close.count(it->first) ? std::next(it) : g_near.erase(it);
}

// ---------------------------------------------------------------- learning

void watch(Live& l, double t) {
    const GameSnapshot& g = l.snap;
    for (const Monster& m : g.monsters) {
        if (!real(m)) continue;
        Watch& w = g_w[m.em];
        w.seen = t;
        int set, id;
        if (read_action(m.em, &set, &id)) {
            bool ok = plausible_action(set, id);
            if (ok && (set != w.set || id != w.id) && w.set >= 0) g_act.note(true, 25, 60);
            if (!ok) g_act.note(false, 25, 60);
            if (ok && (set != w.set || id != w.id)) {
                // a flinch: the action it switches to right after a heavy hit that was not the mod's
                if (w.set >= 0 && t - w.hurtAt < 0.3 && w.hurtBig >= 0.006 && t > w.busyUntil)
                    learn_flinch(m.kind, set, id);
                w.pset = w.set;
                w.pid = w.id;
                w.psince = w.since;
                w.set = set;
                w.id = id;
                w.since = t;
            }
            if (w.wantAt > 0) {  // did a DoAction take?
                if (set == w.wantSet && id == w.wantId) { g_drive.note(true, 3, 6); w.wantAt = 0; }
                else if (t - w.wantAt > 0.4) { g_drive.note(false, 3, 6); w.wantAt = 0; }
            }
        }
        Quat q;
        bool qok = read_quat(m.em, &q);
        g_quat.note(qok, 30, 30);
        if (qok && w.wroteAt > 0) {  // did the last facing write stick?
            g_write.note(fabsf(wrap(yaw_of(q) - w.wroteYaw)) < 0.2f, 5, 10);
            w.wroteAt = 0;
        }
        // Which way is forward: the rotation's z against where the monster walks.
        if (qok && t - w.c0t >= 0.5) {
            Vec3 d = sub(m.center, w.c0);
            float dl = hlen(d);
            if (w.c0t > 0 && dl > 120 && dl < 3000 && g_fwdN < 40) {
                Vec3 f = rotate(q, {0, 0, 1});
                float fl = hlen(f);
                if (fl > 0.3f) {
                    g_fwdSum += (f.x * d.x + f.z * d.z) / (fl * dl);
                    if (++g_fwdN == 40) {
                        float avg = g_fwdSum / g_fwdN;
                        int was = g_fwd;
                        g_fwd = avg > 0.55f ? 1 : avg < -0.55f ? -1 : 0;
                        log("turf: monsters walk along %+.2f of their rotation's z: forward %+d", avg, g_fwd);
                        if (g_fwd != was) g_dirty = true;
                    }
                }
            }
            w.c0 = m.center;
            w.c0t = t;
        }
        // Hits not dealt by the mod (the hunter's MHW weapon, another monster, the environment).
        if (w.hp >= 0 && m.hp < w.hp - 0.5f && t - w.modAt > 0.35) {
            w.hurtAt = t;
            w.hurtBig = (w.hp - m.hp) / std::max(1.0f, m.maxHp);
        }
        w.hp = m.hp;
    }
    // MHW's own turf wars: two monsters badly hurt within a moment of each other, the hunter not next to both.
    for (auto& a : g_w)
        for (auto& b : g_w) {
            if (a.first >= b.first || t - a.second.hurtAt > 0.1 || fabs(a.second.hurtAt - b.second.hurtAt) > 1.5) continue;
            if (a.second.hurtBig < 0.02 || b.second.hurtBig < 0.02) continue;
            const Monster* ma = find(g, a.first);
            const Monster* mb = find(g, b.first);
            if (!ma || !mb || gap_between(*ma, *mb) > 1500) continue;
            if (hlen(sub(closest(*ma, g.pos), g.pos)) < 600 && hlen(sub(closest(*mb, g.pos), g.pos)) < 600) continue;
            if (g_native.insert(kinds_key(ma->kind, mb->kind)).second) {
                g_dirty = true;
                log("turf: %s and %s have a turf war of MHW's own: left to MHW", ma->kind, mb->kind);
            }
        }
    // The hunter hit by a monster: the action that monster is in is an attack.
    if (g.playerHealth && mem_readable(g.playerHealth + 0x60, 8)) {
        float maxHp = *(float*)(g.playerHealth + 0x60), hp = *(float*)(g.playerHealth + 0x64);
        if (g_hp >= 0 && hp < g_hp - 0.04f * maxHp && t - reactions_own_damage_at() > 0.3) {
            const Monster* best = nullptr;
            float bestGap = 900;
            for (const Monster& m : g.monsters) {
                if (!real(m)) continue;
                float gap = hlen(sub(closest(m, g.pos), g.pos));
                if (gap < bestGap) { bestGap = gap; best = &m; }
            }
            if (best) {
                Watch& w = g_w[best->em];
                bool fresh = t - w.since < 0.1 && w.pset >= 0;
                if (fresh) learn_attack(best->kind, w.pset, w.pid, (float)(t - w.psince), bestGap + 100);
                else if (w.set >= 0) learn_attack(best->kind, w.set, w.id, (float)(t - w.since), bestGap + 100);
            }
        }
        g_hp = hp;
    }
    for (auto it = g_w.begin(); it != g_w.end();) it = t - it->second.seen > 5 ? g_w.erase(it) : std::next(it);
}

}  // namespace

// ---------------------------------------------------------------- shared with the bosses

bool turf_enabled() { return cfg(0); }

Vec3 turf_closest(const Monster& m, Vec3 p) { return closest(m, p); }

void turf_note_mod_damage(const uint8_t* em, double t) {
    auto it = g_w.find(em);
    if (it != g_w.end()) it->second.modAt = t;
}

void turf_hurt(const Monster& m, float dmg) {
    dmg = std::min(dmg, m.hp - 0.05f * m.maxHp);  // a turf war never finishes a monster: that is left to the hunter
    if (!m.health || dmg <= 0) return;
    turf_note_mod_damage(m.em, now_s());
    add_hp(m.health, -dmg);
}

bool turf_face(const Monster& m, Vec3 at, float dt) {
    Quat q;
    if (!g_fwd || !read_quat(m.em, &q)) return true;  // cannot tell: carry on as if facing
    float now = yaw_of(q);
    float err = wrap(atan2f(at.x - m.center.x, at.z - m.center.z) - now);
    Watch& w = g_w[m.em];
    if (fabsf(err) < 0.05f || !can_face() || now_s() < w.busyUntil) return fabsf(err) < 0.6f;
    float step = tune(TUNING_TURF_TURN_DPS) * kPi / 180.0f * dt;
    float d = std::max(-step, std::min(step, err));
    Quat turn = {0, sinf(d * 0.5f), 0, cosf(d * 0.5f)};  // about the world's up axis
    Quat nq = qmul(turn, q);
    float n = sqrtf(nq.x * nq.x + nq.y * nq.y + nq.z * nq.z + nq.w * nq.w);
    nq = {nq.x / n, nq.y / n, nq.z / n, nq.w / n};
    memcpy((uint8_t*)m.em + kRot, &nq, 16);
    w.wroteAt = now_s();
    w.wroteYaw = yaw_of(nq);
    return fabsf(err - d) < 0.6f;
}

float turf_attack(const Monster& m, float gap, float* reach) {
    auto it = g_kinds.find(m.kind);
    const Move* mv = it != g_kinds.end() ? pick(it->second.attacks, gap) : nullptr;
    double t = now_s();
    if (mv && drive(m, *mv, t, mv->hitT + 0.6f)) {
        *reach = std::max(mv->reach, 300.0f);
        return std::max(0.25f, std::min(3.0f, mv->hitT));
    }
    // Not learned yet (or moves cannot be played): the blow lands without an animation of its own.
    *reach = std::max(m.half.x, m.half.z) * 1.6f + 300.0f;
    return 0.9f;
}

void turf_flinch(const Monster& m) {
    auto it = g_kinds.find(m.kind);
    if (it == g_kinds.end()) return;
    std::vector<Move> sure;
    for (const Move& f : it->second.flinches)
        if (f.n >= 3) sure.push_back(f);
    const Move* mv = pick(sure, 0);
    double t = now_s();
    if (mv && t >= g_w[m.em].busyUntil) drive(m, *mv, t, 0.8f);
}

bool turf_busy(const uint8_t* em) { return g_war.on && (g_war.em[0] == em || g_war.em[1] == em); }

void turf_claim(const uint8_t* em, bool on) {
    if (on) g_claimed.insert(em);
    else g_claimed.erase(em);
}

// Game thread, after bosses_tick.
void turf_tick(Live& l, double t, float dt) {
    if (!g_loaded) load();
    const GameSnapshot& g = l.snap;
    if (!cfg(0) || !g.inGame || g.inBase || !g.solo || g.zone != g_zone) {
        end_war("zone", t);
        g_zone = g.zone;
        g_near.clear();
        g_claimed.clear();
        if (!g.inGame || g.inBase) return;
    }
    watch(l, t);
    if (cfg(1)) {
        if (g_war.on) run_war(l, t, dt);
        else look_for_war(l, t);
    }
    if (g_dirty && t > g_saveAt) {
        save();
        g_dirty = false;
        g_saveAt = t + 5;
    }
}

}  // namespace rh
