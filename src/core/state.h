// Remnant x MHW mashup core: the Remnant side's state. MIT License (see LICENSE).
#pragma once
#include <map>

#include "core.h"

namespace rh {

using namespace sheets;

inline float tune(int row) { return kTuning[row].value; }

// Saved between sessions (save.json in %LOCALAPPDATA%\RemnantHunt).
struct SaveData {
    bool introDone = false;
    int archetype = -1;
    int scrap = 0;
    bool owned[kWeaponsCount] = {};
    int upgrade[kWeaponsCount] = {};  // McCabe's level per gun, 0..kUpgradesCount
    int equippedLong = -1, equippedHand = -1;
    int relicCharges = 0;
};

// Live, this session.
struct Live {
    GameSnapshot snap;
    bool combatAllowed = false;  // hunt zone (or training area) and solo
    bool gunMode = false;
    bool usingLong = true;
    bool aiming = false;
    int mag[kWeaponsCount] = {};
    int reserve[kWeaponsCount] = {};
    double reloadDone = 0;  // 0 = not reloading
    double nextShot = 0;
    double lastSlinger = 0;
    float modCharge = 0;
    double modUntil = 0;
    int modTargetSlot = -1;
    uint8_t* burnTarget = nullptr;
    double skillReady = 0, skillUntil = 0;
    double relicUntil = 0;
    float relicPerSec = 0;
    float hotPerSec = 0;
    double hotUntil = 0;
    float bonusHealth = 0;
    double lastHurt = 0;
    struct Track { float hp = 0, max = 0; bool large = false, fought = false; };
    std::map<uint8_t*, Track> track;      // per monster: last HP we know of, to see damage from MHW weapons
    float baseFov = 0;
    float lastPlayerHp = -1;
    int lastZone = -1;
    // Feedback for the HUD.
    double hitMarkerUntil = 0;
    bool hitMarkerWeak = false;
    char toast[160] = {0};
    double toastUntil = 0;
    int shots = 0, hits = 0;
    double totalDamage = 0;
    bool dirty = false;
};

struct Shared {
    SRWLOCK lock = SRWLOCK_INIT;
    SaveData save;
    Live live;
};
Shared& S();

double now_s();
void toast(Live& l, const char* fmt, ...);

// Commands from the screens (render thread) to the game thread.
enum Cmd : int { CMD_NONE, CMD_RELOAD, CMD_SWAP, CMD_SKILL, CMD_RELIC, CMD_MOD };
void post(Cmd c);

// Save/load.
void save_load();
void save_write(const SaveData& s);

// Archetype helpers.
const Archetypes* archetype();
int equipped_gun(const SaveData& s, const Live& l);  // weapons row in hand, or -1
void choose_archetype(int row);                     // render thread (takes the lock)

}  // namespace rh
