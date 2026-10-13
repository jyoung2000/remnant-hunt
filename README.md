# Remnant Hunt: Remnant II x Monster Hunter: World

Your Remnant II hero comes through a worldstone into the New World. You hunt Monster Hunter: World's monsters with
Remnant's guns, archetypes, weapon mods and the Dragon Heart, and you can still use MHW's own weapons, armor and
items. Remnant screens look like Remnant; the rest of the game stays Monster Hunter.

## What you need
- **Monster Hunter: World** on Steam, Ver. 15.23.00 (the final PC patch). The mod runs inside it.
- **Remnant II** installed on the same PC. The mod reads Remnant II's names, descriptions, lines and icons from
  your own copy while you play. Nothing from either game is included in this download.
- Windows 10 or 11. DirectX 11 and DirectX 12 mode both work.

## What's in it (v0.2, solo)
- **Worldstone intro**, then **choose your archetype** from Remnant II's starting four (Challenger, Handler, Hunter,
  Medic), with their own names, emblems, skills and perks read from your install.
- **The whole Remnant II arsenal**: all 14 archetypes with their skills, perks and traits, 112 weapons (long guns,
  hand guns and melee weapons), 36 mods, 66 mutators, 225 rings, 103 amulets, 27 relics, 44 relic fragments,
  53 consumables and 120 armor pieces.
- **Remnant menus at MHW's own NPCs** in Astera and Seliana. Stand next to one and press **P**:
  - **Smithy (McCabe):** guns, melee weapons, mods, mutators, armor and gun upgrades (+1 to +10).
  - **Elder Melder (Wallace):** archetype engrams, rings, amulets, relics and relic fragments.
  - **Provisions Manager (Brabus):** consumables for zenny.
  Crafting costs **MHW materials and zenny** from your item box, scaled by rank: starter gear takes low rank ores and
  bones, the best gear takes master rank materials and elder dragon parts.
- **Ward 13 terminal** (press **P** elsewhere in town): your Remnant loadout. Two archetypes (the second adds its
  perks and a second skill on **H**), long gun, hand gun and melee weapon with their mods and mutators, relic, three
  fragments, amulet, four rings, four armor pieces, and consumables to use.
- **Remnant combat on a hunt** (solo):
  - **B** switches to your Remnant guns. Aim with the right mouse button, fire with the left, **R** reloads and
    **X** swaps guns. Shots hit the monster's body, with weak spots on the head and tail.
  - **V** fires your gun's mod once it is charged by the damage you deal (guns or MHW weapons).
  - **G** and **H** use your archetype skills. **T** uses your relic; its charges refill when you return to town.
  - Your rings, amulet, fragments, perks, traits, mutators and armor change your numbers: damage, critical hits,
    weak spots, fire rate, reload, healing, lifesteal, damage reduction and more. Your melee weapon and melee bonuses
    strengthen your MHW weapon's hits.
- Each item's effects are written in the menus in the mod's own words. Remnant II mechanics MHW has no equivalent
  for (movement speed, evade distance, summons walking around) are turned into the nearest thing MHW can do:
  stamina, damage reduction, or damage over time on your target.
- MHW's weapons, armor, items, Palico and quests all still work.

The keys avoid MHW's default PC bindings; every key can be changed in `remnant_hunt/remnant_hunt.ini`. Progress is saved in `%LOCALAPPDATA%\RemnantHunt\save.json`.

## Multiplayer
v0.1 is solo: Remnant combat switches itself off when other hunters are in your quest. Co-op for up to 4 hunters
who all have the mod is planned, with joining through MHW's own sessions and SOS.

## How it works
- A small `dinput8.dll` loader starts the mod when MHW opens.
- The mod draws its screens on top of the game and runs Remnant II's rules on MHW's monsters by calling the game's
  own functions. It never patches MHW's code.
- `r2read.exe` is a separate helper that reads Remnant II's packages from your install, in memory, using the
  open-source ooz decoder. It never writes game files anywhere.

## Uninstall
Delete `dinput8.dll` and the `remnant_hunt` folder from the Monster Hunter: World folder.

## License and credits
GPL-3.0-or-later; remixes are welcome under the same license. The full source is in `source/remnant-hunt-source.zip`. Credits are in `licenses/NOTICE.txt`. Built with Claude (Anthropic) for Jalon.
