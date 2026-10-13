// Remnant x MHW mashup core: Remnant II's rules running on top of Monster Hunter: World. MIT License (see LICENSE).
//
// Every number comes from the design sheets (gen/sheets.gen.h). The game-thread tick reads MHW's state, fires
// Remnant guns as hitscan shots against the monsters' boxes, and turns HP changes into archetype, skill, mod and
// relic effects. MHW's own weapons, armor and items keep working; their hits feed the same Remnant effects.
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

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

const Archetypes* archetype() {
    int a = S().save.archetype;
    return (a >= 0 && a < kArchetypesCount) ? &kArchetypes[a] : nullptr;
}

int equipped_gun(const SaveData& s, const Live& l) { return l.usingLong ? s.equippedLong : s.equippedHand; }

static uint32_t g_rng = 0x9E3779B9u;
static float frand() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return (g_rng & 0xFFFFFF) / 16777216.0f;
}

static void fill_ammo(SaveData& s, Live& l) {
    for (int i = 0; i < kWeaponsCount; i++)
        if (s.owned[i]) {
            l.mag[i] = (int)kWeapons[i].mag;
            l.reserve[i] = (int)kWeapons[i].reserve;
        }
}

void choose_archetype(int row) {
    Shared& sh = S();
    AcquireSRWLockExclusive(&sh.lock);
    SaveData& s = sh.save;
    bool first = s.archetype < 0;
    s.archetype = row;
    const Archetypes& a = kArchetypes[row];
    s.owned[a.long_gun] = s.owned[a.hand_gun] = true;
    s.equippedLong = a.long_gun;
    s.equippedHand = a.hand_gun;
    if (first) {
        s.scrap = (int)tune(TUNING_STARTING_SCRAP);
        s.relicCharges = (int)kRelics[RELICS_DRAGON_HEART].charges;
    }
    s.introDone = true;
    fill_ammo(s, sh.live);
    sh.live.bonusHealth = a.hp_bonus;
    sh.live.skillReady = 0;
    sh.live.dirty = true;
    ReleaseSRWLockExclusive(&sh.lock);
    log("archetype: %s", a.id);
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

// ---------------------------------------------------------------- shooting

static float damage_mult_vs_monsters(const Live& l, double t) {
    float m = 1.0f;
    const Archetypes* a = archetype();
    if (a && t < l.skillUntil && kSkills[a->skill].effect == SKILLS_EFFECT_MONSTER_DAMAGE_MULT) m *= kSkills[a->skill].magnitude;
    int g = S().save.equippedLong;
    if (g >= 0 && kWeapons[g].mod >= 0 && t < l.modUntil && kMods[kWeapons[g].mod].effect == MODS_EFFECT_CORRODE)
        m *= kMods[kWeapons[g].mod].magnitude;
    return m;
}

static void deal(Live& l, const Monster& m, float dmg, Vec3 at, bool crit) {
    if (dmg <= 0) return;
    add_hp(m.health, -dmg);
    Live::Track& tr = l.track[m.em];
    if (tr.max <= 0) { tr.hp = m.hp; tr.max = m.maxHp; tr.large = m.large; }
    tr.hp -= dmg;
    tr.fought = true;
    show_damage(m.em, (int)lroundf(dmg), at, crit);
    l.totalDamage += dmg;
}

static void fire(SaveData& s, Live& l, int gun, double t) {
    const Weapons& w = kWeapons[gun];
    const Archetypes* a = archetype();
    const GameView& v = l.snap.view;
    Vec3 fwd = norm(sub(v.camTarget, v.camPos));
    Vec3 up = {0, 1, 0};
    Vec3 right = norm(cross(fwd, up));
    Vec3 up2 = cross(right, fwd);
    // Start past the hunter so the ray never hits the hunter or the space behind.
    float tMin = std::max(0.0f, dot(sub(l.snap.pos, v.camPos), fwd));
    Vec3 origin = add(v.camPos, mul(fwd, tMin));
    float range = w.range_m * 100.0f;  // MHW units are centimetres
    float spread = w.spread_deg * (l.aiming ? tune(TUNING_ADS_SPREAD_MULT) : 1.0f) * 3.14159265f / 180.0f;
    int lvl = s.upgrade[gun];
    float upMult = lvl > 0 ? kUpgrades[lvl - 1].damage_mult : 1.0f;
    float base = w.damage * tune(TUNING_DAMAGE_SCALE) * upMult * (a ? a->ranged_mult : 1.0f) * damage_mult_vs_monsters(l, t);
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
        bool crit = frand() < tune(TUNING_CRIT_CHANCE);
        float dmg = base * (weak ? w.weakspot_mult : 1.0f) * (crit ? tune(TUNING_CRIT_MULT) : 1.0f);
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
    for (int k = 0; k < nacc; k++) {
        deal(l, *acc[k].m, acc[k].dmg, acc[k].at, acc[k].crit || acc[k].weak);
        l.hits++;
        l.hitMarkerUntil = t + 0.18;
        l.hitMarkerWeak = acc[k].weak;
        int mod = s.equippedLong >= 0 ? kWeapons[s.equippedLong].mod : -1;
        if (mod >= 0 && t >= l.modUntil) l.modCharge = std::min(kMods[mod].charge_damage, l.modCharge + acc[k].dmg);
        if (acc[k].m->large) s.scrap += (int)(acc[k].dmg * tune(TUNING_SCRAP_PER_1000_DAMAGE) / 1000.0f + frand());
        if (tune(TUNING_AGGRO_SHELLS) > 0 && t - l.lastSlinger > tune(TUNING_SLINGER_INTERVAL_S)) {
            Vec3 back = norm(sub(v.camPos, acc[k].at));
            fire_slinger(add(acc[k].at, mul(back, 150.0f)), acc[k].m->center);
            l.lastSlinger = t;
        }
        if (k == 0) l.burnTarget = acc[k].m->em;
    }
}

// ---------------------------------------------------------------- the tick

static double g_lastTick = 0, g_lastSave = 0;

static const Monster* find_monster(const Live& l, uint8_t* em) {
    for (const Monster& m : l.snap.monsters)
        if (m.em == em) return &m;
    return nullptr;
}

void mod_game_tick() {
    Shared& sh = S();
    double t = now_s();
    float dt = g_lastTick > 0 ? (float)std::min(0.25, t - g_lastTick) : 0.016f;
    g_lastTick = t;
    AcquireSRWLockExclusive(&sh.lock);
    SaveData& s = sh.save;
    Live& l = sh.live;
    snapshot(&l.snap);
    const GameSnapshot& g = l.snap;
    const Archetypes* a = archetype();

    // Zone changes: back at base, or setting out on a hunt, is Remnant's rest at a worldstone.
    if (g.zone != l.lastZone && g.zone >= 0) {
        if (l.lastZone >= 0 && (g.inBase || zone_is_base(l.lastZone))) {
            s.relicCharges = (int)kRelics[RELICS_DRAGON_HEART].charges;
            fill_ammo(s, l);
            if (a) l.bonusHealth = a->hp_bonus;
            toast(l, "Rested: relic charges and ammo restored");
        }
        log("zone %d -> %d (%s)", l.lastZone, g.zone, g.inBase ? "base" : "field");
        l.lastZone = g.zone;
        l.track.clear();
        l.lastPlayerHp = -1;
    }
    l.combatAllowed = g.inGame && a && (!g.inBase || g.zone == 504) && g.solo;
    if (!l.combatAllowed) l.gunMode = false;

    LONG cmds = InterlockedExchange(&g_cmds, 0);
    auto has = [&](Cmd c) { return (cmds & (1 << c)) != 0; };

    if (g.inGame && a) {
        // ---- player health: damage taken, refunds, heals
        if (g.playerHealth && l.lastPlayerHp >= 0) {
            float drop = l.lastPlayerHp - g.hp;
            if (drop > 0.5f && drop < 1000.0f) {
                float refund = 0;
                if (t < l.skillUntil && kSkills[a->skill].effect == SKILLS_EFFECT_DAMAGE_REDUCTION)
                    refund += drop * (1.0f - kSkills[a->skill].magnitude);
                refund += drop * (1.0f - a->damage_taken_mult);
                float absorbed = std::min(l.bonusHealth, drop - refund);
                if (absorbed > 0) { refund += absorbed; l.bonusHealth -= absorbed; }
                if (refund > 0) add_hp(g.playerHealth, refund);
                l.lastHurt = t;
            }
        }
        float heal = 0;
        if (t < l.relicUntil) heal += l.relicPerSec * dt;
        if (t < l.hotUntil) heal += l.hotPerSec * dt;
        if (heal > 0 && g.playerHealth) add_hp(g.playerHealth, heal);
        if (t - l.lastHurt > tune(TUNING_BONUS_HEALTH_REGEN_DELAY_S))
            l.bonusHealth = std::min(a->hp_bonus, l.bonusHealth + tune(TUNING_BONUS_HEALTH_REGEN_PER_S) * dt);
        float hpNow = 0, maxNow = 0;
        if (g.playerHealth && mem_readable(g.playerHealth + 0x60, 8)) {
            maxNow = *(float*)(g.playerHealth + 0x60);
            hpNow = *(float*)(g.playerHealth + 0x64);
        }
        l.lastPlayerHp = g.playerHealth ? hpNow : -1;

        // ---- relic
        if (has(CMD_RELIC)) {
            if (s.relicCharges > 0 && t >= l.relicUntil && g.playerHealth) {
                const Relics& r = kRelics[RELICS_DRAGON_HEART];
                s.relicCharges--;
                float total = maxNow * r.heal_pct_max_hp / 100.0f * a->relic_heal_mult;
                l.relicPerSec = total / r.heal_over_s;
                l.relicUntil = t + r.heal_over_s;
                l.dirty = true;
            } else if (s.relicCharges <= 0) {
                toast(l, "No relic charges left");
            }
        }

        // ---- archetype skill
        if (has(CMD_SKILL)) {
            const Skills& sk = kSkills[a->skill];
            if (t >= l.skillReady) {
                l.skillUntil = t + sk.duration_s;
                l.skillReady = t + sk.cooldown_s;
                if (sk.effect == SKILLS_EFFECT_HEAL_OVER_TIME) {
                    l.hotPerSec = sk.magnitude;
                    l.hotUntil = l.skillUntil;
                } else if (sk.effect == SKILLS_EFFECT_INFINITE_AMMO) {
                    for (int gi : {s.equippedLong, s.equippedHand})
                        if (gi >= 0) l.mag[gi] = (int)kWeapons[gi].mag;
                    l.reloadDone = 0;
                }
                log("skill %s on", sk.id);
            }
        }
        bool infinite = t < l.skillUntil && kSkills[a->skill].effect == SKILLS_EFFECT_INFINITE_AMMO;

        // ---- MHW weapon hits and monster deaths -> Remnant effects
        for (const Monster& m : g.monsters) {
            auto it = l.track.find(m.em);
            if (it != l.track.end()) {
                float ext = it->second.hp - m.hp;
                if (ext > 0.5f && ext < 100000.0f) {
                    float mult = damage_mult_vs_monsters(l, t);
                    if (t < l.skillUntil && kSkills[a->skill].effect == SKILLS_EFFECT_DAMAGE_REDUCTION)
                        mult *= kSkills[a->skill].magnitude2;  // Juggernaut also hits harder in melee
                    float bonus = ext * (mult - 1.0f);
                    if (bonus > 0.5f) add_hp(m.health, -bonus);
                    int mod = s.equippedLong >= 0 ? kWeapons[s.equippedLong].mod : -1;
                    if (mod >= 0 && t >= l.modUntil) l.modCharge = std::min(kMods[mod].charge_damage, l.modCharge + ext);
                    if (m.large) s.scrap += (int)(ext * tune(TUNING_SCRAP_PER_1000_DAMAGE) / 1000.0f + frand());
                    it->second.hp = m.hp - std::max(0.0f, bonus);
                    it->second.fought = true;
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
                // A large monster we fought left the list low on health: slain or captured.
                const Live::Track& tr = it->second;
                if (tr.large && tr.fought && tr.max > 0 && tr.hp < 0.25f * tr.max) {
                    s.scrap += (int)tune(TUNING_SCRAP_PER_LARGE_KILL);
                    toast(l, "+%d scrap", (int)tune(TUNING_SCRAP_PER_LARGE_KILL));
                    l.dirty = true;
                }
                it = l.track.erase(it);
            } else {
                ++it;
            }
        }

        // ---- weapon mod
        int mod = s.equippedLong >= 0 ? kWeapons[s.equippedLong].mod : -1;
        if (has(CMD_MOD) && mod >= 0 && l.combatAllowed) {
            const Mods& md = kMods[mod];
            if (l.modCharge >= md.charge_damage && t >= l.modUntil) {
                l.modCharge = 0;
                l.modUntil = t + std::max(0.1f, md.duration_s);
                if (md.effect == MODS_EFFECT_HEAL_BURST && g.playerHealth) add_hp(g.playerHealth, md.magnitude);
                if (md.effect == MODS_EFFECT_BLAST) {
                    const Monster* tgt = find_monster(l, l.burnTarget);
                    if (tgt) deal(l, *tgt, md.magnitude * tune(TUNING_DAMAGE_SCALE), tgt->center, true);
                }
                log("mod %s on", md.id);
            }
        }
        if (mod >= 0 && t < l.modUntil && kMods[mod].effect == MODS_EFFECT_BURN) {
            const Monster* tgt = find_monster(l, l.burnTarget);
            static float burnAcc = 0;
            burnAcc += kMods[mod].magnitude * tune(TUNING_DAMAGE_SCALE) * dt;
            if (tgt && burnAcc >= 4.0f) { deal(l, *tgt, burnAcc, tgt->center, false); burnAcc = 0; }
        }

        // ---- guns
        if (has(CMD_SWAP) && l.gunMode) { l.usingLong = !l.usingLong; l.reloadDone = 0; }
        int gun = equipped_gun(s, l);
        if (gun >= 0) {
            const Weapons& w = kWeapons[gun];
            float reloadTime = w.reload_s * a->reload_mult;
            if (l.reloadDone > 0 && t >= l.reloadDone) {
                int need = (int)w.mag - l.mag[gun];
                int take = std::min(need, l.reserve[gun]);
                l.mag[gun] += take;
                l.reserve[gun] -= take;
                l.reloadDone = 0;
            }
            bool wantReload = has(CMD_RELOAD) || (l.mag[gun] == 0 && l.reserve[gun] > 0);
            if (l.gunMode && wantReload && l.reloadDone == 0 && l.mag[gun] < (int)w.mag && l.reserve[gun] > 0)
                l.reloadDone = t + reloadTime;
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
            if (l.gunMode && g.view.valid && key_down(VK_LBUTTON) && l.reloadDone == 0 && t >= l.nextShot &&
                (l.mag[gun] > 0 || infinite)) {
                fire(s, l, gun, t);
                if (!infinite) l.mag[gun]--;
                float rpm = w.rpm * (infinite ? kSkills[a->skill].magnitude : 1.0f);
                l.nextShot = t + 60.0 / std::max(1.0f, rpm);
                if (test_mode() && l.hits == 1 && l.shots >= 1) melty_log("check shot-landed ok");
            }
        }
    }
    if (l.dirty && t - g_lastSave > 3.0) {
        save_write(s);
        l.dirty = false;
        g_lastSave = t;
    }
    ReleaseSRWLockExclusive(&sh.lock);
}

// ---------------------------------------------------------------- save file

static std::string save_path() {
    char dir[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", dir, sizeof(dir))) snprintf(dir, sizeof(dir), "%s", mod_dir());
    std::string d = std::string(dir) + "\\RemnantHunt";
    CreateDirectoryA(d.c_str(), nullptr);
    return d + "\\save.json";
}

void save_write(const SaveData& s) {
    std::string j = "{\n  \"version\": 1,\n";
    j += std::string("  \"introDone\": ") + (s.introDone ? "true" : "false") + ",\n";
    j += std::string("  \"archetype\": \"") + (s.archetype >= 0 ? kArchetypes[s.archetype].id : "") + "\",\n";
    j += "  \"scrap\": " + std::to_string(s.scrap) + ",\n";
    j += "  \"relic\": " + std::to_string(s.relicCharges) + ",\n";
    j += std::string("  \"long\": \"") + (s.equippedLong >= 0 ? kWeapons[s.equippedLong].id : "") + "\",\n";
    j += std::string("  \"hand\": \"") + (s.equippedHand >= 0 ? kWeapons[s.equippedHand].id : "") + "\",\n";
    j += "  \"guns\": {";
    bool first = true;
    for (int i = 0; i < kWeaponsCount; i++)
        if (s.owned[i]) {
            j += std::string(first ? "" : ", ") + "\"" + kWeapons[i].id + "\": " + std::to_string(s.upgrade[i]);
            first = false;
        }
    j += "}\n}\n";
    std::string p = save_path(), tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fwrite(j.data(), 1, j.size(), f);
    fclose(f);
    MoveFileExA(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
}

static std::string json_str(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return "";
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return "";
    p++;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\n')) p++;
    if (j[p] == '"') {
        size_t e = j.find('"', p + 1);
        return j.substr(p + 1, e - p - 1);
    }
    size_t e = j.find_first_of(",}\n", p);
    return j.substr(p, e - p);
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
        s.introDone = json_str(j, "introDone") == "true";
        std::string a = json_str(j, "archetype");
        for (int i = 0; i < kArchetypesCount; i++)
            if (a == kArchetypes[i].id) s.archetype = i;
        s.scrap = atoi(json_str(j, "scrap").c_str());
        s.relicCharges = atoi(json_str(j, "relic").c_str());
        size_t g = j.find("\"guns\"");
        for (int i = 0; i < kWeaponsCount; i++) {
            std::string id = std::string("\"") + kWeapons[i].id + "\"";
            size_t at = j.find(id, g == std::string::npos ? 0 : g);
            if (g != std::string::npos && at != std::string::npos) {
                s.owned[i] = true;
                s.upgrade[i] = std::min((int)kUpgradesCount, atoi(j.c_str() + j.find(':', at) + 1));
            }
            if (json_str(j, "long") == kWeapons[i].id) s.equippedLong = i;
            if (json_str(j, "hand") == kWeapons[i].id) s.equippedHand = i;
        }
        log("save: loaded %s (archetype %s, scrap %d)", p.c_str(), a.c_str(), s.scrap);
    }
    Shared& sh = S();
    AcquireSRWLockExclusive(&sh.lock);
    sh.save = s;
    fill_ammo(sh.save, sh.live);
    if (const Archetypes* a = s.archetype >= 0 ? &kArchetypes[s.archetype] : nullptr) sh.live.bonusHealth = a->hp_bonus;
    ReleaseSRWLockExclusive(&sh.lock);
}

}  // namespace rh
