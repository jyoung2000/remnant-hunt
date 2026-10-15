// Remnant x MHW mashup core: Remnant II bosses roaming MHW's field zones as creatures of their own. GPL-3.0-or-later.
//
// The bosses are not MHW monsters: the mod runs them (AI, health, attacks, sounds, loot) and draws them with Remnant
// II's own model, textures and animations, read from the player's Remnant II install (render3d.cpp, model.cpp).
// To the rest of the mod they look like monsters (pseudo entries in the snapshot), so the Remnant guns, skills, mods
// and effects work on them unchanged.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "state.h"

namespace rh {

// Bosses use mod-owned identity tokens; these are not MHW uEnemy objects. Match only an exact token address so an
// unrelated game pointer can never be classified by an out-of-bounds relational pointer comparison.
inline int boss_token_index(const uint8_t* em, const uint8_t* tokens, size_t count) {
    if (!em || !tokens) return -1;
    for (size_t i = 0; i < count; ++i)
        if (em == tokens + i) return (int)i;
    return -1;
}

enum BossEvent : int { BOSS_EV_EVADE = 1, BOSS_EV_HURT = 2, BOSS_EV_KILL = 4 };

// Game thread, under S().lock, right after snapshot(): runs the bosses and adds them to l.snap.monsters.
int bosses_tick(SaveData& s, Live& l, double t, float dt);  // returns BossEvent bits
// Damage to a boss (game thread). False when em is not one of the bosses.
bool boss_hit(uint8_t* em, float dmg, const Vec3& at, bool crit, bool weak);
bool is_boss(const uint8_t* em);
// As boss_hit, with how much the hit staggers (1 = a normal hit; area skills and heavy blows more).
bool boss_hit_ex(uint8_t* em, float dmg, const Vec3& at, bool crit, bool weak, float stagger);
float boss_time_scale(const uint8_t* em);  // SLOWED bosses move and act slower

// Turf wars (turf.cpp): staged fights between MHW monsters, and the MHW monster's side of a Remnant boss's fight.
void turf_tick(Live& l, double t, float dt);                  // game thread, after bosses_tick
bool turf_enabled();
void turf_note_mod_damage(const uint8_t* em, double t);      // the mod is damaging this MHW monster itself
void turf_hurt(const Monster& m, float dmg);                  // damage from a turf war (MHW's own add_hp)
bool turf_face(const Monster& m, Vec3 at, float dt);          // turns it towards at; true once it faces it
float turf_attack(const Monster& m, float gap, float* reach); // one of its learned blows; seconds until it lands
void turf_flinch(const Monster& m);                           // its learned flinch, if known
bool turf_busy(const uint8_t* em);                            // in a staged MHW-vs-MHW turf war
void turf_claim(const uint8_t* em, bool on);                  // in a turf war with a Remnant boss
Vec3 turf_closest(const Monster& m, Vec3 p);                  // the closest point of its hitbox to p

// Render thread: draws the bosses into rtv (before the overlay), and their health bar and damage numbers.
void bosses_render(void* device, void* context, void* rtv, int width, int height);
void bosses_hud();

// The Remnant gun in hand (gunmodel.cpp): placed on the game thread, drawn in the bosses' 3D pass, flash in the HUD.
struct DrawItem;
void gunmodel_tick(const SaveData& s, const Live& l);
void gunmodel_append(std::vector<DrawItem>* items);
void gunmodel_hud();

}  // namespace rh
