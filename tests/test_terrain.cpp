// Host test of the bosses' terrain geometry (bosses.cpp, built alone with -DRH_TERRAIN_ONLY): a depth frame rendered
// on the CPU from a known slope with a rock on it, then the probe that reads it back as the ground (unproject and the
// re-projection at the found height), its refusals (off screen, sky, beyond 150 m, an object in front), the median of
// the foot probes and the step rules. The Direct3D copy of MHW's depth itself cannot run here.
// build and run: make test-logic (or tools/build_native.sh test)
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core.h"

using namespace rh;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                   \
    do {                                                                           \
        g_checks++;                                                                \
        if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } \
    } while (0)

// The world: a slope rising 25 cm per metre along x, a 50 cm wave along z, and a rock (a box 8 m tall).
static float terrain_y(float x, float z) { return 0.25f * x + 50.0f * sinf(z / 1000.0f); }
static bool in_rock(Vec3 p) {
    return p.x > -300 && p.x < 300 && p.z > 1500 && p.z < 1800 && p.y < terrain_y(p.x, p.z) + 800;
}
static bool solid(Vec3 p) {
    if (fabsf(p.x) > 12000 || p.z < -5000 || p.z > 30000) return false;  // the world ends: sky beyond
    return p.y < terrain_y(p.x, p.z) || in_rock(p);
}

static Vec3 v3sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static float dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 cross3(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static Vec3 norm3(Vec3 a) {
    float l = sqrtf(dot3(a, a));
    return {a.x / l, a.y / l, a.z / l};
}

// A left-handed look-at camera with a perspective projection (reversed z or not), as row-vector matrices.
static void camera(Vec3 eye, Vec3 at, bool reversed, float view[16], float proj[16]) {
    Vec3 za = norm3(v3sub(at, eye)), xa = norm3(cross3({0, 1, 0}, za)), ya = cross3(za, xa);
    float v[16] = {xa.x, ya.x, za.x, 0, xa.y, ya.y, za.y, 0, xa.z, ya.z, za.z, 0,
                   -dot3(xa, eye), -dot3(ya, eye), -dot3(za, eye), 1};
    memcpy(view, v, sizeof(v));
    float n = 10, f = 100000, ys = 1.0f / tanf(0.5f * 0.9f), xs = ys / (16.0f / 9.0f);
    if (reversed) std::swap(n, f);
    float p[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, f / (f - n), 1, 0, 0, -n * f / (f - n), 0};
    memcpy(proj, p, sizeof(p));
}

// Ray-marches every cell of the frame against the world, as MHW's depth buffer would hold it.
static DepthFrame render(Vec3 eye, Vec3 at, bool reversed) {
    DepthFrame f;
    float view[16], proj[16];
    camera(eye, at, reversed, view, proj);
    mat4_mul(view, proj, f.viewProj);
    mat4_inverse(f.viewProj, f.invViewProj);
    f.eye = eye;
    f.fullW = 640;
    f.fullH = 360;
    f.w = (f.fullW + 3) / 4;
    f.h = (f.fullH + 3) / 4;
    f.ndcZ.assign((size_t)f.w * f.h, NAN);
    for (int cy = 0; cy < f.h; cy++)
        for (int cx = 0; cx < f.w; cx++) {
            float nx = (std::min(cx * 4 + 2, f.fullW - 1) + 0.5f) / f.fullW, ny = (std::min(cy * 4 + 2, f.fullH - 1) + 0.5f) / f.fullH;
            Vec3 a = depth_unproject(f.invViewProj, nx, ny, reversed ? 0.9f : 0.1f);
            Vec3 dir = norm3(v3sub(a, eye));
            float t0 = 0, t1 = -1;
            for (float t = 0; t < 40000; t += 20)
                if (solid({eye.x + dir.x * t, eye.y + dir.y * t, eye.z + dir.z * t})) { t1 = t; break; }
            if (t1 < 0) continue;  // sky
            t0 = t1 - 20;
            for (int i = 0; i < 30; i++) {
                float tm = 0.5f * (t0 + t1);
                (solid({eye.x + dir.x * tm, eye.y + dir.y * tm, eye.z + dir.z * tm}) ? t1 : t0) = tm;
            }
            Vec3 h = {eye.x + dir.x * t1, eye.y + dir.y * t1, eye.z + dir.z * t1};
            float px, py, z;
            depth_project(f.viewProj, h, &px, &py, &z);
            f.ndcZ[(size_t)cy * f.w + cx] = z;
        }
    return f;
}

static void test_matrices() {
    float view[16], proj[16], vp[16], inv[16], id[16];
    camera({100, 1500, -3000}, {0, 0, 2000}, true, view, proj);
    mat4_mul(view, proj, vp);
    CHECK(mat4_inverse(vp, inv));
    mat4_mul(vp, inv, id);
    float err = 0;
    for (int i = 0; i < 16; i++) err = std::max(err, fabsf(id[i] - (i % 5 == 0 ? 1.0f : 0.0f)));
    CHECK(err < 1e-3f);
    float sing[16] = {};
    CHECK(!mat4_inverse(sing, inv));
    mat4_inverse(vp, inv);
    Vec3 p = {250, 120, 1800};
    float nx, ny, z;
    CHECK(depth_project(vp, p, &nx, &ny, &z));
    Vec3 q = depth_unproject(inv, nx, ny, z);
    CHECK(fabsf(q.x - p.x) < 2 && fabsf(q.y - p.y) < 2 && fabsf(q.z - p.z) < 2);
    CHECK(!depth_project(vp, {100, 1500, -6000}, &nx, &ny, &z));  // behind the camera
}

static void test_probe(bool reversed) {
    Vec3 eye = {0, 1800, -3000};
    DepthFrame f = render(eye, {0, 0, 2500}, reversed);
    Vec3 h;
    // On the slope, from a guess 4 m too high and 3 m too low: the height under the point, not along the ray.
    const float xs[3] = {-1500, 600, 2000}, zs[3] = {1000, 3000, 4500};
    for (int i = 0; i < 3; i++) {
        float truth = terrain_y(xs[i], zs[i]);
        for (float off : {400.0f, -300.0f, 0.0f}) {
            ProbeResult r = depth_probe(f, xs[i], zs[i], truth + off, &h);
            CHECK(r == PROBE_OK);
            CHECK(fabsf(h.y - truth) < 25);
            if (fabsf(h.y - truth) >= 25) printf("  probe (%.0f, %.0f) guess %+.0f: %.1f vs %.1f\n", xs[i], zs[i], off, h.y, truth);
        }
    }
    // Off screen: behind the camera, and far to the side.
    CHECK(depth_probe(f, 0, -5000, 0, &h) == PROBE_OFFSCREEN);
    CHECK(depth_probe(f, 9000, 1000, 0, &h) == PROBE_OFFSCREEN);
    // Sky: past the world's end the depth is the far plane.
    CHECK(depth_probe(f, 0, 33000, 4000, &h) == PROBE_SKY);
    // Beyond 150 m.
    CHECK(depth_probe(f, 0, 17000, terrain_y(0, 17000), &h) == PROBE_FAR);
    // Behind the rock (seen from the camera): the depth there is the rock, nearer the camera.
    ProbeResult r = depth_probe(f, 0, 2300, terrain_y(0, 2300), &h);
    CHECK(r == PROBE_OCCLUDED);
    CHECK(h.z < 1850 && h.y > terrain_y(0, 1600) + 100);
    // On top of the rock (guessed near its top, as a boss standing on it would): its top is the ground there.
    r = depth_probe(f, 0, 1650, terrain_y(0, 1650) + 700, &h);
    CHECK(r == PROBE_OK && fabsf(h.y - (terrain_y(0, 1650) + 800)) < 40);
}

static void test_median_steps() {
    float a[3] = {5, 1, 3}, b[4] = {4, 1, 3, 2}, c[5] = {100, 102, 101, 900, 99};
    CHECK(median_of(a, 3) == 3);
    CHECK(median_of(b, 4) == 2.5f);
    CHECK(median_of(c, 5) == 101);  // one probe on a rock does not lift the boss
    CHECK(step_verdict(0, true, 50, 100, false) == STEP_OK);
    CHECK(step_verdict(0, true, 120 + 70 + 1, 100, false) == STEP_WALL);  // over 120 cm + 35 degrees over 1 m
    CHECK(step_verdict(0, true, 180, 100, false) == STEP_OK);           // a 35 degree slope is climbed
    CHECK(step_verdict(0, true, -399, 100, false) == STEP_OK);
    CHECK(step_verdict(0, true, -401, 100, false) == STEP_CLIFF);       // over 300 cm + 45 degrees
    CHECK(step_verdict(0, false, 5000, 100, false) == STEP_OK);         // unknown: no verdict
    CHECK(step_verdict(0, false, 0, 100, true) == STEP_WALL);           // an object standing there
}

int main() {
    test_matrices();
    test_probe(true);
    test_probe(false);
    test_median_steps();
    printf("terrain: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
