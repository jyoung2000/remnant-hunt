#!/usr/bin/env python3
"""Dev tool: read Remnant II's own packages in place (UE5 IoStore .utoc/.ucas and the legacy .pak).

Nothing is copied out of the game folder: files are listed and decoded in memory, and only
the small facts the design sheets need (paths, string keys, sizes) are printed.

usage:
  r2pak.py ls   <Paks dir> [substring ...]     list IoStore + pak paths containing all substrings
  r2pak.py cat  <Paks dir> <exact path> [out]  decode one file (out only for local inspection; never commit)
  r2pak.py info <Paks dir>                     container summary

Oodle blocks are decoded with the GPL-3.0 ooz library (OOZ_LIB=path to libooz.so / ooz.dll).
"""
import ctypes, os, struct, sys

_ooz = None


def ooz():
    global _ooz
    if _ooz is None:
        _ooz = ctypes.CDLL(os.environ.get("OOZ_LIB", os.path.expanduser("~/build/libooz.so")))
        fn = getattr(_ooz, "_Z17Kraken_DecompressPKhmPhm", None) or getattr(_ooz, "Kraken_Decompress")
        fn.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        fn.restype = ctypes.c_int
        _ooz.fn = fn
    return _ooz


def oodle(src, usize):
    dst = ctypes.create_string_buffer(usize + 64)
    n = ooz().fn(src, len(src), dst, usize)
    if n != usize:
        raise ValueError(f"oodle decode failed ({n} != {usize})")
    return dst.raw[:usize]


def u40be(b):  # 5-byte big-endian
    return int.from_bytes(b, "big")


def u_le(b):
    return int.from_bytes(b, "little")


def fstring(buf, pos):
    n = struct.unpack_from("<i", buf, pos)[0]
    pos += 4
    if n == 0:
        return "", pos
    if n < 0:
        raw = buf[pos:pos + (-n) * 2]
        return raw.decode("utf-16-le").rstrip("\0"), pos + (-n) * 2
    return buf[pos:pos + n].decode("utf-8", "replace").rstrip("\0"), pos + n


class IoStore:
    def __init__(self, utoc):
        self.utoc, self.ucas = utoc, utoc[:-5] + ".ucas"
        d = open(utoc, "rb").read()
        assert d[:16] == b"-==--==--==--==-", "not an IoStore toc"
        self.version = d[0x10]
        (hs, ec, cbc, cbs, cmc, cml, self.block_size, dis, pc) = struct.unpack_from("<9I", d, 0x14)
        self.flags = d[0x50]
        seeds = struct.unpack_from("<I", d, 0x54)[0]
        nophc = struct.unpack_from("<I", d, 0x60)[0]
        p = hs
        self.chunk_ids = [d[p + i * 12:p + i * 12 + 12] for i in range(ec)]; p += ec * 12
        self.offlen = [(u40be(d[p + i * 10:p + i * 10 + 5]), u40be(d[p + i * 10 + 5:p + i * 10 + 10]))
                       for i in range(ec)]; p += ec * 10
        p += seeds * 4 + nophc * 4
        self.blocks = []
        for i in range(cbc):
            b = d[p + i * cbs:p + i * cbs + cbs]
            self.blocks.append((u_le(b[0:5]), u_le(b[5:8]), u_le(b[8:11]), b[11]))
        p += cbc * cbs
        self.methods = [d[p + i * cml:p + (i + 1) * cml].split(b"\0")[0].decode() for i in range(cmc)]
        p += cmc * cml
        if self.flags & 4:  # signed
            hsz = struct.unpack_from("<i", d, p)[0]
            p += 4 + hsz * 2 + cbc * 20
        self.files = {}
        if self.flags & 8 and dis:
            self._read_index(d[p:p + dis])

    def _read_index(self, b):
        mount, p = fstring(b, 0)
        n = struct.unpack_from("<i", b, p)[0]; p += 4
        dirs = [struct.unpack_from("<4I", b, p + i * 16) for i in range(n)]; p += n * 16
        n = struct.unpack_from("<i", b, p)[0]; p += 4
        files = [struct.unpack_from("<3I", b, p + i * 12) for i in range(n)]; p += n * 12
        n = struct.unpack_from("<i", b, p)[0]; p += 4
        strings = []
        for _ in range(n):
            s, p = fstring(b, p)
            strings.append(s)
        NONE = 0xFFFFFFFF
        mount = mount.replace("../../../", "")

        def walk(di, path):
            name, first_child, _next, first_file = dirs[di]
            here = path + (strings[name] + "/" if name != NONE else "")
            f = first_file
            while f != NONE:
                fname, nxt, user = files[f]
                self.files[mount + here + strings[fname]] = user
                f = nxt
            c = first_child
            while c != NONE:
                walk(c, here)
                c = dirs[c][2]

        if dirs:
            walk(0, "")

    def read(self, entry):
        off, length = self.offlen[entry]
        bs = self.block_size
        first, last = off // bs, (off + length - 1) // bs
        out = bytearray()
        with open(self.ucas, "rb") as f:
            for bi in range(first, last + 1):
                coff, csize, usize, mi = self.blocks[bi]
                f.seek(coff)
                raw = f.read((csize + 15) & ~15)[:csize]
                if mi == 0:
                    out += raw[:usize]
                else:
                    out += oodle(raw, usize)
        start = off - first * bs
        return bytes(out[start:start + length])


class Pak:
    MAGIC = 0x5A6F12E1

    def __init__(self, path):
        self.path = path
        size = os.path.getsize(path)
        with open(path, "rb") as f:
            f.seek(max(0, size - 4096)); tail = f.read()
        m = tail.rfind(struct.pack("<I", self.MAGIC))
        assert m >= 0, "pak footer not found"
        # The game encrypts some .pak indexes. We never decrypt game files: encrypted paks are skipped
        # and the mod only reads the unencrypted IoStore containers.
        assert tail[m - 1] == 0, "encrypted index (skipped: the mod does not decrypt game files)"
        self.version, idx_off, idx_size = struct.unpack_from("<IQQ", tail, m + 4)
        names_at = m + 4 + 4 + 16 + 20
        self.methods = [""] + [tail[names_at + i * 32:names_at + (i + 1) * 32].split(b"\0")[0].decode()
                               for i in range(5) if names_at + (i + 1) * 32 <= len(tail)]
        with open(path, "rb") as f:
            f.seek(idx_off); idx = f.read(idx_size)
            self.mount, p = fstring(idx, 0)
            self.mount = self.mount.replace("../../../", "")
            num, _seed = struct.unpack_from("<iQ", idx, p); p += 12
            has_phi = struct.unpack_from("<i", idx, p)[0]; p += 4
            if has_phi:
                p += 8 + 8 + 20
            has_fdi = struct.unpack_from("<i", idx, p)[0]; p += 4
            fdi_off = fdi_size = 0
            if has_fdi:
                fdi_off, fdi_size = struct.unpack_from("<qq", idx, p); p += 16 + 20
            enc_size = struct.unpack_from("<i", idx, p)[0]; p += 4
            self.encoded = idx[p:p + enc_size]
            f.seek(fdi_off); fdi = f.read(fdi_size)
        self.files = {}
        p = 0
        ndirs = struct.unpack_from("<i", fdi, p)[0]; p += 4
        for _ in range(ndirs):
            d, p = fstring(fdi, p)
            nf = struct.unpack_from("<i", fdi, p)[0]; p += 4
            for _ in range(nf):
                fn, p = fstring(fdi, p)
                eo = struct.unpack_from("<i", fdi, p)[0]; p += 4
                self.files[self.mount + d.lstrip("/") + fn] = eo

    def _entry(self, eo):
        b, p = self.encoded, eo
        bits = struct.unpack_from("<I", b, p)[0]; p += 4
        if (bits & 0x3F) == 0x3F:
            cbs = struct.unpack_from("<I", b, p)[0]; p += 4
        else:
            cbs = (bits & 0x3F) << 11
        nblocks = (bits >> 6) & 0xFFFF
        encrypted = (bits >> 22) & 1
        method = (bits >> 23) & 0x3F

        def rd(safe):
            nonlocal p
            if safe:
                v = struct.unpack_from("<I", b, p)[0]; p += 4
            else:
                v = struct.unpack_from("<Q", b, p)[0]; p += 8
            return v

        offset = rd(bits & (1 << 31))
        usize = rd(bits & (1 << 30))
        csize = rd(bits & (1 << 29)) if method else usize
        blocks = []
        hdr = 8 + 8 + 8 + 4 + 20 + (4 + nblocks * 16 if method else 0) + 1 + 4
        if method and nblocks:
            if nblocks == 1 and not encrypted:
                blocks = [(hdr, hdr + csize)]
            else:
                start = hdr
                for _ in range(nblocks):
                    sz = struct.unpack_from("<I", b, p)[0]; p += 4
                    blocks.append((start, start + sz))
                    start += (sz + 15) & ~15 if encrypted else sz
        return offset, usize, csize, method, cbs, blocks, encrypted, hdr

    def read(self, eo):
        offset, usize, csize, method, cbs, blocks, enc, hdr = self._entry(eo)
        assert not enc, "encrypted pak entry"
        with open(self.path, "rb") as f:
            if not method:
                f.seek(offset + hdr); return f.read(usize)
            out = bytearray()
            for i, (s, e) in enumerate(blocks):
                f.seek(offset + s)
                want = min(cbs, usize - i * cbs)
                out += oodle(f.read(e - s), want)
            return bytes(out)


def open_all(paks):
    stores = []
    for n in sorted(os.listdir(paks)):
        full = os.path.join(paks, n)
        if n.endswith(".utoc"):
            stores.append(IoStore(full))
        elif n.endswith(".pak"):
            try:
                stores.append(Pak(full))
            except AssertionError as e:
                print("skip", n, e, file=sys.stderr)
    return stores


def main():
    cmd, paks = sys.argv[1], sys.argv[2]
    stores = open_all(paks)
    if cmd == "info":
        for s in stores:
            kind = "iostore" if isinstance(s, IoStore) else "pak v%d" % s.version
            print(os.path.basename(getattr(s, "utoc", getattr(s, "path", ""))), kind, len(s.files), "files",
                  getattr(s, "methods", ""))
    elif cmd == "ls":
        subs = [x.lower() for x in sys.argv[3:]]
        for s in stores:
            for path in s.files:
                if all(x in path.lower() for x in subs):
                    print(path)
    elif cmd == "cat":
        want = sys.argv[3]
        for s in stores:
            if want in s.files:
                data = s.read(s.files[want])
                if len(sys.argv) > 4:
                    open(sys.argv[4], "wb").write(data)
                print(want, len(data), "bytes")
                return
        print("not found", want)
        sys.exit(1)


if __name__ == "__main__":
    main()
