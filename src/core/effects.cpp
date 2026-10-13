// Remnant x MHW mashup core: the effect language (see effects.h). GPL-3.0-or-later (see LICENSE).
#include "effects.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

namespace rh {

static const char* kStatNames[ST_COUNT] = {
    "proc", "dmg", "dmg_ranged", "dmg_melee", "dmg_mod", "dmg_skill", "dmg_elem", "dmg_status", "dmg_explosive",
    "weakspot", "crit", "crit_dmg", "fire_rate", "reload", "mag", "ammo", "accuracy", "mod_gen", "mod_dur", "cdr",
    "skill_dur", "relic_speed", "relic_heal", "relic_charges", "heal", "lifesteal", "dr", "max_hp", "regen", "stamina",
    "shield"};
static const char* kStatLabels[ST_COUNT] = {
    "Damage", "All Damage", "Ranged Damage", "Melee Damage", "Mod Damage", "Skill Damage", "Elemental Damage",
    "Status Damage", "Explosive Damage", "Weakspot Damage", "Critical Chance", "Critical Damage", "Fire Rate",
    "Reload Speed", "Magazine Size", "Ammo Reserves", "Accuracy", "Mod Power Generation", "Mod Duration",
    "Skill Cooldown Reduction", "Skill Duration", "Relic Use Speed", "Relic Healing", "Relic Charges", "Healing",
    "Lifesteal", "Damage Reduction", "Max Health", "Health Regeneration", "Stamina", "Shield"};
static const char* kTrigNames[TR_COUNT] = {"",          "on_kill",  "on_crit",  "on_weakspot", "on_hit",
                                           "on_evade",  "on_relic", "on_skill", "on_mod",      "on_reload",
                                           "on_hurt",   "low_hp",   "high_hp"};
static const char* kTrigLabels[TR_COUNT] = {"",          "On kill",         "On a critical hit", "On a weakspot hit",
                                            "On hit",    "After an evade",  "On relic use",      "On skill use",
                                            "On mod use", "On reload",      "When hit",          "Below %d%% health",
                                            "Above %d%% health"};

const char* stat_label(Stat s) { return s >= 0 && s < ST_COUNT ? kStatLabels[s] : "?"; }

static bool stat_from(const char*& p, Stat* out) {
    int best = -1;
    size_t bestLen = 0;
    for (int i = 0; i < ST_COUNT; i++) {
        size_t n = strlen(kStatNames[i]);
        if (n > bestLen && !strncmp(p, kStatNames[i], n) && (p[n] == '+' || p[n] == '-' || p[n] == ' ' || !p[n])) {
            best = i;
            bestLen = n;
        }
    }
    if (best < 0) return false;
    *out = (Stat)best;
    p += bestLen;
    return true;
}

// "+10%" / "-2.5" -> value, pct
static bool signed_value(const char*& p, float* v, bool* pct) {
    if (*p != '+' && *p != '-') return false;
    float sign = *p == '-' ? -1.0f : 1.0f;
    char* e;
    float x = strtof(p + 1, &e);
    if (e == p + 1) return false;
    p = e;
    *pct = *p == '%';
    if (*pct) p++;
    *v = sign * (*pct ? x / 100.0f : x);
    return true;
}

static void options(const char* p, Effect* e) {
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        char* end;
        if (*p == 'x') {
            e->stacks = std::max(1, (int)strtol(p + 1, &end, 10));
            p = end;
        } else if (!strncmp(p, "cd", 2)) {
            e->cd = strtof(p + 2, &end);
            p = end + (*end == 's');
        } else {
            float v = strtof(p, &end);
            if (end == p) break;
            p = end;
            if (*p == 's') { e->dur = v; p++; }
            else if (*p == '%') p++;
        }
    }
}

static bool parse_one(const std::string& s, Effect* e) {
    const char* p = s.c_str();
    if (!strncmp(p, "use:", 4)) {
        p += 4;
        struct { const char* w; Act a; } verbs[] = {{"heal ", ACT_HEAL},    {"shield ", ACT_SHIELD}, {"burst ", ACT_BURST},
                                                   {"dot ", ACT_DOT},      {"summon ", ACT_SUMMON}, {"buff ", ACT_BUFF},
                                                   {"ammo ", ACT_AMMO},    {"grenade ", ACT_GRENADE}};
        for (auto& v : verbs)
            if (!strncmp(p, v.w, strlen(v.w))) {
                e->act = v.a;
                p += strlen(v.w);
                if (v.a == ACT_BUFF) {
                    if (!stat_from(p, &e->stat) || !signed_value(p, &e->value, &e->pct)) return false;
                } else {
                    char* end;
                    float x = strtof(p, &end);
                    if (end == p) return false;
                    p = end;
                    e->pct = *p == '%';
                    if (e->pct) p++;
                    e->value = e->pct ? x / 100.0f : x;
                }
                options(p, e);
                return true;
            }
        return false;
    }
    const char* colon = strchr(p, ':');
    if (colon) {
        std::string t(p, colon - p);
        size_t par = t.find('(');
        std::string name = par == std::string::npos ? t : t.substr(0, par);
        bool ok = false;
        for (int i = 1; i < TR_COUNT; i++)
            if (name == kTrigNames[i]) { e->trig = (Trig)i; ok = true; }
        if (!ok) return false;
        if (par != std::string::npos) e->trigArg = (float)atof(t.c_str() + par + 1);
        p = colon + 1;
    }
    if (!stat_from(p, &e->stat) || !signed_value(p, &e->value, &e->pct)) return false;
    options(p, e);
    return true;
}

const std::vector<Effect>& effects_of(const char* dsl) {
    static std::mutex m;
    static std::map<const char*, std::vector<Effect>> cache;
    static const std::vector<Effect> none;
    if (!dsl || !*dsl) return none;
    std::lock_guard<std::mutex> l(m);
    auto it = cache.find(dsl);
    if (it != cache.end()) return it->second;
    std::vector<Effect> out;
    std::string s(dsl);
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(';', a);
        if (b == std::string::npos) b = s.size();
        Effect e;
        if (b > a && parse_one(s.substr(a, b - a), &e)) out.push_back(e);
        a = b + 1;
    }
    return cache.emplace(dsl, std::move(out)).first->second;
}

static std::string num(float v) {
    char b[32];
    float r = std::round(v * 10.0f) / 10.0f;
    if (std::fabs(r - std::round(r)) < 0.05f) snprintf(b, sizeof(b), "%d", (int)std::round(r));
    else snprintf(b, sizeof(b), "%.1f", r);
    return b;
}

static std::string amount(const Effect& e) {
    float v = e.pct ? e.value * 100.0f : e.value;
    return (v >= 0 ? "+" : "-") + num(v < 0 ? -v : v) + (e.pct ? "%" : "");
}

std::string describe(const Effect& e) {
    std::string s;
    switch (e.act) {
        case ACT_HEAL:
            s = "Heals " + num(e.value * 100) + "% of max health" + (e.dur > 0 ? " over " + num(e.dur) + "s" : "");
            return s;
        case ACT_SHIELD: return "Shield worth " + num(e.value * 100) + "% of max health for " + num(e.dur) + "s";
        case ACT_BURST: return "Deals " + num(e.value) + " damage to your target";
        case ACT_DOT: return "Deals " + num(e.value) + " damage per second to your target for " + num(e.dur) + "s";
        case ACT_SUMMON: return "A companion deals " + num(e.value) + " damage per second for " + num(e.dur) + "s";
        case ACT_BUFF: return amount(e) + " " + stat_label(e.stat) + " for " + num(e.dur) + "s";
        case ACT_AMMO: return "Refills " + num(e.value * 100) + "% of your reserve ammo";
        case ACT_GRENADE: return "Explodes for " + num(e.value) + " damage around your target";
        default: break;
    }
    if (e.trig != TR_ALWAYS) {
        char t[64];
        snprintf(t, sizeof(t), kTrigLabels[e.trig], (int)e.trigArg);
        s = std::string(t) + ": ";
    }
    if (e.stat == ST_PROC) {
        s += "deal " + num(e.value) + " damage";
        if (e.cd > 0) s += " (at most every " + num(e.cd) + "s)";
        return s;
    }
    s += amount(e) + " " + stat_label(e.stat);
    if (e.dur > 0 && e.trig != TR_ALWAYS) s += " for " + num(e.dur) + "s";
    if (e.stacks > 1) s += ", stacks " + num((float)e.stacks) + "x";
    return s;
}

std::string describe_all(const char* dsl) {
    std::string out;
    for (const Effect& e : effects_of(dsl)) {
        if (!out.empty()) out += "\n";
        out += describe(e);
    }
    return out;
}

float EffectState::total(Stat s, double now) const {
    float v = 0;
    for (const Src& src : sources) {
        if (src.tag && src.tag != inHand) continue;
        for (const Effect& e : effects_of(src.dsl)) {
            if (e.act != ACT_NONE || e.stat != s) continue;
            if (e.trig == TR_ALWAYS) v += e.value * src.scale;
            else if (e.trig == TR_LOW_HP && hpFrac * 100.0f < e.trigArg) v += e.value * src.scale;
            else if (e.trig == TR_HIGH_HP && hpFrac * 100.0f > e.trigArg) v += e.value * src.scale;
        }
    }
    for (const Timed& t : timed)
        if (t.stat == s && t.until > now) v += t.value * t.stacks;
    return v;
}

void EffectState::buff(Stat s, float value, double until, const void* key, int maxStacks) {
    for (Timed& t : timed)
        if (t.key == key && key) {
            t.until = until;
            t.stacks = std::min(t.maxStacks, t.stacks + 1);
            return;
        }
    timed.push_back({s, value, until, 1, std::max(1, maxStacks), key});
}

void EffectState::fire(Trig trig, double now, float* procDamage) {
    for (const Src& src : sources) {
        if (src.tag && src.tag != inHand) continue;
        for (const Effect& e : effects_of(src.dsl)) {
            if (e.act != ACT_NONE || e.trig != trig) continue;
            if (e.stat == ST_PROC) {
                bool ready = true;
                for (Cd& c : cds)
                    if (c.key == &e) {
                        ready = now >= c.ready;
                        if (ready) c.ready = now + std::max(0.2f, e.cd);
                    }
                if (ready) {
                    bool known = false;
                    for (Cd& c : cds) known |= c.key == &e;
                    if (!known) cds.push_back({&e, now + std::max(0.2f, e.cd)});
                    if (procDamage) *procDamage += e.value * src.scale;
                }
                continue;
            }
            buff(e.stat, e.value * src.scale, now + (e.dur > 0 ? e.dur : 10.0), &e, e.stacks);
        }
    }
}

void EffectState::expire(double now) {
    timed.erase(std::remove_if(timed.begin(), timed.end(), [&](const Timed& t) { return t.until <= now; }), timed.end());
}

}  // namespace rh
