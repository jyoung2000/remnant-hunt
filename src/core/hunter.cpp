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
#include "gen/sheets.gen.h"

namespace rh {

namespace {

constexpr int kMaxEntries = 1024;

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
    for (int r = 0; r < 3; r++)
        if (fabsf(len(r) - 1.0f) > 0.05f) return false;
    return fabsf(dt(0, 1)) < 0.05f && fabsf(dt(0, 2)) < 0.05f && fabsf(dt(1, 2)) < 0.05f;
}

uint8_t* block_base(uint8_t* pl, int l1, int l2) {
    if (!pl || !mem_readable(pl + l1, 8)) return nullptr;
    uint8_t* a = *(uint8_t**)(pl + l1);
    if (!a || !mem_readable(a + l2, 8)) return nullptr;
    return *(uint8_t**)(a + l2);
}

// Reads the whole bounded block: MHW leaves unused slots before its live hunter bones, including before the known
// gun hand at entry 396. Invalid matrices are holes, not evidence that the block ended.
int read_block(uint8_t* base, int stride, std::vector<float>* block, std::vector<uint8_t>* ok) {
    block->assign((size_t)kMaxEntries * 16, 0);
    ok->assign(kMaxEntries, 0);
    for (int i = 0; i < kMaxEntries; i++) {
        uint8_t* m = base + (size_t)i * stride;
        if (!mem_readable(m, 64)) break;
        float* f = &(*block)[(size_t)i * 16];
        memcpy(f, m, 64);
        if (rigid(f)) (*ok)[i] = 1;
    }
    return hunter_block_extent(*ok);
}

struct State {
    HunterMap map;
    std::vector<int> entries[kHunterBones];  // matched entry + its copies, per bone
    uint8_t* base = nullptr;
    int stride = 0x40, zone = -2, bad = 0;
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
    bool heldAimPose = false;
    double animT0 = 0, lastWriteLog = 0;
    int ticksWritten = 0;
    float lastHand[16] = {};
    double lastHandAt = 0;
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
    g.heldAimPose = false;
    g.lastHandAt = 0;
}

static const char* aim_pose_id(const GameSnapshot& snap, const float root[16]) {
    char horizontal = 'c', vertical = 'c';
    if (snap.view.valid) {
        float dx = snap.view.camTarget.x - snap.view.camPos.x;
        float dy = snap.view.camTarget.y - snap.view.camPos.y;
        float dz = snap.view.camTarget.z - snap.view.camPos.z;
        float n = sqrtf(dx * dx + dy * dy + dz * dz);
        if (n > 1e-5f) {
            dx /= n; dy /= n; dz /= n;
            if (dy > 0.20f) vertical = 'u';
            else if (dy < -0.20f) vertical = 'd';
            float fx = root[8], fz = root[10], fl = sqrtf(fx * fx + fz * fz);
            float rx = root[0], rz = root[2], rl = sqrtf(rx * rx + rz * rz);
            if (fl > 1e-5f && rl > 1e-5f) {
                float forward = (dx * fx + dz * fz) / fl;
                float side = (dx * rx + dz * rz) / rl;
                if (side > 0.35f && forward > -0.75f) horizontal = 'r';
                else if (side < -0.35f && forward > -0.75f) horizontal = 'l';
            }
        }
    }
    static char id[10];
    snprintf(id, sizeof(id), "r2_aim_%c%c", horizontal, vertical);
    return id;
}

struct JointCandidate {
    uint8_t* base = nullptr;
    int stride = 0x40;
    const char* label = "";
};

struct JointMatch {
    JointCandidate candidate;
    std::vector<float> block;
    std::vector<uint8_t> ok;
    HunterMap map;
    int entries = 0;
    int ms = 0;
    bool valid = false;
};

void rematch(const JointCandidate* candidates, int count, const Vec3& pos, double t) {
    float p[3] = {pos.x, pos.y, pos.z};
    JointMatch best;
    bool haveBest = false;
    int primaryEntries = 0, primaryMatched = 0, alternateEntries = 0, alternateMatched = 0;
    for (int i = 0; i < count; i++) {
        if (!candidates[i].base || (i && candidates[i].base == candidates[0].base && candidates[i].stride == candidates[0].stride)) continue;
        JointMatch attempt;
        attempt.candidate = candidates[i];
        double t0 = now_s();
        attempt.entries = read_block(attempt.candidate.base, attempt.candidate.stride, &attempt.block, &attempt.ok);
        attempt.valid = attempt.entries > 0 && hunter_match(attempt.block.data(), attempt.ok.data(), attempt.entries, p, &attempt.map);
        attempt.ms = (int)((now_s() - t0) * 1000);
        if (i == 0) primaryEntries = attempt.entries, primaryMatched = attempt.map.matched;
        else alternateEntries = attempt.entries, alternateMatched = attempt.map.matched;
        if (!haveBest || attempt.map.matched > best.map.matched) best = std::move(attempt), haveBest = true;
    }
    for (auto& e : g.entries) e.clear();
    if (!haveBest || !best.valid) {
        g.map = HunterMap();
        if (count > 0) {
            const JointCandidate& retry = candidates[0].base ? candidates[0] : candidates[count - 1];
            g.base = retry.base;
            g.stride = retry.stride;
        }
        g.nextTry = t + g.retry;
        g.retry = std::min(5.0, g.retry * 2);
        if (!g.loggedFail) {
            g.loggedFail = true;
            log("hunter: bones not matched in known joint blocks (primary %d entries/%d bones, alternate %d entries/%d bones; retrying later)",
                primaryEntries, primaryMatched, alternateEntries, alternateMatched);
        }
        return;
    }
    g.base = best.candidate.base;
    g.stride = best.candidate.stride;
    g.map = best.map;
    g.loggedFail = false;
    g.retry = 5;
    for (int h = 0; h < kHunterBones; h++)
        if (best.map.entry[h] >= 0) g.entries[h].push_back(best.map.entry[h]);
    for (auto& a : best.map.aliases) g.entries[a.second].push_back(a.first);
    int hl = hunter_bone_by_id(8), hr = hunter_bone_by_id(12), known = gun_cfg().handEntry;
    auto has = [&](int h, int e) { return h >= 0 && std::find(g.entries[h].begin(), g.entries[h].end(), e) != g.entries[h].end(); };
    const char* agree = has(hl, known) ? "the left hand" : has(hr, known) ? "the right hand" : "not a matched hand";
    log("hunter: %d of %d bones matched in the %s joint block (root entry %d, hand L entry %d, hand R entry %d; %zu copies; "
        "%d entries read, %d ms); the known hand entry %d is %s", best.map.matched, kHunterBones,
        best.candidate.label, best.map.root, hl >= 0 ? best.map.entry[hl] : -1, hr >= 0 ? best.map.entry[hr] : -1,
        best.map.aliases.size(), best.entries, best.ms, known, agree);
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

bool hunter_right_hand(float out[16]) {
    if (!out || !g.map.valid || !g.base) return false;
    // The joint table can briefly clear a limb while World changes actions. Keep the last pose from
    // this hunter for that gap; never attach a gun to the unrelated readable entry 396 or a guessed point.
    if (g.lastHandAt <= 0 || now_s() - g.lastHandAt > 0.5) return false;
    memcpy(out, g.lastHand, 64);
    return true;
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
    const GunCfg& gc = gun_cfg();
    g.stride = gc.handStride >= 64 && gc.handStride <= 0x1000 ? gc.handStride : 0x40;
    JointCandidate candidates[2] = {
        {block_base(snap.player, gc.handL1, gc.handL2), g.stride, "primary"},
        {block_base(snap.player, 0xA8, 0x310), 0x40, "alternate"},
    };
    for (auto& candidate : candidates)
        if (!candidate.base || !mem_readable(candidate.base, 64)) candidate.base = nullptr;
    int candidateCount = (candidates[0].base || candidates[1].base) ? 2 : 0;
    if (!candidateCount) {
        if (g.map.valid) unmatch("joint block unreadable");
        g.base = nullptr;
        publish();
        return;
    }
    bool selectedStillPresent = false;
    for (const auto& candidate : candidates)
        selectedStillPresent |= candidate.base == g.base && candidate.stride == g.stride;
    if (!selectedStillPresent) {
        if (g.map.valid) unmatch("both known joint blocks moved");
        g.base = nullptr;
        g.zone = snap.zone;
        g.nextTry = 0;
        g.retry = 5;
    } else if (snap.zone != g.zone) {
        // A zone transition alone does not invalidate a player-owned joint block. The live
        // matrices are checked below each tick; rematching a readable block here can select
        // an unrelated rigid-body table while MHW is streaming the next area.
        g.zone = snap.zone;
    }
    if (!g.map.valid && t >= g.nextTry) rematch(candidates, candidateCount, snap.pos, t);
    build_rig();

    float hw[kHunterBones][16];
    bool hv[kHunterBones] = {};
    if (g.map.valid) {
        for (int h = 0; h < kHunterBones; h++)
            if (g.map.entry[h] >= 0) hv[h] = read_entry(g.map.entry[h], hw[h]);
        // still the hunter's? the root at the hunter, the pelvis readable
        int r0 = hunter_bone_by_id(0);
        // The block was discovered through this player's object and zone. Its root can drift
        // away from snap.pos during root-motion and MHW state transitions, so position alone
        // must not invalidate a still-readable map. Drop it only when its anchor matrices vanish.
        bool sane = r0 >= 0 && hv[r0] && (g.rig.pelvisH < 0 || hv[g.rig.pelvisH]);
        g.bad = sane ? 0 : g.bad + 1;
        if (g.bad > 60) {
            unmatch("the root or pelvis matrix stopped being readable");
            g.retry = 1.0;
            g.nextTry = t + 1.0;
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
            g.heldAimPose = false;
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
    if (g.heldAimPose && !l.gunMode) {
        g.anim = nullptr;
        g.animName.clear();
        g.heldAimPose = false;
    }
    int pelvisBone = g.rig.pelvisH;
    if (l.gunMode && c.poseWrite && g.map.valid && g.rig.ok && g.skel && pelvisBone >= 0 && hv[pelvisBone] &&
        (!g.anim || g.heldAimPose) && pending.empty()) {
        // The matched root can be a permanent identity matrix at the map origin. The live pelvis
        // turns with the visible hunter and is the correct basis for choosing an aim direction.
        const char* id = aim_pose_id(snap, hw[pelvisBone]);
        int row = sheets::player_anims_index(id);
        if (row >= 0 && (!g.heldAimPose || g.animName != id)) {
            bool failed = false;
            const Anim* a = anim_get(sheets::kPlayer_anims[row].asset, skeleton_asset(), g.skel, &failed);
            if (a) {
                g.anim = a;
                g.animName = id;
                g.animT0 = t;
                g.heldAimPose = true;
                log("hunter: Remnant %s gun-hold pose active on the hunter", id);
            } else if (failed) {
                log("hunter: Remnant %s gun-hold pose failed to load", id);
            }
        }
    }
    if (g.anim && (!g.map.valid || !c.poseWrite)) {
        g.anim = nullptr;
        g.heldAimPose = false;
    }
    if (g.anim) {
        float at = g.heldAimPose ? g.anim->length() : (float)(t - g.animT0);
        int pel = g.rig.pelvisH;
        if (at > g.anim->length() && !g.heldAimPose) {
            log("hunter: %s finished after %d ticks of writes; the game has the joints again", g.animName.c_str(),
                g.ticksWritten);
            g.anim = nullptr;
        } else if (pel >= 0 && hv[pel]) {
            std::vector<float> G;
            pose_global(*g.skel, g.anim, at, false, &G);
            float out[kHunterBones][16];
            bool set[kHunterBones];
            // Use the body that World actually renders as the world-space basis. A static root
            // rotates Remnant arms about the map origin and makes them leave the hunter's mesh.
            hunter_retarget(g.rig, G.data(), g.bindG.data(), hw[pel], hw[pel], out, set);
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

    const int rightHand = hunter_bone_by_id(12);
    if (g.map.valid && rightHand >= 0 && hv[rightHand]) {
        float dx = hw[rightHand][12] - snap.pos.x;
        float dy = hw[rightHand][13] - snap.pos.y;
        float dz = hw[rightHand][14] - snap.pos.z;
        if (dx * dx + dy * dy + dz * dz < 300.0f * 300.0f) {
            memcpy(g.lastHand, hw[rightHand], 64);
            g.lastHandAt = t;
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
