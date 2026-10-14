# session-notes: remnant-hunt
Project: C:\Users\jalon\projects\remnant-hunt (git). Read MODLOG.md "Build state" / "Next steps" first.
Toolchain (device VM): $HOME/tc/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64/bin on PATH, then `make`.
Sheets are the source of truth: edit sheets/*.json -> `python3 tools/preflight.py` -> `make` (runs tools/gen.py).
Dev install: tools/install_dev.sh (MHW at D:\SteamLibrary\steamapps\common\Monster Hunter World).
Remnant II at C:\Program Files (x86)\Steam\steamapps\common\Remnant2 (never copied from).

## 2026-10-13 midday (v0.2)
- Regenerate sheets: `python3 tools/r2catalog.py <Paks> .check/catalog.json` -> `python3 tools/author_arsenal.py --paks <Paks>
  --keep-ids .check/sheets_v01_backup` -> `python3 tools/author_sounds.py <Paks>` -> `python3 tools/verify_r2.py <Paks> sheets`
  -> `make`. Toolkit numbers: .check/toolkit.json from tools/r2toolkit_read.py (clone joshpayette/remnant2-toolkit).
- Host test: `g++ -std=c++17 -Isrc/core tests/test_effects.cpp src/core/effects.cpp -o build/test_effects && build/test_effects`.
- Install works while MHW runs (tools/install_dev.sh): core DLL and r2read run from copies; a locked r2read.exe is left as
  r2read_next.exe and swapped in by the next game session. dinput8.dll still needs the game closed (rarely changes).
- MHW was still running the 10:34 build all morning (r2read.exe/dinput8.dll locked): Jalon must close it fully.
- Pending from the diagnostic: NPC spots (menus.json), material ids (materials.json), item_name_stride, item box check,
  item bar field, action/animation ids.

## 2026-10-13 afternoon: bosses, bars, tracker (commit 7cfd4bc)
- Remnant II bosses: sheets/bosses.json + boss_attacks.json, src/core/bosses.cpp (AI, hits, loot), model.cpp (pose),
  render3d.cpp (D3D11 draw at Present), overlay.cpp (ClearDepthStencilView hook picks MHW's scene depth; ini
  [bosses] scene_depth=0 turns it off). Debug key F9 ([keys] boss=). Ravager mesh faces +Y, right = -X, 13 m long.
- Hit windows measured by tools/attack_timing.py (limb speeds). MHW melee on bosses = attack press + reach (approx).
- Bars: Remnant boss bar (top), docked bar for the MHW monster being fought, bars over others; names from
  sheets/mhw_monsters.json by model id (logged as "monsters: met emXXX_YY").
- Map: Jalon chose "compass now, minimap later". Tracker strip done; minimap markers need the minimap's zoom and
  rotation from memory (diagnostic research).
- Untested in game. Next: Jalon tests F9 in the Training Area; check log for render3d/model/anim/scene depth lines.

## 2026-10-13 13:08 request: all bosses, rank scaling, stagger, cross-game damage, guns
- tools/author_bosses.py + tools/boss_roster.json author sheets/bosses.json and boss_attacks.json from the install
  (29 bosses, 317 attacks; cache in .check/bosses/, `--write` merges). Not ported: Many Faces and the Red Prince
  (their animations use UE's own codec, not ACL), Labyrinth Sentinel (static meshes), Spriggan (no body mesh).
- r2mesh.cpp: cloth mapping entries are 64 bytes (cloth meshes failed before). `make linux` builds the Linux r2read.
- Rank: zone 108/109 = MR, else largest MHW monster HP vs tuning rank_hp_hr / rank_hp_mr. Boss min_rank from threat
  (1-2 LR, 3 HR, 4-5 MR); HP = tuning boss_hp_<rank> x hp_mult; damage x boss_dmg_<rank>.
- Stagger: meter (weak spots x2, heavy MHW weapons more), thresholds grow x1.45, forced at 2/3 and 1/3 HP.
- MHW ranged weapons hit bosses along the camera line; weapon type read via SPL's GetWeaponType pattern (mhw.cpp,
  logged "mhw: weapon type ..."). MHW monsters flinching from Remnant guns: research.cpp (dev_research.txt) first.
- research.cpp (diagnostic only): hunter joint matrices, monster flinch counters, save-slot rank ints.
- Guns: weapons.json mesh / mesh_fwd / mesh_tex (tools/author_gun_meshes.py, 95/112). gunmodel.cpp draws the gun at
  an estimated right hand until the hand joint is known; Remnant muzzle flare texture on shots.

## 2026-10-13 13:48 request: gun anims, armor, Remnant effects, flinch, abilities
- Effect language: statuses (inflict:/use:inflict/imbue/aoe/mark/eruption/cure), res_* and vs_* stats; preflight
  updated; tests/test_effects.cpp: 846 strings parse. tools/author_statuses.py writes mods/guns/rings/amulets/
  consumables/armor clauses from the toolkit descriptions (numbers only) and 10 skills' real mechanics.
- remnant.cpp: status runtime (DoTs, EXPOSED +15%, MARKED +10% crit, SLOWED bosses x0.7, eruption, cures,
  resistances, dodge shortens BURNING). Boss attacks inflict statuses (boss_attacks.inflict, tuning boss_status_frac).
- HUD: Remnant status icons (UI_Icons_Status) on every bar and for the hunter; status particles on monsters.
- reactions.cpp: learns MHW's hunter hit reactions from MHW's own hits (learned_reactions.txt) and plays them via
  ActionController::DoAction (SPL pattern) when Remnant bosses hit. Untested.
- Guns: weapons.json skeleton/anim_fire/anim_reload (24 shot, 34 reload anims); gunmodel plays them.
- research.cpp now on by default ([research] enabled=0 to stop): dev_research.txt.
- Still blocked on data from play: armor visuals on the hunter and gun in the hand (joint matrices, hunter facing),
  MHW monsters flinching from Remnant damage (flinch counters), Remnant player arm animations (retarget).

## 2026-10-13 15:31 request: GitHub repo + GUI installer
- Repo: https://github.com/jyoung2000/remnant-hunt (public), pushed from the container clone /home/claude/remnant-hunt
  (the device has no GitHub credentials; device commits travel as git bundles).
- GUI installer: src/installer/setup.cpp + tools/make_installer.py, `make installer` ->
  out/RemnantHuntSetup-<ver>.exe (payload = package.py files() as RCDATA, game-data check). Tested under Wine.
- GitHub releases are blocked for Claude sessions (403): Jalon uploads the exe + zip + release notes by hand.

## 2026-10-13 16:58-17:11 requests: turf wars; MHW stamina bar; first person when aiming
- Jalon chose: staged turf wars with learned moves (fallback: blows without animation), and allowed facing writes.
- turf.cpp: learns MHW monster attacks (action at the hunter hit, hit time, reach) and flinches (action right after a
  heavy non-mod hit) per emXXX_YY into learned_turf.txt; detects MHW's own turf-war pairs (both hurt >=2% within
  1.5 s) and leaves them alone; self-checks (action plausibility, quaternion unit, forward from movement, DoAction
  takes, writes stick) gate every write/call. MHW-vs-MHW: square off 1.6 s, 3-5 blows, loser flinches twice.
  bosses.cpp: Boss.foe = an MHW monster takes the hunter's place in think(); boss_turf() runs the MHW side.
  Tuning turf_*; ini [turf] enabled/mhw_vs_mhw/facing. Never finishes a monster (keeps 5%).
- First person (mhw.cpp apply_first_person after the camera move): eye = hunter pos + fp_eye_height, 22 cm ahead;
  writes camera pos/target and the view matrix translation (rotation kept). gunmodel.cpp: viewmodel when fp > 0.5.
  Logs whether the view matrix eye matched the camera (first in-game check to read).
- Stamina: stats stamina_max / stamina_regen / stamina_cost (re-authored by tools/author_stamina.py); stamina.cpp
  finds MHW's stamina in the hunter object (drop 8-50 then monotonic refill, rests at peak, max float within 0x40)
  -> learned_stamina.txt; applies bonuses relative to MHW's own changes; Remnant evade = stamina spend >= 12.
- Tests: make test-logic (Wine) passes; test_effects 846 parse; preflight clean. Version 0.4.0, commit 9a8b2ca.
- Next: in-game logs to read: "turf:", "first person:", "stamina:" lines in remnant_hunt.log; learned_turf.txt.

## 2026-10-13 19:19-19:29 requests: traits & prisms & skill points; difficulty; per-boss behaviour
- "Skill points" read as Remnant II's trait points. progress.cpp: XP (MHW kills by max HP x xp_per_hp, small x0.5,
  Remnant bosses x1.5 x difficulty xp_mult; Scholar = stat xp), Remnant level curve 3000 + 250n, trait points =
  5 + level (cap 85); traits 0..10 points, scaled effects; prisms (R2 Dark Horizon rules: 3 offers per level, new
  fragment / +1 / fusion of two >= +5, legendary at all-five +10, cleanse from +10; XP 5000 + 300L, 50000 for +51).
- Sheets: traits.json rebuilt (42, kind core|archetype|trait, sold at the melder), prisms/fusions/legendaries.json
  (tools/author_progression.py; names from PrismStone data tables via .check/prism_texts.json made on the device).
  Legendaries with no MHW meaning have no effects and are not offered; Traitor / Prime Time handled in code.
- Rewards: first Remnant boss -> Prism of Voracity; bosses teach a trait (loot_trait_chance). Melder tabs TRAITS, PRISMS.
- Difficulty: difficulties.json (Survivor/Veteran/Nightmare/Apocalypse; hp, dmg, tempo, stagger, enrage, combo,
  status, xp), chosen at Ward 13 (SaveData.difficulty), applied at spawn. Boss styles (bosses.json style/prefer_m/
  combo via tools/author_boss_styles.py): brute, hunter, duelist, ranged, caster, juggernaut, sentinel; enrage roar.
- Tests: make test-logic now also runs tests/test_progress.cpp (all pass).
