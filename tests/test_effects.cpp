// Host test: every effect string in the sheets parses completely, and reads back as our own wording.
// build: g++ -std=c++17 -Isrc/core tests/test_effects.cpp src/core/effects.cpp -o build/test_effects
#include <cstdio>
#include <cstring>
#include <string>

#include "effects.h"
#include "gen/sheets.gen.h"

using namespace sheets;
static int g_bad = 0, g_ok = 0;

static void check(const char* where, const char* id, const char* dsl) {
    if (!dsl || !*dsl) return;
    int parts = 1;
    for (const char* p = dsl; *p; p++) parts += *p == ';';
    const auto& fx = rh::effects_of(dsl);
    if ((int)fx.size() != parts) {
        printf("BAD %s.%s: '%s' parsed %zu of %d\n", where, id, dsl, fx.size(), parts);
        g_bad++;
    } else {
        g_ok++;
    }
}

#define ALL(arr, n, name) for (int i = 0; i < n; i++) check(name, arr[i].id, arr[i].effects)

int main() {
    ALL(kWeapons, kWeaponsCount, "weapons");
    ALL(kMods, kModsCount, "mods");
    ALL(kMutators, kMutatorsCount, "mutators");
    ALL(kArmor, kArmorCount, "armor");
    ALL(kRings, kRingsCount, "rings");
    ALL(kAmulets, kAmuletsCount, "amulets");
    ALL(kRelics, kRelicsCount, "relics");
    ALL(kFragments, kFragmentsCount, "fragments");
    ALL(kConsumables, kConsumablesCount, "consumables");
    ALL(kSkills, kSkillsCount, "skills");
    ALL(kPerks, kPerksCount, "perks");
    ALL(kTraits, kTraitsCount, "traits");
    printf("%d effect strings parse, %d do not\n", g_ok, g_bad);
    for (const char* s : {kRings[0].effects, kAmulets[3].effects, kSkills[skills_index("hunters_mark")].effects,
                          kRelics[relics_index("dragon_heart")].effects, kMods[mods_index("hot_shot")].effects})
        printf("  %-40s -> %s\n", s, rh::describe_all(s).c_str());
    rh::EffectState st;
    st.add_source("crit+10%;on_kill:fire_rate+15% 10s x3;low_hp(35):dr+20%");
    st.fire(rh::TR_ON_KILL, 0, nullptr);
    st.fire(rh::TR_ON_KILL, 1, nullptr);
    st.hpFrac = 0.3f;
    printf("crit %.2f fire_rate %.2f dr %.2f\n", st.total(rh::ST_CRIT, 2), st.total(rh::ST_FIRE_RATE, 2), st.total(rh::ST_DR, 2));
    return g_bad ? 1 : 0;
}
