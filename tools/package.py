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
import hashlib, os, subprocess, sys, zipfile

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
           ("third_party/ww2ogg/COPYING", "ww2ogg-BSD-3-Clause.txt"),
           ("third_party/imgui/LICENSE.txt", "DearImGui-MIT.txt"),
           ("third_party/acl/LICENSE", "acl-MIT.txt"),
           ("third_party/rtm/LICENSE", "rtm-MIT.txt"),
           ("assets/fonts/OFL-Montserrat.txt", "Montserrat-OFL.txt"),
           ("assets/fonts/OFL-EBGaramond.txt", "EBGaramond-OFL.txt")]
    for src, name in lic:
        p = os.path.join(ROOT, src)
        if os.path.exists(p):
            yield p, "remnant_hunt/licenses/" + name
    yield os.path.join(ROOT, "README.md"), "remnant_hunt/README.txt"
    # GPL-3.0: the complete working-tree source of this release travels with it.
    src = os.path.join(ROOT, "out", "source.zip")
    source_archive(src)
    yield src, "remnant_hunt/source/remnant-hunt-source.zip"


def source_archive(path):
    listed = subprocess.run(
        ["git", "-C", ROOT, "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        check=True, capture_output=True,
    ).stdout.decode("utf-8", "replace").split("\0")
    excluded_dirs = {".git", ".token-optimizer", "attachments", "build", "out", "shots"}
    excluded_names = {".env", ".env.local", ".env.production", "credentials.json", "secrets.json"}
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for rel in sorted(set(listed)):
            if not rel:
                continue
            rel = rel.replace("\\", "/")
            parts = rel.split("/")
            name = parts[-1].lower()
            if excluded_dirs.intersection(parts) or name in excluded_names:
                continue
            if os.path.splitext(name)[1] in {".pem", ".p12", ".pfx", ".key"}:
                continue
            src = os.path.join(ROOT, *parts)
            if not os.path.isfile(src) or os.path.islink(src):
                continue
            if os.path.splitext(name)[1].lower() in BANNED_EXT:
                continue
            zi = zipfile.ZipInfo(rel, date_time=(2026, 10, 13, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            z.writestr(zi, open(src, "rb").read())


def main():
    version = sys.argv[1]
    out = os.path.join(ROOT, "out")
    os.makedirs(out, exist_ok=True)
    path = os.path.join(out, f"remnant_hunt-{version}.zip")
    entries = list(files())
    for src, arc in entries:
        if os.path.splitext(arc)[1].lower() in BANNED_EXT:
            sys.exit(f"refusing to package game data: {arc}")
        if arc.endswith(".zip"):
            with zipfile.ZipFile(src) as inner:
                bad = [n for n in inner.namelist() if os.path.splitext(n)[1].lower() in BANNED_EXT]
            if bad:
                sys.exit(f"refusing to package game data inside {arc}: {bad[:5]}")
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
