// Remnant x MHW mashup core: Remnant II's rules running on top of Monster Hunter: World. GPL-3.0-or-later (see LICENSE).
//
// Every number comes from the design sheets (gen/sheets.gen.h). The game-thread tick reads MHW's state, fires
// Remnant guns as hitscan shots against the monsters' boxes, and turns HP changes into the effects of the hunter's
// Remnant loadout (archetypes, perks, traits, guns, mods, mutators, relic, fragments, amulet, rings, armor). MHW's own
// weapons, armor and items keep working; their hits feed the same Remnant effects.
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "bosses.h"
#include "state.h"

namespace rh {

static Shared g_shared;
Shared& S() { return g_shared; }

double now_s() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
}

void toast(Live& l, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l.toast, sizeof(l.toast), fmt, ap);
    va_end(ap);
    l.toastUntil = now_s() + 3.5;
}

static volatile LONG g_cmds = 0;
void post(Cmd c) { InterlockedOr(&g_cmds, 1 << c); }

void request(const Request& r) {
    Shared& sh = S();
    AcquireSRWLockExclusive(&sh.lock);
    sh.live.requests.push_back(r);
    ReleaseSRWLockExclusive(&sh.lock);
}

const Archetypes* archetype() {
    int a = S().save.archetype;
    return (a >= 0 && a < kArchetypesCount) ? &kArchetypes[a] : nullptr;
}

int equipped_gun(const SaveData& s, const Live& l) { return l.usingLong ? s.longGun : s.handGun; }

int gun_mod(const SaveData& s, int gun) {
    if (gun < 0) return -1;
    if (kWeapons[gun].mod >= 0) return kWeapons[gun].mod;
    int slot = kWeapons[gun].slot == WEAPONS_SLOT_LONG ? 0 : 1;
    return s.gunMod[slot];
}

std::vector<int> skills_of(int a) { return a >= 0 ? rows_of(kArchetypes[a].skills, skills_index) : std::vector<int>{}; }
std::vector<int> perks_of(int a) { return a >= 0 ? rows_of(kArchetypes[a].perks, perks_index) : std::vector<int>{}; }

float upgrade_mult(int level) { return level > 0 && level <= kUpgradesCount ? kUpgrades[level - 1].damage_mult : 1.0f; }

bool can_equip_here(const Live& l) { return l.snap.inGame && l.snap.inBase; }

static uint32_t g_rng = 0x9E3779B9u;
static float frand() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (g_rng & 0xFFFFFF) / 16777216.0f;
}

// ---------------------------------------------------------------- loadout -> effect sources

static void rebuild_sources(const SaveData& s, Live& l) {
    EffectState& fx = l.fx;
    fx.clear_sources();
    for (int slot = 0; slot < 2; slot++) {
        int a = slot == 0 ? s.archetype : s.archetype2;
        if (a < 0) continue;
        std::vector<int> pk = perks_of(a);
        for (size_t i = 0; i < pk.size(); i++)
            if (slot == 0 || kPerks[pk[i]].kind != PERKS_KIND_PRIME) fx.add_source(kPerks[pk[i]].effects);
        int tr = kArchetypes[a].trait;
        if (tr >= 0 && slot == 0) fx.add_source(kTraits[tr].effects);
        if (slot == 1 && prime_time(s))  // the Prime Time legendary
            for (int p : pk)
                if (kPerks[p].kind == PERKS_KIND_PRIME) fx.add_source(kPerks[p].effects);
    }
    progress_sources(s, fx);
    if (s.longGun >= 0) fx.add_source(kWeapons[s.longGun].effects, 1.0f, 1);
    if (s.handGun >= 0) fx.add_source(kWeapons[s.handGun].effects, 1.0f, 2);
    if (s.melee >= 0) fx.add_source(kWeapons[s.melee].effects);
    for (int i = 0; i < 3; i++)
        if (s.mutator[i] >= 0) fx.add_source(kMutators[s.mutator[i]].effects, 1.0f, i < 2 ? i + 1 : 0);
    for (int f : s.fragment)
        if (f >= 0) fx.add_source(kFragments[f].effects);
    if (s.amulet >= 0) fx.add_source(kAmulets[s.amulet].effects);
    for (int r : s.ring)
        if (r >= 0) fx.add_source(kRings[r].effects);
    for (int a : s.armor)
        if (a >= 0) fx.add_source(kArmor[a].effects);
}

static float stat(const Live& l, Stat st, double t) { return l.fx.total(st, t); }

static int mag_size(const Live& l, int gun, double t) {
    return std::max(1, (int)lroundf(kWeapons[gun].mag * (1.0f + stat(l, ST_MAG, t))));
}
static int reserve_size(const Live& l, int gun, double t) {
    return (int)lroundf(kWeapons[gun].reserve * (1.0f + stat(l, ST_AMMO, t)));
}

static void fill_ammo(const SaveData& s, Live& l, double t) {
    for (int i = 0; i < kWeaponsCount; i++)
        if (s.has(CAT_WEAPON, i) && kWeapons[i].slot != WEAPONS_SLOT_MELEE) {
            l.mag[i] = mag_size(l, i, t);
            l.reserve[i] = reserve_size(l, i, t);
        }
}

static void rest(SaveData& s, Live& l, double t) {
    s.relicCharges = (int)(tune(TUNING_RELIC_BASE_CHARGES) + stat(l, ST_RELIC_CHARGES, t));
    fill_ammo(s, l, t);
    l.bonusHealth = l.bonusMax;
}

void choose_archetype(int row, int slot) {
    Shared& sh = S();
    AcquireSRWLockExclusive(&sh.lock);
    SaveData& s = sh.save;
    const Archetypes& a = kArchetypes[row];
    bool free = a.zenny <= 0;
    if (!s.has(CAT_ARCHETYPE, row) && !free) {
        ReleaseSRWLockExclusive(&sh.lock);
        return;
    }
    s.owned[CAT_ARCHETYPE][row] = 1;
    bool first = s.archetype < 0;
    if (slot == 0) {
        if (s.archetype2 == row) s.archetype2 = s.archetype;
        s.archetype = row;
    } else if (row != s.archetype) {
        s.archetype2 = row;
    }
    if (first) {
        for (int w : {a.starter_long, a.starter_hand, a.starter_melee})
            if (w >= 0) s.owned[CAT_WEAPON][w] = 1;
        s.longGun = a.starter_long;
        s.handGun = a.starter_hand;
        s.melee = a.starter_melee;
        int dh = relics_index("dragon_heart");
        if (dh >= 0) { s.owned[CAT_RELIC][dh] = 1; s.relic = dh; }
        s.relicCharges = (int)tune(TUNING_RELIC_BASE_CHARGES);
    }
    s.introDone = true;
    sh.live.loadoutChanged = true;
    sh.live.skillReady[0] = sh.live.skillReady[1] = 0;
    sh.live.dirty = true;
    double t = now_s();
    rebuild_sources(s, sh.live);
    fill_ammo(s, sh.live, t);
    ReleaseSRWLockExclusive(&sh.lock);
    log("archetype %s: %s", slot == 0 ? "primary" : "secondary", a.id);
}

void equip(Cat c, int row, int slot) {
    Shared& sh = S();
    AcquireSRWLockExclusive(&sh.lock);
    SaveData& s = sh.save;
    if (row >= 0 && !s.has(c, row)) {
        ReleaseSRWLockExclusive(&sh.lock);
        return;
    }
    auto put = [&](int* arr, int n) {
        for (int i = 0; i < n; i++)
            if (arr[i] == row && row >= 0) arr[i] = -1;  // the same item never sits in two slots
        arr[std::max(0, std::min(n - 1, slot))] = row;
    };
    switch (c) {
        case CAT_WEAPON:
            if (row < 0) break;
            if (kWeapons[row].slot == WEAPONS_SLOT_LONG) s.longGun = row;
            else if (kWeapons[row].slot == WEAPONS_SLOT_HAND) s.handGun = row;
            else s.melee = row;
            break;
        case CAT_MOD: s.gunMod[slot == 1 ? 1 : 0] = row; break;
        case CAT_MUTATOR: put(s.mutator, 3); break;
        case CAT_RELIC: s.relic = row; break;
        case CAT_FRAGMENT: put(s.fragment, 3); break;
        case CAT_AMULET: s.amulet = row; break;
        case CAT_RING: put(s.ring, 4); break;
        case CAT_ARMOR: if (row >= 0) s.armor[kArmor[row].piece] = row; else s.armor[slot & 3] = -1; break;
        default: break;
    }
    sh.live.loadoutChanged = true;
    sh.live.dirty = true;
    ReleaseSRWLockExclusive(&sh.lock);
}

// ---------------------------------------------------------------- vector math

static Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3 mul(Vec3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
static float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static float len(Vec3 a) { return sqrtf(dot(a, a)); }
static Vec3 norm(Vec3 a) { float l = len(a); return l > 1e-6f ? mul(a, 1.0f / l) : Vec3{0, 0, 1}; }

// Ray vs the monster's oriented box. Returns entry distance, local hit point.
static bool ray_obb(const Monster& m, Vec3 o, Vec3 d, float* tHit, float lp[3]) {
    Vec3 rel = sub(o, m.center);
    float ol[3], dl[3], h[3] = {m.half.x, m.half.y, m.half.z};
    for (int i = 0; i < 3; i++) {
        Vec3 axis = {m.rot[i][0], m.rot[i][1], m.rot[i][2]};
        ol[i] = dot(rel, axis);
        dl[i] = dot(d, axis);
    }
    float t0 = 0, t1 = 1e9f;
    for (int i = 0; i < 3; i++) {
        if (fabsf(dl[i]) < 1e-6f) {
            if (fabsf(ol[i]) > h[i]) return false;
            continue;
        }
        float a = (-h[i] - ol[i]) / dl[i], b = (h[i] - ol[i]) / dl[i];
        if (a > b) { float t = a; a = b; b = t; }
        if (a > t0) t0 = a;
        if (b < t1) t1 = b;
        if (t0 > t1) return false;
    }
    *tHit = t0;
    for (int i = 0; i < 3; i++) lp[i] = ol[i] + dl[i] * t0;
    return true;
}

// ---------------------------------------------------------------- damage

static const Monster* find_monster(const Live& l, uint8_t* em) {
    for (const Monster& m : l.snap.monsters)
        if (m.em == em) return &m;
    return nullptr;
}

// The hunter's target: the monster last hit, else the nearest large monster.
static const Monster* current_target(const Live& l) {
    if (const Monster* m = find_monster(l, l.target)) return m;
    const Monster* best = nullptr;
    float bd = 1e12f;
    for (const Monster& m : l.snap.monsters) {
        Vec3 d = sub(m.center, l.snap.pos);
        float dd = dot(d, d) * (m.large ? 1.0f : 4.0f);
        if (dd < bd) { bd = dd; best = &m; }
    }
    return best;
}

// Remnant numbers grow with the monster so the guns matter from low rank to master rank.
static float rank_mult(const Monster& m) {
    float k = m.maxHp / std::max(1.0f, tune(TUNING_RANK_SCALE_HP));
    return std::max(1.0f, std::min(tune(TUNING_RANK_SCALE_MAX), k));
}

// ---------------------------------------------------------------- Remnant statuses
// BLEEDING, BURNING, CORRODED, OVERLOADED deal their damage over time (OVERLOADED in bursts); EXPOSED makes the target
// take 15% more damage; SLOWED slows Remnant bosses; MARKED gives +10% critical chance against it. A status refreshes
// rather than stacks, as in Remnant II. Monsters are keyed by uEnemy* (MHW's and the bosses' pseudo pointers).

struct StatusInst { float perSec = 0; double until = 0, nextTick = 0; };
struct Afflict { StatusInst s[SS_COUNT]; };
static std::map<const void*, Afflict> g_aff;
static Afflict g_me;
static float tick_of(int st) { return st == SS_OVERLOADED ? 2.5f : 0.5f; }
static bool negative(int st) { return st >= SS_BLEEDING && st <= SS_OVERLOADED; }

uint32_t status_bits(const void* em) {
    double t = now_s();
    const Afflict* a = em ? nullptr : &g_me;
    if (em) {
        auto it = g_aff.find(em);
        if (it == g_aff.end()) return 0;
        a = &it->second;
    }
    uint32_t bits = 0;
    for (int s = 1; s < SS_COUNT; s++)
        if (a->s[s].until > t) bits |= 1u << s;
    return bits;
}

static float stat(const Live& l, Stat st, double t);
static float rank_mult(const Monster& m);

static void inflict(Live& l, const Monster& m, Status st, float total, float dur, double t) {
    if (st <= SS_NONE || st >= SS_COUNT) return;
    dur = std::max(1.0f, dur * (1.0f + stat(l, ST_STATUS_DUR, t)));
    StatusInst& s = g_aff[m.em].s[st];
    float scale = tune(TUNING_DAMAGE_SCALE) * rank_mult(m) * (1.0f + stat(l, ST_DMG, t) + stat(l, ST_DMG_STATUS, t));
    float ps = negative(st) ? total * scale / dur : 0;
    s.perSec = std::max(t < s.until ? s.perSec : 0.0f, ps);
    if (t >= s.until) s.nextTick = t + tick_of(st);
    s.until = std::max(s.until, t + dur);
}

void inflict_player(Live& l, Status st, float total, float dur, double t) {
    if (st <= SS_NONE || st >= SS_COUNT) return;
    Stat res = st == SS_BLEEDING ? ST_RES_BLEED : st == SS_BURNING ? ST_RES_FIRE : st == SS_OVERLOADED ? ST_RES_SHOCK
                                                                                : ST_RES_ACID;
    total *= 1.0f - std::max(0.0f, std::min(0.75f, stat(l, res, t)));
    StatusInst& s = g_me.s[st];
    s.perSec = std::max(t < s.until ? s.perSec : 0.0f, total / std::max(1.0f, dur));
    if (t >= s.until) s.nextTick = t + tick_of(st);
    s.until = std::max(s.until, t + dur);
    toast(l, "%s", status_label(st));
}

void status_player_dodge(double t) {
    StatusInst& b = g_me.s[SS_BURNING];
    if (b.until > t) b.until -= 1.5;  // rolling puts the fire out sooner
}

static void cure_player(Status st) { g_me.s[st] = StatusInst(); }

// Remnant damage against a target: EXPOSED and the "damage to <status> targets" bonuses.
static float status_mult(const Live& l, const Monster& m, double t) {
    uint32_t b = status_bits(m.em);
    if (!b) return 1.0f;
    float k = 1.0f;
    if (b & (1u << SS_EXPOSED)) k += 0.15f + stat(l, ST_VS_EXPOSED, t);
    if (b & (1u << SS_BLEEDING)) k += stat(l, ST_VS_BLEEDING, t);
    if (b & (1u << SS_BURNING)) k += stat(l, ST_VS_BURNING, t);
    if (b & (1u << SS_CORRODED)) k += stat(l, ST_VS_CORRODED, t);
    if (b & (1u << SS_OVERLOADED)) k += stat(l, ST_VS_OVERLOADED, t);
    if (b & 0x1E) k += stat(l, ST_VS_STATUS, t);
    return k;
}

static void deal(Live& l, const Monster& m, float dmg, Vec3 at, bool crit) {
    if (dmg <= 0) return;
    dmg *= status_mult(l, m, now_s());
    bool boss = boss_hit(m.em, dmg, at, crit, false);  // a Remnant II boss: the mod keeps its health
    if (!boss) {
        turf_note_mod_damage(m.em, now_s());
        add_hp(m.health, -dmg);
    }
    Live::Track& tr = l.track[m.em];
    if (tr.max <= 0) { tr.hp = m.hp; tr.max = m.maxHp; tr.large = m.large; }
    tr.hp -= dmg;
    tr.fought = true;
    if (!boss) show_damage(m.em, (int)lroundf(dmg), at, crit);
    l.totalDamage += dmg;
}

static Vec3 sub(Vec3 a, Vec3 b);
static float dot(Vec3 a, Vec3 b);

// Monsters near a point (the box's centre, its size counted).
static void each_near(Live& l, Vec3 c, float radius, void (*fn)(Live&, const Monster&, void*), void* ctx) {
    std::vector<Monster> ms = l.snap.monsters;  // fn may change the list's monsters' health
    for (const Monster& m : ms) {
        Vec3 d = sub(m.center, c);
        float r = radius + std::max(m.half.x, m.half.z) * 0.7f;
        if (dot(d, d) <= r * r) fn(l, m, ctx);
    }
}

// Status damage over time, on monsters and on the hunter.
static void statuses_tick(Live& l, double t) {
    for (auto it = g_aff.begin(); it != g_aff.end();) {
        const Monster* m = nullptr;
        for (const Monster& x : l.snap.monsters)
            if (x.em == it->first) m = &x;
        bool live = false;
        for (int s = 1; s < SS_COUNT; s++) {
            StatusInst& si = it->second.s[s];
            if (si.until <= t) continue;
            live = true;
            if (m && si.perSec > 0 && t >= si.nextTick) {
                float k = tick_of(s);
                si.nextTick += k;
                deal(l, *m, si.perSec * k, m->center, false);
            }
        }
        if (!live || !m) it = g_aff.erase(it);
        else ++it;
    }
    for (int s = 1; s < SS_COUNT; s++) {
        StatusInst& si = g_me.s[s];
        if (si.until <= t || si.perSec <= 0 || t < si.nextTick || !l.snap.playerHealth) continue;
        float k = tick_of(s);
        si.nextTick += k;
        reactions_note_own_damage(t);
        add_hp(l.snap.playerHealth, -si.perSec * k);
    }
}

static void heal_player(Live& l, float amount, double t) {
    if (amount <= 0 || !l.snap.playerHealth) return;
    add_hp(l.snap.playerHealth, amount * (1.0f + stat(l, ST_HEAL, t)));
}

static void lifesteal(Live& l, float damage, double t) {
    float ls = stat(l, ST_LIFESTEAL, t);
    if (ls > 0) heal_player(l, damage * ls * 0.1f, t);  // Remnant lifesteal against MHW-sized health pools
}

static void proc(Live& l, float procDamage, double t) {
    if (procDamage <= 0) return;
    if (const Monster* m = current_target(l))
        deal(l, *m, procDamage * tune(TUNING_DAMAGE_SCALE) * rank_mult(*m) * (1.0f + stat(l, ST_DMG, t) + stat(l, ST_DMG_EXPLOSIVE, t)),
             m->center, false);
}

static void trigger(Live& l, Trig tr, double t) {
    float pd = 0;
    l.fx.fire(tr, t, &pd);
    proc(l, pd, t);
    // statuses this trigger applies (e.g. on_hit:inflict:bleeding) go on the hunter's target
    std::vector<std::pair<const Effect*, float>> inf;
    l.fx.inflicts(tr, t, &inf);
    if (inf.empty()) return;
    const Monster* m = find_monster(l, l.target);
    if (!m) m = current_target(l);
    if (m)
        for (auto& x : inf) inflict(l, *m, x.first->status, x.first->value * x.second, x.first->dur, t);
}

// Area statuses and damage from "use:" effects.
struct AreaUse { const Effect* e; float scale; double t; };
static void area_inflict(Live& l, const Monster& m, void* c) {
    AreaUse* a = (AreaUse*)c;
    inflict(l, m, a->e->status, a->e->value, a->e->dur * a->scale, a->t);
}
static void area_mark(Live& l, const Monster& m, void* c) {
    AreaUse* a = (AreaUse*)c;
    inflict(l, m, SS_MARKED, 0, a->e->dur * a->scale, a->t);
}
static void area_damage(Live& l, const Monster& m, void* c) {
    AreaUse* a = (AreaUse*)c;
    float dmg = a->e->value * a->scale * rank_mult(m);
    if (is_boss(m.em)) boss_hit_ex(m.em, dmg * status_mult(l, m, a->t), m.center, false, false, 2.5f);  // area skills stagger
    else deal(l, m, dmg, m.center, false);
}
static void area_erupt(Live& l, const Monster& m, void* c) {
    AreaUse* a = (AreaUse*)c;
    auto it = g_aff.find(m.em);
    if (it == g_aff.end()) return;
    int n = 0;
    for (int s = SS_BLEEDING; s <= SS_OVERLOADED; s++)
        if (it->second.s[s].until > a->t) { n++; it->second.s[s] = StatusInst(); }  // the eruption uses them up
    if (n) deal(l, m, a->e->value * a->scale * rank_mult(m) * n, m.center, true);
}

// Runs an item's "use:" effects. durScale stretches durations (skill and mod duration bonuses); dmgBonus adds the
// damage stats that apply (skill, mod or relic); defaultDur is used when the effect gives none.
static void apply_use(Live& l, const char* dsl, double t, float durScale, float dmgBonus, float defaultDur, float healMult) {
    const Monster* tgt = current_target(l);
    float rk = tgt ? rank_mult(*tgt) : 1.0f;
    float scale = tune(TUNING_DAMAGE_SCALE) * rk * (1.0f + stat(l, ST_DMG, t) + dmgBonus);
    for (const Effect& e : effects_of(dsl)) {
        float dur = (e.dur > 0 ? e.dur : defaultDur) * durScale;
        switch (e.act) {
            case ACT_HEAL:
                if (e.dur <= 0.05f) heal_player(l, l.maxHp * e.value * healMult, t);
                else l.actives.push_back({ACT_HEAL, l.maxHp * e.value * healMult / dur, t + dur, nullptr, 0});
                break;
            case ACT_SHIELD:
                l.bonusHealth += l.maxHp * e.value;
                l.shieldUntil = std::max(l.shieldUntil, t + dur);
                break;
            case ACT_BURST:
                if (tgt) deal(l, *tgt, e.value * scale, tgt->center, true);
                break;
            case ACT_DOT:
                l.actives.push_back({ACT_DOT, e.value * scale, t + dur, tgt ? tgt->em : nullptr, 0});
                break;
            case ACT_SUMMON:
                l.actives.push_back({ACT_SUMMON, std::max(e.value, tune(TUNING_COMPANION_DPS)) * scale, t + dur, nullptr, 0});
                break;
            case ACT_BUFF:
                l.fx.buff(e.stat, e.value, t + dur, &e, 1);
                break;
            case ACT_AMMO: {
                SaveData& s = S().save;
                for (int g : {s.longGun, s.handGun})
                    if (g >= 0) l.reserve[g] = std::min(reserve_size(l, g, t), l.reserve[g] + (int)(reserve_size(l, g, t) * e.value));
                break;
            }
            case ACT_INFLICT:
            case ACT_MARK:
            case ACT_AOE:
            case ACT_ERUPTION: {
                AreaUse a{&e, e.act == ACT_INFLICT || e.act == ACT_MARK ? durScale
                                                                         : tune(TUNING_DAMAGE_SCALE) * (1.0f + stat(l, ST_DMG, t) + dmgBonus),
                          t};
                auto fn = e.act == ACT_INFLICT ? area_inflict : e.act == ACT_MARK ? area_mark
                        : e.act == ACT_AOE     ? area_damage  : area_erupt;
                if (e.radius <= 0) {  // the target only
                    if (tgt) fn(l, *tgt, &a);
                } else {
                    Vec3 c = e.atTarget && tgt ? tgt->center : l.snap.pos;
                    each_near(l, c, e.radius * 100.0f, fn, &a);
                }
                break;
            }
            case ACT_IMBUE:
                l.imbues.push_back({e.status, e.value, e.dur, t + e.span * durScale});
                break;
            case ACT_CURE:
                cure_player(e.status);
                break;
            case ACT_GRENADE:
                if (tgt)
                    for (const Monster& m : l.snap.monsters) {
                        Vec3 d = sub(m.center, tgt->center);
                        if (dot(d, d) < 600.0f * 600.0f)
                            deal(l, m, e.value * scale * (1.0f + stat(l, ST_DMG_EXPLOSIVE, t)), m.center, false);
                    }
                break;
            default: break;
        }
    }
}

static int fire(SaveData& s, Live& l, int gun, double t) {
    const Weapons& w = kWeapons[gun];
    const GameView& v = l.snap.view;
    Vec3 fwd = norm(sub(v.camTarget, v.camPos));
    Vec3 up = {0, 1, 0};
    Vec3 right = norm(cross(fwd, up));
    Vec3 up2 = cross(right, fwd);
    // Start past the hunter so the ray never hits the hunter or the space behind.
    float tMin = std::max(0.0f, dot(sub(l.snap.pos, v.camPos), fwd));
    Vec3 origin = add(v.camPos, mul(fwd, tMin));
    float range = w.range_m * 100.0f;  // MHW units are centimetres
    float spread = w.spread_deg * (l.aiming ? tune(TUNING_ADS_SPREAD_MULT) : 1.0f) *
                   std::max(0.2f, 1.0f - stat(l, ST_ACCURACY, t)) * 3.14159265f / 180.0f;
    float base = w.damage * tune(TUNING_DAMAGE_SCALE) * upgrade_mult(s.level[gun]) *
                 (1.0f + stat(l, ST_DMG, t) + stat(l, ST_DMG_RANGED, t));
    float critChance = w.crit / 100.0f + stat(l, ST_CRIT, t);
    float critMult = tune(TUNING_CRIT_MULT) + stat(l, ST_CRIT_DMG, t);
    float weakMult = w.weakspot_mult + stat(l, ST_WEAKSPOT, t);
    struct Acc { const Monster* m; float dmg; Vec3 at; bool crit, weak; };
    Acc acc[16];
    int nacc = 0;
    int pellets = std::max(1, (int)w.pellets);
    for (int p = 0; p < pellets; p++) {
        float ang = spread * sqrtf(frand()), rot = frand() * 6.2831853f;
        Vec3 d = norm(add(fwd, add(mul(right, tanf(ang) * cosf(rot)), mul(up2, tanf(ang) * sinf(rot)))));
        const Monster* best = nullptr;
        float bestT = 2.0f * range;
        float bestLp[3] = {};
        for (const Monster& m : l.snap.monsters) {
            if (!m.obbOk) continue;
            float th, lp[3];
            if (ray_obb(m, origin, d, &th, lp) && th < bestT) { best = &m; bestT = th; memcpy(bestLp, lp, 12); }
        }
        if (!best) continue;
        Vec3 at = add(origin, mul(d, bestT));
        Vec3 wall;
        if (world_ray(origin, at, &wall) && len(sub(wall, origin)) + 50.0f < bestT) continue;  // a wall in the way
        // Weakspot: the ends of the body's long horizontal axis, upper half (heads and tails).
        int longAxis = best->half.x > best->half.z ? 0 : 2;
        float h[3] = {best->half.x, best->half.y, best->half.z};
        bool weak = fabsf(bestLp[longAxis]) > tune(TUNING_WEAKSPOT_BAND) * h[longAxis] && at.y > best->center.y;
        bool crit = frand() < critChance + ((status_bits(best->em) & (1u << SS_MARKED)) ? 0.10f : 0.0f);
        float dmg = base * rank_mult(*best) * (weak ? weakMult : 1.0f) * (crit ? critMult : 1.0f);
        if (bestT > range) dmg *= std::max(0.5f, 1.0f - 0.5f * (bestT - range) / range);  // falloff
        int k = 0;
        while (k < nacc && acc[k].m != best) k++;
        if (k == nacc && nacc < 16) acc[nacc++] = {best, 0, at, false, false};
        if (k < nacc) {
            acc[k].dmg += dmg;
            acc[k].crit |= crit;
            acc[k].weak |= weak;
        }
    }
    l.shots++;
    audio_play(w.snd_fire, 1.0f);
    int mod = gun_mod(s, gun);
    for (int k = 0; k < nacc; k++) {
        deal(l, *acc[k].m, acc[k].dmg, acc[k].at, acc[k].crit || acc[k].weak);
        for (const Live::Imbue& im : l.imbues)  // ammo mods: hits apply their status
            if (t < im.until) inflict(l, *acc[k].m, im.status, im.total, im.dur, t);
        lifesteal(l, acc[k].dmg, t);
        l.hits++;
        l.hitMarkerUntil = t + 0.18;
        l.hitMarkerWeak = acc[k].weak;
        l.tracers[l.nextTracer++ % 16] = {acc[k].at, t + 0.09, true};
        if (mod >= 0 && t >= l.modUntil)
            l.modCharge = std::min(kMods[mod].charge_damage, l.modCharge + acc[k].dmg * (1.0f + stat(l, ST_MOD_GEN, t)));
        if (tune(TUNING_AGGRO_SHELLS) > 0 && t - l.lastSlinger > tune(TUNING_SLINGER_INTERVAL_S) && !is_boss(acc[k].m->em)) {
            Vec3 back = norm(sub(v.camPos, acc[k].at));
            fire_slinger(add(acc[k].at, mul(back, 150.0f)), acc[k].m->center);
            l.lastSlinger = t;
        }
        if (k == 0) l.target = acc[k].m->em;
        trigger(l, TR_ON_HIT, t);
        if (acc[k].crit) trigger(l, TR_ON_CRIT, t);
        if (acc[k].weak) trigger(l, TR_ON_WEAKSPOT, t);
    }
    return nacc;
}

// ---------------------------------------------------------------- requests from the menus (crafting)

static const char* material_name(int m) {
    int id = (int)kMaterials[m].mhw_item_id;
    const char* n = id > 0 ? mhw_item_name(id) : nullptr;
    return n ? n : kMaterials[m].note;
}

static void handle(SaveData& s, Live& l, const Request& r, double t) {
    ItemView it = item(r.cat, r.row);
    const char* recipe = it.recipe;
    int zen = it.zenny;
    if (r.kind == Request::UPGRADE) {
        int lvl = s.level[r.row];
        if (lvl >= kUpgradesCount) { snprintf(l.said, sizeof(l.said), "Already at the highest level."); return; }
        recipe = kUpgrades[lvl].recipe;
        zen = (int)kUpgrades[lvl].zenny;
    } else if (r.kind == Request::CRAFT && r.cat != CAT_CONSUMABLE && s.has(r.cat, r.row)) {
        snprintf(l.said, sizeof(l.said), "You already have this.");
        return;
    }
    std::vector<Need> need = r.kind == Request::BUY_CONSUMABLE ? std::vector<Need>{} : recipe_of(recipe);
    if (!mhw_inventory_ready() && !need.empty()) {
        snprintf(l.said, sizeof(l.said), "Your item box can't be read yet, so nothing was taken or made.");
        return;
    }
    for (const Need& n : need) {
        int id = (int)kMaterials[n.material].mhw_item_id;
        if (id <= 0) { snprintf(l.said, sizeof(l.said), "This recipe's MHW materials are not set up yet."); return; }
        if (mhw_item_count(id) < n.count) {
            snprintf(l.said, sizeof(l.said), "You need %d x %s.", n.count, material_name(n.material));
            return;
        }
    }
    if (mhw_zenny() < zen) { snprintf(l.said, sizeof(l.said), "You need %d zenny.", zen); return; }
    for (const Need& n : need)
        if (!mhw_take_items((int)kMaterials[n.material].mhw_item_id, n.count)) {
            snprintf(l.said, sizeof(l.said), "Could not take %s from your item box.", material_name(n.material));
            log("craft: taking item %d x%d failed", (int)kMaterials[n.material].mhw_item_id, n.count);
            return;
        }
    if (zen > 0 && !mhw_spend_zenny(zen)) { snprintf(l.said, sizeof(l.said), "Could not take the zenny."); return; }
    switch (r.kind) {
        case Request::UPGRADE: s.level[r.row]++; break;
        case Request::BUY_CONSUMABLE: s.stock[r.row]++; break;
        default:
            if (r.cat == CAT_CONSUMABLE) s.stock[r.row]++;
            else s.owned[r.cat][r.row] = 1;
            if (r.cat == CAT_WEAPON) { l.mag[r.row] = mag_size(l, r.row, t); l.reserve[r.row] = reserve_size(l, r.row, t); }
            break;
    }
    snprintf(l.said, sizeof(l.said), "done");
    l.dirty = true;
    log("craft: %s %s row %d (%d zenny)", r.kind == Request::UPGRADE ? "upgrade" : "made", cat_label(r.cat), r.row, zen);
}

void use_consumable(int row) {
    Shared& sh = S();
    AcquireSRWLockExclusive(&sh.lock);
    SaveData& s = sh.save;
    Live& l = sh.live;
    double t = now_s();
    if (row >= 0 && s.stock[row] > 0) {
        s.stock[row]--;
        apply_use(l, kConsumables[row].effects, t, 1.0f, 0.0f, 10.0f, 1.0f);
        audio_play(kConsumables[row].snd_use);
        bool concoction = false;
        for (const Effect& e : effects_of(kConsumables[row].effects))
            concoction |= e.act == ACT_BUFF;
        if (concoction) {
            // Concoctions (lasting buffs) keep going for up to half an hour, like Remnant II's.
            s.concoction = row;
            for (const Effect& e : effects_of(kConsumables[row].effects))
                if (e.act == ACT_BUFF) l.fx.buff(e.stat, e.value, t + tune(TUNING_CONCOCTION_S), &e, 1);
        }
        l.dirty = true;
    }
    ReleaseSRWLockExclusive(&sh.lock);
}

// ---------------------------------------------------------------- the tick

static double g_lastTick = 0, g_lastSave = 0, g_lastMeleeHit = 0;

void mod_game_tick() {
    Shared& sh = S();
    double t = now_s();
    float dt = g_lastTick > 0 ? (float)std::min(0.25, t - g_lastTick) : 0.016f;
    g_lastTick = t;
    inventory_tick();
    AcquireSRWLockExclusive(&sh.lock);
    SaveData& s = sh.save;
    Live& l = sh.live;
    snapshot(&l.snap);
    int bossEv = bosses_tick(s, l, t, dt);  // Remnant II bosses join the monsters here
    turf_tick(l, t, dt);                    // turf wars between monsters (and the MHW side of the bosses')
    set_first_person(l.gunMode && l.aiming && l.snap.inGame);  // aiming down sights is first person
    stamina_tick(l, t);                     // Remnant's stamina bonuses on MHW's stamina bar
    research_tick(l, t);
    reactions_tick(l, t);
    gunmodel_tick(s, l);
    const GameSnapshot& g = l.snap;
    const Archetypes* a = archetype();
    if (l.loadoutChanged) {
        rebuild_sources(s, l);
        l.loadoutChanged = false;
        // Decode the loadout's sounds ahead of the first shot.
        for (int g : {s.longGun, s.handGun})
            if (g >= 0) {
                audio_prefetch(kWeapons[g].snd_fire);
                audio_prefetch(kWeapons[g].snd_reload);
                int md = gun_mod(s, g);
                if (md >= 0) audio_prefetch(kMods[md].snd_use);
            }
        for (int slot = 0; slot < 2; slot++)
            for (int k : skills_of(slot == 0 ? s.archetype : s.archetype2)) audio_prefetch(kSkills[k].snd_use);
        if (s.relic >= 0) audio_prefetch(kRelics[s.relic].snd_use);
    }
    l.fx.expire(t);
    l.imbues.erase(std::remove_if(l.imbues.begin(), l.imbues.end(), [&](const Live::Imbue& x) { return t >= x.until; }),
                   l.imbues.end());
    statuses_tick(l, t);
    l.fx.inHand = l.gunMode ? (l.usingLong ? 1 : 2) : 0;  // a gun's own effects count while it is in hand

    // Zone changes: back at base, or setting out on a hunt, is Remnant's rest at a worldstone.
    if (g.zone != l.lastZone && g.zone >= 0) {
        if (l.lastZone >= 0 && (g.inBase || zone_is_base(l.lastZone))) {
            rest(s, l, t);
            toast(l, "Rested: relic charges and ammo restored");
        }
        log("zone %d -> %d (%s)", l.lastZone, g.zone, g.inBase ? "base" : "field");
        l.lastZone = g.zone;
        l.track.clear();
        l.lastPlayerHp = -1;
        l.actives.clear();
    }
    l.combatAllowed = g.inGame && a && (!g.inBase || g.zone == 504) && g.solo;
    if (!l.combatAllowed) l.gunMode = false;

    // The NPC menu the hunter stands at.
    l.nearMenu = -1;
    if (g.inGame && g.inBase)
        for (int i = 0; i < kMenusCount; i++) {
            const Menus& m = kMenus[i];
            if ((int)m.zone != g.zone || (m.x == 0 && m.y == 0 && m.z == 0)) continue;
            float dx = g.pos.x - m.x, dy = g.pos.y - m.y, dz = g.pos.z - m.z;
            if (dx * dx + dy * dy + dz * dz < m.radius * m.radius) l.nearMenu = i;
        }

    for (const Request& r : l.requests) handle(s, l, r, t);
    if (!l.requests.empty()) {
        l.requests.clear();
        l.loadoutChanged = true;
    }

    LONG cmds = InterlockedExchange(&g_cmds, 0);
    auto has = [&](Cmd c) { return (cmds & (1 << c)) != 0; };

    if (g.inGame && a) {
        // ---- player health: damage taken, damage reduction, shields, regeneration
        float hpNow = 0;
        if (g.playerHealth && mem_readable(g.playerHealth + 0x60, 8)) {
            l.maxHp = *(float*)(g.playerHealth + 0x60);
            hpNow = *(float*)(g.playerHealth + 0x64);
        }
        l.fx.hpFrac = l.maxHp > 0 ? hpNow / l.maxHp : 1.0f;
        float mh = stat(l, ST_MAX_HP, t);
        l.bonusMax = std::max(0.0f, mh);
        if (t > l.shieldUntil && l.bonusHealth > l.bonusMax) l.bonusHealth = l.bonusMax;
        if (g.playerHealth && l.lastPlayerHp >= 0) {
            float drop = l.lastPlayerHp - hpNow;
            if (drop > 0.5f && drop < 1000.0f) {
                float dr = std::min(tune(TUNING_DR_CAP), std::max(0.0f, stat(l, ST_DR, t)));
                float refund = drop * dr;
                float absorbed = std::min(l.bonusHealth, drop - refund);
                if (absorbed > 0) { refund += absorbed; l.bonusHealth -= absorbed; }
                if (refund > 0) add_hp(g.playerHealth, refund);
                l.lastHurt = t;
                trigger(l, TR_ON_HURT, t);
            }
        }
        float regen = stat(l, ST_REGEN, t);
        if (regen > 0 && g.playerHealth) {
            l.regenAcc += regen * dt;
            if (l.regenAcc >= 1.0) { add_hp(g.playerHealth, (float)l.regenAcc); l.regenAcc = 0; }
        }
        if (t - l.lastHurt > tune(TUNING_BONUS_HEALTH_REGEN_DELAY_S) && l.bonusHealth < l.bonusMax)
            l.bonusHealth = std::min(l.bonusMax, l.bonusHealth + tune(TUNING_BONUS_HEALTH_REGEN_PER_S) * dt);

        // ---- things running for a while
        for (Active& ac : l.actives) {
            if (t >= ac.until) continue;
            if (ac.act == ACT_HEAL) {
                if (g.playerHealth) add_hp(g.playerHealth, ac.perSec * dt * (1.0f + stat(l, ST_HEAL, t)));
                continue;
            }
            const Monster* m = ac.target ? find_monster(l, ac.target) : current_target(l);
            if (!m) continue;
            ac.acc += ac.perSec * dt;
            if (ac.acc >= 5.0f) {
                deal(l, *m, ac.acc, m->center, false);
                ac.acc = 0;
            }
        }
        l.actives.erase(std::remove_if(l.actives.begin(), l.actives.end(), [&](const Active& x) { return t >= x.until; }),
                        l.actives.end());
        l.lastPlayerHp = g.playerHealth ? hpNow : -1;
        if (bossEv & BOSS_EV_EVADE) trigger(l, TR_ON_EVADE, t);  // dodged through a boss's hit

        // ---- relic
        if (has(CMD_RELIC) && s.relic >= 0) {
            if (s.relicCharges > 0 && t >= l.relicBusyUntil && g.playerHealth) {
                s.relicCharges--;
                float speed = 1.0f + stat(l, ST_RELIC_SPEED, t);
                apply_use(l, kRelics[s.relic].effects, t, 1.0f / std::max(0.2f, speed), 0.0f, 5.0f,
                          1.0f + stat(l, ST_RELIC_HEAL, t));
                l.relicBusyUntil = t + 0.8 / speed;
                audio_play(kRelics[s.relic].snd_use);
                trigger(l, TR_ON_RELIC, t);
                l.dirty = true;
            } else if (s.relicCharges <= 0) {
                toast(l, "No relic charges left");
            }
        }

        // ---- archetype skills (primary on the skill key, secondary on the second skill key)
        for (int slot = 0; slot < 2; slot++) {
            if (!has(slot == 0 ? CMD_SKILL : CMD_SKILL2)) continue;
            int ar = slot == 0 ? s.archetype : s.archetype2;
            std::vector<int> sk = skills_of(ar);
            if (sk.empty()) continue;
            const Skills& k = kSkills[sk[std::min((int)sk.size() - 1, std::max(0, s.skill[slot]))]];
            if (t < l.skillReady[slot]) continue;
            float durScale = 1.0f + stat(l, ST_SKILL_DUR, t);
            float cdr = std::min(1.0f - tune(TUNING_SKILL_COOLDOWN_FLOOR), std::max(0.0f, stat(l, ST_CDR, t)));
            l.skillUntil[slot] = t + k.duration_s * durScale;
            l.skillReady[slot] = t + k.cooldown_s * (1.0f - cdr);
            apply_use(l, k.effects, t, durScale, stat(l, ST_DMG_SKILL, t), k.duration_s, 1.0f);
            audio_play(k.snd_use);
            trigger(l, TR_ON_SKILL, t);
            log("skill %s", k.id);
        }

        // ---- MHW weapon hits and monster deaths -> Remnant effects
        float meleeBonus = stat(l, ST_DMG, t) + stat(l, ST_DMG_MELEE, t);
        int curGun = equipped_gun(s, l);
        int curMod = gun_mod(s, curGun);
        for (const Monster& m : g.monsters) {
            auto it = l.track.find(m.em);
            if (it != l.track.end()) {
                float ext = it->second.hp - m.hp;
                if (ext > 0.5f && ext < 100000.0f) {
                    float bonus = ext * meleeBonus;
                    if (bonus > 0.5f && !boss_hit(m.em, bonus, m.center, false, false)) {
                        turf_note_mod_damage(m.em, t);
                        add_hp(m.health, -bonus);
                    }
                    lifesteal(l, ext + bonus, t);
                    if (curMod >= 0 && t >= l.modUntil)
                        l.modCharge = std::min(kMods[curMod].charge_damage, l.modCharge + ext * (1.0f + stat(l, ST_MOD_GEN, t)));
                    it->second.hp = m.hp - std::max(0.0f, bonus);
                    it->second.fought = true;
                    l.target = m.em;
                    if (t - g_lastMeleeHit > 0.25) {
                        trigger(l, TR_ON_HIT, t);
                        g_lastMeleeHit = t;
                    }
                    continue;
                }
                it->second.hp = m.hp;
            } else {
                Live::Track tr;
                tr.hp = m.hp;
                tr.max = m.maxHp;
                tr.large = m.large;
                l.track[m.em] = tr;
            }
        }
        for (auto it = l.track.begin(); it != l.track.end();) {
            if (!find_monster(l, it->first)) {
                // A monster we fought left the list low on health: slain or captured.
                const Live::Track& tr = it->second;
                if (tr.fought && tr.max > 0 && tr.hp < 0.25f * tr.max) {
                    trigger(l, TR_ON_KILL, t);
                    // experience by the monster's size; small monsters a little
                    award_xp(s, l, std::max(60.0f, tr.max * tune(TUNING_XP_PER_HP) * (tr.large ? 1.0f : 0.5f)), t);
                    l.loadoutChanged = true;  // a level or a prism choice may change the numbers
                }
                it = l.track.erase(it);
            } else {
                ++it;
            }
        }

        // ---- weapon mod
        if (has(CMD_MOD) && curMod >= 0 && l.combatAllowed) {
            const Mods& md = kMods[curMod];
            if (l.modCharge >= md.charge_damage && t >= l.modUntil) {
                l.modCharge = 0;
                float durScale = 1.0f + stat(l, ST_MOD_DUR, t);
                l.modUntil = t + 10.0 * durScale;
                apply_use(l, md.effects, t, durScale, stat(l, ST_DMG_MOD, t) + stat(l, ST_DMG_ELEM, t), 10.0f, 1.0f);
                audio_play(md.snd_use);
                trigger(l, TR_ON_MOD, t);
                log("mod %s", md.id);
            }
        }

        // ---- guns
        if (has(CMD_SWAP) && l.gunMode) { l.usingLong = !l.usingLong; l.reloadDone = 0; }
        int gun = equipped_gun(s, l);
        if (gun >= 0) {
            const Weapons& w = kWeapons[gun];
            float reloadTime = w.reload_s / std::max(0.2f, 1.0f + stat(l, ST_RELOAD, t));
            int magMax = mag_size(l, gun, t);
            if (l.reloadDone > 0 && t >= l.reloadDone) {
                int need = magMax - l.mag[gun];
                int take = std::min(need, l.reserve[gun]);
                l.mag[gun] += take;
                l.reserve[gun] -= take;
                l.reloadDone = 0;
                trigger(l, TR_ON_RELOAD, t);
            }
            bool wantReload = has(CMD_RELOAD) || (l.mag[gun] == 0 && l.reserve[gun] > 0);
            if (l.gunMode && wantReload && l.reloadDone == 0 && l.mag[gun] < magMax && l.reserve[gun] > 0) {
                l.reloadDone = t + reloadTime;
                audio_play(w.snd_reload, 0.8f);
            }
            bool wasAiming = l.aiming;
            l.aiming = l.gunMode && key_down(VK_RBUTTON);
            if (!l.aiming && wasAiming && l.baseFov > 5.0f) {
                set_camera_fov(l.baseFov);  // one tick to put the game's own FOV back
            } else if (!l.aiming) {
                if (g.view.valid) l.baseFov = g.view.fovY;
                set_camera_fov(0.0f);
            } else if (l.baseFov > 5.0f) {
                set_camera_fov(l.baseFov * tune(TUNING_ADS_FOV_MULT));
            }
            if (l.gunMode && g.view.valid && key_down(VK_LBUTTON) && l.reloadDone == 0 && t >= l.nextShot && l.mag[gun] > 0) {
                int nacc_last = fire(s, l, gun, t);
                l.mag[gun]--;
                float rpm = w.rpm * (1.0f + stat(l, ST_FIRE_RATE, t));
                l.nextShot = t + 60.0 / std::max(1.0f, rpm);
                l.muzzleUntil = t + 0.05;
                if (nacc_last == 0 && g.view.valid) {
                    Vec3 fwd = norm(sub(g.view.camTarget, g.view.camPos));
                    l.tracers[l.nextTracer++ % 16] = {add(g.view.camPos, mul(fwd, w.range_m * 100.0f)), t + 0.07, false};
                }
                if (test_mode() && l.hits > 0 && !l.shotLogged) {
                    melty_log("check shot-landed ok %.0f-damage", l.totalDamage);
                    l.shotLogged = true;
                }
            }
        }
    }
    if (l.dirty && t - g_lastSave > 3.0) {
        save_write(s);
        l.dirty = false;
        g_lastSave = t;
    }
    bool inGameNow = g.inGame, inBaseNow = g.inBase;
    int zoneNow = g.zone;
    Vec3 posNow = g.pos;
    ReleaseSRWLockExclusive(&sh.lock);
    devconsole_tick(t, inGameNow, inBaseNow, zoneNow, posNow);
}

// ---------------------------------------------------------------- save file

static std::string save_path() {
    char dir[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof(dir))) snprintf(dir, sizeof(dir), "%s", mod_dir());
    std::string d = std::string(dir) + "\\RemnantHunt";
    CreateDirectoryA(d.c_str(), nullptr);
    return d + "\\save.json";
}

static const char* kCatKeys[CAT_COUNT] = {"weapons", "mods", "mutators", "armor", "archetypes", "rings",
                                          "amulets", "relics", "fragments", "consumables", "traits", "prisms"};

// A prism in the save: "id|xp|legendary|slot,slot,...|offer,offer" with slots "f.fragment.lv" / "u.fusion.lv" / "-"
// and offers "f.fragment" / "u.fusion" / "l.legendary" / "r.slot".
static std::string prism_str(int row, const SaveData::Prism& p) {
    char b[64];
    snprintf(b, sizeof(b), "%s|%.0f|%s|", kPrisms[row].id, p.xp, p.legendary >= 0 ? kLegendaries[p.legendary].id : "");
    std::string s = b;
    for (int i = 0; i < 5; i++) {
        const SaveData::PSlot& sl = p.slot[i];
        if (i) s += ",";
        if (sl.kind == PS_FRAGMENT) s += std::string("f.") + kFragments[sl.row].id + "." + std::to_string(sl.lv);
        else if (sl.kind == PS_FUSION) s += std::string("u.") + kFusions[sl.row].id + "." + std::to_string(sl.lv);
        else s += "-";
    }
    s += "|";
    for (int i = 0; i < p.offers; i++) {
        const SaveData::PSlot& o = p.offer[i];
        if (i) s += ",";
        if (o.kind == PS_FRAGMENT) s += std::string("f.") + kFragments[o.row].id;
        else if (o.kind == PS_FUSION) s += std::string("u.") + kFusions[o.row].id;
        else if (o.kind == PS_LEGENDARY) s += std::string("l.") + kLegendaries[o.row].id;
        else s += "r." + std::to_string(o.row);
    }
    return s;
}

static std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> out;
    size_t a = 0;
    while (true) {
        size_t b = s.find(c, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

static void prism_parse(SaveData& s, const std::string& str) {
    std::vector<std::string> f = split(str, '|');
    if (f.size() < 5) return;
    int row = prisms_index(f[0].c_str());
    if (row < 0) return;
    SaveData::Prism p;
    p.xp = atof(f[1].c_str());
    p.legendary = legendaries_index(f[2].c_str());
    std::vector<std::string> sl = split(f[3], ',');
    for (int i = 0; i < 5 && i < (int)sl.size(); i++) {
        std::vector<std::string> k = split(sl[i], '.');
        if (k.size() != 3) continue;
        int r = k[0] == "f" ? fragments_index(k[1].c_str()) : k[0] == "u" ? fusions_index(k[1].c_str()) : -1;
        if (r >= 0) p.slot[i] = {k[0] == "f" ? PS_FRAGMENT : PS_FUSION, (int16_t)r, (int8_t)std::max(1, std::min(10, atoi(k[2].c_str())))};
    }
    for (const std::string& o : split(f[4], ',')) {
        std::vector<std::string> k = split(o, '.');
        if (k.size() != 2 || p.offers >= 3) continue;
        SaveData::PSlot x;
        if (k[0] == "f") x = {PS_FRAGMENT, (int16_t)fragments_index(k[1].c_str()), 0};
        else if (k[0] == "u") x = {PS_FUSION, (int16_t)fusions_index(k[1].c_str()), 0};
        else if (k[0] == "l") x = {PS_LEGENDARY, (int16_t)legendaries_index(k[1].c_str()), 0};
        else if (k[0] == "r") x = {PS_RAISE, (int16_t)atoi(k[1].c_str()), 0};
        if (x.row >= 0) p.offer[p.offers++] = x;
    }
    s.prism[row] = p;
}

static std::string q(const char* s) { return std::string("\"") + (s ? s : "") + "\""; }
static std::string id_or_empty(Cat c, int row) { return q(row >= 0 ? item(c, row).id : ""); }

void save_write(const SaveData& s) {
    std::string j = "{\n  \"version\": 2,\n";
    j += std::string("  \"introDone\": ") + (s.introDone ? "true" : "false") + ",\n";
    j += "  \"archetype\": " + id_or_empty(CAT_ARCHETYPE, s.archetype) + ",\n";
    j += "  \"archetype2\": " + id_or_empty(CAT_ARCHETYPE, s.archetype2) + ",\n";
    j += "  \"skill\": " + std::to_string(s.skill[0]) + ",\n  \"skill2\": " + std::to_string(s.skill[1]) + ",\n";
    j += "  \"relicCharges\": " + std::to_string(s.relicCharges) + ",\n";
    j += "  \"xp\": " + std::to_string((long long)s.xp) + ",\n";
    j += "  \"difficulty\": " + q(kDifficulties[std::max(0, std::min(kDifficultiesCount - 1, s.difficulty))].id) + ",\n";
    j += "  \"prismOn\": " + id_or_empty(CAT_PRISM, s.prismOn) + ",\n";
    j += "  \"traitPoints\": {";
    for (int i = 0, n = 0; i < kTraitsCount; i++)
        if (s.trait[i]) j += (n++ ? ", " : "") + q(kTraits[i].id) + ": " + std::to_string(s.trait[i]);
    j += "},\n  \"prisms\": [";
    for (int i = 0, n = 0; i < kPrismsCount; i++)
        if (s.has(CAT_PRISM, i)) j += (n++ ? ", " : "") + q(prism_str(i, s.prism[i]).c_str());
    j += "],\n";
    j += "  \"long\": " + id_or_empty(CAT_WEAPON, s.longGun) + ",\n";
    j += "  \"hand\": " + id_or_empty(CAT_WEAPON, s.handGun) + ",\n";
    j += "  \"melee\": " + id_or_empty(CAT_WEAPON, s.melee) + ",\n";
    j += "  \"modLong\": " + id_or_empty(CAT_MOD, s.gunMod[0]) + ",\n  \"modHand\": " + id_or_empty(CAT_MOD, s.gunMod[1]) + ",\n";
    j += "  \"relic\": " + id_or_empty(CAT_RELIC, s.relic) + ",\n  \"amulet\": " + id_or_empty(CAT_AMULET, s.amulet) + ",\n";
    auto arr = [&](const char* key, Cat c, const int* v, int n) {
        j += std::string("  \"") + key + "\": [";
        for (int i = 0; i < n; i++) j += (i ? ", " : "") + id_or_empty(c, v[i]);
        j += "],\n";
    };
    arr("mutators", CAT_MUTATOR, s.mutator, 3);
    arr("fragmentSlots", CAT_FRAGMENT, s.fragment, 3);
    arr("ringSlots", CAT_RING, s.ring, 4);
    arr("armorSlots", CAT_ARMOR, s.armor, 4);
    for (int c = 0; c < CAT_COUNT; c++) {
        if (c == CAT_CONSUMABLE) continue;
        j += std::string("  \"owned_") + kCatKeys[c] + "\": [";
        bool first = true;
        for (int i = 0; i < (int)s.owned[c].size(); i++)
            if (s.owned[c][i]) { j += (first ? "" : ", ") + q(item((Cat)c, i).id); first = false; }
        j += "],\n";
    }
    j += "  \"levels\": {";
    bool first = true;
    for (int i = 0; i < kWeaponsCount; i++)
        if (s.level[i]) { j += (first ? "" : ", ") + q(kWeapons[i].id) + ": " + std::to_string(s.level[i]); first = false; }
    j += "},\n  \"stock\": {";
    first = true;
    for (int i = 0; i < kConsumablesCount; i++)
        if (s.stock[i]) { j += (first ? "" : ", ") + q(kConsumables[i].id) + ": " + std::to_string(s.stock[i]); first = false; }
    j += "}\n}\n";
    std::string p = save_path(), tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fwrite(j.data(), 1, j.size(), f);
    fclose(f);
    MoveFileExA(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// Tiny readers for the file we write ourselves.
static size_t value_at(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return p;
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return p;
    p++;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\n')) p++;
    return p;
}
static std::string json_str(const std::string& j, const char* key) {
    size_t p = value_at(j, key);
    if (p == std::string::npos || p >= j.size()) return "";
    if (j[p] == '"') {
        size_t e = j.find('"', p + 1);
        return j.substr(p + 1, e - p - 1);
    }
    size_t e = j.find_first_of(",}\n", p);
    return j.substr(p, e - p);
}
static std::vector<std::string> json_strings(const std::string& j, const char* key) {
    std::vector<std::string> out;
    size_t p = value_at(j, key);
    if (p == std::string::npos || j[p] != '[') return out;
    size_t e = j.find(']', p);
    for (size_t a = j.find('"', p); a != std::string::npos && a < e;) {
        size_t b = j.find('"', a + 1);
        out.push_back(j.substr(a + 1, b - a - 1));
        a = j.find('"', b + 1);
    }
    return out;
}
static std::vector<std::pair<std::string, int>> json_counts(const std::string& j, const char* key) {
    std::vector<std::pair<std::string, int>> out;
    size_t p = value_at(j, key);
    if (p == std::string::npos || j[p] != '{') return out;
    size_t e = j.find('}', p);
    for (size_t a = j.find('"', p); a != std::string::npos && a < e;) {
        size_t b = j.find('"', a + 1);
        size_t c = j.find(':', b);
        out.push_back({j.substr(a + 1, b - a - 1), atoi(j.c_str() + c + 1)});
        a = j.find('"', c);
    }
    return out;
}
static int row_by_id(Cat c, const std::string& id) {
    if (id.empty()) return -1;
    for (int i = 0; i < cat_count(c); i++)
        if (id == item(c, i).id) return i;
    return -1;
}

void save_load() {
    SaveData s;
    std::string p = save_path();
    FILE* f = fopen(p.c_str(), "rb");
    if (f) {
        std::string j;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) j.append(buf, n);
        fclose(f);
        int version = atoi(json_str(j, "version").c_str());
        s.introDone = json_str(j, "introDone") == "true";
        s.archetype = row_by_id(CAT_ARCHETYPE, json_str(j, "archetype"));
        s.archetype2 = row_by_id(CAT_ARCHETYPE, json_str(j, "archetype2"));
        s.skill[0] = atoi(json_str(j, "skill").c_str());
        s.skill[1] = atoi(json_str(j, "skill2").c_str());
        s.longGun = row_by_id(CAT_WEAPON, json_str(j, "long"));
        s.handGun = row_by_id(CAT_WEAPON, json_str(j, "hand"));
        s.melee = row_by_id(CAT_WEAPON, json_str(j, "melee"));
        if (version >= 2) {
            s.relicCharges = atoi(json_str(j, "relicCharges").c_str());
            s.gunMod[0] = row_by_id(CAT_MOD, json_str(j, "modLong"));
            s.gunMod[1] = row_by_id(CAT_MOD, json_str(j, "modHand"));
            s.relic = row_by_id(CAT_RELIC, json_str(j, "relic"));
            s.amulet = row_by_id(CAT_AMULET, json_str(j, "amulet"));
            auto slots = [&](const char* key, Cat c, int* v, int n) {
                std::vector<std::string> ids = json_strings(j, key);
                for (int i = 0; i < n && i < (int)ids.size(); i++) v[i] = row_by_id(c, ids[i]);
            };
            slots("mutators", CAT_MUTATOR, s.mutator, 3);
            slots("fragmentSlots", CAT_FRAGMENT, s.fragment, 3);
            slots("ringSlots", CAT_RING, s.ring, 4);
            slots("armorSlots", CAT_ARMOR, s.armor, 4);
            for (int c = 0; c < CAT_COUNT; c++) {
                if (c == CAT_CONSUMABLE) continue;
                for (const std::string& id : json_strings(j, (std::string("owned_") + kCatKeys[c]).c_str())) {
                    int r = row_by_id((Cat)c, id);
                    if (r >= 0) s.owned[c][r] = 1;
                }
            }
            for (auto& kv : json_counts(j, "levels")) {
                int r = row_by_id(CAT_WEAPON, kv.first);
                if (r >= 0) s.level[r] = std::min((int)kUpgradesCount, kv.second);
            }
            for (auto& kv : json_counts(j, "stock")) {
                int r = row_by_id(CAT_CONSUMABLE, kv.first);
                if (r >= 0) s.stock[r] = kv.second;
            }
            s.xp = atof(json_str(j, "xp").c_str());
            int dfc = difficulties_index(json_str(j, "difficulty").c_str());
            if (dfc >= 0) s.difficulty = dfc;
            s.prismOn = row_by_id(CAT_PRISM, json_str(j, "prismOn"));
            for (auto& kv : json_counts(j, "traitPoints")) {
                int r = row_by_id(CAT_TRAIT, kv.first);
                if (r >= 0) s.trait[r] = (uint8_t)std::max(0, std::min(10, kv.second));
            }
            for (const std::string& p : json_strings(j, "prisms")) prism_parse(s, p);
            for (int i = 0; i < kTraitsCount; i++)
                if (kTraits[i].kind == TRAITS_KIND_CORE) s.owned[CAT_TRAIT][i] = 1;
            while (trait_points_spent(s) > trait_points(s)) traits_reset(s);  // the rules changed: points back
        } else {
            // v0.1 save: guns with McCabe levels (v0.1 had 5; v0.2 keeps them), Dragon Heart charges.
            s.relicCharges = atoi(json_str(j, "relic").c_str());
            for (auto& kv : json_counts(j, "guns")) {
                int r = row_by_id(CAT_WEAPON, kv.first);
                if (r >= 0) { s.owned[CAT_WEAPON][r] = 1; s.level[r] = kv.second; }
            }
            if (s.archetype >= 0) {
                s.owned[CAT_ARCHETYPE][s.archetype] = 1;
                int ml = kArchetypes[s.archetype].starter_melee;
                if (ml >= 0) { s.owned[CAT_WEAPON][ml] = 1; s.melee = ml; }
            }
            int dh = relics_index("dragon_heart");
            if (dh >= 0) { s.owned[CAT_RELIC][dh] = 1; s.relic = dh; }
        }
        log("save: loaded %s (v%d, archetype %s)", p.c_str(), version, s.archetype >= 0 ? kArchetypes[s.archetype].id : "-");
    }
    Shared& sh = S();
    AcquireSRWLockExclusive(&sh.lock);
    sh.save = s;
    rebuild_sources(sh.save, sh.live);
    fill_ammo(sh.save, sh.live, now_s());
    ReleaseSRWLockExclusive(&sh.lock);
}

}  // namespace rh
