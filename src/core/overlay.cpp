// Remnant x MHW mashup core: D3D11 overlay (Dear ImGui) and input routing. MIT License (see LICENSE).
//
// Hooks IDXGISwapChain::Present/ResizeBuffers inside dxgi.dll (not MHW code) to draw the mod's screens, and the
// DirectInput device read calls inside the system dinput8.dll to keep MHW from acting on input the mod consumes.
#include <d3d11.h>
#include <dinput.h>
#include <dxgi.h>

#include <cstdio>
#include <map>
#include <mutex>
#include <string>

#include "MinHook.h"
#include "core.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace rh {

typedef HRESULT(WINAPI* Present_t)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(WINAPI* Resize_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static Present_t oPresent = nullptr;
static Resize_t oResize = nullptr;
static void* g_presentTarget = nullptr;
static void* g_resizeTarget = nullptr;

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static HWND g_hwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;
static bool g_imguiReady = false;
static bool g_dx12Logged = false;
static volatile LONG g_inPresent = 0;

HWND game_window() { return g_hwnd; }

// ---------------------------------------------------------------- input state

static SRWLOCK g_inputLock = SRWLOCK_INIT;
static InputBlock g_block;
static bool g_prevDown[256];
static bool g_pressed[256];

void set_input_block(const InputBlock& b) {
    AcquireSRWLockExclusive(&g_inputLock);
    g_block = b;
    ReleaseSRWLockExclusive(&g_inputLock);
}

static bool focused() { return g_hwnd && GetForegroundWindow() == g_hwnd; }

bool key_down(int vk) { return focused() && (GetAsyncKeyState(vk) & 0x8000) != 0; }
bool key_pressed(int vk) { return vk > 0 && vk < 256 && g_pressed[vk]; }

static void poll_keys() {
    for (int vk = 1; vk < 256; vk++) {
        bool d = key_down(vk);
        g_pressed[vk] = d && !g_prevDown[vk];
        g_prevDown[vk] = d;
    }
}

// DirectInput: MHW reads keyboard and mouse through IDirectInputDevice8. Shared vtable for both device kinds.
typedef HRESULT(WINAPI* GetState_t)(IDirectInputDevice8W*, DWORD, LPVOID);
typedef HRESULT(WINAPI* GetData_t)(IDirectInputDevice8W*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
static GetState_t oGetState = nullptr;
static GetData_t oGetData = nullptr;
static volatile LONG g_diStateCalls = 0, g_diDataCalls = 0;

static bool blocked_key(const InputBlock& b, DWORD dik) {
    if (b.keyboard) return true;
    for (int vk = 1; vk < 256; vk++)
        if (b.keys[vk] && MapVirtualKeyA(vk, MAPVK_VK_TO_VSC) == dik) return true;
    return false;
}

static HRESULT WINAPI hkGetState(IDirectInputDevice8W* dev, DWORD cb, LPVOID data) {
    HRESULT hr = oGetState(dev, cb, data);
    InterlockedIncrement(&g_diStateCalls);
    if (FAILED(hr) || !data) return hr;
    AcquireSRWLockShared(&g_inputLock);
    InputBlock b = g_block;
    ReleaseSRWLockShared(&g_inputLock);
    uint8_t* p = (uint8_t*)data;
    if (cb == 256) {  // keyboard
        for (DWORD k = 0; k < 256; k++)
            if ((p[k] & 0x80) && blocked_key(b, k)) p[k] = 0;
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
    InterlockedIncrement(&g_diDataCalls);
    if (FAILED(hr) || !rg || !inOut || (flags & DIGDD_PEEK)) return hr;
    AcquireSRWLockShared(&g_inputLock);
    InputBlock b = g_block;
    ReleaseSRWLockShared(&g_inputLock);
    if (!b.keyboard && !b.mouse && !b.mouseButtons) {
        bool any = false;
        for (int i = 0; i < 256 && !any; i++) any = b.keys[i] != 0;
        if (!any) return hr;
    }
    DIDEVICEINSTANCEW info = {sizeof(info)};
    bool isMouse = SUCCEEDED(dev->GetDeviceInfo(&info)) && GET_DIDEVICE_TYPE(info.dwDevType) == DI8DEVTYPE_MOUSE;
    DWORD w = 0;
    for (DWORD i = 0; i < *inOut; i++) {
        DIDEVICEOBJECTDATA* e = (DIDEVICEOBJECTDATA*)((uint8_t*)rg + i * cb);
        bool drop;
        if (isMouse) drop = b.mouse || (b.mouseButtons && e->dwOfs >= DIMOFS_BUTTON0 && e->dwOfs <= DIMOFS_BUTTON7);
        else drop = blocked_key(b, e->dwOfs);
        if (!drop) {
            if (w != i) memcpy((uint8_t*)rg + w * cb, e, cb);
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
        log("input: no DirectInput (%p)", (void*)create);
        return;
    }
    IDirectInputDevice8W* kb = nullptr;
    if (SUCCEEDED(di->CreateDevice(GUID_SysKeyboard, &kb, nullptr)) && kb) {
        void** vt = *(void***)kb;
        bool ok = MH_CreateHook(vt[9], (void*)hkGetState, (void**)&oGetState) == MH_OK &&
                  MH_CreateHook(vt[10], (void*)hkGetData, (void**)&oGetData) == MH_OK &&
                  MH_EnableHook(vt[9]) == MH_OK && MH_EnableHook(vt[10]) == MH_OK;
        log("input: DirectInput device reads %s", ok ? "filtered" : "NOT filtered");
        kb->Release();
    }
    di->Release();
}

// ---------------------------------------------------------------- window

static bool modal_wants_input() {
    if (!g_imguiReady) return false;
    AcquireSRWLockShared(&g_inputLock);
    bool m = g_block.mouse && g_block.keyboard;
    ReleaseSRWLockShared(&g_inputLock);
    return m;
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_imguiReady) {
        ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp);
        if (modal_wants_input()) {
            bool inputMsg = (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
                            msg == WM_INPUT;
            if (inputMsg) return msg == WM_INPUT ? DefWindowProcW(h, msg, wp, lp) : 0;
            if (msg == WM_SETCURSOR) return TRUE;
        }
    }
    return CallWindowProcW(g_origWndProc, h, msg, wp, lp);
}

// ---------------------------------------------------------------- textures from Remnant II images

struct Tex { ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; bool failed = false; };
static std::map<std::string, Tex> g_tex;

static DXGI_FORMAT dxgi_format(const std::string& f, int* blockBytes) {
    *blockBytes = 16;
    if (f == "BC7") return DXGI_FORMAT_BC7_UNORM;
    if (f == "BC3") return DXGI_FORMAT_BC3_UNORM;
    if (f == "BC5") return DXGI_FORMAT_BC5_UNORM;
    *blockBytes = 8;
    if (f == "BC1") return DXGI_FORMAT_BC1_UNORM;
    if (f == "BC4") return DXGI_FORMAT_BC4_UNORM;
    *blockBytes = 0;
    if (f == "BGRA8") return DXGI_FORMAT_B8G8R8A8_UNORM;
    return DXGI_FORMAT_UNKNOWN;
}

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
    int bb = 0;
    DXGI_FORMAT fmt = dxgi_format(img->format, &bb);
    if (img->failed || fmt == DXGI_FORMAT_UNKNOWN || img->w <= 0) {
        t.failed = true;
    } else {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = img->w;
        d.Height = img->h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = fmt;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = {};
        sd.pSysMem = img->pixels.data();
        sd.SysMemPitch = bb ? ((img->w + 3) / 4) * bb : img->w * 4;
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(g_dev->CreateTexture2D(&d, &sd, &tex))) {
            g_dev->CreateShaderResourceView(tex, nullptr, &t.srv);
            tex->Release();
            t.w = img->w;
            t.h = img->h;
        } else {
            t.failed = true;
            log("overlay: texture %s could not be created", asset);
        }
    }
    g_tex[k] = t;
    if (w) *w = t.w;
    if (h) *h = t.h;
    return t.srv;
}

// ---------------------------------------------------------------- present

static void make_rtv(IDXGISwapChain* sc) {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, IID_ID3D11Texture2D, (void**)&back)) && back) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static bool init_imgui(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(IID_ID3D11Device, (void**)&g_dev)) || !g_dev) {
        if (!g_dx12Logged) {
            log("overlay: the game is not using Direct3D 11 (DirectX 12 mode?). The mod's screens need DX11 mode.");
            g_dx12Logged = true;
        }
        return false;
    }
    g_dev->GetImmediateContext(&g_ctx);
    DXGI_SWAP_CHAIN_DESC sd;
    sc->GetDesc(&sd);
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
    log("overlay: Direct3D 11, window %p class '%s', %ux%u", (void*)g_hwnd, cls, sd.BufferDesc.Width,
        sd.BufferDesc.Height);
    g_imguiReady = true;
    return true;
}

static HRESULT WINAPI hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    InterlockedIncrement(&g_inPresent);
    if (!g_imguiReady) init_imgui(sc);
    if (g_imguiReady) {
        if (!g_rtv) make_rtv(sc);
        poll_keys();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        mod_frame();
        ImGui::Render();
        if (g_rtv) {
            g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
    }
    HRESULT hr = oPresent(sc, sync, flags);
    InterlockedDecrement(&g_inPresent);
    return hr;
}

static HRESULT WINAPI hkResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    return oResize(sc, n, w, h, f, fl);
}

static bool swapchain_vtable(void*** out) {
    WNDCLASSEXA wc = {sizeof(wc), CS_HREDRAW | CS_VREDRAW, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr),
                      nullptr, nullptr, nullptr, nullptr, "RemnantHuntProbe", nullptr};
    RegisterClassExA(&wc);
    HWND hw = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance,
                            nullptr);
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
        out[0] = (void**)vt[8];
        out[1] = (void**)vt[13];
    }
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(hw);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return ok;
}

bool overlay_install() {
    MH_Initialize();
    hook_dinput();
    void** fns[2] = {};
    if (!swapchain_vtable(fns)) {
        log("overlay: could not create a probe swapchain");
        return false;
    }
    g_presentTarget = fns[0];
    g_resizeTarget = fns[1];
    bool ok = MH_CreateHook(g_presentTarget, (void*)hkPresent, (void**)&oPresent) == MH_OK &&
              MH_CreateHook(g_resizeTarget, (void*)hkResize, (void**)&oResize) == MH_OK &&
              MH_EnableHook(g_presentTarget) == MH_OK && MH_EnableHook(g_resizeTarget) == MH_OK;
    log("overlay: DXGI Present hook %s", ok ? "installed" : "FAILED");
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
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    MH_RemoveHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    log("overlay: removed (DirectInput state calls %ld, data calls %ld)", g_diStateCalls, g_diDataCalls);
}

}  // namespace rh
