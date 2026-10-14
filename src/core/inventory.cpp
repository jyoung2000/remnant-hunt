// Remnant x MHW mashup core: MHW's own zenny, item box and item names. GPL-3.0-or-later (see LICENSE).
//
// Crafting Remnant gear costs MHW zenny and materials. Both live in the current save slot (cUserData): zenny at +0x94,
// items in containers of 16-byte entries {vtable, item id, count} (the item pouch at +0x38080 is one of them; the item
// box is the rest). The containers are found by that shape, the item names come from MHW's own item text (GMD).
// Reads happen on any thread; changes happen only on the game thread, inside the tick.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "state.h"

namespace rh {

static constexpr size_t kSlotSize = 0x26CC00, kPouch = 0x38080;

struct Container { size_t off; int entries; bool pouch; };
static std::mutex g_invLock;
static uint8_t* g_slot = nullptr;
static std::vector<Container> g_containers;
static std::map<int, std::string> g_names;
static void* g_gmd = nullptr;
static double g_nextScan = 0;

bool mhw_inventory_ready() {
    std::lock_guard<std::mutex> l(g_invLock);
    return g_slot && !g_containers.empty();
}

static uint8_t* slot() {
    uint8_t* u = user_data();
    return u && mem_readable(u, kSlotSize) ? u : nullptr;
}

int mhw_zenny() {
    uint8_t* u = slot();
    if (!u) return 0;
    int z = *(int*)(u + 0x94);
    return z >= 0 && z <= 9999999 ? z : 0;
}

bool mhw_spend_zenny(int amount) {
    uint8_t* u = slot();
    if (!u || amount < 0) return false;
    int* z = (int*)(u + 0x94);
    if (*z < amount || *z > 9999999) return false;
    *z -= amount;
    return true;
}

static void scan_containers(uint8_t* u) {
    std::vector<Container> found;
    void* vt = *(void**)(u + kPouch);
    if (!vt) return;
    size_t runStart = 0;
    int run = 0;
    for (size_t off = 0; off + 16 <= kSlotSize; off += 8) {
        if (*(void**)(u + off) == vt) {
            if (run && off == runStart + (size_t)run * 16) {
                run++;
                off += 8;
                continue;
            }
            if (run >= 4) found.push_back({runStart, run, runStart <= kPouch && kPouch < runStart + run * 16});
            runStart = off;
            run = 1;
            off += 8;
        } else if (run && off >= runStart + (size_t)run * 16) {
            if (run >= 4) found.push_back({runStart, run, runStart <= kPouch && kPouch < runStart + run * 16});
            run = 0;
        }
    }
    if (run >= 4) found.push_back({runStart, run, runStart <= kPouch && kPouch < runStart + run * 16});
    std::lock_guard<std::mutex> l(g_invLock);
    g_slot = u;
    g_containers = found;
    int total = 0;
    for (const Container& c : found) total += c.entries;
    log("inventory: %zu item containers, %d entries in the save slot", found.size(), total);
}

int mhw_item_count(int id) {
    if (id <= 0) return 0;
    std::lock_guard<std::mutex> l(g_invLock);
    if (!g_slot) return 0;
    long long n = 0;
    for (const Container& c : g_containers)
        for (int i = 0; i < c.entries; i++) {
            uint8_t* e = g_slot + c.off + (size_t)i * 16;
            if (*(int*)(e + 8) == id) n += std::max(0, *(int*)(e + 12));
        }
    return (int)std::min<long long>(n, 1 << 30);
}

// Game thread. Takes from the item box first, then the pouch.
bool mhw_take_items(int id, int count) {
    if (id <= 0 || count <= 0) return count == 0;
    if (mhw_item_count(id) < count) return false;
    std::lock_guard<std::mutex> l(g_invLock);
    for (int pass = 0; pass < 2 && count > 0; pass++)
        for (const Container& c : g_containers) {
            if (c.pouch != (pass == 1)) continue;
            for (int i = 0; i < c.entries && count > 0; i++) {
                uint8_t* e = g_slot + c.off + (size_t)i * 16;
                int* eid = (int*)(e + 8);
                int* cnt = (int*)(e + 12);
                if (*eid != id || *cnt <= 0) continue;
                int take = std::min(*cnt, count);
                *cnt -= take;
                count -= take;
                if (*cnt == 0 && c.pouch) *eid = 0;  // an empty pouch slot has no item; box slots keep their item
            }
        }
    return count == 0;
}

// The pouch only (what MHW's item bar shows). Any thread.
int mhw_pouch_count(int id) {
    if (id <= 0) return 0;
    std::lock_guard<std::mutex> l(g_invLock);
    if (!g_slot) return 0;
    long long n = 0;
    for (const Container& c : g_containers) {
        if (!c.pouch) continue;
        for (int i = 0; i < c.entries; i++) {
            uint8_t* e = g_slot + c.off + (size_t)i * 16;
            if (*(int*)(e + 8) == id) n += std::max(0, *(int*)(e + 12));
        }
    }
    return (int)std::min<long long>(n, 1 << 30);
}

// Game thread. Sets the pouch's entry for the item to the count (a free slot when it has none); count 0 clears it.
bool mhw_pouch_set(int id, int count) {
    if (id <= 0 || count < 0) return false;
    std::lock_guard<std::mutex> l(g_invLock);
    if (!g_slot || !mem_readable(g_slot, kSlotSize)) return false;
    uint8_t* have = nullptr;
    uint8_t* freeSlot = nullptr;
    int slotNo = 0;
    for (const Container& c : g_containers) {
        if (!c.pouch) continue;
        for (int i = 0; i < c.entries; i++) {
            uint8_t* e = g_slot + c.off + (size_t)i * 16;
            int eid = *(int*)(e + 8);
            if (eid == id && !have) have = e;
            else if (eid == 0 && !freeSlot) { freeSlot = e; slotNo = i; }
        }
    }
    uint8_t* e = have ? have : freeSlot;
    if (!e) return false;
    if (!have && count == 0) return true;
    if (count == 0) { *(int*)(e + 8) = 0; *(int*)(e + 12) = 0; return true; }
    *(int*)(e + 8) = id;
    *(int*)(e + 12) = count;
    static std::map<int, bool> placed;
    if (!placed[id]) {
        placed[id] = true;
        const char* bar = "?";
        for (int i = 0; i < kBar_itemsCount; i++)
            if ((int)kBar_items[i].mhw_item_id == id) bar = kBar_items[i].id;
        if (have) for (const Container& c : g_containers) if (c.pouch) slotNo = (int)((have - (g_slot + c.off)) / 16);
        log("inventory: pouch slot %d now holds item %d x %d for %s", slotNo, id, count, bar);
    }
    return true;
}

// Game thread. Gives the pouch count of the item: tops up its stacks (99 at most), then free slots. Returns how many fit.
int mhw_pouch_add(int id, int count) {
    if (id <= 0 || count <= 0) return 0;
    std::lock_guard<std::mutex> l(g_invLock);
    if (!g_slot || !mem_readable(g_slot, kSlotSize)) return 0;
    constexpr int kStack = 99;
    int left = count;
    for (int pass = 0; pass < 2 && left > 0; pass++)  // pass 0: stacks of the item; pass 1: free slots
        for (const Container& c : g_containers) {
            if (!c.pouch) continue;
            for (int i = 0; i < c.entries && left > 0; i++) {
                uint8_t* e = g_slot + c.off + (size_t)i * 16;
                int* eid = (int*)(e + 8);
                int* cnt = (int*)(e + 12);
                if (pass == 0 ? (*eid != id || *cnt >= kStack) : (*eid != 0)) continue;
                int have = pass == 0 ? std::max(0, *cnt) : 0;
                int put = std::min(left, kStack - have);
                *eid = id;
                *cnt = have + put;
                left -= put;
            }
        }
    if (left < count) log("inventory: pouch gained item %d x %d", id, count - left);
    return count - left;
}

const char* mhw_item_name(int id) {
    std::lock_guard<std::mutex> l(g_invLock);
    auto it = g_names.find(id);
    return it == g_names.end() || it->second.empty() ? nullptr : it->second.c_str();
}

// Game thread: find the save slot's containers (again when the slot changes) and read item names.
void inventory_tick() {
    double t = now_s();
    if (t < g_nextScan) return;
    g_nextScan = t + 2.0;
    game_api_start();
    if (!game_api_ready()) return;
    uint8_t* u = slot();
    if (!u) return;
    bool rescan;
    {
        std::lock_guard<std::mutex> l(g_invLock);
        rescan = u != g_slot || g_containers.empty();
    }
    if (rescan) scan_containers(u);
    int stride = (int)tune(TUNING_ITEM_NAME_STRIDE);
    if (!g_gmd) g_gmd = get_resource("rGUIMessage", "common\\text\\steam\\item_eng");
    if (g_gmd && stride > 0) {
        std::lock_guard<std::mutex> l(g_invLock);
        auto want = [&](int id) {
            if (id <= 0 || g_names.count(id)) return;
            const char* m = gmd_message(g_gmd, id * stride);
            g_names[id] = m && mem_readable(m, 1) ? std::string(m, strnlen(m, 64)) : std::string();
        };
        for (int i = 0; i < kMaterialsCount; i++) want((int)kMaterials[i].mhw_item_id);
        for (int i = 0; i < kBar_itemsCount; i++) want((int)kBar_items[i].mhw_item_id);
    }
}

}  // namespace rh
