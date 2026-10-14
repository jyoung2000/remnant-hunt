// Remnant x MHW mashup core: the Remnant side's state. GPL-3.0-or-later (see LICENSE).
#pragma once
#include <map>
#include <string>
#include <vector>

#include "catalog.h"
#include "core.h"
#include "effects.h"

namespace rh {

using namespace sheets;

inline float tune(int row) { return kTuning[row].value; }

// Saved between sessions (save.json in %LOCALAPPDATA%\RemnantHunt).
struct SaveData {
    bool introDone = false;
    int archetype = -1, archetype2 = -1;  // primary and secondary archetype (archetypes rows)
    int skill[2] = {0, 0};                // chosen skill of each, index into its skill list
    std::vector<uint8_t> owned[CAT_COUNT];
    std::vector<int> stock;               // consumables carried
    std::vector<int> level;               // weapon upgrade level per weapons row
    // Loadout (rows, -1 = empty)
    int longGun = -1, handGun = -1, melee = -1;
    int gunMod[2] = {-1, -1};             // chosen mod for a gun without its own (long, hand)
    int mutator[3] = {-1, -1, -1};        // long, hand, melee
    int relic = -1, amulet = -1;
    int fragment[3] = {-1, -1, -1};
    int ring[4] = {-1, -1, -1, -1};
    int armor[4] = {-1, -1, -1, -1};      // head, body, gloves, legs
    int concoction = -1;                  // active concoction (consumables row), until the next rest
    int relicCharges = 0;
    // Progression (progress.cpp): Remnant experience and levels, trait points, prisms.
    double xp = 0;                        // all experience earned
    std::vector<uint8_t> trait;           // points in each traits row (0..10)
    struct PSlot { int8_t kind = 0; int16_t row = -1; int8_t lv = 0; };  // PS_*; PS_RAISE offers name a slot
    struct Prism { double xp = 0; PSlot slot[5]; int legendary = -1; PSlot offer[3]; int offers = 0; };
    std::vector<Prism> prism;             // per prisms row
    int prismOn = -1;                     // the equipped prism
    int difficulty = 1;                   // difficulties row for the Remnant monsters (Veteran), chosen at Ward 13
    std::vector<double> archXp;           // experience of each archetype (levels 1..10)

    SaveData() {
        for (int c = 0; c < CAT_COUNT; c++) owned[c].assign(cat_count((Cat)c), 0);
        stock.assign(kConsumablesCount, 0);
        level.assign(kWeaponsCount, 0);
        trait.assign(kTraitsCount, 0);
        archXp.assign(kArchetypesCount, 0);
        prism.assign(kPrismsCount, Prism());
        for (int i = 0; i < kTraitsCount; i++)
            if (kTraits[i].kind == TRAITS_KIND_CORE) owned[CAT_TRAIT][i] = 1;  // core traits are known from the start
    }
    bool has(Cat c, int row) const { return row >= 0 && row < (int)owned[c].size() && owned[c][row]; }
};

// Something running for a while: heals, damage over time, companions, shields.
struct Active {
    Act act = ACT_NONE;
    float perSec = 0;
    double until = 0;
    uint8_t* target = nullptr;  // uEnemy* for damage, nullptr = current target
    float acc = 0;
};

// Crafting and other changes the screens ask the game thread to make (they write MHW's memory).
struct Request {
    enum Kind { CRAFT, UPGRADE, BUY_CONSUMABLE } kind;
    Cat cat;
    int row;
};

// Live, this session.
struct Live {
    GameSnapshot snap;
    bool combatAllowed = false;  // hunt zone (or training area) and solo
    bool gunMode = false;
    bool usingLong = true;
    bool aiming = false;
    std::vector<int> mag, reserve;  // per weapons row
    double reloadDone = 0;          // 0 = not reloading
    double nextShot = 0;
    double lastSlinger = 0;
    float modCharge = 0;
    double modUntil = 0;
    uint8_t* target = nullptr;      // last monster the hunter hit
    double skillReady[2] = {0, 0}, skillUntil[2] = {0, 0};
    double relicBusyUntil = 0;
    std::vector<Active> actives;
    EffectState fx;
    float bonusHealth = 0, bonusMax = 0;
    double shieldUntil = 0;
    double lastHurt = 0;
    struct Imbue { Status status; float total, dur; double until; };
    std::vector<Imbue> imbues;  // Remnant ammo mods: hits apply a status for a while
    double regenAcc = 0;
    struct Track { float hp = 0, max = 0; bool large = false, fought = false; };
    std::map<uint8_t*, Track> track;  // per monster: last HP we know of, to see damage from MHW weapons
    float baseFov = 0;
    float lastPlayerHp = -1, maxHp = 0;
    int lastZone = -1;
    int nearMenu = -1;                // menus row the hunter stands at, -1 = none
    std::vector<Request> requests;
    char said[200] = {0};             // result of the last request, for the menu
    // Feedback for the HUD.
    double hitMarkerUntil = 0;
    struct Tracer { Vec3 to; double until; bool hit; };
    Tracer tracers[16];
    int nextTracer = 0;
    double muzzleUntil = 0;
    bool shotLogged = false;
    bool hitMarkerWeak = false;
    char toast[160] = {0};
    double toastUntil = 0;
    int shots = 0, hits = 0;
    double totalDamage = 0;
    bool dirty = false;
    bool loadoutChanged = true;

    Live() {
        mag.assign(kWeaponsCount, 0);
        reserve.assign(kWeaponsCount, 0);
    }
};

struct Shared {
    SRWLOCK lock = SRWLOCK_INIT;
    SaveData save;
    Live live;
};
Shared& S();

double now_s();
void toast(Live& l, const char* fmt, ...);
void research_tick(const Live& l, double now);
void research_stop();
// Progression (progress.cpp): experience, trait points, prisms. Under S().lock.
enum PSKind : int8_t { PS_NONE, PS_FRAGMENT, PS_FUSION, PS_LEGENDARY, PS_RAISE };
double level_xp(int level);                 // experience needed for a Remnant level, from zero
int remnant_level(double xp);
int archetype_level(const SaveData& s, int archetype);  // 1..10 (0 for none)
double archetype_next_xp(int level);        // the archetype's total experience for the next level, 0 at 10
int skills_unlocked(int level);             // Remnant II: the first skill at level 1, the second at 5, the third at 10
int skill_index(const SaveData& s, int slot);  // the chosen skill of an archetype slot, among those unlocked
float perk_scale(int perk, int level);      // how much of a perk an archetype of this level gives (0 = not yet)
int perk_unlock_level(int perk);
int trait_auto(const SaveData& s, int row); // points the archetypes put in a trait on their own
int trait_level(const SaveData& s, int row);  // 0..10, all told
int trait_points(const SaveData& s);        // earned
int trait_points_spent(const SaveData& s);
bool trait_known(const SaveData& s, int row);
bool trait_set(SaveData& s, int row, int delta);  // one point in (+1) or out (-1)
bool trait_can_set(const SaveData& s, int row, int delta);
void traits_reset(SaveData& s);
int prism_level(const SaveData::Prism& p);  // 0..51
double prism_need(const SaveData::Prism& p);  // experience for its next level, 0 at the top
void prism_offer(SaveData::Prism& p);       // three choices once a level is due
bool prism_pick(SaveData& s, int prism, int offer);
bool prism_cleanse(SaveData& s, int prism);
void award_xp(SaveData& s, Live& l, float xp, double t);
void progress_sources(const SaveData& s, EffectState& fx);  // traits and the equipped prism
bool prime_time(const SaveData& s);         // the Prime Time legendary: the second archetype's prime perk too
// Remnant statuses (remnant.cpp). Game thread, under S().lock.
uint32_t status_bits(const void* em);  // bit per Status active on a monster (uEnemy*), nullptr = the hunter
void inflict_player(Live& l, Status st, float total, float dur, double t);  // total: hunter HP over dur
void status_player_dodge(double t);   // a dodge roll: burning goes out faster
// The hunter's hit reactions (reactions.cpp), learned from MHW's own hits. Game thread.
void reactions_tick(const Live& l, double t);
void reactions_note_own_damage(double t);   // the mod is about to damage the hunter itself
// MHW's stamina bar with Remnant's stamina bonuses (stamina.cpp). Game thread.
void stamina_tick(Live& l, double t);
bool stamina_known();
double stamina_dodge_at();  // when MHW last spent a dodge's worth of stamina at once
double reactions_own_damage_at();               // when the mod last damaged the hunter itself
bool mhw_do_action(uint8_t* unit, int set, int id);  // ActionController::DoAction on the hunter or a monster
bool reactions_flinch(float share, double t);  // a Remnant hit worth this share of max health: MHW's reaction  // research.cpp: read-only findings for the diagnostic run

// Commands from the screens (render thread) to the game thread.
enum Cmd : int { CMD_NONE, CMD_RELOAD, CMD_SWAP, CMD_SKILL, CMD_SKILL2, CMD_RELIC, CMD_MOD };
void post(Cmd c);
void request(const Request& r);  // takes the lock

// Save/load.
void save_load();
bool save_write(const SaveData& s);

// Archetype and loadout helpers.
const Archetypes* archetype();
int equipped_gun(const SaveData& s, const Live& l);   // weapons row in hand, or -1
int gun_mod(const SaveData& s, int gun);               // mods row the gun fires, or -1
std::vector<int> skills_of(int archetypeRow);          // skills rows
std::vector<int> perks_of(int archetypeRow);           // perks rows (prime first)
void choose_archetype(int row, int slot);              // render thread (takes the lock); slot 0 primary, 1 secondary
void equip(Cat c, int row, int slot);                  // render thread (takes the lock)
void use_consumable(int row);                          // render thread (takes the lock)
float upgrade_mult(int weapon, int level);
int weapon_max_level(int weapon);          // Remnant II: +20, or +10 for special weapons
std::string upgrade_cost(int weapon, int level, int* zenny);  // recipe from level to level + 1
bool can_equip_here(const Live& l);                    // in town

// MHW's own inventory (inventory.cpp). Reads work on any thread; changes only on the game thread.
int mhw_zenny();
bool mhw_spend_zenny(int amount);
int mhw_item_count(int itemId);
bool mhw_take_items(int itemId, int count);
const char* mhw_item_name(int itemId);  // MHW's own name for the item, or nullptr
bool mhw_inventory_ready();
void inventory_tick();                  // game thread: finds the containers, reads names

}  // namespace rh
