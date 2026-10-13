// Remnant x MHW mashup core: the effect language. GPL-3.0-or-later (see LICENSE).
//
// Every Remnant item's mechanics are written in the sheets as short effect strings (tools/author_arsenal.py):
//   passive   [trigger:]stat+N[%][ Ts][ xS][ cdTs]      e.g. on_evade:crit+10% 15s
//   active    use:heal N% [Ts] | use:shield N% Ts | use:burst N | use:dot N Ts | use:summon N Ts
//             | use:buff stat+N% Ts | use:ammo N% | use:grenade N
// This file parses them once and keeps the running totals the combat code asks for.
#pragma once
#include <string>
#include <vector>

namespace rh {

enum Stat : int {
    ST_PROC, ST_DMG, ST_DMG_RANGED, ST_DMG_MELEE, ST_DMG_MOD, ST_DMG_SKILL, ST_DMG_ELEM, ST_DMG_STATUS, ST_DMG_EXPLOSIVE,
    ST_WEAKSPOT, ST_CRIT, ST_CRIT_DMG, ST_FIRE_RATE, ST_RELOAD, ST_MAG, ST_AMMO, ST_ACCURACY, ST_MOD_GEN, ST_MOD_DUR,
    ST_CDR, ST_SKILL_DUR, ST_RELIC_SPEED, ST_RELIC_HEAL, ST_RELIC_CHARGES, ST_HEAL, ST_LIFESTEAL, ST_DR, ST_MAX_HP,
    ST_REGEN, ST_STAMINA, ST_SHIELD, ST_COUNT
};
enum Trig : int {
    TR_ALWAYS, TR_ON_KILL, TR_ON_CRIT, TR_ON_WEAKSPOT, TR_ON_HIT, TR_ON_EVADE, TR_ON_RELIC, TR_ON_SKILL, TR_ON_MOD,
    TR_ON_RELOAD, TR_ON_HURT, TR_LOW_HP, TR_HIGH_HP, TR_COUNT
};
enum Act : int { ACT_NONE, ACT_HEAL, ACT_SHIELD, ACT_BURST, ACT_DOT, ACT_SUMMON, ACT_BUFF, ACT_AMMO, ACT_GRENADE };

struct Effect {
    Trig trig = TR_ALWAYS;
    float trigArg = 0;  // low_hp(N) / high_hp(N): percent
    Act act = ACT_NONE;  // ACT_NONE = passive
    Stat stat = ST_DMG;
    float value = 0;     // percent values are stored as fractions (10% -> 0.10)
    bool pct = false;
    float dur = 0;       // seconds (0 = while equipped / instant)
    int stacks = 1;
    float cd = 0;        // procs: at most once per cd seconds
};

const std::vector<Effect>& effects_of(const char* dsl);  // parsed once per string
std::string describe(const Effect& e);                    // our own wording, for the menus
std::string describe_all(const char* dsl);                // one line per effect
const char* stat_label(Stat s);

// Running totals for the hunter: passives from the loadout, timed buffs and stacks from triggers.
struct EffectState {
    struct Src { const char* dsl; float scale; int tag; };  // tag: 0 always, 1 long gun in hand, 2 hand gun in hand
    std::vector<Src> sources;
    struct Timed { Stat stat; float value; double until; int stacks, maxStacks; const void* key; };
    std::vector<Timed> timed;
    struct Cd { const void* key; double ready; };
    std::vector<Cd> cds;
    float hpFrac = 1.0f;  // for low_hp / high_hp
    int inHand = 1;       // 1 long gun, 2 hand gun

    void clear_sources() { sources.clear(); }
    void add_source(const char* dsl, float scale = 1.0f, int tag = 0) { if (dsl && *dsl) sources.push_back({dsl, scale, tag}); }
    float total(Stat s, double now) const;
    // Fires a trigger: timed buffs start or stack; procs (stat proc) return their damage through *procDamage.
    void fire(Trig t, double now, float* procDamage);
    void buff(Stat s, float value, double until, const void* key, int maxStacks = 1);
    void expire(double now);
};

}  // namespace rh
