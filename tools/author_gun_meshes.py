#!/usr/bin/env python3
"""Dev tool: point each weapons row at Remnant II's own 3D model of the gun, in the player's install.

Adds to sheets/weapons.json: mesh (the gun's skeletal mesh, or none), mesh_fwd (the axis its barrel points along:
py / ny / px / nx, from the model's shape: the barrel is the long end away from the grip at the origin) and mesh_tex
(slot=texture entries, "*" for the rest). Corrupted and event variants fall back to their base gun's model.
usage: author_gun_meshes.py <Remnant II dir> <r2read (Linux)>
"""
import json, os, re, struct, subprocess, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import r2pak  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, ".."))
R2DIR, EXE = sys.argv[1], sys.argv[2]
C = "Remnant2/Content/"
FILES = sorted({k for s in r2pak.open_all(os.path.join(R2DIR, "Remnant2", "Content", "Paks")) for k in s.files
                if k.endswith(".uasset")})


def base(p):
    return p.rsplit("/", 1)[1][:-7]


def norm(s):
    return re.sub(r"[^a-z0-9]", "", s.lower())


def tokens(s):
    s = re.sub(r"([a-z])([A-Z])", r"\1_\2", s)
    return {t for t in re.split(r"[^a-z0-9]+", s.lower()) if len(t) > 1} - {"player", "sk", "sm", "t", "m", "mi", "diff"}


P = subprocess.Popen([EXE, R2DIR], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
P.stdin.write(b"HELLO\n")
P.stdin.flush()
P.stdout.readline()


def mesh(asset):
    P.stdin.write(("MESH\t" + asset + "\n").encode())
    P.stdin.flush()
    h = P.stdout.readline().decode().strip()
    if not h.startswith("OK"):
        return None
    b = P.stdout.read(int(h.split()[-1]))
    q = 4
    nb = struct.unpack_from("<I", b, q)[0]
    q += 4
    for _ in range(nb):
        q += 2 + struct.unpack_from("<H", b, q)[0] + 44
    nm = struct.unpack_from("<I", b, q)[0]
    q += 4
    mats = []
    for _ in range(nm):
        ln = struct.unpack_from("<H", b, q)[0]
        mats.append(b[q + 2:q + 2 + ln].decode(errors="replace"))
        q += 2 + ln
    ns = struct.unpack_from("<I", b, q)[0]
    q += 4 + ns * 20
    nv = struct.unpack_from("<I", b, q)[0]
    q += 4
    v = np.frombuffer(b, dtype=np.uint8, count=nv * 52, offset=q).reshape(nv, 52)
    pos = v[:, :12].copy().view(np.float32).reshape(nv, 3)
    return {"materials": mats, "min": np.percentile(pos, 1, axis=0), "max": np.percentile(pos, 99, axis=0)}


MESHES = [f for f in FILES if "/Items/Weapons/" in f and re.search(r"/Mesh(es)?/Player_[^/]+\.uasset$", f)
          and "Physics" not in f]
TEX = [f for f in FILES if "/Items/Weapons/" in f and "/Texture" in f and
       re.search(r"(?i)(_d|_diff|_diffuse|_basecolor|_bc|_albedo|_color|_col)$", base(f))]

sh = json.load(open(os.path.join(ROOT, "sheets", "weapons.json"), encoding="utf-8"))
cols = sh["columns"]
cols["mesh"] = "str"
cols["mesh_fwd"] = "enum:py|ny|px|nx"
cols["mesh_tex"] = "list"
found = 0
for r in sh["rows"]:
    a = r["name"]["r2text"]["asset"] if isinstance(r["name"], dict) else ""
    folder = a.rsplit("/", 1)[0]
    cands = [m for m in MESHES if m.startswith(folder + "/")]
    want = tokens(r["id"].replace("corrupted", "")) | {norm(r["id"]).replace("corrupted", "")}
    if not cands:  # variants: the base gun's model, its corrupted look first
        sc = sorted(((len(want & (tokens(base(m)) | {norm(base(m))[6:]})), "orrupt" in m, -len(m), m) for m in MESHES),
                    reverse=True)
        cands = [sc[0][3]] if sc and sc[0][0] > 0 else []
    if "corrupted" in r["id"]:
        cands.sort(key=lambda m: 0 if "orrupt" in m else 1)
    else:
        cands.sort(key=lambda m: (1 if "orrupt" in m else 0, len(m)))
    info = None
    for m in cands[:3]:
        info = mesh(m)
        if info:
            r["mesh"] = m
            break
    if not info:
        r["mesh"], r["mesh_fwd"], r["mesh_tex"] = "none", "px", ["none"]
        continue
    found += 1
    mn, mx = info["min"], info["max"]
    ax = 0 if (mx[0] - mn[0]) >= (mx[1] - mn[1]) else 1
    sign = "p" if abs(mx[ax]) >= abs(mn[ax]) else "n"
    r["mesh_fwd"] = sign + ("x" if ax == 0 else "y")
    mf = r["mesh"].rsplit("/Mesh", 1)[0]
    tex = [t for t in TEX if t.startswith(mf + "/")] or [t for t in TEX if len(tokens(base(t)) & tokens(base(r["mesh"]))) >= 1]
    corrupt = "orrupt" in r["mesh"]
    tex.sort(key=lambda t: (0 if ("orrupt" in t) == corrupt else 1, -len(tokens(base(t)) & tokens(base(r["mesh"]))), len(t)))
    out = []
    for slot in info["materials"]:
        st = tokens(slot)
        best = max(tex, key=lambda t: len(st & tokens(base(t)))) if tex else None
        if best and len(st & tokens(base(best))) > 0:
            out.append(f"{slot}={best}")
    if tex:
        out.append(f"*={tex[0]}")
    r["mesh_tex"] = out or ["none"]
json.dump(sh, open(os.path.join(ROOT, "sheets", "weapons.json"), "w", encoding="utf-8"), indent=1, ensure_ascii=False)
print(f"{found} of {len(sh['rows'])} weapons have a model")
P.kill()
