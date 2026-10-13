#!/usr/bin/env python3
"""Dev tool: show the localizable text (FText key + English source) and name map of Remnant II packages.

Reads in place through r2pak.py; prints only what the design sheets need to point at.
usage: r2text.py <Paks dir> <asset path> [--names]
"""
import re, struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r2pak

HEXKEY = re.compile(r"^[0-9A-F]{32}$")


def fstrings(data):
    """Yield (offset, text) for every plausible serialized FString (ANSI or UTF-16)."""
    i, n = 0, len(data)
    while i + 4 < n:
        ln = struct.unpack_from("<i", data, i)[0]
        if 2 <= ln <= 4096 and i + 4 + ln <= n and data[i + 4 + ln - 1] == 0:
            raw = data[i + 4:i + 4 + ln - 1]
            if raw and all(32 <= c < 127 or c in (9, 10, 13) for c in raw):
                yield i, raw.decode("ascii")
                i += 4 + ln
                continue
        if -4096 <= ln <= -2 and i + 4 - ln * 2 <= n:
            raw = data[i + 4:i + 4 + (-ln) * 2]
            if raw[-2:] == b"\0\0":
                try:
                    s = raw[:-2].decode("utf-16-le")
                    if s and all(ch.isprintable() or ch in "\r\n\t" for ch in s):
                        yield i, s
                        i += 4 - ln * 2
                        continue
                except UnicodeDecodeError:
                    pass
        i += 1


def texts(data):
    """FText pattern: [flags u32][history i8 = 0][namespace][key][source] -> (key, source)."""
    out, strs = [], list(fstrings(data))
    for a, b in zip(strs, strs[1:]):
        if HEXKEY.match(a[1]) and not HEXKEY.match(b[1]):
            out.append((a[1], b[1]))
    return out


def names(data):
    return names_and_end(data)[0]


def names_and_end(data):
    # Zen package summary: name map follows the fixed header; find it by its count/size pair.
    hdr = struct.unpack_from("<I", data, 4)[0]
    for off in range(16, min(hdr, 0x200), 4):
        cnt, nbytes = struct.unpack_from("<II", data, off)
        if 0 < cnt < 100000 and 0 < nbytes < 10 ** 7 and off + 16 + cnt * 10 + nbytes <= len(data):
            hv = struct.unpack_from("<Q", data, off + 8)[0]
            if hv == 0xC1640000:  # FNameHash algorithm id used by UE
                p = off + 16 + cnt * 8
                lens = [struct.unpack_from(">H", data, p + i * 2)[0] for i in range(cnt)]
                p += cnt * 2
                res = []
                for L in lens:
                    if L & 0x8000:
                        L &= 0x7FFF
                        p += p & 1
                        res.append(data[p:p + L * 2].decode("utf-16-le")); p += L * 2
                    else:
                        res.append(data[p:p + L].decode("latin1")); p += L
                return res, p
    return [], None


def bulk_map(data):
    """UE 5.2+ zen packages keep bulk-data headers in a table right after the name map."""
    _, p = names_and_end(data)
    size = struct.unpack_from("<Q", data, p)[0]
    p += 8
    out = []
    for i in range(size // 32):
        so, dso, ss, fl = struct.unpack_from("<qqqI", data, p + i * 32)
        out.append({"offset": so, "size": ss, "flags": fl})
    return out


def main():
    paks, want = sys.argv[1], sys.argv[2]
    for s in r2pak.open_all(paks):
        if want in s.files:
            data = s.read(s.files[want])
            for k, v in texts(data):
                print(f"TEXT {k} = {v!r}")
            if "--names" in sys.argv:
                print("NAMES", names(data))
            return
    print("not found", want)


if __name__ == "__main__":
    main()
