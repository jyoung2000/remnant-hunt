// Remnant x MHW mashup core: real Remnant items in MHW's item bar. GPL-3.0-or-later (see LICENSE).
//
// MHW loads loose files from <game>\nativePC before its chunk archives. So the mod copies MHW's own item table and
// item text out of memory (read-only), appends one Antidote-like row per bar item, writes both files, and the next
// start MHW knows the new items by itself. Nothing here is committed or shipped: the files are made on the player's PC.
//
// itemData.itm: 10-byte header {01 10 09 18, u16 @4 (0x00be), u16 count @6, u16 0}, then count rows of 32 bytes.
//   Row: u16 id @0 (= row index), u8 type @5, u8 rarity @9, u8 carry limits @10/@11, u16 sort order @12,
//   u32 icon id @18, u16 icon colour @22, prices @24/@28. Action rows clone Antidote (id 5);
//   boss loot rows clone Iron Ore (id 205) so they are inert MHW materials, not drinkable items.
// item_eng.gmd: 20-byte head, u32 label count, u32 text count, u32 label bytes, u32 text bytes, u32 name length,
//   name + NUL, then per message a 32-byte entry {u32 index, u32 hash1, u32 hash2, u32 0xCDCDCDCD, u64 label offset,
//   u64 next entry in the hash chain (0 = end)}, 256 u64 chain heads (-1 = empty), the NUL-separated labels, the
//   NUL-separated texts. hash1 = ~crc32(label x2), hash2 = ~crc32(label x3), chain = ~crc32(label) & 255, ascending
//   index; index 0 is never listed as a head (the original files do it that way and the writer copies that).
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

#include "catalog.h"

namespace rh {
using namespace sheets;

static constexpr int kClone = 5;  // Antidote: a harmless drinkable
static std::vector<uint8_t> g_empty;

static uint32_t crc32(const std::string& s, int times) {
    uint32_t c = 0xFFFFFFFFu;
    for (int t = 0; t < times; t++)
        for (unsigned char ch : s) {
            c ^= ch;
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        }
    return c;  // the register itself = bit-inverse of zlib's crc32 (that is what the files store)
}

template <class T> static void put(std::vector<uint8_t>& o, T v) { o.insert(o.end(), (uint8_t*)&v, (uint8_t*)&v + sizeof(T)); }
static void put_str(std::vector<uint8_t>& o, const std::string& s) { o.insert(o.end(), s.begin(), s.end()); o.push_back(0); }
template <class T> static T get(const uint8_t* d, size_t at) { T v; memcpy(&v, d + at, sizeof(T)); return v; }

// ---------------------------------------------------------------- item_eng.gmd
bool np_gmd_parse(const uint8_t* d, size_t n, NpGmd* g) {
    if (n < 45 || memcmp(d, "GMD\0", 4) != 0) return false;
    uint32_t cnt = get<uint32_t>(d, 20), cnt2 = get<uint32_t>(d, 24), lsz = get<uint32_t>(d, 28), ssz = get<uint32_t>(d, 32),
             nl = get<uint32_t>(d, 36);
    if (cnt != cnt2 || nl > 64 || cnt > 200000) return false;
    size_t ent = 41 + nl, tab = ent + (size_t)cnt * 32, lab = tab + 256 * 8, sec = lab + lsz;
    if (sec + ssz != n) return false;
    memcpy(g->head, d, 20);
    g->name.assign((const char*)d + 40, nl);
    g->labels.assign(cnt, std::string());
    g->msgs.assign(cnt, std::string());
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t idx = get<uint32_t>(d, ent + (size_t)i * 32);
        uint64_t off = get<uint64_t>(d, ent + (size_t)i * 32 + 16);
        if (idx >= cnt || lab + off >= sec) return false;
        g->labels[idx] = (const char*)d + lab + off;
    }
    size_t at = sec;  // texts are in index order
    for (uint32_t i = 0; i < cnt; i++) {
        size_t len = strnlen((const char*)d + at, n - at);
        if (at + len >= n) return false;
        g->msgs[i].assign((const char*)d + at, len);
        at += len + 1;
    }
    return at == n;
}

void np_gmd_add(NpGmd* g, const std::string& label, const std::string& text) {
    g->labels.push_back(label);
    g->msgs.push_back(text);
}

std::vector<uint8_t> np_gmd_write(const NpGmd& g) {
    size_t n = g.msgs.size();
    std::vector<uint8_t> lab, sec, o;
    std::vector<uint64_t> off(n), next(n, 0), head(256, ~0ull);
    std::vector<int> last(256, -1);
    for (size_t i = 0; i < n; i++) {
        off[i] = lab.size();
        put_str(lab, g.labels[i]);
        put_str(sec, g.msgs[i]);
        int b = (int)(crc32(g.labels[i], 1) & 255);
        if (last[b] >= 0) next[last[b]] = i;
        else if (i != 0) head[b] = i;  // index 0 starts a chain but is not stored as its head
        last[b] = (int)i;
    }
    o.insert(o.end(), g.head, g.head + 20);
    put<uint32_t>(o, (uint32_t)n); put<uint32_t>(o, (uint32_t)n);
    put<uint32_t>(o, (uint32_t)lab.size()); put<uint32_t>(o, (uint32_t)sec.size());
    put<uint32_t>(o, (uint32_t)g.name.size());
    put_str(o, g.name);
    for (size_t i = 0; i < n; i++) {
        put<uint32_t>(o, (uint32_t)i);
        put<uint32_t>(o, crc32(g.labels[i], 2));
        put<uint32_t>(o, crc32(g.labels[i], 3));
        put<uint32_t>(o, 0xCDCDCDCDu);
        put<uint64_t>(o, off[i]);
        put<uint64_t>(o, next[i]);
    }
    for (uint64_t h : head) put<uint64_t>(o, h);
    o.insert(o.end(), lab.begin(), lab.end());
    o.insert(o.end(), sec.begin(), sec.end());
    return o;
}

// ---------------------------------------------------------------- itemData.itm
bool np_itm_add(std::vector<uint8_t>* itm, int cloneId, int n) {
    if (itm->size() < 10 || get<uint32_t>(itm->data(), 0) != 0x18091001u) return false;
    uint16_t count = get<uint16_t>(itm->data(), 6);
    if (itm->size() != 10 + (size_t)count * 32 || cloneId >= count || count + n > 0xFFFF) return false;
    std::vector<uint8_t> row(itm->begin() + 10 + cloneId * 32, itm->begin() + 10 + cloneId * 32 + 32);
    for (int i = 0; i < n; i++) {
        uint16_t id = (uint16_t)(count + i);
        memcpy(row.data(), &id, 2);
        itm->insert(itm->end(), row.begin(), row.end());
    }
    uint16_t total = (uint16_t)(count + n);
    memcpy(itm->data() + 6, &total, 2);
    return true;
}

// ---------------------------------------------------------------- finding the originals in memory (reads only)
// The loaded resources keep the file bytes: item text strings point into the file image, the item table holds its rows.
static bool readable(const uint8_t* p, size_t n) { return mem_readable(p, n); }

static bool gmd_image(const uint8_t* at, int count, std::vector<uint8_t>* out) {
    if (!readable(at, 45) || memcmp(at, "GMD\0", 4) != 0 || get<uint32_t>(at, 20) != (uint32_t)count) return false;
    size_t total = 41 + get<uint32_t>(at, 36) + (size_t)count * 32 + 2048 + get<uint32_t>(at, 28) + get<uint32_t>(at, 32);
    if (total > (64u << 20) || !readable(at, total)) return false;
    out->assign(at, at + total);
    return true;
}

static bool find_gmd(void* gmd, int count, std::vector<uint8_t>* out) {
    const uint8_t* g = (const uint8_t*)gmd;
    const char* first = gmd_message(gmd, 0);
    if (!first || !g || !readable(g, 0x100)) return false;
    for (size_t back = 0; back < (8u << 20); back += 4) {  // the file starts before its first text, within a few MB
        const uint8_t* at = (const uint8_t*)first - back;
        if ((back == 0 || ((uintptr_t)at & 0xFFF) >= 0xFFC) && !readable(at, 4)) break;  // checked once per page
        if (memcmp(at, "GMD\0", 4) == 0 && gmd_image(at, count, out)) return true;
    }
    return false;
}

static bool itm_at(const uint8_t* p, int expect, std::vector<uint8_t>* out) {
    if (!readable(p, 10) || get<uint32_t>(p, 0) != 0x18091001u) return false;
    int count = get<uint16_t>(p, 6);
    if (count < 1000 || (expect > 0 && count != expect) || !readable(p, 10 + (size_t)count * 32)) return false;
    for (int k : {0, 1, 2, 3, 5, count - 1})
        if (get<uint16_t>(p, 10 + (size_t)k * 32) != k) return false;
    out->assign(p, p + 10 + (size_t)count * 32);
    return true;
}

static bool find_itm(void* res, int expect, std::vector<uint8_t>* out) {
    const uint8_t* r = (const uint8_t*)res;
    // The loaded resource keeps no file header: a count (2775) sits a few bytes before a pointer to the 32-byte rows.
    for (size_t o = 0x40; o + 4 <= 0x200 && expect > 0 && readable(r + o, 4); o += 4) {
        if (get<uint32_t>(r, o) != (uint32_t)expect) continue;
        for (size_t q = o + 4; q + 8 <= o + 0x40 && readable(r + q, 8); q += 8) {
            const uint8_t* rows = *(const uint8_t* const*)(r + q);
            if (!rows || (uintptr_t)rows < 0x10000 || !readable(rows, (size_t)expect * 32)) continue;
            bool ok = true;
            for (int k : {0, 1, 2, 3, 5, expect - 1}) ok = ok && get<uint16_t>(rows, (size_t)k * 32) == k;
            if (!ok) continue;
            out->resize(10 + (size_t)expect * 32);
            uint8_t head[10] = {0x01, 0x10, 0x09, 0x18, 0xBE, 0x00, (uint8_t)(expect & 0xFF), (uint8_t)(expect >> 8), 0, 0};
            memcpy(out->data(), head, 10);
            memcpy(out->data() + 10, rows, (size_t)expect * 32);
            log("nativepc: item rows at %p (count %d at resource +0x%zx, pointer at +0x%zx)", rows, expect, o, q);
            return true;
        }
    }
    for (size_t o = 0; o + 10 <= 0x800 && readable(r + o, 10); o++)  // the file image inside the resource object
        if (itm_at(r + o, expect, out)) return true;
    for (size_t o = 0; o + 8 <= 0x400 && readable(r + o, 8); o += 8) {  // or in a buffer it points to
        const uint8_t* p = *(const uint8_t* const*)(r + o);
        if (!p || (uintptr_t)p < 0x10000 || !readable(p, 0x200)) continue;
        for (size_t k = 0; k < 0x100; k++)
            if (itm_at(p + k, expect, out)) return true;
    }
    return false;
}

// ---------------------------------------------------------------- files and state
static std::string g_game, g_realJson;
static int g_ids[kBar_itemsCount] = {};
static int g_lootBase = 0;
static std::vector<std::pair<Cat, int>> g_loot;
static bool g_present = false, g_done = false;
static char g_toast[200] = {};
static double g_nextTry = 0;

static void make_dirs(const std::string& file) {
    for (size_t i = 3; i < file.size(); i++)
        if (file[i] == '\\' || file[i] == '/') CreateDirectoryA(file.substr(0, i).c_str(), nullptr);
}

static bool exists(const std::string& p);

static bool write_file(const std::string& path, const void* d, size_t n) {
    make_dirs(path);
    if (exists(path)) return false;
    std::string tmp = path + ".remnant-hunt.tmp";
    HANDLE f = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const uint8_t* bytes = (const uint8_t*)d;
    size_t at = 0;
    bool ok = true;
    while (at < n) {
        DWORD part = (DWORD)std::min<size_t>(n - at, 1u << 20), wrote = 0;
        if (!WriteFile(f, bytes + at, part, &wrote, nullptr) || wrote != part) { ok = false; break; }
        at += wrote;
    }
    ok = FlushFileBuffers(f) != 0 && ok;
    CloseHandle(f);
    if (!ok) { DeleteFileA(tmp.c_str()); return false; }
    // No MOVEFILE_REPLACE_EXISTING: an install or another mod that appears concurrently is preserved.
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp.c_str());
        return false;
    }
    return true;
}

static bool exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static bool read_text(const std::string& p, std::string* out) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long n = ftell(f);
    if (n <= 0 || n > 65536 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    out->resize((size_t)n);
    bool ok = fread(&(*out)[0], 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

static int json_num(const std::string& j, const std::string& key) {
    size_t at = j.find("\"" + key + "\"");
    if (at == std::string::npos) return -1;
    size_t colon = j.find(':', at + key.size() + 2);
    return colon == std::string::npos ? -1 : atoi(j.c_str() + colon + 1);
}

static void upper(std::string& s) { for (char& c : s) c = (char)toupper((unsigned char)c); }

// Boss loot references are authored in bosses.json; catalog rows supply their Remnant names and icons.
static void collect_loot() {
    if (!g_loot.empty()) return;
    static const struct { const char* sheet; Cat cat; } types[] = {
        {"weapons", CAT_WEAPON}, {"mods", CAT_MOD}, {"mutators", CAT_MUTATOR}, {"armor", CAT_ARMOR},
        {"archetypes", CAT_ARCHETYPE}, {"rings", CAT_RING}, {"amulets", CAT_AMULET}, {"relics", CAT_RELIC},
        {"fragments", CAT_FRAGMENT}, {"consumables", CAT_CONSUMABLE}, {"traits", CAT_TRAIT}, {"prisms", CAT_PRISM}};
    for (const Bosses& boss : kBosses) {
        std::string all = boss.loot;
        size_t at = 0;
        while (at < all.size()) {
            size_t end = all.find(',', at);
            if (end == std::string::npos) end = all.size();
            std::string ref = all.substr(at, end - at);
            size_t dot = ref.find('.');
            if (dot != std::string::npos)
                for (const auto& type : types)
                    if (ref.compare(0, dot, type.sheet) == 0)
                        for (int row = 0; row < cat_count(type.cat); row++)
                            if (ref.compare(dot + 1, std::string::npos, item(type.cat, row).id) == 0)
                                g_loot.push_back({type.cat, row});
            at = end + 1;
        }
    }
    std::sort(g_loot.begin(), g_loot.end());
    g_loot.erase(std::unique(g_loot.begin(), g_loot.end()), g_loot.end());
}

static const char* loot_sheet(Cat cat) {
    static const char* names[] = {"weapons", "mods", "mutators", "armor", "archetypes", "rings", "amulets",
                                  "relics", "fragments", "consumables", "traits", "prisms"};
    return cat >= 0 && cat < CAT_COUNT ? names[cat] : "unknown";
}

static bool generate(void* res, void* gmd, int gmdCount) {
    std::string root = g_game + "\\nativePC\\common\\";
    const std::string itmPath = root + "item\\itemData.itm";
    const std::string gmdPath = root + "text\\steam\\item_eng.gmd";
    if (exists(itmPath) || exists(gmdPath) || exists(g_realJson)) {
        log("nativepc: an item table, item text, or items.json already exists; refusing to overwrite it");
        return false;
    }
    collect_loot();
    if (!r2_ready()) return false;
    bool namesReady = true;
    for (const auto& ref : g_loot) {
        ItemView v = item(ref.first, ref.second);
        if (!v.name || r2_text(*v.name).empty()) namesReady = false;
    }
    if (!namesReady) return false;  // all requests were queued; retry when the reader has filled its cache
    std::vector<uint8_t> itm, gmdImg;
    if (!find_itm(res, gmdCount / 2, &itm)) { log("nativepc: item table not found in memory (rItemData %p); nothing written", res); return false; }
    int base = get<uint16_t>(itm.data(), 6), n = kBar_itemsCount + (int)g_loot.size();
    NpGmd g;
    bool exact = find_gmd(gmd, gmdCount, &gmdImg);
    if (!exact || !np_gmd_parse(gmdImg.data(), gmdImg.size(), &g) || np_gmd_write(g) != gmdImg) {
        // Rebuilding old labels from message text would break other MHW text lookups by hash.
        log("nativepc: could not round-trip MHW's exact item_eng.gmd image; nothing written");
        return false;
    }
    if ((int)g.msgs.size() != base * 2 || !np_itm_add(&itm, kClone, kBar_itemsCount) ||
        !np_itm_add(&itm, 205, (int)g_loot.size())) {
        log("nativepc: item table (%d) and text (%zu) disagree; nothing written", base, g.msgs.size()); return false;
    }
    std::string ids = "{\n  \"base\": " + std::to_string(base) + ",\n  \"count\": " + std::to_string(n) + ",\n  \"items\": {\n";
    for (int i = 0; i < kBar_itemsCount; i++) {
        std::string key = kBar_items[i].id, up = key;
        upper(up);
        np_gmd_add(&g, "REMNANT_" + up + "_NAME", kBar_items[i].native_name);
        np_gmd_add(&g, "REMNANT_" + up + "_EXP", kBar_items[i].native_desc);
        ids += "    \"" + key + "\": " + std::to_string(base + i) + ",\n";
    }
    for (size_t i = 0; i < g_loot.size(); i++) {
        const auto& ref = g_loot[i];
        ItemView v = item(ref.first, ref.second);
        std::string key = std::string(loot_sheet(ref.first)) + "." + v.id, up = key;
        upper(up);
        if (!v.name) { log("nativepc: %s has no name reference; waiting for sheet correction", key.c_str()); return false; }
        const std::string& name = r2_text(*v.name);
        if (name.empty()) return false;  // the Remnant reader fetches queued text on its worker thread
        const std::string& desc = v.desc ? r2_text(*v.desc) : std::string();
        np_gmd_add(&g, "REMNANT_LOOT_" + up + "_NAME", name);
        np_gmd_add(&g, "REMNANT_LOOT_" + up + "_EXP", desc.empty() ? "Remnant II gear." : desc);
        ids += "    \"" + key + "\": " + std::to_string(base + kBar_itemsCount + i) +
               (i + 1 < g_loot.size() ? ",\n" : "\n");
    }
    ids += "  }\n}\n";
    std::vector<uint8_t> gmdOut = np_gmd_write(g);
    if (!write_file(itmPath, itm.data(), itm.size())) {
        log("nativepc: could not create %s without replacing an existing file", itmPath.c_str());
        return false;
    }
    if (!write_file(gmdPath, gmdOut.data(), gmdOut.size())) {
        DeleteFileA(itmPath.c_str());  // this call created it; leave any pre-existing path untouched
        log("nativepc: could not create %s; rolled back our item table", gmdPath.c_str());
        return false;
    }
    if (!write_file(g_realJson, ids.data(), ids.size())) {
        DeleteFileA(gmdPath.c_str());
        DeleteFileA(itmPath.c_str());
        log("nativepc: could not create %s; rolled back our nativePC item files", g_realJson.c_str());
        return false;
    }
    snprintf(g_toast, sizeof(g_toast), "nativepc: wrote itemData (+%d rows, ids %d..%d) and item_eng.gmd; restart the game for MHW to load them",
             n, base, base + n - 1);
    log("%s", g_toast);
    return true;
}

void nativepc_tick() {
    if (g_done) return;
    if (!game_api_ready()) return;
    double t = (double)GetTickCount64() / 1000.0;
    if (t < g_nextTry) return;
    g_nextTry = t + 2.0;
    if (g_game.empty()) {
        g_game = mod_dir();
        g_game = g_game.substr(0, g_game.find_last_of("\\/"));
        g_realJson = std::string(mod_dir()) + "\\items.json";
    }
    void* res = get_resource("rItemData", "common\\item\\itemData");
    void* gmd = get_resource("rGUIMessage", "common\\text\\steam\\item_eng");
    const uint8_t* gm = (const uint8_t*)gmd;
    if (!res || !gm || !readable(gm + 0xD8, 4)) return;  // not loaded yet: try again
    int gmdCount = *(const int*)(gm + 0xD8);
    if (gmdCount <= 0 || (gmdCount & 1)) return;
    collect_loot();
    std::string root = g_game + "\\nativePC\\common\\", json;
    bool itmFile = exists(root + "item\\itemData.itm");
    bool gmdFile = exists(root + "text\\steam\\item_eng.gmd");
    bool files = itmFile && gmdFile;
    if (read_text(g_realJson, &json)) {
        int base = json_num(json, "base"), count = json_num(json, "count");
        bool idsOk = base > 0 && (count == kBar_itemsCount || count == kBar_itemsCount + (int)g_loot.size());
        for (int i = 0; idsOk && i < kBar_itemsCount; i++) idsOk = json_num(json, kBar_items[i].id) == base + i;
        if (idsOk && count > kBar_itemsCount)
            for (size_t i = 0; idsOk && i < g_loot.size(); i++) {
                const auto& ref = g_loot[i];
                std::string key = std::string(loot_sheet(ref.first)) + "." + item(ref.first, ref.second).id;
                idsOk = json_num(json, key) == base + kBar_itemsCount + (int)i;
            }
        std::vector<uint8_t> loaded;
        bool tableOk = idsOk && gmdCount / 2 >= base + count && find_itm(res, gmdCount / 2, &loaded) &&
                       get<uint16_t>(loaded.data(), 6) >= base + count;
        if (tableOk) {
            for (int i = 0; i < kBar_itemsCount; i++) g_ids[i] = base + i;
            g_lootBase = count > kBar_itemsCount ? base + kBar_itemsCount : 0;
            g_present = true;
            g_done = true;
            log("nativepc: Remnant items present");
        } else if (files) { g_done = true; log("nativepc: generated files exist but their rows are not loaded; restart the game"); }
        else { g_done = true; log("nativepc: items.json exists without a matching loaded item table; refusing to regenerate files"); }
    } else if (exists(g_realJson)) {
        g_done = true;
        log("nativepc: items.json exists but could not be read; refusing to overwrite nativePC files");
    } else if (itmFile || gmdFile) {
        g_done = true;
        log("nativepc: an itemData.itm or item_eng.gmd already exists without matching metadata; leaving it untouched");
    } else {
        if (generate(res, gmd, gmdCount)) g_done = true;
    }
}

int nativepc_item_id(int barRow) { return g_present && barRow >= 0 && barRow < kBar_itemsCount ? g_ids[barRow] : 0; }

int nativepc_loot_id(int cat, int row) {
    if (!g_present || !g_lootBase || cat < 0 || cat >= CAT_COUNT) return 0;
    auto it = std::lower_bound(g_loot.begin(), g_loot.end(), std::make_pair((Cat)cat, row));
    return it != g_loot.end() && *it == std::make_pair((Cat)cat, row) ? g_lootBase + (int)(it - g_loot.begin()) : 0;
}

const char* nativepc_take_toast() {
    static char out[200];
    if (!g_toast[0]) return nullptr;
    memcpy(out, g_toast, sizeof(out));
    g_toast[0] = 0;
    return out;
}

}  // namespace rh
