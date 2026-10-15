# Hidden "host" World monster: research (MHW PC 15.23.00, build 421810)

Goal: spawn (or borrow) a large World monster, pin its position and facing, hide its model, make it harmless and
lootless, so the hunter's clutch claw, mount and part breaks run on it while the Remnant boss is drawn in its place.

Confidence legend:
- **[V]** verified in this build (mod runtime log or exe).
- **[S]** stated by a maintained source that runs on 421810 (SharpPluginLoader, HunterPie).
- **[O]** from an older build's notes (15.11 / 15.20); needs a runtime check.
- **[G]** educated guess.

## 0. Why no byte patterns were verified in the exe file

`MonsterHunterWorld.exe` on disk is wrapped by SteamStub v3.1 (x64): the `.bind` section holds the stub, and the header
at `EP (0x559F310) - 0xF0` XOR-decodes to the signature `0xC0DEC0DF`. `.text` (raw 0x400, size 0x2E11200) has entropy
7.9998, so it is encrypted. None of the known-good patterns (`add_hp`, SPL `ActionController:DoAction`) appear in the
file. Decrypting the DRM wrapper was refused by the session's permission policy, so I stopped there and did not work
around it.

**So every code pattern has to be checked at runtime**, inside the game, after MHW decrypts itself. That is what
`check_hooks()` and `scan_text()` already do. The diagnostic added to `src/core/devdiag.cpp` (section 5) does this for
the spawn candidates.

## A. Spawn routine

### What the community has

- **No public mod spawns a monster at runtime on PC 15.23 with a published signature.** There is no "Monster Spawner"
  plugin on GitHub or Nexus for Stracker's loader or for SharpPluginLoader (SPL). SPL's `Monster` API has no spawn
  method (`SharpPluginLoader.Core/Entities/Monster.cs`).
- Fexty's research header has absolute addresses and **no patterns**, mostly for 15.11 with some rows re-found for
  15.20. The same file is in SharpPluginLoader (`dependencies/generic/include/utility/game_functions.h`), CatMod
  (`ExtraFiles/game_functions.h`) and mhw-thk-logger.
  - `MH::Debug::SpawnMonsterInit(undefined8 sEnemy, ulonglong emId, ulonglong subId, char flag)`: `0x141A5A3E0` in
    15.20 (older: `0x141A6E3D0`). CatMod 15.20 puts `MH::Debug::SpawnVar` and `MH::sEnemy::Instance` at the **same**
    address, `0x14500AD00`, so arg1 is the **sEnemy singleton**. In 421810 that singleton is at `0x14500CF40`. **[O]**
  - Pre-15.20 only:
    - `MH::Debug::SpawnMonster(undefined8, uint, uint)` at `0x141B31D80`
    - `CallSpawnMonsterInit(undefined8, undefined4)` at `0x141A6DF10`
    - `CallsCreateMonster(void*, __int64, int, QWORD*)` at `0x142125050`
    - `Calls_CallsCreateMonster(char*, __int64*)` at `0x142124E90` **[O]**
  - `SpawnQuestMonster(QuestDataList, 0, 0, 0)` at `0x141B889B0`, described as "spawns monsters from the quest data
    list". `QuestDataList` is at `0x14506F240`. Also `GetMonsterSobjPathExped(sQuest*, char*, int, int, int)` at
    `0x141B689A0`. **[O]**
  - Other monster functions in the same header:
    - flinch/override: `NextAction_(ai=[em+0x12278], [[em+0x12278]+0x4B0])`
    - `Enrage` / `Unenrage([em+0x1BD08])`
    - `SetTargetCoord`, `GetTargetMonster`, `ReadMonsterCol` **[O]**
- Stracker's loader (`Fexty12573/MHW-QuestLoader`, game 421652; files `Plugins/MonsterLoader/dllmain.cpp` and
  `MHWLoader/ghidra_export.h`) hooks two functions:
  - **`MH::EmSetter::CreateMonster(this=cEmSetter*, unkn, ptr, char flag)`** (`0x141A6B4F0` there). It reads the em id
    at `setter+0x168`.
  - `MH::Monster::GenerateFilePaths(this, emId, variant)` (`0x141CCE8F0`).

  This is the path quests and expeditions use: a `cEmSetter`, from the stage's `.sobj` or from the quest, creates the
  monster. **[O]**
- `cEmSetter` layout, from Ando's ClassPropDump for 15.11 (`DaveuRrr/MHW_Research/dti_prop_dump_MHW_15_11_00.h`).
  Size 0x2D0. **[O]**

  | Field | Offset |
  | --- | --- |
  | mFloorNo | +0x10 |
  | mGroupID | +0x14 |
  | mProps | +0x60 |
  | mPosData (PosSetData: mPos +0x10, mProb +0x20) | +0x120 |
  | mEmID (wrapper; the value is at +0x168) | +0x160 |
  | mEmSubID (value at +0x178) | +0x170 |
  | mDesireList | +0x180 |
  | mRepopSetList | +0x230 |
  | mprFSM | +0x268 |
  | mprMotionList | +0x270 |
  | mprThinkTableList | +0x278 |
  | mMulScale | +0x280 |
  | mArrivalRadius | +0x284 |
  | mStartFSM | +0x288 |

  The `.sobj` template (`SOBJ/cEmSetter.bt`) shows the same fields in file order.
- Marcus101RR's Cheat Engine table (Nexus mod 2161) has an "Expedition Editor". It changes which monsters an expedition
  spawns by editing the loaded expedition data. It does not spawn on demand. **[S]**

### SPL patterns that match 15.23

SPL scans these at runtime (`AddressRecords.json`). The mod already resolves `ActionController:DoAction` from the same
list at runtime, so the list is good for 421810. **[S]**

| Function | Pattern | Offset |
| --- | --- | --- |
| `Monster:Ctor` | `4C 89 B3 10 76 00 00` | -60 |
| `Monster:Die` | `48 8B E9 8B 82 80 47 01 00` | -26 |
| `Monster:Dtor` | `48 83 EC 20 48 8B B9 A0 09 00 00` | -20 |
| `Monster:LaunchAction` | `84 C0 74 1E 48 8B 8B E0 89 00 00 48 8D 54 24 38 41 B8 01 00 00` | -24 |
| `Monster:ForceAction` | `48 89 6C 24 30 48 8B CB 48 89 7C 24 38 4C 89 74 24 40 C6 86 A5 00 00 00` | -40 |
| `Monster:Enrage` | `C1 EA 03 F6 C2 01 74 07 32 C0 48 83 C4 28` | -17 |
| `Monster:Unenrage` | `83 E9 01 74 28 83 F9 01 75 37 83 A3 78 01 00 00` | -21 |
| `Monster:Flinch` | `8B A8 40 89 01 00` | -98 |

The list also has the `Quest:*` functions. `Monster::Ctor` on its own does not register a unit. It is an anchor for
finding the creator, by following its callers and their callers.

### How to get the 15.23 spawn function (recommended order)

1. **Identify it at runtime (diagnostic added).**
   - The `host diag: call ...` lines list every function called right after `mov rcx, [rip+sEnemy]`. Each line shows
     the `edx` / `r8d` immediates seen before the call and the function's first 24 bytes.
   - The 15.20 `SpawnMonsterInit(sEnemy, emId, subId, flag)` should be in that list, because its callers load sEnemy
     into rcx.
   - Turn its first bytes into a pattern. Then confirm it in game: call it **once** in the training area with a known
     em id (for example 1 = Rathian, sub 0) and watch sEnemy's list for a new slot.
   - Unknowns:
     - It may also need a quest or stage context. The "Debug" name suggests a dev spawner that could be stubbed out in
       retail.
     - Where it places the monster, probably at the setter or default position. Then write +0x160 / +0xA50.
2. **Data path (no code calls; nativePC writes are approved).**
   - Option 1: add a `cEmSetter` to the stage's set-object file (`.sobj` under `stage/st4xx/.../set/`).
   - Option 2: ship a custom quest (QuestLoader-style `.mib`) for the training area or an arena map.
   - Either one sets em id, sub id, spawn position and `mMulScale`; a quest also sets the HP, attack and size tables.
     The game then spawns the monster natively when the zone loads.
   - Lowest risk, but fixed per zone and needs a reload.
3. **Borrow (the current fallback).** Use a large monster already in the zone (expeditions): pin it, hide it, keep it
   alive. This is not possible in the training area, which has no large monster.

## B. Monster fields

uEnemy = uEm###. It inherits uCharacterModel -> uMhModel -> uModel -> uBaseModel -> uCoord -> cUnit.

| Field | Offset | Source / confidence |
| --- | --- | --- |
| mPos (x, y, z, cm) | +0x160 | DTI dump 15.11, SPL `Model.Position`, HunterPie **[S]** |
| mQuat (x, y, z, w) | +0x170 | DTI, SPL `Model.Rotation`; turf.cpp already writes it **[V]** |
| mScale (vec3) | +0x180..+0x18B | DTI `mScale`, SPL `Model.Size` / `Resize()` **[S]** |
| Crown size | +0x184 / +0x7730 | HunterPie: `sizeMultiplier` = +0x184 (scale.y), `sizeModifier` = +0x7730; crown = their ratio **[S]** |
| Collision position | +0xA50 | SPL `Teleport()` writes both Position **and** CollisionPosition **[S]** |
| mTransparency (f32) | +0x314 | DTI uBaseModel **[O]**; the game may set it every frame |
| cUnit "frozen" | +0x14 bit 0 (0 = frozen) | SPL `Model.Frozen`: "freezes the model and pauses all processing" **[S]** |
| cUnit Draw / Move / Fix | pseudo-props (Draw getter at `0x142245480` in 15.11) | DTI **[O]**; bit position unknown, see the diag's `unit +0x08` words |
| PartsDisp[] | pseudo-prop array | DTI uBaseModel: one display bool per mesh part **[O]** |
| em id / variant | +0x12280 / +0x12288 | SPL `Monster.Type` / `Variant` **[S]** |
| AI data | [+0x12278] | SPL `AiData`, hooks.json **[S]** |
| Difficulty index | [AI+0x8AC] | SPL `DifficultyIndex` (row of the HP / attack rate table) **[S]** |
| Target | [[AI+0xAA8]+0x5D0] | SPL `SetTarget()` **[S]** |
| Forced action id | +0x18938, then call `ForceAction([AI+0x4B0], AI)` | SPL `ForceAction()` **[S]** |
| ActionController | +0x61C8; {set, id} at +0xAC / +0xB0 | SPL, mod **[V]** |
| Animation speed | +0x1D8A8 (the game resets it periodically) | SPL `Speed` **[S]** |
| Despawn timer (s) | +0x1C3D4 | SPL `DespawnTime` **[S]**; probably the carcass timer |
| Enrage | struct at +0x1BE30 (Duration +0x24, MaxDuration +0x28); the Enrage call takes +0x1BD08 | HunterPie / SPL **[S]** |
| Stamina | +0x1C0F0 (cur, max) | HunterPie **[S]** |
| Ailments | +0x1BC40 list; +0x1C458 tenderize | HunterPie **[S]** |
| HP | [+0x7670]+0x60 max, +0x64 cur | hooks.json **[V]** |
| Dead | [AI]+0x14780 bit 2 | hooks.json **[V]** |

No direct "attack multiplier" field is documented. Attack comes from the quest / difficulty table row
(`DifficultyIndex`) and from the shell and attack params. No index change brings it to zero.

### No damage (best to worst)

1. **Keep the host out of combat.** Set its target to null every frame (SPL `SetTarget`: `[[AI+0xAA8]+0x5D0] = 0`), so
   the AI never picks an attack. Cancel any attack action with the existing DoAction (turf.cpp), back to an idle or
   walk action from the monster's own action list. **[S]+[G]**
2. **Freeze it** (+0x14 bit 0 = 0). No AI, no attacks, no animation. Collision boxes probably stay at the last pose, so
   a latch may still connect. But claw, mount and flinch reactions are AI actions and would not play. Test only.
   **[G]**
3. **Set speed +0x1D8A8 = 0 every frame.** This freezes animation, so attack hitboxes never activate, but the game
   resets the value. **[S]**
4. Refund damage to the hunter with `add_hp` (already used), as a safety net only.

### No loot

- A monster becomes carvable once its death action puts it in the carcass state (dead bit 2).
- So never let the host die: every frame, write its HP back to max or `add_hp` the damage back. Part HP still drops,
  so breaks still happen.
- When the Remnant boss dies, remove the host instead of killing it. There is no documented, safe despawn call. The
  safe fallback is to teleport it far below the map (+0x160 and +0xA50), freeze it and stop pinning it.
- **Note:** part breaks and claw / flinch "shiny" drops are also rewards.
  - Shiny drops come from flinch and knockdown events. The SPL `Monster:Flinch` hook could see them, but suppressing
    them would need a hook, which is a code patch and not allowed.
  - Break rewards are given at quest end, so with no quest (training area, expedition) only shiny drops are a concern.
    **[G]**

## C. Clutch claw, mount, part breaks

### Latching

- The claw is a hunter shell that hits the monster's **hurt collision**: the `.col` capsules bound to its joints,
  loaded by `ReadMonsterCol` **[O]** (SPL `Entity.CollisionComponent` at +0x6A8). The hunter then attaches to the
  joint it hit.
- The joints and capsules follow the model scale; that is how crown sizes change hitboxes. A host scaled to 0.01
  would leave hurtboxes a few cm across at a single point. Claw, mount and breaks would become nearly impossible, and
  the hunter would be pinned to that point. **Hiding by scale defeats the purpose.** **[G], high confidence**

### Hiding without scale

Keep mScale near 1 (or at the size that matches the Remnant boss) and hide the model by drawing instead:

1. Set `mTransparency` +0x314 to 0. Check with the diag whether the game overwrites it.
2. Use the cUnit Draw bit. First find which bit of the +0x08..+0x17 words it is: toggle bits on a test monster and see
   whether the model disappears while its HP bar stays.
3. Set all of PartsDisp[] to false. This needs the DTI pseudo-prop setter, so it is a function call.
4. Last resort: sink the host 1-2 m into the ground. This is bad: the claw would mostly hit the floor and the hunter
   would latch below ground.

### Mounting

- Mounting is driven by mount buildup from aerial and claw hits, and by the monster's own mount actions (AI).
- The host's AI must keep running (not frozen), and the mod must not cancel mount or claw reaction actions. List the
  reaction ids and let them through. QuestLoader's ClutchRework reads them from the monster's reaction table:
  - type 172 = claw extend
  - types 164 / 165 = turn by claw

  **[O]**

### Part breaks

HunterPie reads the part table from `[+0x1D058]`. **[S]**

- Normal parts: start at `+0x40`, stride 0x1F8.
- Severable parts: start at `+0x1FC8`, stride 0x78.
- Part struct:

  | Field | Offset |
  | --- | --- |
  | MaxHealth | +0x0C |
  | Health | +0x10 |
  | Counter (break count) | +0x18 |
  | ExtraMaxHealth | +0x1C |
  | ExtraHealth | +0x20 |

Breaks happen through normal damage, so they work as long as hits land on full-size hurtboxes.

## D. Pinning

- Every frame, after the game's update, write +0x160 (and +0xA50, as SPL `Teleport` does) and +0x170.
- The quaternion write is already in use (turf.cpp).
- The game moves the monster with root motion each frame. Pin late in the frame (the mod's game tick) and expect one
  frame of jitter.

## 5. Diagnostic added (src/core/devdiag.cpp, read-only)

`host_diag_tick()` runs from `devconsole_tick()` every 5 s while in game, when the `dev_diag` file exists. It logs:

- `host diag: [slot] <em> <DTI class> path <em\emNNN\..> id <u32> var <u32> pos ... quat ...`
- `host diag: [slot] +0x180..0x1A0 <8 floats> sizeMod(+0x7730) transp(+0x314)`
- `host diag: [slot] unit +0x08 <4 words> action set:id hp cur/max deadflags diffIdx despawn speed enrage parts`
- `host diag: sEnemy <ptr> (<DTI>), N large monsters`
- Once per session: `host diag: call <fn> (rva) xN first site ... edx=.. r8d=.. : <24 bytes>`, one line for every call
  after `mov rcx,[sEnemy]`. These are the spawn candidates. Building the list takes one pass over .text, roughly a
  50-100 ms hitch, and runs only with the diagnostic on.

Two bugs found in passing in existing code (not changed):

- `devdiag.cpp` `menu_watch_tick` builds `"%s\dev_diag"`. `\d` is not a valid escape, so the path loses its backslash.
- `menu_flag_read` builds `"%s\remnant_hunt.ini"`. `\r` is a carriage return.

As a result, the menu-watch flag file and the `[menu] flag=` ini key are never read.

## Sources

- SharpPluginLoader: https://github.com/Fexty12573/SharpPluginLoader
  - `Assets/Common/AddressRecords.json`
  - `SharpPluginLoader.Core/Entities/Monster.cs`, `Models/Model.cs`, `Entities/Entity.cs`
  - `dependencies/generic/include/utility/game_functions.h`
- CatMod (15.20 rows): https://github.com/Fexty12573/CatMod, `ExtraFiles/game_functions.h`
- Stracker's loader / QuestLoader: https://github.com/Fexty12573/MHW-QuestLoader
  - `Plugins/MonsterLoader`, `Plugins/ClutchRework`, `MHWLoader/ghidra_export.h`
- DTI dump and SOBJ templates: https://github.com/DaveuRrr/MHW_Research
  - `dti_prop_dump_MHW_15_11_00.h`, `SOBJ/cEmSetter.bt`
- HunterPie: https://github.com/HunterPie/HunterPie, under `HunterPie.Integrations/Datasources/MonsterHunterWorld/`
  - `Entity/Enemy/MHWMonster.cs`
  - `Definitions/MHWMonsterPartStructure.cs`, `Definitions/MHWMonsterStatusStructure.cs`
- Marcus101RR table: https://www.nexusmods.com/monsterhunterworld/mods/2161
- Exe: this session's scan. SteamStub v3.1 header at EP 0x559F310 - 0xF0 (signature 0xC0DEC0DF); .text encrypted.
