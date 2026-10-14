// Remnant x MHW mashup core: the hunter's skeleton, the joint block matcher and the Remnant -> hunter retarget.
// Pure math (no game memory), so tests/test_hunter.cpp runs it on synthetic data. GPL-3.0-or-later.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "gen/sheets.gen.h"
#include "hunter.h"
#include "model.h"

namespace rh {

namespace {

// 3x3 row-major rotations, column-vector convention.
void m3_mul(const float a[9], const float b[9], float o[9]) {
    float r[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    memcpy(o, r, sizeof(r));
}
void m3_t(const float a[9], float o[9]) {
    float r[9] = {a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8]};
    memcpy(o, r, sizeof(r));
}
void m3_v(const float a[9], const float v[3], float o[3]) {
    float r[3];
    for (int i = 0; i < 3; i++) r[i] = a[i * 3] * v[0] + a[i * 3 + 1] * v[1] + a[i * 3 + 2] * v[2];
    memcpy(o, r, sizeof(r));
}
void m3_id(float o[9]) {
    static const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    memcpy(o, I, sizeof(I));
}
const float kC[9] = {1, 0, 0, 0, 0, 1, 0, 1, 0};  // Remnant (x, y, z) -> MHW (x, z, y)

// The row-vector game matrix <-> rotation (column form) and translation.
void from_game(const float g[16], float R[9], float p[3]) {
    for (int i = 0; i < 3; i++)
        for (int r = 0; r < 3; r++) R[i * 3 + r] = g[r * 4 + i];
    p[0] = g[12], p[1] = g[13], p[2] = g[14];
}
void to_game(const float R[9], const float p[3], float g[16]) {
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 3; i++) g[r * 4 + i] = R[i * 3 + r];
        g[r * 4 + 3] = 0;
    }
    g[12] = p[0], g[13] = p[1], g[14] = p[2], g[15] = 1;
}
// 3x4 (row-major, column-vector) <-> rotation + translation
void from34(const float* m, float R[9], float p[3]) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) R[i * 3 + j] = m[i * 4 + j];
        p[i] = m[i * 4 + 3];
    }
}
void to34(const float R[9], const float p[3], float* m) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m[i * 4 + j] = R[i * 3 + j];
        m[i * 4 + 3] = p[i];
    }
}
void mul34(const float* a, const float* b, float* out) {
    float o[12];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) o[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j];
        o[i * 4 + 3] = a[i * 4] * b[3] + a[i * 4 + 1] * b[7] + a[i * 4 + 2] * b[11] + a[i * 4 + 3];
    }
    memcpy(out, o, sizeof(o));
}
void xform34(const Xform& x, float m[12]) {
    float qx = x.q[0], qy = x.q[1], qz = x.q[2], qw = x.q[3];
    float r[9] = {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw),
                  2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw),
                  2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m[i * 4 + j] = r[i * 3 + j] * x.s[j];
        m[i * 4 + 3] = x.t[i];
    }
}
float dist(const float a[3], const float b[3]) {
    float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}
bool unit(float v[3]) {
    float n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n < 1e-6f) return false;
    for (int i = 0; i < 3; i++) v[i] /= n;
    return true;
}
// The smallest rotation taking unit a onto unit b.
void rot_between(const float a[3], const float b[3], float R[9]) {
    float v[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    float c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    if (c < -0.9999f) {  // opposite: half a turn about any perpendicular axis
        float ax[3] = {0, -a[2], a[1]};
        if (!unit(ax)) { ax[0] = -a[2], ax[1] = 0, ax[2] = a[0]; unit(ax); }
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) R[i * 3 + j] = 2 * ax[i] * ax[j] - (i == j ? 1.0f : 0.0f);
        return;
    }
    float K[9] = {0, -v[2], v[1], v[2], 0, -v[0], -v[1], v[0], 0}, K2[9];
    m3_mul(K, K, K2);
    float k = 1.0f / (1.0f + c);
    for (int i = 0; i < 9; i++) R[i] = (i % 4 == 0 ? 1.0f : 0.0f) + K[i] + K2[i] * k;
}
float rot_sim(const float a[9], const float b[9]) {  // 1 = the same rotation
    float s = 0;
    for (int i = 0; i < 9; i++) s += a[i] * b[i];
    return s / 3.0f;
}

std::vector<int> topo_order(const std::vector<HBone>& B) {
    std::vector<int> order, state(B.size(), 0);
    std::vector<std::vector<int>> kids(B.size());
    for (size_t i = 0; i < B.size(); i++)
        if (B[i].parent >= 0) kids[B[i].parent].push_back((int)i);
    for (size_t i = 0; i < B.size(); i++) {
        if (B[i].parent >= 0) continue;
        std::vector<int> st{(int)i};
        while (!st.empty()) {
            int h = st.back();
            st.pop_back();
            order.push_back(h);
            for (auto it = kids[h].rbegin(); it != kids[h].rend(); ++it) st.push_back(*it);
        }
    }
    return order;
}

// Bones at their parent in the bind pose, from the root down (the root's sub-root, the pelvis): animated
// translations, so the bind lengths say nothing about where they are in the game.
std::vector<char> free_bones(const std::vector<HBone>& B) {
    std::vector<char> f(B.size(), 0);
    int r0 = -1;
    for (size_t i = 0; i < B.size(); i++)
        if (B[i].id == 0) r0 = (int)i;
    for (int h : topo_order(B)) {
        int p = B[h].parent;
        if (p >= 0 && B[h].len < 0.5f && (p == r0 || f[p])) f[h] = 1;
    }
    return f;
}

}  // namespace

void hunter_r2_to_mhw(const float in[3], float out[3]) { m3_v(kC, in, out); }

const std::vector<HBone>& hunter_bones() {
    static std::vector<HBone> B;
    if (!B.empty()) return B;
    using namespace sheets;
    B.resize(kHunter_bonesCount);
    std::map<int, int> byId;
    for (int i = 0; i < kHunter_bonesCount; i++) byId[atoi(kHunter_bones[i].id)] = i;
    for (int i = 0; i < kHunter_bonesCount; i++) {
        const Hunter_bones& r = kHunter_bones[i];
        HBone& b = B[i];
        b.id = atoi(r.id);
        int pid = (int)r.parent_id;
        b.parent = pid >= 0 && byId.count(pid) ? byId[pid] : -1;
        b.len = r.length;
        sscanf(r.local_pos, "%f,%f,%f", &b.local[0], &b.local[1], &b.local[2]);
        if (r.r2 && strcmp(r.r2, "none") != 0 && r.r2[0]) b.r2 = r.r2;
    }
    for (int h : topo_order(B)) {
        HBone& b = B[h];
        for (int k = 0; k < 3; k++) b.bind[k] = b.local[k] + (b.parent >= 0 ? B[b.parent].bind[k] : 0.0f);
    }
    return B;
}

int hunter_bone_by_id(int id) {
    const auto& B = hunter_bones();
    for (size_t i = 0; i < B.size(); i++)
        if (B[i].id == id) return (int)i;
    return -1;
}

// ---------------------------------------------------------------- matching

namespace {

struct Ent { float p[3], R[9]; bool ok; };

struct Matcher {
    const std::vector<HBone>& B;
    std::vector<Ent> E;
    std::vector<std::vector<int>> kids;
    std::vector<int> order;
    std::vector<char> isFree;
    int r0 = -1;
    explicit Matcher(const std::vector<HBone>& b) : B(b) {
        kids.resize(B.size());
        for (size_t i = 0; i < B.size(); i++)
            if (B[i].parent >= 0) kids[B[i].parent].push_back((int)i);
        order = topo_order(B);
        for (size_t i = 0; i < B.size(); i++)
            if (B[i].id == 0) r0 = (int)i;
        isFree = free_bones(B);
    }
    bool fits(int h, const float from[3], int e) const {
        return E[e].ok && fabsf(dist(E[e].p, from) - B[h].len) <= 2.0f;
    }
    // Entry e where bone h sits on entry c as its parent: c's rotation applied to h's bind offset (pose independent
    // while the joints only rotate).
    // 1 exactly there .. 0 at 3 cm or more.
    float fitsAt(int h, int c, int e) const {
        if (!E[e].ok) return 0;
        float w[3];
        m3_v(E[c].R, B[h].local, w);
        for (int k = 0; k < 3; k++) w[k] += E[c].p[k];
        return std::max(0.0f, 1.0f - dist(E[e].p, w) / 3.0f);
    }
    // How well the children of h are found from entry c: 1 each at the expected place, 0.25 at the length only
    // (nontrivial: only the children with a length).
    float look(int h, int c, const std::vector<char>& taken, bool nontrivial) const {
        float n = 0;
        for (int k : kids[h]) {
            if (nontrivial && B[k].len < 0.5f) continue;
            float best = 0;
            for (size_t e = 0; e < E.size() && best < 0.999f; e++) {
                if ((int)e == c || taken[e]) continue;
                float f = fitsAt(k, c, (int)e);
                if (f > 0) best = std::max(best, 0.25f + 0.75f * f);
                else if (fits(k, E[c].p, (int)e)) best = std::max(best, 0.25f);
            }
            n += best;
        }
        return n;
    }
    int run(int rootEntry, int out[kHunterBones]) const {
        int nb = (int)std::min<size_t>(B.size(), kHunterBones);
        for (int i = 0; i < kHunterBones; i++) out[i] = -1;
        std::vector<char> taken(E.size(), 0);
        out[r0] = rootEntry;
        taken[rootEntry] = 1;
        std::vector<int> deferred;
        for (int h : order) {
            if (h >= nb || h == r0 || B[h].parent < 0) continue;
            int best = -1;
            float bestS = -1e9f;
            if (isFree[h]) {
                bool any = false;
                for (int k : kids[h]) any |= B[k].len >= 0.5f;
                if (!any) { deferred.push_back(h); continue; }
                for (size_t e = 0; e < E.size(); e++) {
                    if (!E[e].ok || taken[e] || dist(E[e].p, E[rootEntry].p) > 200.0f) continue;
                    float s = look(h, (int)e, taken, true);
                    if (s <= 0) continue;
                    int pe = out[B[h].parent];  // ties: at the parent (the pelvis sits on the sub-root), turned as it
                    if (pe >= 0) s += 0.5f * std::max(0.0f, 1.0f - dist(E[e].p, E[pe].p) / 2.0f) + 0.25f * rot_sim(E[e].R, E[pe].R);
                    if (s > bestS) { bestS = s; best = (int)e; }
                }
            } else {
                int pe = out[B[h].parent];
                if (pe < 0) continue;
                const Ent& P = E[pe];
                float want[3];
                m3_v(P.R, B[h].local, want);
                float wantDir[3] = {want[0], want[1], want[2]};
                bool hasDir = unit(wantDir);
                for (size_t e = 0; e < E.size(); e++) {
                    if (taken[e] || !fits(h, P.p, (int)e)) continue;
                    float d[3] = {E[e].p[0] - P.p[0], E[e].p[1] - P.p[1], E[e].p[2] - P.p[2]};
                    float pref;
                    float at = fitsAt(h, pe, (int)e);
                    if (at > 0) pref = 2.0f + 4.0f * at;
                    else if (B[h].len >= 1.0f && hasDir && unit(d)) pref = d[0] * wantDir[0] + d[1] * wantDir[1] + d[2] * wantDir[2];
                    else pref = rot_sim(E[e].R, P.R);
                    float s = look(h, (int)e, taken, false) + 0.5f * pref;
                    if (s > bestS) { bestS = s; best = (int)e; }
                }
            }
            if (best >= 0) { out[h] = best; taken[best] = 1; }
        }
        // Free bones with no length below them (the sub-root): where their child or their parent is.
        for (int h : deferred) {
            int ref = -1;
            for (int k : kids[h])
                if (out[k] >= 0) ref = out[k];
            if (ref < 0) ref = out[B[h].parent];
            if (ref < 0) continue;
            int best = -1;
            float bestS = -1e9f;
            for (size_t e = 0; e < E.size(); e++) {
                if (!E[e].ok || taken[e] || dist(E[e].p, E[ref].p) > 2.0f) continue;
                float s = rot_sim(E[e].R, E[ref].R);
                if (s > bestS) { bestS = s; best = (int)e; }
            }
            if (best >= 0) { out[h] = best; taken[best] = 1; }
        }
        int n = 0;
        for (int i = 0; i < nb; i++) n += out[i] >= 0;
        return n;
    }
};

}  // namespace

bool hunter_match(const float* block, const uint8_t* ok, int n, const float playerPos[3], HunterMap* out, int minMatched) {
    *out = HunterMap();
    const auto& B = hunter_bones();
    Matcher M(B);
    if (M.r0 < 0 || n <= 0) return false;
    M.E.resize(n);
    std::vector<std::pair<float, int>> roots;
    for (int i = 0; i < n; i++) {
        Ent& e = M.E[i];
        e.ok = ok[i] != 0;
        from_game(block + (size_t)i * 16, e.R, e.p);
        if (!e.ok) continue;
        float d = dist(e.p, playerPos);
        if (d < 20.0f) roots.push_back({d, i});
    }
    std::sort(roots.begin(), roots.end());
    if (roots.size() > 4) roots.resize(4);
    int best[kHunterBones], bestN = 0, bestRoot = -1;
    for (auto& r : roots) {
        int cur[kHunterBones];
        int m = M.run(r.second, cur);
        if (m > bestN) {
            bestN = m;
            bestRoot = r.second;
            memcpy(best, cur, sizeof(best));
        }
    }
    if (bestRoot < 0) return false;
    memcpy(out->entry, best, sizeof(best));
    out->matched = bestN;
    out->root = bestRoot;
    out->valid = bestN >= minMatched;
    // The other copies of each matched bone (the same matrix in another model's part of the block).
    std::vector<char> used(n, 0);
    for (int e : best)
        if (e >= 0) used[e] = 1;
    for (int h = 0; h < kHunterBones; h++) {
        int e = best[h];
        if (e < 0) continue;
        for (int j = 0; j < n; j++)
            if (!used[j] && M.E[j].ok && dist(M.E[j].p, M.E[e].p) < 0.3f && rot_sim(M.E[j].R, M.E[e].R) > 0.999f) {
                out->aliases.push_back({j, h});
                used[j] = 1;
            }
    }
    return out->valid;
}

// ---------------------------------------------------------------- the rig

bool hunter_rig_build(const std::vector<std::string>& names, const float* G, int n, HunterRig* rig) {
    *rig = HunterRig();
    const auto& B = hunter_bones();
    std::map<std::string, int> byName;
    for (int i = 0; i < n; i++) byName[names[i]] = i;
    for (size_t h = 0; h < B.size() && h < (size_t)kHunterBones; h++) {
        m3_id(rig->A[h]);
        m3_id(rig->CR[h]);
        if (B[h].r2.empty()) continue;
        auto f = byName.find(B[h].r2);
        if (f == byName.end()) continue;
        rig->r2[h] = f->second;
        float R[9], p[3];
        from34(G + (size_t)f->second * 12, R, p);
        m3_mul(kC, R, rig->CR[h]);
        rig->mapped++;
        if (B[h].r2 == "Bone_M_Pelvis") {
            rig->pelvisR2Bone = f->second;
            rig->pelvisH = (int)h;
            memcpy(rig->pelvisR2, p, sizeof(p));
        }
    }
    // The rest pose correction: each mapped bone's direction to its longest mapped child, Remnant's (A-pose) turned
    // onto the hunter's (T-pose).
    for (size_t h = 0; h < B.size() && h < (size_t)kHunterBones; h++) {
        if (rig->r2[h] < 0) continue;
        int child = -1;
        for (size_t k = 0; k < B.size() && k < (size_t)kHunterBones; k++)
            if (B[k].parent == (int)h && rig->r2[k] >= 0 && B[k].len > 1.0f && (child < 0 || B[k].len > B[child].len))
                child = (int)k;
        if (child < 0) continue;
        float dh[3] = {B[child].bind[0] - B[h].bind[0], B[child].bind[1] - B[h].bind[1], B[child].bind[2] - B[h].bind[2]};
        const float* a = G + (size_t)rig->r2[h] * 12;
        const float* b = G + (size_t)rig->r2[child] * 12;
        float dr[3] = {b[3] - a[3], b[7] - a[7], b[11] - a[11]}, drm[3];
        m3_v(kC, dr, drm);
        if (unit(dh) && unit(drm)) rot_between(drm, dh, rig->A[h]);
    }
    rig->ok = rig->mapped >= 20 && rig->pelvisH >= 0;
    return rig->ok;
}

void hunter_retarget(const HunterRig& rig, const float* r2G, const float* r2Bind, const float rootM[16],
                     const float pelvisM[16], float out[kHunterBones][16], bool set[kHunterBones]) {
    const auto& B = hunter_bones();
    float F[9], rootP[3], pelR[9], pelP[3];
    from_game(rootM, F, rootP);
    from_game(pelvisM, pelR, pelP);
    float W[kHunterBones][9], P[kHunterBones][3];
    int nb = (int)std::min<size_t>(B.size(), kHunterBones);
    for (int i = 0; i < kHunterBones; i++) set[i] = false;
    // the pelvis's own motion in the animation (crouching, leaning), in the world
    float pelD[3] = {0, 0, 0};
    if (rig.pelvisR2Bone >= 0) {
        const float* a = r2G + (size_t)rig.pelvisR2Bone * 12;
        const float* b = r2Bind + (size_t)rig.pelvisR2Bone * 12;
        float d[3] = {a[3] - b[3], a[7] - b[7], a[11] - b[11]}, dm[3];
        m3_v(kC, d, dm);
        m3_v(F, dm, pelD);
    }
    static const std::vector<char> isFree = free_bones(B);
    for (int h : topo_order(B)) {
        if (h >= nb) continue;
        int p = B[h].parent;
        bool free = isFree[h] != 0;
        if (p < 0) {  // the root (and the stray roots): the game's root
            memcpy(W[h], F, sizeof(F));
            float o[3];
            m3_v(F, B[h].bind, o);
            for (int k = 0; k < 3; k++) P[h][k] = rootP[k] + (B[h].id == 0 ? 0.0f : o[k]);
        } else if (free) {  // the sub-root and the pelvis: at the game's pelvis
            memcpy(W[h], W[p], sizeof(F));
            for (int k = 0; k < 3; k++) P[h][k] = pelP[k] + (rig.r2[h] >= 0 ? pelD[k] : 0.0f);
        } else {
            float o[3];
            m3_v(W[p], B[h].local, o);
            for (int k = 0; k < 3; k++) P[h][k] = P[p][k] + o[k];
            memcpy(W[h], W[p], sizeof(F));
        }
        if (rig.r2[h] >= 0) {
            // world = F * C * Rcur * (A * C * Rbind)^-1
            float Rc[9], pc[3], T[9], inv[9], AC[9];
            from34(r2G + (size_t)rig.r2[h] * 12, Rc, pc);
            m3_mul(F, kC, T);
            m3_mul(T, Rc, T);
            m3_mul(rig.A[h], rig.CR[h], AC);
            m3_t(AC, inv);
            m3_mul(T, inv, W[h]);
            set[h] = true;
        }
        to_game(W[h], P[h], out[h]);
    }
}

bool hunter_mesh_palette(const HunterRig& rig, const Model& m, const float hw[kHunterBones][16], const bool hv[kHunterBones],
                         std::vector<float>* palette) {
    if (!rig.ok || rig.pelvisH < 0 || !hv[rig.pelvisH]) return false;
    const auto& B = hunter_bones();
    static std::map<std::string, int> byName;
    if (byName.empty())
        for (size_t h = 0; h < B.size() && h < (size_t)kHunterBones; h++)
            if (!B[h].r2.empty()) byName[B[h].r2] = (int)h;
    size_t n = m.bones.size();
    if (m.invBind.size() < n * 12) return false;
    std::vector<float> bind(n * 12), G(n * 12);
    // the place of the Remnant model in the world, through the hunter's pelvis: [R_pelvis | p_pelvis] * [C | -C*pelvis]
    float pr[9], pp[3], H[12], K[12], HK[12];
    from_game(hw[rig.pelvisH], pr, pp);
    to34(pr, pp, H);
    float cp[3];
    m3_v(kC, rig.pelvisR2, cp);
    for (int k = 0; k < 3; k++) cp[k] = -cp[k];
    to34(kC, cp, K);
    mul34(H, K, HK);
    for (size_t i = 0; i < n; i++) {
        float local[12];
        xform34(m.bones[i].ref, local);
        int p = m.bones[i].parent;
        bool hasP = p >= 0 && (size_t)p < i;
        if (hasP) mul34(&bind[p * 12], local, &bind[i * 12]);
        else memcpy(&bind[i * 12], local, sizeof(local));
        auto f = byName.find(m.bones[i].name);
        int h = f != byName.end() ? f->second : -1;
        if (h >= 0 && hv[h]) {
            float Rh[9], ph[3], Rb[9], pb[3], T[9];
            from_game(hw[h], Rh, ph);
            from34(&bind[i * 12], Rb, pb);
            m3_mul(Rh, rig.A[h], T);
            m3_mul(T, kC, T);
            m3_mul(T, Rb, T);
            to34(T, ph, &G[i * 12]);
        } else if (hasP) {
            mul34(&G[p * 12], local, &G[i * 12]);
        } else {
            mul34(HK, &bind[i * 12], &G[i * 12]);
        }
    }
    palette->resize(n * 12);
    for (size_t i = 0; i < n; i++) mul34(&G[i * 12], &m.invBind[i * 12], &(*palette)[i * 12]);
    return true;
}

}  // namespace rh
