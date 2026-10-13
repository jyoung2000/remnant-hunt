// Remnant x MHW mashup: dinput8.dll loader for Monster Hunter: World. GPL-3.0-or-later (see LICENSE).
// Adapted from justbustin/minecraft-crossover-bridge (MIT): forwards DirectInput to the system DLL and loads the
// mod's core once the game window exists. It touches no game code.
#include <cctype>
#include <cstdio>
#include <cstring>

#include "../common/log.h"

static HMODULE g_real = nullptr;

static FARPROC real_proc(const char* name) {
    if (!g_real) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 20) return nullptr;
        strcat(path, "\\dinput8.dll");
        g_real = LoadLibraryA(path);
        if (!g_real) return nullptr;
    }
    return GetProcAddress(g_real, name);
}

extern "C" {
HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE hinst, DWORD version, REFIID riid, LPVOID* out, void* outer) {
    typedef HRESULT(WINAPI * fn_t)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);
    fn_t f = (fn_t)real_proc("DirectInput8Create");
    return f ? f(hinst, version, riid, out, outer) : E_FAIL;
}
HRESULT WINAPI Proxy_DllCanUnloadNow() {
    typedef HRESULT(WINAPI * fn_t)();
    fn_t f = (fn_t)real_proc("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    typedef HRESULT(WINAPI * fn_t)(REFCLSID, REFIID, LPVOID*);
    fn_t f = (fn_t)real_proc("DllGetClassObject");
    return f ? f(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}
HRESULT WINAPI Proxy_DllRegisterServer() {
    typedef HRESULT(WINAPI * fn_t)();
    fn_t f = (fn_t)real_proc("DllRegisterServer");
    return f ? f() : E_FAIL;
}
HRESULT WINAPI Proxy_DllUnregisterServer() {
    typedef HRESULT(WINAPI * fn_t)();
    fn_t f = (fn_t)real_proc("DllUnregisterServer");
    return f ? f() : E_FAIL;
}
void* WINAPI Proxy_GetdfDIJoystick() {
    typedef void*(WINAPI * fn_t)();
    fn_t f = (fn_t)real_proc("GetdfDIJoystick");
    return f ? f() : nullptr;
}
}  // extern "C"

typedef bool (*core_init_t)(const char* modDir);
typedef void (*core_shutdown_t)();

static char g_modDir[MAX_PATH];
static HMODULE g_core = nullptr;
static core_shutdown_t g_coreShutdown = nullptr;

static bool is_game_process() {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    for (char* p = exe; *p; ++p) *p = (char)tolower((unsigned char)*p);
    return strstr(exe, "monsterhunterworld.exe") != nullptr;
}

static bool game_window_ready() {
    struct Ctx { DWORD pid; bool found; } ctx = {GetCurrentProcessId(), false};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            Ctx* c = (Ctx*)lp;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != c->pid || !IsWindowVisible(hwnd)) return TRUE;
            RECT r;
            GetClientRect(hwnd, &r);
            if (r.right - r.left >= 320 && r.bottom - r.top >= 200) { c->found = true; return FALSE; }
            return TRUE;
        },
        (LPARAM)&ctx);
    return ctx.found;
}

static FILETIME core_mtime(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    FILETIME z = {0, 0};
    return GetFileAttributesExA(path, GetFileExInfoStandard, &a) ? a.ftLastWriteTime : z;
}

static void unload_core() {
    if (!g_core) return;
    if (g_coreShutdown) g_coreShutdown();
    FreeLibrary(g_core);
    g_core = nullptr;
    g_coreShutdown = nullptr;
    rh::log("loader: core unloaded");
}

static bool load_core(unsigned gen) {
    char src[MAX_PATH], dir[MAX_PATH], dst[MAX_PATH];
    snprintf(src, sizeof(src), "%s\\remnant_hunt.dll", g_modDir);
    // Load a copy so the file on disk can be replaced (and hot-reloaded in dev mode) while the game runs.
    snprintf(dir, sizeof(dir), "%s\\loaded", g_modDir);
    CreateDirectoryA(dir, nullptr);
    snprintf(dst, sizeof(dst), "%s\\core_%lu_%u.dll", dir, GetCurrentProcessId(), gen);
    if (!CopyFileA(src, dst, FALSE)) {
        rh::log("loader: cannot copy %s (%lu)", src, GetLastError());
        return false;
    }
    HMODULE m = LoadLibraryA(dst);
    if (!m) {
        rh::log("loader: LoadLibrary(%s) failed (%lu)", dst, GetLastError());
        return false;
    }
    core_init_t init = (core_init_t)GetProcAddress(m, "rh_init");
    core_shutdown_t shutdown = (core_shutdown_t)GetProcAddress(m, "rh_shutdown");
    if (!init || !shutdown) {
        rh::log("loader: core is missing rh_init/rh_shutdown");
        FreeLibrary(m);
        return false;
    }
    g_core = m;
    g_coreShutdown = shutdown;
    bool ok = init(g_modDir);
    rh::log("loader: core generation %u started (%s)", gen, ok ? "ok" : "init reported errors");
    return ok;
}

static DWORD WINAPI loader_thread(LPVOID) {
    rh::log("loader: attached to MonsterHunterWorld.exe pid %lu", GetCurrentProcessId());
    for (int i = 0; i < 1200 && !game_window_ready(); i++) Sleep(100);
    Sleep(1500);
    char core[MAX_PATH], devFlag[MAX_PATH];
    snprintf(core, sizeof(core), "%s\\remnant_hunt.dll", g_modDir);
    snprintf(devFlag, sizeof(devFlag), "%s\\dev_reload", g_modDir);
    unsigned gen = 1;
    load_core(gen);
    FILETIME last = core_mtime(core);
    // Dev mode only (a file named dev_reload in the mod folder): reload the core when it changes on disk.
    for (;;) {
        Sleep(1000);
        if (GetFileAttributesA(devFlag) == INVALID_FILE_ATTRIBUTES) continue;
        FILETIME now = core_mtime(core);
        if (CompareFileTime(&now, &last) != 0) {
            Sleep(500);
            last = core_mtime(core);
            rh::log("loader: core changed on disk, reloading");
            unload_core();
            load_core(++gen);
        }
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        if (!is_game_process()) return TRUE;
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        char* slash = strrchr(exe, '\\');
        if (slash) *slash = 0;
        snprintf(g_modDir, sizeof(g_modDir), "%s\\remnant_hunt", exe);
        rh::set_mod_dir(g_modDir);
        HANDLE t = CreateThread(nullptr, 0, loader_thread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
