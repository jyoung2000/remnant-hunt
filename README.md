# Remnant Hunt: Remnant II x Monster Hunter: World

Your Remnant II hero comes through a worldstone into the New World. You hunt Monster Hunter: World's monsters with
Remnant's guns, archetypes, weapon mods and the Dragon Heart, and you can still use MHW's own weapons, armor and
items. Remnant screens look like Remnant; the rest of the game stays Monster Hunter.

## What you need
- **Monster Hunter: World** on Steam, Ver. 15.23.00 (the final PC patch). The mod runs inside it.
- **Remnant II** installed on the same PC. The mod reads Remnant II's names, descriptions, lines and icons from
  your own copy while you play. Nothing from either game is included in this download.
- Windows 10 or 11. DirectX 11 and DirectX 12 mode both work.

## What's in it (v0.1, solo)
- **Worldstone intro**, then **choose your archetype**: Hunter, Medic, Challenger or Gunslinger, with Remnant II's
  own names, emblems, prime perks and skills.
- **Ward 13 terminal** (press **H** in Astera, Seliana or your room):
  - **Brabus** sells long guns and hand guns for scrap: Western Classic, Tech 22, Blackmaw AR-47, Coach Gun,
    Huntmaster M1 and Widowmaker.
  - **McCabe** upgrades your guns (+1 to +5).
  - You can change archetype here.
  - Both NPCs greet you with their own lines from Remnant II.
- **Remnant combat on a hunt** (solo):
  - **G** switches to your Remnant guns. Aim with the right mouse button, fire with the left, **R** reloads and
    **X** or the mouse wheel swaps guns.
  - Shots hit the monster's body, with weak spots on the head and tail. Damage numbers show in MHW's own style.
  - Each long gun has its **mod** (Hot Shot, Concussive Shot, Corrosive Rounds, Healing Shot). It charges as you deal
    damage, with guns or MHW weapons. Press **B** to use it.
  - **T** uses your archetype skill: Hunter's Mark, Wellspring, Juggernaut or Bulletstorm.
  - **V** uses the **Dragon Heart**. Its charges refill when you return to town.
- **Scrap**, Remnant's currency, comes from damaging and slaying large monsters.
- MHW's weapons, armor, items, Palico and quests all still work. Your archetype's perks and skill also boost your MHW
  weapon hits.

Every key can be changed in `remnant_hunt/remnant_hunt.ini`. Progress is saved in `%LOCALAPPDATA%\RemnantHunt\save.json`.

## Multiplayer
v0.1 is solo: Remnant combat switches itself off when other hunters are in your quest. Co-op for up to 4 hunters
who all have the mod is planned, with joining through MHW's own sessions and SOS.

## How it works
- A small `dinput8.dll` loader starts the mod when MHW opens.
- The mod draws its screens on top of the game and runs Remnant II's rules on MHW's monsters by calling the game's
  own functions. It never patches MHW's code.
- `r2read.exe` is a separate GPL-3.0 program that reads Remnant II's packages from your install, in memory, using the
  open-source ooz decoder. It never writes game files anywhere.

## Uninstall
Delete `dinput8.dll` and the `remnant_hunt` folder from the Monster Hunter: World folder.

## Credits
See `licenses/NOTICE.txt`. Built with Claude (Anthropic) for Jalon.
