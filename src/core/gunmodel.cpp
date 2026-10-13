// Remnant x MHW mashup core: the Remnant gun in hand, drawn with Remnant II's own model. GPL-3.0-or-later.
//
// In gun mode the equipped gun's model (sheets/weapons.json mesh, read from the player's Remnant II install) is drawn
// at the hunter's right hand, its barrel on the aim line, with Remnant's own muzzle flash texture at the barrel when
// it fires. Until the diagnostic run finds MHW's hand joint (research.cpp), the hand is placed from the hunter's
// position and the camera (shoulder height, right side). [visuals] gun_model=0 in remnant_hunt.ini turns it off.
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include "bosses.h"
#include "imgui.h"
#include "model.h"
#include "render3d.h"

namespace rh {

namespace {

struct GunDraw {
    bool on = false;
    int row = -1;
    float world[16];
    Vec3 muzzle{};
    double flashUntil = 0;
    std::string anim;  // the gun's own shot or reload animation, and the time in it
    float animT = 0;
};
int g_lastShots = -1;
double g_fireAt = -10, g_reloadStart = 0;
std::mutex g_lock;
GunDraw g_gun;
int g_enabled = -1;
const char* kFlash = "Remnant2/Content/Shared/VFX/Textures/Flares/T_Flare_Muzzle_01.uasset";

Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 mul(Vec3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 unit(Vec3 a) {
    float n = sqrtf(dot(a, a));
    return n > 1e-6f ? mul(a, 1.0f / n) : Vec3{0, 0, 1};
}

// Model forward / right (xy) for a barrel axis (Unreal's frame: facing +x, the right side is +y).
void axes(int fwd, float f[2], float r[2]) {
    switch (fwd) {
        case WEAPONS_MESH_FWD_PY: f[0] = 0; f[1] = 1; r[0] = -1; r[1] = 0; break;
        case WEAPONS_MESH_FWD_NY: f[0] = 0; f[1] = -1; r[0] = 1; r[1] = 0; break;
        case WEAPONS_MESH_FWD_NX: f[0] = -1; f[1] = 0; r[0] = 0; r[1] = -1; break;
        default: f[0] = 1; f[1] = 0; r[0] = 0; r[1] = 1; break;
    }
}

bool enabled() {
    if (g_enabled < 0) {
        char ini[MAX_PATH];
        snprintf(ini, sizeof(ini), "%s\\remnant_hunt.ini", mod_dir());
        g_enabled = GetPrivateProfileIntA("visuals", "gun_model", 1, ini);
    }
    return g_enabled != 0;
}

bool project(const GameView& v, const Vec3& p, ImVec2 vpPos, ImVec2 vpSize, ImVec2* out) {
    float in[4] = {p.x, p.y, p.z, 1}, a[4], c[4];
    for (int j = 0; j < 4; j++) a[j] = in[0] * v.view[j] + in[1] * v.view[4 + j] + in[2] * v.view[8 + j] + in[3] * v.view[12 + j];
    for (int j = 0; j < 4; j++) c[j] = a[0] * v.proj[j] + a[1] * v.proj[4 + j] + a[2] * v.proj[8 + j] + a[3] * v.proj[12 + j];
    if (c[3] <= 1.0f) return false;
    out->x = vpPos.x + (c[0] / c[3] * 0.5f + 0.5f) * vpSize.x;
    out->y = vpPos.y + (0.5f - c[1] / c[3] * 0.5f) * vpSize.y;
    return true;
}

}  // namespace

// Game thread, after the bosses: where the gun is this frame.
void gunmodel_tick(const SaveData& s, const Live& l) {
    GunDraw d;
    const GameSnapshot& g = l.snap;
    int gun = l.gunMode ? equipped_gun(s, l) : -1;
    if (enabled() && gun >= 0 && g.inGame && g.view.valid && strcmp(kWeapons[gun].mesh, "none") != 0) {
        const Model* m = model_get(kWeapons[gun].mesh);
        if (m) {
            bool rh = g.view.proj[11] < 0;
            Vec3 look = unit(sub(g.view.camTarget, g.view.camPos));
            Vec3 flat = unit({look.x, 0, look.z});
            Vec3 right = rh ? Vec3{-flat.z, 0, flat.x} : Vec3{flat.z, 0, -flat.x};
            Vec3 upW = {0, 1, 0};
            // the right hand: at the hunter's right shoulder, forward along the aim
            Vec3 hand = add(add(add(g.pos, mul(upW, l.aiming ? 138.0f : 120.0f)), mul(right, l.aiming ? 18.0f : 26.0f)),
                            mul(flat, l.aiming ? 38.0f : 30.0f));
            Vec3 aimAt = add(g.view.camPos, mul(look, 3000.0f));
            Vec3 fwd = unit(sub(aimAt, hand));
            if (!l.aiming) fwd = unit(add(mul(fwd, 0.6f), mul(upW, -0.35f)));  // held low when not aiming
            Vec3 r = unit(sub(right, mul(fwd, dot(right, fwd))));
            Vec3 up = unit(sub(upW, add(mul(fwd, dot(upW, fwd)), mul(r, dot(upW, r)))));
            float f[2], rr[2];
            axes(kWeapons[gun].mesh_fwd, f, rr);
            Vec3 ix = add(mul(fwd, f[0]), mul(r, rr[0])), iy = add(mul(fwd, f[1]), mul(r, rr[1]));
            float rows[4][4] = {{ix.x, ix.y, ix.z, 0}, {iy.x, iy.y, iy.z, 0}, {up.x, up.y, up.z, 0}, {hand.x, hand.y, hand.z, 1}};
            memcpy(d.world, rows, 64);
            // the muzzle: the far end of the model along the barrel
            float reach = 0;
            for (float x : {m->boundsMin[0], m->boundsMax[0]})
                for (float y : {m->boundsMin[1], m->boundsMax[1]}) reach = std::max(reach, x * f[0] + y * f[1]);
            d.muzzle = add(hand, mul(fwd, reach));
            d.on = true;
            d.row = gun;
            d.flashUntil = l.muzzleUntil;
            // its moving parts: the shot animation after each shot, the reload animation over the reload
            double t = now_s();
            if (l.shots != g_lastShots) {
                if (g_lastShots >= 0) g_fireAt = t;
                g_lastShots = l.shots;
            }
            if (l.reloadDone > 0) {
                if (g_reloadStart == 0) g_reloadStart = t;
            } else {
                g_reloadStart = 0;
            }
            const Weapons& w = kWeapons[gun];
            const Anim* rl = strcmp(w.anim_reload, "none") ? anim_get(w.anim_reload, w.skeleton, m) : nullptr;
            const Anim* fi = strcmp(w.anim_fire, "none") ? anim_get(w.anim_fire, w.skeleton, m) : nullptr;
            if (rl && g_reloadStart > 0 && l.reloadDone > g_reloadStart) {
                d.anim = w.anim_reload;
                d.animT = (float)((t - g_reloadStart) / (l.reloadDone - g_reloadStart)) * rl->length();
            } else if (fi && t - g_fireAt < fi->length()) {
                d.anim = w.anim_fire;
                d.animT = (float)(t - g_fireAt);
            }
        }
    }
    std::lock_guard<std::mutex> lk(g_lock);
    g_gun = d;
}

// Render thread: the gun as one more model in the 3D pass.
void gunmodel_append(std::vector<DrawItem>* items) {
    GunDraw d;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        d = g_gun;
    }
    if (!d.on) return;
    const Model* m = model_get(kWeapons[d.row].mesh);
    if (!m) return;
    DrawItem it;
    it.model = m;
    memcpy(it.world, d.world, 64);
    const Anim* a = d.anim.empty() ? nullptr : anim_get(d.anim.c_str(), kWeapons[d.row].skeleton, m);
    pose(*m, a, d.animT, false, nullptr, 0, 0, &it.palette);  // its rest pose, or the shot / reload moving its parts
    static std::set<std::string> keep;
    size_t slots = m->materials.size();
    for (const ModelSection& s : m->sections) slots = std::max(slots, (size_t)std::max<int16_t>(0, s.material) + 1);
    it.diffuse.assign(slots, nullptr);
    const char* fallback = nullptr;
    std::string all = kWeapons[d.row].mesh_tex;
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
            else
                for (size_t k = 0; k < m->materials.size(); k++)
                    if (!it.diffuse[k] && m->materials[k].find(slot) != std::string::npos) it.diffuse[k] = kept;
        }
        i = j + 1;
    }
    for (auto& t : it.diffuse)
        if (!t) t = fallback;
    items->push_back(std::move(it));
}

// Render thread, inside the ImGui frame: Remnant's muzzle flash at the barrel.
void gunmodel_hud() {
    GunDraw d;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        d = g_gun;
    }
    if (!d.on || now_s() > d.flashUntil) return;
    GameView v;
    if (!camera_view(&v)) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 p;
    if (!project(v, d.muzzle, vp->Pos, vp->Size, &p)) return;
    int w = 0, h = 0;
    void* tex = texture_for(kFlash, 256, &w, &h);
    float sc = vp->Size.y / 1080.0f, r = 46 * sc;
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    if (tex) {
        float a = (float)(now_s() * 37.0);  // a turn of the flare each shot
        ImVec2 c[4];
        for (int k = 0; k < 4; k++) {
            float t = a + k * 1.5707963f;
            c[k] = {p.x + r * cosf(t), p.y + r * sinf(t)};
        }
        bg->AddImageQuad((ImTextureID)(uintptr_t)tex, c[0], c[1], c[2], c[3], {0, 0}, {1, 0}, {1, 1}, {0, 1}, IM_COL32(255, 220, 170, 255));
    } else {
        bg->AddCircleFilled(p, r * 0.5f, IM_COL32(255, 210, 140, 200), 16);
    }
}

}  // namespace rh
