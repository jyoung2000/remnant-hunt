// Remnant x MHW mashup core: start-up, settings and the Melty test checks. MIT License (see LICENSE).
#include <cstdio>
#include <cstring>

#include "core.h"
#include "state.h"

namespace rh {

static Keys g_keys;
static bool g_test = false;
static volatile bool g_stop = false;
static HANDLE g_thread = nullptr;

const Keys& keys() { return g_keys; }
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

static void load_settings() {
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%s\\remnant_hunt.ini", mod_dir());
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = fopen(ini, "wb");
        if (f) {
            fputs("; Remnant x Monster Hunter: World settings\r\n"
                  "[keys]\r\n; gun mode, reload, swap guns, weapon mod, archetype skill, relic, Ward 13 terminal\r\n"
                  "gun=B\r\nreload=R\r\nswap=X\r\nmod=V\r\nskill=G\r\nrelic=T\r\nterminal=P\r\n"
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
    g_keys.relic = read_key(ini, "relic", 'T');
    g_keys.terminal = read_key(ini, "terminal", 'P');
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
    rh::Shared& sh = rh::S();
    AcquireSRWLockExclusive(&sh.lock);
    rh::save_write(sh.save);
    ReleaseSRWLockExclusive(&sh.lock);
    rh::log("core: stopped");
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
