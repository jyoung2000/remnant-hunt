// Host test of the host-monster logic (host.cpp's pure part, -DRH_HOST_PURE): the facing quaternion, the clutch-claw
// latch detection with synthetic positions and actions (learning, latch, claw hits, let go, mount), the tenderise / mount
// timers, and the validators: transparency classes, the parts table, part -> hitbox sphere, em ids, .text RVAs, prologues.
// Build: tools/build_native.sh test. Needs no game.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "host.h"

using namespace rh;

static int g_fail = 0;
#define CHECK(c)                                                                   \
    do {                                                                           \
        if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } \
    } while (0)

static bool close_to(float a, float b, float e = 1e-3f) { return fabsf(a - b) < e; }
static float wrapf(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

int main() {
    // facing: unit quaternions about +y whose forward is the boss's (sin yaw, 0, cos yaw)
    for (int fwd : {1, -1, 0})
        for (float yaw = -3.0f; yaw <= 3.0f; yaw += 0.25f) {
            HostQuat q = host_quat_from_yaw(yaw, fwd);
            CHECK(close_to(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w, 1.0f));
            CHECK(close_to(q.x, 0) && close_to(q.z, 0));
            CHECK(close_to(wrapf(host_yaw_of_quat(q, fwd) - yaw), 0.0f, 1e-3f));
        }
    {
        HostQuat q = host_quat_from_yaw(0, 1);
        CHECK(close_to(q.w, 1) && close_to(q.y, 0));
        q = host_quat_from_yaw(0, -1);  // forward along -z: turned half way round
        CHECK(close_to(fabsf(q.y), 1) && close_to(q.w, 0));
    }

    // claw family
    CHECK(host_claw_family(3, 100, 3, 104));
    CHECK(!host_claw_family(3, 100, 3, 105));
    CHECK(!host_claw_family(3, 100, 2, 100));
    CHECK(!host_claw_family(-1, 0, 0, 0));

    // latch learning: nothing learned until an action change at the host within 1.5 s of a press
    HostLatch L;
    double t = 0;
    int ev = L.step(t, 5000, 0, 1, 10, false, false);
    CHECK(ev == 0);
    ev = L.step(t += 0.1, 200, 0, 1, 11, false, false);  // close, an action change, but no press
    CHECK(ev == 0 && L.clawSet < 0);
    ev = L.step(t += 0.1, 200, 0, 1, 11, true, false);    // the press
    CHECK(ev == 0);
    ev = L.step(t += 2.0, 200, 0, 2, 50, false, false);   // too late after the press
    CHECK(ev == 0 && L.clawSet < 0);
    L.step(t += 0.1, 200, 0, 2, 50, true, false);         // press again (no action change)
    ev = L.step(t += 0.5, 900, 0, 2, 60, false, false);   // far from the host: not the claw
    CHECK(ev == 0 && L.clawSet < 0);
    ev = L.step(t += 0.3, 240, 0, 3, 100, false, false);  // close, within the window: learned and latched
    CHECK((ev & HL_LEARNED) && (ev & HL_LATCH) && (ev & HL_CLAW_HIT));
    CHECK(L.clawSet == 3 && L.clawId == 100 && L.latched);
    // latched: moving through the claw family keeps it; host HP drops are claw hits, at most one per 0.8 s
    ev = L.step(t += 0.1, 100, 0, 3, 102, false, false);
    CHECK(ev == 0 && L.latched);
    ev = L.step(t += 0.1, 100, 0, 3, 102, false, true);
    CHECK(ev == 0);  // 0.2 s after the latch's own hit
    ev = L.step(t += 0.8, 100, 0, 3, 103, false, true);
    CHECK(ev == HL_CLAW_HIT);
    ev = L.step(t += 0.1, 100, 0, 3, 103, false, true);
    CHECK(ev == 0);
    // let go: an action outside the family
    ev = L.step(t += 0.1, 100, 0, 1, 10, false, false);
    CHECK((ev & HL_UNLATCH) && !L.latched);
    // once learned, a latch needs no press: the claw action at the host
    ev = L.step(t += 1.0, 230, 0, 3, 101, false, false);
    CHECK((ev & HL_LATCH) && L.latched);
    // too far while hanging on: let go
    ev = L.step(t += 0.1, 700, 0, 3, 101, false, false);
    CHECK((ev & HL_UNLATCH) && !L.latched);
    // the claw action far from any host: no latch
    L.step(t += 0.1, 5000, 0, 1, 10, false, false);
    ev = L.step(t += 0.1, 5000, 0, 3, 100, false, false);
    CHECK(ev == 0 && !L.latched);
    // invalid action reads keep the previous action
    ev = L.step(t += 0.1, 100, 0, -1, -1, false, false);
    CHECK(ev == 0 && L.prevSet == 3);

    // mount: on top of the host for a second, then not again for 20 s
    HostLatch M;
    t = 100;
    M.step(t, 100, 0, 1, 1, false, false);
    ev = M.step(t += 0.5, 50, 250, 1, 1, false, false);
    CHECK(ev == 0);
    ev = M.step(t += 0.6, 50, 250, 1, 1, false, false);
    CHECK(ev == 0);  // 0.6 s on top
    ev = M.step(t += 0.5, 50, 250, 1, 1, false, false);
    CHECK(ev == HL_MOUNT);
    ev = M.step(t += 2.0, 50, 250, 1, 1, false, false);
    CHECK(ev == 0);  // cooldown
    M.step(t += 0.1, 50, 0, 1, 1, false, false);           // down again
    ev = M.step(t += 20.0, 50, 260, 1, 1, false, false);
    ev |= M.step(t += 1.1, 50, 260, 1, 1, false, false);
    CHECK(ev == HL_MOUNT);
    ev = M.step(t += 30.0, 400, 260, 1, 1, false, false);  // above but not on it
    ev |= M.step(t += 1.5, 400, 260, 1, 1, false, false);
    CHECK(ev == 0);

    // timers
    HostBossFx fx;
    CHECK(fx.mult(4, 0) == 1.0f && !fx.mounted(0));
    fx.tenderise(4, 10);
    CHECK(fx.mult(4, 10) == 2.0f && fx.mult(5, 10) == 1.0f && fx.mult(-1, 10) == 1.0f);
    CHECK(fx.mult(4, 10 + kHostTenderS - 0.1) == 2.0f && fx.mult(4, 10 + kHostTenderS + 0.1) == 1.0f);
    fx.tenderise(7, 50);  // a new claw hit moves the spot
    CHECK(fx.mult(4, 51) == 1.0f && fx.mult(7, 51) == 2.0f);
    fx.mount(200);
    CHECK(fx.mounted(200) && fx.mounted(200 + kHostMountS - 0.1) && !fx.mounted(200 + kHostMountS + 0.1));

    // transparency value classes
    auto bitsOf = [](float f) { uint32_t b; memcpy(&b, &f, 4); return b; };
    CHECK(host_alpha_class(0) == HOST_ALPHA_FLOAT);
    CHECK(host_alpha_class(bitsOf(1.0f)) == HOST_ALPHA_FLOAT);
    CHECK(host_alpha_class(bitsOf(0.5f)) == HOST_ALPHA_FLOAT);
    CHECK(host_alpha_class(bitsOf(2.0f)) == HOST_ALPHA_NONE);
    CHECK(host_alpha_class(bitsOf(-1.0f)) == HOST_ALPHA_NONE);
    CHECK(host_alpha_class(bitsOf(NAN)) == HOST_ALPHA_NONE);
    CHECK(host_alpha_class(0xFF) == HOST_ALPHA_BYTE && host_alpha_class(0x80) == HOST_ALPHA_BYTE && host_alpha_class(1) == HOST_ALPHA_BYTE);
    CHECK(host_alpha_class(0x100) == HOST_ALPHA_NONE);       // a word, not a byte
    CHECK(host_alpha_class(0x12345678) == HOST_ALPHA_NONE);  // a tiny float: not an alpha
    CHECK(host_alpha_class(0xCDCDCDCD) == HOST_ALPHA_NONE);

    // parts table on synthetic bytes
    {
        std::vector<uint8_t> tb(kHostPartsAt + kHostMaxParts * kHostPartStride, 0);
        auto setp = [&](int i, float mx, float hp, int br) {
            size_t o = kHostPartsAt + (size_t)i * kHostPartStride;
            memcpy(&tb[o + 0x0C], &mx, 4);
            memcpy(&tb[o + 0x10], &hp, 4);
            memcpy(&tb[o + 0x18], &br, 4);
        };
        HostPart out[kHostMaxParts];
        CHECK(host_parts_read(tb.data(), tb.size(), out, kHostMaxParts) == 0);  // all zero: no table
        setp(0, 500, 320, 0);
        setp(1, 800, 800, 1);
        setp(2, 300, -10, 2);  // just broken, below zero
        CHECK(host_parts_read(tb.data(), tb.size(), out, kHostMaxParts) == 3);
        CHECK(close_to(out[0].maxHp, 500) && close_to(out[0].hp, 320) && out[1].breaks == 1 && out[2].breaks == 2);
        setp(3, 400, 401, 0);  // current above max: stops there
        CHECK(host_parts_read(tb.data(), tb.size(), out, kHostMaxParts) == 3);
        setp(3, 400, 100, 500);  // a count that is no break count
        CHECK(host_parts_read(tb.data(), tb.size(), out, kHostMaxParts) == 3);
        setp(3, 400, 100, 0);
        CHECK(host_parts_read(tb.data(), tb.size(), out, kHostMaxParts) == 4);
        CHECK(host_parts_read(tb.data(), tb.size(), out, 2) == 2);
        CHECK(host_parts_read(tb.data(), kHostPartsAt + kHostPartStride + 0x10, out, kHostMaxParts) == 1);  // short copy
        setp(0, NAN, 0, 0);
        CHECK(host_parts_read(tb.data(), tb.size(), out, kHostMaxParts) == 0);
        CHECK(host_parts_read(nullptr, 0, out, kHostMaxParts) == 0);
    }
    // part -> hitbox sphere by size
    {
        const float r[4] = {20, 80, 50, 80};
        CHECK(host_part_cluster(0, r, 4) == 1 && host_part_cluster(1, r, 4) == 3);  // ties keep their order
        CHECK(host_part_cluster(2, r, 4) == 2 && host_part_cluster(3, r, 4) == 0 && host_part_cluster(4, r, 4) == 1);
        CHECK(host_part_cluster(0, r, 0) == -1 && host_part_cluster(-1, r, 4) == -1);
    }
    // em ids
    {
        int id = -1, sub = -1;
        CHECK(host_parse_em("em001_00", &id, &sub) && id == 1 && sub == 0);
        CHECK(host_parse_em("em113_01", &id, &sub) && id == 113 && sub == 1);
        CHECK(!host_parse_em("em001", &id, &sub) && !host_parse_em("ems01_00", &id, &sub) && !host_parse_em("em001_00x", &id, &sub));
        CHECK(!host_parse_em("", &id, &sub) && !host_parse_em(nullptr, &id, &sub) && !host_parse_em("em000_00", &id, &sub));
    }
    // RVA inside .text, with a fake PE header
    {
        std::vector<uint8_t> img(0x400, 0);
        img[0] = 'M';
        img[1] = 'Z';
        uint32_t lfa = 0x80;
        memcpy(&img[0x3C], &lfa, 4);
        memcpy(&img[lfa], "PE\0\0", 4);
        uint16_t ns = 2, so = 0xF0;
        memcpy(&img[lfa + 6], &ns, 2);
        memcpy(&img[lfa + 20], &so, 2);
        size_t sec = lfa + 24 + so;
        auto setsec = [&](int i, const char* name, uint32_t va, uint32_t vs) {
            memcpy(&img[sec + i * 40], name, strlen(name));
            memcpy(&img[sec + i * 40 + 8], &vs, 4);
            memcpy(&img[sec + i * 40 + 12], &va, 4);
        };
        setsec(0, ".bind", 0x1000, 0x500);
        setsec(1, ".text", 0x2000, 0x2E11200);
        CHECK(host_rva_in_text(img.data(), img.size(), 0x2000));
        CHECK(host_rva_in_text(img.data(), img.size(), 0x1A5A3E0));
        CHECK(!host_rva_in_text(img.data(), img.size(), 0x1FFF));
        CHECK(!host_rva_in_text(img.data(), img.size(), 0x2000 + 0x2E11200));
        CHECK(!host_rva_in_text(img.data(), img.size(), 0x1200));  // .bind
        CHECK(!host_rva_in_text(img.data(), 0x100, 0x2000));      // headers cut short
        img[0] = 'X';
        CHECK(!host_rva_in_text(img.data(), img.size(), 0x2000));
    }
    // prologues
    {
        const uint8_t a[] = {0x48, 0x89, 0x5C, 0x24, 0x08}, b[] = {0x4C, 0x89, 0x44, 0x24, 0x18}, c[] = {0x40, 0x53, 0x48, 0x83, 0xEC};
        const uint8_t d[] = {0xCC, 0xCC, 0xCC, 0xCC, 0xCC}, e[] = {0xC3, 0x90, 0x90, 0x90, 0x90}, f[] = {0x41, 0x56, 0x48, 0x83, 0xEC};
        CHECK(host_looks_like_prologue(a, 5) && host_looks_like_prologue(b, 5) && host_looks_like_prologue(c, 5) && host_looks_like_prologue(f, 5));
        CHECK(!host_looks_like_prologue(d, 5) && !host_looks_like_prologue(e, 5) && !host_looks_like_prologue(a, 3));
    }

    if (g_fail) { printf("host: %d checks failed\n", g_fail); return 1; }
    printf("host: all passed\n");
    return 0;
}
