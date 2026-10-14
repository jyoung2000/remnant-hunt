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
#include <dxgi1_4.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

#include "MinHook.h"
#include "bosses.h"
#include "core.h"
#include "render3d.h"
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
static bool g_prevDown[256];
static bool g_pressed[256];
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
bool key_pressed(int vk) { return vk > 0 && vk < 256 && g_pressed[vk]; }

static void poll_keys() {
    for (int vk = 1; vk < 256; vk++) {
        bool d = key_down(vk);
        g_pressed[vk] = d && !g_prevDown[vk];
        g_prevDown[vk] = d;
    }
}

static bool vk_blocked(const InputBlock& b, int vk) {
    if (vk <= 0 || vk >= 256) return false;
    bool mouseVk = vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
    if (mouseVk) return b.mouse || b.mouseButtons;
    return b.keyboard || b.keys[vk];
}

// Which input paths the game uses (logged once, so the in-game test shows what was filtered).
static volatile LONG g_cntDiState = 0, g_cntDiData = 0, g_cntRaw = 0, g_cntRawBuf = 0, g_cntAsync = 0;

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
        else if (b.mouseButtons) memset(p + 12, 0, buttons);
    }
    return hr;
}

static HRESULT WINAPI hkGetData(IDirectInputDevice8W* dev, DWORD cb, LPDIDEVICEOBJECTDATA rg, LPDWORD inOut,
                                DWORD flags) {
    HRESULT hr = oGetData(dev, cb, rg, inOut, flags);
    InterlockedIncrement(&g_cntDiData);
    if (FAILED(hr) || !rg || !inOut || (flags & DIGDD_PEEK) || *inOut == 0) return hr;
    InputBlock b = current_block();
    DIDEVICEINSTANCEW info = {sizeof(info)};
    bool isMouse = SUCCEEDED(dev->GetDeviceInfo(&info)) && GET_DIDEVICE_TYPE(info.dwDevType) == DI8DEVTYPE_MOUSE;
    DWORD w = 0;
    for (DWORD i = 0; i < *inOut; i++) {
        DIDEVICEOBJECTDATA* e = (DIDEVICEOBJECTDATA*)((uint8_t*)rg + i * cb);
        bool drop = isMouse ? (b.mouse || (b.mouseButtons && e->dwOfs >= DIMOFS_BUTTON0 && e->dwOfs <= DIMOFS_BUTTON7))
                            : dik_blocked(b, e->dwOfs);
        if (!drop) {
            if (w != i) memmove((uint8_t*)rg + w * cb, e, cb);
            w++;
        }
    }
    *inOut = w;
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
    return vk_blocked(current_block(), vk) ? 0 : s;
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
        if (b.mouseButtons && buttonMsg) return 0;
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

struct Tex { ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; bool failed = false; };
static std::map<std::string, Tex> g_tex;

void* texture_for(const char* asset, int maxSide, int* w, int* h) {
    if (!asset || !g_dev) return nullptr;
    std::string k = std::string(asset) + "|" + std::to_string(maxSide);
    auto it = g_tex.find(k);
    if (it != g_tex.end()) {
        if (w) *w = it->second.w;
        if (h) *h = it->second.h;
        return it->second.srv;
    }
    const R2Image* img = r2_image(asset, maxSide);
    if (!img) return nullptr;
    Tex t;
    if (r2_make_texture(g_dev, *img, asset, &t.srv)) {  // texture.cpp, shared with tests/test_textures.cpp
        t.w = img->w;
        t.h = img->h;
    } else {
        t.failed = true;
    }
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
struct DepthSeen { ID3D11Resource* res; UINT w, h; DXGI_FORMAT fmt; float clear; int clears, lastFrame; };
static SRWLOCK g_depthLock = SRWLOCK_INIT;
static DepthSeen g_depthSeen[8];
static int g_depthCount = 0, g_frame = 0, g_depthMode = 1;
static ID3D11Resource* g_depthRes = nullptr;
static ID3D11ShaderResourceView* g_depthSrv = nullptr;
static bool g_depthLogged = false;

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
                        g_depthSeen[g_depthCount++] = {r, td.Width, td.Height, td.Format, depth, 0, -1};
                    }
                }
            }
            if (i < g_depthCount) {
                g_depthSeen[i].clear = depth;
                g_depthSeen[i].clears++;
                g_depthSeen[i].lastFrame = g_frame;
            }
            ReleaseSRWLockExclusive(&g_depthLock);
            r->Release();
        }
    }
    oClearDSV(c, dsv, flags, depth, stencil);
}

static void release_depth() {
    AcquireSRWLockExclusive(&g_depthLock);
    render3d_set_game_depth(nullptr, false, 0, 0);
    if (g_depthSrv) { g_depthSrv->Release(); g_depthSrv = nullptr; }
    g_depthRes = nullptr;
    for (int i = 0; i < g_depthCount; i++) g_depthSeen[i].res->Release();
    g_depthCount = 0;
    ReleaseSRWLockExclusive(&g_depthLock);
}

// Render thread, at Present: the scene depth is the full-frame one MHW clears most.
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
    if (g_depthSrv)
        render3d_set_game_depth(g_depthSrv, g_depthSeen[best].clear < 0.5f, (int)g_depthSeen[best].w, (int)g_depthSeen[best].h);
    else
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

static void render(IDXGISwapChain* sc) {
    if (!g_imguiReady && !g_initFailed) init_imgui(sc);
    if (!g_imguiReady) return;
    if (!g_rtv && !g_bufferCount && !make_targets(sc)) return;
    poll_keys();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
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
