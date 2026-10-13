// Remnant x MHW mashup core: Monster Hunter: World 15.23.00 (build 421810) game layer. MIT License (see LICENSE).
//
// Addresses come from the hooks sheet (sheets/hooks.json -> gen/sheets.gen.h), which credits where each was found
// (justbustin/minecraft-crossover-bridge, MIT; HunterPie, Apache-2.0). Nothing here patches MHW's code: the mod
// reads memory, calls the game's own functions, and gets a per-frame game-thread callback by pointing the sMhCamera
// object at a copy of its vtable with one slot replaced. MHW's code integrity checks are never touched.
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core.h"

using namespace sheets;

namespace rh {

// ---------------------------------------------------------------- hook verification

static bool g_hookOk[kHooksCount];
static bool g_hookLogged[kHooksCount];

bool hook_ok(int row) { return row >= 0 && row < kHooksCount && g_hookOk[row]; }

static void mark(int row, bool ok, const char* why) {
    g_hookOk[row] = ok;
    if (!g_hookLogged[row] || !ok) {
        log("hook %s %s (%s)", ok ? "ok" : "FAIL", kHooks[row].id, why);
        g_hookLogged[row] = ok;
    }
}

static const char* after(const char* s, const char* prefix) {
    size_t n = strlen(prefix);
    return strncmp(s, prefix, n) == 0 ? s + n : nullptr;
}

// MHW's .text is encrypted on disk and decrypted by the Steam stub at startup: wait for real code.
bool wait_for_game_code(int timeoutMs) {
    for (int t = 0; t < timeoutMs; t += 250) {
        if (sig_match(HOOKV_FIND_MASTER_PLAYER, after(kHooks[HOOKS_FIND_MASTER_PLAYER].check, "sig:"))) return true;
        Sleep(250);
    }
    return false;
}

int check_hooks() {
    int failed = 0;
    for (int i = 0; i < kHooksCount; i++) {
        const Hooks& h = kHooks[i];
        bool ok = true;
        const char* why = "checked at use";
        if (const char* sig = after(h.check, "sig:")) {
            uintptr_t a = strtoull(h.value, nullptr, 16);
            ok = sig_match(a, sig);
            why = "code signature";
        } else if (h.kind == HOOKS_KIND_STATIC_PTR) {
            uintptr_t a = strtoull(h.value, nullptr, 16);
            ok = mem_readable((void*)a, 8);
            why = "static readable";
            if (const char* vt = after(h.check, "vt:")) {
                uint8_t* obj = rd_ptr(a);
                if (obj) {
                    ok = *(uintptr_t*)obj == strtoull(vt, nullptr, 16);
                    why = "object vtable";
                } else {
                    why = "object not created yet";
                }
            }
        } else if (h.kind == HOOKS_KIND_VTABLE_SLOT) {
            const char* hex = strstr(h.base, "0x");
            uintptr_t vt = hex ? strtoull(hex, nullptr, 16) : 0;
            uintptr_t fn = 0;
            ok = rd(vt + 8 * strtoul(h.value, nullptr, 10), &fn) && sig_match(fn, "48 8B C4 48 89 58 20 55");
            why = "vtable slot target";
        } else {
            ok = true;  // offsets and Win32/DXGI items are validated where they are used (logged then)
        }
        mark(i, ok, why);
        if (!ok) failed++;
    }
    return failed;
}

// Validates an offset hook the first time it is used successfully.
static void used_ok(int row, const char* why) {
    if (!g_hookLogged[row]) mark(row, true, why);
}

// ---------------------------------------------------------------- game functions

typedef void*(__fastcall* FindMasterPlayer_t)(void* sPlayer);
typedef void(__fastcall* AddHP_t)(void* healthManager, float delta);
typedef void(__fastcall* ShowDamage_t)(void* target, int damage, float* pos, int a4, int a5, int a6, int a7,
                                       uint8_t a8, int a9);
typedef void*(__fastcall* CreateShell_t)(void* shellParam, void* owner, void* creator, void* params);
typedef int(__fastcall* CheckSegment_t)(void* coll, const float* seg, uint8_t flag, void* tri, void* param);
typedef void(__fastcall* Obj_t)(void* obj);
typedef void(__fastcall* ParamCtor_t)(void* p, uint32_t a, uint32_t b, uint32_t c, uint32_t s20, uint32_t s28,
                                      uint64_t s30, uint8_t s38, uint32_t s40, uint32_t s48, uint8_t s50);

constexpr size_t kHealthMax = 0x60, kHealthCur = 0x64;
constexpr size_t kEnemySlots = 0x38;
constexpr int kEnemySlotCount = 128;
constexpr size_t kAiEnemy = 0x138, kUnitState = 0x0C, kEnemyAi = 0x12278;
constexpr size_t kObbRot = 0x3A0, kObbCenter = 0x3D0, kObbHalf = 0x3E0;
constexpr size_t kViewport0 = 0x50, kVp0Camera = 0x58, kVpView = 0xA0, kVpProj = 0xE0;
constexpr size_t kCamFov = 0x144, kCamPos = 0x150, kCamTarget = 0x170;

static uint8_t* g_player = nullptr;  // refreshed each game tick

static uint8_t* find_player() {
    if (!hook_ok(HOOKS_FIND_MASTER_PLAYER)) return nullptr;
    uint8_t* sp = rd_ptr(HOOKV_S_PLAYER);
    if (!sp) return nullptr;
    return (uint8_t*)((FindMasterPlayer_t)HOOKV_FIND_MASTER_PLAYER)(sp);
}

static bool health_of(uint8_t* owner, size_t off, uint8_t** hm, float* cur, float* max) {
    *hm = rd_ptr((uintptr_t)owner + off);
    if (!*hm || !mem_readable(*hm + kHealthMax, 8)) return false;
    static const uintptr_t kHealthVt = strtoull(after(kHooks[HOOKS_PLAYER_HEALTH_MGR].check, "vt:"), nullptr, 16);
    uintptr_t vt = 0;
    rd((uintptr_t)*hm, &vt);
    if (vt != kHealthVt) return false;  // cpHealthManager (hooks sheet)
    *max = *(float*)(*hm + kHealthMax);
    *cur = *(float*)(*hm + kHealthCur);
    return std::isfinite(*cur) && std::isfinite(*max) && *max > 0;
}

bool zone_is_base(int z) { return z == 301 || z == 302 || z == 303 || z == 305 || z == 306 || (z >= 501 && z <= 506); }

void snapshot(GameSnapshot* s) {
    *s = GameSnapshot();
    uint8_t* sp = rd_ptr(HOOKV_S_PLAYER);
    if (sp) {
        int z = -1;
        if (rd((uintptr_t)sp + HOOKV_PLAYER_ZONE, &z) && z >= 0 && z < 1000) {
            s->zone = z;
            used_ok(HOOKS_PLAYER_ZONE, "zone id in range");
        }
    }
    s->inBase = zone_is_base(s->zone);
    g_player = find_player();
    uint8_t* pl = g_player;
    if (pl && mem_readable(pl + HOOKV_PLAYER_POS, 12)) {
        s->inGame = true;
        s->player = pl;
        memcpy(&s->pos, pl + HOOKV_PLAYER_POS, 12);
        if (health_of(pl, HOOKV_PLAYER_HEALTH_MGR, &s->playerHealth, &s->hp, &s->maxHp))
            used_ok(HOOKS_PLAYER_HEALTH_MGR, "cpHealthManager vtable");
    }
    // Party size (HunterPie PARTY offsets): unverified meaning until seen in game; treat as solo unless clearly >1.
    s->solo = true;
    static const uintptr_t kPartyBase = strtoull(kHooks[HOOKS_PARTY_COUNT].base, nullptr, 16);
    uint8_t* party = rd_ptr(kPartyBase);
    int count = 0;
    if (party && rd((uintptr_t)party + HOOKV_PARTY_COUNT, &count) && count > 1 && count <= 4) s->solo = false;

    // Camera.
    uint8_t* mgr = rd_ptr(HOOKV_S_MH_CAMERA);
    if (mgr && mem_readable(mgr + kViewport0 + kVpProj, 64)) {
        uint8_t* cam = rd_ptr((uintptr_t)mgr + kVp0Camera);
        if (cam && mem_readable(cam + kCamFov, 0x40)) {
            memcpy(&s->view.camPos, cam + kCamPos, 12);
            memcpy(&s->view.camTarget, cam + kCamTarget, 12);
            s->view.fovY = *(float*)(cam + kCamFov);
            memcpy(s->view.view, mgr + kViewport0 + kVpView, 64);
            memcpy(s->view.proj, mgr + kViewport0 + kVpProj, 64);
            s->view.valid = std::isfinite(s->view.camPos.x) && s->view.fovY > 1 && s->view.fovY < 170;
        }
    }

    // Monsters.
    uint8_t* se = rd_ptr(HOOKV_S_ENEMY);
    if (!se) return;
    for (int i = 0; i < kEnemySlotCount; i++) {
        uint8_t* ai = rd_ptr((uintptr_t)se + kEnemySlots + i * 8);
        if (!ai || !mem_readable(ai + kUnitState, 1) || (ai[kUnitState] & 0xE) == 0) continue;
        uint8_t* em = rd_ptr((uintptr_t)ai + kAiEnemy);
        if (!em || !mem_readable(em + kUnitState, 1) || (em[kUnitState] & 0xE) == 0) continue;
        uint8_t* aid = rd_ptr((uintptr_t)em + kEnemyAi);
        uint32_t flags = 0;
        if (aid && rd((uintptr_t)aid + HOOKV_ENEMY_DEAD_FLAG, &flags) && (flags & 4)) continue;  // carcass
        Monster m{};
        m.em = em;
        if (!health_of(em, HOOKV_ENEMY_HEALTH_MGR, &m.health, &m.hp, &m.maxHp)) continue;
        used_ok(HOOKS_ENEMY_HEALTH_MGR, "cpHealthManager vtable");
        if (m.hp <= 0) continue;
        // Path "em\\em101_00..." vs small monsters "em\\ems...".
        uint8_t* res = rd_ptr((uintptr_t)em + HOOKV_ENEMY_PATH);
        m.large = true;
        if (res && mem_readable(res + 0x0C, 8)) {
            const char* p = (const char*)res + 0x0C;
            const char* b = strchr(p, '\\');
            m.large = !(b && strncmp(b + 1, "ems", 3) == 0);
            used_ok(HOOKS_ENEMY_PATH, "model path");
        }
        if (mem_readable(em + kObbRot, 0x50)) {
            memcpy(m.rot, em + kObbRot, sizeof(m.rot));
            memcpy(&m.center, em + kObbCenter, 12);
            memcpy(&m.half, em + kObbHalf, 12);
            m.obbOk = m.half.x > 1 && m.half.y > 1 && m.half.z > 1 && m.half.x < 5000 && m.half.y < 5000 &&
                      m.half.z < 5000;
            if (m.obbOk) used_ok(HOOKS_ENEMY_OBB, "box extents in range");
        }
        s->monsters.push_back(m);
    }
    used_ok(HOOKS_S_ENEMY, "monster list read");
}

bool add_hp(uint8_t* health, float delta) {
    if (!health || !hook_ok(HOOKS_ADD_HP) || !std::isfinite(delta)) return false;
    ((AddHP_t)HOOKV_ADD_HP)(health, delta);
    return true;
}

void show_damage(uint8_t* em, int amount, const Vec3& pos, bool crit) {
    if (!em || !hook_ok(HOOKS_SHOW_DAMAGE) || amount <= 0) return;
    float p[3] = {pos.x, pos.y, pos.z};
    // Argument pattern of the game's own hit numbers (crossover-bridge's logged calls).
    ((ShowDamage_t)HOOKV_SHOW_DAMAGE)(em, amount, p, 1, crit ? 1 : 0, 0, 0, 0, 0);
}

bool fire_slinger(const Vec3& origin, const Vec3& target) {
    uint8_t* pl = g_player;
    if (!pl || !hook_ok(HOOKS_CREATE_SHELL)) return false;
    uint8_t* shll = rd_ptr((uintptr_t)pl + HOOKV_PLAYER_SLINGER_SHLL);
    if (!shll || !mem_readable(shll + 0xA8, 0x10)) return false;
    uint32_t count = *(uint32_t*)(shll + 0xB0);
    uint8_t* entries = *(uint8_t**)(shll + 0xA8);
    if (count == 0 || !entries || !mem_readable(entries, 16)) return false;
    void* shellParam = *(void**)(entries + 8);  // shell 0: the plain slinger shot
    if (!shellParam || !mem_readable(shellParam, 0x100)) return false;
    used_ok(HOOKS_PLAYER_SLINGER_SHLL, "shell list readable");
    alignas(16) uint8_t params[0x130];
    memset(params, 0, sizeof(params));
    memcpy(params, &origin, 12);
    params[0x10] = 1;
    memcpy(params + 0x40, &target, 12);
    params[0x50] = 1;
    const int32_t ids[3] = {0x12, -1, -1};
    memcpy(params + 0xA0, ids, sizeof(ids));
    return ((CreateShell_t)HOOKV_CREATE_SHELL)(shellParam, pl, pl, params) != nullptr;
}

bool world_ray(const Vec3& from, const Vec3& to, Vec3* hit) {
    if (!hook_ok(HOOKS_CHECK_SEGMENT) || !hook_ok(HOOKS_PARAM_CTOR) || !hook_ok(HOOKS_TRI_INFO_CTOR) ||
        !hook_ok(HOOKS_TRI_INFO_RESET) || !hook_ok(HOOKS_PARAM_DTOR))
        return false;
    void* coll = rd_ptr(HOOKV_S_COLLISION);
    if (!coll) return false;
    alignas(16) uint8_t param[0x140];
    alignas(16) uint8_t tri[0x140];
    alignas(16) float seg[8] = {from.x, from.y, from.z, 0.0f, to.x, to.y, to.z, 0.0f};
    memset(param, 0, sizeof(param));
    memset(tri, 0, sizeof(tri));
    // Same arguments as the game's own camera ground clamp (crossover-bridge).
    ((ParamCtor_t)HOOKV_PARAM_CTOR)(param, 0x7FFFFFFF, 0x3FFFFFFF, 0, 0, 0xA, 0, 1, 1, 0, 1);
    param[0xF9] = 0;
    ((Obj_t)HOOKV_TRI_INFO_CTOR)(tri);
    int r = ((CheckSegment_t)HOOKV_CHECK_SEGMENT)(coll, seg, 1, tri, param);
    if (r && hit) memcpy(hit, tri + 0xC0, 12);
    ((Obj_t)HOOKV_TRI_INFO_RESET)(tri);
    ((Obj_t)HOOKV_PARAM_DTOR)(param);
    used_ok(HOOKS_S_COLLISION, "segment test ran");
    return r != 0;
}

static float g_fovOverride = 0.0f;
void set_camera_fov(float fovY) { g_fovOverride = fovY; }

// ---------------------------------------------------------------- game-thread tick (vtable copy, no code patch)

typedef void(__fastcall* Move_t)(void* self);
static Move_t g_origMove = nullptr;
static uintptr_t* g_vtCopy = nullptr;
static uint8_t* g_camObj = nullptr;
static uintptr_t g_origVt = 0;
static volatile LONG g_inTick = 0;
static DWORD g_gameThread = 0;
constexpr int kVtSlots = 64;

static void __fastcall tick_move(void* self) {
    InterlockedIncrement(&g_inTick);
    if (!g_gameThread) {
        g_gameThread = GetCurrentThreadId();
        log("game tick: sMhCamera::move runs on thread %lu", g_gameThread);
        mark(HOOKS_CAM_MOVE_SLOT, true, "game called the copied slot");
    }
    // Aim zoom: written before and after the camera's own move, since which one sticks depends on when the game
    // rebuilds the FOV (checked in game).
    auto apply_fov = [&] {
        if (g_fovOverride <= 5.0f) return;
        uint8_t* cam = rd_ptr((uintptr_t)self + kVp0Camera);
        if (cam && mem_readable(cam + kCamFov, 4)) *(float*)(cam + kCamFov) = g_fovOverride;
    };
    apply_fov();
    g_origMove(self);
    apply_fov();
    mod_game_tick();
    InterlockedDecrement(&g_inTick);
}

bool game_tick_installed() { return g_camObj != nullptr && g_gameThread != 0; }

bool install_game_tick() {
    if (g_camObj) return true;
    if (!hook_ok(HOOKS_S_MH_CAMERA) || !hook_ok(HOOKS_CAM_MOVE_SLOT)) return false;
    uint8_t* obj = rd_ptr(HOOKV_S_MH_CAMERA);
    if (!obj) return false;
    uintptr_t vt = *(uintptr_t*)obj;
    if (vt != strtoull(after(kHooks[HOOKS_S_MH_CAMERA].check, "vt:"), nullptr, 16)) return false;
    g_vtCopy = (uintptr_t*)VirtualAlloc(nullptr, sizeof(uintptr_t) * (kVtSlots + 1), MEM_COMMIT | MEM_RESERVE,
                                        PAGE_READWRITE);
    if (!g_vtCopy) return false;
    // Keep the slot before the vtable (MSVC RTTI locator) so anything reading it still works.
    memcpy(g_vtCopy, (void*)(vt - 8), sizeof(uintptr_t) * (kVtSlots + 1));
    uintptr_t* slots = g_vtCopy + 1;
    int slot = (int)HOOKV_CAM_MOVE_SLOT;
    g_origMove = (Move_t)slots[slot];
    slots[slot] = (uintptr_t)&tick_move;
    g_origVt = vt;
    g_camObj = obj;
    InterlockedExchange64((LONG64*)obj, (LONG64)(uintptr_t)slots);  // the object now uses the copy
    log("game tick: sMhCamera %p now uses a copied vtable (slot %d -> mod tick)", obj, slot);
    return true;
}

void remove_game_tick() {
    if (!g_camObj) return;
    InterlockedExchange64((LONG64*)g_camObj, (LONG64)g_origVt);
    for (int i = 0; i < 200 && g_inTick; i++) Sleep(5);
    Sleep(50);
    g_camObj = nullptr;
    // The copy stays allocated: a frame in flight may still hold it.
    log("game tick: removed");
}

}  // namespace rh
