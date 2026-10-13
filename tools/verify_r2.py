#!/usr/bin/env python3
"""Check every Remnant II cell in the sheets against the player's own install and record the evidence in
sheets/_verified.json. Evidence never contains game text: only lengths, formats and sizes.
usage: verify_r2.py <Remnant2 Paks dir> [sheets dir]
"""
import json, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r2pak, r2text, r2tex


def main():
    paks = sys.argv[1]
    d = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sheets")
    idx = {}
    for s in r2pak.open_all(paks):
        for k, v in s.files.items():
            idx[k] = (s, v)
    cache = {}

    def pkg(asset):
        if asset not in cache:
            s, e = idx[asset]
            cache[asset] = s.read(e)
        return cache[asset]

    vp = os.path.join(d, "_verified.json")
    ver = json.load(open(vp, encoding="utf-8")) if os.path.exists(vp) else {}
    ok = bad = 0
    for n in sorted(os.listdir(d)):
        if not n.endswith(".json") or n.startswith("_"):
            continue
        sh = json.load(open(os.path.join(d, n), encoding="utf-8"))
        for r in sh["rows"]:
            for col, v in r.items():
                if not isinstance(v, dict):
                    continue
                where = f"{sh['sheet']}.{r['id']}.{col}"
                kind, spec = next(iter(v.items()))
                try:
                    if kind == "r2text":
                        found = [t for k, t in r2text.texts(pkg(spec["asset"])) if k == spec["key"]]
                        assert found, "key not in package"
                        ev = f"ok:text {len(found[0])} chars"
                    elif kind == "r2tex":
                        info = r2tex.parse(pkg(spec + ".uasset"))
                        m = info["mips"][0]
                        if not m["inline"]:
                            assert spec + ".ubulk" in idx, "missing .ubulk"
                        ev = f"ok:{info['format']} {info['w']}x{info['h']} {len(info['mips'])} mips"
                    elif kind == "r2name":
                        names = r2text.names(pkg(spec["asset"]))
                        if "pattern" in spec:
                            hit = [m.group(1) for m in (re.match(spec["pattern"], x) for x in names) if m]
                        else:
                            hit = [x for x in names if x == spec["name"]]
                        assert hit, "name not in package"
                        ev = f"ok:name {len(hit[0])} chars"
                    else:
                        continue
                    ok += 1
                except (KeyError, AssertionError, ValueError) as e:
                    ev = f"fail:{type(e).__name__} {e}"
                    bad += 1
                ver[where] = ev
                if ev.startswith("fail"):
                    print("FAIL", where, ev)
    json.dump(ver, open(vp, "w", encoding="utf-8"), indent=1, sort_keys=True)
    print(f"verified {ok} Remnant II cells, {bad} failed")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
