// Remnant x MHW mashup core: one view over every sheet of things the hunter can own. GPL-3.0-or-later (see LICENSE).
#pragma once
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core.h"

namespace rh {

using namespace sheets;

enum Cat : int {
    CAT_WEAPON, CAT_MOD, CAT_MUTATOR, CAT_ARMOR, CAT_ARCHETYPE, CAT_RING, CAT_AMULET, CAT_RELIC, CAT_FRAGMENT,
    CAT_CONSUMABLE, CAT_COUNT
};

struct ItemView {
    Cat cat = CAT_WEAPON;
    int row = -1;
    const char* id = "";
    const R2Text* name = nullptr;
    const R2Text* desc = nullptr;
    const char* icon = nullptr;
    const char* effects = "";
    int tier = 1;
    int menu = 0;  // MENUS_ROLE_*
    int zenny = 0;
    const char* recipe = "";
};

inline int cat_count(Cat c) {
    switch (c) {
        case CAT_WEAPON: return kWeaponsCount;
        case CAT_MOD: return kModsCount;
        case CAT_MUTATOR: return kMutatorsCount;
        case CAT_ARMOR: return kArmorCount;
        case CAT_ARCHETYPE: return kArchetypesCount;
        case CAT_RING: return kRingsCount;
        case CAT_AMULET: return kAmuletsCount;
        case CAT_RELIC: return kRelicsCount;
        case CAT_FRAGMENT: return kFragmentsCount;
        case CAT_CONSUMABLE: return kConsumablesCount;
        default: return 0;
    }
}

// The sheets' menu enums all list smithy|melder|provisions in that order.
template <class T>
inline ItemView view_of(Cat c, int row, const T& r) {
    ItemView v;
    v.cat = c;
    v.row = row;
    v.id = r.id;
    v.name = &r.name;
    v.tier = (int)r.tier;
    v.menu = r.menu;
    v.zenny = (int)r.zenny;
    v.recipe = r.recipe;
    return v;
}

inline ItemView item(Cat c, int row) {
    ItemView v;
    switch (c) {
        case CAT_WEAPON: { const Weapons& r = kWeapons[row]; v = view_of(c, row, r); v.desc = &r.desc; v.icon = r.icon; v.effects = r.effects; break; }
        case CAT_MOD: { const Mods& r = kMods[row]; v = view_of(c, row, r); v.desc = &r.desc; v.icon = r.icon; v.effects = r.effects; break; }
        case CAT_MUTATOR: { const Mutators& r = kMutators[row]; v = view_of(c, row, r); v.desc = &r.desc; v.icon = r.icon; v.effects = r.effects; break; }
        case CAT_ARMOR: { const Armor& r = kArmor[row]; v = view_of(c, row, r); v.desc = &r.desc; v.icon = r.icon; v.effects = r.effects; break; }
        case CAT_ARCHETYPE: { const Archetypes& r = kArchetypes[row]; v = view_of(c, row, r); v.desc = &r.desc; v.icon = r.emblem; break; }
        case CAT_RING: { const Rings& r = kRings[row]; v = view_of(c, row, r); v.desc = &r.desc; v.icon = r.icon; v.effects = r.effects; break; }
        case CAT_AMULET: { const Amulets& r = kAmulets[row]; v = view_of(c, row, r); v.desc = &r.desc; v.icon = r.icon; v.effects = r.effects; break; }
        case CAT_RELIC: { const Relics& r = kRelics[row]; v = view_of(c, row, r); v.desc = &r.desc; v.icon = r.icon; v.effects = r.effects; break; }
        case CAT_FRAGMENT: { const Fragments& r = kFragments[row]; v = view_of(c, row, r); v.desc = &r.desc; v.icon = r.icon; v.effects = r.effects; break; }
        case CAT_CONSUMABLE: { const Consumables& r = kConsumables[row]; v = view_of(c, row, r); v.desc = &r.desc; v.icon = r.icon; v.effects = r.effects; break; }
        default: break;
    }
    return v;
}

inline const char* cat_label(Cat c) {
    static const char* k[CAT_COUNT] = {"Weapons", "Mods", "Mutators", "Armor", "Archetypes", "Rings", "Amulets",
                                       "Relics", "Relic Fragments", "Consumables"};
    return c >= 0 && c < CAT_COUNT ? k[c] : "";
}

// "ore_low*3,bone_low*2" -> (materials row, count)
struct Need { int material; int count; };
inline std::vector<Need> recipe_of(const char* r) {
    std::vector<Need> out;
    if (!r) return out;
    std::string s(r);
    size_t a = 0;
    while (a < s.size()) {
        size_t b = s.find(',', a);
        if (b == std::string::npos) b = s.size();
        std::string part = s.substr(a, b - a);
        size_t star = part.find('*');
        if (star != std::string::npos) {
            int m = materials_index(part.substr(0, star).c_str());
            if (m >= 0) out.push_back({m, atoi(part.c_str() + star + 1)});
        }
        a = b + 1;
    }
    return out;
}

// Comma-separated row ids (list:ref columns) -> row indexes
template <class F>
inline std::vector<int> rows_of(const char* list, F index) {
    std::vector<int> out;
    if (!list) return out;
    std::string s(list);
    size_t a = 0;
    while (a < s.size()) {
        size_t b = s.find(',', a);
        if (b == std::string::npos) b = s.size();
        int i = index(s.substr(a, b - a).c_str());
        if (i >= 0) out.push_back(i);
        a = b + 1;
    }
    return out;
}

}  // namespace rh
