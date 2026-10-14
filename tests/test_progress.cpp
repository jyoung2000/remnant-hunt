// Host test (a Windows build, run under Wine): Remnant levels and trait points, traits, and prisms from their first
// fragment to a legendary bonus, on the real sheets. build and run: make test-logic
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "state.h"

namespace rh {
const char* mod_dir() { return "."; }
void log(const char*, ...) {}
static char g_toast[256];
void toast(Live&, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vsnprintf(g_toast, sizeof(g_toast), fmt, a);
    va_end(a);
}
}  // namespace rh

using namespace rh;
static int g_fail = 0;
#define EXPECT(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int find_offer(const SaveData::Prism& p, int kind) {
    for (int i = 0; i < p.offers; i++)
        if (p.offer[i].kind == kind) return i;
    return -1;
}

int main() {
    srand(3);
    SaveData s;
    Live l;
    int vigor = traits_index("vigor"), scholar = traits_index("scholar");
    EXPECT(vigor >= 0 && trait_known(s, vigor), "core traits are known from the start");
    EXPECT(scholar >= 0 && !trait_known(s, scholar), "other traits are learned");
    EXPECT(trait_points(s) == (int)tune(TUNING_TRAIT_POINTS_START), "starting points %d", trait_points(s));
    int start = trait_points(s);
    for (int i = 0; i < start; i++) EXPECT(trait_set(s, vigor, 1), "point %d into Vigor", i);
    EXPECT(!trait_set(s, vigor, 1), "no points left");
    EXPECT(!trait_set(s, scholar, 1), "an unknown trait takes no points");
    award_xp(s, l, (float)level_xp(3), 0);
    EXPECT(remnant_level(s.xp) == 3 && trait_points(s) == start + 3, "level %d, points %d", remnant_level(s.xp), trait_points(s));
    EXPECT(strstr(g_toast, "Remnant level 3") != nullptr, "a level-up toast: %s", g_toast);
    EXPECT(trait_set(s, vigor, -1) && trait_set(s, vigor, 1), "points come back and go again");

    // A prism, equipped, fed experience.
    int greed = prisms_index("greed");
    s.owned[CAT_PRISM][greed] = 1;
    s.prismOn = greed;
    SaveData::Prism& p = s.prism[greed];
    award_xp(s, l, 6000, 0);
    EXPECT(p.offers == 3, "three choices at the first prism level, got %d", p.offers);
    int lv = 0, picks = 0;
    while (prism_level(p) < 50 && picks < 400) {
        if (p.offers == 0) { p.xp += prism_need(p); prism_offer(p); }
        int k = find_offer(p, PS_FRAGMENT);
        if (k < 0) k = find_offer(p, PS_RAISE);
        if (k < 0) k = 0;
        int before = prism_level(p);
        bool fusion = p.offer[k].kind == PS_FUSION;
        EXPECT(prism_pick(s, greed, k), "pick %d", picks);
        if (!fusion) EXPECT(prism_level(p) == before + 1, "a pick adds one level (%d -> %d)", before, prism_level(p));
        picks++;
        lv = prism_level(p);
    }
    EXPECT(lv == 50, "five fragments at +10 after %d picks (level %d)", picks, lv);
    for (const SaveData::PSlot& sl : p.slot) EXPECT(sl.kind != PS_NONE && sl.lv == 10, "a full slot");
    p.xp = prism_need(p);
    prism_offer(p);
    EXPECT(p.offers == 3 && p.offer[0].kind == PS_LEGENDARY, "legendary choices at +50");
    int traitor = legendaries_index("traitor"), leg = -1;
    for (int i = 0; i < p.offers; i++) leg = p.offer[i].row;
    EXPECT(prism_pick(s, greed, p.offers - 1) && p.legendary == leg && prism_level(p) == 51 && prism_need(p) == 0, "+51 with a legendary");

    // Fusion: two fragments of a pair, both +5, fuse into one +1 fragment with both.
    int other = prisms_index("pride");
    s.owned[CAT_PRISM][other] = 1;
    SaveData::Prism& q = s.prism[other];
    const Fusions& f = kFusions[0];
    q.slot[0] = {PS_FRAGMENT, (int16_t)f.a, 5};
    q.slot[1] = {PS_FRAGMENT, (int16_t)f.b, 6};
    int got = -1;
    for (int tries = 0; tries < 200 && got < 0; tries++) {
        q.offers = 0;
        q.xp = prism_need(q);
        prism_offer(q);
        got = find_offer(q, PS_FUSION);
    }
    EXPECT(got >= 0, "a fusion is offered");
    if (got >= 0) {
        EXPECT(prism_pick(s, other, got), "fuse");
        EXPECT(q.slot[0].kind == PS_FUSION && q.slot[0].lv == 1 && q.slot[1].kind == PS_NONE && prism_level(q) == 1,
               "fused into one +1 slot (level %d)", prism_level(q));
    }
    EXPECT(!prism_cleanse(s, other), "no cleanse below +10");
    EXPECT(prism_cleanse(s, greed) && prism_level(s.prism[greed]) == 0, "cleansed");

    // Effects: Vigor's points, and the Traitor legendary maxing core traits.
    traits_reset(s);
    EffectState fx;
    s.trait[vigor] = 5;
    progress_sources(s, fx);
    EXPECT(fabsf(fx.total(ST_MAX_HP, 0) - 15) < 0.01f, "Vigor 5 of 10 gives 15 max health, got %.2f", fx.total(ST_MAX_HP, 0));
    s.prism[greed].legendary = traitor;
    EffectState fx2;
    progress_sources(s, fx2);
    EXPECT(fabsf(fx2.total(ST_MAX_HP, 0) - 30) < 0.01f, "Traitor maxes Vigor, got %.2f", fx2.total(ST_MAX_HP, 0));
    printf(g_fail ? "progress: %d FAILED\n" : "progress: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
