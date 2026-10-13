// Remnant x MHW mashup core: Remnant II's stamina effects on MHW's own stamina bar. GPL-3.0-or-later.
//
// Remnant gear changes stamina three ways: how much there is, how fast it comes back, and what actions cost. In the
// mashup they all act on MHW's stamina bar, the one the hunter dodges, sprints, guards and charges with:
//  - stamina_max: MHW's max stamina is raised (up to MHW's cap of 150; past it the rest comes back as regeneration);
//  - stamina_regen: while MHW refills the bar, the mod adds that share of what it refilled (or takes it, if negative);
//  - stamina_cost: when MHW spends stamina, the mod gives that share back (heavier Remnant armor takes more instead).
// Where MHW keeps the hunter's stamina is found by watching in the field. The value drops when the hunter dodges or
// charges, climbs back, and has its maximum stored next to it. The place is kept in learned_stamina.txt and checked
// again each session before use. The values are the hunter's data, not MHW's code. In remnant_hunt.ini,
// [stamina] enabled=0 turns this off.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "state.h"

namespace rh {

namespace {

constexpr int kSpan = 0x20000;  // how much of the hunter's object is searched
constexpr int kFloats = kSpan / 4;

int g_cfg = -1;
bool g_loaded = false;
int g_cur = -1, g_max = -1;  // byte offsets in the hunter's object; -1 = not known
bool g_checked = false;      // offsets from the file passed this session's check
double g_checkUntil = 0;

// discovery
uint8_t* g_pl = nullptr;
double g_lastSample = 0, g_evalAt = 0, g_fieldTime = 0;
bool g_reported = false;
std::vector<float> g_prev, g_dropTo, g_peak;
std::vector<double> g_dropAt;
std::vector<uint16_t> g_events, g_changes, g_atPeak;
std::vector<uint8_t> g_rising;  // since the last spend, it has only gone up
std::vector<uint8_t> g_bad;
int g_samples = 0;

// applying
float g_lastCur = -1, g_written = -1, g_base = -1;
double g_dodgeAt = -10;  // MHW spent a dodge's worth of stamina at once

std::string path() {
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%s\\learned_stamina.txt", mod_dir());
    return p;
}

void load() {
    g_loaded = true;
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%s\\remnant_hunt.ini", mod_dir());
    g_cfg = GetPrivateProfileIntA("stamina", "enabled", 1, ini);
    if (FILE* f = fopen(path().c_str(), "rb")) {
        int c = -1, m = -1;
        if (fscanf(f, "stamina %x %x", &c, &m) >= 1 && c >= 0 && c < kSpan) {
            g_cur = c;
            g_max = m >= 0 && m < kSpan ? m : -1;
            log("stamina: MHW's stamina at hunter+0x%X (max %s), to be checked", g_cur, g_max >= 0 ? "known" : "unknown");
        }
        fclose(f);
    }
}

void save() {
    if (FILE* f = fopen(path().c_str(), "wb")) {
        fprintf(f, "stamina %X %X\n", g_cur, g_max >= 0 ? g_max : -1);
        fclose(f);
    }
}

bool in_range(float v) { return std::isfinite(v) && v >= -0.01f && v <= 150.5f; }

void forget(const char* why) {
    log("stamina: hunter+0x%X no longer reads as stamina (%s): searching again", g_cur, why);
    g_cur = g_max = -1;
    g_checked = false;
    g_lastCur = g_written = g_base = -1;
    g_pl = nullptr;  // restart the search
    save();
}

// ---------------------------------------------------------------- finding it

void reset_search(uint8_t* pl) {
    g_pl = pl;
    g_prev.assign(kFloats, NAN);
    g_dropTo.assign(kFloats, 0);
    g_peak.assign(kFloats, 0);
    g_dropAt.assign(kFloats, 0);
    g_events.assign(kFloats, 0);
    g_changes.assign(kFloats, 0);
    g_atPeak.assign(kFloats, 0);
    g_rising.assign(kFloats, 0);
    g_bad.assign(kFloats, 0);
    g_samples = 0;
}

void search(uint8_t* pl, double t) {
    if (pl != g_pl) reset_search(pl);
    if (t - g_lastSample < 0.1) return;
    double dt = g_lastSample > 0 ? std::min(1.0, t - g_lastSample) : 0.1;
    g_lastSample = t;
    g_fieldTime += dt;
    g_samples++;
    for (int page = 0; page < kSpan; page += 0x1000) {
        if (!mem_readable(pl + page, 0x1000)) {
            for (int i = page / 4; i < (page + 0x1000) / 4; i++) g_bad[i] = 1;
            continue;
        }
        const float* v = (const float*)(pl + page);
        for (int k = 0; k < 0x400; k++) {
            int i = page / 4 + k;
            if (g_bad[i]) continue;
            float x = v[k], p = g_prev[i];
            if (!in_range(x)) { g_bad[i] = 1; continue; }
            g_peak[i] = std::max(g_peak[i], x);
            if (x >= g_peak[i] - 0.01f && g_atPeak[i] < 60000) g_atPeak[i]++;  // stamina rests full most of the time
            if (std::isfinite(p)) {
                if (x != p && g_changes[i] < 60000) g_changes[i]++;
                if (p - x >= 8 && p - x <= 50) {  // a spend: a dodge, a charge, a guard
                    g_dropAt[i] = t;
                    g_dropTo[i] = x;
                    g_rising[i] = 1;
                } else if (g_dropAt[i] > 0) {
                    if (x < p - 0.001f) g_rising[i] = 0;  // a refill only climbs
                    if (x >= g_dropTo[i] + 8 && t - g_dropAt[i] <= 3.0) {  // and it refills
                        if (g_rising[i] && g_events[i] < 60000) g_events[i]++;
                        g_dropAt[i] = 0;
                    } else if (t - g_dropAt[i] > 3.0) {
                        g_dropAt[i] = 0;
                    }
                }
            }
            g_prev[i] = x;
        }
    }
    if (t < g_evalAt) return;
    g_evalAt = t + 2.0;
    int best = -1, second = 0;
    for (int i = 0; i < kFloats; i++) {
        if (g_bad[i] || g_events[i] < 3 || g_peak[i] < 25 || g_atPeak[i] * 4 < g_samples) continue;
        if (best < 0 || g_events[i] > g_events[best]) {
            if (best >= 0) second = std::max<int>(second, g_events[best]);
            best = i;
        } else {
            second = std::max<int>(second, g_events[i]);
        }
    }
    if (best >= 0 && g_events[best] > second) {
        // its maximum: a steady value next to it, equal to the most it held
        int max = -1;
        for (int j = std::max(0, best - 16); j <= std::min(kFloats - 1, best + 16); j++) {
            if (j == best || g_bad[j] || !std::isfinite(g_prev[j])) continue;
            if (fabsf(g_prev[j] - g_peak[best]) < 0.6f && g_prev[j] >= 25 && g_changes[j] * 20 < g_samples) {
                max = j;
                break;
            }
        }
        if (max >= 0 || g_events[best] >= 6) {
            g_cur = best * 4;
            g_max = max >= 0 ? max * 4 : -1;
            g_checked = true;
            log("stamina: MHW's stamina found at hunter+0x%X (%d spends and refills), max %s", g_cur, g_events[best],
                g_max >= 0 ? "next to it" : "not found");
            if (g_max >= 0) log("stamina: max stamina at hunter+0x%X", g_max);
            save();
            g_prev.clear();  // free the search
            g_prev.shrink_to_fit();
            g_dropTo = g_peak = std::vector<float>();
            g_dropAt = std::vector<double>();
            g_events = g_changes = g_atPeak = std::vector<uint16_t>();
            g_bad = g_rising = std::vector<uint8_t>();
            return;
        }
    }
    if (!g_reported && g_fieldTime > 600) {
        g_reported = true;
        log("stamina: not found in the hunter's data after 10 minutes in the field (best candidate %d spends)",
            best >= 0 ? (int)g_events[best] : 0);
    }
}

}  // namespace

// Game thread, each tick.
void stamina_tick(Live& l, double t) {
    if (!g_loaded) load();
    const GameSnapshot& g = l.snap;
    uint8_t* pl = player_ptr();
    if (!g_cfg || !g.inGame || !pl) { g_lastCur = -1; return; }
    if (g_cur < 0) {
        if (!g.inBase) search(pl, t);  // stamina is only spent in the field
        return;
    }
    if (!mem_readable(pl + g_cur, 4) || (g_max >= 0 && !mem_readable(pl + g_max, 4))) return;
    float* cur = (float*)(pl + g_cur);
    float* max = g_max >= 0 ? (float*)(pl + g_max) : nullptr;
    if (!in_range(*cur) || (max && (!in_range(*max) || *max < 24.5f || *cur > *max + 0.6f))) {
        forget("out of range");
        return;
    }
    if (!g_checked) {  // from the file: a minute of sane readings in the field before anything is written
        if (g.inBase) return;
        if (g_checkUntil == 0) g_checkUntil = t + 60;
        if (t < g_checkUntil) return;
        g_checked = true;
        log("stamina: hunter+0x%X still reads as stamina", g_cur);
    }
    if (g.inBase) { g_lastCur = -1; return; }
    float bonusMax = l.fx.total(ST_STAMINA_MAX, t);
    float regen = l.fx.total(ST_STAMINA_REGEN, t), cost = l.fx.total(ST_STAMINA_COST, t);
    // Max stamina: MHW's own maximum plus the bonus, kept separate so MHW's changes (food, wear) carry on.
    if (max) {
        if (g_written < 0 || fabsf(*max - g_written) > 0.01f) g_base = *max - (g_written >= 0 ? g_written - g_base : 0.0f);
        g_base = std::max(25.0f, std::min(150.0f, g_base));
        float want = std::max(25.0f, std::min(150.0f, g_base + bonusMax));
        regen += std::max(0.0f, g_base + bonusMax - 150.0f) / 100.0f;  // past MHW's cap: regeneration instead
        if (fabsf(*max - want) > 0.01f) *max = want;
        g_written = want;
    } else if (bonusMax > 0) {
        regen += bonusMax / 100.0f;
    }
    // Spending and refilling: MHW's change since the last tick, scaled by the Remnant bonuses.
    float now = *cur, ceiling = max ? *max : 150.0f;
    if (g_lastCur >= 0) {
        float d = now - g_lastCur;
        if (d <= -12.0f) g_dodgeAt = t;  // a dodge (or a guard, a charge): Remnant's evade goes by it
        // cost -50%: half of what MHW spent comes back; regen +25%: a quarter more of what it refilled
        float k = d < -0.01f ? std::max(-1.0f, std::min(2.0f, cost)) : d > 0.01f ? std::max(-0.9f, std::min(3.0f, regen)) : 0.0f;
        if (k != 0) {
            now = std::max(0.0f, std::min(ceiling, now + d * k));
            *cur = now;
        }
    }
    g_lastCur = now;
}

bool stamina_known() { return g_cur >= 0 && g_checked; }
double stamina_dodge_at() { return g_dodgeAt; }

}  // namespace rh
