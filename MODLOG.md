# MODLOG: Remnant II x Monster Hunter: World mashup

## Intake (2026-10-12)
- Host (primary): Monster Hunter: World, Steam, D:\SteamLibrary\steamapps\common\Monster Hunter World
  - MonsterHunterWorld.exe PE timestamp 2024-08-30 = Ver. 15.23.00 (build 421810), the final PC patch. No ASLR.
  - Fresh install: no nativePC, no loader. Sections .text/.rdata/.data/... + .bind (Steam stub). No packer sections.
- Companion: Remnant II, Steam, C:\Program Files (x86)\Steam\steamapps\common\Remnant2
  - UE 5.2-era IoStore: pakchunk0/1-Windows.utoc/.ucas, TOC v5, flags Compressed|Signed|Indexed (NOT encrypted),
    compression Oodle (Kraken family). 747,366 paths.
  - The legacy .pak files have an AES-encrypted index. We never decrypt: they are skipped. Localization (.locres)
    and font faces live there, so text comes from FText source strings inside the unencrypted zen packages.
- Design (from Jalon): mix of intro + Astera hub; Remnant gun feel + archetypes + relic, while MHW weapons/armor/items
  still work; MHW UI stays, Remnant-styled UI for Remnant tasks; solo first, co-op planned.

## Route
- MHW: native DLL loaded by our own dinput8.dll proxy (Melty installs no loader for MHW). No inline patches of
  MHW's code: MHW has code CRC checks and we do not touch or disable them. We read memory, call game functions,
  hook DXGI Present (dxgi.dll, not MHW code) and swap an object's vptr to a copied vtable for a game-thread tick.
- Remnant II: content read in place at runtime by r2read (GPL-3.0 helper, uses ooz's open-source Oodle decoder):
  FText (key + English source) from zen packages, Texture2D BC7 mips via the UE 5.2 BulkDataMap. Nothing copied.
- Reference: justbustin/minecraft-crossover-bridge (MIT) MHW 15.23 addresses; HunterPie (Apache-2.0) 421810 map.

## Verified facts
- Zen package: FText = [flags][history 0][namespace][32-hex key][source]. Name map hash algo id 0xC1640000.
- UE 5.2 Texture2D cooked: SizeX, SizeY, PackedData, PixelFormat FString, FirstMip, NumMips, per mip:
  BulkDataMap index (i32) [+ inline payload] + SizeX/SizeY/SizeZ. BulkDataMap (u64 size, 32-byte entries) follows the
  name map. Checked visually on two icons (AR-47 icon from .ubulk, Medic emblem inline).
- Archetype emblems: Hunter = T_UI_Icon_Archetype_Sharpshooter, Challenger = ..._Defender (checked visually).
- Brabus's shop stock and lines are in Quest_Ward13/EventTree_Brabus_PlayerBase; McCabe's in EventTree_AvaMcCabe_PlayerBase.

## Gotchas
1. device_commit_files can deliver a stale copy when the same staged path is re-committed: stage under a new name.
2. ooz-x needs -mavx2 -DOODLE_ENABLE_AVX2 and lzna.cpp for LZNA_InitLookup.

## Build state (2026-10-13 ~11:40 CT, v0.2 in progress)
- In game (log of 10:34): 25/25 hooks pass, DX11 overlay at 2560x1440, Remnant II text resolves, game tick runs,
  zones 0 -> 301 -> 501 -> 504 tracked, intro and archetype choice worked (hunter).
- Jalon's v0.2 asks: Remnant gear in MHW's item bar; Remnant animations/sounds/textures on the hunter; craft Remnant
  items, classes and weapons from NPCs in the hub, with MHW materials scaled to MHW; menus offering all Remnant
  classes, weapons and items. Agreed design changes: MHW's own NPCs (Smithy, Elder Melder, Provisions Manager) host
  the Remnant menus; animations come from MHW's own moves that fit each item (bowgun moves for guns, drinking for
  relics and consumables, hunter actions for skills).
- Sheets v0.2 (tools/author_arsenal.py from the install catalog + Remnant 2 Toolkit numbers, GPL-3.0): 14 archetypes,
  42 skills, 65 perks, 13 traits, 112 weapons, 36 mods, 66 mutators, 225 rings, 103 amulets, 27 relics,
  44 fragments, 53 consumables, 120 armor, 15 material classes, 6 NPC menus, 10 upgrade levels.
  2481 Remnant II cells verified against the install. 841 effect strings parse (tests/test_effects.cpp).
  Effect fidelity: 397 exact, 281 approx, 228 fallback (by the item's tags). Hand fixes: sheets/_overrides.json.
- Code v0.2: effects.cpp (effect language, totals, triggers), catalog.h, inventory.cpp (zenny, item box containers,
  MHW item names), remnant.cpp (loadout -> effects, crafting requests, skills x2, any relic, mods, actives),
  ui.cpp (NPC menus with crafting, loadout terminal, archetypes, HUD), r2client lazy text.
- Diagnostic v2 (read-only, dev_diag file): singletons by class name, save data checked by name/zenny, item names
  (dev_items.txt), item containers, NPC spots (stand still 4 s), action/animation log, item bar watch.

## Waiting on the diagnostic run (Jalon)
- NPC spots -> sheets/menus.json x/y/z (0 = unknown; the terminal offers the three vendors meanwhile).
- MHW item ids for the material classes -> sheets/materials.json mhw_item_id; item_name_stride in tuning.
- Item box layout check (containers with the pouch's vtable); zenny at +0x94.
- Item bar current-item field (watch) -> bar_items; action/animation ids for item animations.

## Next steps
1. Read dev_diag.txt + dev_items.txt; fill menus, materials, bar_items; verify containers; rebuild.
2. Item bar: Remnant gear in pouch slots (names via GMD, icons over the slot, use -> Remnant action).
3. Animations: ActionController::DoAction / Entity::DoAnimation (SPL patterns) with ids from the action log.
4. Sounds: Wwise media from the install (event -> media ids), decoded and played through our own audio.
5. Package v0.2, Melty (blocked by network policy: see MELTY_HANDOFF.md).

## Current hand and boss investigation (2026-10-14)
- Creator correction: player animations do run. The current failure is integration: extra-looking arms/hands, and the Remnant gun is not aligned with the MHW hunter's actual hands.
- Live log identifies the mapped right-hand bone at joint entry 271, while the configured/validated gun candidate is entry 396 and is explicitly not a matched hand. `gunmodel.cpp` consumed the latter snapshot before `hunter_tick` wrote the Remnant pose.
- Local source now exposes the matched right-hand matrix, reads it after the hunter pose update, and evaluates the gun after that update. The `systems.json` acceptance checks now require the real gun grip and visible native-hand pose. These changes are unbuilt and untested in game.
- The Remnant player body mesh is not drawn by this path; equipped armor overlays and the separate weapon mesh are. The live mod save reports no Remnant armor equipped, so the exact source of the extra-looking limbs still needs an in-game isolation check; do not claim this solved it.
- A live screenshot showed the Ravager absent while its boss bar and close-distance marker remained. This is one frame of evidence; capture a short sequence after a verified build before calling its flicker fixed.
- `python tools/preflight.py --strict` still fails on 15 unverified gameplay systems (solo gate, gun mode, fire, reload, swap, mod charge, skill, damage watch, relic, refill, scrap, vendor, save, intro, Melty test). No build, release upload, Melty test, or publish was done. Keep the running MHW process open.
- Next: isolate the duplicated limb source and verify the mapped-hand attachment with the sheet criteria; clear the required in-game evidence rows, then rerun strict preflight before any build. Keep SSJ2 credited and the draft private.

## Boss asset resolution (2026-10-14)
- The renderer was requesting boss diffuse textures at a 1024-pixel maximum. A direct read from the installed Remnant II content confirmed four Corrupted Ravager BC7 textures have 4096x4096 source mips; the prior live log only showed 1024x1024 uploads.
- Boss and held-prop draw items now request up to 4096 pixels; other Remnant models keep the 1024 default. The texture cache now releases entries unused for 90 seconds, sweeping every 15 seconds, to bound retained GPU resources when changing bosses.
- Updated the boss asset system sheet to make its new high-mip/no-failure live-log test explicit and cleared its prior verification record. Standard sheet preflight and a build remain unrun after these edits; the strict preflight still has its existing gameplay evidence blockers, plus boss assets now correctly require verification.
- No DLL was built or installed and the running MHW session was left untouched. Check a live boss render logs 4096x4096 uploads and no failed mesh/material texture loads before claiming success.

