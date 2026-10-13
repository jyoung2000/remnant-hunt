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
