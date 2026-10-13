#!/usr/bin/env python3
"""Dev tool: decode a Remnant II UI texture (Texture2D) from the player's own install, in place.

Prints the format, size and mip table so the runtime reader (src/r2read) can be checked against it.
With --dds <out.dds> it writes the top mip as a DDS for a local visual check only (never committed).
usage: r2tex.py <Paks dir> <asset path without extension> [--dds out.dds]
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r2pak, r2text

FORMATS = {"PF_BC7": ("BC7", 16), "PF_DXT5": ("BC3", 16), "PF_DXT1": ("BC1", 8), "PF_BC5": ("BC5", 16),
           "PF_BC4": ("BC4", 8), "PF_B8G8R8A8": ("BGRA8", 0), "PF_G8": ("R8", 0)}

# FBulkData flags (UE5)
BULK_PayloadAtEndOfFile = 0x0001
BULK_Size64Bit = 0x2000
BULK_Unused = 0x0020
BULK_ForceInlinePayload = 0x0040
BULK_PayloadInSeperateFile = 0x0100
BULK_OptionalPayload = 0x0800
BULK_NoOffsetFixUp = 0x10000


def find_platform_data(data):
    for off, s in r2text.fstrings(data):
        if s in FORMATS:
            return off, s
    return None, None


def parse(data):
    """UE 5.2 cooked FTexturePlatformData in a zen package (verified on Remnant II):
    SizeX i32, SizeY i32, PackedData u32, PixelFormat FString, [OptData 8], FirstMip i32, NumMips i32,
    then per mip: BulkDataMap index i32, [inline payload when the entry is not in a separate file],
    SizeX i32, SizeY i32, SizeZ i32."""
    off, pf = find_platform_data(data)
    if off is None:
        raise ValueError("no pixel format string found")
    bmap = r2text.bulk_map(data)
    sx, sy, packed = struct.unpack_from("<iiI", data, off - 12)
    p = off + 4 + len(pf) + 1
    if packed & (1 << 30):  # HasOptData: ExtData u32 + NumMipsInTail u32
        p += 8
    first_mip, nmips = struct.unpack_from("<ii", data, p); p += 8
    mips = []
    for _ in range(nmips):
        bi = struct.unpack_from("<i", data, p)[0]; p += 4
        e = bmap[bi]
        inline = None
        if not (e["flags"] & (BULK_PayloadInSeperateFile | BULK_OptionalPayload | BULK_PayloadAtEndOfFile)):
            inline = (p, e["size"])
            p += e["size"]
        mx, my, mz = struct.unpack_from("<iii", data, p); p += 12
        mips.append({"flags": e["flags"], "size": e["size"], "offset": e["offset"], "inline": inline,
                     "w": mx, "h": my})
    return {"w": sx, "h": sy, "format": pf, "first_mip": first_mip, "mips": mips}


def dds(fmt, w, h, payload):
    fourcc = {"BC1": b"DXT1", "BC3": b"DXT5"}.get(fmt)
    hdr = bytearray(b"DDS " + struct.pack("<7I", 124, 0x1007 | 0x80000, h, w, len(payload), 0, 1) + b"\0" * 44)
    pfm = bytearray(32)
    struct.pack_into("<II", pfm, 0, 32, 4)
    pfm[8:12] = fourcc or b"DX10"
    hdr += pfm + struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    if not fourcc:
        dxgi = {"BC7": 98, "BC5": 83, "BC4": 80}[fmt]
        hdr += struct.pack("<5I", dxgi, 3, 0, 1, 0)
    return bytes(hdr) + payload


def main():
    paks, asset = sys.argv[1], sys.argv[2]
    stores = r2pak.open_all(paks)
    idx = {}
    for s in stores:
        for k, v in s.files.items():
            idx[k] = (s, v)
    s, e = idx[asset + ".uasset"]
    data = s.read(e)
    info = parse(data)
    print(info["format"], info["w"], "x", info["h"], "mips", len(info["mips"]))
    for m in info["mips"][:3]:
        print("  mip", m["w"], m["h"], "bytes", m["size"], "inline" if m["inline"] else "ubulk@%d" % m["offset"],
              hex(m["flags"]))
    if "--dds" in sys.argv:
        out = sys.argv[sys.argv.index("--dds") + 1]
        m = info["mips"][0]
        if m["inline"]:
            payload = data[m["inline"][0]:m["inline"][0] + m["size"]]
        else:
            bs, be = idx[asset + ".ubulk"]
            bulk = bs.read(be)
            payload = bulk[m["offset"]:m["offset"] + m["size"]]
        open(out, "wb").write(dds(FORMATS[info["format"]][0], m["w"], m["h"], payload))
        print("wrote", out)


if __name__ == "__main__":
    main()
