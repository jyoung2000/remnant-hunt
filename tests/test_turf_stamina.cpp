// Host test (a Windows build, run under Wine on Linux): the turf wars and the stamina bar on fake game data.
//  - stamina: MHW's stamina hidden among noise in a fake hunter object is found from dodges and refills, and the
//    Remnant bonuses then act on it (cheaper spending, faster refill, more max stamina);
//  - turf: a monster's facing is turned towards a point through its rotation quaternion, and two fake MHW monsters
//    that meet stage a turf war with their learned moves, losing health to each other.
// build and run: make test-logic
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bosses.h"

namespace rh {
static char g_dir[MAX_PATH];
const char* mod_dir() { return g_dir; }
void log(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    printf("  log: ");
    vprintf(fmt, a);
    printf("\n");
    va_end(a);
}
static uint8_t* g_player;
uint8_t* player_ptr() { return g_player; }
static double g_t;
double now_s() { return g_t; }
void toast(Live&, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    printf("  toast: ");
    vprintf(fmt, a);
    printf("\n");
    va_end(a);
}
bool add_hp(uint8_t* h, float d) { *(float*)(h + 0x64) += d; return true; }
bool is_boss(const uint8_t*) { return false; }
double reactions_own_damage_at() { return -100; }
static int g_driven;
bool mhw_do_action(uint8_t* unit, int set, int id) {
    g_driven++;
    *(int*)(unit + 0x61C8 + 0xAC) = set;
    *(int*)(unit + 0x61C8 + 0xB0) = id;
    return true;
}
}  // namespace rh

using namespace rh;
static int g_fail = 0;
#define EXPECT(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_boss_token_identity() {
    uint8_t tokens[4] = {};
    uint8_t gameEntity[4] = {};
    EXPECT(rh::boss_token_index(nullptr, tokens, 4) == -1, "null entity was accepted as a boss token");
    EXPECT(rh::boss_token_index(tokens, nullptr, 4) == -1, "null token storage was accepted");
    for (int i = 0; i < 4; ++i)
        EXPECT(rh::boss_token_index(&tokens[i], tokens, 4) == i, "boss token %d did not retain its exact index", i);
    EXPECT(rh::boss_token_index(gameEntity, tokens, 4) == -1, "ordinary game entity was mistaken for a boss token");
    EXPECT(rh::boss_token_index(tokens + 4, tokens, 4) == -1, "one-past token pointer was accepted as a boss");
}

static float yaw_of(const uint8_t* em) {
    const float* q = (const float*)(em + 0x170);
    // forward +z rotated by q
    float x = 2 * (q[0] * q[2] + q[3] * q[1]), z = 1 - 2 * (q[0] * q[0] + q[1] * q[1]);
    return atan2f(x, z) * 180.0f / 3.14159265f;
}

static void test_stamina() {
    printf("stamina\n");
    const int kCur = 0x7A40, kMax = 0x7A44;
    g_player = (uint8_t*)VirtualAlloc(nullptr, 0x20000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    float* f = (float*)g_player;
    for (int i = 0; i < 0x8000; i++) f[i] = (float)(rand() % 1000) * 0.37f;  // noise, much of it out of range
    Live l;
    l.snap.inGame = true;
    l.snap.inBase = false;
    float* cur = (float*)(g_player + kCur);
    float* max = (float*)(g_player + kMax);
    *max = 100;
    *cur = 100;
    double nextDodge = 1;
    // the game: dodges every 3 s cost 25, refills at 30/s up to its max; some timers tick in range too
    auto game = [&](float dt) {
        if (g_t >= nextDodge) { *cur = std::max(0.0f, *cur - 25); nextDodge = g_t + 3; }
        else *cur = std::min(*max, *cur + 30 * dt);
        for (int i = 0x100; i < 0x140; i++) f[i] = fmodf(f[i] + 13.0f * dt, 100.0f);  // timers that wrap
        for (int i = 0x2000; i < 0x2010; i++) f[i] = (float)(rand() % 140);         // jitter in range
    };
    for (g_t = 0; g_t < 40 && !stamina_known(); g_t += 1.0 / 60) {
        game(1.0f / 60);
        stamina_tick(l, g_t);
    }
    EXPECT(stamina_known(), "stamina not found after 40 s");
    // the bonuses: stamina costs halved, refill half again as fast, +20 max
    l.fx.add_source("stamina_cost-50%;stamina_regen+50%;stamina_max+20");
    for (double end = g_t + 2; g_t < end; g_t += 1.0 / 60) { stamina_tick(l, g_t); game(1.0f / 60); }
    EXPECT(fabsf(*max - 120) < 0.01f, "max stamina %.1f, expected 120", *max);
    // a dodge now costs half
    while (*cur < *max - 0.01f) { g_t += 1.0 / 60; game(1.0f / 60); stamina_tick(l, g_t); }
    nextDodge = g_t;
    float before = *cur;
    g_t += 1.0 / 60;
    game(1.0f / 60);
    stamina_tick(l, g_t);
    EXPECT(fabsf((before - *cur) - 12.5f) < 0.6f, "a dodge cost %.1f, expected 12.5", before - *cur);
    // and refills at 45/s
    float a = *cur;
    for (int k = 0; k < 12; k++) { g_t += 1.0 / 60; game(1.0f / 60); stamina_tick(l, g_t); }
    EXPECT(fabsf((*cur - a) - 9.0f) < 0.8f, "refilled %.1f in 0.2 s, expected 9", *cur - a);
    // bonuses gone: MHW's own max comes back
    l.fx.clear_sources();
    g_t += 1.0 / 60;
    stamina_tick(l, g_t);
    EXPECT(fabsf(*max - 100) < 0.01f, "max stamina %.1f after the bonus is gone, expected 100", *max);
}

struct FakeMonster {
    uint8_t* em;
    uint8_t* health;
    Monster m;
};

static FakeMonster make_monster(const char* kind, Vec3 at, float hp) {
    FakeMonster fm{};
    fm.em = (uint8_t*)VirtualAlloc(nullptr, 0x20000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    fm.health = (uint8_t*)VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    *(float*)(fm.health + 0x60) = hp;
    *(float*)(fm.health + 0x64) = hp;
    float q[4] = {0, 0, 0, 1};  // facing +z
    memcpy(fm.em + 0x170, q, 16);
    *(int*)(fm.em + 0x61C8 + 0xAC) = 1;
    *(int*)(fm.em + 0x61C8 + 0xB0) = 3;
    Monster& m = fm.m;
    m.em = fm.em;
    m.health = fm.health;
    m.hp = m.maxHp = hp;
    m.large = true;
    m.obbOk = true;
    float rot[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    memcpy(m.rot, rot, sizeof(rot));
    m.center = at;
    m.half = {300, 250, 500};
    strcpy(m.kind, kind);
    return fm;
}

static void test_turf() {
    printf("turf\n");
    FILE* f = fopen((std::string(g_dir) + "\\learned_turf.txt").c_str(), "wb");
    fprintf(f, "forward 1\nattack em001_00 1 20 5 0.80 600\nattack em002_00 1 21 5 1.10 700\nflinch em001_00 1 40 5\n"
               "flinch em002_00 1 41 5\n");
    fclose(f);
    FakeMonster a = make_monster("em001_00", {0, 0, 0}, 20000), b = make_monster("em002_00", {1400, 0, 0}, 24000);
    Live l;
    l.snap.inGame = true;
    l.snap.inBase = false;
    l.snap.zone = 101;
    l.snap.pos = {900, 0, -2500};
    l.snap.playerHealth = nullptr;
    int ids = 0;
    auto tick = [&](float dt) {
        g_t += dt;
        // MHW's AI: the monsters change actions now and then
        if (fmod(g_t, 0.5) < dt) {
            for (FakeMonster* m : {&a, &b}) *(int*)(m->em + 0x61C8 + 0xB0) = 3 + (ids++ % 7);
        }
        a.m.hp = *(float*)(a.health + 0x64);
        b.m.hp = *(float*)(b.health + 0x64);
        l.snap.monsters = {a.m, b.m};
        turf_tick(l, g_t, dt);
    };
    // facing: monster a turns to face b (+x is 90 degrees) at turf_turn_dps
    for (int k = 0; k < 60; k++) tick(1.0f / 60);  // the checks: actions and rotation read sanely
    bool facing = false;
    for (int k = 0; k < 120 && !facing; k++) {
        facing = turf_face(a.m, b.m.center, 1.0f / 60) && fabsf(yaw_of(a.em) - 90) < 35;
        tick(1.0f / 60);
    }
    EXPECT(fabsf(yaw_of(a.em) - 90) < 35, "monster a's yaw %.0f, expected about 90", yaw_of(a.em));
    // a turf war between the two: they meet near the hunter; within a few minutes one is staged and fought
    float hpA = *(float*)(a.health + 0x64), hpB = *(float*)(b.health + 0x64);
    int driven = g_driven;
    for (int k = 0; k < 60 * 600; k++) tick(1.0f / 60);
    float lostA = hpA - *(float*)(a.health + 0x64), lostB = hpB - *(float*)(b.health + 0x64);
    printf("  monster a lost %.0f, b lost %.0f, %d moves played\n", lostA, lostB, g_driven - driven);
    EXPECT(lostA > 0 && lostB > 0, "both should be hurt in a turf war");
    EXPECT(g_driven - driven >= 4, "their learned moves should be played");
}

// Learning by watching: the move a monster is in when it hits the hunter, and the one it switches to when hurt.
static void test_learning() {
    printf("learning\n");
    FakeMonster m = make_monster("em010_00", {0, 0, 300}, 18000);
    uint8_t* hunterHealth = (uint8_t*)VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    *(float*)(hunterHealth + 0x60) = 150;
    *(float*)(hunterHealth + 0x64) = 150;
    Live l;
    l.snap.inGame = true;
    l.snap.zone = 102;
    l.snap.pos = {0, 0, -300};
    l.snap.playerHealth = hunterHealth;
    auto tick = [&](float dt) {
        g_t += dt;
        m.m.hp = *(float*)(m.health + 0x64);
        l.snap.monsters = {m.m};
        turf_tick(l, g_t, dt);
    };
    auto act = [&](int id) { *(int*)(m.em + 0x61C8 + 0xB0) = id; };
    for (int k = 0; k < 2; k++) {  // a bite that hits the hunter 0.4 s in, twice
        act(50);
        for (int i = 0; i < 24; i++) tick(1.0f / 60);
        *(float*)(hunterHealth + 0x64) -= 30;
        tick(1.0f / 60);
        act(3);
        for (int i = 0; i < 60; i++) tick(1.0f / 60);
        *(float*)(hunterHealth + 0x64) = 150;
        tick(1.0f / 60);
    }
    for (int k = 0; k < 3; k++) {  // a heavy hit from the hunter's weapon, and the monster recoils
        *(float*)(m.health + 0x64) -= 900;
        tick(1.0f / 60);
        act(60);
        for (int i = 0; i < 60; i++) tick(1.0f / 60);
        act(3);
        for (int i = 0; i < 30; i++) tick(1.0f / 60);
    }
    for (int i = 0; i < 60 * 6; i++) tick(1.0f / 60);  // saved within a few seconds
    std::string text;
    if (FILE* f = fopen((std::string(g_dir) + "\\learned_turf.txt").c_str(), "rb")) {
        char b[4096];
        size_t n = fread(b, 1, sizeof(b) - 1, f);
        b[n] = 0;
        text = b;
        fclose(f);
    }
    EXPECT(text.find("attack em010_00 1 50 2 0.4") != std::string::npos, "the bite was not learned:\n%s", text.c_str());
    EXPECT(text.find("flinch em010_00 1 60 3") != std::string::npos, "the flinch was not learned:\n%s", text.c_str());
}

int main() {
    test_boss_token_identity();
    GetTempPathA(MAX_PATH, g_dir);
    size_t n = strlen(g_dir);
    if (n && g_dir[n - 1] == '\\') g_dir[n - 1] = 0;
    srand(7);
    DeleteFileA((std::string(g_dir) + "\\learned_stamina.txt").c_str());
    test_stamina();
    test_turf();
    test_learning();
    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
