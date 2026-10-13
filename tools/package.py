#!/usr/bin/env python3
"""Build the release zip Melty installs into the Monster Hunter: World folder.

Layout (paths inside the zip are relative to the game folder):
  dinput8.dll                      loader (forwards DirectInput to Windows' own dinput8.dll)
  remnant_hunt/remnant_hunt.dll    the mod
  remnant_hunt/r2read.exe          GPL-3.0 helper that reads Remnant II from the player's own install
  remnant_hunt/fonts/*.ttf         Montserrat and EB Garamond (SIL OFL 1.1)
  remnant_hunt/licenses/*          licenses and credits
  remnant_hunt/README.txt
No game files from either game are included: the zip is checked for that before it is written.
usage: package.py <version>
"""
import hashlib, os, sys, zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
B = os.path.join(ROOT, "build", "win")

BANNED_EXT = {".uasset", ".ubulk", ".uexp", ".utoc", ".ucas", ".pak", ".locres", ".wem", ".bnk", ".arc", ".tex",
              ".mod3", ".dds"}


def files():
    yield os.path.join(B, "dinput8.dll"), "dinput8.dll"
    yield os.path.join(B, "remnant_hunt.dll"), "remnant_hunt/remnant_hunt.dll"
    yield os.path.join(B, "r2read.exe"), "remnant_hunt/r2read.exe"
    for f in sorted(os.listdir(os.path.join(ROOT, "assets", "fonts"))):
        if f.endswith(".ttf"):
            yield os.path.join(ROOT, "assets", "fonts", f), "remnant_hunt/fonts/" + f
    lic = [("LICENSE", "LICENSE-GPL-3.0.txt"), ("NOTICE.md", "NOTICE.txt"),
           ("third_party/ooz/UPSTREAM.txt", "ooz-source.txt"),
           ("third_party/minhook/LICENSE.txt", "MinHook-BSD-2-Clause.txt"),
           ("third_party/imgui/LICENSE.txt", "DearImGui-MIT.txt"),
           ("assets/fonts/OFL-Montserrat.txt", "Montserrat-OFL.txt"),
           ("assets/fonts/OFL-EBGaramond.txt", "EBGaramond-OFL.txt")]
    for src, name in lic:
        p = os.path.join(ROOT, src)
        if os.path.exists(p):
            yield p, "remnant_hunt/licenses/" + name
    yield os.path.join(ROOT, "README.md"), "remnant_hunt/README.txt"


def main():
    version = sys.argv[1]
    out = os.path.join(ROOT, "out")
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, f"remnant_hunt-{version}.zip")
    entries = list(files())
    for src, arc in entries:
        if os.path.splitext(arc)[1].lower() in BANNED_EXT:
            sys.exit(f"refusing to package game data: {arc}")
        if not os.path.exists(src):
            sys.exit(f"missing {src} (run make first)")
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for src, arc in entries:
            zi = zipfile.ZipInfo(arc, date_time=(2026, 10, 13, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            z.writestr(zi, open(src, "rb").read())
    data = open(path, "rb").read()
    print(path, len(data), "bytes", "sha256", hashlib.sha256(data).hexdigest())
    for _, arc in entries:
        print("  ", arc)


if __name__ == "__main__":
    main()
