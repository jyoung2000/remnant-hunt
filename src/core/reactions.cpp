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

std::string path() {
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s\\learned_reactions.txt", mod_dir());
    return p;
}

void load() {
    g_loaded = true;
    FILE* f = fopen(path().c_str(), "rb");
    if (!f) return;
    int w, h, s, i, c;
    while (fscanf(f, "%d %d %d %d %d", &w, &h, &s, &i, &c) == 5) g_learned[{w, h}].push_back({s, i, c});
    fclose(f);
    log("reactions: %zu learned hit reactions loaded", g_learned.size());
}

void save() {
    FILE* f = fopen(path().c_str(), "wb");
    if (!f) return;
    for (auto& kv : g_learned)
        for (const Seen& s : kv.second) fprintf(f, "%d %d %d %d %d\n", kv.first.weapon, kv.first.heavy, s.set, s.id, s.count);
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

// Game thread: watch MHW's own hits on the hunter and learn the reactions they start.
static void learn(int weapon, int heavy, int set, int id) {
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
