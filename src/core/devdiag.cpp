// Remnant x MHW mashup core: MHW engine API (found by pattern) and a read-only diagnostic. GPL-3.0-or-later.
//
// The diagnostic takes no input. When a file named dev_diag exists in the mod folder, it writes a fixed report once
// per session, after the hunter reaches town: what the API lookups resolved to, the item pouch, item names and item
// data headers. It only reads game memory.
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core.h"

namespace rh {

static std::string g_out;
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
static uintptr_t g_sResource = 0, g_sUserData = 0;

static uintptr_t rip_target(uintptr_t insn, int dispOffset, int len) {
    int32_t d = *(int32_t*)(insn + dispOffset);
    return insn + len + d;
}

bool game_api_init() {
    static bool done = false, ok = false;
    if (done) return ok;
    done = true;
    auto one = [](const char* pat, int off) -> uintptr_t {
        auto h = scan_text(pat, 2);
        return h.size() == 1 ? h[0] + off : 0;
    };
    g_findDti = (FindDti_t)one("39 48 34 74 0B 48 8B 40 28", -19);
    uintptr_t gr = one("48 83 EC 30 80 B9 7B 80 20 00 00", -26);
    g_getResource = (GetResource_t)gr;
    uintptr_t sel = one("89 91 A0 00 00 00 48 8B 0D ?? ?? ?? ?? E9", 0);
    if (sel) g_sUserData = rip_target(sel + 6, 3, 7);
    // sMhResource: the static most often loaded into rcx right before a call to GetResource.
    if (gr) {
        std::map<uintptr_t, int> votes;
        const uint8_t* b = (const uint8_t*)g_textBeg;
        for (uintptr_t p = g_textBeg; p + 5 < g_textEnd; p++) {
            if (b[p - g_textBeg] != 0xE8) continue;
            if (rip_target(p, 1, 5) != gr) continue;
            for (uintptr_t q = p - 7; q > p - 40; q--)
                if (*(uint8_t*)q == 0x48 && *(uint8_t*)(q + 1) == 0x8B && *(uint8_t*)(q + 2) == 0x0D) {
                    votes[rip_target(q, 3, 7)]++;
                    break;
                }
        }
        int best = 0;
        for (auto& v : votes)
            if (v.second > best) { best = v.second; g_sResource = v.first; }
        log("api: sMhResource static %p (%d callers agree)", (void*)g_sResource, best);
    }
    ok = g_findDti && g_getResource && g_sResource && g_sUserData;
    log("api: FindDti %p GetResource %p sUserData static %p -> %s", (void*)g_findDti, (void*)g_getResource,
        (void*)g_sUserData, ok ? "ok" : "MISSING");
    return ok;
}

void* find_dti(const char* name) { return g_findDti ? g_findDti(mt_crc(name) & 0x7FFFFFFF) : nullptr; }

// Game thread only.
void* get_resource(const char* dtiName, const char* path) {
    if (!game_api_init()) return nullptr;
    void* dti = find_dti(dtiName);
    void* mgr = rd_ptr(g_sResource);
    if (!dti || !mgr) return nullptr;
    return g_getResource(mgr, dti, path, 1);
}

// Current save slot's cUserData (layout per SharpPluginLoader: +0xA0 slot, +0xA8 first slot, 0x26CC00 per slot).
uint8_t* user_data() {
    if (!game_api_init()) return nullptr;
    uint8_t* s = rd_ptr(g_sUserData);
    if (!s || !mem_readable(s + 0xA0, 0x10)) return nullptr;
    int slot = *(int*)(s + 0xA0);
    uint8_t* first = *(uint8_t**)(s + 0xA8);
    if (slot < 0 || slot > 2 || !first) return nullptr;
    return first + (size_t)slot * 0x26CC00;
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

static void report() {
    outf("== Remnant Hunt diagnostic (read-only) ==");
    outf("api %s", game_api_init() ? "ok" : "missing");
    module_range();
    outf("module base %p text %p-%p", (void*)g_base, (void*)g_textBeg, (void*)g_textEnd);
    uint8_t* u = user_data();
    outf("userdata %p", (void*)u);
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
            }
        outf("-- userdata around the pouch");
        hexdump((uintptr_t)(u + 0x38040), 0x60);
    }
    const char* filters[] = {"Item", "GUI", "Pouch", nullptr};
    int n = 0;
    void* root = find_dti("MtObject");
    outf("-- DTIs (root %p)", root);
    if (root) walk_dti((uint8_t*)root, filters, 0, &n);
    void* gmd = get_resource("rGUIMessage", "common\\text\\steam\\item_eng");
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
    outf("== end ==");
}

static bool g_reported = false;

// Game thread, from mod_game_tick.
void devconsole_tick(double now) {
    (void)now;
    if (g_reported) return;
    char flag[MAX_PATH];
    snprintf(flag, sizeof(flag), "%s\\dev_diag", mod_dir());
    if (GetFileAttributesA(flag) == INVALID_FILE_ATTRIBUTES) return;
    uint8_t* u = user_data();
    if (!u) return;  // not in a save yet
    g_reported = true;
    g_out.clear();
    report();
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\dev_diag.txt", mod_dir());
    FILE* f = fopen(path, "ab");
    if (f) {
        fwrite(g_out.data(), 1, g_out.size(), f);
        fclose(f);
    }
    log("diagnostic report written (%zu bytes)", g_out.size());
}

}  // namespace rh
