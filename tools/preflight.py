#!/usr/bin/env python3
"""Preflight: lay every sheet's rows and columns over each other and report what is not done.

  unfilled   a cell that is missing or empty
  broken     a reference that does not resolve (ref:<sheet>, list:ref:<sheet>, '<sheet>.<column>' mentions)
  invalid    a value that does not fit its column type (num, enum)
  unverified a cell or row whose evidence is not recorded in sheets/_verified.json yet
             (Remnant II cells: tools/verify_r2.py; hooks and systems: the in-game log via tools/ingest_log.py)

usage: preflight.py [sheets dir] [--phase v0.1] [--strict]
Exit 1 when anything is unfilled, broken or invalid (the build must not start). --strict also fails on unverified.
"""
import json, os, sys

R2_TYPES = ("r2text", "r2tex", "r2name")


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
    phase = sys.argv[sys.argv.index("--phase") + 1] if "--phase" in sys.argv else "v0.1"
    sheets, ver = load(d)
    ids = {n: {r["id"] for r in s["rows"]} for n, s in sheets.items()}
    unfilled, broken, invalid, unverified = [], [], [], []
    total = 0
    for name, sh in sheets.items():
        if sh.get("phase", phase) != phase:
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
                if typ == "num":
                    if not isinstance(v, (int, float)):
                        invalid.append(f"{where}: not a number ({v!r})")
                elif typ.startswith("enum:"):
                    if v not in typ[5:].split("|"):
                        invalid.append(f"{where}: {v!r} not in {typ[5:]}")
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
