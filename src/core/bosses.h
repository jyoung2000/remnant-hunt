// Remnant x MHW mashup core: Remnant II bosses roaming MHW's field zones as creatures of their own. GPL-3.0-or-later.
//
// The bosses are not MHW monsters: the mod runs them (AI, health, attacks, sounds, loot) and draws them with Remnant
// II's own model, textures and animations, read from the player's Remnant II install (render3d.cpp, model.cpp).
// To the rest of the mod they look like monsters (pseudo entries in the snapshot), so the Remnant guns, skills, mods
// and effects work on them unchanged.
#pragma once
#include <vector>

#include "state.h"

namespace rh {

enum BossEvent : int { BOSS_EV_EVADE = 1, BOSS_EV_HURT = 2, BOSS_EV_KILL = 4 };

// Game thread, under S().lock, right after snapshot(): runs the bosses and adds them to l.snap.monsters.
int bosses_tick(SaveData& s, Live& l, double t, float dt);  // returns BossEvent bits
// Damage to a boss (game thread). False when em is not one of the bosses.
bool boss_hit(uint8_t* em, float dmg, const Vec3& at, bool crit, bool weak);
bool is_boss(const uint8_t* em);

// Render thread: draws the bosses into rtv (before the overlay), and their health bar and damage numbers.
void bosses_render(void* device, void* context, void* rtv, int width, int height);
void bosses_hud();

// The Remnant gun in hand (gunmodel.cpp): placed on the game thread, drawn in the bosses' 3D pass, flash in the HUD.
struct DrawItem;
void gunmodel_tick(const SaveData& s, const Live& l);
void gunmodel_append(std::vector<DrawItem>* items);
void gunmodel_hud();

}  // namespace rh
