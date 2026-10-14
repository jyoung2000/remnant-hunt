// Remnant x MHW mashup core: Remnant II's progression: experience, trait points, traits and prisms. GPL-3.0-or-later.
//
// Experience comes from the hunt: MHW monsters slain or captured (by their size), and Remnant bosses defeated. It
// raises the Remnant level, and every level is a trait point (as in Remnant II), spent on the traits the hunter knows
// at the Ward 13 terminal: up to 10 points each, given back at will. Core traits are known from the start; the others
// are learned from the Elder Melder or from Remnant bosses.
// The same experience flows into the equipped prism, as in Remnant II's The Dark Horizon. Each prism level offers
// three choices: a new relic fragment for one of its five slots, one more level for a fragment (up to +10), or a
// fusion of two fragments that are both +5 or more. Once all five are +10, it offers legendary bonuses. A prism can be
// cleansed from +10 to start again. The fragments' effects (fragments.json) are their +10 values, scaled by level.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "state.h"

namespace rh {

namespace {

constexpr int kSlots = 5;
constexpr int kTraitMax = 10;

float frand01() { return (float)rand() / ((float)RAND_MAX + 1.0f); }

bool in_prism(const SaveData::Prism& p, int frag) {
    for (const SaveData::PSlot& s : p.slot) {
        if (s.kind == PS_FRAGMENT && s.row == frag) return true;
        if (s.kind == PS_FUSION && (kFusions[s.row].a == frag || kFusions[s.row].b == frag)) return true;
    }
    return false;
}
int slot_of(const SaveData::Prism& p, int frag) {
    for (int i = 0; i < kSlots; i++)
        if (p.slot[i].kind == PS_FRAGMENT && p.slot[i].row == frag) return i;
    return -1;
}
bool offerable_legendary(int row) { return kLegendaries[row].effects[0] || kLegendaries[row].special == LEGENDARIES_SPECIAL_YES; }

}  // namespace

// ---------------------------------------------------------------- experience and trait points

double level_xp(int level) {
    double base = tune(TUNING_XP_LEVEL_BASE), step = tune(TUNING_XP_LEVEL_STEP);
    return level * base + step * level * (level - 1) / 2.0;
}

int remnant_level(double xp) {
    int n = 0;
    while (n < 500 && xp >= level_xp(n + 1)) n++;
    return n;
}

int trait_points(const SaveData& s) {
    return (int)tune(TUNING_TRAIT_POINTS_START) + std::min(remnant_level(s.xp), (int)tune(TUNING_TRAIT_POINTS_MAX));
}

int trait_points_spent(const SaveData& s) {
    int n = 0;
    for (int i = 0; i < kTraitsCount && i < (int)s.trait.size(); i++)
        if (kTraits[i].kind != TRAITS_KIND_ARCHETYPE) n += s.trait[i];
    return n;
}

bool trait_known(const SaveData& s, int row) {
    return row >= 0 && row < kTraitsCount && (kTraits[row].kind == TRAITS_KIND_CORE || s.has(CAT_TRAIT, row));
}

bool trait_set(SaveData& s, int row, int delta) {
    if (!trait_known(s, row) || kTraits[row].kind == TRAITS_KIND_ARCHETYPE || (delta != 1 && delta != -1)) return false;
    int v = s.trait[row] + delta;
    if (v < 0 || v > kTraitMax) return false;
    if (delta > 0 && trait_points_spent(s) >= trait_points(s)) return false;
    s.trait[row] = (uint8_t)v;
    return true;
}

void traits_reset(SaveData& s) { std::fill(s.trait.begin(), s.trait.end(), 0); }

// ---------------------------------------------------------------- prisms

int prism_level(const SaveData::Prism& p) {
    int n = 0;
    for (const SaveData::PSlot& s : p.slot) n += s.kind ? s.lv : 0;
    return n + (p.legendary >= 0 ? 1 : 0);
}

double prism_need(const SaveData::Prism& p) {
    int lv = prism_level(p);
    if (lv >= 51) return 0;
    if (lv >= 50) return tune(TUNING_PRISM_LEGEND_XP);
    return tune(TUNING_PRISM_XP_BASE) + tune(TUNING_PRISM_XP_STEP) * lv;
}

// When a level is due and nothing is waiting: three choices.
void prism_offer(SaveData::Prism& p) {
    double need = prism_need(p);
    if (p.offers > 0 || need <= 0 || p.xp < need) return;
    struct Cand { SaveData::PSlot o; float w; };
    std::vector<Cand> c;
    bool full = true;
    for (const SaveData::PSlot& s : p.slot) full &= s.kind != PS_NONE && s.lv >= kTraitMax;
    if (full && p.legendary < 0) {
        for (int i = 0; i < kLegendariesCount; i++)
            if (offerable_legendary(i)) c.push_back({{PS_LEGENDARY, (int16_t)i, 0}, 1.0f});
    } else {
        bool freeSlot = false;
        for (int i = 0; i < kSlots; i++) {
            const SaveData::PSlot& s = p.slot[i];
            if (s.kind == PS_NONE) freeSlot = true;
            else if (s.lv < kTraitMax) c.push_back({{PS_RAISE, (int16_t)i, 0}, 2.0f});
        }
        for (int f = 0; f < kFusionsCount; f++) {
            int a = slot_of(p, kFusions[f].a), b = slot_of(p, kFusions[f].b);
            if (a >= 0 && b >= 0 && p.slot[a].lv >= 5 && p.slot[b].lv >= 5) c.push_back({{PS_FUSION, (int16_t)f, 0}, 3.0f});
        }
        if (freeSlot) {
            std::vector<int> fresh;
            for (int f = 0; f < kFragmentsCount; f++)
                if (!in_prism(p, f)) fresh.push_back(f);
            // new fragments are three of the choices' worth, whichever ones
            for (int f : fresh) c.push_back({{PS_FRAGMENT, (int16_t)f, 0}, 3.0f * 3.0f / std::max<size_t>(3, fresh.size())});
        }
    }
    p.offers = 0;
    for (int k = 0; k < 3 && !c.empty(); k++) {
        float total = 0;
        for (const Cand& x : c) total += x.w;
        float r = frand01() * total;
        size_t pick = c.size() - 1;
        for (size_t i = 0; i < c.size(); i++) {
            if (r < c[i].w) { pick = i; break; }
            r -= c[i].w;
        }
        p.offer[p.offers++] = c[pick].o;
        c.erase(c.begin() + pick);
    }
}

bool prism_pick(SaveData& s, int prism, int offer) {
    if (prism < 0 || prism >= kPrismsCount || !s.has(CAT_PRISM, prism)) return false;
    SaveData::Prism& p = s.prism[prism];
    if (offer < 0 || offer >= p.offers) return false;
    double need = prism_need(p);
    SaveData::PSlot o = p.offer[offer];
    switch (o.kind) {
        case PS_FRAGMENT:
            for (SaveData::PSlot& sl : p.slot)
                if (sl.kind == PS_NONE) { sl = {PS_FRAGMENT, o.row, 1}; break; }
            break;
        case PS_RAISE: p.slot[o.row].lv = (int8_t)std::min(kTraitMax, p.slot[o.row].lv + 1); break;
        case PS_FUSION: {
            int a = slot_of(p, kFusions[o.row].a), b = slot_of(p, kFusions[o.row].b);
            if (a < 0 || b < 0) return false;
            p.slot[a] = {PS_FUSION, o.row, 1};
            p.slot[b] = SaveData::PSlot();
            break;
        }
        case PS_LEGENDARY: p.legendary = o.row; break;
        default: return false;
    }
    p.xp = std::max(0.0, p.xp - need);
    p.offers = 0;
    prism_offer(p);
    return true;
}

bool prism_cleanse(SaveData& s, int prism) {
    if (prism < 0 || prism >= kPrismsCount || prism_level(s.prism[prism]) < 10) return false;
    s.prism[prism] = SaveData::Prism();
    return true;
}

// ---------------------------------------------------------------- the hunt's experience

void award_xp(SaveData& s, Live& l, float xp, double t) {
    if (!(xp > 0)) return;
    xp *= 1.0f + l.fx.total(ST_XP, t);  // Scholar
    int before = remnant_level(s.xp), pointsBefore = trait_points(s);
    s.xp += xp;
    int after = remnant_level(s.xp), gained = trait_points(s) - pointsBefore;
    if (after > before)
        toast(l, "Remnant level %d%s", after, gained > 0 ? (gained > 1 ? ": trait points to spend at Ward 13" : ": a trait point to spend at Ward 13") : "");
    if (s.prismOn >= 0 && s.prismOn < kPrismsCount && s.has(CAT_PRISM, s.prismOn)) {
        SaveData::Prism& p = s.prism[s.prismOn];
        double need = prism_need(p);
        if (need > 0) {
            bool waiting = p.offers > 0;
            p.xp = std::min(p.xp + xp, need * 5);  // up to five levels banked
            prism_offer(p);
            if (!waiting && p.offers > 0) toast(l, "Your prism can level up: choose at Ward 13");
        }
    }
    l.dirty = true;
}

// ---------------------------------------------------------------- effects

void progress_sources(const SaveData& s, EffectState& fx) {
    const SaveData::Prism* p = s.prismOn >= 0 && s.prismOn < kPrismsCount && s.has(CAT_PRISM, s.prismOn) ? &s.prism[s.prismOn] : nullptr;
    bool traitor = p && p->legendary >= 0 && strcmp(kLegendaries[p->legendary].id, "traitor") == 0;
    for (int i = 0; i < kTraitsCount && i < (int)s.trait.size(); i++) {
        if (kTraits[i].kind == TRAITS_KIND_ARCHETYPE || !trait_known(s, i)) continue;
        int lv = traitor && kTraits[i].kind == TRAITS_KIND_CORE ? kTraitMax : s.trait[i];
        if (lv > 0) fx.add_source(kTraits[i].effects, lv / (float)kTraitMax);
    }
    if (!p) return;
    for (const SaveData::PSlot& sl : p->slot) {
        if (sl.kind == PS_FRAGMENT) fx.add_source(kFragments[sl.row].effects, sl.lv / (float)kTraitMax);
        if (sl.kind == PS_FUSION) {
            fx.add_source(kFragments[kFusions[sl.row].a].effects, sl.lv / (float)kTraitMax);
            fx.add_source(kFragments[kFusions[sl.row].b].effects, sl.lv / (float)kTraitMax);
        }
    }
    if (p->legendary >= 0) fx.add_source(kLegendaries[p->legendary].effects);
}

bool prime_time(const SaveData& s) {
    if (s.prismOn < 0 || s.prismOn >= kPrismsCount || !s.has(CAT_PRISM, s.prismOn)) return false;
    int l = s.prism[s.prismOn].legendary;
    return l >= 0 && strcmp(kLegendaries[l].id, "primetime") == 0;
}

}  // namespace rh
