// Remnant x MHW mashup core: Remnant II content from the player's own install. GPL-3.0-or-later (see LICENSE).
//
// The mod never ships Remnant II files. It starts r2read.exe (a separate GPL-3.0 program in the mod folder) with
// the path of the player's Remnant II install; r2read decodes the requested text and icons in memory and returns
// them over a pipe. Everything is kept in memory for this session only.
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include "core.h"

using namespace sheets;

namespace rh {

static HANDLE g_proc = nullptr, g_in = nullptr, g_out = nullptr;
static std::mutex g_pipeLock;
static char g_status[256] = "not started";
static volatile bool g_ready = false;
static std::string g_dir;

const char* r2_status() { return g_status; }
bool r2_ready() { return g_ready; }

static bool dir_has_remnant(const std::string& d) {
    std::string p = d + "\\Remnant2\\Content\\Paks\\pakchunk0-Windows.utoc";
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Steam library folders from libraryfolders.vdf ("path" lines).
static std::vector<std::string> steam_libraries() {
    std::vector<std::string> libs;
    char steam[MAX_PATH] = {0};
    DWORD n = sizeof(steam);
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, nullptr, steam, &n) !=
        ERROR_SUCCESS)
        return libs;
    for (char* p = steam; *p; p++)
        if (*p == '/') *p = '\\';
    libs.push_back(steam);
    std::string vdf = std::string(steam) + "\\steamapps\\libraryfolders.vdf";
    FILE* f = fopen(vdf.c_str(), "rb");
    if (!f) return libs;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char* k = strstr(line, "\"path\"");
        if (!k) continue;
        char* a = strchr(k + 6, '"');
        char* b = a ? strchr(a + 1, '"') : nullptr;
        if (!a || !b) continue;
        std::string path;
        for (char* c = a + 1; c < b; c++) {
            if (c[0] == '\\' && c[1] == '\\') c++;
            path.push_back(*c);
        }
        libs.push_back(path);
    }
    fclose(f);
    return libs;
}

// "--remnant2 <folder>" or "--remnant2=<folder>" on the game's command line (Melty passes {game:<slug>} this way).
static std::string from_command_line() {
    std::string cl = GetCommandLineA();
    size_t p = cl.find("--remnant2");
    if (p == std::string::npos) return "";
    p += 10;
    while (p < cl.size() && (cl[p] == '=' || cl[p] == ' ')) p++;
    if (p >= cl.size()) return "";
    size_t e;
    if (cl[p] == '"') {
        e = cl.find('"', p + 1);
        return cl.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
    }
    e = cl.find(' ', p);
    return cl.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

static std::string find_remnant() {
    std::string arg = from_command_line();
    if (!arg.empty() && dir_has_remnant(arg)) return arg;
    char env[MAX_PATH];
    if (GetEnvironmentVariableA("REMNANTHUNT_R2", env, sizeof(env)) && dir_has_remnant(env)) return env;
    char ini[MAX_PATH], val[MAX_PATH];
    snprintf(ini, sizeof(ini), "%s\\remnant_hunt.ini", mod_dir());
    if (GetPrivateProfileStringA("paths", "remnant2", "", val, sizeof(val), ini) && dir_has_remnant(val)) return val;
    for (const std::string& lib : steam_libraries()) {
        std::string d = lib + "\\steamapps\\common\\Remnant2";
        if (dir_has_remnant(d)) return d;
    }
    return "";
}

static bool write_all(const std::string& s) {
    DWORD w;
    return WriteFile(g_in, s.data(), (DWORD)s.size(), &w, nullptr) && w == s.size();
}
static bool read_exact(void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    while (n) {
        DWORD r = 0;
        if (!ReadFile(g_out, p, (DWORD)std::min<size_t>(n, 1 << 20), &r, nullptr) || r == 0) return false;
        p += r;
        n -= r;
    }
    return true;
}
static bool read_line(std::string* out) {
    out->clear();
    char c;
    while (read_exact(&c, 1)) {
        if (c == '\n') return true;
        out->push_back(c);
        if (out->size() > 4096) return false;
    }
    return false;
}

// One request/answer. Worker threads only (never the render or game thread).
static bool ask(const std::string& req, std::string* head, std::vector<uint8_t>* body) {
    std::lock_guard<std::mutex> l(g_pipeLock);
    if (!g_proc) return false;
    if (!write_all(req + "\n") || !read_line(head)) {
        snprintf(g_status, sizeof(g_status), "r2read stopped answering");
        g_ready = false;
        return false;
    }
    body->clear();
    if (head->compare(0, 3, "OK ") != 0) return false;
    size_t sp = head->find_last_of(' ');
    size_t n = strtoull(head->c_str() + sp + 1, nullptr, 10);
    if (head->compare(0, 11, "OK r2read 1") == 0) return true;
    body->resize(n);
    return read_exact(body->data(), n);
}

// ---------------------------------------------------------------- caches

static std::mutex g_cacheLock;
static std::map<std::string, std::string> g_text;   // "asset\tkey" -> text
static std::map<std::string, R2Image> g_img;        // "asset|max" -> image
static const std::string kEmpty;

static std::string strip_markup(const std::string& s) {
    // Remnant's rich text: <span ...>WORD</> and <Stat>Name</>. Keep the words.
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '<') {
            size_t e = s.find('>', i);
            if (e != std::string::npos) { i = e; continue; }
        }
        if (s[i] == '\r') continue;
        out.push_back(s[i]);
    }
    return out;
}

static std::mutex g_queueLock;
static HANDLE g_wake = nullptr;

static void fetch_text(const std::string& asset, const std::string& key) {
    std::string head;
    std::vector<uint8_t> body;
    std::string id = asset + "\t" + key;
    std::string val;
    if (ask("TEXT\t" + id, &head, &body)) val = strip_markup(std::string(body.begin(), body.end()));
    else log("r2: text %s %.8s... unavailable (%s)", asset.c_str(), key.c_str(), head.c_str());
    std::lock_guard<std::mutex> l(g_cacheLock);
    g_text[id] = val;
}

static std::string camel_split(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        bool upper = s[i] >= 'A' && s[i] <= 'Z';
        bool afterMc = i >= 2 && s[i - 2] == 'M' && s[i - 1] == 'c';
        if (i && upper && !afterMc && s[i - 1] != ' ') out.push_back(' ');
        out.push_back(s[i]);
    }
    return out;
}

static void fetch_name(const std::string& asset, const std::string& pattern) {
    std::string head;
    std::vector<uint8_t> body;
    std::string id = "N\t" + asset + "\t" + pattern;
    std::string val;
    if (ask("NAME\t" + asset + "\t" + pattern, &head, &body)) val = camel_split(std::string(body.begin(), body.end()));
    std::lock_guard<std::mutex> l(g_cacheLock);
    g_text[id] = val;
}

static void fetch_image(const std::string& asset, int maxSide) {
    std::string head;
    std::vector<uint8_t> body;
    R2Image img;
    if (ask("TEX\t" + asset + "\t" + std::to_string(maxSide), &head, &body)) {
        char fmt[16] = {0};
        sscanf(head.c_str(), "OK %15s %d %d", fmt, &img.w, &img.h);
        img.format = fmt;
        img.pixels = std::move(body);
    } else {
        img.failed = true;
        log("r2: image %s unavailable (%s)", asset.c_str(), head.c_str());
    }
    std::lock_guard<std::mutex> l(g_cacheLock);
    g_img[asset + "|" + std::to_string(maxSide)] = std::move(img);
}

static std::vector<std::pair<std::string, std::string>> g_textQueue;  // (asset, key) asked for, not yet read

const std::string& r2_text(const R2Text& t) {
    if (!t.asset || !t.key) return kEmpty;
    std::string id = std::string(t.asset) + "\t" + t.key;
    {
        std::lock_guard<std::mutex> l(g_cacheLock);
        auto it = g_text.find(id);
        if (it != g_text.end()) return it->second;
    }
    // Not read yet: queue it for the worker (the screens show it a moment later).
    std::lock_guard<std::mutex> q(g_queueLock);
    for (auto& e : g_textQueue)
        if (e.first == t.asset && e.second == t.key) return kEmpty;
    g_textQueue.push_back({t.asset, t.key});
    if (g_wake) SetEvent(g_wake);
    return kEmpty;
}

const std::string& r2_name(const R2Name& n) {
    std::lock_guard<std::mutex> l(g_cacheLock);
    auto it = g_text.find(std::string("N\t") + n.asset + "\t" + n.pattern);
    return it == g_text.end() ? kEmpty : it->second;
}

static std::vector<std::pair<std::string, int>> g_imgQueue;

const R2Image* r2_image(const char* asset, int maxSide) {
    if (!asset) return nullptr;
    std::string k = std::string(asset) + "|" + std::to_string(maxSide);
    {
        std::lock_guard<std::mutex> l(g_cacheLock);
        auto it = g_img.find(k);
        if (it != g_img.end()) return &it->second;
    }
    std::lock_guard<std::mutex> q(g_queueLock);
    for (auto& e : g_imgQueue)
        if (e.first == asset && e.second == maxSide) return nullptr;
    g_imgQueue.push_back({asset, maxSide});
    if (g_wake) SetEvent(g_wake);
    return nullptr;
}

// The cells the first screens need, read at start; the rest of the catalog (about two thousand cells) is read the
// first time a screen shows it.
static void prefetch_all() {
    auto text = [](const R2Text& t) { if (t.asset && t.key) fetch_text(t.asset, t.key); };
    for (const Archetypes& a : kArchetypes) { text(a.name); text(a.prime); text(a.desc); }
    for (const Skills& s : kSkills) text(s.name);
    for (const Weapons& w : kWeapons) text(w.name);
    for (const Mods& m : kMods) text(m.name);
    for (const Relics& r : kRelics) text(r.name);
    for (const Npcs& n : kNpcs) {
        fetch_name(n.name.asset, n.name.pattern);
        for (const R2Text* t : {&n.greet_1, &n.greet_2, &n.greet_3, &n.open_option, &n.leave_option, &n.shop_line,
                                &n.done_line})
            text(*t);
    }
}

int r2_cells_checked(int* failed) {
    int ok = 0, bad = 0;
    std::lock_guard<std::mutex> l(g_cacheLock);
    for (auto& e : g_text) (e.second.empty() ? bad : ok)++;
    for (auto& e : g_img) (e.second.failed ? bad : ok)++;
    if (failed) *failed = bad;
    return ok;
}

static DWORD WINAPI worker(LPVOID) {
    HANDLE in_r, in_w, out_r, out_w;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    CreatePipe(&in_r, &in_w, &sa, 0);
    CreatePipe(&out_r, &out_w, &sa, 0);
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi = {};
    char cmd[MAX_PATH * 3];
    snprintf(cmd, sizeof(cmd), "\"%s\\r2read.exe\" \"%s\"", mod_dir(), g_dir.c_str());
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                        mod_dir(), &si, &pi)) {
        snprintf(g_status, sizeof(g_status), "cannot start r2read.exe (%lu)", GetLastError());
        log("r2: %s", g_status);
        return 1;
    }
    CloseHandle(in_r);
    CloseHandle(out_w);
    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;
    g_in = in_w;
    g_out = out_r;
    std::string head;
    std::vector<uint8_t> body;
    if (!ask("HELLO", &head, &body)) {
        snprintf(g_status, sizeof(g_status), "r2read could not open Remnant II: %s", head.c_str());
        log("r2: %s", g_status);
        return 1;
    }
    log("r2: %s (%s)", head.c_str(), g_dir.c_str());
    prefetch_all();
    g_ready = true;
    snprintf(g_status, sizeof(g_status), "reading Remnant II from your install");
    int bad = 0, ok = r2_cells_checked(&bad);
    log("r2: %d text cells resolved, %d unavailable", ok, bad);
    for (;;) {
        WaitForSingleObject(g_wake, 500);
        if (!g_proc) break;
        for (;;) {
            std::pair<std::string, std::string> tj;
            {
                std::lock_guard<std::mutex> q(g_queueLock);
                if (g_textQueue.empty()) break;
                tj = g_textQueue.front();
            }
            fetch_text(tj.first, tj.second);
            std::lock_guard<std::mutex> q(g_queueLock);
            g_textQueue.erase(g_textQueue.begin());
        }
        for (;;) {
            std::pair<std::string, int> job;
            {
                std::lock_guard<std::mutex> q(g_queueLock);
                if (g_imgQueue.empty()) break;
                job = g_imgQueue.front();
            }
            fetch_image(job.first, job.second);
            std::lock_guard<std::mutex> q(g_queueLock);
            g_imgQueue.erase(g_imgQueue.begin());
        }
    }
    return 0;
}

bool r2_start() {
    g_dir = find_remnant();
    if (g_dir.empty()) {
        snprintf(g_status, sizeof(g_status), "Remnant II install not found");
        log("r2: %s (set REMNANTHUNT_R2 or [paths] remnant2= in remnant_hunt.ini)", g_status);
        return false;
    }
    g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HANDLE t = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    return true;
}

void r2_stop() {
    std::lock_guard<std::mutex> l(g_pipeLock);
    if (g_proc) {
        DWORD w;
        WriteFile(g_in, "QUIT\n", 5, &w, nullptr);
        if (WaitForSingleObject(g_proc, 1000) != WAIT_OBJECT_0) TerminateProcess(g_proc, 0);
        CloseHandle(g_proc);
        CloseHandle(g_in);
        CloseHandle(g_out);
        g_proc = nullptr;
    }
    if (g_wake) SetEvent(g_wake);
}

}  // namespace rh
