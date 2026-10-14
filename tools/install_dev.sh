#!/usr/bin/env bash
# Dev install into Jalon's MHW folder (the same layout the Melty release installs). Removes nothing of the game's.
# Works while the game runs: the mod loads copies of its DLL and helper, and a helper that is in use is left as
# r2read_next.exe, which the next game session swaps in.
set -e
G="${1:-$HOME/mnt/Monster Hunter World}"
M="$G/remnant_hunt"
mkdir -p "$M/fonts" "$M/licenses"
cmp -s build/win/dinput8.dll "$G/dinput8.dll" || cp build/win/dinput8.dll "$G/dinput8.dll" || echo "dinput8.dll in use: close the game and install again"
cp build/win/remnant_hunt.dll "$M/"
if ! cmp -s build/win/r2read.exe "$M/r2read.exe"; then
  { cp build/win/r2read.exe "$M/r2read.exe" 2>/dev/null && rm -f "$M/r2read_next.exe"; } || { cp build/win/r2read.exe "$M/r2read_next.exe"; echo "r2read.exe in use: left as r2read_next.exe"; }
fi
cp assets/fonts/*.ttf assets/fonts/OFL-*.txt "$M/fonts/"
cp LICENSE* NOTICE* "$M/licenses/" 2>/dev/null || true
cp third_party/ooz/COPYING "$M/licenses/r2read-GPL-3.0.txt"
cp third_party/ww2ogg/COPYING "$M/licenses/ww2ogg-BSD-3-Clause.txt"
cp third_party/acl/LICENSE "$M/licenses/acl-MIT.txt"
cp third_party/rtm/LICENSE "$M/licenses/rtm-MIT.txt"
[ -e "$M/dev_reload" ] && mv -f "$M/dev_reload" "$M/dev_reload.off" || true  # no hot reload by default
# Melty's test runs install the release and remove it afterwards, taking dinput8.dll with them: verify both halves.
cmp -s build/win/dinput8.dll "$G/dinput8.dll" && cmp -s build/win/remnant_hunt.dll "$M/remnant_hunt.dll"   && echo "installed into $G (loader + core verified)" || { echo "INSTALL INCOMPLETE: check $G/dinput8.dll and $M/remnant_hunt.dll"; exit 1; }
