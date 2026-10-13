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

## Build state (2026-10-13 ~00:40 CT)
- Built: build/win/{dinput8.dll, remnant_hunt.dll, r2read.exe}; dev-installed into the MHW folder
  (dinput8.dll + remnant_hunt/). `make`, `make test R2=<Remnant2 dir>`, `make package VERSION=x`.
- r2read (Windows SSE2 build of ooz + our IoStore reader) cross-checked on Linux against the Python tools:
  82/82 sheet cells match (texts, names, BC7/BC1 icons).
- Overlay: DX11 direct, DX12 via D3D11On12 (queue found by hooking ExecuteCommandLists in d3d12.dll).
- Input: DirectInput, raw input, GetAsyncKeyState, SetCursorPos/ClipCursor filtered in Windows DLLs only.
  The log reports which input paths MHW actually uses ~20 s after the first frame.
- Game tick: sMhCamera object pointed at a copied vtable (slot 6 -> mod tick). No MHW code patched.
- Keys moved off MHW defaults: B gun mode, R reload, X swap (gun mode), V mod, G skill, T relic, P terminal.

## Not yet verified in game (needs the PC usable)
- Every hooks.* row and systems.* row (preflight lists them as UNVERIFIED until the log shows them).
- Aim zoom (FOV write before/after camera move), solo detection (party offset meaning), weakspot band, damage scale.
- Computer use: screenshots came back fully masked and steam://rungameid/582010 failed to open (PC asleep/locked?).

## Waiting on Jalon
- License + remix choice (MIT recommended). r2read stays GPL-3.0 because ooz is.
- Permission to use the Melty token (session safety check blocked sending it).

## Next steps
1. Launch MHW (Steam), read remnant_hunt/remnant_hunt.log, fix hook failures, go to Training Area (zone 504),
   press B, shoot the dummy, check damage numbers / HP log; screenshot the intro + HUD.
2. Melty: game_info both games, search_mashups, inspect_package, validate_recipe (dinput8.dll -> {game},
   remnant_hunt/ -> {game}/remnant_hunt, env REMNANTHUNT_R2={game:<remnant slug>}), one_click_check, create_mod,
   upload, test_release, screenshot, publish on approval.
