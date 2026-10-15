// Remnant x MHW mashup core: overlay (Dear ImGui on D3D11, or on D3D12 through D3D11On12) and input routing.
// GPL-3.0-or-later (see LICENSE).
//
// Everything hooked here lives in Windows' own DLLs, never in MHW's code:
//   dxgi.dll   IDXGISwapChain::Present / Present1 / ResizeBuffers   draw the mod's screens on the game's frame
//   d3d12.dll  ID3D12CommandQueue::ExecuteCommandLists              find the game's queue when it runs in DX12 mode
//   dinput8    IDirectInputDevice8::GetDeviceState / GetDeviceData   hide input the mod consumes from the game
//   user32     GetRawInputData / GetRawInputBuffer                  same, for raw input
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dinput.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "MinHook.h"
#include "bosses.h"
#include "core.h"
#include "key_edges.h"
#include "render3d.h"
#include "state.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace rh {

typedef HRESULT(WINAPI* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(WINAPI* Present1_t)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(WINAPI* Resize_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef void(WINAPI* Execute_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

struct HookSet { void* present = nullptr; void* present1 = nullptr; void* resize = nullptr; };
static Present_t oPresent[2];
static Present1_t oPresent1[2];
static Resize_t oResize[2];
static Execute_t oExecute = nullptr;
static HookSet g_targets[2];

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static HWND g_hwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;
static bool g_imguiReady = false;
static bool g_initFailed = false;
static volatile LONG g_inPresent = 0;

// DX11 path.
static ID3D11RenderTargetView* g_rtv = nullptr;
// DX12 path (D3D11On12 over the game's queue).
static bool g_dx12 = false;
static ID3D12CommandQueue* volatile g_queue = nullptr;
static ID3D11On12Device* g_on12 = nullptr;
static const int kMaxBuffers = 8;
static ID3D11Resource* g_wrapped[kMaxBuffers] = {};
static ID3D11RenderTargetView* g_wrappedRtv[kMaxBuffers] = {};
static UINT g_bufferCount = 0;

HWND game_window() { return g_hwnd; }

// ---------------------------------------------------------------- input state

static SRWLOCK g_inputLock = SRWLOCK_INIT;
static InputBlock g_block;
static KeyEdgeState g_keyEdges;
typedef SHORT(WINAPI* GetAsyncKeyState_t)(int);
static GetAsyncKeyState_t pGetAsyncKeyState = GetAsyncKeyState;

static InputBlock current_block() {
    AcquireSRWLockShared(&g_inputLock);
    InputBlock b = g_block;
    ReleaseSRWLockShared(&g_inputLock);
    return b;
}

void set_input_block(const InputBlock& b) {
    AcquireSRWLockExclusive(&g_inputLock);
    g_block = b;
    ReleaseSRWLockExclusive(&g_inputLock);
}

static bool focused() { return g_hwnd && GetForegroundWindow() == g_hwnd; }

bool key_down(int vk) { return focused() && (pGetAsyncKeyState(vk) & 0x8000) != 0; }
bool key_pressed(int vk) { return g_keyEdges.wasPressed(vk); }

static void poll_keys() {
    std::array<bool, 256> down{};
    const bool hasFocus = focused();
    for (int vk = 1; vk < 256; vk++) {
        down[(size_t)vk] = hasFocus && (pGetAsyncKeyState(vk) & 0x8000) != 0;
    }
    g_keyEdges.sample(hasFocus, down);
}

// Mouse button index (DirectInput order: 0 left, 1 right, 2 middle, 3 X1, 4 X2) of a VK, or -1.
static int vk_button(int vk) {
    switch (vk) {
        case VK_LBUTTON: return 0;
        case VK_RBUTTON: return 1;
        case VK_MBUTTON: return 2;
        case VK_XBUTTON1: return 3;
        case VK_XBUTTON2: return 4;
    }
    return -1;
}
static bool button_blocked(const InputBlock& b, int idx) {
    return b.mouse || b.mouseButtons || (idx >= 0 && idx < 8 && b.mouseButton[idx]);
}

// Synthesised clutch-claw press (inject_claw): reported down between g_clawFrom and g_clawTo (GetTickCount64 ms).
static volatile LONGLONG g_clawFrom = 0, g_clawTo = 0;
static bool claw_active() {
    LONGLONG t = (LONGLONG)GetTickCount64();
    return g_clawTo && t >= g_clawFrom && t < g_clawTo;
}
static int claw_button() {
    int i = vk_button(keys().clutchClaw);
    return i < 0 ? 2 : i;
}
void inject_claw(double delaySeconds) {
    LONGLONG t = (LONGLONG)GetTickCount64();
    g_clawFrom = t + (LONGLONG)(delaySeconds * 1000);
    g_clawTo = g_clawFrom + 80;
    static LONGLONG lastLog = 0;
    if (t - lastLog >= 1000) {
        lastLog = t;
        log("input: claw shot injected (button %d)", claw_button());
    }
}

// Per frame: mirror the claw window as window messages (for a game that reads the mouse through them).
static void claw_tick() {
    static bool down = false;
    bool want = claw_active();
    if (want == down || !g_hwnd) return;
    down = want;
    int b = claw_button();
    UINT msg = 0;
    WPARAM wp = 0;
    if (b == 0) { msg = want ? WM_LBUTTONDOWN : WM_LBUTTONUP; wp = want ? MK_LBUTTON : 0; }
    else if (b == 1) { msg = want ? WM_RBUTTONDOWN : WM_RBUTTONUP; wp = want ? MK_RBUTTON : 0; }
    else if (b == 2) { msg = want ? WM_MBUTTONDOWN : WM_MBUTTONUP; wp = want ? MK_MBUTTON : 0; }
    else { msg = want ? WM_XBUTTONDOWN : WM_XBUTTONUP; wp = MAKEWPARAM(want ? (b == 4 ? MK_XBUTTON2 : MK_XBUTTON1) : 0, b == 4 ? XBUTTON2 : XBUTTON1); }
    POINT pt = {};
    GetCursorPos(&pt);
    ScreenToClient(g_hwnd, &pt);
    PostMessageW(g_hwnd, msg, wp, MAKELPARAM(pt.x, pt.y));
}

static bool vk_blocked(const InputBlock& b, int vk) {
    if (vk <= 0 || vk >= 256) return false;
    int btn = vk_button(vk);
    if (btn >= 0) return button_blocked(b, btn);
    return b.keyboard || b.keys[vk];
}

// Which input paths the game uses (logged once, so the in-game test shows what was filtered).
static volatile LONG g_cntDiState = 0, g_cntDiData = 0, g_cntRaw = 0, g_cntRawBuf = 0, g_cntAsync = 0;

// Input reads the game makes per second, over 0.25 s windows (render thread). MHW stops polling gameplay input while
// a menu is open, so ui.cpp reads a drop in this rate as "a menu is open".
int input_reads_per_second() {
    static double lastT = 0;
    static long lastN = 0;
    static int rate = 0;
    double t = GetTickCount64() / 1000.0;
    long n = g_cntDiState + g_cntDiData + g_cntRaw + g_cntRawBuf + g_cntAsync;
    if (lastT == 0) { lastT = t; lastN = n; return 0; }
    if (t - lastT >= 0.25) {
        rate = (int)((n - lastN) / (t - lastT));
        lastT = t;
        lastN = n;
    }
    return rate;
}

// --- DirectInput
typedef HRESULT(WINAPI* GetState_t)(IDirectInputDevice8W*, DWORD, LPVOID);
typedef HRESULT(WINAPI* GetData_t)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
static GetState_t oGetState = nullptr;
static GetData_t oGetData = nullptr;

static bool dik_blocked(const InputBlock& b, DWORD dik) {
    if (b.keyboard) return true;
    for (int vk = 1; vk < 256; vk++)
        if (b.keys[vk] && MapVirtualKeyA(vk, MAPVK_VK_TO_VSC) == dik) return true;
    return false;
}

static HRESULT WINAPI hkGetState(IDirectInputDevice8W* dev, DWORD cb, LPVOID data) {
    HRESULT hr = oGetState(dev, cb, data);
    InterlockedIncrement(&g_cntDiState);
    if (FAILED(hr) || !data) return hr;
    InputBlock b = current_block();
    uint8_t* p = (uint8_t*)data;
    if (cb == 256) {
        for (DWORD k = 0; k < 256; k++)
            if ((p[k] & 0x80) && dik_blocked(b, k)) p[k] = 0;
    } else if (cb == sizeof(DIMOUSESTATE) || cb == sizeof(DIMOUSESTATE2)) {
        size_t buttons = cb == sizeof(DIMOUSESTATE) ? 4 : 8;
        if (b.mouse) memset(p, 0, cb);
        else {
            for (size_t i = 0; i < buttons; i++)
                if (button_blocked(b, (int)i)) p[12 + i] = 0;
            int cbtn = claw_button();
            if (claw_active() && cbtn < (int)buttons) p[12 + cbtn] = 0x80;
        }
    }
    return hr;
}

static HRESULT WINAPI hkGetData(IDirectInputDevice8W* dev, DWORD cb, LPDIDEVICEOBJECTDATA rg, LPDWORD inOut,
                                DWORD flags) {
    DWORD cap = inOut ? *inOut : 0;
    HRESULT hr = oGetData(dev, cb, rg, inOut, flags);
    InterlockedIncrement(&g_cntDiData);
    if (FAILED(hr) || !rg || !inOut || (flags & DIGDD_PEEK)) return hr;
    InputBlock b = current_block();
    DIDEVICEINSTANCEW info = {sizeof(info)};
    bool isMouse = SUCCEEDED(dev->GetDeviceInfo(&info)) && GET_DIDEVICE_TYPE(info.dwDevType) == DI8DEVTYPE_MOUSE;
    DWORD w = 0;
    for (DWORD i = 0; i < *inOut; i++) {
        DIDEVICEOBJECTDATA* e = (DIDEVICEOBJECTDATA*)((uint8_t*)rg + i * cb);
        bool isBtn = e->dwOfs >= DIMOFS_BUTTON0 && e->dwOfs <= DIMOFS_BUTTON7;
        bool drop = isMouse ? (b.mouse || (isBtn && button_blocked(b, (int)(e->dwOfs - DIMOFS_BUTTON0))))
                            : dik_blocked(b, e->dwOfs);
        if (!drop) {
            if (w != i) memmove((uint8_t*)rg + w * cb, e, cb);
            w++;
        }
    }
    *inOut = w;
    if (isMouse && !b.mouse && cb >= sizeof(DIDEVICEOBJECTDATA)) {
        // The injected claw press: one down event when the window opens, one up when it closes.
        static bool reported = false;
        bool want = claw_active();
        if (want != reported && w < cap) {
            DIDEVICEOBJECTDATA* e = (DIDEVICEOBJECTDATA*)((uint8_t*)rg + w * cb);
            memset(e, 0, cb);
            e->dwOfs = DIMOFS_BUTTON0 + claw_button();
            e->dwData = want ? 0x80 : 0;
            e->dwTimeStamp = GetTickCount();
            reported = want;
            *inOut = w + 1;
        }
    }
    return hr;
}

static void hook_dinput() {
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat(path, "\\dinput8.dll");
    HMODULE real = LoadLibraryA(path);
    typedef HRESULT(WINAPI * Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    Create_t create = real ? (Create_t)GetProcAddress(real, "DirectInput8Create") : nullptr;
    IDirectInput8W* di = nullptr;
    if (!create || FAILED(create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, (void**)&di, nullptr))) {
        log("input: no DirectInput");
        return;
    }
    IDirectInputDevice8W* kb = nullptr;
    if (SUCCEEDED(di->CreateDevice(GUID_SysKeyboard, &kb, nullptr)) && kb) {
        void** vt = *(void***)kb;
        bool ok = MH_CreateHook(vt[9], (void*)hkGetState, (void**)&oGetState) == MH_OK &&
                  MH_CreateHook(vt[10], (void*)hkGetData, (void**)&oGetData) == MH_OK &&
                  MH_EnableHook(vt[9]) == MH_OK && MH_EnableHook(vt[10]) == MH_OK;
        log("input: DirectInput reads %s", ok ? "filtered" : "NOT filtered");
        kb->Release();
    }
    di->Release();
}

// --- Raw input
typedef UINT(WINAPI* GetRawInputData_t)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
typedef UINT(WINAPI* GetRawInputBuffer_t)(PRAWINPUT, PUINT, UINT);
static GetRawInputData_t oGetRawInputData = nullptr;
static GetRawInputBuffer_t oGetRawInputBuffer = nullptr;

static void filter_raw(RAWINPUT* ri, const InputBlock& b) {
    if (ri->header.dwType == RIM_TYPEMOUSE) {
        if (b.mouse) {
            ri->data.mouse.lLastX = ri->data.mouse.lLastY = 0;
            ri->data.mouse.usButtonFlags = 0;
        } else if (b.mouseButtons) {
            ri->data.mouse.usButtonFlags &= (USHORT)(RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL);
        } else {
            static const USHORT kBits[5] = {0x0003, 0x000C, 0x0030, 0x00C0, 0x0300};  // RI_MOUSE_*_BUTTON_n_DOWN|UP
            USHORT drop = 0;
            for (int i = 0; i < 5; i++)
                if (b.mouseButton[i]) drop |= kBits[i];
            ri->data.mouse.usButtonFlags &= (USHORT)~drop;
        }
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        if (vk_blocked(b, ri->data.keyboard.VKey)) {
            ri->data.keyboard.VKey = 0xFF;
            ri->data.keyboard.MakeCode = 0;
        }
    }
}

static UINT WINAPI hkGetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr) {
    UINT r = oGetRawInputData(h, cmd, data, size, hdr);
    InterlockedIncrement(&g_cntRaw);
    if (cmd == RID_INPUT && data && r != (UINT)-1 && r >= sizeof(RAWINPUTHEADER)) filter_raw((RAWINPUT*)data, current_block());
    return r;
}

static UINT WINAPI hkGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT hdr) {
    UINT n = oGetRawInputBuffer(data, size, hdr);
    InterlockedIncrement(&g_cntRawBuf);
    if (data && n != (UINT)-1 && n > 0) {
        InputBlock b = current_block();
        RAWINPUT* ri = data;
        for (UINT i = 0; i < n; i++) {
            filter_raw(ri, b);
            ri = (RAWINPUT*)(((uintptr_t)ri + ri->header.dwSize + 7) & ~(uintptr_t)7);
        }
    }
    return n;
}

static SHORT WINAPI hkGetAsyncKeyState(int vk) {
    SHORT s = pGetAsyncKeyState(vk);
    InterlockedIncrement(&g_cntAsync);
    if (vk_blocked(current_block(), vk)) return 0;
    if (vk_button(vk) >= 0 && vk_button(vk) == claw_button() && claw_active()) s |= (SHORT)0x8000;
    return s;
}

// While a Remnant screen is open the game must not re-centre or confine the cursor.
typedef BOOL(WINAPI* SetCursorPos_t)(int, int);
typedef BOOL(WINAPI* ClipCursor_t)(const RECT*);
static SetCursorPos_t oSetCursorPos = nullptr;
static ClipCursor_t oClipCursor = nullptr;
static BOOL WINAPI hkSetCursorPos(int x, int y) {
    if (current_block().mouse) return TRUE;
    return oSetCursorPos(x, y);
}
static BOOL WINAPI hkClipCursor(const RECT* r) {
    if (current_block().mouse) return oClipCursor(nullptr);
    return oClipCursor(r);
}

static void hook_user32() {
    HMODULE u = GetModuleHandleA("user32.dll");
    struct { const char* name; void* detour; void** orig; } h[] = {
        {"GetRawInputData", (void*)hkGetRawInputData, (void**)&oGetRawInputData},
        {"GetRawInputBuffer", (void*)hkGetRawInputBuffer, (void**)&oGetRawInputBuffer},
        {"GetAsyncKeyState", (void*)hkGetAsyncKeyState, (void**)&pGetAsyncKeyState},
        {"SetCursorPos", (void*)hkSetCursorPos, (void**)&oSetCursorPos},
        {"ClipCursor", (void*)hkClipCursor, (void**)&oClipCursor},
    };
    int ok = 0;
    for (auto& e : h) {
        void* t = (void*)GetProcAddress(u, e.name);
        if (t && MH_CreateHook(t, e.detour, e.orig) == MH_OK && MH_EnableHook(t) == MH_OK) ok++;
        else log("input: could not filter %s", e.name);
    }
    log("input: %d of 5 user32 input calls filtered", ok);
}

// ---------------------------------------------------------------- window messages

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_imguiReady) {
        ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp);
        InputBlock b = current_block();
        bool mouseMsg = msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
        bool buttonMsg = mouseMsg && msg != WM_MOUSEMOVE && msg != WM_MOUSEWHEEL && msg != WM_MOUSEHWHEEL;
        bool keyMsg = msg >= WM_KEYFIRST && msg <= WM_KEYLAST;
        if (b.mouse && mouseMsg) return 0;
        if (buttonMsg) {
            int idx = -1;
            if (msg >= WM_LBUTTONDOWN && msg <= WM_LBUTTONDBLCLK) idx = 0;
            else if (msg >= WM_RBUTTONDOWN && msg <= WM_RBUTTONDBLCLK) idx = 1;
            else if (msg >= WM_MBUTTONDOWN && msg <= WM_MBUTTONDBLCLK) idx = 2;
            else if (msg >= WM_XBUTTONDOWN && msg <= WM_XBUTTONDBLCLK) idx = GET_XBUTTON_WPARAM(wp) == XBUTTON2 ? 4 : 3;
            if (button_blocked(b, idx)) return 0;
        }
        // Key-ups always reach the game, or a key held while a screen opens stays latched.
        bool keyUp = msg == WM_KEYUP || msg == WM_SYSKEYUP;
        if (keyMsg && !keyUp && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && vk_blocked(b, (int)wp))
            return 0;
        if (keyMsg && !keyUp && b.keyboard) return 0;
        if (b.mouse && msg == WM_SETCURSOR) return TRUE;
    }
    return CallWindowProcW(g_origWndProc, h, msg, wp, lp);
}

// ---------------------------------------------------------------- textures from Remnant II images

struct Tex { ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; bool failed = false; ULONGLONG lastUsed = 0; };
static std::map<std::string, Tex> g_tex;
static ULONGLONG g_texSweepAt = 0;

static void evict_stale_textures(ULONGLONG now) {
    if (now < g_texSweepAt + 15000) return;
    g_texSweepAt = now;
    for (auto it = g_tex.begin(); it != g_tex.end();) {
        if (now >= it->second.lastUsed && now - it->second.lastUsed > 90000) {
            if (it->second.srv) it->second.srv->Release();
            it = g_tex.erase(it);
        } else {
            ++it;
        }
    }
}

void* texture_for(const char* asset, int maxSide, int* w, int* h, bool srgb) {
    if (!asset || !g_dev) return nullptr;
    const ULONGLONG now = GetTickCount64();
    evict_stale_textures(now);
    std::string k = std::string(asset) + "|" + std::to_string(maxSide) + (srgb ? "|s" : "");
    auto it = g_tex.find(k);
    if (it != g_tex.end()) {
        it->second.lastUsed = now;
        if (w) *w = it->second.w;
        if (h) *h = it->second.h;
        return it->second.srv;
    }
    const R2Image* img = r2_image(asset, maxSide);
    if (!img) return nullptr;
    Tex t;
    if (r2_make_texture(g_dev, *img, asset, &t.srv, srgb)) {  // texture.cpp, shared with tests/test_textures.cpp
        t.w = img->w;
        t.h = img->h;
    } else {
        t.failed = true;
    }
    t.lastUsed = now;
    if (!strstr(asset, "/UI2/"))  // model textures: say once what became of each (icons are too many to list)
        log("overlay: texture %s -> %s %s %dx%d (%zu bytes)", asset, t.failed ? "FAILED" : "ok", img->format.c_str(), img->w, img->h, img->pixels.size());
    g_tex[k] = t;
    if (w) *w = t.w;
    if (h) *h = t.h;
    return t.srv;
}

// ---------------------------------------------------------------- render targets

// ---------------------------------------------------------------- MHW's scene depth (Direct3D 11), for the bosses
// Read-only: the depth buffer MHW clears each frame is recognised from its ClearDepthStencilView calls (a hook in
// d3d11.dll, like the Present hook), and a shader view of it lets render3d hide the Remnant bosses behind MHW's
// world. [bosses] scene_depth=0 in remnant_hunt.ini turns this off.

typedef void(STDMETHODCALLTYPE* ClearDSV_t)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
static ClearDSV_t oClearDSV = nullptr;
static thread_local bool t_ownDraw = false;
static UINT g_bbW = 0, g_bbH = 0;
struct DepthSeen { ID3D11Resource* res; UINT w, h; DXGI_FORMAT fmt; float clear; int clears, lastFrame; ULONGLONG lastPreProbe; };
static SRWLOCK g_depthLock = SRWLOCK_INIT;
static DepthSeen g_depthSeen[8];
static int g_depthCount = 0, g_frame = 0, g_depthMode = 1;
static ID3D11Resource* g_depthRes = nullptr;
static ID3D11ShaderResourceView* g_depthSrv = nullptr;
static bool g_depthLogged = false;
static ULONGLONG g_gridPresentAt = 0;
static float sample_depth(ID3D11DeviceContext* ctx, const DepthSeen& d, float nx, float ny);

// Diagnostic only: identify whether a candidate still contains scene geometry at Present or
// only immediately before MHW clears it for the next pass. Never use these samples to enable occlusion.
static void log_depth_grid(ID3D11DeviceContext* ctx, const char* phase, int index, const DepthSeen& d) {
    if (!g_dev || !ctx) return;
    static const float uv[5][2] = {{.25f, .25f}, {.75f, .25f}, {.5f, .5f}, {.25f, .75f}, {.75f, .75f}};
    float s[5];
    int drawn = 0;
    for (int k = 0; k < 5; k++) {
        s[k] = sample_depth(ctx, d, uv[k][0], uv[k][1]);
        if (s[k] >= 0 && fabsf(s[k] - d.clear) > .003f) drawn++;
    }
    log("overlay: depth grid %s candidate %d: %.4f %.4f %.4f %.4f %.4f (%d/5 non-clear, clear %.3f)",
        phase, index, s[0], s[1], s[2], s[3], s[4], drawn, d.clear);
}

static DXGI_FORMAT depth_srv_format(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

static void STDMETHODCALLTYPE hkClearDSV(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth,
                                         UINT8 stencil) {
    if (dsv && (flags & D3D11_CLEAR_DEPTH) && !t_ownDraw) {
        ID3D11Resource* r = nullptr;
        dsv->GetResource(&r);
        if (r) {
            AcquireSRWLockExclusive(&g_depthLock);
            int i = 0;
            while (i < g_depthCount && g_depthSeen[i].res != r) i++;
            if (i == g_depthCount && g_depthCount < 8) {
                ID3D11Texture2D* tex = nullptr;
                D3D11_TEXTURE2D_DESC td = {};
                if (SUCCEEDED(r->QueryInterface(IID_ID3D11Texture2D, (void**)&tex)) && tex) {
                    tex->GetDesc(&td);
                    tex->Release();
                    // Only depth MHW can also read (deferred lighting does), single-sampled, one slice.
                    if ((td.BindFlags & D3D11_BIND_SHADER_RESOURCE) && td.SampleDesc.Count == 1 && td.ArraySize == 1 &&
                        depth_srv_format(td.Format) != DXGI_FORMAT_UNKNOWN) {
                        r->AddRef();
                        g_depthSeen[g_depthCount++] = {r, td.Width, td.Height, td.Format, depth, 0, -1, 0};
                    }
                }
            }
            if (i < g_depthCount) {
                DepthSeen& seen = g_depthSeen[i];
                ULONGLONG now = GetTickCount64();
                if (g_frame > 600 && now - seen.lastPreProbe >= 20000) {
                    seen.lastPreProbe = now;
                    log_depth_grid(c, "before clear", i, seen);
                }
                seen.clear = depth;
                seen.clears++;
                seen.lastFrame = g_frame;
            }
            ReleaseSRWLockExclusive(&g_depthLock);
            r->Release();
        }
    }
    oClearDSV(c, dsv, flags, depth, stencil);
}

static void terrain_release();  // below: the terrain copy of the depth

static void release_depth() {
    AcquireSRWLockExclusive(&g_depthLock);
    render3d_set_game_depth(nullptr, false, 0, 0);
    terrain_release();
    if (g_depthSrv) { g_depthSrv->Release(); g_depthSrv = nullptr; }
    g_depthRes = nullptr;
    for (int i = 0; i < g_depthCount; i++) g_depthSeen[i].res->Release();
    g_depthCount = 0;
    ReleaseSRWLockExclusive(&g_depthLock);
}

// ---- depth self-check: measure instead of guess. Once a second the depth under the hunter's root is copied into a 1x1
// staging texture (CopySubresourceRegion: read-only for the game's buffer) and compared with the depth the camera's
// projection predicts for that point; the candidate that agrees is the scene depth, and whether it agrees as-is or as
// 1 - expected says how its Z runs relative to the projection.
static ULONGLONG g_measAt = 0;
static int g_measState = 0;  // 0 not measured yet, 1 a candidate matched, 2 nothing matched
static ID3D11Resource* g_measRes = nullptr;
static bool g_measFlip = false, g_measProjRev = false;
static ID3D11Resource* g_measSig[8];
static int g_measSigN = 0;
static bool g_measNoneLogged = false;
static int g_measFails = 0;
static ULONGLONG g_measFailSince = 0, g_measDiagAt = 0;

static bool hunter_clip(float* ndcZ, float* nx, float* ny, bool* projRev) {
    GameView v;
    Vec3 p;
    bool ok;
    AcquireSRWLockExclusive(&S().lock);
    ok = S().live.snap.inGame && S().live.snap.view.valid;
    v = S().live.snap.view;
    p = S().live.snap.pos;
    ReleaseSRWLockExclusive(&S().lock);
    if (!ok) return false;
    float in[4] = {p.x, p.y, p.z, 1}, a[4], c[4];
    for (int j = 0; j < 4; j++) a[j] = in[0] * v.view[j] + in[1] * v.view[4 + j] + in[2] * v.view[8 + j] + in[3] * v.view[12 + j];
    for (int j = 0; j < 4; j++) c[j] = a[0] * v.proj[j] + a[1] * v.proj[4 + j] + a[2] * v.proj[8 + j] + a[3] * v.proj[12 + j];
    if (c[3] <= 1.0f) return false;
    *nx = c[0] / c[3] * 0.5f + 0.5f;
    *ny = 0.5f - c[1] / c[3] * 0.5f;
    *ndcZ = c[2] / c[3];
    if (*nx < 0 || *nx >= 1 || *ny < 0 || *ny >= 1) return false;
    // Which way the projection maps depth (as render3d decides): a near and a far point straight ahead.
    float z[2] = {0, 0};
    const float d[2] = {100.0f, 100000.0f};
    for (int k = 0; k < 2; k++)
        for (float sign : {-1.0f, 1.0f}) {
            float cz = 0, cw = 0;
            for (int i = 0; i < 4; i++) {
                float vi[4] = {0, 0, sign * d[k], 1};
                cz += vi[i] * v.proj[i * 4 + 2];
                cw += vi[i] * v.proj[i * 4 + 3];
            }
            if (cw > 1e-6f) { z[k] = cz / cw; break; }
        }
    *projRev = z[0] > z[1];
    return true;
}

// The depth value under (nx, ny) of a depth texture, or -1.
static float sample_depth(ID3D11DeviceContext* ctx, const DepthSeen& d, float nx, float ny) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = d.fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &st)) || !st) return -1;
    UINT x = std::min((UINT)(nx * d.w), d.w - 1), y = std::min((UINT)(ny * d.h), d.h - 1);
    D3D11_BOX box = {x, y, 0, x + 1, y + 1, 1};
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, d.res, 0, &box);
    float out = -1;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms))) {
        switch (d.fmt) {
            case DXGI_FORMAT_R24G8_TYPELESS: out = (float)(*(const uint32_t*)ms.pData & 0xFFFFFF) / 16777215.0f; break;
            case DXGI_FORMAT_R32_TYPELESS:
            case DXGI_FORMAT_R32G8X24_TYPELESS: out = *(const float*)ms.pData; break;
            case DXGI_FORMAT_R16_TYPELESS: out = (float)*(const uint16_t*)ms.pData / 65535.0f; break;
            default: break;
        }
        ctx->Unmap(st, 0);
    }
    st->Release();
    return out;
}

// Under g_depthLock. Re-checks every second until a candidate matches, then every 10 s, and at once when the candidates change.
static void measure_depth() {
    ULONGLONG now = GetTickCount64();
    bool changed = g_depthCount != g_measSigN;
    for (int i = 0; !changed && i < g_depthCount; i++) changed = g_depthSeen[i].res != g_measSig[i];
    ULONGLONG dt = now - g_measAt;
    if (changed) {
        g_measSigN = g_depthCount;
        for (int i = 0; i < g_depthCount; i++) g_measSig[i] = g_depthSeen[i].res;
        g_measState = 0;
        g_measRes = nullptr;
        g_measNoneLogged = false;
        g_measFails = 0;
    } else if (dt < (g_measState == 1 ? 2500u : 1000u)) {
        return;
    }
    g_measAt = now;
    float ez, nx, ny;
    bool projRev;
    if (!hunter_clip(&ez, &nx, &ny, &projRev)) return;  // hunter off-screen or not in a hunt: try again next second
    float aspect = (float)g_bbW / (float)g_bbH;
    const bool holding = g_measState == 1;  // a matched candidate is kept; only it is re-checked
    for (int i = 0; i < g_depthCount; i++) {
        const DepthSeen& d = g_depthSeen[i];
        if (holding && d.res != g_measRes) continue;
        if (g_frame - d.lastFrame > 2 || !d.h || d.w * 2 < g_bbW || fabsf((float)d.w / (float)d.h - aspect) > 0.03f) continue;
        float sv = sample_depth(g_ctx, d, nx, ny);
        if (sv < 0) continue;
        if (now - g_measDiagAt >= 60000) {
            g_measDiagAt = now;
            log("overlay: depth check candidate %d: sampled %.4f, expected %.4f (plain) or %.4f (1-z)", i, sv, ez, 1.0f - ez);
        }
        // A near-clear sample in the hub once matched 0.0228 at 0.0078 under the old 0.02
        // tolerance. That was not proof of scene depth and could re-enable boss flicker.
        if (fabsf(sv - d.clear) <= .003f) continue;
        float flippedZ = 1.0f - ez;
        bool asIs = fabsf(sv - ez) < std::min(.008f, std::max(.002f, ez * .08f));
        bool flipped = fabsf(sv - flippedZ) < std::min(.008f, std::max(.002f, flippedZ * .08f));
        if (!asIs && !flipped) continue;
        bool same = g_measState == 1 && g_measRes == d.res && g_measFlip == !asIs;
        g_measState = 1;
        g_measRes = d.res;
        g_measFlip = !asIs;  // when both agree (depth near 0.5) the plain reading wins
        g_measProjRev = projRev;
        g_measFails = 0;
        if (!same)
            log("overlay: scene depth candidate %d matches the hunter (sampled %.4f, expected %.4f%s)", i, sv,
                asIs ? ez : 1.0f - ez, asIs ? "" : ", reversed Z");
        return;
    }
    if (holding) {
        // The probe is asynchronous with the game snapshot and can miss while the hunter is
        // moving or the camera is transitioning. A transient mismatch must not revoke a
        // candidate that has already matched: pick_depth() would fall back to the most-cleared
        // depth texture (which may be another pass), making the boss pop as its pixels are
        // discarded against unrelated depth. Keep the validated resource/polarity until the
        // resource list itself changes or the swap chain is released.
        if (g_measFails++ == 0) g_measFailSince = now;
        if (g_measFails == 5 && now - g_measFailSince >= 10000)
            log("overlay: scene depth hunter probe is temporarily inconsistent; retaining the previously validated resource");
        return;
    }
    if (g_depthCount && g_measState != 1) {
        g_measState = 2;
        if (!g_measNoneLogged) {
            g_measNoneLogged = true;
            log("overlay: no scene depth candidate matches the hunter (expected %.4f); boss occlusion stays off", ez);
        }
    }
}

// ---- the scene depth as terrain for the bosses (bosses.cpp ground_probe). Each frame a shader copies every 4th pixel
// of MHW's depth (read-only, through the shader view above) into a small float target, which is copied into one of
// three staging textures; the oldest is mapped with DO_NOT_WAIT, so the CPU never waits on the GPU. The finished copy
// is kept with the camera of its frame, converted to that projection's NDC z (NaN where it is sky).
static const char* kTerrainShader = R"(
Texture2D<float> gd : register(t0);
float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps_main(float4 p : SV_Position) : SV_Target {
    uint w, h;
    gd.GetDimensions(w, h);
    int2 q = min(int2(p.xy) * 4 + 2, int2(w, h) - 1);
    return gd.Load(int3(q, 0));
}
)";
struct TerrainSlot { bool pending; float vp[16], inv[16]; Vec3 eye; bool flip, gameRev; float clear; ULONGLONG tick; };
static ID3D11VertexShader* g_tdVs = nullptr;
static ID3D11PixelShader* g_tdPs = nullptr;
static bool g_tdFailed = false;
static ID3D11Texture2D* g_tdRt = nullptr;
static ID3D11RenderTargetView* g_tdRtv = nullptr;
static ID3D11Texture2D* g_tdStage[3] = {};
static TerrainSlot g_tdSlot[3];
static int g_tdNext = 0;
static UINT g_tdW = 0, g_tdH = 0, g_tdFullW = 0, g_tdFullH = 0;
static std::mutex g_tdLock;
static std::shared_ptr<const DepthFrame> g_tdFrame;

std::shared_ptr<const DepthFrame> depth_frame() {
    std::lock_guard<std::mutex> lk(g_tdLock);
    return g_tdFrame;
}

static void terrain_release() {
    if (g_tdRtv) { g_tdRtv->Release(); g_tdRtv = nullptr; }
    if (g_tdRt) { g_tdRt->Release(); g_tdRt = nullptr; }
    for (auto& s : g_tdStage)
        if (s) { s->Release(); s = nullptr; }
    for (auto& s : g_tdSlot) s.pending = false;
    g_tdW = g_tdH = 0;
}

static bool terrain_targets(UINT fullW, UINT fullH) {
    UINT w = (fullW + 3) / 4, h = (fullH + 3) / 4;
    if (g_tdRt && w == g_tdW && h == g_tdH && fullW == g_tdFullW && fullH == g_tdFullH) return true;
    terrain_release();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_tdRt)) || !g_tdRt) return false;
    if (FAILED(g_dev->CreateRenderTargetView(g_tdRt, nullptr, &g_tdRtv)) || !g_tdRtv) { terrain_release(); return false; }
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& s : g_tdStage)
        if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &s)) || !s) { terrain_release(); return false; }
    g_tdW = w;
    g_tdH = h;
    g_tdFullW = fullW;
    g_tdFullH = fullH;
    return true;
}

static bool terrain_shaders() {
    if (g_tdVs && g_tdPs) return true;
    if (g_tdFailed) return false;
    ID3DBlob *vsb = nullptr, *psb = nullptr, *errb = nullptr;
    bool ok = SUCCEEDED(D3DCompile(kTerrainShader, strlen(kTerrainShader), "terrain", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vsb, &errb)) &&
              SUCCEEDED(D3DCompile(kTerrainShader, strlen(kTerrainShader), "terrain", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &psb, &errb));
    if (ok) ok = SUCCEEDED(g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_tdVs)) &&
                 SUCCEEDED(g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_tdPs));
    if (!ok) {
        g_tdFailed = true;
        log("overlay: terrain depth copy shader failed: %s", errb ? (const char*)errb->GetBufferPointer() : "no device shader");
    }
    if (vsb) vsb->Release();
    if (psb) psb->Release();
    if (errb) errb->Release();
    return ok;
}

// The oldest pending copy, if the GPU has finished it, becomes the frame the game thread probes.
static void terrain_readback() {
    for (int k = 0; k < 3; k++) {
        int i = (g_tdNext + k) % 3;  // oldest first: g_tdNext is the one written three frames ago
        TerrainSlot& sl = g_tdSlot[i];
        if (!sl.pending) continue;
        D3D11_MAPPED_SUBRESOURCE ms;
        if (g_ctx->Map(g_tdStage[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms) != S_OK) return;
        auto f = std::make_shared<DepthFrame>();
        f->w = (int)g_tdW;
        f->h = (int)g_tdH;
        f->fullW = (int)g_tdFullW;
        f->fullH = (int)g_tdFullH;
        f->ndcZ.resize((size_t)g_tdW * g_tdH);
        const float sky = sl.gameRev ? 0.0f : 1.0f;
        for (UINT y = 0; y < g_tdH; y++) {
            const float* row = (const float*)((const uint8_t*)ms.pData + (size_t)y * ms.RowPitch);
            float* out = f->ndcZ.data() + (size_t)y * g_tdW;
            for (UINT x = 0; x < g_tdW; x++) {
                float v = row[x];
                bool isSky = fabsf(v - sky) < 1e-6f || fabsf(v - sl.clear) < 1e-6f || !(v >= 0 && v <= 1);
                out[x] = isSky ? NAN : (sl.flip ? 1.0f - v : v);
            }
        }
        g_ctx->Unmap(g_tdStage[i], 0);
        memcpy(f->viewProj, sl.vp, sizeof(sl.vp));
        memcpy(f->invViewProj, sl.inv, sizeof(sl.inv));
        f->eye = sl.eye;
        f->tickMs = sl.tick;
        sl.pending = false;
        std::lock_guard<std::mutex> lk(g_tdLock);
        g_tdFrame = std::move(f);
        return;
    }
}

// Render thread, at Present, under g_depthLock: one reduced copy of the scene depth (srv) per frame.
static void terrain_capture(ID3D11ShaderResourceView* srv, bool gameRev, float clear, UINT fullW, UINT fullH) {
    if (!srv || !fullW || !fullH || !terrain_enabled() || !terrain_shaders() || !terrain_targets(fullW, fullH)) return;
    terrain_readback();
    GameView v;
    bool ok;
    AcquireSRWLockExclusive(&S().lock);
    ok = S().live.snap.inGame && S().live.snap.view.valid;
    v = S().live.snap.view;
    ReleaseSRWLockExclusive(&S().lock);
    if (!ok) return;
    TerrainSlot sl = {};
    mat4_mul(v.view, v.proj, sl.vp);
    float invView[16];
    if (!mat4_inverse(sl.vp, sl.inv) || !mat4_inverse(v.view, invView)) return;
    sl.eye = {invView[12], invView[13], invView[14]};
    // Which way the projection runs (as hunter_clip decides): a near and a far point straight ahead.
    float z[2] = {0, 0};
    const float d[2] = {100.0f, 100000.0f};
    for (int k = 0; k < 2; k++)
        for (float sign : {-1.0f, 1.0f}) {
            float cz = 0, cw = 0;
            for (int i = 0; i < 4; i++) {
                float vi[4] = {0, 0, sign * d[k], 1};
                cz += vi[i] * v.proj[i * 4 + 2];
                cw += vi[i] * v.proj[i * 4 + 3];
            }
            if (cw > 1e-6f) { z[k] = cz / cw; break; }
        }
    bool projRev = z[0] > z[1];
    sl.gameRev = gameRev;
    sl.flip = gameRev != projRev;
    sl.clear = clear;
    sl.tick = GetTickCount64();

    // Draw the copy, leaving the context as MHW had it.
    ID3D11RenderTargetView* oRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oDsv = nullptr;
    D3D11_VIEWPORT oVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT oVpN = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11VertexShader* oVs = nullptr;
    ID3D11PixelShader* oPs = nullptr;
    ID3D11GeometryShader* oGs = nullptr;
    ID3D11HullShader* oHs = nullptr;
    ID3D11DomainShader* oDs = nullptr;
    ID3D11ShaderResourceView* oSrv = nullptr;
    ID3D11InputLayout* oIl = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY oTopo;
    ID3D11RasterizerState* oRs = nullptr;
    ID3D11BlendState* oBs = nullptr;
    float oBf[4];
    UINT oMask;
    ID3D11DepthStencilState* oDss = nullptr;
    UINT oRef;
    g_ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oRtv, &oDsv);
    g_ctx->RSGetViewports(&oVpN, oVp);
    g_ctx->VSGetShader(&oVs, nullptr, nullptr);
    g_ctx->PSGetShader(&oPs, nullptr, nullptr);
    g_ctx->GSGetShader(&oGs, nullptr, nullptr);
    g_ctx->HSGetShader(&oHs, nullptr, nullptr);
    g_ctx->DSGetShader(&oDs, nullptr, nullptr);
    g_ctx->PSGetShaderResources(0, 1, &oSrv);
    g_ctx->IAGetInputLayout(&oIl);
    g_ctx->IAGetPrimitiveTopology(&oTopo);
    g_ctx->RSGetState(&oRs);
    g_ctx->OMGetBlendState(&oBs, oBf, &oMask);
    g_ctx->OMGetDepthStencilState(&oDss, &oRef);

    D3D11_VIEWPORT vp = {0, 0, (float)g_tdW, (float)g_tdH, 0, 1};
    g_ctx->OMSetRenderTargets(1, &g_tdRtv, nullptr);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_tdVs, nullptr, 0);
    g_ctx->PSSetShader(g_tdPs, nullptr, 0);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->HSSetShader(nullptr, nullptr, 0);
    g_ctx->DSSetShader(nullptr, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->RSSetState(nullptr);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(nullptr, 0);
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &none);

    g_ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oRtv, oDsv);
    g_ctx->RSSetViewports(oVpN, oVp);
    g_ctx->VSSetShader(oVs, nullptr, 0);
    g_ctx->PSSetShader(oPs, nullptr, 0);
    g_ctx->GSSetShader(oGs, nullptr, 0);
    g_ctx->HSSetShader(oHs, nullptr, 0);
    g_ctx->DSSetShader(oDs, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &oSrv);
    g_ctx->IASetInputLayout(oIl);
    g_ctx->IASetPrimitiveTopology(oTopo);
    g_ctx->RSSetState(oRs);
    g_ctx->OMSetBlendState(oBs, oBf, oMask);
    g_ctx->OMSetDepthStencilState(oDss, oRef);
    for (auto* r : oRtv)
        if (r) r->Release();
    for (IUnknown* u : {(IUnknown*)oDsv, (IUnknown*)oVs, (IUnknown*)oPs, (IUnknown*)oGs, (IUnknown*)oHs, (IUnknown*)oDs,
                        (IUnknown*)oSrv, (IUnknown*)oIl, (IUnknown*)oRs, (IUnknown*)oBs, (IUnknown*)oDss})
        if (u) u->Release();

    g_ctx->CopyResource(g_tdStage[g_tdNext], g_tdRt);
    g_tdSlot[g_tdNext] = sl;
    g_tdSlot[g_tdNext].pending = true;
    g_tdNext = (g_tdNext + 1) % 3;
}

// Render thread, at Present: only a depth resource confirmed by measure_depth() may hide a boss or supply terrain.
// An unrelated pass can be full-frame and cleared often, yet make the boss pop out when used as scene depth.
static void pick_depth() {
    if (!g_depthMode || !oClearDSV || !g_bbW || !g_bbH) return;
    AcquireSRWLockExclusive(&g_depthLock);
    int best = -1;
    float aspect = (float)g_bbW / (float)g_bbH;
    for (int i = 0; i < g_depthCount; i++) {
        const DepthSeen& d = g_depthSeen[i];
        if (g_frame - d.lastFrame > 2 || !d.h || d.w * 2 < g_bbW) continue;
        if (fabsf((float)d.w / (float)d.h - aspect) > 0.03f) continue;
        if (best < 0 || d.clears > g_depthSeen[best].clears) best = i;
    }
    measure_depth();
    ULONGLONG now = GetTickCount64();
    if (g_frame > 600 && now - g_gridPresentAt >= 20000) {
        g_gridPresentAt = now;
        for (int i = 0; i < g_depthCount; i++)
            if (g_frame - g_depthSeen[i].lastFrame <= 2)
                log_depth_grid(g_ctx, "at Present", i, g_depthSeen[i]);
    }
    bool measured = false;
    if (g_measState == 1 && g_measRes) {
        for (int i = 0; i < g_depthCount; i++)
            if (g_depthSeen[i].res == g_measRes && g_frame - g_depthSeen[i].lastFrame <= 60) { best = i; measured = true; }
    }
    if (!measured) best = -1;
    ID3D11Resource* res = best >= 0 ? g_depthSeen[best].res : nullptr;
    if (res != g_depthRes) {
        if (g_depthSrv) { g_depthSrv->Release(); g_depthSrv = nullptr; }
        g_depthRes = res;
        if (res) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = depth_srv_format(g_depthSeen[best].fmt);
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            HRESULT hr = g_dev->CreateShaderResourceView(res, &sd, &g_depthSrv);
            log("overlay: scene depth %ux%u format %d, cleared to %.0f: %s", g_depthSeen[best].w, g_depthSeen[best].h,
                (int)g_depthSeen[best].fmt, g_depthSeen[best].clear, SUCCEEDED(hr) ? "in use" : "no shader view");
        }
    }
    if (g_depthSrv) {
        bool rev = measured ? (g_measFlip ? !g_measProjRev : g_measProjRev) : g_depthSeen[best].clear < 0.5f;
        render3d_set_game_depth(g_depthSrv, rev, (int)g_depthSeen[best].w, (int)g_depthSeen[best].h);
        terrain_capture(g_depthSrv, rev, g_depthSeen[best].clear, g_depthSeen[best].w, g_depthSeen[best].h);
    } else
        render3d_set_game_depth(nullptr, false, 0, 0);
    if (!g_depthLogged && g_frame > 900) {
        g_depthLogged = true;
        for (int i = 0; i < g_depthCount; i++)
            log("overlay: depth candidate %d: %ux%u format %d, clear %.1f, %d clears%s", i, g_depthSeen[i].w, g_depthSeen[i].h,
                (int)g_depthSeen[i].fmt, g_depthSeen[i].clear, g_depthSeen[i].clears, i == best ? " (chosen)" : "");
        if (!g_depthCount) log("overlay: no readable scene depth seen; Remnant bosses draw without MHW's occlusion");
    }
    g_frame++;
    ReleaseSRWLockExclusive(&g_depthLock);
}

static bool hook(void* target, void* detour, void** orig, const char* what);

static void release_targets() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    for (int i = 0; i < kMaxBuffers; i++) {
        if (g_wrappedRtv[i]) { g_wrappedRtv[i]->Release(); g_wrappedRtv[i] = nullptr; }
        if (g_wrapped[i]) { g_wrapped[i]->Release(); g_wrapped[i] = nullptr; }
    }
    if (g_ctx) g_ctx->Flush();
    g_bufferCount = 0;
    release_depth();
}

static bool make_targets(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC bd;
    if (SUCCEEDED(sc->GetDesc(&bd))) { g_bbW = bd.BufferDesc.Width; g_bbH = bd.BufferDesc.Height; }
    if (!g_dx12) {
        ID3D11Texture2D* back = nullptr;
        if (FAILED(sc->GetBuffer(0, IID_ID3D11Texture2D, (void**)&back)) || !back) return false;
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
        return g_rtv != nullptr;
    }
    DXGI_SWAP_CHAIN_DESC sd;
    sc->GetDesc(&sd);
    g_bufferCount = std::min<UINT>(sd.BufferCount, kMaxBuffers);
    D3D11_RESOURCE_FLAGS fl = {D3D11_BIND_RENDER_TARGET, 0, 0, 0};
    for (UINT i = 0; i < g_bufferCount; i++) {
        ID3D12Resource* res = nullptr;
        HRESULT hr = sc->GetBuffer(i, IID_ID3D12Resource, (void**)&res);
        if (SUCCEEDED(hr) && res) {
            hr = g_on12->CreateWrappedResource(res, &fl, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT,
                                               IID_ID3D11Resource, (void**)&g_wrapped[i]);
            res->Release();
        }
        if (SUCCEEDED(hr)) hr = g_dev->CreateRenderTargetView(g_wrapped[i], nullptr, &g_wrappedRtv[i]);
        if (FAILED(hr) || !g_wrappedRtv[i]) {
            static bool logged = false;  // render() retries every frame; say it once
            if (!logged) { logged = true; log("overlay: back buffer %u target failed (0x%08lx), will retry", i, (unsigned long)hr); }
            release_targets();  // leaves g_bufferCount at 0 so render() tries again
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- D3D12: find the game's queue

static void WINAPI hkExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    if (!g_queue) {
        D3D12_COMMAND_QUEUE_DESC d = q->GetDesc();
        if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            q->AddRef();
            g_queue = q;
            log("overlay: D3D12 direct queue %p found", (void*)q);
        }
    }
    oExecute(q, n, lists);
}

// ---------------------------------------------------------------- set-up on the first frame

static bool init_imgui(IDXGISwapChain* sc) {
    ID3D11Device* d11 = nullptr;
    ID3D12Device* d12 = nullptr;
    DXGI_SWAP_CHAIN_DESC sd;
    sc->GetDesc(&sd);
    if (SUCCEEDED(sc->GetDevice(IID_ID3D11Device, (void**)&d11)) && d11) {
        g_dev = d11;
        g_dev->GetImmediateContext(&g_ctx);
        g_dx12 = false;
        char ini[MAX_PATH];
        snprintf(ini, sizeof(ini), "%s\\remnant_hunt.ini", mod_dir());
        g_depthMode = GetPrivateProfileIntA("bosses", "scene_depth", 1, ini);
        if (g_depthMode) hook((*(void***)g_ctx)[53], (void*)hkClearDSV, (void**)&oClearDSV, "ClearDepthStencilView");
    } else if (SUCCEEDED(sc->GetDevice(IID_ID3D12Device, (void**)&d12)) && d12) {
        if (!g_queue) { d12->Release(); return false; }  // wait for the game to submit work
        IUnknown* queues[] = {(IUnknown*)g_queue};
        HRESULT hr = D3D11On12CreateDevice(d12, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, queues, 1, 0, &g_dev,
                                           &g_ctx, nullptr);
        d12->Release();
        if (FAILED(hr) || !g_dev) {
            log("overlay: D3D11On12 device failed (0x%08lx)", hr);
            g_initFailed = true;
            return false;
        }
        g_dev->QueryInterface(IID_ID3D11On12Device, (void**)&g_on12);
        g_dx12 = true;
    } else {
        log("overlay: the swapchain has neither a D3D11 nor a D3D12 device");
        g_initFailed = true;
        return false;
    }
    g_hwnd = sd.OutputWindow;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    ui_init_fonts();
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)wndproc);
    char cls[128] = {0};
    GetClassNameA(g_hwnd, cls, sizeof(cls));
    log("overlay: %s, window class '%s', %ux%u, %u buffers, format %d", g_dx12 ? "Direct3D 12 (via 11on12)" : "Direct3D 11",
        cls, sd.BufferDesc.Width, sd.BufferDesc.Height, sd.BufferCount, (int)sd.BufferDesc.Format);
    melty_log("check overlay ok %s", g_dx12 ? "dx12" : "dx11");
    g_imguiReady = true;
    return true;
}

static double g_inputReportAt = 0;
static float g_uiScaleIni = 0;  // [visuals] ui_scale; 0 = auto (framebuffer height / 1080, at least 1)
static bool g_uiScaleRead = false;

static void render(IDXGISwapChain* sc) {
    if (!g_imguiReady && !g_initFailed) init_imgui(sc);
    if (!g_imguiReady) return;
    if (!g_rtv && !g_bufferCount && !make_targets(sc)) return;
    poll_keys();
    claw_tick();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // The UI lays out at a logical resolution (about 1080p) and the framebuffer scale blows it up to the real one, so
    // 1440p and 4K get proportionally larger text. Glyphs rasterise at the framebuffer scale by themselves (ImGui 1.92's
    // font rasterizer density); FontScaleDpi stays 1 or text would scale twice.
    {
        ImGuiIO& io = ImGui::GetIO();
        if (!g_uiScaleRead) {
            g_uiScaleRead = true;
            char ini[MAX_PATH], v[32] = {0};
            snprintf(ini, sizeof(ini), "%s\\remnant_hunt.ini", mod_dir());
            GetPrivateProfileStringA("visuals", "ui_scale", "0", v, sizeof(v), ini);
            g_uiScaleIni = (float)atof(v);
        }
        float scale = g_uiScaleIni > 0 ? g_uiScaleIni : std::max(1.0f, (float)g_bbH / 1080.0f);
        if (scale > 8) scale = 8;
        if (g_bbW && g_bbH) {
            io.DisplaySize = ImVec2((float)g_bbW / scale, (float)g_bbH / scale);
            io.DisplayFramebufferScale = ImVec2(scale, scale);
        }
        ImGui::GetStyle().FontScaleDpi = 1.0f;
        if (scale != 1.0f && GetForegroundWindow() == g_hwnd) {
            // The Win32 backend reports the mouse in framebuffer pixels; the last event of a frame wins.
            POINT pt;
            RECT rc;
            if (GetCursorPos(&pt) && ScreenToClient(g_hwnd, &pt) && GetClientRect(g_hwnd, &rc) && PtInRect(&rc, pt))
                io.AddMousePosEvent((float)pt.x / scale, (float)pt.y / scale);
        }
    }
    ImGui::NewFrame();
    mod_frame();
    ImGui::Render();
    if (!g_dx12) {
        // Remnant II bosses into the frame, under the overlay.
        pick_depth();
        t_ownDraw = true;
        bosses_render(g_dev, g_ctx, g_rtv, (int)g_bbW, (int)g_bbH);
        t_ownDraw = false;
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    } else {
        IDXGISwapChain3* sc3 = nullptr;
        UINT idx = 0;
        if (SUCCEEDED(sc->QueryInterface(IID_IDXGISwapChain3, (void**)&sc3)) && sc3) {
            idx = sc3->GetCurrentBackBufferIndex();
            sc3->Release();
        }
        if (idx < g_bufferCount && g_wrapped[idx]) {
            g_on12->AcquireWrappedResources(&g_wrapped[idx], 1);
            bosses_render(g_dev, g_ctx, g_wrappedRtv[idx], (int)g_bbW, (int)g_bbH);
            g_ctx->OMSetRenderTargets(1, &g_wrappedRtv[idx], nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_on12->ReleaseWrappedResources(&g_wrapped[idx], 1);
            g_ctx->Flush();
        }
    }
    double t = GetTickCount64() / 1000.0;
    if (g_inputReportAt == 0) g_inputReportAt = t + 20;
    if (t > g_inputReportAt && g_inputReportAt > 0) {
        log("input paths used by the game: dinput state %ld, dinput data %ld, raw %ld, raw buffer %ld, key state %ld",
            g_cntDiState, g_cntDiData, g_cntRaw, g_cntRawBuf, g_cntAsync);
        g_inputReportAt = -1;
    }
}

static HRESULT WINAPI hkPresent0(IDXGISwapChain* sc, UINT sync, UINT flags) {
    InterlockedIncrement(&g_inPresent);
    if (!(flags & DXGI_PRESENT_TEST)) render(sc);
    HRESULT hr = oPresent[0](sc, sync, flags);
    InterlockedDecrement(&g_inPresent);
    return hr;
}
static HRESULT WINAPI hkPresent1_0(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    InterlockedIncrement(&g_inPresent);
    if (!(flags & DXGI_PRESENT_TEST)) render(sc);
    HRESULT hr = oPresent1[0](sc, sync, flags, p);
    InterlockedDecrement(&g_inPresent);
    return hr;
}
static HRESULT WINAPI hkResize0(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    release_targets();
    return oResize[0](sc, n, w, h, f, fl);
}
static HRESULT WINAPI hkPresent1(IDXGISwapChain* sc, UINT sync, UINT flags) {
    InterlockedIncrement(&g_inPresent);
    if (!(flags & DXGI_PRESENT_TEST)) render(sc);
    HRESULT hr = oPresent[1](sc, sync, flags);
    InterlockedDecrement(&g_inPresent);
    return hr;
}
static HRESULT WINAPI hkPresent1_1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    InterlockedIncrement(&g_inPresent);
    if (!(flags & DXGI_PRESENT_TEST)) render(sc);
    HRESULT hr = oPresent1[1](sc, sync, flags, p);
    InterlockedDecrement(&g_inPresent);
    return hr;
}
static HRESULT WINAPI hkResize1(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    release_targets();
    return oResize[1](sc, n, w, h, f, fl);
}

// ---------------------------------------------------------------- probes (throwaway device + swapchain)

static HWND probe_window() {
    WNDCLASSEXA wc = {sizeof(wc), CS_HREDRAW | CS_VREDRAW, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr),
                      nullptr, nullptr, nullptr, nullptr, "RemnantHuntProbe", nullptr};
    RegisterClassExA(&wc);
    return CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance,
                         nullptr);
}

static bool probe_dx11(HWND hw, HookSet* out) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hw;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    bool ok = SUCCEEDED(hr) && sc;
    if (ok) {
        void** vt = *(void***)sc;
        out->present = vt[8];
        out->resize = vt[13];
        out->present1 = vt[22];
    }
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    return ok;
}

static bool probe_dx12(HWND hw, HookSet* out, void** execute) {
    HMODULE m = LoadLibraryA("d3d12.dll");
    if (!m) return false;
    typedef HRESULT(WINAPI * Create_t)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    Create_t create = (Create_t)GetProcAddress(m, "D3D12CreateDevice");
    if (!create) return false;
    ID3D12Device* dev = nullptr;
    if (FAILED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_ID3D12Device, (void**)&dev)) || !dev) return false;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr;
    bool ok = SUCCEEDED(dev->CreateCommandQueue(&qd, IID_ID3D12CommandQueue, (void**)&q)) && q;
    if (ok) {
        *execute = (*(void***)q)[10];
        IDXGIFactory4* f = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_IDXGIFactory4, (void**)&f)) && f) {
            DXGI_SWAP_CHAIN_DESC1 sd = {};
            sd.BufferCount = 2;
            sd.Width = sd.Height = 64;
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            sd.SampleDesc.Count = 1;
            IDXGISwapChain1* sc = nullptr;
            if (SUCCEEDED(f->CreateSwapChainForHwnd(q, hw, &sd, nullptr, nullptr, &sc)) && sc) {
                void** vt = *(void***)sc;
                out->present = vt[8];
                out->resize = vt[13];
                out->present1 = vt[22];
                sc->Release();
            }
            f->Release();
        }
        q->Release();
    }
    dev->Release();
    return ok;
}

static bool hook(void* target, void* detour, void** orig, const char* what) {
    if (!target) return false;
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    if (s != MH_OK) log("overlay: hook %s failed (%d)", what, (int)s);
    return s == MH_OK;
}

bool overlay_install() {
    MH_Initialize();
    hook_dinput();
    hook_user32();
    HWND hw = probe_window();
    void* execute = nullptr;
    bool d11 = probe_dx11(hw, &g_targets[0]);
    bool d12 = probe_dx12(hw, &g_targets[1], &execute);
    DestroyWindow(hw);
    UnregisterClassA("RemnantHuntProbe", GetModuleHandleA(nullptr));
    bool ok = false;
    if (d11) {
        ok |= hook(g_targets[0].present, (void*)hkPresent0, (void**)&oPresent[0], "Present");
        hook(g_targets[0].present1, (void*)hkPresent1_0, (void**)&oPresent1[0], "Present1");
        hook(g_targets[0].resize, (void*)hkResize0, (void**)&oResize[0], "ResizeBuffers");
    }
    if (d12) {
        hook(execute, (void*)hkExecute, (void**)&oExecute, "ExecuteCommandLists");
        // A flip-model swapchain may use another implementation in dxgi.dll: hook it too when it differs.
        if (g_targets[1].present && g_targets[1].present != g_targets[0].present)
            ok |= hook(g_targets[1].present, (void*)hkPresent1, (void**)&oPresent[1], "Present (flip)");
        if (g_targets[1].present1 && g_targets[1].present1 != g_targets[0].present1)
            hook(g_targets[1].present1, (void*)hkPresent1_1, (void**)&oPresent1[1], "Present1 (flip)");
        if (g_targets[1].resize && g_targets[1].resize != g_targets[0].resize)
            hook(g_targets[1].resize, (void*)hkResize1, (void**)&oResize[1], "ResizeBuffers (flip)");
    }
    log("overlay: probes dx11=%d dx12=%d, present hooks %s", d11, d12, ok ? "installed" : "FAILED");
    return ok;
}

void overlay_remove() {
    MH_DisableHook(MH_ALL_HOOKS);
    for (int i = 0; i < 200 && g_inPresent; i++) Sleep(5);
    Sleep(100);
    if (g_origWndProc && g_hwnd) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
    if (g_imguiReady) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiReady = false;
    }
    for (auto& e : g_tex)
        if (e.second.srv) e.second.srv->Release();
    g_tex.clear();
    release_targets();
    if (g_on12) { g_on12->Release(); g_on12 = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    MH_RemoveHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    log("overlay: removed");
}

}  // namespace rh
