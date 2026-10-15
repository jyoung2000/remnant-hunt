// Remnant x MHW mashup core: MHW engine API (found by pattern) and a read-only diagnostic. GPL-3.0-or-later.
//
// The diagnostic takes no input. When a file named dev_diag exists in the mod folder, it writes a fixed report once
// per session, after the hunter reaches town: what the API lookups resolved to, the item pouch, item names and item
// data headers. It only reads game memory.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core.h"
#include "state.h"

namespace rh {

static std::string g_out;
static std::vector<int> g_reportIds;  // item pouch ids seen by the report
static void outf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void outf(const char* fmt, ...) {
    char b[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    g_out += b;
    g_out += "\n";
}

static uintptr_t g_base = 0, g_textBeg = 0, g_textEnd = 0;
static void module_range() {
    if (g_base) return;
    g_base = (uintptr_t)GetModuleHandleA(nullptr);
    auto* dos = (IMAGE_DOS_HEADER*)g_base;
    auto* nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (memcmp(sec[i].Name, ".text", 5) == 0) {
            g_textBeg = g_base + sec[i].VirtualAddress;
            g_textEnd = g_textBeg + sec[i].Misc.VirtualSize;
        }
}

// "48 8B ?? 05" -> bytes + mask
static bool parse_pattern(const std::string& p, std::vector<int>* out) {
    out->clear();
    for (size_t i = 0; i < p.size();) {
        while (i < p.size() && p[i] == ' ') i++;
        if (i >= p.size()) break;
        if (p[i] == '?') {
            out->push_back(-1);
            i += (i + 1 < p.size() && p[i + 1] == '?') ? 2 : 1;
        } else {
            out->push_back((int)strtol(p.substr(i, 2).c_str(), nullptr, 16));
            i += 2;
        }
    }
    return !out->empty();
}

std::vector<uintptr_t> scan_text(const std::string& pattern, int max) {
    module_range();
    std::vector<int> pat;
    std::vector<uintptr_t> hits;
    if (!parse_pattern(pattern, &pat)) return hits;
    const uint8_t* b = (const uint8_t*)g_textBeg;
    size_t n = g_textEnd - g_textBeg, m = pat.size();
    for (size_t i = 0; i + m <= n && (int)hits.size() < max; i++) {
        if (pat[0] >= 0 && b[i] != pat[0]) continue;
        size_t k = 1;
        while (k < m && (pat[k] < 0 || b[i + k] == pat[k])) k++;
        if (k == m) hits.push_back(g_textBeg + i);
    }
    return hits;
}

// MHW's CRC32 (standard reflected table, init 0xFFFFFFFF, no final xor) for DTI ids.
uint32_t mt_crc(const char* s) {
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++) c = (c >> 8) ^ t[(c ^ (uint8_t)*s) & 0xFF];
    return c;
}

// ---------------------------------------------------------------- game functions found by pattern (SPL records)

typedef void*(__fastcall* FindDti_t)(uint32_t id);
typedef void*(__fastcall* GetResource_t)(void* mgr, void* dti, const char* path, int flags);
static FindDti_t g_findDti = nullptr;
static GetResource_t g_getResource = nullptr;
static uintptr_t g_sResource = 0;
static uintptr_t g_getResourceSpl = 0, g_selectSlot = 0;

static uintptr_t rip_target(uintptr_t insn, int dispOffset, int len) {
    int32_t d = *(int32_t*)(insn + dispOffset);
    return insn + len + d;
}

// Start of the function containing a: MSVC aligns functions to 16 bytes and pads with int3 (or ends on ret).
static uintptr_t func_start(uintptr_t a) {
    for (uintptr_t p = a & ~(uintptr_t)15; p > a - 0x800 && p > g_textBeg; p -= 16) {
        uint8_t prev = *(uint8_t*)(p - 1);
        if (prev == 0xCC || prev == 0xC3) return p;
    }
    return 0;
}

// The static most often loaded into rcx (mov rcx,[rip+x]) shortly before a call or jump to fn.
static uintptr_t static_before_calls(uintptr_t fn, int* calls, int* agree) {
    std::map<uintptr_t, int> votes;
    *calls = 0;
    *agree = 0;
    const uint8_t* b = (const uint8_t*)g_textBeg;
    size_t n = g_textEnd - g_textBeg;
    for (size_t i = 64; i + 5 < n; i++) {
        if (b[i] != 0xE8 && b[i] != 0xE9) continue;
        uintptr_t p = g_textBeg + i;
        if (rip_target(p, 1, 5) != fn) continue;
        (*calls)++;
        for (uintptr_t q = p - 7; q > p - 64; q--) {
            const uint8_t* c = (const uint8_t*)q;
            if (c[0] == 0x48 && c[1] == 0x8B && c[2] == 0x0D) {
                votes[rip_target(q, 3, 7)]++;
                break;
            }
        }
    }
    uintptr_t best = 0;
    for (auto& v : votes)
        if (v.second > *agree) { *agree = v.second; best = v.first; }
    return best;
}

// ---------------------------------------------------------------- singletons, found read-only
// Every MtObject's vtable slot 4 is GetDTI, compiled as `lea rax,[rip+dti]; ret`. Decoding those bytes (never calling
// them) names the class of any object; the .data pointers to objects whose class starts with 's' are the singletons.

static uintptr_t g_dataBeg = 0, g_dataEnd = 0;
static std::map<std::string, uintptr_t> g_singletons;

static bool in_image(const void* p);

static bool readable_cached(uintptr_t a, size_t len, std::map<uintptr_t, std::pair<uintptr_t, bool>>& regions) {
    auto it = regions.upper_bound(a);
    if (it != regions.begin()) {
        --it;
        if (a >= it->first && a + len <= it->second.first) return it->second.second;
    }
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)a, &mbi, sizeof(mbi))) return false;
    uintptr_t b = (uintptr_t)mbi.BaseAddress, e = b + mbi.RegionSize;
    DWORD pr = mbi.Protect;
    bool ok = mbi.State == MEM_COMMIT && !(pr & (PAGE_GUARD | PAGE_NOACCESS)) &&
              (pr & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                     PAGE_EXECUTE_WRITECOPY));
    regions[b] = {e, ok};
    return a + len <= e ? ok : (ok && mem_readable((void*)a, len));
}

static const char* dti_name_of_vtable(uintptr_t vt) {
    if (!in_image((void*)vt) || !mem_readable((void*)vt, 5 * 8)) return nullptr;
    uintptr_t f = ((const uintptr_t*)vt)[4];
    if (f < g_textBeg || f + 8 > g_textEnd) return nullptr;
    const uint8_t* c = (const uint8_t*)f;
    if (!(c[0] == 0x48 && c[1] == 0x8D && c[2] == 0x05 && c[7] == 0xC3)) return nullptr;
    uint8_t* dti = (uint8_t*)(f + 7 + *(const int32_t*)(f + 3));
    if (!mem_readable(dti + 8, 8)) return nullptr;
    const char* n = *(const char**)(dti + 8);
    if (!n || !in_image(n) || !mem_readable(n, 2)) return nullptr;
    return n;
}

const char* dti_name_of(const void* obj) {
    if (!obj || !mem_readable(obj, 8)) return nullptr;
    return dti_name_of_vtable(*(const uintptr_t*)obj);
}

static void scan_singletons() {
    auto* dos = (IMAGE_DOS_HEADER*)g_base;
    auto* nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (memcmp(sec[i].Name, ".data", 6) == 0) {
            g_dataBeg = g_base + sec[i].VirtualAddress;
            g_dataEnd = g_dataBeg + sec[i].Misc.VirtualSize;
        }
    if (!g_dataBeg) return;
    std::map<uintptr_t, std::pair<uintptr_t, bool>> regions;
    int objects = 0;
    for (uintptr_t a = g_dataBeg; a + 8 <= g_dataEnd; a += 8) {
        if (!readable_cached(a, 8, regions)) { a = (a | 0xFFF); a -= 7; continue; }
        uintptr_t v = *(const uintptr_t*)a;
        if (v < 0x10000 || (v & 7) || v > 0x7FFFFFFFFFFFull || in_image((void*)v)) continue;
        if (!readable_cached(v, 8, regions)) continue;
        // The object may be freed by a game thread at any moment: read its vtable without risking a fault.
        uintptr_t vt = 0;
        SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), (void*)v, &vt, 8, &got) || got != 8) continue;
        if (!in_image((void*)vt)) continue;
        const char* n = dti_name_of_vtable(vt);
        if (!n) continue;
        objects++;
        if (n[0] == 's' && !g_singletons.count(n)) g_singletons[n] = a;
    }
    log("api: %d objects referenced from .data, %zu singletons", objects, g_singletons.size());
}

uintptr_t singleton_static(const char* name) {
    auto it = g_singletons.find(name);
    return it == g_singletons.end() ? 0 : it->second;
}

static std::mutex g_apiLock;
static std::atomic<bool> g_apiDone{false};
static void ud_sources();

// Each lookup stands on its own; returns true when all of them resolved. Only reads memory (code, data and the
// objects they point at), so it can run on a worker thread; the first caller does the work, others wait for it.
bool game_api_init() {
    std::lock_guard<std::mutex> lk(g_apiLock);
    static bool done = false, ok = false;
    if (done) return ok;
    done = true;
    module_range();
    auto one = [](const char* pat, int off) -> uintptr_t {
        auto h = scan_text(pat, 2);
        return h.size() == 1 ? h[0] + off : 0;
    };
    scan_singletons();
    g_findDti = (FindDti_t)one("39 48 34 74 0B 48 8B 40 28", -19);
    uintptr_t hit = one("48 83 EC 30 80 B9 7B 80 20 00 00", 0);
    if (hit) {
        g_getResourceSpl = hit - 26;
        uintptr_t fs = func_start(hit);
        // Prefer whichever start actually has callers.
        int c1 = 0, a1 = 0, c2 = 0, a2 = 0;
        uintptr_t s1 = static_before_calls(g_getResourceSpl, &c1, &a1);
        uintptr_t s2 = fs && fs != g_getResourceSpl ? static_before_calls(fs, &c2, &a2) : 0;
        log("api: GetResource spl %p (%d calls, static %p x%d) aligned %p (%d calls, static %p x%d)",
            (void*)g_getResourceSpl, c1, (void*)s1, a1, (void*)fs, c2, (void*)s2, a2);
        // A static counts only when several callers agree on it.
        if (a2 > a1) { g_getResource = (GetResource_t)fs; g_sResource = a2 >= 3 ? s2 : 0; }
        else { g_getResource = (GetResource_t)g_getResourceSpl; g_sResource = a1 >= 3 ? s1 : 0; }
    }
    if (uintptr_t st = singleton_static("sMhResource")) {
        log("api: sMhResource static %p (by class name)", (void*)st);
        g_sResource = st;
    }
    if (!g_sResource) {
        // The constructor is called right after the allocation, and its result is stored to the static.
        uintptr_t chit = one("48 8D 9F 1A 9F 21 00 48 89 AF D0 9C 21 00 8B F5 48 89 AF 10 9D 21 00", 0);
        uintptr_t starts[2] = {chit ? chit - 54 : 0, chit ? func_start(chit) : 0};
        std::map<uintptr_t, int> votes;
        int calls = 0;
        const uint8_t* b = (const uint8_t*)g_textBeg;
        size_t n = g_textEnd - g_textBeg;
        for (size_t i = 64; chit && i + 64 < n; i++) {
            if (b[i] != 0xE8) continue;
            uintptr_t p = g_textBeg + i, t = rip_target(p, 1, 5);
            if (t != starts[0] && t != starts[1]) continue;
            calls++;
            for (uintptr_t q = p + 5; q < p + 48; q++) {
                const uint8_t* c = (const uint8_t*)q;
                if (c[0] == 0x48 && c[1] == 0x89 && (c[2] == 0x05 || c[2] == 0x1D || c[2] == 0x35 || c[2] == 0x3D)) {
                    votes[rip_target(q, 3, 7)]++;
                    break;
                }
            }
        }
        int best = 0;
        uintptr_t st = 0;
        for (auto& v : votes)
            if (v.second > best) { best = v.second; st = v.first; }
        uint8_t* o = st ? rd_ptr(st) : nullptr;
        bool plausible = o && mem_readable(o + 0x20807B, 1);
        log("api: ResourceManager ctor %p/%p calls %d static %p x%d plausible %d", (void*)starts[0], (void*)starts[1],
            calls, (void*)st, best, plausible ? 1 : 0);
        if (plausible) g_sResource = st;
    }
    g_selectSlot = one("89 91 A0 00 00 00 48 8B 0D ?? ?? ?? ?? E9", 0);
    ok = g_findDti && g_getResource && g_sResource;
    log("api: FindDti %p GetResource %p sMhResource %p SelectSlot %p -> %s", (void*)g_findDti,
        (void*)g_getResource, (void*)g_sResource, (void*)g_selectSlot, ok ? "ok" : "partial");
    ud_sources();
    g_apiDone = true;
    return ok;
}

// Starts the lookups on a worker thread (they scan the game's code and data for a second or two).
void game_api_start() {
    static std::once_flag once;
    std::call_once(once, [] {
        HANDLE h = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { game_api_init(); return 0; }, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    });
}
bool game_api_ready() { return g_apiDone; }

static bool looks_like_func(uintptr_t f) {
    if (!f || (f & 15) || f <= g_textBeg || f >= g_textEnd) return false;
    uint8_t prev = *(uint8_t*)(f - 1);
    return prev == 0xCC || prev == 0xC3;
}

static bool in_image(const void* p) {
    auto* dos = (IMAGE_DOS_HEADER*)g_base;
    auto* nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew);
    uintptr_t a = (uintptr_t)p;
    return a >= g_base && a < g_base + nt->OptionalHeader.SizeOfImage;
}

void* find_dti(const char* name) {
    if (!looks_like_func((uintptr_t)g_findDti)) return nullptr;
    uint8_t* d = (uint8_t*)g_findDti(mt_crc(name) & 0x7FFFFFFF);
    if (!d || !mem_readable(d + 8, 8)) return nullptr;
    const char* n = *(const char**)(d + 8);
    return n && mem_readable(n, strlen(name) + 1) && !strcmp(n, name) ? d : nullptr;
}

// Game thread only.
void* get_resource(const char* dtiName, const char* path) {
    if (!g_apiDone) return nullptr;
    if (!looks_like_func((uintptr_t)g_getResource) || !g_sResource) return nullptr;
    void* dti = find_dti(dtiName);
    uint8_t* mgr = rd_ptr(g_sResource);
    if (!dti || !mgr || !mem_readable(mgr, 8) || !in_image(*(void**)mgr)) return nullptr;
    return g_getResource(mgr, dti, path, 1);
}

// ---------------------------------------------------------------- save data (cUserData)
// sUserData: +0xA0 current slot, +0xA8 first slot, 0x26CC00 per slot (SharpPluginLoader); in a slot, +0x50 hunter
// name and +0x94 zenny (HunterPie). The static is found three ways and each answer is checked against the save.

static bool plausible_save(uint8_t* u) {
    if (!u || !mem_readable(u + 0x50, 0x48) || !mem_readable(u + 0x38080, 0x500)) return false;
    const unsigned char* name = (const unsigned char*)(u + 0x50);
    size_t len = strnlen((const char*)name, 0x40);
    if (len == 0 || len >= 0x40) return false;
    for (size_t i = 0; i < len; i++)
        if (name[i] < 0x20) return false;
    int zenny = *(int*)(u + 0x94);
    return zenny >= 0 && zenny <= 9999999;
}

static uint8_t* slot_via(uintptr_t st) {
    if (!st) return nullptr;
    uint8_t* s = rd_ptr(st);
    if (!s || !mem_readable(s + 0xA0, 0x10)) return nullptr;
    int slot = *(int*)(s + 0xA0);
    uint8_t* first = *(uint8_t**)(s + 0xA8);
    if (slot < 0 || slot > 2 || !first) return nullptr;
    uint8_t* u = first + (size_t)slot * 0x26CC00;
    return plausible_save(u) ? u : nullptr;
}

struct UdSource { const char* how; uintptr_t st; };
static std::vector<UdSource> g_udSources;
static int g_udChosen = -1;

static void ud_sources() {  // inside game_api_init
    if (!g_udSources.empty()) return;
    uintptr_t rebase = g_base - 0x140000000ull;
    if (uintptr_t st = singleton_static("sUserData")) g_udSources.push_back({"class name sUserData", st});
    if (g_selectSlot) {
        int calls = 0, agree = 0;
        uintptr_t st = static_before_calls(g_selectSlot, &calls, &agree);
        log("api: SelectSlot callers %d, static %p x%d", calls, (void*)st, agree);
        if (st) g_udSources.push_back({"SelectSlot callers", st});
        g_udSources.push_back({"SelectSlot tail", rip_target(g_selectSlot + 6, 3, 7)});
    }
    g_udSources.push_back({"HunterPie LEVEL_OFFSET", 0x145013950ull + rebase});
}

uint8_t* user_data() {
    if (!g_apiDone) return nullptr;
    if (g_udChosen >= 0) {
        if (uint8_t* u = slot_via(g_udSources[g_udChosen].st)) return u;
        return nullptr;  // between saves (title screen): keep the source
    }
    for (size_t i = 0; i < g_udSources.size(); i++)
        if (uint8_t* u = slot_via(g_udSources[i].st)) {
            g_udChosen = (int)i;
            log("api: sUserData %p via %s", (void*)g_udSources[i].st, g_udSources[i].how);
            return u;
        }
    return nullptr;
}

// GMD message by id (rGUIMessage: +0xD8 count, +0xF8 char** messages).
const char* gmd_message(void* gmd, int id) {
    uint8_t* g = (uint8_t*)gmd;
    if (!g || !mem_readable(g + 0xD8, 0x28)) return nullptr;
    int count = *(int*)(g + 0xD8);
    char** msgs = *(char***)(g + 0xF8);
    if (id < 0 || id >= count || !msgs || !mem_readable(msgs + id, 8)) return nullptr;
    return msgs[id];
}

// ---------------------------------------------------------------- read-only diagnostic report

static void hexdump(uintptr_t a, size_t n) {
    for (size_t i = 0; i < n; i += 16) {
        if (!mem_readable((void*)(a + i), 16)) { outf("%p: <unreadable>", (void*)(a + i)); return; }
        char line[128];
        int k = snprintf(line, sizeof(line), "%p:", (void*)(a + i));
        for (int j = 0; j < 16; j++) k += snprintf(line + k, sizeof(line) - k, " %02X", *(uint8_t*)(a + i + j));
        k += snprintf(line + k, sizeof(line) - k, "  ");
        for (int j = 0; j < 16; j++) {
            uint8_t c = *(uint8_t*)(a + i + j);
            line[k++] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        line[k] = 0;
        outf("%s", line);
    }
}

static void walk_dti(uint8_t* d, const char* const* filters, int depth, int* n) {
    while (d && mem_readable(d, 0x38) && *n < 300) {
        const char* name = *(const char**)(d + 8);
        if (name && mem_readable(name, 4))
            for (const char* const* f = filters; *f; f++)
                if (strstr(name, *f)) {
                    outf("dti %s size 0x%X", name, ((*(uint32_t*)(d + 0x30)) & 0x7FFFFF) << 2);
                    (*n)++;
                    break;
                }
        if (depth < 40) walk_dti(*(uint8_t**)(d + 0x18), filters, depth + 1, n);
        d = *(uint8_t**)(d + 0x10);
    }
}

// Time-scale hunt: floats == 1.0f stable over 2 s with a frame-delta-like float (0.001..0.1) within +-0x20.
static void time_scale_candidates() {
    outf("-- time scale candidates");
    for (auto& kv : g_singletons) {
        const std::string& n = kv.first;
        for (const char* k : {"Time", "Main", "Scene", "Game", "Pause", "Step"})
            if (n.find(k) != std::string::npos) { outf("time-ish singleton: %s static %p obj %p", n.c_str(), (void*)kv.second, (void*)rd_ptr(kv.second)); break; }
    }
    struct S { std::string name; uint8_t* o; std::vector<uint8_t> a; };
    std::vector<S> v;
    for (auto& kv : g_singletons) {
        uint8_t* o = rd_ptr(kv.second);
        if (!o || !mem_readable(o, 0x1000)) continue;
        v.push_back({kv.first, o, std::vector<uint8_t>(o, o + 0x1000)});
    }
    Sleep(2000);
    int printed = 0;
    for (auto& s : v) {
        if (!mem_readable(s.o, 0x1000)) continue;
        const uint8_t* b = s.o;
        for (size_t off = 0; off + 4 <= 0x1000 && printed < 40; off += 4) {
            float f1, f2;
            memcpy(&f1, &s.a[off], 4); memcpy(&f2, b + off, 4);
            if (f1 != 1.0f || f2 != 1.0f) continue;
            for (int d = -0x20; d <= 0x20; d += 4) {
                long o2 = (long)off + d;
                if (!d || o2 < 0 || o2 + 4 > 0x1000) continue;
                float g;
                memcpy(&g, b + o2, 4);
                if (g >= 0.001f && g <= 0.1f) {
                    outf("time scale candidate: %s +0x%zX (neighbour delta %g at +0x%lX)", s.name.c_str(), off, g, o2);
                    printed++;
                    break;
                }
            }
        }
    }
    outf("time scale candidates printed: %d", printed);
}

// Sun hunt: float3 unit vectors in light/sky/scene-like singletons, sampled 2 s apart. The sun moves slowly (small
// change) or, when static, points up (y > 0.2). Use as [visuals] sun_from=<singleton>+0x<off>.
static void light_dir_candidates() {
    outf("-- light direction candidates");
    struct S { std::string name; uint8_t* o; std::vector<uint8_t> a; };
    std::vector<S> v;
    for (auto& kv : g_singletons) {
        bool match = false;
        for (const char* k : {"Render", "Scene", "Light", "Sky", "Env", "Shadow", "Camera", "World"})
            if (kv.first.find(k) != std::string::npos) { match = true; break; }
        if (!match) continue;
        uint8_t* o = rd_ptr(kv.second);
        if (!o || !mem_readable(o, 0x2000)) continue;
        v.push_back({kv.first, o, std::vector<uint8_t>(o, o + 0x2000)});
    }
    Sleep(2000);
    int printed = 0;
    for (auto& s : v) {
        if (!mem_readable(s.o, 0x2000)) continue;
        for (size_t off = 0; off + 12 <= 0x2000 && printed < 40; off += 4) {
            float a[3], b[3];
            memcpy(a, &s.a[off], 12); memcpy(b, s.o + off, 12);
            auto unit = [](const float* f) {
                if (!(fabsf(f[0]) < 2 && fabsf(f[1]) < 2 && fabsf(f[2]) < 2)) return false;
                float l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
                return l >= 0.98f && l <= 1.02f;
            };
            if (!unit(a) || !unit(b)) continue;
            float d = fabsf(a[0] - b[0]) + fabsf(a[1] - b[1]) + fabsf(a[2] - b[2]);
            bool moved = d > 1e-6f && d < 0.05f;
            bool still = d <= 1e-6f && fabsf(b[1]) > 0.2f;
            if (!moved && !still) continue;
            outf("light dir candidate: %s +0x%zX (%.3f,%.3f,%.3f) -> (%.3f,%.3f,%.3f)", s.name.c_str(), off, a[0], a[1], a[2], b[0], b[1], b[2]);
            printed++;
        }
    }
    outf("light dir candidates printed: %d", printed);
}

static void report() {
    outf("== Remnant Hunt diagnostic (read-only) ==");
    outf("api %s", game_api_init() ? "ok" : "missing");
    module_range();
    outf("module base %p text %p-%p", (void*)g_base, (void*)g_textBeg, (void*)g_textEnd);
    outf("-- singletons (%zu)", g_singletons.size());
    for (auto& kv : g_singletons) outf("singleton %s static %p obj %p", kv.first.c_str(), (void*)kv.second, (void*)rd_ptr(kv.second));
    {
        uint8_t* pl = player_ptr();
        const char* pn = dti_name_of(pl);
        outf("player %p class %s", (void*)pl, pn ? pn : "?");
    }
    uint8_t* u = user_data();
    outf("userdata %p (source %s)", (void*)u, g_udChosen >= 0 ? g_udSources[g_udChosen].how : "none");
    for (const UdSource& src : g_udSources) {
        uint8_t* o = rd_ptr(src.st);
        outf("-- source %s: static %p -> %p", src.how, (void*)src.st, (void*)o);
        if (o && mem_readable(o + 0xA0, 0x10)) {
            uint8_t* first = *(uint8_t**)(o + 0xA8);
            outf("   +A0 %d  +A8 %p", *(int*)(o + 0xA0), (void*)first);
            for (int k = 0; k < 3 && first; k++) {
                uint8_t* sl = first + (size_t)k * 0x26CC00;
                outf("   slot %d %p plausible %d name '%.32s' zenny %d", k, (void*)sl, plausible_save(sl) ? 1 : 0,
                     mem_readable(sl + 0x50, 0x20) ? (const char*)(sl + 0x50) : "?",
                     mem_readable(sl + 0x94, 4) ? *(int*)(sl + 0x94) : -1);
            }
        }
    }
    std::vector<int> pouchIds;
    if (u) {
        outf("zenny %d", *(int*)(u + 0x94));
        const char* pouches[] = {"item", "ammo", "material", "special"};
        const size_t offs[] = {0x38080, 0x38200, 0x38380, 0x38500};
        const int counts[] = {24, 24, 24, 5};
        for (int k = 0; k < 4; k++)
            for (int i = 0; i < counts[k]; i++) {
                uint8_t* e = u + offs[k] + i * 0x10;
                if (!mem_readable(e, 16)) break;
                int id = *(int*)(e + 8), c = *(int*)(e + 12);
                outf("%s[%d] id %d x%d vt %p", pouches[k], i, id, c, *(void**)e);
                if (id) pouchIds.push_back(id);
                if (id && k == 0) g_reportIds.push_back(id);
            }
        void* vt = *(void**)(u + 0x38080);
        bool slotReadable = mem_readable(u, 0x26CC00);
        outf("-- item containers in the save slot (entries with the pouch's vtable %p)", vt);
        size_t runStart = 0, runLen = 0;
        int runNonzero = 0;
        for (size_t off = 0; slotReadable && off + 16 <= 0x26CC00; off += 8) {
            uint8_t* e = u + off;
            bool hit = *(void**)e == vt;
            if (hit && runLen && off == runStart + runLen * 16) {
                runLen++;
                if (*(int*)(e + 12)) runNonzero++;
                off += 8;
                continue;
            }
            if (runLen >= 4) outf("container +0x%zX entries %zu (with items %d)", runStart, runLen, runNonzero);
            runLen = 0;
            if (hit) { runStart = off; runLen = 1; runNonzero = *(int*)(e + 12) ? 1 : 0; off += 8; }
        }
        if (runLen >= 4) outf("container +0x%zX entries %zu (with items %d)", runStart, runLen, runNonzero);
        outf("-- userdata around the pouch");
        hexdump((uintptr_t)(u + 0x38040), 0x60);
    }
    const char* filters[] = {"Item", "GUI", "Pouch", nullptr};
    int n = 0;
    void* root = find_dti("MtObject");
    outf("-- DTIs (root %p)", root);
    if (root) walk_dti((uint8_t*)root, filters, 0, &n);
    void* gmd = nullptr;
    const char* gmdPaths[] = {"common\\text\\steam\\item_eng", "common\\text\\item_eng", "common\\text\\steam\\item_eng.gmd"};
    for (const char* gp : gmdPaths) {
        gmd = get_resource("rGUIMessage", gp);
        outf("gmd %s -> %p", gp, gmd);
        if (gmd) break;
    }
    if (gmd) {
        int cnt = mem_readable((uint8_t*)gmd + 0xD8, 4) ? *(int*)((uint8_t*)gmd + 0xD8) : 0;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\dev_items.txt", mod_dir());
        if (FILE* f = fopen(path, "wb")) {
            for (int i = 0; i < cnt && i < 40000; i++) {
                const char* m = gmd_message(gmd, i);
                fprintf(f, "%d\t%s\n", i, m && mem_readable(m, 1) ? m : "<null>");
            }
            fclose(f);
        }
    }
    int count = gmd && mem_readable((uint8_t*)gmd + 0xD8, 4) ? *(int*)((uint8_t*)gmd + 0xD8) : -1;
    outf("-- item names gmd %p count %d", gmd, count);
    for (int id : pouchIds) {
        const char* m = gmd_message(gmd, id * 3);
        outf("name(id %d -> msg %d) %s", id, id * 3, m && mem_readable(m, 1) ? m : "<null>");
    }
    for (int i = 0; gmd && i < 12; i++) {
        const char* m = gmd_message(gmd, i);
        outf("msg %d %s", i, m && mem_readable(m, 1) ? m : "<null>");
    }
    int unavailable = 0;
    for (int i = 0; gmd && i < count; i += 3) {
        const char* m = gmd_message(gmd, i);
        if (m && mem_readable(m, 1) && (!strcmp(m, "Unavailable") || !m[0])) {
            if (unavailable < 60) outf("unused name at msg %d (item id %d?) '%s'", i, i / 3, m);
            unavailable++;
        }
    }
    outf("unused-looking item names: %d", unavailable);
    const char* dataDtis[] = {"rItemData", "rItem", nullptr};
    for (const char* const* d = dataDtis; *d; d++) {
        void* r = get_resource(*d, "common\\item\\itemData");
        outf("-- resource %s common\\item\\itemData = %p", *d, r);
        if (r) hexdump((uintptr_t)r, 0x100);
    }
    time_scale_candidates();
    light_dir_candidates();
    outf("== end ==");
}


// ---------------------------------------------------------------- item bar watch (read-only)
// For 150 s after the report, the player object, the save slot and the first 0x4000 bytes of every singleton are
// sampled every 0.25 s. Every 4-byte field that changes is counted; the last 16 values and the times of the first 12
// changes are kept (from a pool preallocated in watch_start). The report lists fields that changed 2..60 times and
// only ever held small values (an index), pouch item ids, or bytes (0..255): candidates for the item bar's current slot.

struct WatchRec {
    uint32_t vals[16];
    float times[12];
};
struct Watch {
    uint8_t* base = nullptr;
    size_t size = 0;
    std::string name;
    std::vector<uint32_t> last, rec;       // rec: 1-based index into g_recs, 0 = none
    std::vector<uint8_t> changes, flags;   // flags: 1 small 0..31, 2 pouch id, 4 byte 0..255 (true so far for every value)
};
static std::vector<Watch> g_watch;
static std::vector<WatchRec> g_recs;
static std::vector<uint32_t> g_recFree;
static std::vector<int> g_watchIds;
static std::string g_watchNotes;
static double g_watchStart = 0, g_watchUntil = 0, g_watchNext = 0;
static const size_t kWatchCap = 3u << 20;   // total watched bytes
static const double kWatchSecs = 150.0, kWatchStep = 0.25;

static bool is_pouch_id(uint32_t v) {
    return std::find(g_watchIds.begin(), g_watchIds.end(), (int)v) != g_watchIds.end();
}
static uint8_t val_flags(uint32_t v) { return (uint8_t)((v < 32 ? 1 : 0) | (is_pouch_id(v) ? 2 : 0) | (v < 256 ? 4 : 0)); }

static void watch_add(uint8_t* base, size_t size, const std::string& name, size_t* total) {
    if (!base) return;
    if (*total + size > kWatchCap) { outf("watch: skipped %s (%zu bytes would exceed the %zu byte cap)", name.c_str(), size, kWatchCap); return; }
    if (!mem_readable(base, size)) { outf("watch: skipped %s (not readable)", name.c_str()); return; }
    for (const Watch& o : g_watch) if (o.base == base) return;
    Watch w;
    w.base = base;
    w.size = size;
    w.name = name;
    size_t n = size / 4;
    w.last.assign((uint32_t*)base, (uint32_t*)base + n);
    w.rec.assign(n, 0);
    w.changes.assign(n, 0);
    w.flags.resize(n);
    for (size_t i = 0; i < n; i++) w.flags[i] = val_flags(w.last[i]);
    *total += size;
    g_watch.push_back(std::move(w));
}

static void watch_start(uint8_t* player, uint8_t* u, const std::vector<int>& ids, double now) {
    g_watchIds = ids;
    g_out.clear();
    g_watch.clear();
    g_watch.reserve(100);
    g_recs.assign(30000, WatchRec{});
    g_recFree.clear();
    g_recFree.reserve(g_recs.size());
    for (size_t i = g_recs.size(); i > 0; i--) g_recFree.push_back((uint32_t)i);   // 1-based
    size_t total = 0;
    watch_add(player, 0x20000, "player", &total);
    for (auto& kv : g_singletons) watch_add(rd_ptr(kv.second), 0x4000, kv.first, &total);
    watch_add(u, 0x26CC00, "save slot", &total);
    g_watchStart = now;
    g_watchUntil = now + kWatchSecs;
    g_watchNext = now + kWatchStep;
    outf("watch: %zu targets, %zu bytes", g_watch.size(), total);
    g_watchNotes = g_out;
    g_out.clear();
    Shared& sh = S();
    AcquireSRWLockExclusive(&sh.lock);
    toast(sh.live, "Remnant Hunt diagnostic: scroll the item bar with the arrow keys, then open and close the item bar");
    sh.live.toastUntil = now + 20.0;
    ReleaseSRWLockExclusive(&sh.lock);
}

static void watch_sample(double now) {
    float t = (float)(now - g_watchStart);
    for (Watch& w : g_watch) {
        if (!mem_readable(w.base, w.size)) continue;
        const uint32_t* cur = (const uint32_t*)w.base;
        for (size_t i = 0, n = w.last.size(); i < n; i++) {
            uint32_t v = cur[i];
            if (v == w.last[i]) continue;
            w.last[i] = v;
            uint8_t c = w.changes[i];
            if (c >= 61) continue;                    // too noisy to be an index
            w.changes[i] = ++c;
            w.flags[i] &= val_flags(v);
            if (c > 60) {                             // drop its history, it is out
                if (w.rec[i]) { g_recFree.push_back(w.rec[i]); w.rec[i] = 0; }
                continue;
            }
            if (!w.rec[i]) {
                if (g_recFree.empty()) continue;      // pool exhausted: counts only
                w.rec[i] = g_recFree.back();
                g_recFree.pop_back();
            }
            WatchRec& r = g_recs[w.rec[i] - 1];
            r.vals[(c - 1) & 15] = v;
            if (c <= 12) r.times[c - 1] = t;
        }
    }
}

static void watch_report() {
    g_out.clear();
    outf("== item bar watch ==");
    g_out += g_watchNotes;
    struct Cand { size_t w, i; int c; };
    std::vector<Cand> cands;
    for (size_t wi = 0; wi < g_watch.size(); wi++) {
        const Watch& w = g_watch[wi];
        for (size_t i = 0; i < w.last.size(); i++)
            if (w.changes[i] >= 2 && w.changes[i] <= 60 && w.flags[i] && w.rec[i]) cands.push_back({wi, i, w.changes[i]});
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.c < b.c; });
    outf("candidates: %zu (showing up to 200, fewest changes first)", cands.size());
    int shown = 0;
    for (const Cand& cd : cands) {
        if (shown++ >= 200) break;
        const Watch& w = g_watch[cd.w];
        const WatchRec& r = g_recs[w.rec[cd.i] - 1];
        std::string vs, ts;
        int n = cd.c, first = n > 16 ? n - 16 : 0;
        char b[32];
        for (int k = first; k < n; k++) { snprintf(b, sizeof(b), "%s%u", vs.empty() ? "" : ",", r.vals[k & 15]); vs += b; }
        for (int k = 0; k < n && k < 12; k++) { snprintf(b, sizeof(b), "%s%.1f", ts.empty() ? "" : ",", r.times[k]); ts += b; }
        outf("%s +0x%zX changes %d values %s at %ss", w.name.c_str(), cd.i * 4, cd.c, vs.c_str(), ts.c_str());
    }
    // Best lead: fields that only ever held pouch item ids and changed at least 3 times.
    int lead = 0;
    for (const Cand& cd : cands) {
        const Watch& w = g_watch[cd.w];
        if (lead >= 40) break;
        if ((w.flags[cd.i] & 2) && cd.c >= 3) {
            outf("best lead: %s +0x%zX only pouch ids, changed %d times, now %u", w.name.c_str(), cd.i * 4, cd.c, w.last[cd.i]);
            lead++;
        }
    }
    outf("== end watch ==");
}

static void append_out() {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\dev_diag.txt", mod_dir());
    FILE* f = fopen(path, "ab");
    if (f) {
        fwrite(g_out.data(), 1, g_out.size(), f);
        fclose(f);
    }
}

static bool g_reported = false;

// Where the hunter stands still in town (to place NPC menus), and which actions and animations the hunter performs
// (to choose MHW animations for Remnant items).
static void track_session(double now, bool inBase, int zone, const Vec3& pos) {
    static double stillSince = 0;
    static Vec3 still{};
    static bool noted = false;
    static int spots = 0, actionLines = 0;
    static int lastSet = -1, lastId = -1;
    static uint32_t lastAnim = 0xFFFFFFFF;
    char line[160];
    if (inBase) {
        float dx = pos.x - still.x, dy = pos.y - still.y, dz = pos.z - still.z;
        if (stillSince == 0 || dx * dx + dy * dy + dz * dz > 40.0f * 40.0f) {
            stillSince = now;
            still = pos;
            noted = false;
        } else if (!noted && now - stillSince > 4.0) {
            noted = true;
            spots++;
            snprintf(line, sizeof(line), "spot %d zone %d pos %.0f %.0f %.0f", spots, zone, pos.x, pos.y, pos.z);
            g_out = std::string(line) + "\n";
            append_out();
            Shared& sh = S();
            AcquireSRWLockExclusive(&sh.lock);
            toast(sh.live, "Remnant Hunt diagnostic: spot %d noted", spots);
            ReleaseSRWLockExclusive(&sh.lock);
        }
    } else {
        stillSince = 0;
    }
    uint8_t* pl = player_ptr();
    if (!pl || actionLines > 3000 || !mem_readable(pl + 0x61C8 + 0xAC, 8) || !mem_readable(pl + 0x55B8, 4)) return;
    int set = *(int*)(pl + 0x61C8 + 0xAC), id = *(int*)(pl + 0x61C8 + 0xB0);
    uint32_t anim = *(uint32_t*)(pl + 0x55B8);
    if (set == lastSet && id == lastId && anim == lastAnim) return;
    lastSet = set;
    lastId = id;
    lastAnim = anim;
    snprintf(line, sizeof(line), "t %.1f zone %d action %d:%d anim %u.%u", now, zone, set, id, anim >> 12, anim & 0xFFF);
    g_out = std::string(line) + "\n";
    append_out();
    actionLines++;
}

// ---------------------------------------------------------------- menu flag ([menu] flag=) and the menu watch (read-only)
// [menu] flag=<singleton>+0x<off> in the ini names one byte inside a singleton's object that is non-zero while MHW's
// Item Box / equipment menu is open; ui.cpp then uses it instead of its input-rate heuristic.
int menu_flag_read() {
    static std::atomic<int> state{0};  // 0 not parsed, 1 unset, 2 set
    static char name[64];
    static size_t off = 0;
    int st = state.load();
    if (st == 0) {
        char ini[MAX_PATH], v[128] = {0};
        snprintf(ini, sizeof(ini), "%s\\remnant_hunt.ini", mod_dir());
        GetPrivateProfileStringA("menu", "flag", "", v, sizeof(v), ini);
        char* plus = strchr(v, '+');
        st = 1;
        if (plus && plus > v && plus - v < (int)sizeof(name)) {
            memcpy(name, v, plus - v);
            name[plus - v] = 0;
            off = (size_t)strtoul(plus + 1, nullptr, 0);
            if (off < 0x100000) st = 2;
        }
        state.store(st);
    }
    if (st != 2 || !game_api_ready()) return -1;
    uintptr_t stc = singleton_static(name);
    if (!stc) return -1;
    uint8_t* obj = rd_ptr(stc);
    if (!obj || !mem_readable(obj + off, 1)) return -1;
    return obj[off];
}

// For 120 s after the hunter stands still in town (and dev_diag exists), the first 0x4000 bytes of every singleton with
// GUI, Menu, Gui, Pad, Cursor or Item in its name are sampled every 0.25 s. Bytes that were 0 in every sample taken
// while ui.cpp's input-rate heuristic said "no menu" and 1 in every sample taken while it said "menu open" are printed
// as "menu flag candidate: <singleton> +0x<off>"; open and close the Item Box a few times while it runs.
struct MenuWatch {
    uint8_t* base = nullptr;
    size_t size = 0;
    std::string name;
    std::vector<uint8_t> badClosed, badOpen;   // 1 = seen non-zero while closed / seen not 1 while open
    std::vector<uint16_t> nClosed, nOpen;
};
static std::vector<MenuWatch> g_mw;
static double g_mwUntil = 0, g_mwNext = 0, g_mwStable = 0;
static bool g_mwDone = false, g_mwLastOpen = false;

static bool mw_name_ok(const std::string& n) {
    for (const char* k : {"GUI", "Menu", "Gui", "Pad", "Cursor", "Item"})
        if (n.find(k) != std::string::npos) return true;
    return false;
}

static void menu_watch_tick(double now, bool inGame, bool inBase, const Vec3& pos) {
    static double stillSince = 0, flagChecked = 0;
    static bool flagFile = false;
    static Vec3 still{};
    if (g_mwDone) return;
    if (now - flagChecked > 5.0) {
        flagChecked = now;
        char f[MAX_PATH];
        snprintf(f, sizeof(f), "%s\\dev_diag", mod_dir());
        flagFile = GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES;
    }
    if (!flagFile) return;
    if (g_mwUntil > 0) {
        bool open = menu_heuristic_open();
        if (open != g_mwLastOpen) { g_mwLastOpen = open; g_mwStable = now + 1.0; }   // skip the swing right after a change
        if (now >= g_mwUntil) {
            g_mwDone = true;
            g_mwUntil = 0;
            g_out.clear();
            outf("== menu watch ==");
            int shown = 0;
            for (const MenuWatch& w : g_mw)
                for (size_t i = 0; i < w.size; i++)
                    if (w.nClosed[i] >= 4 && w.nOpen[i] >= 4 && !w.badClosed[i] && !w.badOpen[i] && shown++ < 60)
                        outf("menu flag candidate: %s +0x%zX", w.name.c_str(), i);
            outf("menu watch: %d candidates (a field needs 4+ samples closed and open; open and close the Item Box a few times)", shown);
            outf("== end menu watch ==");
            append_out();
            log("diagnostic menu watch written (%d candidates)", shown);
            std::vector<MenuWatch>().swap(g_mw);
        } else if (now >= g_mwNext) {
            g_mwNext = now + 0.25;
            if (now < g_mwStable) return;
            for (MenuWatch& w : g_mw) {
                if (!mem_readable(w.base, w.size)) continue;
                for (size_t i = 0; i < w.size; i++) {
                    uint8_t v = w.base[i];
                    if (open) { if (w.nOpen[i] < 60000) w.nOpen[i]++; if (v != 1) w.badOpen[i] = 1; }
                    else { if (w.nClosed[i] < 60000) w.nClosed[i]++; if (v != 0) w.badClosed[i] = 1; }
                }
            }
        }
        return;
    }
    if (!inGame || !inBase) { stillSince = 0; return; }
    float dx = pos.x - still.x, dy = pos.y - still.y, dz = pos.z - still.z;
    if (stillSince == 0 || dx * dx + dy * dy + dz * dz > 40.0f * 40.0f) { stillSince = now; still = pos; return; }
    if (now - stillSince < 4.0) return;
    game_api_start();
    if (!game_api_ready()) return;
    size_t total = 0;
    for (auto& kv : g_singletons) {
        if (!mw_name_ok(kv.first)) continue;
        uint8_t* b = rd_ptr(kv.second);
        if (!b || !mem_readable(b, 0x4000) || total + 0x4000 > (2u << 20)) continue;
        MenuWatch w;
        w.base = b;
        w.size = 0x4000;
        w.name = kv.first;
        w.badClosed.assign(w.size, 0);
        w.badOpen.assign(w.size, 0);
        w.nClosed.assign(w.size, 0);
        w.nOpen.assign(w.size, 0);
        total += w.size;
        g_mw.push_back(std::move(w));
    }
    g_mwUntil = now + 120.0;
    g_mwNext = now + 0.25;
    g_mwStable = now + 1.0;
    g_mwLastOpen = menu_heuristic_open();
    log("diagnostic menu watch: %zu singletons, %zu bytes, 120 s", g_mw.size(), total);
    Shared& sh = S();
    AcquireSRWLockExclusive(&sh.lock);
    toast(sh.live, "Remnant Hunt diagnostic: open and close the Item Box menu a few times (2 minutes)");
    sh.live.toastUntil = now + 15.0;
    ReleaseSRWLockExclusive(&sh.lock);
}

// ---------------------------------------------------------------- host diag (read-only)
// For the hidden "host" monster research (research/host_monster.md). While dev_diag exists, every 5 s each large
// monster in sEnemy's list is logged: address, DTI class, model path, em id/variant, position (+0x160), rotation
// quaternion (+0x170), the floats at +0x180..+0x1A0 (mScale is +0x180..+0x18B per the 15.11 DTI dump), the size
// modifier (+0x7730), mTransparency (+0x314), the cUnit words at +0x08..+0x17, the ActionController's {set, id}
// (+0x61C8+0xAC), HP, despawn timer, speed, enrage duration and the part table pointer. Once per session it also lists
// the functions called right after `mov rcx, [sEnemy]` (spawn routine candidates). Nothing is written.
static void host_diag_callers() {
    module_range();
    const uint8_t* b = (const uint8_t*)g_textBeg;
    size_t n = g_textEnd - g_textBeg;
    struct Hit { int count = 0; uint32_t edx = 0xFFFFFFFF, r8 = 0xFFFFFFFF; uintptr_t site = 0; };
    std::map<uintptr_t, Hit> hits;
    int loads = 0;
    for (size_t i = 0; i + 64 < n; i++) {
        // mov rcx, [rip+disp32] = 48 8B 0D disp32
        if (b[i] != 0x48 || b[i + 1] != 0x8B || b[i + 2] != 0x0D) continue;
        uintptr_t at = g_textBeg + i;
        if (at + 7 + *(const int32_t*)(b + i + 3) != HOOKV_S_ENEMY) continue;
        loads++;
        uint32_t edx = 0xFFFFFFFF, r8 = 0xFFFFFFFF;
        for (size_t k = i + 7; k < i + 48; k++) {
            if (b[k] == 0xBA) edx = *(const uint32_t*)(b + k + 1);                                 // mov edx, imm32
            if (b[k] == 0x41 && b[k + 1] == 0xB8) r8 = *(const uint32_t*)(b + k + 2);              // mov r8d, imm32
            if (b[k] == 0xE8) {
                uintptr_t t = g_textBeg + k + 5 + *(const int32_t*)(b + k + 1);
                if (t < g_textBeg || t >= g_textEnd) break;
                Hit& h = hits[t];
                if (!h.count) { h.edx = edx; h.r8 = r8; h.site = at; }
                h.count++;
                break;
            }
            if (b[k] == 0xC3) break;
        }
    }
    log("host diag: %d loads of sEnemy into rcx, %zu distinct call targets after them", loads, hits.size());
    int shown = 0;
    for (auto& kv : hits) {
        if (shown++ >= 60) break;
        char bytes[3 * 24 + 1] = {0};
        if (mem_readable((void*)kv.first, 24))
            for (int k = 0; k < 24; k++) snprintf(bytes + 3 * k, 4, "%02X ", ((const uint8_t*)kv.first)[k]);
        log("host diag: call %p (rva 0x%llX) x%d first site %p edx=0x%X r8d=0x%X : %s", (void*)kv.first,
            (unsigned long long)(kv.first - g_base), kv.second.count, (void*)kv.second.site, kv.second.edx,
            kv.second.r8, bytes);
    }
}

static void host_diag_tick(double now, bool inGame) {
    static double flagChecked = 0, next = 0;
    static bool flagFile = false, callersDone = false;
    if (now - flagChecked > 5.0) {
        flagChecked = now;
        char f[MAX_PATH];
        snprintf(f, sizeof(f), "%s\\dev_diag", mod_dir());
        flagFile = GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES;
    }
    if (!flagFile || !inGame || now < next) return;
    next = now + 5.0;
    module_range();
    if (!callersDone) { callersDone = true; host_diag_callers(); }
    uint8_t* se = rd_ptr(HOOKV_S_ENEMY);
    if (!se) return;
    const char* sen = dti_name_of(se);
    int large = 0;
    for (int i = 0; i < 128; i++) {
        uint8_t* ai = rd_ptr((uintptr_t)se + 0x38 + i * 8);
        if (!ai || !mem_readable(ai + 0x0C, 1) || (ai[0x0C] & 0xE) == 0) continue;
        uint8_t* em = rd_ptr((uintptr_t)ai + 0x138);
        if (!em || !mem_readable(em, 0x1D8B0)) continue;
        // Model path "em\emNNN\NN\..." (small monsters "em\ems...").
        char path[64] = {0};
        uint8_t* res = rd_ptr((uintptr_t)em + HOOKV_ENEMY_PATH);
        if (res && mem_readable(res + 0x0C, sizeof(path) - 1)) memcpy(path, res + 0x0C, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
        const char* bs = strchr(path, '\\');
        if (bs && strncmp(bs + 1, "ems", 3) == 0) continue;
        large++;
        const float* p = (const float*)(em + 0x160);
        const float* q = (const float*)(em + 0x170);
        const float* s = (const float*)(em + 0x180);
        const uint32_t* u = (const uint32_t*)(em + 0x08);
        int set = *(const int*)(em + 0x61C8 + 0xAC), id = *(const int*)(em + 0x61C8 + 0xB0);
        float hp = -1, maxHp = -1;
        uint8_t* hm = rd_ptr((uintptr_t)em + HOOKV_ENEMY_HEALTH_MGR);
        if (hm && mem_readable(hm + 0x60, 8)) { maxHp = *(float*)(hm + 0x60); hp = *(float*)(hm + 0x64); }
        uint8_t* aid = rd_ptr((uintptr_t)em + 0x12278);
        uint32_t dead = 0, diff = 0;
        if (aid) { rd((uintptr_t)aid + HOOKV_ENEMY_DEAD_FLAG, &dead); rd((uintptr_t)aid + 0x8AC, &diff); }
        const char* dn = dti_name_of(em);
        log("host diag: [%d] %p %s path %.40s id %u var %u pos %.0f %.0f %.0f quat %.3f %.3f %.3f %.3f", i, (void*)em,
            dn ? dn : "?", path, *(const uint32_t*)(em + 0x12280), *(const uint32_t*)(em + 0x12288), p[0], p[1], p[2],
            q[0], q[1], q[2], q[3]);
        log("host diag: [%d] +0x180..0x1A0 %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f sizeMod(+0x7730) %.4f transp(+0x314) %.3f",
            i, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], *(const float*)(em + 0x7730), *(const float*)(em + 0x314));
        log("host diag: [%d] unit +0x08 %08X %08X %08X %08X action %d:%d hp %.0f/%.0f deadflags %08X diffIdx %u "
            "despawn(+0x1C3D4) %.1f speed(+0x1D8A8) %.2f enrage(+0x1BE30+0x24) %.1f parts(+0x1D058) %p",
            i, u[0], u[1], u[2], u[3], set, id, hp, maxHp, dead, diff, *(const float*)(em + 0x1C3D4),
            *(const float*)(em + 0x1D8A8), *(const float*)(em + 0x1BE30 + 0x24), (void*)rd_ptr((uintptr_t)em + 0x1D058));
    }
    log("host diag: sEnemy %p (%s), %d large monsters", (void*)se, sen ? sen : "?", large);
}

// Game thread, from mod_game_tick.

void devconsole_tick(double now, bool inGame, bool inBase, int zone, const Vec3& pos) {
    static double inGameSince = 0;
    host_diag_tick(now, inGame);
    menu_watch_tick(now, inGame, inBase, pos);
    if (g_reported && inGame) track_session(now, inBase, zone, pos);
    if (g_watchUntil > 0) {
        if (now >= g_watchUntil) {
            g_watchUntil = 0;
            watch_report();
            append_out();
            log("diagnostic item bar watch written");
            std::vector<Watch>().swap(g_watch);
        } else if (now >= g_watchNext) {
            g_watchNext = now + 0.5;
            watch_sample(now);
        }
        return;
    }
    if (g_reported) return;
    if (!inGame) { inGameSince = 0; return; }
    if (inGameSince == 0) inGameSince = now;
    char flag[MAX_PATH];
    snprintf(flag, sizeof(flag), "%s\\dev_diag", mod_dir());
    if (GetFileAttributesA(flag) == INVALID_FILE_ATTRIBUTES) return;
    game_api_start();
    if (!game_api_ready()) return;
    // In a save: report once the save data is found, or after a minute in game either way.
    if (!user_data() && now - inGameSince < 60.0) return;
    g_reported = true;
    g_out.clear();
    report();
    append_out();
    log("diagnostic report written (%zu bytes)", g_out.size());
    uint8_t* u = user_data();
    if (u && !g_reportIds.empty()) watch_start(player_ptr(), u, g_reportIds, now);
}

}  // namespace rh
