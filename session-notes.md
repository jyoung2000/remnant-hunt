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

## 2026-10-13 19:40 request: classes and traits exactly as Remnant II; Remnant features as in R2; MHW-looking GUI
- Archetypes (progress.cpp): levels 1-10 on R2's XP table (5000 ... 94770), XP split between the two equipped
  (a level-10 one takes none); skills unlock at 1/5/10 (skill_index); perks by type (perks.json type): prime 0.6/0.8/1.0
  at 1/5/10, damage level/10, team/utility/relic unlock 2/3/4 at 0.6 and full at 7/8/9; prime only from the primary.
- Traits: each equipped archetype's trait +1 per archetype level (auto); primary's 5 core points (archetypes.json
  core_*, from the toolkit); archetype traits usable by others for points once that archetype is level 10; free
  points from Remnant levels (start 0, cap 85). 43 traits (Gifted added); Warden's 4 perks and Explorer's Scavenger
  added; perk effects re-written by hand at their top level (tools/author_archetypes.py).
- Weapons: +20 for regular, +10 for special (own mod) weapons; regular levels are half steps of upgrades.json.
- Not added: Gambler and Paragon (World_DLC4 in the install) are unreleased placeholders ("TBD" text).
- GUI: Monster Hunter menu style everywhere (palette, gold double frame with diamonds, amber cursor buttons, section
  headings with diamonds, RARE n in MHW colours). The Remnant-style health bars stay (asked for earlier).
- tests/preview_ui.cpp: preview harness for the screens under Wine (not finished: needs stubs).

## GUI check (software preview)
- `make preview-ui` builds build/preview_ui.exe; `wine preview_ui.exe <0-4> <dir with fonts\> out.bmp` renders terminal/traits/prism/archetype/melder with a sample save.
- Fixed from the preview: window padding inherited by child panels, label() breaking SameLine rows, progression block moved to the terminal's HUNTER panel, archetype-trait "+" only from archetype level 10 (trait_can_set), Elder Melder lists archetype engrams, leave button sized to its text.

## 2026-10-13 evening: native Windows build, GUI production pass (branch claude/remnant-hunt-gui-debug-78c4ff)
- Build now runs on Jalon's PC without WSL/Wine: llvm-mingw 20261006 Windows release in C:\Users\jalon\tc\, GNU make
  from winget (ezwinports.make). `tools/build_native.sh` = make -j8; `... test` = the three logic tests run natively
  (all pass at 42049a1); `... preview` = screens 0-5 to build/preview/screen_N.png (tools/bmp2png.py converts).
- Model routing through Jev (jev_decide ROUTE): first BLOCKED on the missing toolchain, PROCEED once it built. Split:
  Haiku = web research (scratchpad gui_research.md) and README; Sonnet = ui.cpp polish from a written spec and a
  read-only correctness review of the rest of src/core; Fable = spec, render review, fixes of review findings, notes.
- GUI findings from the before renders (build/preview/before_N.png):
  terminal: slot rows 44 px clipped their second line; weapon icons 80 wide pushed labels; three fallback vendor
  buttons all read "VISIT WALLACE" (preview stub, but the role was missing); the right HUNTER panel was mostly empty;
  button heights were 30/34/36/38/40. traits: three-line explanation paragraph; "-" / "+" could collide with pips;
  the selected trait sat under the archetype-trait block. prism: STRENGTHEN cards had no effect lines (uneven cards);
  fragments as a plain list. archetype: USE THIS SKILL under the bullets; perk kind text ran into the name.
  melder: ImGui's stock tab bar; empty detail panel until a click; zenny placed from the window width.
  HUD: no preview existed, so it was never checked.
- In-game test checklist for Jalon (send back remnant_hunt.log and the learned files):
  1. Start MHW, reach Astera. Expect the Worldstone intro once, then the archetype choice (if the save is fresh).
  2. P at Ward 13 -> terminal: scroll the slot list, pick LONG GUN, equip a gun, REMOVE a ring, BACK TO HUNTER; change
     difficulty; open TRAITS, spend and refund a point; open PRISM. Esc from each. Check nothing is clipped at 2560x1440.
  3. P near McCabe / Wallace / Brabus (or the VENDOR buttons): tabs with Q/E, search, craft one ring, buy a consumable.
  4. B then RMB: first person; log lines "first person:" (eye vs camera), "mhw: weapon type".
  5. F9 in the Training Area: boss spawns; log "render3d", "model", "anim", "scene depth"; its bar and the compass strip.
  6. Fight an MHW monster near a boss: "turf:" lines and learned_turf.txt.
  7. Sprint/dodge: "stamina:" lines, learned_stamina.txt. Take hits from a boss: learned_reactions.txt.
  8. Kill something: archetype XP and trait points rise on the terminal (HUNTER panel).
- Done this session (all built, `tools/build_native.sh test` passes: turf/stamina, progress, 875 effect strings):
  ui.cpp: list_row (flat rows, amber cursor + diamond on the selection), section/label(rule), tab_strip (MHW tabs, Q/E),
  foot strip (kFoot 56, BACK always right, Esc hint bottom-left), button heights 40/34/26, help_mark tooltips instead
  of paragraphs, zenny_str, auto-select of the first row in every list; terminal slots grouped and 52 px, EQUIPPED grid
  and vendor buttons by role; traits/prism/archetype/melder per scratchpad spec; HUD restyled (gun panel with the
  accent bar, 60 px skill boxes, toast). NPC action button pinned under a scrolling detail. Preview screens: 0 terminal,
  1 traits, 2 prism, 3 archetype, 4 melder, 5 HUD, 6 terminal with a slot picked, 7 smithy.
  Core fixes from the review (scratchpad core_review.md): json_strings no longer loops on an unterminated string;
  zero-sample animations rejected; save_write reports failures and keeps dirty; an unreadable save.json is copied to
  save.json.bad before anything overwrites it; craft/buy/equip save at once; r2client never overwrites a text the
  render thread may hold, caps replies at 64 MB and goes dead on a short read; r2_stop kills r2read before locking;
  texture size checked; DX12 make_targets retries after a failure; render3d stops leaking after a failed init; boss
  consumable loot goes to stock; prism offer rows clamped; install_game_tick logs its reason; key-ups always reach the
  game; weapon levels >= 0; reactions range-check actions; rand seeded; bounded resource-path read; 256 vtable slots.
- TEMPORARY until dev_items.txt fills materials.json: recipe materials with mhw_item_id 0 are skipped (zenny still
  charged) so crafting works at all. Remove the remove_if in remnant.cpp handle() and the have<0 rule in ui.cpp
  recipe_box once the ids are in.
- For Jalon to decide (not changed): the mod writes MHW's zenny and item counts when crafting (inventory.cpp), which is
  the v0.2 design but not on the approved-writes list; the stamina offset comes from a heuristic scan and is written to;
  mod_game_tick holds the Shared lock across I/O and one-time init (a Present hitch on first use).
- Still not confirmed in game: everything since v0.2 (checklist above).
- Assets: tools/check_assets.py drives build/win/r2read.exe against the real Remnant II install for every asset the
  sheets and bosses.cpp name. Result: 3610 load, 0 fail (1850 texts, 833 textures, 3 names, 115 meshes, 58 skeletons,
  360 animations incl. all boss attacks, 391 sounds). Two data bugs fixed on the way: difficulties.json icons and the
  seven HUD status icons in bosses.cpp carried ".uasset", which the TEX request adds again, so they never loaded.
  Run it after any sheet change: `python -I tools/check_assets.py "<abs path>\build\win\r2read.exe" "<Remnant2 dir>"`.
  Not covered (needs the game): the DX11 upload of BC7 textures and the 3D draw of meshes; the preview stubs icons.

## 2026-10-13 late: Jalon's approvals
- Approved as designed: (1) crafting spends MHW zenny and takes MHW materials from the item box / pouch (inventory.cpp),
  now on the list of allowed game-data writes; (2) the stamina offset from stamina.cpp's scan is an allowed write;
  (3) mod_game_tick keeping the Shared lock across I/O and first-use init stays for now.
- Asked: crafting must work as intended -> fill materials.json mhw_item_id from public MHW item id tables (two sources
  agreeing), keep the zenny-only fallback for any id still 0, and test the craft path on the host.
- Crafting: materials.json now maps all 15 classes to MHW items (ids from mhw-db.com, which agrees with the known
  Potion = 1 / Mega Potion = 2 and whose ids index itemData.itm like MHWISaveEditor does): Iron Ore 116, Machalite 117,
  Dragonite 118, Fucium 120, Earth Crystal 121, Monster Bone S/M/+ 147/148/150, Keenbone 151, Hardbone 152, Elder
  Dragon Bone 153, Elder Dragon Blood 174, Eltalite 604, Solidbone 668, Spiritvein Gem 990. tuning.item_name_stride = 2.
  In-game confirmation: the COST rows at the Smithy must show MHW's own item names ("5 x Dragonite Ore", "have N");
  if a name is wrong or blank, the id or stride is off (dev_items.txt settles it). The zenny-only fallback for id 0
  stays in remnant.cpp handle() and ui.cpp recipe_box as a safety net.
- tests/test_craft.cpp (Makefile test-logic and build_native.sh test): a fake save slot with a pouch and an item box;
  containers found by shape, counts across both, box taken first, pouch slot cleared when empty, zenny bounds, all
  742 recipes (2116 needs) parse to real materials, every material mapped. Passes.
- GUI pass 2, matched against real MHW World screenshots (Smithy > Forge Equipment): each column is its own
  translucent panel with a 28 px title band (panel_begin), breadcrumb top-left ("SMITHY > REMNANT WARES"), zenny
  plate top-right, stat rows with right-aligned values, REQUIRED MATERIALS / REQUIRED COST sub-panels next to the
  item name (have-counts red when short), pill tabs, a centred hint pill at the foot with the prompts at the right,
  one thin frame line instead of the double frame, selection marked by World's small triangle. HUD unchanged.
  Jalon confirmed the target is Monster Hunter: World (not Wilds).
- Icons end to end: src/core/texture.cpp now holds the Direct3D 11 upload (overlay.cpp calls it), and
  tests/test_textures.cpp runs the real r2client + r2read.exe against the install and uploads 385 sampled icons
  (archetypes, skills, traits, weapons at 128 and 512, mods, mutators, rings, amulets, relics, fragments,
  consumables, armor, difficulties, UI art) to a real D3D11 device: 385/385, formats BC7/BC3/BC1. Runs from
  `tools/build_native.sh test` when Remnant II is installed. Traits gained their Remnant II card icons (37 of 43;
  fortify, untouchable, fitness, rugged, scholar, wayfarer have no card asset in the install) and show them in the
  list and the detail. Asset check: 3647 load, 0 fail.
- Release: `tools/build_native.sh package` and `... installer` rebuild out/remnant_hunt-0.4.0.zip and
  out/RemnantHuntSetup-0.4.0.exe; sha256s in out/release-notes-0.4.0.md (out/ is gitignored).
- Palette (Jalon): translucent black plates with World's pale-blue outlines (col::frameHi/frameLo/line/mhwGold), gold
  titles and the amber cursor kept. Preview draws real icons now (tests/preview_ui.cpp starts r2read.exe and decodes
  BC7/BC3/BC1 with third_party/bcdec, dev-only). Melty: draft mashup remnant-hunt created; melty_log mirrors the
  startup checks (hooks, game-tick, overlay, remnant2) into Melty's test checks file. Previous in-game log (10:34
  today, old build): hooks 0 failed, overlay DX11 2560x1440, r2read OK, game tick OK, but "sUserData ... MISSING"
  (inventory not found in that build; crafting depends on it: check "inventory:" lines in the new log).
- In-game (Melty test runs, 23:01 and 23:07): all four checks pass, save slot found (123 containers). Diagnostics
  (scratchpad diag_report.md): MHW's own item text gives the REAL ids: Iron Ore 205, Machalite 206, Dragonite 207,
  Fucium 209, Earth Crystal 210, Bone S/M/+ 236/237/239, Keenbone 240, Hardbone 241, Elder Dragon Bone 242, Elder
  Dragon Blood 263, Eltalite 1030, Solidbone 1051, Spiritvein 1852, Whetstone 114, Capture Net 115 (mhw-db ids were
  NOT game ids; materials.json corrected). Pouch at save slot +0x760, 16 bytes per entry; item bar current-slot field
  still unknown (watch found only small counters at player +0x34/+0x50/+0xD4/+0xB0C/+0xC20). NPC spots recorded but
  unlabelled: zone 301 (5225,1782,-1732) and (4753,1791,-2679); zone 501 (10066,1601,128). Joints: research.cpp's
  one-level scan found only the hunter's own transform; the hand joint needs a motion-based multi-level search.
  Actions seen: set 1 idle 0,2-8,10,32,33,53,...; set 3 (weapon drawn) 0,1,2,4,5,7,18,23,27,34,70-72,75-78,81,85,88,
  94,110; draw sequence 3:5 -> 3:1 -> 3:2 -> 3:0. No item-use, bowgun fire/reload or flinch data yet.
- BUG found and fixed: a stale r2read_next.exe (this morning's old helper, left by install_dev.sh) was swapped in
  over the new r2read.exe at startup, so MESH/SKEL/ANIM replied "bad request": no gun model, no boss models, and
  "could not be decoded" logged every frame. r2client now only swaps in a NEWER helper and drops a stale one;
  install_dev.sh deletes r2read_next.exe after a direct copy; model_get logs a failed decode once.
- Bosses "not loading": no "boss" line in the log at all; key_down needs the game window focused. Added a log line on
  the boss key press ("boss key: view ..., zone ..., rank ...") so the next run shows whether F9 arrived.

## Next: the hunter as the Remnant II player (Jalon, 2026-10-13 late)
Asked: Remnant guns replace the MHW weapon in the hunter's hands with the right animations; Remnant armour on the
hunter; skill/ability animations and effects; the Dragon Heart customised as in Remnant II; Remnant gear in MHW's
item bar beside the whetstone and net; the hunter should feel like the Remnant II player with Remnant gear.
What exists: gun drawn at an estimated hand position with its own fire/reload animations (gunmodel.cpp); relic +
3 fragment slots at the terminal (that is Remnant II's relic customisation: relic choice and three fragments);
first person when aiming; skills with effects but no body animation.
What each needs, from the diagnostics so far:
1. Gun in the real hand: the right-hand joint world matrix. research.cpp's one-level scan found only the hunter root.
   Plan: a motion-based search (a block whose translation moves with the hunter but differs from the root by a
   stable 60-110 cm, and swings when the hunter attacks), 2-3 pointer levels deep from the player object, logged with
   the candidate offsets; then gunmodel attaches to it and hides nothing of MHW (MHW's weapon stays sheathed: the mod
   should keep the hunter in the "no weapon" action set, set 1, while in gun mode, which the action log shows as the
   default stance).
2. Hunter animations: play MHW's own bowgun draw/fire/reload actions through ActionController::DoAction (allowed) once
   their ids are known. The action log only recorded set 1 and set 3 so far; a labelled recording (press the key,
   log "label: ...") of light bowgun draw, fire, reload and item drinking is the missing data.
3. Remnant armour on the hunter: needs the hunter's joint matrices (same as 1) to skin Remnant armour meshes over
   the hunter, and MHW's own armour hidden (a game write Jalon has not approved yet: ask first).
4. Item bar: the current-slot field was not found by the watch; candidates player +0x34/+0x50/+0xD4/+0xB0C/+0xC20.
   A second watch that records which candidate changes exactly when the player scrolls the bar settles it.
- 0.4.1: gun diffuse maps are UE virtual textures (NumMips 0, bIsVirtual 1, 136x136 BC7 tiles in Morton order with a
  4 px border): r2pak.cpp parseVirtual + vtAssemble stitch them; 88 of 92 distinct gun maps load, 4 (Anguish,
  Corrupted Deceit, Corrupted Savior, +1) use a layout still rejected ("unsupported virtual texture") and keep the
  flat colour. weapons.json mesh_tex lost its ".uasset" suffixes (the TEX request adds one); r2client strips a
  suffix defensively. r2read gained DUMP <pkg> (raw package bytes, debugging). Preview screen 8 draws a gun map.
  ui.cpp: Item-Box-style section row (EQUIPMENT / ITEMS / ARCHETYPES / TRAITS / PRISMS) on the Ward 13 screens, ITEMS
  view on the terminal, gun panel moved up and right of MHW's centre prompts, "[P] Remnant gear" prompt in town, the
  mouse wheel swaps long gun / hand gun in gun mode (wheel blocked from the game then). Boss key press now logs.
  Not yet: the hunter holding/reloading the gun with MHW animations and Remnant armour (needs the hand joint and
  action ids, see the plan above); consumables in MHW's item bar (needs the bar's current-slot field).
- Published: GitHub v0.4.1 (1dd8613); Melty remnant-hunt 0.4.0 then 0.4.1 live (2-run tests passed each time).
  Installed in MHW: the dev build c9ce122 = 0.4.1 + research.cpp's motion-based hand-joint search (hunting zones
  and the Training Area, first 10 min per zone, top candidates every 30 s in dev_research.txt, "[hand-like]" tag).
  Jalon's next session: Training Area, draw the weapon, swing it, walk; then send dev_research.txt and
  remnant_hunt.log ("boss key" line after F9).

## 2026-10-14 morning: first driven sessions (Jalon playing, Claude reading the files)
- Confirmed in game: hooks 0/25 failed, overlay DX11 2560x1440, r2read 282 cells, game tick, save slot (123
  containers), gun mesh + reload anim decode, F9 spawns bosses (boss key line, Corrupted Ravager / Nightweaver out),
  the boss bar shows once a boss has aggro (now also by proximity).
- Hand joint: the threaded search found it: chain player+0x90 -> +0x210 or +0x2B0, 0x40-stride world matrices,
  entry 396 (score 503, local offset (-32,105,-5) cm, spread 27) with a fallback chain player+0xA8 -> +0x310.
  gunmodel draws the gun there ([gun] hand_l1/hand_l2/hand_entry/hand_stride, hand_axis, hand_offset_x/y/z in the
  ini for in-game alignment; left/right hand sign not yet confirmed).
- Freeze fixed: the joint search stalled the game thread (reports 1 s apart in 3 min); it runs on its own thread
  with a 5 ms budget now. The item bar watch sampled during the stall is void; it needs a clean session where
  Jalon scrolls the bar (Ctrl + wheel) in the first 150 s after the diagnostic report.
- Fire sound piling up after release: audio cuts the previous voice of a rapidly repeated event and
  audio_stop(snd_fire) runs when the trigger is released.
- OPEN, needs research: Remnant bosses cannot kill the hunter. hurt_hunter uses the game's AddHP; MHW's faint is
  triggered in its own damage handler, not by HP reaching 0. Need the hunter damage function (the one MHW's
  monsters call) to route lethal Remnant damage through it; a devdiag hook/log on hunter hits is the next step.
- OPEN: skill / Dragon Heart body animations need MHW action ids (drink item, gestures). Jalon: drink a potion and
  use a few items in a session so the diagnostic action log records them (dev_diag.txt "action" lines).
- OPEN: in-game texture fate of virtual textures (gun body, bosses) is logged now ("overlay: texture ... -> ok|FAILED");
  read the next session's log.
- Item bar (Jalon: the same bar as the potions): PROXY ITEMS. bar_items.json maps each Remnant bar item to a harmless
  real MHW item kept in the pouch at the Remnant count (Dragon Heart = Antidote 5, skill = Energy Drink 8, long gun =
  Hot Drink 1000, hand gun = Cool Drink 13, weapon mod = Dash Juice 18, carried consumables 1-4 = Nulberry 7 /
  Herbal Medicine 6 / Immunizer 16 / Astera Jerky 17). remnant.cpp bar_tick: a pouch count one below the expected
  count (box + pouch total also down by one, no Remnant screen open) = MHW consumed the proxy -> the Remnant action
  runs and the count is put back (inventory.cpp mhw_pouch_count / mhw_pouch_set, approved inventory writes). The
  HUD draws a legend by the bar ("Antidote = Dragon Heart x3"). The bar's own current-slot field is no longer needed.
- Death: hurt_hunter logs "hunter hit: ... hp A -> B of M"; a lethal hit forces HP to 0 (approved write) and plays
  [bosses] faint_action=S:I through DoAction when set; remnant.cpp regen/refund/heal skip while HP is 0. To find the
  faint action id: faint once normally (any MHW monster) and read the "action S:I" lines around it in dev_diag.txt.
- Jalon (2026-10-14): Remnant items must be NEW MHW items with their own icons, names and animations, not proxies of
  existing ones. The only way MHW can show a new item in its bar is through its own data: itemData.itm (item rows),
  item_eng.gmd (names) and the icon atlas .tex, overridden through the nativePC folder that MHW reads before its
  chunks (the standard MHW data-mod path; no code is patched). The mod would generate those overrides on the player's
  PC from their own install (rule 2 holds: nothing shipped), with rows cloned from a cure-type item so MHW uses its
  drinking animation and no built-in effect. Spike started: src/mhwread (chunk reader: Blowfish + Oodle) to extract
  the three files; then an "nativePC writer" at install/first start. The proxy system stays as the fallback until the
  new items work (bar_items.json decides).
- New items spike (src/mhwread, tools/mhwchunk_notes.md): the chunk format is public (8-byte header, Oodle Kraken
  blocks of 0x40000 XORed with 16 fixed keys picked by WorldChunkTool's keyseq.key, PKG file table at 0x100; later
  chunks override earlier). mhwread LIST/GET extracted itemData.itm (10-byte header, u16 count 2775, 32-byte records
  whose first u16 is the id), item_eng.gmd (515 KB, chunkG8) and the icon atlas ui/common/tex/cmn_item00_ID.tex
  (1024x1024, 192-byte header). Blocker: keyseq.key (1 MB, no licence) cannot ship. PLAN WITHOUT IT: the mod reads the
  loaded rItemData and the item GMD from game memory (read-only, both already located) and writes
  nativePC/common/item/itemData.itm + nativePC/common/text/steam/item_eng.gmd with appended rows (clones of a
  cure-type item: drinking animation, no built-in effect) and names for the Remnant bar items; MHW loads them at the
  next start; bar_items.json takes the generated ids. Icons: the appended rows point at existing icon ids first;
  custom icons need an atlas .tex override (format known: 192-byte header) and a BC7 or uncompressed encoder.
  Needs Jalon's go: it is a new kind of output (files in nativePC, generated on the player's PC from memory).
- Session 09:14 evidence: suffixed texture paths FAILED in game (the strip was missing: restored, ca8a614), the gun
  map loaded (BC7 1024), hitbox 191 bones, hand joint via the fallback chain player+0xA8 -> +0x310, depth candidate
  flaps (matched then "no candidate"), the hunter sat at HP 0 alive (MHW does not faint from HP alone), boss HP
  9000 LR too high for MHW hits (now 2500/6000/11000).
- 10:30 session (ca8a614): all Ravager maps + muzzle flare load (BC7), 30 boss anims decode, hitboxes 191/151/103
  bones, Ravager staggers; hunter HP read right (150 -> 127) then stuck at 0 alive: MHW faints only through its own
  damage path. d890918 (installed, needs a game restart): [bar] proxies=0 by default (stand-ins removed from the
  pouch), [actions] relic/skill/consumable=S:I play MHW actions on use, the faint action is LEARNED when HP reaches 0
  from MHW's own damage (learned_reactions.txt "faint S I"; a barrel bomb in the Training Area does it), the depth
  match keeps its candidate (the boss flicker was the match flapping), bosses.cpp ini path had a literal CR ("\r"),
  so [bosses] keys were never read before.
- Jalon's open asks and what they need: clutch claw on Remnant bosses = only possible if bosses become real MHW
  monsters (nativePC model swap over an MHW monster, MHW AI); Remnant animations on the hunter's body = hunter pose
  write (new write kind) + skeleton retarget; real new items in the bar = nativePC itemData/GMD/atlas overrides
  generated from memory; pause = time-scale write. All four need his decision.
