#!/usr/bin/env bash
# Native Windows build from Git Bash: llvm-mingw (Windows release) + GNU make, no Wine needed.
#   tools/build_native.sh            -> make -j8
#   tools/build_native.sh test       -> the three logic tests, run natively
#   tools/build_native.sh preview    -> build/preview_ui.exe, renders screens 0-9 to build/preview/*.png
#   tools/build_native.sh <make args>
set -eo pipefail
cd "$(dirname "$0")/.."
TCB="${TC:-/c/Users/jalon/tc/llvm-mingw-20261006-ucrt-x86_64/bin}"
MAKE_DIR=$(dirname "$(ls /c/Users/jalon/AppData/Local/Microsoft/WinGet/Packages/ezwinports.make_*/bin/make.exe 2>/dev/null | head -1)")
mkdir -p build/shim
[ -x build/shim/python3.exe ] || cp "$(which python)" build/shim/python3.exe   # the Makefile calls python3
export PATH="$PWD/build/shim:$MAKE_DIR:/usr/bin:$PATH"
CXX="$TCB/x86_64-w64-mingw32-clang++"
case "${1:-all}" in
  test)
    mkdir -p build
    F="-O1 -std=c++17 -Wall -Wno-unused-function -Wno-missing-braces -Isrc/core"
    $CXX $F tests/test_turf_stamina.cpp src/core/turf.cpp src/core/stamina.cpp src/core/effects.cpp src/core/mem.cpp -static -o build/test_logic.exe
    $CXX $F tests/test_progress.cpp src/core/progress.cpp src/core/effects.cpp src/core/mem.cpp -static -o build/test_progress.exe
    $CXX -std=c++17 -Isrc/core tests/test_effects.cpp src/core/effects.cpp -static -o build/test_effects.exe
    $CXX $F tests/test_craft.cpp src/core/inventory.cpp src/core/mem.cpp -static -o build/test_craft.exe
    $CXX $F tests/test_hunter.cpp src/core/hunter_rig.cpp -static -o build/test_hunter.exe
    $CXX $F -DRH_TERRAIN_ONLY tests/test_terrain.cpp src/core/bosses.cpp -static -o build/test_terrain.exe
    ./build/test_logic.exe | tail -1 && ./build/test_progress.exe | tail -1 && ./build/test_effects.exe | tail -1 && ./build/test_craft.exe | tail -2
    ./build/test_hunter.exe | tail -6
    ./build/test_terrain.exe | tail -1
    # nativePC writers against the extracted originals (tools/mhwchunk_notes.md); skipped where build/mhw/ is missing.
    if [ -f build/mhw/itemData.itm ] && [ -f build/mhw/item_eng.gmd ]; then
      $CXX $F tests/test_nativepc.cpp src/core/nativepc.cpp src/core/mem.cpp -static -o build/test_nativepc.exe
      ./build/test_nativepc.exe build/mhw | tail -3
    else
      echo "nativepc: skipped (build/mhw/ not extracted)"
    fi
    # The icon pipeline (real r2read + Direct3D 11) needs Remnant II installed; skipped where it is not.
    R2="${R2:-/c/Program Files (x86)/Steam/steamapps/common/Remnant2}"
    if [ -d "$R2" ] && [ -x build/win/r2read.exe ]; then
      $CXX $F tests/test_textures.cpp src/core/r2client.cpp src/core/texture.cpp src/core/mem.cpp -static -ld3d11 -o build/test_textures.exe
      ./build/test_textures.exe "$(cygpath -w "$PWD/build/win")" | tail -22
    else
      echo "textures: skipped (Remnant II not found at $R2)"
    fi ;;
  preview)
    make preview-ui TC="$TCB" 2>&1 | grep -E "error|Error" || true
    [ -x build/preview_ui.exe ] || { echo "preview build failed"; exit 1; }
    mkdir -p build/preview
    R2W="$(cygpath -w "${R2:-/c/Program Files (x86)/Steam/steamapps/common/Remnant2}")"
    for i in 0 1 2 3 4 5 6 7 8 9; do ./build/preview_ui.exe $i assets build/preview/screen_$i.bmp "$(cygpath -w "$PWD/build/win")" "$R2W" >/dev/null; done
    python tools/bmp2png.py build/preview/screen_*.bmp && ls build/preview/*.png ;;
  *) make -j8 TC="$TCB" "$@" ;;
esac
