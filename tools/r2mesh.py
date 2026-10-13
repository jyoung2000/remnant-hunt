#!/usr/bin/env python3
"""Dev tool: read a Remnant II skeletal mesh (UE 5.2 cooked, Zen package) from the player's own install, in memory.

Prototype of the reader r2read uses at runtime (src/r2read/r2mesh.cpp). Formats follow CUE4Parse (Apache-2.0) and
UE 5.2's own serializers. Unversioned properties are not parsed: the native part of the export is found by its
reference skeleton (bone count, then FName + parent index per bone, the root's parent being -1).

usage: r2mesh.py <Paks dir> <mesh .uasset> [preview.png]
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r2pak, r2text


class R:
    def __init__(self, d, p=0):
        self.d, self.p = d, p

    def u8(self):
        v = self.d[self.p]; self.p += 1; return v

    def i16(self):
        v = struct.unpack_from("<h", self.d, self.p)[0]; self.p += 2; return v

    def u16(self):
        v = struct.unpack_from("<H", self.d, self.p)[0]; self.p += 2; return v

    def i32(self):
        v = struct.unpack_from("<i", self.d, self.p)[0]; self.p += 4; return v

    def u32(self):
        v = struct.unpack_from("<I", self.d, self.p)[0]; self.p += 4; return v

    def b(self):
        v = self.u32()
        if v not in (0, 1):
            raise ValueError(f"bool expected at {self.p - 4}, got {v}")
        return v == 1

    def strip(self):
        g, c = self.d[self.p], self.d[self.p + 1]; self.p += 2; return g, c

    def arr(self, fmt):
        n = self.i32()
        sz = struct.calcsize(fmt)
        v = [struct.unpack_from("<" + fmt, self.d, self.p + i * sz) for i in range(n)]
        self.p += n * sz
        return v

    def bulk(self):
        size, count = self.i32(), self.i32()
        at = self.p
        self.p += size * count
        return size, count, at


def find_ref_skeleton(d, names, start):
    """(offset of the bone count, bone count): the first place where a plausible bone list starts."""
    n_names = len(names)
    for p in range(start, len(d) - 64):
        n = struct.unpack_from("<i", d, p)[0]
        if not 1 <= n <= 4096:
            continue
        ok = True
        for k in range(min(n, 6)):
            ni, num, par = struct.unpack_from("<IIi", d, p + 4 + k * 12)
            if ni >= n_names or num > 64 or (k == 0 and par != -1) or (k > 0 and not 0 <= par < k):
                ok = False
                break
        if ok:
            return p, n
    return None, 0


def parse(d):
    names, name_end = r2text.names_and_end(d)
    header_size = struct.unpack_from("<I", d, 4)[0]
    p, nbones = find_ref_skeleton(d, names, header_size)
    if p is None:
        raise ValueError("no reference skeleton found")
    r = R(d, p + 4)
    bones = []
    for _ in range(nbones):
        ni, num, par = struct.unpack_from("<IIi", d, r.p); r.p += 12
        bones.append({"name": names[ni] + (f"_{num - 1}" if num else ""), "parent": par})
    npose = r.i32()
    for i in range(npose):
        q = struct.unpack_from("<4d", d, r.p); t = struct.unpack_from("<3d", d, r.p + 32); s = struct.unpack_from("<3d", d, r.p + 56)
        r.p += 80
        bones[i].update(rot=q, pos=t, scale=s)
    nmap = r.i32()
    r.p += nmap * 12
    # FSkeletalMeshRenderData (cooked)
    cooked = r.b()
    nlods = r.i32()
    lods = []
    for li in range(nlods):
        g, c = r.strip()
        cooked_out = r.b()
        inlined = r.b()
        required = r.arr("H")
        if cooked_out:
            lods.append(None)
            continue
        sections = []
        for _ in range(r.i32()):
            sg, sc = r.strip()
            sec = {"material": r.i16(), "base_index": r.i32(), "triangles": r.i32()}
            r.b()  # bRecomputeTangent
            r.u8()  # RecomputeTangentsVertexMaskChannel
            r.b()  # bCastShadow
            r.b()  # bVisibleInRayTracing
            sec["base_vertex"] = r.u32()
            for _ in range(r.i32()):  # cloth mapping LODs
                n = r.i32(); r.p += n * 36
            sec["bone_map"] = [x[0] for x in r.arr("H")]
            sec["vertices"] = r.i32()
            sec["max_influences"] = r.i32()
            r.i16()  # CorrespondClothAssetIndex
            r.p += 20  # FClothingSectionData
            if not (sc & 1):
                n = r.i32(); r.p += n * 4  # DupVertData
                n = r.i32(); r.p += n * 8  # DupVertIndexData
            sec["disabled"] = r.b()
            sections.append(sec)
        active = r.arr("H")
        r.p += 4  # buffers size
        lod = {"sections": sections, "inlined": inlined}
        if inlined:
            sg, sc = r.strip()
            isz = r.u8()
            esz, cnt, at = r.bulk()
            lod["indices"] = struct.unpack_from(f"<{cnt}{'H' if esz == 2 else 'I'}", d, at)
            stride, nverts = r.i32(), r.i32()
            esz, cnt, at = r.bulk()
            lod["positions"] = [struct.unpack_from("<3f", d, at + i * 12) for i in range(cnt)]
            vg, vc = r.strip()
            ntex, nv = r.i32(), r.i32()
            full_uv, high_tan = r.b(), r.b()
            esz, cnt, at_t = r.bulk()  # tangents
            esz_uv, cnt_uv, at_uv = r.bulk()
            uvs = []
            for i in range(nv):
                if full_uv:
                    u, v = struct.unpack_from("<2f", d, at_uv + i * ntex * 8)
                else:
                    u, v = struct.unpack_from("<2e", d, at_uv + i * ntex * 4)
                uvs.append((u, v))
            lod["uvs"] = uvs
            normals = []
            for i in range(nv):
                if high_tan:
                    zx, zy, zz, zw = struct.unpack_from("<4h", d, at_t + i * 16 + 8)
                    normals.append((zx / 32767, zy / 32767, zz / 32767))
                else:
                    zx, zy, zz, zw = struct.unpack_from("<4B", d, at_t + i * 8 + 4)
                    normals.append(((zx - 127.5) / 127.5, (zy - 127.5) / 127.5, (zz - 127.5) / 127.5))
            lod["normals"] = normals
            # skin weights
            wg, wc = r.strip()
            variable = r.b()
            max_inf = r.u32()
            nbone_entries = r.u32()
            nvw = r.u32()
            idx16 = r.b()
            w16 = r.b()
            esz, cnt, at_w = r.bulk()
            lg, lc = r.strip()
            nlook = r.i32()
            lsz, lcnt, at_l = r.bulk()
            weights = []
            ib = 2 if idx16 else 1
            wb = 2 if w16 else 1
            for i in range(nvw):
                if variable:
                    look = struct.unpack_from("<I", d, at_l + i * 4)[0]
                    off, ninf = look >> 8, look & 0xFF
                else:
                    off, ninf = i * max_inf * (ib + wb), max_inf
                q = at_w + off
                idx = struct.unpack_from(f"<{ninf}{'H' if idx16 else 'B'}", d, q)
                wts = struct.unpack_from(f"<{ninf}{'H' if w16 else 'B'}", d, q + ninf * ib)
                weights.append((idx, wts))
            lod["weights"] = weights
            lod["info"] = dict(index_size=isz, verts=nverts, tex=ntex, full_uv=full_uv, high_tan=high_tan,
                               max_inf=max_inf, variable=variable, idx16=idx16, w16=w16)
            lods.append(lod)
            break  # the rest of LOD0's streamed data and the other LODs are not needed for the prototype
        else:
            lods.append({"sections": sections, "inlined": False})
            break
    return {"bones": bones, "lods": lods, "skeleton_at": p}


def preview(mesh, out):
    """Software render of LOD0 from the side and the front, shaded by normal, to check the reader by eye."""
    import numpy as np
    from PIL import Image
    lod = mesh["lods"][0]
    P = np.array(lod["positions"], dtype=np.float32)
    N = np.array(lod["normals"], dtype=np.float32)
    I = np.array(lod["indices"], dtype=np.int64).reshape(-1, 3)
    W, H = 640, 480
    img = np.zeros((H, W * 2, 3), dtype=np.uint8) + 30
    for view, (ax, ay, flip) in enumerate([(1, 2, 1), (0, 2, 1)]):  # UE: x forward, y right, z up
        x, y = P[:, ax], P[:, ay]
        mn = np.array([x.min(), y.min()]); mx = np.array([x.max(), y.max()])
        s = 0.9 * min(W / (mx[0] - mn[0]), H / (mx[1] - mn[1]))
        sx = (x - (mn[0] + mx[0]) / 2) * s + W / 2 + view * W
        sy = H / 2 - (y - (mn[1] + mx[1]) / 2) * s
        depth = P[:, 3 - ax - ay] if view else P[:, 0]
        zbuf = np.full((H, W * 2), -1e9, dtype=np.float32)
        light = np.array([0.3, -0.4, 0.85]); light /= np.linalg.norm(light)
        shade = np.clip(N @ light, 0.15, 1.0)
        for tri in I[:: max(1, len(I) // 120000)]:
            xs, ys = sx[tri], sy[tri]
            x0, x1 = int(max(view * W, xs.min())), int(min(view * W + W - 1, xs.max()))
            y0, y1 = int(max(0, ys.min())), int(min(H - 1, ys.max()))
            z = depth[tri].mean()
            c = int(255 * shade[tri].mean())
            if x1 >= x0 and y1 >= y0 and x1 - x0 < 40 and y1 - y0 < 40:
                region = zbuf[y0:y1 + 1, x0:x1 + 1]
                m = region < z
                region[m] = z
                img[y0:y1 + 1, x0:x1 + 1][m] = (c, int(c * 0.9), int(c * 0.8))
    Image.fromarray(img).save(out)


def main():
    paks, asset = sys.argv[1], sys.argv[2]
    for s in r2pak.open_all(paks):
        if asset in s.files:
            d = s.read(s.files[asset])
            m = parse(d)
            print("bones", len(m["bones"]), "root", m["bones"][0]["name"])
            for i, lod in enumerate(m["lods"]):
                if not lod:
                    continue
                print("lod", i, "sections", [(x["material"], x["triangles"], x["vertices"]) for x in lod["sections"]])
                if "positions" in lod:
                    P = lod["positions"]
                    print("  verts", len(P), "indices", len(lod["indices"]), lod["info"])
                    xs = [p[0] for p in P]; ys = [p[1] for p in P]; zs = [p[2] for p in P]
                    print("  bounds x %.0f..%.0f y %.0f..%.0f z %.0f..%.0f" % (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))
                    print("  max index", max(lod["indices"]), "uv0", lod["uvs"][0], "w0", lod["weights"][0])
            if len(sys.argv) > 3:
                preview(m, sys.argv[3])
            return
    print("not found", asset)


if __name__ == "__main__":
    main()
