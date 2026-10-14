# Remnant Hunt: Remnant II x Monster Hunter: World

Your Remnant II hero comes through a worldstone into the New World. You hunt Monster Hunter: World's monsters with
Remnant's guns, archetypes, weapon mods and the Dragon Heart, and you can still use MHW's own weapons, armor and
items. Remnant screens look like Remnant; the rest of the game stays Monster Hunter.

## Install
1. Download **`RemnantHuntSetup-<version>.exe`** from [Releases](https://github.com/jyoung2000/remnant-hunt/releases/latest).
2. Close Monster Hunter: World and run it. It finds Monster Hunter: World and Remnant II in your Steam libraries
   (or use **Browse**), then **Install** puts `dinput8.dll` and the `remnant_hunt` folder next to
   `MonsterHunterWorld.exe`. Everything is inside the exe; nothing is downloaded.
3. Press **Play**, or start the game from Steam as usual.

Windows SmartScreen may say the installer is from an unknown publisher (it is not code-signed): choose
**More info → Run anyway**. Run it again later to **Update** or **Uninstall**.

Or, from **PowerShell**:

```powershell
irm https://raw.githubusercontent.com/jyoung2000/remnant-hunt/main/install.ps1 | iex
```

- **By hand:** download `remnant_hunt-<version>.zip` from [Releases](https://github.com/jyoung2000/remnant-hunt/releases)
  and extract it into your Monster Hunter: World folder (the one with `MonsterHunterWorld.exe`).
- **Another mod loader:** Stracker's Loader also uses `dinput8.dll`. Both installers stop rather than replace it;
  tick **Replace it** in the setup window (or run `install.ps1 -Force`) to keep yours as `dinput8.dll.bak`.
- **Game in another folder:** **Browse** in the setup window, or `.\install.ps1 -GameDir "D:\Games\Monster Hunter World"`.
- **Uninstall:** **Uninstall** in the setup window, `.\install.ps1 -Uninstall`, or delete `dinput8.dll` and the
  `remnant_hunt` folder.

## What you need
- **Monster Hunter: World** on Steam, Ver. 15.23.00 (the final PC patch). The mod runs inside it.
- **Remnant II** installed on the same PC. The mod reads Remnant II's names, descriptions, lines and icons from
  your own copy while you play. Nothing from either game is included in this download.
- Windows 10 or 11. DirectX 11 and DirectX 12 mode both work.

## New in v0.4
- **Turf wars.** A Remnant boss that meets a large MHW monster may fight it: it roars, closes in and attacks with its
  own moves, and the MHW monster turns to face it and fights back with its own. Two large MHW monsters that meet near
  you may stage a turf war too, even pairs MHW never gave one. The one hurt worse goes down. Pairs that already have
  a turf war of MHW's own are left to MHW.
  MHW doesn't say which of a monster's moves are attacks, so the mod learns them from the monster's fights with you.
  Until it has seen a monster's moves, that monster's blows in a turf war land without an animation of their own.
- **First person when aiming down sights.** Hold right-click with a Remnant gun and the view moves to the hunter's
  eyes, with the gun's own model in front of you. Firing from the hip stays third person.
- **Remnant's stamina effects use MHW's stamina bar.** Gear and buffs for max stamina, stamina regeneration and
  stamina cost act on MHW's own stamina. Heavier Remnant armor makes stamina costs higher. A Remnant evade is a real
  dodge that spent stamina, so it now works on a gamepad too. The mod finds where MHW keeps your stamina during your
  first dodges in a hunt.
- **Archetypes, perks, traits and trait points work as in Remnant II.** Slaying or capturing monsters and defeating
  Remnant bosses earns experience.
  - **Archetype levels:** archetypes level from 1 to 10 on Remnant II's experience table. The two equipped archetypes
    share the experience, and a level-10 archetype takes none.
  - **Skills:** each archetype's skills unlock at levels 1, 5 and 10.
  - **Perks:** the prime perk grows at levels 5 and 10. The damage perk grows with every level. Team, utility and
    relic perks unlock at levels 2, 3 and 4 and are upgraded five levels later. The prime perk only works from the
    primary archetype.
  - **Archetype traits:** each equipped archetype's trait gains a point per archetype level. When an archetype reaches
    level 10, its trait can be used with any archetype for trait points.
  - **Core traits:** your primary archetype puts its 5 core points into Vigor, Endurance, Spirit and Expertise.
  - **Trait points:** every Remnant level earns a trait point to spend in **TRAITS** at the Ward 13 terminal, up to
    85. Each trait goes up to 10, and you can take your points back at any time.
  - **Learning traits:** learn more traits from the Elder Melder or from Remnant bosses. All 14 archetypes have their
    3 skills and 5 perks, and all 43 traits are in.
  - **Weapon upgrades:** weapons upgrade to +20, or to +10 for special weapons with a mod of their own.
- **Prisms**, as in Remnant II's The Dark Horizon. Craft them at the Elder Melder; the first Remnant boss you defeat
  also leaves one. The equipped prism gains experience as you hunt. Each prism level offers three choices: a new relic
  fragment for one of its five slots, one more level for a fragment (up to +10), or a fusion of two fragments that are
  both at +5 or higher. With all five fragments at +10, it offers a legendary bonus. A prism at +10 or higher can be
  cleansed to start over.
- **Difficulty for the Remnant monsters.** Pick Survivor, Veteran, Nightmare or Apocalypse at the Ward 13 terminal.
  It changes their health, damage, pace and statuses, how hard they are to stagger, how soon they enrage and how
  often they chain attacks. Harder difficulties give more experience.
- **Each Remnant boss fights its own way:**
  - Brutes close in and hit hard.
  - Hunters (Ravager, Lydusa, Venom) circle you, pounce and back off.
  - Duelists (Faerin, the One True King, Gwendil) stay close, chain attacks and step around you.
  - Ranged bosses (Phantom, Mother Mind) keep their distance and back away when pressed.
  - Casters (Nightweaver, Sunken Witch) hold a middle distance and favour area attacks.
  - Juggernauts (Annihilation, Kiln) advance relentlessly and shrug off flinches.
  - Sentinels hold their ground.
  - All of them enrage at low health.
- Settings in `remnant_hunt.ini`: `[turf] enabled`, `mhw_vs_mhw`, `facing`; `[visuals] first_person`,
  `fp_eye_height`; `[stamina] enabled`.

## New in v0.3
- **Remnant II's bosses roam MHW's hunting zones** as creatures of their own, drawn with Remnant II's own models,
  textures, animations and sounds: 29 of them, from the Corrupted Ravager and the Nightweaver to Annihilation, the One
  True King, Lydusa and Alepsis-Taura. They are scaled to the zone's rank, and the strongest only appear in high or
  master rank. Press **F9** in the Training Area to bring them out one by one.
- **Stagger** for the bosses from both games' weapons; MHW's bows and bowguns hit them too.
- **Remnant-style health bars** for the bosses and for MHW's monsters, damage numbers, and a tracker for Remnant
  creatures in the zone.
- **Remnant statuses** (BLEEDING, BURNING, CORRODED, OVERLOADED, EXPOSED, SLOWED, MARKED) on monsters and on you,
  with Remnant II's icons; armor resistances and cures. Archetype skills such as War Stomp, Miasma, Eruption and
  Hunter's Mark work as in Remnant II.
- **Remnant gun models** in gun mode with their own shot and reload animations and Remnant's muzzle flash.
- The hunter flinches from Remnant hits with MHW's own reactions, learned from MHW's monsters as you play.

## What's in it (solo)
- **Worldstone intro**, then **choose your archetype** from Remnant II's starting four (Challenger, Handler, Hunter,
  Medic), with their own names, emblems, skills and perks read from your install.
- **The whole Remnant II arsenal**: all 14 archetypes with their skills, perks and traits, 112 weapons (long guns,
  hand guns and melee weapons), 36 mods, 66 mutators, 225 rings, 103 amulets, 27 relics, 44 relic fragments,
  53 consumables, 120 armor pieces, 43 traits, 7 prisms with 23 fusions and Remnant II's legendary
  prism bonuses.
- **Remnant menus at MHW's own NPCs** in Astera and Seliana. Stand next to one and press **P**:
  - **Smithy (McCabe):** guns, melee weapons, mods, mutators, armor and gun upgrades (+1 to +10).
  - **Elder Melder (Wallace):** archetype engrams, rings, amulets, relics and relic fragments.
  - **Provisions Manager (Brabus):** consumables for zenny.
  Crafting costs **MHW materials and zenny** from your item box, scaled by rank: starter gear takes low rank ores and
  bones, the best gear takes master rank materials and elder dragon parts.
- **Ward 13 terminal** (press **P** elsewhere in town): your Remnant loadout. Two archetypes (the second adds its
  perks and a second skill on **H**), long gun, hand gun and melee weapon with their mods and mutators, relic, three
  fragments, amulet, four rings, four armor pieces, and consumables to use. Your traits, your prism, and the
  difficulty of the Remnant monsters are also set here.
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
Remnant Hunt is solo for now: Remnant combat switches itself off when other hunters are in your quest. Co-op for up to 4 hunters
who all have the mod is planned, with joining through MHW's own sessions and SOS.

## How it works
- A small `dinput8.dll` loader starts the mod when MHW opens.
- The mod draws its screens on top of the game and runs Remnant II's rules on MHW's monsters by calling the game's
  own functions. It never patches MHW's code.
- `r2read.exe` is a separate helper that reads Remnant II's packages from your install, in memory, using the
  open-source ooz decoder. It never writes game files anywhere.

## Uninstall
Run the setup again and press **Uninstall**, or delete `dinput8.dll` and the `remnant_hunt` folder from the
Monster Hunter: World folder.

## License and credits
GPL-3.0-or-later; remixes are welcome under the same license. The full source is in `source/remnant-hunt-source.zip`. Credits are in `licenses/NOTICE.txt`. Built with Claude (Anthropic) for Jalon.
