// Remnant Hunt setup: a one-file Windows installer with a window. GPL-3.0-or-later (see LICENSE).
//
// Every file of the release travels inside this exe (resources written by tools/make_installer.py), so nothing is
// downloaded. It finds Monster Hunter: World and Remnant II in the Steam libraries, shows what it found, and installs
// or removes only the mod's own files: dinput8.dll and the remnant_hunt folder next to MonsterHunterWorld.exe.
// Another mod loader's dinput8.dll (Stracker's Loader uses the same name) is never replaced unless the box is ticked,
// and is then kept as dinput8.dll.bak. An update keeps the settings (remnant_hunt.ini); uninstalling moves the
// remnant_hunt folder to the Recycle Bin. Remnant progress (%LOCALAPPDATA%\RemnantHunt) is never touched.
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <uxtheme.h>

#include <string>
#include <vector>

#include "payload.h"  // kPayload[] {resource id, path in the game folder}, kPayloadCount, kVersion, font ids

namespace {

const COLORREF kBg = RGB(20, 18, 16), kPanel = RGB(30, 27, 24), kText = RGB(236, 228, 214), kDim = RGB(150, 140, 125),
               kGold = RGB(200, 162, 90), kRed = RGB(180, 40, 28), kGood = RGB(120, 190, 90), kBad = RGB(220, 80, 60);
enum : int { ID_GAME = 100, ID_GAME_BROWSE, ID_R2, ID_R2_BROWSE, ID_FORCE, ID_INSTALL, ID_UNINSTALL, ID_PLAY, ID_CLOSE };

HINSTANCE g_inst;
HWND g_wnd, g_game, g_r2, g_force, g_install, g_uninstall;
HFONT g_title, g_body, g_bold, g_small;
HBRUSH g_bgBrush, g_panelBrush;
int g_dpi = 96;
std::wstring g_status = L"", g_gameInfo, g_r2Info, g_loaderInfo;
COLORREF g_statusColor = kDim;
bool g_gameOk = false, g_r2Ok = false, g_otherLoader = false, g_installed = false;

int S(int v) { return MulDiv(v, g_dpi, 96); }

std::wstring get_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/' || s.back() == L' ')) s.pop_back();
    return s;
}
bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// ---------------------------------------------------------------- Steam

std::vector<std::wstring> steam_libraries() {
    std::vector<std::wstring> libs;
    wchar_t buf[MAX_PATH];
    DWORD n = sizeof(buf);
    std::wstring steam;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &n) == ERROR_SUCCESS)
        steam = buf;
    else
        steam = L"C:\\Program Files (x86)\\Steam";
    for (auto& c : steam)
        if (c == L'/') c = L'\\';
    libs.push_back(steam);
    HANDLE f = CreateFileW((steam + L"\\steamapps\\libraryfolders.vdf").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return libs;
    std::string text(GetFileSize(f, nullptr), '\0');
    DWORD got = 0;
    ReadFile(f, text.data(), (DWORD)text.size(), &got, nullptr);
    CloseHandle(f);
    size_t p = 0;
    while ((p = text.find("\"path\"", p)) != std::string::npos) {
        size_t a = text.find('"', p + 6), b = a == std::string::npos ? a : text.find('"', a + 1);
        if (b == std::string::npos) break;
        std::string v = text.substr(a + 1, b - a - 1), u;
        for (size_t i = 0; i < v.size(); i++) {  // "D:\\Games" -> D:\Games
            if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == '\\') i++;
            u += v[i];
        }
        int wn = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, nullptr, 0);
        std::wstring w(wn, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, u.c_str(), -1, w.data(), wn);
        w.resize(wcslen(w.c_str()));
        libs.push_back(w);
        p = b;
    }
    return libs;
}

std::wstring find_app(const wchar_t* folder, const wchar_t* probe) {
    for (const std::wstring& lib : steam_libraries()) {
        std::wstring d = lib + L"\\steamapps\\common\\" + folder;
        if (exists(d + probe)) return d;
    }
    return L"";
}

// ---------------------------------------------------------------- what is there

bool our_loader(const std::wstring& dll) {
    HANDLE f = CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string b(GetFileSize(f, nullptr), '\0');
    DWORD got = 0;
    ReadFile(f, b.data(), (DWORD)b.size(), &got, nullptr);
    CloseHandle(f);
    return b.find("remnant_hunt") != std::string::npos;
}

std::wstring installed_version(const std::wstring& game) {
    HANDLE f = CreateFileW((game + L"\\remnant_hunt\\version.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return exists(game + L"\\remnant_hunt\\remnant_hunt.dll") ? L"an earlier version" : L"";
    char b[64] = {0};
    DWORD got = 0;
    ReadFile(f, b, sizeof(b) - 1, &got, nullptr);
    CloseHandle(f);
    std::wstring w;
    for (char* c = b; *c && *c != '\r' && *c != '\n'; c++) w += (wchar_t)*c;
    return w;
}

bool game_running() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe = {sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = _wcsicmp(pe.szExeFile, L"MonsterHunterWorld.exe") == 0;
    CloseHandle(snap);
    return found;
}

void refresh() {
    std::wstring game = get_text(g_game), r2 = get_text(g_r2);
    g_gameOk = !game.empty() && exists(game + L"\\MonsterHunterWorld.exe");
    g_r2Ok = !r2.empty() && exists(r2 + L"\\Remnant2\\Content\\Paks");
    g_gameInfo = g_gameOk ? L"Found" : game.empty() ? L"Not found: choose the folder with MonsterHunterWorld.exe"
                                                    : L"MonsterHunterWorld.exe is not in this folder";
    g_r2Info = g_r2Ok ? L"Found: the mod reads Remnant II's models, sounds and text from it"
                      : L"Not found: the mod needs Remnant II installed (choose its folder)";
    g_otherLoader = false;
    g_installed = false;
    if (g_gameOk) {
        std::wstring dll = game + L"\\dinput8.dll", ver = installed_version(game);
        g_installed = !ver.empty();
        if (exists(dll) && !our_loader(dll)) {
            g_otherLoader = true;
            g_loaderInfo = L"Another mod loader's dinput8.dll is installed (Stracker's Loader uses that name too).";
        } else {
            g_loaderInfo = g_installed ? L"Remnant Hunt " + ver + L" is installed." : L"Remnant Hunt is not installed yet.";
        }
    } else {
        g_loaderInfo = L"";
    }
    ShowWindow(g_force, g_otherLoader ? SW_SHOW : SW_HIDE);
    SetWindowTextW(g_install, g_installed ? L"Update" : L"Install");
    EnableWindow(g_install, g_gameOk);
    EnableWindow(g_uninstall, g_gameOk && g_installed);
    InvalidateRect(g_wnd, nullptr, TRUE);
}

void status(const std::wstring& s, COLORREF c) {
    g_status = s;
    g_statusColor = c;
    InvalidateRect(g_wnd, nullptr, TRUE);
    UpdateWindow(g_wnd);
}

// ---------------------------------------------------------------- install / uninstall

bool relaunch_elevated(const wchar_t* action) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring args = L"--game \"" + get_text(g_game) + L"\" --r2 \"" + get_text(g_r2) + L"\" " + action;
    if (SendMessageW(g_force, BM_GETCHECK, 0, 0) == BST_CHECKED) args += L" --force";
    SHELLEXECUTEINFOW sei = {sizeof(sei)};
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return false;
    PostQuitMessage(0);
    return true;
}

DWORD write_payload(const std::wstring& game, const PayloadFile& p) {
    HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(p.id), RT_RCDATA);
    HGLOBAL h = r ? LoadResource(g_inst, r) : nullptr;
    const void* data = h ? LockResource(h) : nullptr;
    DWORD size = r ? SizeofResource(g_inst, r) : 0;
    if (!data) return ERROR_RESOURCE_DATA_NOT_FOUND;
    std::wstring path = game + L"\\" + p.path;
    std::wstring dir = path.substr(0, path.find_last_of(L'\\'));
    int rc = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS) return (DWORD)rc;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD wrote = 0;
    BOOL ok = WriteFile(f, data, size, &wrote, nullptr);
    CloseHandle(f);
    return ok && wrote == size ? ERROR_SUCCESS : GetLastError();
}

void install() {
    std::wstring game = get_text(g_game), r2 = get_text(g_r2);
    if (game_running()) { status(L"Close Monster Hunter: World first.", kBad); return; }
    std::wstring dll = game + L"\\dinput8.dll";
    bool force = SendMessageW(g_force, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (exists(dll) && !our_loader(dll)) {
        if (!force) { status(L"Tick the box to replace the other mod loader (it is kept as dinput8.dll.bak).", kBad); return; }
        if (!CopyFileW(dll.c_str(), (dll + L".bak").c_str(), FALSE)) {
            if (GetLastError() == ERROR_ACCESS_DENIED && relaunch_elevated(L"--install")) return;
            status(L"Could not back up the other dinput8.dll.", kBad);
            return;
        }
    }
    for (int i = 0; i < kPayloadCount; i++) {
        status(L"Installing " + std::wstring(kPayload[i].path) + L" ...", kDim);
        DWORD e = write_payload(game, kPayload[i]);
        if (e == ERROR_ACCESS_DENIED || e == ERROR_PRIVILEGE_NOT_HELD) {
            if (relaunch_elevated(L"--install")) return;
            status(L"Windows refused to write into the game folder. Run the installer as administrator.", kBad);
            return;
        }
        if (e != ERROR_SUCCESS) {
            wchar_t msg[160];
            wsprintfW(msg, L"Could not write %s (error %lu).", kPayload[i].path, e);
            status(msg, kBad);
            return;
        }
    }
    std::wstring ver = game + L"\\remnant_hunt\\version.txt";
    HANDLE f = CreateFileW(ver.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(f, kVersionA, (DWORD)strlen(kVersionA), &w, nullptr);
        CloseHandle(f);
    }
    // A Remnant II that Steam does not list: tell the mod where it is.
    if (g_r2Ok && find_app(L"Remnant2", L"\\Remnant2\\Content\\Paks") != r2)
        WritePrivateProfileStringW(L"paths", L"remnant2", r2.c_str(), (game + L"\\remnant_hunt\\remnant_hunt.ini").c_str());
    refresh();
    status(L"Remnant Hunt " + std::wstring(kVersion) + L" is installed. Start Monster Hunter: World from Steam.", kGood);
}

void uninstall() {
    std::wstring game = get_text(g_game);
    if (game_running()) { status(L"Close Monster Hunter: World first.", kBad); return; }
    std::wstring dll = game + L"\\dinput8.dll";
    if (exists(dll) && our_loader(dll) && !DeleteFileW(dll.c_str())) {
        if (GetLastError() == ERROR_ACCESS_DENIED && relaunch_elevated(L"--uninstall")) return;
        status(L"Could not remove dinput8.dll.", kBad);
        return;
    }
    if (exists(dll + L".bak") && !exists(dll)) MoveFileW((dll + L".bak").c_str(), dll.c_str());
    std::wstring dir = game + L"\\remnant_hunt";
    dir.push_back(L'\0');  // SHFileOperation wants a double-terminated list
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = dir.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;  // to the Recycle Bin
    if (exists(game + L"\\remnant_hunt") && SHFileOperationW(&op) != 0) {
        status(L"Could not remove the remnant_hunt folder (is a file in it open?).", kBad);
        return;
    }
    refresh();
    status(L"Remnant Hunt is removed (its folder is in the Recycle Bin). Your Remnant progress is kept.", kGood);
}

void browse(HWND edit, const wchar_t* title) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(title);
    IShellItem* it = nullptr;
    if (SUCCEEDED(d->Show(g_wnd)) && SUCCEEDED(d->GetResult(&it))) {
        wchar_t* p = nullptr;
        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            SetWindowTextW(edit, p);
            CoTaskMemFree(p);
        }
        it->Release();
    }
    d->Release();
    refresh();
}

// ---------------------------------------------------------------- window

void text(HDC dc, HFONT f, COLORREF c, int x, int y, const std::wstring& s, int w = 0, UINT fmt = 0) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    RECT r = {x, y, x + (w ? w : S(560)), y + S(40)};
    DrawTextW(dc, s.c_str(), -1, &r, fmt | DT_NOPREFIX | DT_SINGLELINE);
}

// A green or red dot, then the line (drawn, so it does not depend on the font having check-mark glyphs).
void found(HDC dc, bool ok, int x, int y, const std::wstring& s) {
    HBRUSH b = CreateSolidBrush(ok ? kGood : kBad);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, x, y + S(4), x + S(9), y + S(13));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    text(dc, g_small, ok ? kGood : kBad, x + S(16), y, s);
}

void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    FillRect(dc, &rc, g_bgBrush);
    RECT band = {0, 0, rc.right, S(96)};
    FillRect(dc, &band, g_panelBrush);
    RECT line = {0, S(96), rc.right, S(98)};
    HBRUSH gold = CreateSolidBrush(kGold);
    FillRect(dc, &line, gold);
    // a worldstone: a red crystal framed in gold
    POINT gem[] = {{S(52), S(22)}, {S(72), S(48)}, {S(52), S(74)}, {S(32), S(48)}};
    HBRUSH red = CreateSolidBrush(kRed);
    HPEN pen = CreatePen(PS_SOLID, S(2), kGold);
    SelectObject(dc, red);
    SelectObject(dc, pen);
    Polygon(dc, gem, 4);
    SetBkMode(dc, TRANSPARENT);
    text(dc, g_title, kText, S(92), S(16), L"REMNANT HUNT");
    text(dc, g_small, kGold, S(94), S(60), std::wstring(L"Remnant II \x00D7 Monster Hunter: World  \x2022  version ") + kVersion);
    int y = S(116);
    text(dc, g_bold, kText, S(24), y, L"Monster Hunter: World");
    found(dc, g_gameOk, S(24), y + S(52), g_gameInfo);
    y += S(84);
    text(dc, g_bold, kText, S(24), y, L"Remnant II");
    found(dc, g_r2Ok, S(24), y + S(52), g_r2Info);
    y += S(84);
    text(dc, g_body, g_otherLoader ? kBad : kDim, S(24), y, g_loaderInfo);
    text(dc, g_small, g_statusColor, S(24), rc.bottom - S(84), g_status, rc.right - S(48), DT_END_ELLIPSIS);
    DeleteObject(gold);
    DeleteObject(red);
    DeleteObject(pen);
    EndPaint(h, &ps);
}

void draw_button(const DRAWITEMSTRUCT* d) {
    bool primary = d->CtlID == ID_INSTALL, disabled = d->itemState & ODS_DISABLED, down = d->itemState & ODS_SELECTED;
    COLORREF fill = disabled ? RGB(45, 42, 38) : primary ? (down ? RGB(170, 135, 70) : kGold) : (down ? RGB(55, 50, 45) : kPanel);
    HBRUSH b = CreateSolidBrush(fill);
    FillRect(d->hDC, &d->rcItem, b);
    DeleteObject(b);
    HPEN pen = CreatePen(PS_SOLID, 1, disabled ? RGB(60, 56, 50) : kGold);
    HGDIOBJ old = SelectObject(d->hDC, pen);
    SelectObject(d->hDC, GetStockObject(NULL_BRUSH));
    Rectangle(d->hDC, d->rcItem.left, d->rcItem.top, d->rcItem.right, d->rcItem.bottom);
    SelectObject(d->hDC, old);
    DeleteObject(pen);
    wchar_t t[64];
    GetWindowTextW(d->hwndItem, t, 64);
    SetBkMode(d->hDC, TRANSPARENT);
    SelectObject(d->hDC, g_bold);
    SetTextColor(d->hDC, disabled ? RGB(100, 94, 86) : primary ? RGB(20, 18, 16) : kText);
    RECT r = d->rcItem;
    DrawTextW(d->hDC, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

HWND make(const wchar_t* cls, const wchar_t* t, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(0, cls, t, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, S(x), S(y), S(w), S(h), g_wnd, (HMENU)(INT_PTR)id,
                             g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_body, TRUE);
    return c;
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_PAINT: paint(h); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_CTLCOLOREDIT:
            SetTextColor((HDC)w, kText);
            SetBkColor((HDC)w, kPanel);
            return (LRESULT)g_panelBrush;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetTextColor((HDC)w, kText);
            SetBkColor((HDC)w, kBg);
            return (LRESULT)g_bgBrush;
        case WM_DRAWITEM: draw_button((const DRAWITEMSTRUCT*)l); return TRUE;
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case ID_GAME:
                case ID_R2:
                    if (HIWORD(w) == EN_CHANGE) refresh();
                    break;
                case ID_GAME_BROWSE: browse(g_game, L"The Monster Hunter: World folder (with MonsterHunterWorld.exe)"); break;
                case ID_R2_BROWSE: browse(g_r2, L"The Remnant II folder (with Remnant2.exe)"); break;
                case ID_INSTALL: install(); break;
                case ID_UNINSTALL: uninstall(); break;
                case ID_PLAY: ShellExecuteW(nullptr, L"open", L"steam://rungameid/582010", nullptr, nullptr, SW_SHOWNORMAL); break;
                case ID_CLOSE: DestroyWindow(h); break;
            }
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void load_fonts() {
    for (int id : {kFontTitle, kFontBody, kFontBold}) {
        HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (!r) continue;
        DWORD n = 0;
        AddFontMemResourceEx(LockResource(LoadResource(g_inst, r)), SizeofResource(g_inst, r), nullptr, &n);
    }
    auto font = [](int px, int weight, const wchar_t* face) {
        return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
    };
    g_title = font(38, FW_BOLD, L"EB Garamond");
    g_body = font(15, FW_MEDIUM, L"Montserrat");
    g_bold = font(15, FW_BOLD, L"Montserrat");
    g_small = font(13, FW_MEDIUM, L"Montserrat");
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    g_inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(nullptr, screen);
    g_bgBrush = CreateSolidBrush(kBg);
    g_panelBrush = CreateSolidBrush(kPanel);
    load_fonts();

    WNDCLASSW wc = {};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"RemnantHuntSetup";
    RegisterClassW(&wc);
    RECT r = {0, 0, S(640), S(470)};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"Remnant Hunt Setup", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);

    g_game = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER, 24, 140, 470, 26, ID_GAME);
    make(L"BUTTON", L"Browse", BS_OWNERDRAW, 506, 140, 110, 26, ID_GAME_BROWSE);
    g_r2 = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER, 24, 224, 470, 26, ID_R2);
    make(L"BUTTON", L"Browse", BS_OWNERDRAW, 506, 224, 110, 26, ID_R2_BROWSE);
    g_force = make(L"BUTTON", L"Replace it (kept as dinput8.dll.bak)", BS_AUTOCHECKBOX, 24, 312, 400, 22, ID_FORCE);
    SetWindowTheme(g_force, L"", L"");  // unthemed, so its text takes the light colour on the dark window
    g_install = make(L"BUTTON", L"Install", BS_OWNERDRAW, 24, 408, 140, 40, ID_INSTALL);
    g_uninstall = make(L"BUTTON", L"Uninstall", BS_OWNERDRAW, 176, 408, 140, 40, ID_UNINSTALL);
    make(L"BUTTON", L"Play", BS_OWNERDRAW, 328, 408, 140, 40, ID_PLAY);
    make(L"BUTTON", L"Close", BS_OWNERDRAW, 480, 408, 136, 40, ID_CLOSE);

    // From an elevated restart: the folders and the action come on the command line.
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring game = find_app(L"Monster Hunter World", L"\\MonsterHunterWorld.exe");
    std::wstring r2 = find_app(L"Remnant2", L"\\Remnant2\\Content\\Paks");
    bool doInstall = false, doUninstall = false;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--game" && i + 1 < argc) game = argv[++i];
        else if (a == L"--r2" && i + 1 < argc) r2 = argv[++i];
        else if (a == L"--install") doInstall = true;
        else if (a == L"--uninstall") doUninstall = true;
        else if (a == L"--force") SendMessageW(g_force, BM_SETCHECK, BST_CHECKED, 0);
    }
    LocalFree(argv);
    SetWindowTextW(g_game, game.c_str());
    SetWindowTextW(g_r2, r2.c_str());
    refresh();
    status(g_gameOk ? L"Ready." : L"Choose your Monster Hunter: World folder.", kDim);
    ShowWindow(g_wnd, show);
    UpdateWindow(g_wnd);
    if (doInstall) install();
    if (doUninstall) uninstall();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return 0;
}
