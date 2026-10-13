# Melty handoff: Remnant Hunt v0.1.0

Everything below is ready except the steps that need melty.gg, which the cloud session's network policy blocks.
A Claude Code session on this PC (which can reach melty.gg) can finish from here.

## Prompt for Claude Code on this PC
Open Claude Code in `C:\Users\jalon\projects\remnant-hunt`, paste Melty's **Publish** prompt for this mashup, and add:

> The mashup is already built in this folder. Read MELTY_HANDOFF.md, MODLOG.md and README.md first and do not rebuild
> or redesign it. Release: out/remnant_hunt-0.1.0.zip. Listing text, games, license and the recipe draft are in
> MELTY_HANDOFF.md. Run the Melty steps (game_info, search_mashups, inspect_package, validate_recipe, one_click_check,
> create_mod, upload, test_release with screenshot) and stop before publish so I can approve it.

## Listing
- **Title:** Remnant Hunt
- **Tagline:** Your Remnant II hero, guns and archetypes on the hunt in Monster Hunter: World.
- **Description:** Step through a worldstone into the New World as your Remnant II hero. Choose Hunter, Medic,
  Challenger or Gunslinger, then hunt Monster Hunter: World's monsters with Remnant guns, weapon mods, archetype
  skills and the Dragon Heart, alongside MHW's own weapons, armor and items. At the Ward 13 terminal (P), Brabus
  sells guns for scrap and McCabe upgrades them, each greeting you with their own lines. All Remnant II names, lines
  and icons are read from your own Remnant II install. Solo for now; co-op is planned.
- **Games:** Monster Hunter: World (primary, slug `monster-hunter-world`, Ver. 15.23.00) and Remnant II (companion,
  the `custom-` slug search_games returns for the installed copy).
- **Players:** solo (v0.1). Do not list as multiplayer yet.
- **License:** GPL-3.0-or-later. **Remixing:** allowed.
- **Credits:** Jalon (author), built with Claude (Anthropic). ooz (Powzix, GPL-3.0), Dear ImGui (MIT), MinHook
  (BSD-2), Montserrat and EB Garamond (OFL). MHW research: justbustin/minecraft-crossover-bridge (MIT), HunterPie
  (Apache-2.0). Toolkit: rehan-remade/universal-modder.

## Release contents (out/remnant_hunt-0.1.0.zip, paths relative to the MHW folder)
- `dinput8.dll` -> `{game}/dinput8.dll` (loader; forwards to Windows' dinput8)
- `remnant_hunt/` -> `{game}/remnant_hunt/` (mod DLL, r2read.exe, fonts, licenses, README, full source zip)
- No files from either game. Uninstall = delete those two items.

## Recipe draft (check against validate_recipe; the schema there wins)
```json
{
  "games": [
    {"slug": "monster-hunter-world", "role": "primary"},
    {"slug": "<remnant ii custom- slug>", "role": "companion"}
  ],
  "components": [
    {"fileName": "remnant_hunt-*.zip", "files": [
      {"from": "dinput8.dll", "to": "{game}/dinput8.dll"},
      {"from": "remnant_hunt/", "to": "{game}/remnant_hunt/"}
    ]}
  ],
  "launch": {"kind": "game", "args": ["--remnant2", "{game:<remnant ii custom- slug>}"]},
  "test": {"checks": "see melty.log lines below"}
}
```
The mod finds Remnant II from `--remnant2 <folder>` on MHW's command line, then the `REMNANTHUNT_R2` environment
variable, then `[paths] remnant2=` in `remnant_hunt/remnant_hunt.ini`, then Steam's library folders.

## Test signals (remnant_hunt/melty.log, one line per check)
- `check hooks ok 0-failed`: every MHW 15.23 address passed its signature check
- `check game-tick ok`: the per-frame game-thread callback runs
- `check overlay ok dx11|dx12`: the screens draw on the game's frame
- `check remnant2 ok N-cells 0-missing`: Remnant II content resolved from the player's install
- `check shot-landed ok`: a Remnant gun shot hit a monster (only with MELTY_TEST=1 or `[test] enabled=1` in the ini,
  and needs play: Training Area, press B, shoot the dummy)
Detailed log: `remnant_hunt/remnant_hunt.log`.

## Still to do
1. First in-game run on this PC (not done yet: the remote screen was unavailable). Fix anything the log shows.
2. A real gameplay screenshot for the listing (intro or archetype screen, then the gun HUD in the Training Area).
3. The Melty steps above, then publish after Jalon approves.
