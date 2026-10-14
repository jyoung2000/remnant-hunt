// Host test of the crafting path on a fake MHW save slot: the item containers are found by their shape, materials are
// counted across the item box and the pouch, taken box-first, zenny is spent within bounds, and every recipe in the
// sheets names real materials. Build: see Makefile test-logic. Needs no game.
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "state.h"

namespace rh {
static std::vector<uint8_t> g_fakeSlot;
const char* mod_dir() { return "."; }
void log(const char*, ...) {}
double now_s() { static double t = 0; return t += 3.0; }
void game_api_start() {}
bool game_api_ready() { return true; }
uint8_t* user_data() { return g_fakeSlot.data(); }
uint8_t* player_ptr() { return nullptr; }
void* get_resource(const char*, const char*) { return nullptr; }
const char* gmd_message(void*, int) { return nullptr; }
}  // namespace rh
using namespace rh;

static int g_fail = 0;
#define CHECK(c)                                                                   \
    do {                                                                           \
        if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } \
    } while (0)

static constexpr size_t kSlotSize = 0x26CC00, kPouch = 0x38080, kBox = 0x1000;
static const uintptr_t kVt = 0x7FF6DEAD0000ull;  // the pouch's vtable pointer, shared by every item container

static void entry(size_t at, int id, int count) {
    uint8_t* e = g_fakeSlot.data() + at;
    memcpy(e, &kVt, 8);
    memcpy(e + 8, &id, 4);
    memcpy(e + 12, &count, 4);
}

int main() {
    g_fakeSlot.assign(kSlotSize, 0);
    int zenny = 125400;
    memcpy(g_fakeSlot.data() + 0x94, &zenny, 4);
    for (int i = 0; i < 24; i++) entry(kPouch + i * 16, 0, 0);   // the pouch: 24 slots
    for (int i = 0; i < 200; i++) entry(kBox + i * 16, 0, 0);    // the item box: 200 slots
    entry(kBox + 0 * 16, 133, 30);   // 30 ore in the box
    entry(kBox + 1 * 16, 134, 5);
    entry(kPouch + 0 * 16, 133, 3);  // 3 ore in the pouch

    CHECK(!mhw_inventory_ready());
    inventory_tick();
    CHECK(mhw_inventory_ready());
    CHECK(mhw_zenny() == 125400);
    CHECK(mhw_item_count(133) == 33);
    CHECK(mhw_item_count(134) == 5);
    CHECK(mhw_item_count(999) == 0);
    CHECK(mhw_item_count(0) == 0);

    // Not enough: nothing changes.
    CHECK(!mhw_take_items(133, 34));
    CHECK(mhw_item_count(133) == 33);
    // Box first, then the pouch; an emptied box slot keeps its id, an emptied pouch slot is cleared.
    CHECK(mhw_take_items(133, 32));
    CHECK(mhw_item_count(133) == 1);
    int id = 0, cnt = -1;
    memcpy(&id, g_fakeSlot.data() + kBox + 8, 4);
    memcpy(&cnt, g_fakeSlot.data() + kBox + 12, 4);
    CHECK(id == 133 && cnt == 0);
    memcpy(&cnt, g_fakeSlot.data() + kPouch + 12, 4);
    CHECK(cnt == 1);
    CHECK(mhw_take_items(133, 1));
    memcpy(&id, g_fakeSlot.data() + kPouch + 8, 4);
    CHECK(id == 0);
    CHECK(mhw_take_items(133, 0));
    CHECK(!mhw_take_items(0, 1));

    CHECK(!mhw_spend_zenny(125401));
    CHECK(mhw_zenny() == 125400);
    CHECK(mhw_spend_zenny(12000));
    CHECK(mhw_zenny() == 113400);
    CHECK(!mhw_spend_zenny(-5));

    // Every recipe in the sheets names real materials with positive counts, and a material id lookup round-trips.
    int recipes = 0, needs = 0;
    auto check_recipe = [&](const char* r) {
        if (!r || !*r) return;
        recipes++;
        int parts = 1;
        for (const char* p = r; *p; p++) parts += *p == ',';
        std::vector<Need> n = recipe_of(r);
        CHECK((int)n.size() == parts);
        for (const Need& x : n) {
            needs++;
            CHECK(x.material >= 0 && x.material < kMaterialsCount && x.count > 0);
            CHECK(materials_index(kMaterials[x.material].id) == x.material);
        }
    };
    for (int i = 0; i < kWeaponsCount; i++) check_recipe(kWeapons[i].recipe);
    for (int i = 0; i < kModsCount; i++) check_recipe(kMods[i].recipe);
    for (int i = 0; i < kMutatorsCount; i++) check_recipe(kMutators[i].recipe);
    for (int i = 0; i < kRingsCount; i++) check_recipe(kRings[i].recipe);
    for (int i = 0; i < kAmuletsCount; i++) check_recipe(kAmulets[i].recipe);
    for (int i = 0; i < kRelicsCount; i++) check_recipe(kRelics[i].recipe);
    for (int i = 0; i < kFragmentsCount; i++) check_recipe(kFragments[i].recipe);
    for (int i = 0; i < kArmorCount; i++) check_recipe(kArmor[i].recipe);
    for (int i = 0; i < kUpgradesCount; i++) check_recipe(kUpgrades[i].recipe);
    CHECK(recipes > 500 && needs > recipes);

    // Materials: every row has an MHW item id, and the ids are distinct.
    int mapped = 0;
    for (int i = 0; i < kMaterialsCount; i++) {
        int a = (int)kMaterials[i].mhw_item_id;
        if (a > 0) mapped++;
        for (int j = 0; j < i; j++) CHECK(a <= 0 || a != (int)kMaterials[j].mhw_item_id);
    }
    printf("craft: %d recipes, %d needs, %d of %d materials mapped to MHW items\n", recipes, needs, mapped, (int)kMaterialsCount);
    CHECK(mapped == kMaterialsCount);

    if (g_fail) { printf("craft: %d checks failed\n", g_fail); return 1; }
    printf("craft: all passed\n");
    return 0;
}
