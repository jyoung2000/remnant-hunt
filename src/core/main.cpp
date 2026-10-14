// Remnant x MHW mashup core: start-up, settings and the Melty test checks. GPL-3.0-or-later (see LICENSE).
#include <cstdio>
#include <cstring>

#include "core.h"
#include "render3d.h"
#include "state.h"

namespace rh {

static Keys g_keys;
static GunCfg g_gun;
static ActionCfg g_actions;
static bool g_proxies = false;
static bool g_test = false;
static volatile bool g_stop = false;
static HANDLE g_thread = nullptr;

const Keys& keys() { return g_keys; }
const GunCfg& gun_cfg() { return g_gun; }
const ActionCfg& action_cfg() { return g_actions; }
bool bar_proxies() { return g_proxies; }
bool test_mode() { return g_test; }

static int read_key(const char* ini, const char* name, int def) {
    char v[32];
    GetPrivateProfileStringA("keys", name, "", v, sizeof(v), ini);
    if (!v[0]) return def;
    if (v[1] == 0 && v[0] >= 'a' && v[0] <= 'z') return v[0] - 32;
    if (v[1] == 0) return (unsigned char)v[0];
    if (_stricmp(v, "mouse4") == 0) return VK_XBUTTON1;
    if (_stricmp(v, "mouse5") == 0) return VK_XBUTTON2;
    if ((v[0] == 'F' || v[0] == 'f') && v[1] >= '1' && v[1] <= '9') return VK_F1 + atoi(v + 1) - 1;
    return def;
}

static int read_num(const char* ini, const char* name, int def) {
    char v[32];
    GetPrivateProfileStringA("gun", name, "", v, sizeof(v), ini);
    if (!v[0]) return def;
    char* e = nullptr;
    long n = strtol(v, &e, 0);
    return e == v ? def : (int)n;
}

static float read_f(const char* ini, const char* name) {
    char v[32];
    GetPrivateProfileStringA("gun", name, "", v, sizeof(v), ini);
    return v[0] ? (float)atof(v) : 0.0f;
}

static void load_settings() {
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%s\\remnant_hunt.ini", mod_dir());
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = fopen(ini, "wb");
        if (f) {
            fputs("; Remnant x Monster Hunter: World settings\r\n"
                  "[keys]\r\n; gun mode, reload, swap guns, weapon mod, archetype skill, second archetype's skill, relic,\r\n"
                  "; Ward 13 terminal and the Remnant menus at MHW's NPCs\r\n"
                  "gun=B\r\nreload=R\r\nswap=X\r\nmod=V\r\nskill=G\r\nskill2=H\r\nrelic=T\r\nterminal=P\r\n"
                  "; brings a Remnant II boss 30 m in front of you (hunting zones and the Training Area)\r\nboss=F9\r\n"
                  "[gun]\r\n; the hunter's hand joint (a world matrix found by the diagnostic): player+hand_l1 -> +hand_l2 -> entry hand_entry\r\n"
                  "; of hand_stride bytes. hand_offset_x/y/z (cm, in the joint's frame) and hand_axis (x,y,z,-x,-y,-z: the barrel direction)\r\n"
                  "; let you align the gun in game\r\n"
                  "hand_l1=0x90\r\nhand_l2=0x2B0\r\nhand_entry=396\r\nhand_stride=0x40\r\n"
                  "hand_offset_x=0\r\nhand_offset_y=0\r\nhand_offset_z=0\r\nhand_axis=z\r\n"
                  "[bar]\r\n; 1 puts Remnant stand-in items (Antidote etc.) in MHW's item bar\r\nproxies=0\r\n"
                  "[actions]\r\n; MHW action (set:id) played when a Remnant item is used; empty = none\r\nrelic=\r\nskill=\r\nconsumable=\r\n"
                  "[visuals]\r\n; UI scale: 0 = automatic (screen height / 1080, at least 1), or e.g. 1.5\r\nui_scale=0\r\n"
                  "; Remnant bosses: sun direction in degrees (fixed in the world), sun strength, ambient light\r\n"
                  "sun_yaw=35\r\nsun_pitch=55\r\nsun_strength=1.0\r\nambient=0.45\r\n"
                  "[paths]\r\n; Only needed if Remnant II is not found automatically\r\nremnant2=\r\n",
                  f);
            fclose(f);
        }
    }
    g_keys.gun = read_key(ini, "gun", 'B');
    g_keys.reload = read_key(ini, "reload", 'R');
    g_keys.swap = read_key(ini, "swap", 'X');
    g_keys.mod = read_key(ini, "mod", 'V');
    g_keys.skill = read_key(ini, "skill", 'G');
    g_keys.skill2 = read_key(ini, "skill2", 'H');
    g_keys.relic = read_key(ini, "relic", 'T');
    g_keys.terminal = read_key(ini, "terminal", 'P');
    g_keys.boss = read_key(ini, "boss", VK_F9);
    g_gun.handL1 = read_num(ini, "hand_l1", 0x90);
    g_gun.handL2 = read_num(ini, "hand_l2", 0x2B0);
    g_gun.handEntry = read_num(ini, "hand_entry", 396);
    g_gun.handStride = read_num(ini, "hand_stride", 0x40);
    g_gun.off[0] = read_f(ini, "hand_offset_x");
    g_gun.off[1] = read_f(ini, "hand_offset_y");
    g_gun.off[2] = read_f(ini, "hand_offset_z");
    char ax[16];
    GetPrivateProfileStringA("gun", "hand_axis", "z", ax, sizeof(ax), ini);
    const char* a = ax;
    if (*a == '-') { g_gun.axisNeg = true; a++; }
    g_gun.axis = (*a == 'x' || *a == 'X') ? 0 : (*a == 'y' || *a == 'Y') ? 1 : 2;
    g_proxies = GetPrivateProfileIntA("bar", "proxies", 0, ini) == 1;
    {
        auto fv = [&](const char* name, float def) {
            char v[32] = {0};
            GetPrivateProfileStringA("visuals", name, "", v, sizeof(v), ini);
            return v[0] ? (float)atof(v) : def;
        };
        g_sceneLight.sunYaw = fv("sun_yaw", 35.0f);
        g_sceneLight.sunPitch = fv("sun_pitch", 55.0f);
        g_sceneLight.sunStrength = fv("sun_strength", 1.0f);
        g_sceneLight.ambient = fv("ambient", 0.45f);
    }
    static const char* const kActKeys[3] = {"relic", "skill", "consumable"};
    for (int i = 0; i < 3; i++) {
        char v[32] = {0};
        GetPrivateProfileStringA("actions", kActKeys[i], "", v, sizeof(v), ini);
        int s, d;
        if (sscanf(v, "%d:%d", &s, &d) == 2 && s >= 0) { g_actions.set[i] = s; g_actions.id[i] = d; }
        else log("actions: no [actions] %s action set; drink a potion with the diagnostic running to find the drinking action id", kActKeys[i]);
    }
    char env[8];
    g_test = (GetEnvironmentVariableA("MELTY_TEST", env, sizeof(env)) && env[0] == '1') ||
             GetPrivateProfileIntA("test", "enabled", 0, ini) == 1;
}

static DWORD WINAPI init_thread(LPVOID) {
    log("core: starting (mod folder %s)", mod_dir());
    load_settings();
    if (!wait_for_game_code(90000)) {
        log("core: MHW's code never matched build 421810 (Ver. 15.23.00); the mod stays off");
        melty_log("check hooks fail unsupported-build");
        return 1;
    }
    int failed = check_hooks();
    log("core: %d of %d hooks failed their checks", failed, kHooksCount);
    melty_log("check hooks %s %d-failed", failed == 0 ? "ok" : "partial", failed);
    save_load();
    if (!r2_start()) melty_log("check remnant2 fail %s", r2_status());
    if (!overlay_install()) melty_log("check overlay fail");
    bool tickLogged = false, r2Logged = false;
    while (!g_stop) {
        Sleep(250);
        if (!game_tick_installed() && !install_game_tick()) continue;
        if (!tickLogged && game_tick_installed()) {
            melty_log("check game-tick ok");
            tickLogged = true;
        }
        if (!r2Logged && r2_ready()) {
            int bad = 0, ok = r2_cells_checked(&bad);
            melty_log("check remnant2 %s %d-cells %d-missing", bad == 0 ? "ok" : "partial", ok, bad);
            r2Logged = true;
        }
    }
    return 0;
}

}  // namespace rh

extern "C" __declspec(dllexport) bool rh_init(const char* modDir) {
    rh::set_mod_dir(modDir);
    rh::g_thread = CreateThread(nullptr, 0, rh::init_thread, nullptr, 0, nullptr);
    return rh::g_thread != nullptr;
}

extern "C" __declspec(dllexport) void rh_shutdown() {
    rh::g_stop = true;
    if (rh::g_thread) {
        WaitForSingleObject(rh::g_thread, 3000);
        CloseHandle(rh::g_thread);
    }
    rh::remove_game_tick();
    rh::overlay_remove();
    rh::r2_stop();
    rh::research_stop();
    rh::Shared& sh = rh::S();
    AcquireSRWLockExclusive(&sh.lock);
    rh::save_write(sh.save);
    ReleaseSRWLockExclusive(&sh.lock);
    rh::log("core: stopped");
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
