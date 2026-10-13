#!/usr/bin/env python3
"""Dev tool: author sheets/bosses.json and sheets/boss_attacks.json from the player's own Remnant II install.

For each boss of tools/boss_roster.json it asks the Linux build of r2read for the candidate meshes, the skeleton and
the animations, then:
  - picks the boss's body mesh (largest that fits the skeleton) and a diffuse texture per material slot;
  - sorts the animations into idle / walk / run / turns / roar / flinch / stagger / death / attacks by name;
  - measures each attack from the animation itself: forward kinematics of the skeleton, the limb that moves fastest
    (relative to the root, so lunges do not count) gives the hit windows, the reach and the side it strikes;
    attacks are melee, area (slams, shockwaves) or ranged (casts, shots: a projectile released at the peak);
  - finds which way the model faces (where its attacks land), and its walk / run speeds;
  - picks Remnant II's own sound events for roar, pain, death, steps and each attack;
  - reads the boss's display name from its character blueprint, and its loot from the toolkit item data.
Nothing of Remnant II is written to the sheets except asset paths and text keys.

Resumable: each boss is cached in .check/bosses/<id>.json; a run stops after --budget seconds.
usage: author_bosses.py <Remnant II dir> <r2read (Linux)> [--budget S] [--only id] [--write]
"""
import json, os, re, struct, subprocess, sys, time
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import r2pak, r2text  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, ".."))
R2DIR, EXE = sys.argv[1], sys.argv[2]
BUDGET = float(sys.argv[sys.argv.index("--budget") + 1]) if "--budget" in sys.argv else 160
ONLY = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else None
CACHE = os.path.join(ROOT, ".check", "bosses")
os.makedirs(CACHE, exist_ok=True)
T0 = time.time()
C = "Remnant2/Content/"

STORES = r2pak.open_all(os.path.join(R2DIR, "Remnant2", "Content", "Paks"))
FILES = {}
for s in STORES:
    for k, v in s.files.items():
        FILES[k] = (s, v)


def read(path):
    s, e = FILES[path]
    return s.read(e)


def under(prefix, suffix=".uasset"):
    p = C + prefix.rstrip("/") + "/"
    return sorted(k for k in FILES if k.startswith(p) and k.endswith(suffix))


def base(path):
    return path.rsplit("/", 1)[1][:-7]


def norm(s):
    return re.sub(r"[^a-z0-9]", "", s.lower())


def tokens(s):
    s = re.sub(r"([a-z])([A-Z])", r"\1_\2", s)
    return {t for t in re.split(r"[^a-z0-9]+", s.lower()) if len(t) > 1 and not t.isdigit()} - {
        "mi", "mat", "material", "lod", "lod0", "inst", "t", "m", "sk", "the"}


# ---------------------------------------------------------------- r2read

class R2:
    def __init__(self):
        self.p = subprocess.Popen([EXE, R2DIR], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.p.stdin.write(b"HELLO\n")
        self.p.stdin.flush()
        self.p.stdout.readline()

    def ask(self, cmd):
        self.p.stdin.write((cmd + "\n").encode())
        self.p.stdin.flush()
        h = self.p.stdout.readline().decode().strip()
        if not h.startswith("OK"):
            return None
        return self.p.stdout.read(int(h.split()[-1]))


R = R2()


def bones_of(b, q):
    n = struct.unpack_from("<I", b, q)[0]
    q += 4
    out = []
    for _ in range(n):
        ln = struct.unpack_from("<H", b, q)[0]
        name = b[q + 2:q + 2 + ln].decode(errors="replace")
        q += 2 + ln
        par = struct.unpack_from("<i", b, q)[0]
        v = struct.unpack_from("<10f", b, q + 4)
        q += 44
        out.append((name, par, v))
    return out, q


def mesh(asset):
    b = R.ask("MESH\t" + asset)
    if not b or b[:4] != b"RMSH":
        return None
    bones, q = bones_of(b, 4)
    nm = struct.unpack_from("<I", b, q)[0]
    q += 4
    mats = []
    for _ in range(nm):
        ln = struct.unpack_from("<H", b, q)[0]
        mats.append(b[q + 2:q + 2 + ln].decode(errors="replace"))
        q += 2 + ln
    ns = struct.unpack_from("<I", b, q)[0]
    secmat = [struct.unpack_from("<h", b, q + 4 + i * 20)[0] for i in range(ns)]
    q += 4 + ns * 20
    nv = struct.unpack_from("<I", b, q)[0]
    q += 4
    v = np.frombuffer(b, dtype=np.uint8, count=nv * 52, offset=q).reshape(nv, 52)
    pos = v[:, :12].copy().view(np.float32).reshape(nv, 3)
    # the body's size without stray far vertices (effects planes, detached bits)
    lo, hi = np.percentile(pos, 1, axis=0), np.percentile(pos, 99, axis=0)
    return {"asset": asset, "bones": [x[0] for x in bones], "materials": mats, "verts": int(nv),
            "min": lo.tolist(), "max": hi.tolist(), "slots": max([m + 1 for m in secmat] + [len(mats)])}


def skeleton(asset):
    b = R.ask("SKEL\t" + asset)
    if not b or b[:4] != b"RSKL":
        return None
    bones, _ = bones_of(b, 4)
    return bones


def anim(asset):
    b = R.ask("ANIM\t" + asset)
    if not b or b[:4] != b"RANM":
        return None
    tr, sm, rate = struct.unpack_from("<IIf", b, 4)
    t2b = np.frombuffer(b, dtype=np.int32, count=tr, offset=16)
    d = np.frombuffer(b, dtype=np.float32, count=sm * tr * 10, offset=16 + tr * 4).reshape(sm, tr, 10)
    return t2b, d, rate


def quat_mat(q):
    x, y, z, w = q[..., 0], q[..., 1], q[..., 2], q[..., 3]
    return np.stack([np.stack([1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)], -1),
                     np.stack([2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)], -1),
                     np.stack([2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)], -1)], -2)


def fk(skel, a):
    """Bone positions (samples, bones, 3) in model space."""
    t2b, d, rate = a
    ns, nb = d.shape[0], len(skel)
    ref = np.array([s[2] for s in skel], dtype=np.float32)
    L = np.broadcast_to(ref, (ns, nb, 10)).copy()
    for tr, b in enumerate(t2b):
        if 0 <= b < nb:
            L[:, b] = d[:, tr]
    Rm = quat_mat(L[..., 0:4]) * L[..., None, 7:10]
    GR = np.zeros((ns, nb, 3, 3), np.float32)
    GT = np.zeros((ns, nb, 3), np.float32)
    for b in range(nb):
        p = skel[b][1]
        if p < 0 or p >= b:
            GR[:, b], GT[:, b] = Rm[:, b], L[:, b, 4:7]
        else:
            GR[:, b] = GR[:, p] @ Rm[:, b]
            GT[:, b] = np.einsum("sij,sj->si", GR[:, p], L[:, b, 4:7]) + GT[:, p]
    return GT


SKIP_BONE = re.compile(r"(?i)(ik_|twist|ctrl|camera|attach|socket|_end$|finger|thumb|index|middle|ring|pinky|toe|"
                       r"eye|lid|brow|lip|cheek|tongue|teeth|tooth|hair|cloth|cape|chain|tassel|dyn|jiggle|vfx|fx_|"
                       r"tentacle_.*_0[5-9]|_tip|nub|prop)")


def measure(skel, a):
    """Striker bone, hit windows (fractions), reach (cm), striker xy at the peak, root motion (cm)."""
    t2b, d, rate = a
    P = fk(skel, a)
    if not np.isfinite(P).all() or np.abs(P).max() > 1e5:
        return None
    rel = P - P[:, :1, :]
    V = np.linalg.norm(np.diff(rel, axis=0), axis=2) * rate
    ok = np.array([i > 2 and not SKIP_BONE.search(skel[i][0]) for i in range(len(skel))])
    if not ok.any() or V.shape[0] < 3:
        return None
    peaks = np.where(ok, V.max(axis=0), 0)
    st = int(np.argmax(peaks))
    s = V[:, st]
    pk = float(s.max())
    if pk < 150:  # nothing moves fast: not an attack
        return None
    on = s > 0.55 * pk
    segs, i = [], 0
    while i < len(on):
        if on[i]:
            j = i
            while j + 1 < len(on) and (on[j + 1] or (j + 3 < len(on) and on[j + 2:j + 4].any())):
                j += 1
            segs.append((i, j))
            i = j + 1
        else:
            i += 1
    segs = [g for g in segs if s[g[0]:g[1] + 1].max() >= 0.6 * pk]
    segs = sorted(sorted(segs, key=lambda g: -s[g[0]:g[1] + 1].max())[:3])
    n = max(1, len(s))
    wins = [(max(0.0, g[0] / n - 0.02), min(1.0, (g[1] + 1) / n + 0.02)) for g in segs]
    frames = np.concatenate([np.arange(g[0], g[1] + 2) for g in segs]).clip(0, P.shape[0] - 1)
    reach = float(np.linalg.norm(rel[frames, st, :2], axis=1).max())
    k = int(np.argmax(s))
    root = (P[-1, 0] - P[0, 0]).tolist()
    return {"bone": skel[st][0], "windows": wins, "reach": reach, "xy": rel[k, st, :2].tolist(), "root": root,
            "len": (d.shape[0] - 1) / rate, "peak": pk, "up": float(rel[frames, st, 2].max())}


# ---------------------------------------------------------------- classification

EXCL = re.compile(r"(?i)(additive|_add$|_add_|aimoffset|_ao_|^ao_|lookpose|look_|_pose|pose_|blendspace|^bs_|_bs_|montage|"
                  r"^am_|cine|emote|_vo_|^vo_|eat_|_eat|int_|interact|npc|dialog|ragdoll|_mirror|preview|test|_old|"
                  r"temp|retarget|bind)")
SEGMENT = re.compile(r"(?i)(_start|_loop|_end|_stop|_recover|_to_|from|transition|windup|_out$|_in$|_miss|_fail|"
                     r"_outro|_intro_loop)")
ATTACK = re.compile(r"(?i)(atk|attack|melee|swipe|slash|bite|slam|stomp|smash|cast|shoot|fire_|_fire|throw|spit|beam|"
                    r"blast|lunge|leap|pounce|grab|sweep|claw|punch|kick|strike|stab|thrust|swing|combo|spin|tail|ram|"
                    r"tackle|laser|missile|comet|nova|pulse|summon|barrage|volley|bolt|orb|charge_atk|whip|cleave)")
RANGED = re.compile(r"(?i)(cast|shoot|fire|throw|spit|beam|blast|missile|laser|comet|projectile|bolt|orb|volley|"
                    r"barrage|summon|ranged|spray|lob|grenade|bomb)")
AREA = re.compile(r"(?i)(slam|stomp|shockwave|nova|pulse|aoe|explo|quake|ground|smash|spin|burst|eruption|whirl)")


def classify(names):
    """names: {base name: asset} -> roles: ranked candidates per role, and the attacks."""
    good = {n: a for n, a in names.items() if not EXCL.search(n)}
    # the creature's own animations share a prefix ("Pan_Wolf_"): those first
    import collections
    pre = collections.Counter("_".join(n.split("_")[:2]) for n in good).most_common(1)
    pre = pre[0][0] if pre else ""

    loops = re.compile(r"(?i)(_start|_end|_stop|_recover|_to_|from|transition|_out$|_in$)")

    def first(*pats, avoid=SEGMENT):
        out = []
        for pat in pats:
            c = sorted((n for n in good if re.search(pat, n, re.I) and not (avoid and avoid.search(n)) and n not in out),
                       key=lambda n: (0 if n.startswith(pre) else 1, len(n)))
            out += c
        return out
    r = {
        "idle": first(r"(^|_)idle$", r"(^|_)idle_(combat|01|a|loop)$", r"(^|_)combat_idle", r"(^|_)idle", avoid=loops),
        "walk": first(r"walk_f(wd|orward)?$", r"walk_f(wd|orward)?_(loop|01|a)$", r"(^|_)walk$", r"walk_f", r"(^|_)walk",
                      r"(^|_)move_f", avoid=loops),
        "run": first(r"(run|sprint|charge|jog|gallop|dash)_f(wd|orward)?$", r"(run|sprint|jog|gallop)_f(wd|orward)?_",
                     r"(^|_)(run|sprint|jog|gallop)$", avoid=loops),
        "turn_l": first(r"turn_?l(eft)?_?90", r"turn_90_l", r"turn_?l(eft)?$", r"turn.*_l$"),
        "turn_r": first(r"turn_?r(ight)?_?90", r"turn_90_r", r"turn_?r(ight)?$", r"turn.*_r$"),
        "roar": first(r"(roar|howl|taunt|scream|enrage|bellow|aggro)", r"(alert|intro)"),
        "flinch": first(r"flinch", r"(hit_?react|impact_(head|body|f)|impact_fl|impact_fr)", r"(impact|pain|hurt|react)"),
        "stun": first(r"stun", r"stagger", r"(knockdown|knocked|daze)"),
        "death": first(r"(death|die|dead)$", r"(death|die)(_0?1|_a|_f)?$", r"(death|die)", avoid=re.compile(r"(?i)(loop|pose|idle)")),
    }
    used = {v[0] for v in r.values() if v}
    atk = []
    for n in sorted(good, key=lambda x: (len(x), x)):
        if n in used or not ATTACK.search(n) or SEGMENT.search(n):
            continue
        if re.search(r"(?i)(turn|walk|run|idle|death|flinch|impact|evade|dodge|jump_back)", n) and not re.search(
                r"(?i)(atk|attack)", n):
            continue
        atk.append(n)
    r["attacks"] = atk
    return r


# ---------------------------------------------------------------- sounds

def playable(ev):
    d = read(ev)
    p = d.find(b"BKHD")
    if p < 0:
        return False
    while p + 8 <= len(d):
        tag, sz = d[p:p + 4], struct.unpack_from("<I", d, p + 4)[0]
        if tag == b"HIRC":
            n = struct.unpack_from("<I", d, p + 8)[0]
            q = p + 12
            for _ in range(n):
                t, ln = d[q], struct.unpack_from("<I", d, q + 1)[0]
                if t == 2:
                    plug, src = struct.unpack_from("<I", d, q + 9)[0], struct.unpack_from("<I", d, q + 14)[0]
                    if plug == 0x00040001 and f"{C}WwiseAudio/Media/{src}.ubulk" in FILES:
                        return True
                q += 5 + ln
            return False
        if not tag.isalpha():
            return False
        p += 8 + sz
    return False


_play = {}


def ok_sound(ev):
    if ev not in _play:
        _play[ev] = playable(ev)
    return _play[ev]


def pick_sound(evs, *pats):
    for avoid in (r"(?i)(loop|stop|_lp$|cine|vo_\d)", None):
        for pat in pats:
            for ev in evs:
                b = base(ev)
                if avoid and re.search(avoid, b):
                    continue
                if re.search(pat, b, re.I) and ok_sound(ev):
                    return ev
    return "none"


def attack_sound(evs, anim_name):
    want = tokens(anim_name) - {"atk", "attack", "pan", "wolf", "root", "fae", "nerud"}
    best, score = None, 0
    for ev in evs:
        b = base(ev)
        if re.search(r"(?i)(loop|stop|cine|footstep|foley)", b):
            continue
        sc = len(want & tokens(b))
        if sc > score and ok_sound(ev):
            best, score = ev, sc
    return best or pick_sound(evs, r"voc.*(attack|atk|effort|grunt|swing)", r"(attack|atk|swing|whoosh)")


# ---------------------------------------------------------------- one boss

FWD = {"py": (0, 1), "ny": (0, -1), "px": (1, 0), "nx": (-1, 0)}


def right_of(f):  # Unreal's left-handed frame: facing +x the right side is +y
    return {"py": (-1, 0), "ny": (1, 0), "px": (0, 1), "nx": (0, -1)}[f]


def name_text(folder, hint, want=None):
    files = [f for f in under(folder) if re.match(r"(?i)char(acter)?_", base(f)) and not re.search(
        r"(?i)(soundset|statemachine|eventtree|attacks|visualset|_ai|blackboard|navfilter|anims?bp|spawn|projectile|"
        r"_add|minion|summon)", base(f))]
    files.sort(key=lambda f: (0 if hint and hint.lower() in base(f).lower() else 1, len(base(f))))
    for f in files:
        try:
            for k, t in r2text.texts(read(f)):
                if re.match(r"^[A-Z][A-Za-z'’ :,\-]{2,40}$", t) and t not in ("None", "True", "False") and (
                        not want or re.search(want, t)):
                    return {"asset": f, "key": k}, t
        except Exception:
            pass
    return None, None


def author(row):
    bid = row["id"]
    folder = row["folder"]
    log = []
    # meshes
    cands = []
    excl = row.get("exclude")
    for d in row.get("mesh", []) + [folder]:
        for f in under(d):
            b = base(f)
            if not (re.search(r"/Mesh(es)?/", f) or b.lower().startswith("sk_") or "/Quests/" in f):
                continue
            if re.search(r"(?i)(skeleton|physics|_pa$|anim|abp|^bp_|blueprint|^mi?_|^t_|lod|^sm_|static|cloth|proxy|"
                         r"destruct|_rig|^ik|socket|^ns_|^p_|fx|gib|ragdoll|_pose|corpse)", b) or "/Animation" in f:
                continue
            if excl and re.search(excl, b, re.I):
                continue
            if f not in cands:
                cands.append(f)
    hint = (row.get("mesh_hint") or "").lower()
    short = folder.rsplit("/", 1)[1].lower()
    cands.sort(key=lambda f: (0 if hint and hint in base(f).lower() else 1,
                              0 if re.search(r"(?i)boss", base(f)) else 1,
                              0 if base(f).lower() == short else 1, len(base(f))))
    meshes = []
    for f in cands[:12]:
        m = mesh(f)
        if m and m["verts"] > 500:
            meshes.append(m)
    if not meshes:
        return None, ["no readable skeletal mesh"]
    # the animations, and a few plain ones to test which skeleton they were made for
    names = {}
    for f in under(folder):
        if "/Animation" not in f or re.search(r"(?i)/(blendspaces?|aimoffsets?|lookposes?|working|cine\w*)/", f):
            continue
        names.setdefault(base(f), f)
    raw = {}

    def rawanim(n):
        if n not in raw:
            raw[n] = anim(names[n]) if n in names else None
        return raw[n]
    probes = sorted((n for n in names if re.search(r"(?i)(^|_)(idle|walk_f|atk)", n) and not EXCL.search(n)), key=len)[:4]

    def fits(sk):
        return sum(1 for n in probes if rawanim(n) is not None and len(rawanim(n)[0]) and
                   (rawanim(n)[0] < len(sk)).mean() >= 0.8 and len(rawanim(n)[0]) <= len(sk) + 16)
    # skeletons
    skels = []
    for f in under(folder):
        if re.search(r"(?i)skeleton", base(f)):
            sk = skeleton(f)
            if sk:
                skels.append((f, sk))
    for d in row.get("mesh", []):
        parent = d.rsplit("/", 1)[0]
        for f in under(parent):
            if re.search(r"(?i)skeleton", base(f)) and all(f != s[0] for s in skels):
                sk = skeleton(f)
                if sk:
                    skels.append((f, sk))
    best = None
    for m in meshes:
        mb = set(m["bones"])
        for f, sk in skels:
            ov = len(mb & {b[0] for b in sk}) / max(1, len(mb))
            key = (fits(sk), 1 if hint and hint in base(m["asset"]).lower() else 0,
                   1 if re.search(r"(?i)boss", base(m["asset"])) else 0, ov > 0.8, m["verts"] if ov > 0.6 else 0)
            if ov > 0.6 and (best is None or key > best[0]):
                best = (key, m, f, sk)
    if not best:
        return None, ["no skeleton fits the meshes: " + ", ".join(base(m["asset"]) for m in meshes[:4])]
    _, m, skel_asset, skel = best
    log.append(f"mesh {base(m['asset'])} ({m['verts']} verts, {len(m['materials'])} slots), skeleton {base(skel_asset)}")
    roles = classify(names)
    an = {}

    def get(n):
        if n not in an:
            a = rawanim(n)
            if a is not None and (len(a[0]) == 0 or (a[0] < len(skel)).mean() < 0.8):
                a = None  # made for another skeleton
            an[n] = a
        return an[n]
    for k in ("idle", "walk", "run", "turn_l", "turn_r", "roar", "flinch", "stun", "death"):
        roles[k] = next((n for n in roles[k][:6] if get(n) is not None), None)
    if not roles["idle"]:
        return None, log + ["no idle animation among " + str(len(names))]
    # attacks
    atk = []
    for n in roles["attacks"]:
        if len(atk) >= 14:
            break
        a = get(n)
        if a is None:
            continue
        ms = measure(skel, a)
        if not ms or not ms["windows"] or ms["len"] < 0.5 or ms["len"] > 9:
            continue
        ms["name"] = n
        atk.append(ms)
    if not atk:
        return None, log + ["no measurable attack among " + str(len(roles["attacks"]))]
    # facing: where the attacks land (strikers at their peak, plus forward lunges)
    acc = np.zeros(2)
    for ms in atk:
        v = np.array(ms["xy"]) + 0.5 * np.array(ms["root"][:2])
        nv = np.linalg.norm(v)
        if nv > 1:
            acc += v / nv
    fwd = ("px" if acc[0] > 0 else "nx") if abs(acc[0]) > abs(acc[1]) else ("py" if acc[1] > 0 else "ny")
    F, Rt = np.array(FWD[fwd]), np.array(right_of(fwd))
    mn, mx = np.array(m["min"]), np.array(m["max"])
    corners = np.array([[x, y] for x in (mn[0], mx[0]) for y in (mn[1], mx[1])])
    along = corners @ F
    length = float(along.max() - along.min())
    height = float(mx[2] - mn[2])
    # attack rows
    out = []
    for ms in atk:
        n = ms["name"]
        kind = "ranged" if RANGED.search(n) else "aoe" if AREA.search(n) else "melee"
        xy = np.array(ms["xy"])
        ang = abs(float(np.degrees(np.arctan2(xy @ Rt, xy @ F)))) if np.linalg.norm(xy) > 1 else 0
        lunge = float(np.array(ms["root"][:2]) @ F)
        reach = ms["reach"] / 100 + 1.0
        if kind == "ranged":
            wins = [(round(max(0, ms["windows"][0][0]), 3), round(min(1, ms["windows"][0][0] + 0.06), 3))]
            mn_m, mx_m, arc = 8, 40, 60
        elif kind == "aoe":
            wins = ms["windows"]
            mn_m, mx_m, arc = 0, round(reach + 1, 1), 360
            reach = max(reach, 0.45 * length / 100 + 3)
        else:
            wins = ms["windows"]
            arc = int(min(220, max(70, 2 * ang + 60)))
            mn_m = round(max(0, lunge / 100 * 0.5), 1) if lunge > 500 else 0
            mx_m = round(reach + max(0, lunge) / 100 + 1, 1)
        dmg = round(10 + 2.5 * row["threat"] + (5 if ms["len"] > 2.5 else 0) + (4 if kind == "aoe" else 0) -
                    (3 if kind == "ranged" else 0), 1)
        out.append({"id": f"{bid}_{norm(n)[-28:]}", "boss": bid, "anim": n, "kind": kind, "min_m": mn_m, "max_m": mx_m,
                    "hits": [f"{a:.2f}-{b:.2f}" for a, b in wins], "reach_m": round(reach, 1), "arc_deg": arc,
                    "damage": dmg, "weight": 2 if kind == "melee" else 1.5, "cooldown_s": round(3 + ms["len"], 1),
                    "track": 0.8 if kind == "melee" else 0.5, "snd": "none", "_bone": ms["bone"]})
    # speeds from root motion when the loops carry it, else from size
    def speed(n):
        if not n:
            return 0
        a = get(n)
        if a is None:
            return 0
        P0 = fk(skel, (a[0], a[1][[0, -1]], a[2]))
        d = float(np.linalg.norm(P0[-1, 0, :2] - P0[0, 0, :2]))
        ln = (a[1].shape[0] - 1) / a[2]
        return d / 100 / ln if d > 50 and ln > 0 else 0
    walk = speed(roles["walk"]) or (max(1.2, min(4.0, 0.35 * length / 100)) if roles["walk"] else 0)
    run = speed(roles["run"]) or (2.6 * walk if roles["run"] else walk)
    # textures
    tex = []
    isdiff = re.compile(r"(?i)(_d|_diff|_diffuse|_basecolor|_bc|_albedo|_color|_col|_alb|_diff_\w+)$")
    tdirs = {folder} | {d.rsplit("/", 1)[0] for d in row.get("mesh", [])} | set(row.get("mesh", []))
    diffuse = sorted({f for d in tdirs for f in under(d) if "/Texture" in f and isdiff.search(base(f))})
    mtok = tokens(base(m["asset"])) | tokens(folder.rsplit("/", 1)[1])
    if not diffuse:  # textures kept elsewhere in the same world
        world = folder.split("/")[0]
        diffuse = sorted(f for f in FILES if f.startswith(C + world + "/") and f.endswith(".uasset") and "/Texture" in f
                         and isdiff.search(base(f)) and len(mtok & tokens(base(f))) >= 2)
    th = (row.get("tex_hint") or "").lower()

    def tscore(f, want):
        return (len(want & tokens(base(f))) + (2 if th and th in base(f).lower() else 0)
                - (1 if re.search(r"(?i)(weapon|sword|shield|hair|eye|vfx|fx)", base(f)) else 0), -len(base(f)))
    main = max(diffuse, key=lambda f: tscore(f, mtok)) if diffuse else None
    for slot in m["materials"]:
        st = tokens(slot)
        sc = max(diffuse, key=lambda f: tscore(f, st)) if diffuse else None
        if sc and len(st & tokens(base(sc))) > 0:
            tex.append(f"{slot}={sc}")
    if main and (len(tex) < m["slots"]):
        tex.append(f"*={main}")  # every other section: the body's main texture
    # sounds
    evs = sorted(k for k in FILES if k.startswith(f"{C}WwiseAudio/Events/Creature/{row.get('snd', '-')}/") and
                 k.endswith(".uasset"))
    snd = {"snd_roar": pick_sound(evs, r"voc.*(roar|howl|taunt|scream|aggro|alert|laugh|shout|intro)",
                                  r"(roar|howl|taunt|scream|aggro|alert)"),
           "snd_pain": pick_sound(evs, r"voc.*(pain|hurt|flinch|damage|stagger)", r"(pain|hurt|flinch|impact)"),
           "snd_death": pick_sound(evs, r"voc.*(death|die)", r"(death|die|dissolve)"),
           "snd_step": pick_sound(evs, r"(footstep|foot_|step|foley.*(heavy|move|walk))", r"(foley|move)")}
    for a in out:
        a["snd"] = attack_sound(evs, a["anim"]) if evs else "none"
    name, text = name_text(folder, row.get("bp_hint"), row.get("name_re"))
    if not name and row.get("mesh"):
        for d in row["mesh"]:
            name, text = name_text(d.rsplit("/", 1)[0], row.get("bp_hint"), row.get("name_re"))
            if name:
                break
    if not name and row.get("title"):
        text = row["title"]
    adir = folder + "/"
    rel = lambda n: names[n][len(C + adir):-7] if n else ""  # noqa: E731
    boss = {"id": bid, "name": {"r2text": name} if name else "none", "title": text or bid.replace("_", " ").title(),
            "tier": "boss", "mesh": m["asset"], "skeleton": skel_asset, "forward": fwd, "textures": tex or ["none"],
            "anim_dir": C + adir, "anim_idle": rel(roles["idle"]), "anim_walk": rel(roles["walk"]),
            "anim_run": rel(roles["run"]), "anim_turn_l": rel(roles["turn_l"]), "anim_turn_r": rel(roles["turn_r"]),
            "anim_roar": rel(roles["roar"]), "anim_flinch": rel(roles["flinch"]), "anim_stun": rel(roles["stun"]),
            "anim_death": rel(roles["death"]),
            "scale": row.get("scale", round(min(1.0, 1800.0 / max(length, height, 1)), 2)), "walk_mps": round(walk, 2), "run_mps": round(run, 2),
            "turn_dps": 80 if length > 1000 else 120, **snd}
    for a in out:
        a["anim"] = rel(a["anim"])
    log.append(f"name {text!r}, facing {fwd}, {length / 100:.1f} m long, {height / 100:.1f} m tall, walk {walk:.1f} "
               f"run {run:.1f} m/s, {len(out)} attacks ({', '.join(a['kind'][0] for a in out)}), {len(tex)}/"
               f"{len(m['materials'])} textures, roles " + " ".join(k for k in ('walk', 'run', 'turn_l', 'roar', 'flinch',
                                                                                'stun', 'death') if roles[k]))
    return {"boss": boss, "attacks": out}, log


# ---------------------------------------------------------------- run

roster = json.load(open(os.path.join(HERE, "boss_roster.json"), encoding="utf-8"))["bosses"]
for row in roster:
    if row.get("keep") or (ONLY and row["id"] != ONLY):
        continue
    cp = os.path.join(CACHE, row["id"] + ".json")
    if os.path.exists(cp) and not ONLY:
        continue
    if time.time() - T0 > BUDGET:
        print("budget reached")
        break
    t = time.time()
    try:
        res, log = author(row)
    except Exception as e:  # keep going with the others
        res, log = None, [f"failed: {type(e).__name__}: {e}"]
    json.dump({"result": res, "log": log}, open(cp, "w"), indent=1)
    print(f"{row['id']}: {'OK' if res else 'SKIPPED'} ({time.time() - t:.0f} s)")
    for line in log:
        print("   ", line)
R.p.kill()

# ---------------------------------------------------------------- --write: the sheets

ZONES = {"yaesha": ["101", "109"], "losomn": ["104", "108", "109"], "nerud": ["102", "105", "109"],
         "root": ["104", "105", "109"], "labyrinth": ["105", "109"]}
WORLD = {"yaesha": "Yaesha", "losomn": "Losomn", "nerud": "N'Erud", "root": "Root Earth", "labyrinth": "Labyrinth"}
FX = {"yaesha": "#8CF06A", "losomn": "#B072FF", "nerud": "#4FD8FF", "root": "#FF4A2A", "labyrinth": "#FFD27A"}
SHEETS = os.path.join(ROOT, "sheets")


def loot_for(row, rowmap, toolkit):
    hits = []
    want = set(row.get("loot_dungeons", []))
    for cat, items in toolkit.items():
        for it in items:
            loc = it.get("location") or {}
            dn = loc.get("dungeon")
            dns = set(dn if isinstance(dn, list) else [dn])
            slug = re.sub(r"^[A-Za-z]+_|_C$", "", it.get("saveFileSlug") or "")
            key = rowmap.get(norm(it["name"])) or rowmap.get(norm(slug))
            if not key:
                continue
            if want & dns:
                hits.append((0, key))
            elif not want and loc.get("world") == WORLD[row["world"]]:
                hits.append((1 if dns & {"World Drop", "Aberration"} else 2, key))
    import hashlib
    hits = sorted(set(hits), key=lambda h: (h[0], hashlib.md5((row["id"] + h[1]).encode()).hexdigest()))
    return [h[1] for h in hits[:6]] or ["none"]


if "--write" in sys.argv:
    toolkit = json.load(open(os.path.join(ROOT, ".check", "toolkit.json"), encoding="utf-8"))
    rowmap = {}
    for sh in ("weapons", "mods", "mutators", "armor", "rings", "amulets", "relics", "fragments", "consumables"):
        for r in json.load(open(os.path.join(SHEETS, sh + ".json"), encoding="utf-8"))["rows"]:
            rowmap.setdefault(norm(r["id"]), f"{sh}.{r['id']}")
    old = json.load(open(os.path.join(SHEETS, "bosses.json"), encoding="utf-8"))
    old_atk = json.load(open(os.path.join(SHEETS, "boss_attacks.json"), encoding="utf-8"))
    cols = {"id": "id", "name": "r2text|none", "title": "str", "tier": "enum:boss|elite|creature", "mesh": "str",
            "skeleton": "str", "forward": "enum:py|ny|px|nx", "textures": "list", "anim_dir": "str", "anim_idle": "str",
            "anim_walk": "str", "anim_run": "str", "anim_turn_l": "str", "anim_turn_r": "str", "anim_roar": "str",
            "anim_flinch": "str", "anim_stun": "str", "anim_death": "str", "scale": "num", "threat": "num",
            "min_rank": "enum:lr|hr|mr", "hp_mult": "num", "walk_mps": "num", "run_mps": "num", "turn_dps": "num",
            "aggro_m": "num", "leash_m": "num", "flinch_frac": "num", "stagger_frac": "num", "melee_hits": "num",
            "snd_roar": "r2snd|none", "snd_pain": "r2snd|none", "snd_death": "r2snd|none", "snd_step": "r2snd|none",
            "zones": "list", "spawn_weight": "num", "loot": "list", "fx_color": "str"}
    acols = {"id": "id", "boss": "ref:bosses", "anim": "str", "kind": "enum:melee|aoe|ranged", "min_m": "num",
             "max_m": "num", "hits": "list", "reach_m": "num", "arc_deg": "num", "damage": "num", "weight": "num",
             "cooldown_s": "num", "track": "num", "snd": "r2snd|none",
             "inflict": "enum:none|bleeding|burning|corroded|overloaded"}

    def inflict_of(world, a):
        n = a["anim"].lower()
        for st, pat in (("burning", r"fire|flame|burn|lava|cinder|ember|inferno|blaze|magma|heat|nova|sun"),
                        ("overloaded", r"laser|beam|shock|lightning|electr|spark|volt|arc|plasma|energy|comet|star"),
                        ("corroded", r"acid|spit|poison|rot|corrod|toxic|bile|vomit|blight|spore|ooze"),
                        ("bleeding", r"bite|claw|swipe|slash|blade|spear|impale|stab|cleave|saw|thorn|spike|whip|pounce")):
            if re.search(pat, n):
                return st
        if a["kind"] != "melee":
            return {"root": "corroded", "nerud": "overloaded", "losomn": "burning", "yaesha": "bleeding",
                    "labyrinth": "overloaded"}[world]
        return "none"
    rows, arows, seen = [], [], set()
    for r in roster:
        t = r["threat"]
        common = {"threat": t, "min_rank": "lr" if t <= 2 else "hr" if t == 3 else "mr",
                  "hp_mult": round(0.7 + 0.15 * t, 2), "aggro_m": 40, "leash_m": 160, "flinch_frac": 0.07,
                  "stagger_frac": 0.12, "melee_hits": 140, "zones": ZONES[r["world"]], "spawn_weight": 1.0,
                  "fx_color": FX[r["world"]]}
        if r.get("keep"):
            b = next(x for x in old["rows"] if x["id"] == r["id"])
            nb = {k: b.get(k) for k in cols if k in b}
            nb.update({"title": "Corrupted Ravager", "forward": "py", **common, "loot": b["loot"]})
            for k in ("anim_walk", "anim_run", "anim_turn_l", "anim_turn_r", "anim_roar", "anim_flinch", "anim_stun",
                      "anim_death"):
                nb[k] = nb.get(k) or "none"
            rows.append({k: nb[k] for k in cols})
            for a in old_atk["rows"]:
                if a["boss"] == r["id"]:
                    a = dict(a)
                    a["kind"] = a.get("kind") or ("aoe" if a["arc_deg"] >= 359 else "melee")
                    a["inflict"] = inflict_of(r["world"], a)
                    arows.append({k: a[k] for k in acols})
            continue
        cp = os.path.join(CACHE, r["id"] + ".json")
        res = json.load(open(cp))["result"] if os.path.exists(cp) else None
        if not res:
            continue
        b = dict(res["boss"])
        for k in ("anim_walk", "anim_run", "anim_turn_l", "anim_turn_r", "anim_roar", "anim_flinch", "anim_stun",
                  "anim_death"):
            b[k] = b[k] or "none"
        b.update(common)
        b["loot"] = loot_for(r, rowmap, toolkit)
        rows.append({k: b[k] for k in cols})
        for a in res["attacks"]:
            a = dict(a)
            while a["id"] in seen:
                a["id"] += "_b"
            seen.add(a["id"])
            a["inflict"] = inflict_of(r["world"], a)
            arows.append({k: a[k] for k in acols})
    about = ("Remnant II's bosses roaming MHW's field zones as creatures of their own, written by tools/author_bosses.py "
             "from the player's install (asset paths and text keys only). tier: boss = Remnant II's large bar at the top "
             "of the screen; elite / creature = a bar over it. forward: the axis the model faces (py = +y, nx = -x ...). threat 1-5: strength, "
             "and min_rank, the lowest MHW rank whose zones it roams (lr / hr / mr). hp_mult: times the rank's boss HP "
             "(tuning). flinch_frac / stagger_frac: share of max HP that makes it flinch / stagger (stagger grows each "
             "time). melee_hits: MHW melee hits to kill it at an average weapon. zones: MHW stage ids. fx_color: its "
             "projectiles. Animation names are relative to anim_dir; none = it has no such animation.")
    json.dump({"sheet": "bosses", "phase": "v0.2", "about": about, "columns": cols, "rows": rows},
              open(os.path.join(SHEETS, "bosses.json"), "w", encoding="utf-8"), indent=1, ensure_ascii=False)
    json.dump({"sheet": "boss_attacks", "phase": "v0.2", "about": old_atk.get("about", ""), "columns": acols,
               "rows": arows}, open(os.path.join(SHEETS, "boss_attacks.json"), "w", encoding="utf-8"), indent=1,
              ensure_ascii=False)
    print(f"wrote {len(rows)} bosses, {len(arows)} attacks")
