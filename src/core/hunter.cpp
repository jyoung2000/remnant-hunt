// Remnant x MHW mashup core: the Remnant player on the MHW hunter. GPL-3.0-or-later.
//
// Game thread (hunter_tick, from mod_game_tick right after the snapshot):
//  - finds the hunter's 86 .mod3 bones in MHW's joint block (player+0x90 -> +0x2B0, 0x40-byte world matrices; the
//    [gun] hand chain) by geometry (hunter_rig.cpp), again on a zone change or when the block moves. Read-only.
//  - draws the equipped Remnant armour (armor.json mesh/textures) skinned on those joints: [hunter] armour=1.
//  - while a Remnant animation plays (relic / skill / consumable rows with an anim), writes the retargeted pose
//    into the matched joint entries (and their copies) each tick: [hunter] pose_write=1. Nothing is written at any
//    other time; the game's own animation takes the joints back when it ends. These joint matrices are the only
//    game memory the mod writes here (approved by the user).
//  - [hunter] pose_test=1: for 2 s after the relic key, lifts the two hand entries 30 cm, to see whether writes
//    made on the game tick reach the screen.
#include "hunter.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include "model.h"
#include "render3d.h"
#include "state.h"

namespace rh {

void research_write(const std::string& text);  // research.cpp: appended to dev_research.txt

namespace {

constexpr int kMaxEntries = 2048;
constexpr float kNearCm = 400.0f;  // a joint entry's translation is within this of the player

struct Cfg { int armour = 1, poseWrite = 1, poseTest = 0; };
const Cfg& cfg() {
    static Cfg c;
    static bool read = false;
    if (!read) {
        read = true;
        char ini[MAX_PATH];
        snprintf(ini, sizeof(ini), "%s\\remnant_hunt.ini", mod_dir());
        c.armour = GetPrivateProfileIntA("hunter", "armour", 1, ini);
        c.poseWrite = GetPrivateProfileIntA("hunter", "pose_write", 1, ini);
        c.poseTest = GetPrivateProfileIntA("hunter", "pose_test", 0, ini);
        log("hunter: armour=%d pose_write=%d pose_test=%d", c.armour, c.poseWrite, c.poseTest);
    }
    return c;
}

bool writable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t a = (uintptr_t)p, end = a + n;
    while (a < end) {
        if (!VirtualQuery((void*)a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if (!(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) return false;
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}

bool rigid(const float* f) {
    for (int i = 0; i < 16; i++)
        if (!std::isfinite(f[i]) || fabsf(f[i]) > 1e7f) return false;
    auto len = [&](int r) { return sqrtf(f[r * 4] * f[r * 4] + f[r * 4 + 1] * f[r * 4 + 1] + f[r * 4 + 2] * f[r * 4 + 2]); };
    auto dt = [&](int a, int b) { return f[a * 4] * f[b * 4] + f[a * 4 + 1] * f[b * 4 + 1] + f[a * 4 + 2] * f[b * 4 + 2]; };
    for (int r = 0; r < 3; r++)  // the tolerance of mhw.cpp read_hand, which reads the same entries for the gun
        if (fabsf(len(r) - 1.0f) > 0.1f) return false;
    return fabsf(dt(0, 1)) < 0.1f && fabsf(dt(0, 2)) < 0.1f && fabsf(dt(1, 2)) < 0.1f;
}

bool near_player(const float* f, const Vec3& pos) {
    float dx = f[12] - pos.x, dy = f[13] - pos.y, dz = f[14] - pos.z;
    return dx * dx + dy * dy + dz * dz <= kNearCm * kNearCm;
}

uint8_t* block_base(uint8_t* pl, int l1, int l2) {
    if (!pl || !mem_readable(pl + l1, 8)) return nullptr;
    uint8_t* a = *(uint8_t**)(pl + l1);
    if (!a || !mem_readable(a + l2, 8)) return nullptr;
    return *(uint8_t**)(a + l2);
}

// The joint block as mhw.cpp read_hand reaches it (the gun's hand joint): player+l1 -> ptr -> +l2 -> ptr = base,
// entry * stride. The [gun] chain first, then the fallback 0xA8/0x310; the chain whose hand entry is a rigid matrix
// near the hunter wins, else the first readable one.
struct Chain {
    uint8_t* base = nullptr;
    int stride = 0x40, l1 = 0, l2 = 0, hand = 396;
    bool handRigid = false;
};
Chain resolve_chain(uint8_t* pl, const Vec3& pos) {
    const GunCfg& gc = gun_cfg();
    const int cand[2][4] = {{gc.handL1, gc.handL2, gc.handStride, gc.handEntry}, {0xA8, 0x310, 0x40, 396}};
    Chain first;
    for (const auto& c : cand) {
        if (c[0] < 0 || c[1] < 0 || c[2] < 64 || c[2] > 0x1000 || c[3] < 0) continue;
        uint8_t* b = block_base(pl, c[0], c[1]);
        if (!b || !mem_readable(b, 64)) continue;
        Chain ch;
        ch.base = b, ch.l1 = c[0], ch.l2 = c[1], ch.stride = c[2], ch.hand = c[3];
        uint8_t* m = b + (size_t)c[3] * c[2];
        float f[16];
        if (mem_readable(m, 64)) {
            memcpy(f, m, 64);
            ch.handRigid = rigid(f) && near_player(f, pos);
        }
        if (ch.handRigid) return ch;
        if (!first.base) first = ch;
    }
    return first;
}

// Reads all kMaxEntries entries (stops only where the memory becomes unreadable); each entry on its own: ok = rigid
// rows, finite, translation within kNearCm of the player. The block interleaves the joints with other data, so
// there is no "end of block" test. Returns the entries read.
int read_block(uint8_t* base, int stride, const Vec3& pos, std::vector<float>* block, std::vector<uint8_t>* ok,
               bool log_it) {
    block->assign((size_t)kMaxEntries * 16, 0);
    ok->assign(kMaxEntries, 0);
    int n = 0, nRigid = 0, first = -1, last = -1;
    for (int i = 0; i < kMaxEntries; i++) {
        uint8_t* m = base + (size_t)i * stride;
        if (!mem_readable(m, 64)) break;
        float* f = &(*block)[(size_t)i * 16];
        memcpy(f, m, 64);
        n = i + 1;
        if (rigid(f) && near_player(f, pos)) {
            (*ok)[i] = 1;
            nRigid++;
            if (first < 0) first = i;
            last = i;
        }
    }
    if (log_it) {
        log("hunter: joint block %p: %d rigid entries of %d scanned, first %d, last %d", (void*)base, nRigid, n, first, last);
        if (nRigid < 60) {
            std::string line = "hunter: joint block first entries (raw first floats):";
            for (int i = 0; i < 8 && i < n; i++) {
                const float* f = &(*block)[(size_t)i * 16];
                char b[96];
                snprintf(b, sizeof(b), " [%d] %g %g %g %g", i, f[0], f[1], f[2], f[3]);
                line += b;
            }
            log("%s", line.c_str());
        }
    }
    return n;
}

struct State {
    HunterMap map;
    std::vector<int> entries[kHunterBones];  // matched entry + its copies, per bone
    uint8_t* base = nullptr;
    int stride = 0x40, zone = -2, bad = 0, dumpedZone = -2;
    std::string chainSaid;
    double nextTry = 0, retry = 5;  // a failed match is tried again later and later (it costs a hitch)
    bool loggedFail = false;
    // rig
    HunterRig rig;
    const Model* skel = nullptr;
    std::vector<float> bindG;
    bool rigLogged = false;
    // playing
    const Anim* anim = nullptr;
    std::string animName;
    double animT0 = 0, lastWriteLog = 0;
    int ticksWritten = 0;
    double testUntil = 0;
    bool testLogged = false;
};
State g;

std::mutex g_reqLock;
std::string g_req;
std::atomic<bool> g_relicKey{false};

std::mutex g_drawLock;
std::vector<DrawItem> g_draw;

const char* skeleton_asset() { return kArmorCount > 0 ? kArmor[0].skeleton : ""; }

void assign_textures(const Model& m, const char* list, DrawItem* it) {
    static std::set<std::string> keep;  // stable storage for the texture asset strings (game thread only)
    size_t slots = m.materials.size();
    for (const ModelSection& s : m.sections) slots = std::max(slots, (size_t)std::max<int16_t>(0, s.material) + 1);
    it->diffuse.assign(slots, nullptr);
    const char* fallback = nullptr;
    std::string all = list ? list : "";
    size_t i = 0;
    while (i < all.size()) {
        size_t j = all.find(',', i);
        if (j == std::string::npos) j = all.size();
        std::string e = all.substr(i, j - i);
        size_t eq = e.find('=');
        if (eq != std::string::npos) {
            std::string slot = e.substr(0, eq);
            const char* kept = keep.insert(e.substr(eq + 1)).first->c_str();
            if (slot == "*") fallback = kept;
            for (size_t k = 0; k < m.materials.size() && slot != "*"; k++)
                if (!it->diffuse[k] && m.materials[k].find(slot) != std::string::npos) it->diffuse[k] = kept;
        }
        i = j + 1;
    }
    for (auto& t : it->diffuse)
        if (!t) t = fallback;
}

void unmatch(const char* why) {
    if (g.map.valid) log("hunter: joint map dropped (%s)", why);
    g.map = HunterMap();
    for (auto& e : g.entries) e.clear();
    g.anim = nullptr;
}

// dev_research / dev_diag in the mod folder: the whole block (index, rigid, translation relative to the player) into
// dev_research.txt, once per zone, so the layout can be read offline.
void research_dump(uint8_t* base, const Vec3& pos, const std::vector<float>& block, const std::vector<uint8_t>& ok,
                   int n) {
    if (g.dumpedZone == g.zone) return;
    char f1[MAX_PATH], f2[MAX_PATH];
    snprintf(f1, sizeof(f1), "%s\\dev_research", mod_dir());
    snprintf(f2, sizeof(f2), "%s\\dev_diag", mod_dir());
    if (GetFileAttributesA(f1) == INVALID_FILE_ATTRIBUTES && GetFileAttributesA(f2) == INVALID_FILE_ATTRIBUTES) return;
    g.dumpedZone = g.zone;
    std::string out;
    char b[160];
    snprintf(b, sizeof(b), "== hunter joint block zone %d base %p stride 0x%X player %.1f %.1f %.1f: %d entries ==\n",
             g.zone, (void*)base, g.stride, pos.x, pos.y, pos.z, n);
    out += b;
    out += "index rigid dx dy dz (translation minus the player, cm; rigid 1 = rigid rows, finite, within 400 cm)\n";
    for (int i = 0; i < n; i++) {
        const float* f = &block[(size_t)i * 16];
        snprintf(b, sizeof(b), "%d %d %.1f %.1f %.1f\n", i, ok[i] ? 1 : (rigid(f) ? 2 : 0), f[12] - pos.x, f[13] - pos.y,
                 f[14] - pos.z);
        out += b;
    }
    out += "(rigid 2 = rigid but farther than 400 cm, 0 = not rigid)\n";
    research_write(out);
    log("hunter: joint block layout written to dev_research.txt (zone %d)", g.zone);
}

void rematch(uint8_t* base, const Vec3& pos, double t) {
    std::vector<float> block;
    std::vector<uint8_t> ok;
    int n = read_block(base, g.stride, pos, &block, &ok, true);
    research_dump(base, pos, block, ok, n);
    float p[3] = {pos.x, pos.y, pos.z};
    HunterMap m;
    double t0 = now_s();
    bool valid = n > 0 && hunter_match(block.data(), ok.data(), n, p, &m);
    int ms = (int)((now_s() - t0) * 1000);
    g.map = m;
    for (auto& e : g.entries) e.clear();
    if (!valid) {
        g.nextTry = t + g.retry;
        g.retry = std::min(60.0, g.retry * 2);
        if (!g.loggedFail) {
            g.loggedFail = true;
            log("hunter: bones not matched in the joint block (%d entries read, best %d of %d, %d ms; retrying later)", n,
                m.matched, kHunterBones, ms);
        }
        return;
    }
    g.loggedFail = false;
    g.retry = 5;
    for (int h = 0; h < kHunterBones; h++)
        if (m.entry[h] >= 0) g.entries[h].push_back(m.entry[h]);
    for (auto& a : m.aliases) g.entries[a.second].push_back(a.first);
    int hl = hunter_bone_by_id(8), hr = hunter_bone_by_id(12), known = gun_cfg().handEntry;
    auto has = [&](int h, int e) { return h >= 0 && std::find(g.entries[h].begin(), g.entries[h].end(), e) != g.entries[h].end(); };
    const char* agree = has(hl, known) ? "the left hand" : has(hr, known) ? "the right hand" : "not a matched hand";
    log("hunter: %d of %d bones matched in the joint block (root entry %d, hand L entry %d, hand R entry %d; %zu copies; "
        "%d entries read, %d ms); the known hand entry %d is %s", m.matched, kHunterBones, m.root,
        hl >= 0 ? m.entry[hl] : -1, hr >= 0 ? m.entry[hr] : -1, m.aliases.size(), n, ms, known, agree);
}

bool read_entry(int e, float out[16]) {
    uint8_t* p = g.base + (size_t)e * g.stride;
    if (!mem_readable(p, 64)) return false;
    memcpy(out, p, 64);
    return rigid(out);
}

int write_bone(int h, const float m[16]) {
    int n = 0;
    for (int e : g.entries[h]) {
        float* p = (float*)(g.base + (size_t)e * g.stride);
        if (!writable(p, 64)) continue;
        float v[16];
        memcpy(v, m, 64);
        v[3] = p[3], v[7] = p[7], v[11] = p[11], v[15] = p[15];  // the game's own fourth column
        memcpy(p, v, 64);
        n++;
    }
    return n;
}

void build_rig() {
    if (g.rig.ok) return;
    bool failed = false;
    if (!g.skel) g.skel = skeleton_model(skeleton_asset(), &failed);
    if (!g.skel) {
        if (failed && !g.rigLogged) {
            g.rigLogged = true;
            log("hunter: Remnant II's player skeleton could not be read; no armour or animations on the hunter");
        }
        return;
    }
    pose_global(*g.skel, nullptr, 0, false, &g.bindG);
    std::vector<std::string> names;
    for (const ModelBone& b : g.skel->bones) names.push_back(b.name);
    bool ok = hunter_rig_build(names, g.bindG.data(), (int)names.size(), &g.rig);
    log("hunter: Remnant -> hunter rig %s (%d of %d hunter bones have a Remnant bone)", ok ? "ready" : "NOT built",
        g.rig.mapped, kHunterBones);
    if (!ok) g.skel = nullptr, g.rig = HunterRig(), g.rigLogged = true;
}

}  // namespace

void hunter_play(const char* anim) {
    if (!anim || !anim[0] || !strcmp(anim, "none")) return;
    std::lock_guard<std::mutex> l(g_reqLock);
    g_req = anim;
}

void hunter_relic_key() { g_relicKey = true; }

void hunter_tick(const SaveData& s, const Live& l, double t) {
    const Cfg& c = cfg();
    const GameSnapshot& snap = l.snap;
    std::vector<DrawItem> draw;
    auto publish = [&]() {
        std::lock_guard<std::mutex> lk(g_drawLock);
        g_draw.swap(draw);
    };
    std::string req;
    {
        std::lock_guard<std::mutex> lk(g_reqLock);
        req.swap(g_req);
    }
    bool relicKey = g_relicKey.exchange(false);
    if (!snap.inGame || !snap.player) {
        if (g.map.valid) unmatch("no hunter");
        publish();
        return;
    }
    // ---- the joint block and the map
    Chain ch = resolve_chain(snap.player, snap.pos);
    uint8_t* base = ch.base;
    if (base) {
        g.stride = ch.stride;
        char said[160];
        snprintf(said, sizeof(said), "player+0x%X -> +0x%X, stride 0x%X; entry %d is %s", ch.l1, ch.l2, ch.stride, ch.hand,
                 ch.handRigid ? "a rigid matrix near the hunter" : "NOT rigid / not near the hunter");
        if (g.chainSaid != said) {
            g.chainSaid = said;
            log("hunter: joint block chain %s (base %p)", said, (void*)base);
        }
    }
    if (!base || !mem_readable(base, 64)) {
        if (g.map.valid) unmatch("joint block unreadable");
        g.base = nullptr;
        publish();
        return;
    }
    if (base != g.base || snap.zone != g.zone) {
        if (g.map.valid) unmatch(base != g.base ? "the joint block moved" : "zone change");
        g.base = base;
        g.zone = snap.zone;
        g.nextTry = 0;
        g.retry = 5;
    }
    if (!g.map.valid && t >= g.nextTry) rematch(base, snap.pos, t);
    build_rig();

    float hw[kHunterBones][16];
    bool hv[kHunterBones] = {};
    if (g.map.valid) {
        for (int h = 0; h < kHunterBones; h++)
            if (g.map.entry[h] >= 0) hv[h] = read_entry(g.map.entry[h], hw[h]);
        // still the hunter's? the root at the hunter, the pelvis readable
        int r0 = hunter_bone_by_id(0);
        bool sane = r0 >= 0 && hv[r0] && (g.rig.pelvisH < 0 || hv[g.rig.pelvisH]);
        if (sane) {
            float dx = hw[r0][12] - snap.pos.x, dy = hw[r0][13] - snap.pos.y, dz = hw[r0][14] - snap.pos.z;
            sane = dx * dx + dy * dy + dz * dz < 60.0f * 60.0f;
        }
        g.bad = sane ? 0 : g.bad + 1;
        if (g.bad > 60) {
            unmatch("the root left the hunter");
            g.nextTry = t;
        }
    }

    // ---- a Remnant animation on the hunter
    static std::string pending;
    static double pendingAt = 0;
    if (!req.empty()) {
        if (!c.poseWrite) {
            // nothing: the learned MHW action plays as before
        } else if (!g.map.valid) {
            static bool said = false;
            if (!said) {
                said = true;
                log("hunter: pose write skipped: bones not matched");
            }
        } else {
            pending = req;
            pendingAt = t;
        }
    }
    if (!pending.empty() && g.rig.ok && g.skel) {
        bool failed = false;
        const Anim* a = anim_get(pending.c_str(), skeleton_asset(), g.skel, &failed);
        if (a) {
            g.anim = a;
            g.animName = pending;
            g.animT0 = t;
            g.ticksWritten = 0;
            log("hunter: playing %s on the hunter (%.2f s)", pending.c_str(), a->length());
            pending.clear();
        } else if (failed || t - pendingAt > 3.0) {
            log("hunter: %s not ready (%s); the MHW action plays alone", pending.c_str(), failed ? "unreadable" : "still loading");
            pending.clear();
        }
    } else if (!pending.empty() && t - pendingAt > 3.0) {
        log("hunter: %s skipped: the Remnant skeleton is not ready", pending.c_str());
        pending.clear();
    }
    if (g.anim && (!g.map.valid || !c.poseWrite)) g.anim = nullptr;
    if (g.anim) {
        float at = (float)(t - g.animT0);
        int r0 = hunter_bone_by_id(0), pel = g.rig.pelvisH;
        if (at > g.anim->length()) {
            log("hunter: %s finished after %d ticks of writes; the game has the joints again", g.animName.c_str(),
                g.ticksWritten);
            g.anim = nullptr;
        } else if (r0 >= 0 && pel >= 0 && hv[r0] && hv[pel]) {
            std::vector<float> G;
            pose_global(*g.skel, g.anim, at, false, &G);
            float out[kHunterBones][16];
            bool set[kHunterBones];
            hunter_retarget(g.rig, G.data(), g.bindG.data(), hw[r0], hw[pel], out, set);
            int n = 0;
            for (int h = 0; h < kHunterBones; h++) {
                if (!set[h] || g.map.entry[h] < 0) continue;
                n += write_bone(h, out[h]);
                memcpy(hw[h], out[h], 64);  // the armour follows the written pose
                hv[h] = true;
            }
            g.ticksWritten++;
            if (t - g.lastWriteLog >= 1.0) {
                g.lastWriteLog = t;
                log("hunter: %s at %.2f s: wrote %d joint entries", g.animName.c_str(), at, n);
            }
        }
    }

    // ---- [hunter] pose_test: the hands lifted 30 cm for 2 s after the relic key
    if (relicKey && c.poseTest) {
        g.testUntil = t + 2.0;
        g.testLogged = false;
    }
    if (t < g.testUntil) {
        if (!g.map.valid) {
            if (!g.testLogged) {
                g.testLogged = true;
                log("hunter: pose test: bones not matched, nothing written");
            }
        } else {
            int n = 0;
            for (int id : {8, 12}) {
                int h = hunter_bone_by_id(id);
                if (h < 0 || !hv[h]) continue;
                float m[16];
                memcpy(m, hw[h], 64);
                m[13] += 30.0f;
                n += write_bone(h, m);
            }
            if (!g.testLogged) {
                g.testLogged = true;
                log("hunter: pose test wrote %d entries (the hands, 30 cm up, for 2 s). If the hands do not move on screen, "
                    "the game rebuilds the joint block after the mod's tick and before drawing, and pose writes need a "
                    "later point in the frame", n);
            }
        }
    }

    // ---- the Remnant armour on the hunter
    if (c.armour && g.map.valid && g.rig.ok) {
        int pel = g.rig.pelvisH;
        for (int piece = 0; piece < 4; piece++) {
            int row = s.armor[piece];
            if (row < 0 || row >= kArmorCount || !kArmor[row].mesh || !strcmp(kArmor[row].mesh, "none")) continue;
            const Model* m = model_get(kArmor[row].mesh);
            if (!m) continue;
            DrawItem it;
            it.model = m;
            if (!hunter_mesh_palette(g.rig, *m, hw, hv, &it.palette)) continue;
            // placed at the pelvis (the pass samples the scene under an item's origin), the palette relative to it
            float o[3] = {hw[pel][12], hw[pel][13], hw[pel][14]};
            for (size_t b = 0; b * 12 < it.palette.size(); b++)
                for (int k = 0; k < 3; k++) it.palette[b * 12 + k * 4 + 3] -= o[k];
            float w[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, o[0], o[1], o[2], 1};
            memcpy(it.world, w, 64);
            assign_textures(*m, kArmor[row].textures, &it);
            draw.push_back(std::move(it));
        }
    }
    publish();
}

void hunter_append(std::vector<DrawItem>* items) {
    std::lock_guard<std::mutex> lk(g_drawLock);
    for (const DrawItem& d : g_draw) items->push_back(d);
}

}  // namespace rh
