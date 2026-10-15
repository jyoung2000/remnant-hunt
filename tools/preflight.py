#!/usr/bin/env python3
"""Preflight: lay every sheet's rows and columns over each other and report what is not done.

  unfilled   a cell that is missing or empty
  broken     a reference that does not resolve (ref:<sheet>, list:ref:<sheet>, '<sheet>.<column>' mentions)
  invalid    a value that does not fit its column type (num, enum)
  unverified a cell or row whose evidence is not recorded in sheets/_verified.json yet
             (Remnant II cells: tools/verify_r2.py; hooks and systems: the in-game log via tools/ingest_log.py)

usage: preflight.py [sheets dir] [--phase build|v0.1|v0.2|...] [--strict]   (build = every phase the build uses)
Exit 1 when anything is unfilled, broken or invalid (the build must not start). --strict also fails on unverified.
"""
import json, os, re, sys

R2_TYPES = ("r2text", "r2tex", "r2name")
BUILD_PHASES = ("v0.1", "v0.2")  # sheets the build generates code from; later phases are plans

# The effect language (tools/author_arsenal.py, src/core/effects.cpp).
_STAT = (r"(?:proc|dmg|dmg_ranged|dmg_melee|dmg_mod|dmg_skill|dmg_elem|dmg_status|dmg_explosive|weakspot|crit|crit_dmg|"
         r"fire_rate|reload|mag|ammo|accuracy|mod_gen|mod_dur|cdr|skill_dur|relic_speed|relic_heal|relic_charges|heal|"
         r"lifesteal|dr|max_hp|regen|stamina_max|stamina_regen|stamina_cost|shield|res_bleed|res_fire|res_shock|res_acid|vs_bleeding|vs_burning|"
         r"vs_corroded|vs_overloaded|vs_exposed|vs_status|status_dur|xp)")
_STATUS = r"(?:bleeding|burning|corroded|overloaded|exposed|slowed|marked)"
_N = r"\d+(?:\.\d+)?"
_TRIG = r"(?:on_kill|on_crit|on_weakspot|on_hit|on_evade|on_relic|on_skill|on_mod|on_reload|on_hurt|low_hp\(\d+\)|high_hp\(\d+\))"
EFFECT_RX = re.compile(
    rf"^(?:(?:{_TRIG}:)?{_STAT}[+-]{_N}%?(?: {_N}s)?(?: x\d+)?(?: cd{_N}s)?"
    rf"|use:(?:heal {_N}%(?: {_N}s)?|shield {_N}% {_N}s|burst {_N}|dot {_N} {_N}s|summon {_N} {_N}s"
    rf"|buff {_STAT}[+-]{_N}%? {_N}s|ammo {_N}%|grenade {_N}"
    rf"|inflict {_STATUS} {_N} {_N}s {_N}m(?:@target)?|imbue {_STATUS} {_N} {_N}s {_N}s|aoe {_N} {_N}m(?:@target)?"
    rf"|mark {_N}s {_N}m|eruption {_N} {_N}m|cure {_STATUS})"
    rf"|(?:{_TRIG}:)?inflict:{_STATUS} {_N} {_N}s(?: {_N}%)?(?: cd{_N}s)?)$")


def effects_ok(v):
    return all(EFFECT_RX.match(e) for e in v.split(";"))


def load(d):
    sheets = {}
    for n in sorted(os.listdir(d)):
        if n.endswith(".json") and not n.startswith("_"):
            sh = json.load(open(os.path.join(d, n), encoding="utf-8"))
            sheets[sh["sheet"]] = sh
    ver = {}
    vp = os.path.join(d, "_verified.json")
    if os.path.exists(vp):
        ver = json.load(open(vp, encoding="utf-8"))
    return sheets, ver


def empty(v):
    return v is None or v == "" or v == [] or v == {}


def main():
    argv = sys.argv[1:]
    args = [a for i, a in enumerate(argv) if not a.startswith("--") and (i == 0 or argv[i - 1] != "--phase")]
    d = args[0] if args else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sheets")
    phase = sys.argv[sys.argv.index("--phase") + 1] if "--phase" in sys.argv else "build"
    phases = BUILD_PHASES if phase == "build" else (phase,)
    sheets, ver = load(d)
    ids = {n: {r["id"] for r in s["rows"]} for n, s in sheets.items()}
    unfilled, broken, invalid, unverified = [], [], [], []
    total = 0
    for name, sh in sheets.items():
        if sh.get("phase", "v0.1") not in phases:
            continue
        cols = sh["columns"]
        for r in sh["rows"]:
            rid = r.get("id", "?")
            for col, typ in cols.items():
                total += 1
                where = f"{name}.{rid}.{col}"
                v = r.get(col)
                if empty(v):
                    unfilled.append(where); continue
                kinds = typ.split("|")
                if v == "none" and "none" in kinds:
                    continue
                if typ.startswith("r2snd"):
                    if not (isinstance(v, str) and v.startswith("Remnant2/Content/WwiseAudio/Events/") and v.endswith(".uasset")):
                        invalid.append(f"{where}: not a Remnant II sound event ({v!r})")
                    continue
                if typ.startswith("effects"):
                    if not isinstance(v, str) or not effects_ok(v):
                        invalid.append(f"{where}: not in the effect language ({v!r})")
                    continue
                if typ == "recipe":
                    if v != ["none"]:
                        for x in v:
                            mat, _, n = x.partition("*")
                            if mat not in ids.get("materials", set()) or not n.isdigit():
                                broken.append(f"{where}: {x!r} is not <materials id>*<count>")
                    continue
                if "num" in kinds:
                    if not isinstance(v, (int, float)):
                        invalid.append(f"{where}: not a number ({v!r})")
                elif typ.startswith("enum:"):
                    if v not in typ[5:].split("|"):
                        invalid.append(f"{where}: {v!r} not in {typ[5:]}")
                elif typ.startswith("refasset:"):
                    target = kinds[0][len("refasset:"):]
                    allowed = kinds[1:]
                    if v not in allowed and v not in ids.get(target, set()):
                        broken.append(f"{where}: {v!r} is not a row of {target}")
                elif typ.startswith("ref:"):
                    target = kinds[0][4:]
                    allowed = kinds[1:]
                    if v not in allowed and v not in ids.get(target, set()):
                        broken.append(f"{where}: {v!r} is not a row of {target}")
                elif typ.startswith("list:ref:"):
                    target = typ[9:]
                    for x in v:
                        if x != "none" and x not in ids.get(target, set()):
                            broken.append(f"{where}: {x!r} is not a row of {target}")
                elif typ == "list:sheet":
                    for x in v:
                        if x not in sheets:
                            broken.append(f"{where}: {x!r} is not a sheet")
                elif typ == "list":
                    for x in v:
                        head = x.split(".")[0]
                        if "." in x and head in sheets and x.split(".")[1] not in sheets[head]["columns"] \
                                and x.split(".")[1] not in ids[head]:
                            broken.append(f"{where}: {x!r} names no column of {head}")
                if isinstance(v, dict):
                    k = next(iter(v))
                    if k in R2_TYPES and ver.get(where, "").split(":")[0] != "ok":
                        unverified.append(f"{where}: Remnant II {k} not verified")
            if name in ("hooks", "systems") and ver.get(f"{name}.{rid}", "").split(":")[0] != "ok":
                unverified.append(f"{name}.{rid}: needs in-game evidence")
    print(f"preflight {phase}: {total} cells in {len(sheets)} sheets")
    for title, items in (("UNFILLED", unfilled), ("BROKEN", broken), ("INVALID", invalid), ("UNVERIFIED", unverified)):
        print(f"{title}: {len(items)}")
        for x in items[:200]:
            print("  -", x)
    bad = unfilled or broken or invalid or ("--strict" in sys.argv and unverified)
    print("RESULT:", "FAIL" if bad else "CLEAN")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
