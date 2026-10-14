#!/usr/bin/env python3
"""Ask the built r2read helper for every asset the sheets (and the code) reference, from the player's own Remnant II
install: texts, icons and art, names, meshes, skeletons, animations and sounds. Prints pass/fail counts per kind and
writes the failures to build/asset_check.txt. Never copies game data.
usage: check_assets.py <r2read.exe> <Remnant2 install dir> [sheets dir]
"""
import collections, json, os, struct, subprocess, sys, time

# Textures named in the code rather than the sheets (bosses.cpp status icons).
CODE_TEXTURES = [
    "Remnant2/Content/UI2/UI_Icons/UI_Icons_Status/T_UI_Icon_Status_" + n
    for n in ("Bleed", "Fire", "Corrosive", "Shock", "Exposed", "Slow", "Omen_Bad")
]


class Helper:
    def __init__(self, exe, game):
        self.p = subprocess.Popen([exe, game], stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def ask(self, *fields):
        self.p.stdin.write(("\t".join(fields) + "\n").encode()); self.p.stdin.flush()
        head = self.p.stdout.readline().decode(errors="replace").strip()
        if not head.startswith("OK") or head.startswith("OK r2read"):
            return head, 0
        n = int(head.split()[-1])
        got = self.p.stdout.read(n)
        self.last = got
        return head, len(got)

    def bones(self, skeleton):
        """Bone names of a skeleton asset (RSKL reply), or None."""
        head, n = self.ask("SKEL", skeleton)
        b = self.last if head.startswith("OK") and n else b""
        if b[:4] != b"RSKL":
            return None
        cnt = struct.unpack_from("<I", b, 4)[0]
        q, out = 8, set()
        for _ in range(cnt):
            ln = struct.unpack_from("<H", b, q)[0]
            out.add(b[q + 2:q + 2 + ln].decode(errors="replace"))
            q += 2 + ln + 44
        return out


def main():
    exe, game = sys.argv[1], sys.argv[2]
    d = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sheets")
    h = Helper(exe, game)
    print(h.ask("HELLO")[0])
    ok, bad = collections.Counter(), collections.Counter()
    fails = []
    seen = set()
    skel_bones = {}
    t0 = time.time()

    def check(kind, where, *req):
        if req in seen:
            return
        seen.add(req)
        head, n = h.ask(*req)
        good = head.startswith("OK") and (n > 0 or req[0] == "NAME")
        if req[0] == "TEX":
            parts = head.split()
            good = good and len(parts) >= 5 and int(parts[2]) > 0 and n == int(parts[-1])
        (ok if good else bad)[kind] += 1
        if not good:
            fails.append(f"{kind} {where} {req[0]} {req[1]} -> {head}")

    sheets = {}
    for name in sorted(os.listdir(d)):
        if name.endswith(".json") and not name.startswith("_"):
            sheets[name] = json.load(open(os.path.join(d, name), encoding="utf-8"))
    for name, sh in sheets.items():
        for r in sh["rows"]:
            where = f"{sh['sheet']}.{r['id']}"
            for col, v in r.items():
                if isinstance(v, dict):
                    kind, spec = next(iter(v.items()))
                    if kind == "r2text":
                        check("text", where + "." + col, "TEXT", spec["asset"], spec["key"])
                    elif kind == "r2tex":
                        check("texture", where + "." + col, "TEX", spec, "256")
                    elif kind == "r2name":
                        check("name", where + "." + col, "NAME", spec["asset"], spec.get("pattern", "^(" + spec.get("name", "") + ")$"))
                elif col == "props" and isinstance(v, list):  # "bone=mesh": a skeletal mesh, held by a bone of the body's skeleton
                    for e in v:
                        if "=" in e:
                            bone, mesh = e.split("=", 1)
                            check("prop-mesh", where, "MESH", mesh)
                            if r["skeleton"] not in skel_bones:
                                skel_bones[r["skeleton"]] = h.bones(r["skeleton"])
                            have = skel_bones[r["skeleton"]]
                            key = ("bone", r["skeleton"], bone)
                            if key not in seen:
                                seen.add(key)
                                if have is not None and bone in have:
                                    ok["prop-bone"] += 1
                                else:
                                    bad["prop-bone"] += 1
                                    fails.append(f"prop-bone {where} {bone} is not a bone of {r['skeleton']}")
                elif col in ("mesh_tex", "textures") and isinstance(v, list):
                    for e in v:
                        if "=" in e:
                            check("gun-texture", where + ".mesh_tex", "TEX", e.split("=", 1)[1].replace(".uasset", ""), "1024")
                elif isinstance(v, str) and "Remnant2/Content" in v:
                    if col == "mesh":
                        check("mesh", where, "MESH", v)
                    elif col == "skeleton":
                        check("skeleton", where, "SKEL", v)
                    elif col.startswith("anim") and v.endswith(".uasset"):
                        check("animation", where + "." + col, "ANIM", v)
                    elif col.startswith("snd") or col == "event":
                        check("sound", where + "." + col, "SND", v, "1")
                    elif col == "mesh_tex":
                        for e in v.split(","):
                            if "=" in e:
                                check("gun-texture", where + ".mesh_tex", "TEX", e.split("=", 1)[1].replace(".uasset", ""), "1024")
    # boss attacks name their animation relative to the boss's anim_dir
    bosses = {b["id"]: b for b in sheets["bosses.json"]["rows"]}
    for r in sheets["boss_attacks.json"]["rows"]:
        b, a = bosses.get(r.get("boss")), r.get("anim")
        if b and a and b.get("anim_dir"):
            path = a if "Remnant2/Content" in a else b["anim_dir"] + a + ("" if a.endswith(".uasset") else ".uasset")
            check("animation", f"boss_attacks.{r['id']}", "ANIM", path)
    for t in CODE_TEXTURES:
        check("texture", "bosses.cpp", "TEX", t, "256")
    h.ask("QUIT")
    os.makedirs("build", exist_ok=True)
    with open(os.path.join("build", "asset_check.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(fails) + ("\n" if fails else ""))
    for k in sorted(set(ok) | set(bad)):
        print(f"{k:10s} ok {ok[k]:5d}  fail {bad[k]:4d}")
    print(f"{sum(ok.values())} assets load, {sum(bad.values())} fail, {time.time() - t0:.0f} s; failures in build/asset_check.txt")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
