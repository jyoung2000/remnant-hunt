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
