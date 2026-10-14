#!/usr/bin/env python3
"""Dev tool: fill empty / "none" animation and sound fields of sheets/bosses.json from the player's Remnant II install.

Reads the install in place (asset paths only); only fills a field when one candidate is plausible or the best one is
unambiguous, never overwrites a non-empty value, edits the JSON as text so the diff stays minimal.
usage: author_boss_gaps.py <Remnant II dir> [--write]
"""
import json, os, re, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import r2pak  # noqa: E402

R2DIR = sys.argv[1]
WRITE = "--write" in sys.argv
SHEET = os.path.join(HERE, "..", "sheets", "bosses.json")
FILES = set()
for s in r2pak.open_all(os.path.join(R2DIR, "Remnant2", "Content", "Paks")):
    FILES.update(s.files)
UA = [f for f in FILES if f.endswith(".uasset")]
ANIM_ROOTS = sorted(UA)

# (field, [(keyword group, ...)] tiers in priority order, each tier = list of substrings that must ALL appear)
ANIM = {
    "anim_run": [["run", "_f"], ["sprint", "_f"], ["run", "fwd"], ["run"], ["sprint"], ["charge", "_f"]],
    "anim_turn_l": [["turn", "90", "l"], ["turn", "135", "l"], ["turn", "180", "l"], ["turn", "-", "l"]],
    "anim_turn_r": [["turn", "90", "r"], ["turn", "135", "r"], ["turn", "180", "r"], ["turn", "-", "r"]],
    "anim_roar": [["roar"], ["taunt"], ["scream"], ["howl"], ["intro"], ["aggro"]],
    "anim_stun": [["stun"], ["stagger"], ["dazed"], ["groggy"], ["knockdown"]],
    "anim_walk": [["walk", "_f"], ["walk", "fwd"], ["walk"]],
    "anim_flinch": [["flinch"], ["impact", "hit"], ["hit"], ["damage"]],
}
SND = {
    "snd_roar": [["roar"], ["howl"], ["scream"], ["taunt"]],
    "snd_death": [["death"], ["die"]],
    "snd_step": [["step"], ["foot"]],
}
BAD = ("additive", "_add", "mirror", "pose", "lod", "ref", "_nc_", "_p_")
# not locomotion / reaction clips: attacks, transitions, VO, niagara, blendspaces, aim offsets
NOT_ANIM = re.compile(r"(^|[/_])(atk|attack|start|end|enter|exit|stop|slash|vo|ns|action|fx|bs|ao|aoe|cc|cd|cu|ref)(_|$|\d)", re.I)


def split_tokens(name):
    return name.lower()


def match_tier(name, tier):
    n = name.lower()
    if len(tier) == 3 and tier[0] == "turn":
        side = re.search(r"turn(left|right)?(_?[lr]|\d)", n)  # sanity: has a turn token
        other = {"l": "r", "r": "l"}[tier[2]]
        has_side = re.search(r"(^|[^a-z])turn_?" + tier[2] + r"([^a-z]|\d|$)|\d_" + tier[2] + r"([^a-z]|$)|turn" + tier[2] + r"\d", n)
        deg = tier[1] == "-" or re.search(r"(?<!\d)" + tier[1] + r"(?!\d)", n)
        return bool(side and has_side and deg and "turn" in n)
    if len(tier) == 2 and tier[0] in ("run", "walk", "sprint", "charge") and tier[1] == "_f":
        return tier[0] in n and re.search(r"_f(wd)?([^a-z]|$)", n) is not None
    return all(t in n for t in tier)


def pick(cands, tiers):
    """cands: list of basenames. returns (choice or None, note)."""
    cands = [c for c in cands if not any(b in c.lower() for b in BAD)] or cands
    for tier in tiers:
        hit = sorted({c for c in cands if match_tier(c, tier)})
        if not hit:
            continue
        if len(hit) == 1:
            return hit[0], ""
        # tie-break: shortest name, if unique and others are plain variants of it
        hit.sort(key=lambda c: (len(c), c))
        if len(hit[0]) < len(hit[1]) and all(h.lower().startswith(hit[0].lower()) for h in hit[1:]):
            return hit[0], "(of %d)" % len(hit)
        return None, "AMBIGUOUS " + ", ".join(hit[:6])
    return None, ""


def creature(row):
    m = re.search(r"/Characters/([^/]+)/", row["mesh"])
    return m.group(1) if m else ""


def snd_dirs(row):
    dirs = []
    for k in ("snd_roar", "snd_pain", "snd_death", "snd_step"):
        v = row.get(k)
        if v and v != "none":
            dirs.append(v.rsplit("/", 1)[0] + "/")
    if dirs:
        return sorted(set(dirs))
    cn = creature(row).lower()
    if not cn:
        return []
    out = set()
    pre = "Remnant2/Content/WwiseAudio/Events/"
    for f in UA:
        if f.startswith(pre):
            d = f.rsplit("/", 1)[0]
            if cn in d.rsplit("/", 1)[1].lower():
                out.add(d + "/")
    return sorted(out)


def main():
    text = open(SHEET, encoding="utf-8").read()
    rows = json.loads(text)["rows"]
    filled, missing = 0, []
    # text segments per row
    starts = [(m.start(), m.group(1)) for m in re.finditer(r'^   "id": "([^"]+)"', text, re.M)]
    edits = []
    for idx, row in enumerate(rows):
        gaps = [c for c in list(ANIM) + list(SND) if not row.get(c) or row.get(c) == "none"]
        if not gaps:
            continue
        adir = row["anim_dir"]
        anims = {}
        for f in UA:
            if f.startswith(adir):
                rel = f[len(adir):-7]
                anims[rel] = rel.rsplit("/", 1)[-1]
        sdirs = snd_dirs(row)
        sfiles = [f for f in UA for d in sdirs if f.startswith(d)] if sdirs else []
        for c in gaps:
            if c in ANIM:
                bn = {}
                for rel, b in anims.items():
                    if re.search(r"(Blendspace|AimOffset|Lookpose)", rel, re.I) or NOT_ANIM.search(b):
                        continue
                    bn.setdefault(b, rel)
                ch, note = pick(list(bn), ANIM[c])
                val = bn.get(ch)
            else:
                bn = {}
                for f in sfiles:
                    bn.setdefault(f.rsplit("/", 1)[1][:-7], f)
                ch, note = pick(list(bn), SND[c])
                val = bn.get(ch)
            if val:
                print("FILL  %-18s %-12s %s %s" % (row["id"], c, val.rsplit("/", 1)[-1], note))
                edits.append((row["id"], c, val))
                filled += 1
            else:
                print("EMPTY %-18s %-12s %s" % (row["id"], c, note or "no candidate"))
                missing.append((row["id"], c))
    print("filled", filled, "remaining", len(missing))
    if WRITE:
        for i, (rid, c, val) in enumerate(edits):
            s = next(p for p, n in starts if n == rid)
            nxt = [p for p, n in starts if p > s]
            e = nxt[0] if nxt else len(text)
            seg = text[s:e]
            seg2, n = re.subn(r'("%s": )"(none)?"' % c, lambda m: m.group(1) + json.dumps(val), seg, count=1)
            assert n == 1, (rid, c)
            text = text[:s] + seg2 + text[e:]
            starts = [(p + (len(seg2) - len(seg)) if p > s else p, nm) for p, nm in starts]
        open(SHEET, "w", encoding="utf-8", newline="").write(text)
        json.loads(text)


main()
