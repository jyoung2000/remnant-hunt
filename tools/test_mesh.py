#!/usr/bin/env python3
"""Dev check: ask r2read (Linux build) for a mesh, its skeleton and an animation, pose the mesh at a few frames and
render previews, to see the decoders work before the mod draws anything.
usage: test_mesh.py <r2read> <Remnant II dir> <mesh> <skeleton> <anim> <out.png>"""
import struct, subprocess, sys
import numpy as np
from PIL import Image

exe, r2dir, mesh_a, skel_a, anim_a, out = sys.argv[1:7]
p = subprocess.Popen([exe, r2dir], stdin=subprocess.PIPE, stdout=subprocess.PIPE)


def ask(cmd):
    p.stdin.write((cmd + "\n").encode()); p.stdin.flush()
    head = p.stdout.readline().decode().strip()
    if not head.startswith("OK"):
        raise SystemExit(cmd + " -> " + head)
    n = int(head.split()[-1])
    return p.stdout.read(n)


ask("HELLO") if False else None
p.stdin.write(b"HELLO\n"); p.stdin.flush(); print(p.stdout.readline().decode().strip())


class Rd:
    def __init__(s, b): s.b, s.p = b, 0
    def u(s, f):
        v = struct.unpack_from("<" + f, s.b, s.p); s.p += struct.calcsize("<" + f); return v if len(v) > 1 else v[0]
    def bones(s):
        out = []
        for _ in range(s.u("I")):
            n = s.u("H"); name = s.b[s.p:s.p + n].decode(); s.p += n
            par = s.u("i"); q = s.u("4f"); t = s.u("3f"); sc = s.u("3f")
            out.append((name, par, q, t, sc))
        return out


m = Rd(ask("MESH\t" + mesh_a)); assert m.b[:4] == b"RMSH"; m.p = 4
mb = m.bones()
mats = []
for _ in range(m.u("I")):
    n = m.u("H"); mats.append(m.b[m.p:m.p + n].decode()); m.p += n
secs = [m.u("hHIIII")[0] for _ in range(m.u("I"))] if False else None
ns = m.u("I"); m.p += ns * 20
nv = m.u("I")
V = np.frombuffer(m.b, dtype=np.dtype([("pos", "<3f4"), ("n", "<4i2"), ("uv", "<2f4"), ("bi", "<8u2"), ("bw", "<8u1")]), count=nv, offset=m.p)
m.p += nv * 52
ni = m.u("I")
I = np.frombuffer(m.b, dtype="<u4", count=ni, offset=m.p).reshape(-1, 3)
print("mesh bones", len(mb), "materials", mats, "verts", nv, "tris", len(I))

s = Rd(ask("SKEL\t" + skel_a)); s.p = 4
sb = s.bones()
a = Rd(ask("ANIM\t" + anim_a)); a.p = 4
nt, nsmp, rate = a.u("I"), a.u("I"), a.u("f")
t2b = np.frombuffer(a.b, dtype="<i4", count=nt, offset=a.p); a.p += nt * 4
S = np.frombuffer(a.b, dtype="<f4", count=nt * nsmp * 10, offset=a.p).reshape(nsmp, nt, 10)
print("skeleton bones", len(sb), "anim tracks", nt, "samples", nsmp, "rate", rate, "t2b", t2b[:8])


def qmat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def mat(q, t, sc):
    M = np.eye(4); M[:3, :3] = qmat(q) * np.array(sc)[None, :]; M[:3, 3] = t; return M


def globals_(local, parents):
    G = [None] * len(local)
    for i, L in enumerate(local):
        G[i] = L if parents[i] < 0 else G[parents[i]] @ L
    return G


mparents = [b[1] for b in mb]
ref_local = [mat(b[2], b[3], b[4]) for b in mb]
ref_g = globals_(ref_local, mparents)
inv_ref = [np.linalg.inv(g) for g in ref_g]
mesh_index = {b[0]: i for i, b in enumerate(mb)}
track_of_mesh_bone = {}
for t in range(nt):
    sbone = t2b[t]
    if 0 <= sbone < len(sb) and sb[sbone][0] in mesh_index:
        track_of_mesh_bone[mesh_index[sb[sbone][0]]] = t
print("mesh bones driven by the animation:", len(track_of_mesh_bone), "of", len(mb))

P0 = np.array(V["pos"], dtype=np.float64)
N0 = V["n"][:, :3].astype(np.float64) / 32767.0
BI = V["bi"].astype(np.int64); BW = V["bw"].astype(np.float64) / 255.0


def pose(frame):
    local = []
    for i, b in enumerate(mb):
        t = track_of_mesh_bone.get(i)
        if t is None:
            local.append(ref_local[i])
        else:
            q = S[frame, t, 0:4]; tr = S[frame, t, 4:7]; sc = S[frame, t, 7:10]
            local.append(mat(q, tr, sc))
    G = globals_(local, mparents)
    skin = np.array([G[i] @ inv_ref[i] for i in range(len(mb))])
    Ph = np.c_[P0, np.ones(len(P0))]
    out = np.zeros((len(P0), 3)); nout = np.zeros((len(P0), 3))
    for k in range(8):
        w = BW[:, k]
        if not w.any():
            continue
        Mk = skin[BI[:, k]]
        out += w[:, None] * np.einsum("vij,vj->vi", Mk, Ph)[:, :3]
        nout += w[:, None] * np.einsum("vij,vj->vi", Mk[:, :3, :3], N0)
    return out, nout


def draw(P, N, img, x0, W, H, ax=(1, 2)):
    x, y = P[:, ax[0]], P[:, ax[1]]
    span = max(np.ptp(P0[:, ax[0]]), np.ptp(P0[:, ax[1]]), 100.0)
    s = 0.85 * min(W, H) / span
    sx = (x - P0[:, ax[0]].mean()) * s + x0 + W / 2; sy = H * 0.92 - (y - P0[:, ax[1]].min()) * s
    depth = P[:, 0]
    light = np.array([0.3, -0.4, 0.85]); light /= np.linalg.norm(light)
    nn = N / (np.linalg.norm(N, axis=1)[:, None] + 1e-9)
    shade = np.clip(nn @ light, 0.15, 1.0)
    z = np.full((H, img.shape[1]), -1e9)
    for tri in I[:: max(1, len(I) // 60000)]:
        xs, ys = sx[tri], sy[tri]
        a0, a1 = int(max(x0, xs.min())), int(min(x0 + W - 1, xs.max()))
        b0, b1 = int(max(0, ys.min())), int(min(H - 1, ys.max()))
        if a1 < a0 or b1 < b0 or a1 - a0 > 30 or b1 - b0 > 30:
            continue
        d = depth[tri].mean(); c = int(255 * shade[tri].mean())
        reg = z[b0:b1 + 1, a0:a1 + 1]; msk = reg < d; reg[msk] = d
        img[b0:b1 + 1, a0:a1 + 1][msk] = (c, int(c * .88), int(c * .78))


W, H = 420, 360
frames = [0, nsmp // 3, 2 * nsmp // 3]
img = np.zeros((H, W * (len(frames) + 1), 3), dtype=np.uint8) + 28
draw(P0, N0, img, 0, W, H)
for k, f in enumerate(frames):
    P, N = pose(f)
    draw(P, N, img, (k + 1) * W, W, H)
Image.fromarray(img).save(out)
print("saved", out)
p.stdin.write(b"QUIT\n"); p.stdin.flush()
