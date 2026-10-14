// Host test of the hunter bone matcher and the Remnant -> hunter retarget (no game): a synthetic joint block built
// from the hunter's bind pose (sheets/hunter_bones.json), shuffled, with copies and noise; and a T-pose retarget.
// usage: test_hunter.exe [build/r2_player_bones.json]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "hunter.h"

using namespace rh;

static int g_fail = 0;
#define CHECK(c, ...)                         \
    do {                                      \
        if (!(c)) {                           \
            g_fail++;                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);              \
            printf("\n");                     \
        }                                     \
    } while (0)

static const float kC[9] = {1, 0, 0, 0, 0, 1, 0, 1, 0};
static void m3_mul(const float a[9], const float b[9], float o[9]) {
    float r[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    memcpy(o, r, sizeof(r));
}
static void yaw(float deg, float R[9]) {
    float c = cosf(deg * 3.14159265f / 180), s = sinf(deg * 3.14159265f / 180);
    float r[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
    memcpy(R, r, sizeof(r));
}
static void game_matrix(const float R[9], const float p[3], float* g) {
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 3; i++) g[r * 4 + i] = R[i * 3 + r];
        g[r * 4 + 3] = 0;
    }
    g[12] = p[0], g[13] = p[1], g[14] = p[2], g[15] = 1;
}

// ---- 1. the matcher on a shuffled synthetic block
static void test_match(bool posed) {
    const auto& B = hunter_bones();
    CHECK(B.size() == kHunterBones, "hunter_bones has %zu rows", B.size());
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> U(-1, 1);
    float R[9];
    yaw(40, R);
    const float root[3] = {1000, 50, -300};
    const float pelvisUp = 95;  // in the game the pelvis stands above the root (an animated translation)
    struct E { float m[16]; uint8_t ok; int bone; };
    std::vector<E> es;
    std::vector<std::vector<float>> want(B.size(), std::vector<float>(3));
    // world rotation of each bone: the body's yaw, or (posed) forward kinematics with a random turn at every joint
    std::vector<std::vector<float>> WR(B.size(), std::vector<float>(R, R + 9));
    std::vector<int> order;
    for (size_t h = 0; h < B.size(); h++) order.push_back((int)h);
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        auto depth = [&](int x) { int d = 0; while (B[x].parent >= 0) { x = B[x].parent; d++; } return d; };
        return depth(a) < depth(b);
    });
    for (int h : order) {
        int p = B[h].parent;
        float w[3];
        bool freeB = p >= 0 && B[h].len < 0.5f && (B[p].id == 0 || B[p].id == 13);
        if (!posed || p < 0 || freeB) {
            float local[3] = {B[h].bind[0], B[h].bind[1] + (B[h].id == 0 ? 0.0f : pelvisUp), B[h].bind[2]};
            for (int i = 0; i < 3; i++) w[i] = root[i] + R[i * 3] * local[0] + R[i * 3 + 1] * local[1] + R[i * 3 + 2] * local[2];
        } else {
            const float* Rp = WR[p].data();
            for (int i = 0; i < 3; i++)
                w[i] = want[p][i] + Rp[i * 3] * B[h].local[0] + Rp[i * 3 + 1] * B[h].local[1] + Rp[i * 3 + 2] * B[h].local[2];
            // a turn of up to 50 degrees about a random axis
            float ax[3] = {U(rng), U(rng), U(rng)}, n = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]) + 1e-6f;
            for (float& v : ax) v /= n;
            float a = 0.87f * U(rng), c = cosf(a), sn = sinf(a), t = 1 - c;
            float L[9] = {t * ax[0] * ax[0] + c, t * ax[0] * ax[1] - sn * ax[2], t * ax[0] * ax[2] + sn * ax[1],
                          t * ax[0] * ax[1] + sn * ax[2], t * ax[1] * ax[1] + c, t * ax[1] * ax[2] - sn * ax[0],
                          t * ax[0] * ax[2] - sn * ax[1], t * ax[1] * ax[2] + sn * ax[0], t * ax[2] * ax[2] + c};
            m3_mul(Rp, L, WR[h].data());
        }
        want[h] = {w[0], w[1], w[2]};
    }
    for (size_t h = 0; h < B.size(); h++) {
        float w[3] = {want[h][0], want[h][1], want[h][2]};
        for (int copy = 0; copy < 3; copy++) {  // the armour pieces each carry a copy of the skeleton
            E e;
            game_matrix(WR[h].data(), w, e.m);
            e.ok = 1;
            e.bone = (int)h;
            es.push_back(e);
        }
    }
    for (int i = 0; i < 300; i++) {  // other models' joints around the hunter
        E e;
        float Rn[9], p[3] = {root[0] + 150 * U(rng), root[1] + 100 + 100 * U(rng), root[2] + 150 * U(rng)};
        yaw(180 * U(rng), Rn);
        game_matrix(Rn, p, e.m);
        e.ok = 1;
        e.bone = -1;
        es.push_back(e);
    }
    for (int i = 0; i < 6; i++) {
        E e{};
        e.ok = 0;
        e.bone = -1;
        es.push_back(e);
    }
    std::shuffle(es.begin(), es.end(), rng);
    std::vector<float> block(es.size() * 16);
    std::vector<uint8_t> ok(es.size());
    for (size_t i = 0; i < es.size(); i++) {
        memcpy(&block[i * 16], es[i].m, 64);
        ok[i] = es[i].ok;
    }
    HunterMap map;
    bool valid = hunter_match(block.data(), ok.data(), (int)es.size(), root, &map);
    int right = 0;
    for (size_t h = 0; h < B.size(); h++) {
        int e = map.entry[h];
        if (e < 0) continue;
        const float* p = &block[(size_t)e * 16 + 12];
        float d = sqrtf((p[0] - want[h][0]) * (p[0] - want[h][0]) + (p[1] - want[h][1]) * (p[1] - want[h][1]) +
                        (p[2] - want[h][2]) * (p[2] - want[h][2]));
        if (d < 0.5f) right++;
        else if (getenv("HUNTER_DEBUG")) printf("  bone %zu (id %d) entry %d (bone %d) off by %.1f\n", h, B[h].id, e, es[e].bone, d);
    }
    printf("match (%s): %d entries, %d of %zu bones matched, %d at the right place, %zu aliases\n", posed ? "posed" : "bind", (int)es.size(), map.matched,
           B.size(), right, map.aliases.size());
    CHECK(valid, "map not valid");
    CHECK(right >= 80, "only %d bones recovered", right);
}

// ---- 2. the retarget: Remnant's skeleton in the hunter's T-pose gives the hunter's bind pose
struct R2Bone { std::string name; int parent; float pos[3], q[4]; };

static bool load_r2(const char* path, std::vector<R2Bone>* out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    size_t at = 0;
    while ((at = s.find("\"name\"", at)) != std::string::npos) {
        R2Bone b;
        size_t q0 = s.find('"', s.find(':', at) + 1), q1 = s.find('"', q0 + 1);
        b.name = s.substr(q0 + 1, q1 - q0 - 1);
        const char* p = s.c_str() + s.find("\"parent\"", q1);
        b.parent = atoi(strchr(p, ':') + 1);
        p = strchr(s.c_str() + s.find("\"pos\"", q1), '[') + 1;
        for (float& v : b.pos) { v = strtof(p, (char**)&p); p = strchr(p, *p == ']' ? ']' : ',') + 1; }
        p = strchr(s.c_str() + s.find("\"rot\"", q1), '[') + 1;
        for (float& v : b.q) { v = strtof(p, (char**)&p); p = strchr(p, *p == ']' ? ']' : ',') + 1; }
        out->push_back(b);
        at = q1;
    }
    return !out->empty();
}

static void test_retarget(const char* r2path) {
    const auto& B = hunter_bones();
    std::vector<std::string> names;
    std::vector<float> G;
    std::vector<R2Bone> r2;
    bool real = load_r2(r2path, &r2);
    if (real) {
        G.resize(r2.size() * 12);
        for (size_t i = 0; i < r2.size(); i++) {
            names.push_back(r2[i].name);
            float x = r2[i].q[0], y = r2[i].q[1], z = r2[i].q[2], w = r2[i].q[3];
            float L[12] = {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), r2[i].pos[0],
                           2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), r2[i].pos[1],
                           2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), r2[i].pos[2]};
            float* o = &G[i * 12];
            int p = r2[i].parent;
            if (p < 0) { memcpy(o, L, sizeof(L)); continue; }
            const float* a = &G[(size_t)p * 12];
            for (int r = 0; r < 3; r++) {
                for (int c = 0; c < 3; c++) o[r * 4 + c] = a[r * 4] * L[c] + a[r * 4 + 1] * L[4 + c] + a[r * 4 + 2] * L[8 + c];
                o[r * 4 + 3] = a[r * 4] * L[3] + a[r * 4 + 1] * L[7] + a[r * 4 + 2] * L[11] + a[r * 4 + 3];
            }
        }
    } else {  // no Remnant data: a stand-in skeleton, the hunter's bind pose in Remnant's frame
        printf("retarget: %s not found, using a stand-in Remnant skeleton\n", r2path);
        for (const HBone& b : B) {
            if (b.r2.empty()) continue;
            names.push_back(b.r2);
            float m[12] = {1, 0, 0, b.bind[0], 0, 0, 1, b.bind[2], 0, 1, 0, b.bind[1] + 90};
            G.insert(G.end(), m, m + 12);
        }
    }
    HunterRig rig;
    bool ok = hunter_rig_build(names, G.data(), (int)names.size(), &rig);
    printf("retarget: rig %s, %d hunter bones mapped to Remnant bones\n", ok ? "ok" : "NOT ok", rig.mapped);
    CHECK(ok && rig.mapped >= 40, "rig not built (%d mapped)", rig.mapped);
    if (!ok) return;
    // the T-pose: every mapped Remnant bone turned by the rig's rest correction (C * A * C * R), positions unchanged
    std::vector<float> T = G;
    for (size_t h = 0; h < B.size(); h++) {
        if (rig.r2[h] < 0) continue;
        float AC[9], R[9];
        m3_mul(rig.A[h], rig.CR[h], AC);
        m3_mul(kC, AC, R);
        float* o = &T[(size_t)rig.r2[h] * 12];
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) o[r * 4 + c] = R[r * 3 + c];
    }
    float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, zero[3] = {0, 0, 0}, rootM[16], pelM[16];
    game_matrix(I, zero, rootM);
    game_matrix(I, zero, pelM);
    float out[kHunterBones][16];
    bool set[kHunterBones];
    hunter_retarget(rig, T.data(), G.data(), rootM, pelM, out, set);
    float worst = 0;
    int worstH = -1, written = 0;
    for (size_t h = 0; h < B.size(); h++) {
        float d = 0;
        for (int k = 0; k < 3; k++) d += (out[h][12 + k] - B[h].bind[k]) * (out[h][12 + k] - B[h].bind[k]);
        d = sqrtf(d);
        if (d > worst) { worst = d; worstH = (int)h; }
        written += set[h];
    }
    printf("retarget: T-pose -> hunter bind pose, worst error %.3f cm (bone %d), %d bones written\n", worst,
           worstH >= 0 ? B[worstH].id : -1, written);
    CHECK(worst < 1.0f, "T-pose retarget off by %.2f cm at bone id %d", worst, worstH >= 0 ? B[worstH].id : -1);
    if (real) {
        // Remnant's own reference pose (arms down): the hunter's arms come down too, and stay on their side.
        hunter_retarget(rig, G.data(), G.data(), rootM, pelM, out, set);
        int sh = hunter_bone_by_id(6), hl = hunter_bone_by_id(8), hr = hunter_bone_by_id(12);
        float drop = out[sh][13] - out[hl][13];
        printf("retarget: Remnant reference pose: left hand %.1f cm below the shoulder, x %.1f / %.1f (left / right)\n",
               drop, out[hl][12], out[hr][12]);
        CHECK(drop > 15, "arms did not come down (%.1f)", drop);
        CHECK(out[hl][12] > 10 && out[hr][12] < -10, "hands swapped sides");
    }
}

int main(int argc, char** argv) {
    test_match(false);
    test_match(true);
    test_retarget(argc > 1 ? argv[1] : "build/r2_player_bones.json");
    if (g_fail) printf("hunter: %d FAILED\n", g_fail);
    else printf("hunter: all passed\n");
    return g_fail ? 1 : 0;
}
