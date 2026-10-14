// Dev preview: draws one of the mod's screens with a sample save to a BMP (Dear ImGui, rendered in software), so the
// look can be checked outside the game. build: make preview-ui; run (Wine): preview_ui.exe <screen 0-5> <dir with fonts\> <out.bmp>
#include <windows.h>

#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "imgui.h"
#include "state.h"

namespace rh {
void ui_init_fonts();
void ui_preview(int which, const SaveData& s, const Live& l);
}
using namespace rh;

// What the screens need from the rest of the mod, stood in for: names from the asset file names, no textures.
namespace rh {
static char g_dir[MAX_PATH];
const char* mod_dir() { return g_dir; }
void log(const char*, ...) {}
Shared& S() { static Shared s; return s; }
double now_s() { return GetTickCount64() / 1000.0; }
const Archetypes* archetype() { return S().save.archetype >= 0 ? &kArchetypes[S().save.archetype] : nullptr; }
void audio_play(const char*, float) {}
void bosses_hud() {}
void choose_archetype(int, int) {}
int equipped_gun(const SaveData& s, const Live&) { return s.longGun; }
int gun_mod(const SaveData& s, int gun) {
    if (gun < 0) return -1;
    return kWeapons[gun].mod >= 0 ? kWeapons[gun].mod : s.gunMod[kWeapons[gun].slot == WEAPONS_SLOT_HAND ? 1 : 0];
}
bool key_pressed(int) { return false; }
int input_reads_per_second() { return 0; }
int menu_flag_read() { return -1; }
const Keys& keys() { static Keys k; return k; }
int mhw_zenny() { return 125400; }
void post(Cmd) {}
bool r2_ready() { return true; }
static std::string pretty(const char* asset) {
    std::string a = asset ? asset : "";
    size_t sl = a.rfind('/'), dot = a.rfind('.');
    std::string b = a.substr(sl == std::string::npos ? 0 : sl + 1, dot == std::string::npos ? std::string::npos : dot - sl - 1);
    for (const char* pre : {"Trait_", "Perk_", "Skill_", "Archetype_", "RelicFragment_", "Weapon_", "Ring_", "Amulet_", "PrimePerk_"})
        if (b.rfind(pre, 0) == 0) b = b.substr(strlen(pre));
    std::string o;
    for (size_t i = 0; i < b.size(); i++) {
        if (i && isupper((unsigned char)b[i]) && islower((unsigned char)b[i - 1])) o += ' ';
        o += b[i] == '_' ? ' ' : b[i];
    }
    return o;
}
const std::string& r2_text(const sheets::R2Text& t) {
    static std::string out[64];
    static int n = 0;
    std::string& o = out[n++ % 64];
    o = pretty(t.asset);
    return o;
}
const std::string& r2_name(const sheets::R2Name&) { static const std::string w = "Wallace"; return w; }
void set_input_block(const InputBlock&) {}
void inject_claw(double) {}
void* texture_for(const char* asset, int maxSide, int* w, int* h, bool srgb);  // real icons through r2read, below
void use_consumable(int) {}
bool can_equip_here(const Live&) { return true; }
void equip(Cat, int, int) {}
bool mhw_inventory_ready() { return true; }
int mhw_item_count(int) { return 3; }
int mhw_pouch_count(int) { return 2; }
bool bar_proxies() { return true; }
volatile long g_remnantScreenOpen = 0;
const char* mhw_item_name(int) { return "Iron Ore"; }
void request(const Request&) {}
void toast(Live&, const char*, ...) {}
int weapon_max_level(int w) { return w >= 0 && kWeapons[w].mod >= 0 ? 10 : 20; }
float upgrade_mult(int, int level) { return 1.0f + level * 0.1f; }
std::string upgrade_cost(int, int, int* z) { *z = 1000; return "ore_low*2"; }
std::vector<int> skills_of(int a) { return a >= 0 ? rows_of(kArchetypes[a].skills, skills_index) : std::vector<int>{}; }
std::vector<int> perks_of(int a) { return a >= 0 ? rows_of(kArchetypes[a].perks, perks_index) : std::vector<int>{}; }
}  // namespace rh
void rh_imgui_assert(const char*, const char*, int) {}

// A small software renderer for Dear ImGui's draw data (textured, vertex-coloured triangles, alpha blended), so the
// preview needs no GPU: it writes the screen to a BMP.
struct Tex { int w = 0, h = 0; std::vector<uint8_t> rgba; };
static std::map<ImTextureID, Tex> g_tex;
static ImTextureID g_nextTex = 1;

// Real icons: the preview starts the mod's own r2read.exe against the Remnant II install (argv[4] = folder holding
// r2read.exe, argv[5] = Remnant II folder) and decodes the BC blocks on the CPU with bcdec, so the screenshots show
// what the game draws. Without the install the icons stay outlined boxes.
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
static HANDLE g_r2In = nullptr, g_r2Out = nullptr;
static bool g_r2Tried = false;
static char g_r2Dir[MAX_PATH], g_r2Game[MAX_PATH];
static std::map<std::string, ImTextureID> g_r2Cache;

static bool r2_write(const std::string& s) {
    DWORD n = 0;
    return WriteFile(g_r2In, s.data(), (DWORD)s.size(), &n, nullptr) && n == s.size();
}
static bool r2_read(void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    while (n) {
        DWORD got = 0;
        if (!ReadFile(g_r2Out, p, (DWORD)n, &got, nullptr) || !got) return false;
        p += got;
        n -= got;
    }
    return true;
}
static bool r2_line(std::string* out) {
    out->clear();
    char c;
    while (r2_read(&c, 1)) {
        if (c == '\n') return true;
        if (c != '\r') out->push_back(c);
    }
    return false;
}
static bool r2_preview_start() {
    if (g_r2Tried) return g_r2In != nullptr;
    g_r2Tried = true;
    char exe[MAX_PATH];
    snprintf(exe, sizeof(exe), "%s\\r2read.exe", g_r2Dir);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES || GetFileAttributesA(g_r2Game) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "preview: no r2read.exe at %s or no Remnant II at %s: icons stay blank\n", exe, g_r2Game);
        return false;
    }
    HANDLE inR, inW, outR, outW;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    CreatePipe(&inR, &inW, &sa, 0);
    CreatePipe(&outR, &outW, &sa, 0);
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::string cmd = std::string("\"") + exe + "\" \"" + g_r2Game + "\"";
    if (!CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        fprintf(stderr, "preview: r2read.exe did not start\n");
        return false;
    }
    CloseHandle(inR);
    CloseHandle(outW);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g_r2In = inW;
    g_r2Out = outR;
    std::string head;
    r2_write("HELLO\n");
    return r2_line(&head) && head.rfind("OK", 0) == 0;
}

namespace rh {
void* texture_for(const char* asset, int maxSide, int* w, int* h, bool) {
    if (!asset || !*asset) return nullptr;
    std::string key = std::string(asset) + "|" + std::to_string(maxSide);
    auto it = g_r2Cache.find(key);
    if (it == g_r2Cache.end()) {
        ImTextureID id = 0;
        if (r2_preview_start()) {
            std::string head;
            char fmt[16] = {0};
            int tw = 0, th = 0;
            size_t n = 0;
            if (r2_write("TEX\t" + std::string(asset) + "\t" + std::to_string(maxSide) + "\n") && r2_line(&head) &&
                sscanf(head.c_str(), "OK %15s %d %d %zu", fmt, &tw, &th, &n) == 4 && tw > 0 && th > 0) {
                std::vector<uint8_t> px(n);
                if (r2_read(px.data(), n)) {
                    Tex t;
                    t.w = tw;
                    t.h = th;
                    t.rgba.assign((size_t)tw * th * 4, 0);
                    std::string f = fmt;
                    int bs = f == "BC7" || f == "BC3" ? 16 : f == "BC1" ? 8 : 0;
                    if (bs) {
                        int bw = (tw + 3) / 4, bh = (th + 3) / 4;
                        std::vector<uint8_t> blk(16 * 4);
                        for (int by = 0; by < bh; by++)
                            for (int bx = 0; bx < bw; bx++) {
                                const uint8_t* src = px.data() + ((size_t)by * bw + bx) * bs;
                                if (src + bs > px.data() + px.size()) break;
                                if (f == "BC7") bcdec_bc7(src, blk.data(), 16);
                                else if (f == "BC3") bcdec_bc3(src, blk.data(), 16);
                                else bcdec_bc1(src, blk.data(), 16);
                                for (int y = 0; y < 4 && by * 4 + y < th; y++)
                                    for (int x = 0; x < 4 && bx * 4 + x < tw; x++)
                                        memcpy(&t.rgba[(((size_t)by * 4 + y) * tw + bx * 4 + x) * 4], &blk[(y * 4 + x) * 4], 4);
                            }
                    } else if (f == "BGRA8" && n >= (size_t)tw * th * 4) {
                        for (size_t i = 0; i < (size_t)tw * th; i++) {
                            t.rgba[i * 4] = px[i * 4 + 2];
                            t.rgba[i * 4 + 1] = px[i * 4 + 1];
                            t.rgba[i * 4 + 2] = px[i * 4];
                            t.rgba[i * 4 + 3] = px[i * 4 + 3];
                        }
                    } else {
                        bs = -1;
                    }
                    if (bs >= 0) {
                        id = g_nextTex++;
                        g_tex[id] = t;
                    }
                }
            } else if (!head.empty()) {
                fprintf(stderr, "preview: %s: %s\n", asset, head.c_str());
            }
        }
        it = g_r2Cache.emplace(key, id).first;
    }
    if (!it->second) return nullptr;
    const Tex& t = g_tex[it->second];
    if (w) *w = t.w;
    if (h) *h = t.h;
    return (void*)(uintptr_t)it->second;
}
}  // namespace rh

static void textures(ImDrawData* dd) {
    if (!dd->Textures) return;
    for (ImTextureData* t : *dd->Textures) {
        if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates) {
            ImTextureID id = t->TexID ? t->TexID : g_nextTex++;
            Tex& x = g_tex[id];
            x.w = t->Width;
            x.h = t->Height;
            x.rgba.resize((size_t)x.w * x.h * 4);
            const uint8_t* src = (const uint8_t*)t->GetPixels();
            for (int i = 0; i < x.w * x.h; i++) {
                if (t->Format == ImTextureFormat_RGBA32) memcpy(&x.rgba[i * 4], src + i * 4, 4);
                else { x.rgba[i * 4] = x.rgba[i * 4 + 1] = x.rgba[i * 4 + 2] = 255; x.rgba[i * 4 + 3] = src[i]; }
            }
            t->SetTexID(id);
            t->SetStatus(ImTextureStatus_OK);
        } else if (t->Status == ImTextureStatus_WantDestroy) {
            g_tex.erase(t->TexID);
            t->SetTexID(ImTextureID_Invalid);
            t->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

static void raster(ImDrawData* dd, std::vector<float>& px, int W, int H) {
    for (int n = 0; n < dd->CmdListsCount; n++) {
        const ImDrawList* cl = dd->CmdLists[n];
        for (const ImDrawCmd& c : cl->CmdBuffer) {
            if (c.UserCallback) continue;
            auto it = g_tex.find(c.GetTexID());
            const Tex* tex = it != g_tex.end() ? &it->second : nullptr;
            int cx0 = std::max(0, (int)c.ClipRect.x), cy0 = std::max(0, (int)c.ClipRect.y);
            int cx1 = std::min(W, (int)c.ClipRect.z), cy1 = std::min(H, (int)c.ClipRect.w);
            for (unsigned e = 0; e + 2 < c.ElemCount; e += 3) {
                const ImDrawVert* v[3];
                for (int k = 0; k < 3; k++) v[k] = &cl->VtxBuffer[c.VtxOffset + cl->IdxBuffer[c.IdxOffset + e + k]];
                float area = (v[1]->pos.x - v[0]->pos.x) * (v[2]->pos.y - v[0]->pos.y) - (v[2]->pos.x - v[0]->pos.x) * (v[1]->pos.y - v[0]->pos.y);
                if (fabsf(area) < 1e-6f) continue;
                int x0 = std::max(cx0, (int)floorf(std::min({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x})));
                int x1 = std::min(cx1, (int)ceilf(std::max({v[0]->pos.x, v[1]->pos.x, v[2]->pos.x})));
                int y0 = std::max(cy0, (int)floorf(std::min({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y})));
                int y1 = std::min(cy1, (int)ceilf(std::max({v[0]->pos.y, v[1]->pos.y, v[2]->pos.y})));
                for (int y = y0; y < y1; y++)
                    for (int x = x0; x < x1; x++) {
                        float qx = x + 0.5f, qy = y + 0.5f, w[3];
                        for (int k = 0; k < 3; k++) {
                            const ImDrawVert *a = v[(k + 1) % 3], *b = v[(k + 2) % 3];
                            w[k] = ((b->pos.x - a->pos.x) * (qy - a->pos.y) - (qx - a->pos.x) * (b->pos.y - a->pos.y)) / area;
                        }
                        if (w[0] < -1e-4f || w[1] < -1e-4f || w[2] < -1e-4f) continue;
                        float col[4] = {0, 0, 0, 0}, u = 0, vv = 0;
                        for (int k = 0; k < 3; k++) {
                            ImU32 cc = v[k]->col;
                            for (int ch = 0; ch < 4; ch++) col[ch] += w[k] * ((cc >> (8 * ch)) & 255) / 255.0f;
                            u += w[k] * v[k]->uv.x;
                            vv += w[k] * v[k]->uv.y;
                        }
                        if (tex && tex->w > 0) {
                            int tx = std::min(tex->w - 1, std::max(0, (int)(u * tex->w))), ty = std::min(tex->h - 1, std::max(0, (int)(vv * tex->h)));
                            const uint8_t* t = &tex->rgba[((size_t)ty * tex->w + tx) * 4];
                            for (int ch = 0; ch < 4; ch++) col[ch] *= t[ch] / 255.0f;
                        }
                        float* d = &px[((size_t)y * W + x) * 3];
                        for (int ch = 0; ch < 3; ch++) d[ch] = d[ch] * (1 - col[3]) + col[ch] * col[3];
                    }
            }
        }
    }
}

static void write_bmp(const char* path, const std::vector<float>& px, int W, int H) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    int row = (W * 3 + 3) & ~3, size = 54 + row * H;
    uint8_t h[54] = {'B', 'M'};
    memcpy(h + 2, &size, 4);
    int off = 54, hs = 40, planes = 1 | (24 << 16), img = row * H;
    memcpy(h + 10, &off, 4);
    memcpy(h + 14, &hs, 4);
    memcpy(h + 18, &W, 4);
    memcpy(h + 22, &H, 4);
    memcpy(h + 26, &planes, 4);
    memcpy(h + 34, &img, 4);
    fwrite(h, 1, 54, f);
    std::vector<uint8_t> line(row, 0);
    for (int y = H - 1; y >= 0; y--) {
        for (int x = 0; x < W; x++)
            for (int ch = 0; ch < 3; ch++)
                line[x * 3 + ch] = (uint8_t)std::min(255.0f, std::max(0.0f, px[((size_t)y * W + x) * 3 + (2 - ch)] * 255.0f));
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
}

int main(int argc, char** argv) {
    int which = argc > 1 ? atoi(argv[1]) : 0;
    snprintf(g_dir, sizeof(g_dir), "%s", argc > 2 ? argv[2] : ".");
    const char* out = argc > 3 ? argv[3] : "preview.bmp";
    snprintf(g_r2Dir, sizeof(g_r2Dir), "%s", argc > 4 ? argv[4] : "build\\win");
    snprintf(g_r2Game, sizeof(g_r2Game), "%s", argc > 5 ? argv[5] : "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Remnant2");
    const int W = 1600, H = 900;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)W, (float)H);
    io.DeltaTime = 1.0f / 60;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.IniFilename = nullptr;
    ui_init_fonts();
    // a sample hunter
    SaveData s;
    s.archetype = archetypes_index("gunslinger");
    s.archetype2 = archetypes_index("hunter");
    s.archXp[s.archetype] = 40000;
    s.archXp[s.archetype2] = 12000;
    s.xp = 52000;
    for (int i = 0; i < kTraitsCount; i += 3) s.owned[CAT_TRAIT][i] = 1;
    s.trait[traits_index("vigor")] = 4;
    s.trait[traits_index("barkskin")] = 3;
    s.owned[CAT_TRAIT][traits_index("barkskin")] = 1;
    int pr = prisms_index("voracity");
    s.owned[CAT_PRISM][pr] = 1;
    s.prismOn = pr;
    SaveData::Prism& p = s.prism[pr];
    p.slot[0] = {PS_FRAGMENT, (int16_t)fragments_index("critical_damage"), 7};
    p.slot[1] = {PS_FRAGMENT, (int16_t)fragments_index("weakspot_damage"), 5};
    p.slot[2] = {PS_FRAGMENT, (int16_t)fragments_index("reload_speed"), 3};
    p.xp = 12000;
    prism_offer(p);
    s.longGun = 0; s.owned[CAT_WEAPON][0] = 1;
    Live l;
    if (which == 5) {  // the HUD: a long gun with a mod, mid-reload, one skill cooling, a toast
        s.gunMod[0] = 0;
        const Weapons& w = kWeapons[s.longGun];
        double t = now_s();
        l.snap.inGame = true;
        l.snap.view.valid = false;
        l.combatAllowed = true;
        l.gunMode = true;
        l.mag[s.longGun] = 7;
        l.reserve[s.longGun] = 56;
        l.reloadDone = t + 0.6 * w.reload_s;
        l.skillReady[0] = t + 12;
        l.skillReady[1] = 0;
        s.relic = 0;
        s.owned[CAT_RELIC][0] = 1;
        s.relicCharges = 3;
        snprintf(l.toast, sizeof(l.toast), "Prism of Voracity gained");
        l.toastUntil = t + 5;
        l.modCharge = 0.6f * kMods[gun_mod(s, s.longGun)].charge_damage;
        l.bonusHealth = 40;
        l.bonusMax = 60;
        l.nearMenu = -1;
    }
    S().save = s;
    for (int frame = 0; frame < 4; frame++) {
        io.MousePos = ImVec2(-1, -1);
        ImGui::NewFrame();
        ui_preview(which, s, l);
        ImGui::Render();
        textures(ImGui::GetDrawData());
    }
    std::vector<float> px((size_t)W * H * 3);
    for (size_t i = 0; i < px.size(); i += 3) { px[i] = 0.20f; px[i + 1] = 0.24f; px[i + 2] = 0.17f; }  // the game behind
    raster(ImGui::GetDrawData(), px, W, H);
    write_bmp(out, px, W, H);
    printf("wrote %s\n", out);
    return 0;
}
