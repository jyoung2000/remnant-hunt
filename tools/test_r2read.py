#!/usr/bin/env python3
"""Cross-check the r2read helper against the Python dev tools for every Remnant II cell in the sheets.
Prints only pass/fail and sizes, never game text.
usage: test_r2read.py <r2read binary> <Remnant2 install dir> [sheets dir]
"""
import json, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r2pak, r2text, r2tex


class Helper:
    def __init__(self, exe, game):
        self.p = subprocess.Popen([exe, game], stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def ask(self, *fields):
        self.p.stdin.write(("\t".join(fields) + "\n").encode()); self.p.stdin.flush()
        head = self.p.stdout.readline().decode().strip()
        if not head.startswith("OK") or head.startswith("OK r2read"):
            return head, b""
        n = int(head.split()[-1])
        return head, self.p.stdout.read(n)


def main():
    exe, game = sys.argv[1], sys.argv[2]
    d = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sheets")
    paks = os.path.join(game, "Remnant2", "Content", "Paks")
    idx = {}
    for s in r2pak.open_all(paks):
        for k, v in s.files.items():
            idx[k] = (s, v)
    h = Helper(exe, game)
    head, _ = h.ask("HELLO")
    print(head)
    ok = bad = 0
    tcache = {}

    def texts_of(asset):
        if asset not in tcache:
            s, e = idx[asset]
            tcache[asset] = dict(r2text.texts(s.read(e)))
        return tcache[asset]

    for n in sorted(os.listdir(d)):
        if not n.endswith(".json") or n.startswith("_"):
            continue
        sh = json.load(open(os.path.join(d, n), encoding="utf-8"))
        for r in sh["rows"]:
            for col, v in r.items():
                if not isinstance(v, dict):
                    continue
                kind, spec = next(iter(v.items()))
                where = f"{sh['sheet']}.{r['id']}.{col}"
                if kind == "r2text":
                    want = texts_of(spec["asset"])[spec["key"]].encode()
                    head, got = h.ask("TEXT", spec["asset"], spec["key"])
                    good = got == want
                elif kind == "r2tex":
                    head, got = h.ask("TEX", spec, "256")
                    parts = head.split()
                    good = parts[0] == "OK" and len(got) == int(parts[-1]) and int(parts[2]) > 0
                elif kind == "r2name":
                    head, got = h.ask("NAME", spec["asset"], spec.get("pattern", "^(" + spec.get("name", "") + ")$"))
                    good = head.startswith("OK") and len(got) > 0
                else:
                    continue
                if good:
                    ok += 1
                else:
                    bad += 1
                    print("FAIL", where, head)
    h.ask("QUIT")
    print(f"r2read matches the dev tools on {ok} cells, {bad} failed")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
