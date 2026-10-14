#!/usr/bin/env python3
"""Dev tool: author the `props` column of sheets/bosses.json (held weapons and other props) from the player's own
Remnant II install. For each boss it lists the install under the boss's Characters/<Name>/ folders for meshes other than
the body (names with Weapon, Sword, Blade, Staff, Spear, Axe, Hammer, Shield, Scythe, Bow, Lance, Mace, Prop, Pickaxe),
keeps those r2read can decode (PhysicsAsset, VFX, shard and chain-segment pieces are skipped), and picks the bone of the
body skeleton each is held by ("weapon" / "prop" sockets first, then right-hand bones; shields the left hand).
Entries are "bone=meshpath"; bosses without a prop get ["none"]. Ambiguous choices are logged to stdout.
usage: author_boss_props.py <Remnant II dir> <r2read.exe> [--write]
"""
import json, os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import r2pak  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, ".."))
R2DIR, EXE = sys.argv[1], os.path.abspath(sys.argv[2])
WRITE = "--write" in sys.argv
KW = re.compile(r"weapon|sword|blade|staff|spear|axe|hammer|shield|scythe|bow|lance|mace|prop|pickaxe", re.I)
SKIP = re.compile(r"physics|vfx|shard|chain|_lod|blockout|broken|split|destroy|gib|fx_|_fx|icon|ui_|socket|anim|head_legs|_head$", re.I)
SHEET = os.path.join(ROOT, "sheets", "bosses.json")


class R2:
    def __init__(self):
        self.p = subprocess.Popen([EXE, R2DIR], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.ask("HELLO")

    def ask(self, cmd):
        self.p.stdin.write((cmd + "\n").encode()); self.p.stdin.flush()
        h = self.p.stdout.readline().decode().strip()
        if not h.startswith("OK"):
            return None
        return self.p.stdout.read(int(h.split()[-1])) if not h.startswith("OK r2read") else b""


def bone_names(b):
    n = struct.unpack_from("<I", b, 4)[0]
    q, out = 8, []
    for _ in range(n):
        ln = struct.unpack_from("<H", b, q)[0]
        out.append(b[q + 2:q + 2 + ln].decode(errors="replace"))
        q += 2 + ln + 44
    return out


def materials_of(b):
    n = struct.unpack_from("<I", b, 4)[0]
    q = 8
    for _ in range(n):
        q += 2 + struct.unpack_from("<H", b, q)[0] + 44
    nm = struct.unpack_from("<I", b, q)[0]
    q += 4
    out = []
    for _ in range(nm):
        ln = struct.unpack_from("<H", b, q)[0]
        out.append(b[q + 2:q + 2 + ln].decode(errors="replace"))
        q += 2 + ln
    return out


def toks(s):
    s = re.sub(r"([a-z])([A-Z])", r"_", s)
    return {t for t in re.split(r"[^a-z0-9]+", s.lower()) if len(t) > 1 and not t.isdigit()} - {
        "mi", "mat", "material", "lod", "lod0", "inst", "t", "m", "sk", "the", "diff", "diffuse", "d"}


def pick_bone(bones, mesh_name):
    low = [(i, n.lower()) for i, n in enumerate(bones)]
    left = bool(re.search(r"shield", mesh_name, re.I))
    side = "_l|left|l_" if left else "_r|right|r_"
    sideRx = re.compile(r"(^|[_\W])(l|left)($|[_\W])" if left else r"(^|[_\W])(r|right)($|[_\W])")
    tiers = [
        [n for _, n in low if re.search(r"weapon|prop", n) and sideRx.search(n)],
        [] if left else [n for _, n in low if re.search(r"weapon|prop", n)],
        [n for _, n in low if re.search(r"hand", n) and sideRx.search(n) and not re.search(r"finger|thumb|index|middle|ring|pinky|\d", n)],
        [n for _, n in low if re.search(r"hand", n) and sideRx.search(n)],
    ]
    for t in tiers:
        if t:
            names = [bones[[x.lower() for x in bones].index(n)] for n in t]
            return names[0], names
    return None, []


isdiff = re.compile(r"(?i)(_d|_diff|_diffuse|_basecolor|_bc|_albedo|_color|_col|_alb|_diff_\w+)$")


def main():
    stores = r2pak.open_all(os.path.join(R2DIR, "Remnant2", "Content", "Paks"))
    files = {}
    for s in stores:
        for k in s.files:
            files[k] = 1
    R = R2()
    sheet = json.load(open(SHEET, encoding="utf-8"))
    got = 0
    for r in sheet["rows"]:
        folders = {re.match(r"(.*/Characters/[^/]+)/", r[c]).group(1) for c in ("mesh", "skeleton")}
        body = r["mesh"].rsplit("/", 1)[1][:-7].lower()
        mini = "mini" in body
        cands = sorted(k for f in folders for k in files
                       if k.startswith(f + "/") and k.endswith(".uasset") and KW.search(k.rsplit("/", 1)[1][:-7])
                       and not SKIP.search(k.rsplit("/", 1)[1][:-7]) and "/Animations/" not in k)
        sk = R.ask("SKEL\t" + r["skeleton"])
        bones = bone_names(sk) if sk and sk[:4] == b"RSKL" else []
        ok, mats = [], {}
        for c in cands:
            m = R.ask("MESH\t" + c)
            if m and m[:4] == b"RMSH":
                ok.append(c)
                mats[c] = materials_of(m)
        # the boss's own variant: a miniboss takes the MiniBoss weapon files when there are any
        mine = [c for c in ok if ("mini" in c.lower()) == mini] or ok
        # one entry per kind of prop (weapon / shield / bow ...): the first of each, preferring "_SM"-less skeletal names
        kinds = {}
        for c in mine:
            n = c.rsplit("/", 1)[1][:-7].lower()
            kind = "shield" if "shield" in n else "weapon"
            kinds.setdefault(kind, []).append(c)
        out = []
        for kind, lst in kinds.items():
            c = lst[0]
            bone, alts = pick_bone(bones, c)
            note = ""
            if len(lst) > 1:
                note += f" {len(lst)} meshes ({', '.join(x.rsplit('/', 1)[1][:-7] for x in lst)})"
            if len(alts) > 1:
                note += f" bones {alts}"
            if not bone:
                print(f"AMBIGUOUS {r['id']}: {c.rsplit('/', 1)[1]}: no weapon/prop/hand bone in {len(bones)} bones; skipped")
                continue
            if note:
                print(f"AMBIGUOUS {r['id']}: picked {bone}={c.rsplit('/', 1)[1]};{note}")
            out.append(f"{bone}={c}")
            # the prop's own textures join the boss's textures column as "<material>=asset" (matched by slot name)
            have = [e.split("=", 1)[0] for e in r["textures"] if "=" in e]
            diffuse = sorted(f for fo in folders for f in files if f.startswith(fo + "/") and "/Texture" in f
                             and f.endswith(".uasset") and isdiff.search(f.rsplit("/", 1)[1][:-7]))
            for mat in mats[c]:
                if any(h in mat for h in have if h != "*"):
                    continue
                best = max(((len(toks(mat) & toks(f.rsplit("/", 1)[1][:-7])), -len(f), f) for f in diffuse), default=None)
                if best and best[0] > 0:
                    r["textures"].insert(0 if "*=" not in "".join(r["textures"]) else len(r["textures"]) - 1, f"{mat}={best[2]}")
                    have.append(mat)
                else:
                    print(f"AMBIGUOUS {r['id']}: no texture found for {c.rsplit('/', 1)[1]} material {mat}; the body's are used")
        r["props"] = out or ["none"]
        got += bool(out)
        print(f"{r['id']}: {len(cands)} candidates, {len(ok)} readable -> {r['props']}")
    sheet["columns"]["props"] = "list"
    print(f"{got} of {len(sheet['rows'])} bosses have props")
    if WRITE:
        with open(SHEET, "w", encoding="utf-8", newline="\n") as f:
            json.dump(sheet, f, indent=1, ensure_ascii=False)
            f.write("\n")
        print("written")


main()
