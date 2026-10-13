#!/usr/bin/env bash
# Dev install into Jalon's MHW folder (the same layout the Melty release installs). Removes nothing of the game's.
set -e
G="${1:-$HOME/mnt/Monster Hunter World}"
M="$G/remnant_hunt"
mkdir -p "$M/fonts" "$M/licenses"
cp build/win/dinput8.dll "$G/dinput8.dll"
cp build/win/remnant_hunt.dll build/win/r2read.exe "$M/"
cp assets/fonts/*.ttf assets/fonts/OFL-*.txt "$M/fonts/"
cp LICENSE* NOTICE* "$M/licenses/" 2>/dev/null || true
cp third_party/ooz/COPYING "$M/licenses/r2read-GPL-3.0.txt"
[ -e "$M/dev_reload" ] && mv -f "$M/dev_reload" "$M/dev_reload.off" || true  # no hot reload by default
echo "installed into $G"
ls -la "$G/dinput8.dll" "$M"
