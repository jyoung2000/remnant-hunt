// Remnant x MHW mashup core: the hunter flinches from Remnant hits with MHW's own hit reactions. GPL-3.0-or-later.
//
// MHW's hunter has hit reactions (a flinch for light hits, a knock-back for heavy ones) that depend on the weapon.
// The mod learns them by watching: when an MHW monster hits the hunter, the action MHW starts right after the hit is
// noted per weapon type and per light / heavy hit. Once one has been seen twice, a Remnant boss's hit of that weight
// plays it through MHW's own ActionController::DoAction (SharpPluginLoader's pattern; the function is called, never
// changed). What was learned is kept in learned_reactions.txt in the mod folder for the next sessions.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "state.h"

namespace rh {

namespace {

constexpr int kActionController = 0x61C8;  // in the player (SharpPluginLoader)
constexpr int kCurrentAction = 0xAC;       // {set, id}
typedef bool(__fastcall* DoAction_t)(void* controller, int* action);
DoAction_t g_doAction = nullptr;
bool g_searched = false, g_loaded = false;

struct Key {
    int weapon, heavy;
    bool operator<(const Key& o) const { return std::tie(weapon, heavy) < std::tie(o.weapon, o.heavy); }
};
struct Seen { int set, id, count; };
std::map<Key, std::vector<Seen>> g_learned;

double g_ownHurtAt = -10;   // a hit the mod dealt itself (not one to learn from)
float g_lastHp = -1;
double g_watchUntil = 0;
int g_watchSet = -1, g_watchId = -1, g_watchHeavy = 0, g_watchWeapon = -1;
double g_lastPlayed = 0;
int g_prevSet = -1, g_prevId = -1;
// the faint action: learned from an HP drop to 0 that the mod did not cause
int g_faintSet = -1, g_faintId = 0;
double g_faintArmAt = -1, g_faintCandAt = -1;
int g_faintCandSet = -1, g_faintCandId = -1;
// the drink action: learned when the pouch's potion-type counts (ids 1..20) drop by one and a new action that lasts
// at least 1 s begins within 1 s of it
int g_drinkSet = -1, g_drinkId = 0;
int g_poolSum = -1;
double g_poolAt = -10, g_drinkArmAt = -1, g_drinkCandAt = -1, g_actChangedAt = -10;
int g_drinkCandSet = -1, g_drinkCandId = -1, g_dPrevSet = -1, g_dPrevId = -1;
// the carve action: learned when MHW's own carve adds an item to the pouch (ids > 20) while HP is steady and the
// mod did not add it; the action that began 2.5..5 s before and lasted 2 s or more is the carving animation
struct ActChange { double t; int set, id; };
std::vector<ActChange> g_actRing;  // the last 6 s of action changes
int g_carveSet = -1, g_carveId = 0, g_carveSum = -1;
double g_carvePollAt = -10, g_ownPouchUntil = -10, g_hpChangedAt = -10;

// The same range check as turf.cpp: item-use or status animations and corrupt files are not hit reactions.
bool plausible_action(int set, int id) { return set >= 0 && set <= 15 && id >= 0 && id < 4096; }

std::string path() {
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s\\learned_reactions.txt", mod_dir());
    return p;
}

void load() {
    g_loaded = true;
    FILE* f = fopen(path().c_str(), "rb");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int w, h, s, i, c;
        if (strncmp(line, "carve ", 6) == 0) {
            if (sscanf(line + 6, "%d %d", &s, &i) == 2 && plausible_action(s, i)) { g_carveSet = s; g_carveId = i; }
        } else if (strncmp(line, "drink ", 6) == 0) {
            if (sscanf(line + 6, "%d %d", &s, &i) == 2 && plausible_action(s, i)) { g_drinkSet = s; g_drinkId = i; }
        } else if (strncmp(line, "faint ", 6) == 0) {
            if (sscanf(line + 6, "%d %d", &s, &i) == 2 && plausible_action(s, i)) { g_faintSet = s; g_faintId = i; }
        } else if (sscanf(line, "%d %d %d %d %d", &w, &h, &s, &i, &c) == 5 && w >= 0 && plausible_action(s, i)) {
            g_learned[{w, h}].push_back({s, i, c});
        }
    }
    fclose(f);
    log("reactions: %zu learned hit reactions loaded%s", g_learned.size(), g_faintSet >= 0 ? ", faint action known" : "");
}

void save() {
    FILE* f = fopen(path().c_str(), "wb");
    if (!f) return;
    for (auto& kv : g_learned)
        for (const Seen& s : kv.second) fprintf(f, "%d %d %d %d %d\n", kv.first.weapon, kv.first.heavy, s.set, s.id, s.count);
    if (g_faintSet >= 0) fprintf(f, "faint %d %d\n", g_faintSet, g_faintId);
    if (g_drinkSet >= 0) fprintf(f, "drink %d %d\n", g_drinkSet, g_drinkId);
    if (g_carveSet >= 0) fprintf(f, "carve %d %d\n", g_carveSet, g_carveId);
    fclose(f);
}

void find_do_action() {
    g_searched = true;
    for (uintptr_t h : scan_text("48 8D 41 07 48 C1 E0 04 46 3B 04 08", 4)) {
        uint8_t* f = (uint8_t*)h - 10;
        // a function start: the usual prologue bytes, after alignment padding
        if (!mem_readable(f - 1, 12)) continue;
        bool padded = f[-1] == 0xCC || f[-1] == 0x90 || f[-1] == 0xC3;
        if (!padded) continue;
        g_doAction = (DoAction_t)f;
        log("reactions: ActionController::DoAction at %p", (void*)f);
        return;
    }
    log("reactions: ActionController::DoAction not found; Remnant hits will not make the hunter flinch");
}

bool current_action(uint8_t* pl, int* set, int* id) {
    if (!pl || !mem_readable(pl + kActionController + kCurrentAction, 8)) return false;
    *set = *(int*)(pl + kActionController + kCurrentAction);
    *id = *(int*)(pl + kActionController + kCurrentAction + 4);
    return true;
}

const Seen* best(int weapon, int heavy) {
    auto it = g_learned.find({weapon, heavy});
    if (it == g_learned.end()) return nullptr;
    const Seen* b = nullptr;
    for (const Seen& s : it->second)
        if (s.count >= 2 && (!b || s.count > b->count)) b = &s;
    return b;
}

}  // namespace

void reactions_note_own_damage(double t) { g_ownHurtAt = t; }
bool reactions_faint(int* set, int* id) {
    if (!g_loaded) load();
    if (g_faintSet < 0) return false;
    *set = g_faintSet;
    *id = g_faintId;
    return true;
}
bool reactions_drink(int* set, int* id) {
    if (!g_loaded) load();
    if (g_drinkSet < 0) return false;
    *set = g_drinkSet;
    *id = g_drinkId;
    return true;
}
bool reactions_carve(int* set, int* id) {
    if (!g_loaded) load();
    if (g_carveSet < 0) return false;
    *set = g_carveSet;
    *id = g_carveId;
    return true;
}
// The mod is about to change the pouch itself: not a carve of MHW's to learn from (masked for 1 s).
void reactions_own_pouch_change(double t) { g_ownPouchUntil = t + 1.0; }
double reactions_own_damage_at() { return g_ownHurtAt; }

// Plays an action through the unit's own ActionController (the hunter's, or a monster's: the turf wars).
bool mhw_do_action(uint8_t* unit, int set, int id) {
    if (!g_searched) find_do_action();
    if (!g_doAction || !unit || !plausible_action(set, id) || !mem_readable(unit + kActionController + kCurrentAction, 8))
        return false;
    int action[2] = {set, id};
    return g_doAction(unit + kActionController, action);
}

bool reactions_play(int set, int id) { return mhw_do_action(player_ptr(), set, id); }

// Game thread: watch MHW's own hits on the hunter and learn the reactions they start.
static void learn(int weapon, int heavy, int set, int id) {
    if (weapon < 0 || !plausible_action(set, id)) return;  // unknown weapon type, or not a hit reaction
    auto& v = g_learned[{weapon, heavy}];
    auto it = std::find_if(v.begin(), v.end(), [&](const Seen& s) { return s.set == set && s.id == id; });
    if (it == v.end()) v.push_back({set, id, 1});
    else it->count++;
    save();
    log("reactions: MHW hit (%s, weapon %d) -> action %d:%d", heavy ? "heavy" : "light", weapon, set, id);
}

void reactions_tick(const Live& l, double t) {
    if (!g_loaded) load();
    const GameSnapshot& g = l.snap;
    uint8_t* pl = player_ptr();
    if (!g.inGame || !pl || !g.playerHealth || !mem_readable(g.playerHealth + 0x60, 8)) { g_lastHp = -1; return; }
    float maxHp = *(float*)(g.playerHealth + 0x60), hp = *(float*)(g.playerHealth + 0x64);
    int set, id;
    if (!current_action(pl, &set, &id)) return;
    if (g_watchUntil > 0) {
        if (set != g_watchSet || id != g_watchId) {
            learn(g_watchWeapon, g_watchHeavy, set, id);
            g_watchUntil = 0;
        } else if (t > g_watchUntil) {
            g_watchUntil = 0;
        }
    }
    if (g_lastHp >= 0 && hp < g_lastHp - 0.5f && t - g_ownHurtAt > 0.3 && hp > 0 && g_watchUntil == 0) {
        int heavy = (g_lastHp - hp) >= 0.22f * maxHp ? 1 : 0, weapon = player_weapon_type();
        if (set != g_prevSet || id != g_prevId) {
            learn(weapon, heavy, set, id);  // the reaction started in the same frame as the hit
        } else {
            g_watchUntil = t + 0.35;  // or it starts a moment later
            g_watchSet = set;
            g_watchId = id;
            g_watchHeavy = heavy;
            g_watchWeapon = weapon;
        }
    }
    // Faint: HP went from > 0 to <= 0 without a hit of the mod's; the action that begins within 1.5 s and lasts 2 s is it.
    if (g_lastHp > 0 && hp <= 0 && t - g_ownHurtAt > 0.3) {
        g_faintArmAt = t;
        g_faintCandAt = -1;
    }
    if (g_faintCandAt >= 0) {
        if (set != g_faintCandSet || id != g_faintCandId) {
            g_faintCandAt = -1;  // it ended too soon: a flinch, not the faint
        } else if (t - g_faintCandAt >= 2.0) {
            g_faintCandAt = -1;
            if (g_faintSet != set || g_faintId != id) {
                g_faintSet = set;
                g_faintId = id;
                save();
                log("reactions: learned faint action %d:%d", set, id);
            }
        }
    } else if (g_faintArmAt >= 0) {
        if (t - g_faintArmAt > 1.5) {
            g_faintArmAt = -1;
        } else if (set != g_prevSet || id != g_prevId) {
            g_faintArmAt = -1;
            g_faintCandAt = t;
            g_faintCandSet = set;
            g_faintCandId = id;
        }
    }
    // Drink: the pouch's potion-type counts (ids 1..20) fall by one, then an action of 1 s or more begins around it.
    if (set != g_dPrevSet || id != g_dPrevId) {
        g_actChangedAt = t;
        g_dPrevSet = set;
        g_dPrevId = id;
    }
    if (t - g_poolAt >= 0.25) {
        g_poolAt = t;
        int sum = 0;
        sum = mhw_pouch_sum(1, 20);
        if (g_poolSum >= 0 && sum == g_poolSum - 1 && g_drinkSet < 0 && hp > 0) {
            if (t - g_actChangedAt <= 1.0) {  // the drinking action began just before the count fell
                g_drinkCandAt = g_actChangedAt;
                g_drinkCandSet = set;
                g_drinkCandId = id;
                g_drinkArmAt = -1;
            } else {
                g_drinkArmAt = t;
            }
        }
        g_poolSum = sum;
    }
    if (g_drinkCandAt >= 0) {
        if (set != g_drinkCandSet || id != g_drinkCandId) {
            g_drinkCandAt = -1;  // too short: not a drink
        } else if (t - g_drinkCandAt >= 1.0) {
            g_drinkCandAt = -1;
            if (plausible_action(set, id)) {
                g_drinkSet = set;
                g_drinkId = id;
                save();
                log("reactions: learned drink action %d:%d", set, id);
            }
        }
    } else if (g_drinkArmAt >= 0) {
        if (t - g_drinkArmAt > 1.0) {
            g_drinkArmAt = -1;
        } else if (g_actChangedAt >= g_drinkArmAt) {
            g_drinkArmAt = -1;
            g_drinkCandAt = g_actChangedAt;
            g_drinkCandSet = set;
            g_drinkCandId = id;
        }
    }
    // Carve: the pouch gains one item (ids > 20) with HP steady, the mod not having added it.
    if (g_lastHp >= 0 && fabsf(hp - g_lastHp) > 0.5f) g_hpChangedAt = t;
    if (g_actRing.empty() || g_actRing.back().set != set || g_actRing.back().id != id) g_actRing.push_back({t, set, id});
    while (g_actRing.size() > 1 && g_actRing[1].t < t - 6.0) g_actRing.erase(g_actRing.begin());
    if (t - g_carvePollAt >= 0.25) {
        g_carvePollAt = t;
        int sum = 0;
        sum = mhw_pouch_sum(21, 2500);
        if (g_carveSum >= 0 && sum == g_carveSum + 1 && t > g_ownPouchUntil && g_carveSet < 0 && hp > 0 &&
            t - g_hpChangedAt > 5.0) {
            double want = t - 3.0;
            for (size_t k = 0; k < g_actRing.size(); k++) {
                double begin = g_actRing[k].t, end = k + 1 < g_actRing.size() ? g_actRing[k + 1].t : t;
                if (begin <= want && want < end) {
                    if (t - begin >= 2.5 && t - begin <= 5.0 && end - begin >= 2.0 &&
                        plausible_action(g_actRing[k].set, g_actRing[k].id)) {
                        g_carveSet = g_actRing[k].set;
                        g_carveId = g_actRing[k].id;
                        save();
                        log("reactions: learned carve action %d:%d", g_carveSet, g_carveId);
                    }
                    break;
                }
            }
        }
        g_carveSum = sum;
    }
    g_lastHp = hp;
    g_prevSet = set;
    g_prevId = id;
}

// Game thread: a Remnant boss's hit of this share of the hunter's health; plays the learned reaction, if any.
bool reactions_flinch(float share, double t) {
    if (!g_searched) find_do_action();
    if (!g_doAction || t - g_lastPlayed < 0.6) return false;
    int weapon = player_weapon_type();
    int heavy = share >= 0.22f ? 1 : 0;
    const Seen* s = best(weapon, heavy);
    if (!s && heavy) s = best(weapon, 0);
    if (!s) return false;
    uint8_t* pl = player_ptr();
    int cs, ci;
    if (!current_action(pl, &cs, &ci) || (cs == s->set && ci == s->id)) return false;
    int action[2] = {s->set, s->id};
    g_lastPlayed = t;
    return g_doAction(pl + kActionController, action);
}

}  // namespace rh
