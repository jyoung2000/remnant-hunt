#!/usr/bin/env python3
"""Dev tool: read the Remnant 2 Toolkit item constants (GPL-3.0, github.com/joshpayette/remnant2-toolkit) as data,
without running them: a small parser for the object-literal subset those files use (strings, numbers, arrays,
objects, + concatenation, comments). Used only to author effect numbers and gun stats into our own sheets.
usage: r2toolkit_read.py <toolkit>/src/app/(items)/_constants .check/toolkit.json
"""
import json, os, re, sys


class P:
    def __init__(self, s):
        self.s, self.i = s, 0

    def ws(self):
        s = self.s
        while self.i < len(s):
            if s[self.i].isspace():
                self.i += 1
            elif s.startswith("//", self.i):
                self.i = s.find("\n", self.i) + 1 or len(s)
            elif s.startswith("/*", self.i):
                self.i = s.find("*/", self.i) + 2
            else:
                break

    def peek(self):
        self.ws()
        return self.s[self.i] if self.i < len(self.s) else ""

    def string(self):
        q = self.s[self.i]
        self.i += 1
        out = []
        while self.s[self.i] != q:
            c = self.s[self.i]
            if c == "\\":
                n = self.s[self.i + 1]
                out.append({"n": "\n", "t": "\t"}.get(n, n))
                self.i += 2
            else:
                out.append(c)
                self.i += 1
        self.i += 1
        return "".join(out)

    def value(self):
        c = self.peek()
        if c in "'\"`":
            v = self.string()
            while self.peek() == "+":
                self.i += 1
                v += self.value()
            return v
        if c == "[":
            self.i += 1
            arr = []
            while self.peek() != "]":
                arr.append(self.value())
                if self.peek() == ",":
                    self.i += 1
            self.i += 1
            return arr
        if c == "{":
            self.i += 1
            obj = {}
            while self.peek() != "}":
                if self.peek() in "'\"":
                    k = self.string()
                else:
                    m = re.compile(r"[A-Za-z_$][\w$]*").match(self.s, self.i)
                    k = m.group(0)
                    self.i = m.end()
                if self.peek() == ":":
                    self.i += 1
                    obj[k] = self.value()
                else:
                    obj[k] = None  # shorthand
                if self.peek() == ",":
                    self.i += 1
            self.i += 1
            return obj
        m = re.compile(r"-?\d+(\.\d+)?").match(self.s, self.i)
        if m:
            self.i = m.end()
            return float(m.group(0)) if "." in m.group(0) else int(m.group(0))
        m = re.compile(r"[A-Za-z_$][\w$.]*").match(self.s, self.i)
        if m:
            self.i = m.end()
            return {"true": True, "false": False, "null": None, "undefined": None}.get(m.group(0), "$" + m.group(0))
        raise ValueError(f"unexpected {self.s[self.i:self.i + 40]!r}")


def main():
    d, out = sys.argv[1], sys.argv[2]
    res = {}
    for f in sorted(os.listdir(d)):
        if not f.endswith("-items.ts") or f == "all-items.ts":
            continue
        s = open(os.path.join(d, f), encoding="utf-8").read()
        m = re.search(r"export const \w+\s*:\s*[\w\[\]<>]+\s*=\s*\[", s)
        if not m:
            print("skip", f)
            continue
        p = P(s)
        p.i = m.end() - 1
        res[f[:-9]] = p.value()
    json.dump(res, open(out, "w"), indent=1)
    print({k: len(v) for k, v in res.items()})


if __name__ == "__main__":
    main()
