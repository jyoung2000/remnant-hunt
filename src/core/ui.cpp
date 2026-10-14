// Remnant x MHW mashup core: screens and HUD. GPL-3.0-or-later (see LICENSE).
//
// The screens for Remnant tasks: the worldstone intro, archetypes, the Ward 13 terminal (the Remnant loadout), traits,
// prisms and the Remnant menus at MHW's own NPCs (crafting Remnant gear from MHW materials), all drawn in Monster
// Hunter: World's menu style: dark warm panels framed in gold, an amber cursor, cream text and MHW's rarity colours;
// a small HUD in the same colours for the guns, skills and relic. Names, descriptions, lines and icons are read from the player's own
// Remnant II install; effect lines are our own wording; MHW's own UI is left as it is.
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "bosses.h"
#include "state.h"

namespace rh {

// ---------------------------------------------------------------- look

// Monster Hunter: World's menu colours.
namespace col {
constexpr ImU32 bg = IM_COL32(17, 16, 14, 238), panel = IM_COL32(31, 28, 23, 242), line = IM_COL32(104, 89, 60, 255);
constexpr ImU32 text = IM_COL32(240, 232, 211, 255), dim = IM_COL32(163, 152, 130, 255);
constexpr ImU32 accent = IM_COL32(232, 168, 58, 255), gold = IM_COL32(216, 186, 118, 255);
constexpr ImU32 frameHi = IM_COL32(196, 166, 104, 255), frameLo = IM_COL32(70, 60, 42, 255);
constexpr ImU32 mhwFrame = IM_COL32(28, 23, 18, 205), mhwGold = IM_COL32(184, 154, 94, 255);
constexpr ImU32 mhwText = IM_COL32(242, 232, 207, 255), red = IM_COL32(214, 58, 46, 255);
}  // namespace col

static ImFont* g_body = nullptr;   // Montserrat Medium
static ImFont* g_bold = nullptr;   // Montserrat Bold
static ImFont* g_title = nullptr;  // EB Garamond Bold
ImFont* ui_font(int which) { return which == 2 ? g_title : which == 1 ? g_bold : g_body; }

void ui_init_fonts() {
    ImGuiIO& io = ImGui::GetIO();
    char p[MAX_PATH];
    auto load = [&](const char* file, float size) -> ImFont* {
        snprintf(p, sizeof(p), "%s\\fonts\\%s", mod_dir(), file);
        if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) return nullptr;
        return io.Fonts->AddFontFromFileTTF(p, size);
    };
    g_body = load("Montserrat-Medium.ttf", 18.0f);
    g_bold = load("Montserrat-Bold.ttf", 18.0f);
    g_title = load("EBGaramond-Bold.ttf", 34.0f);
    if (!g_body) g_body = io.Fonts->AddFontDefault();
    if (!g_bold) g_bold = g_body;
    if (!g_title) g_title = g_body;
    io.FontDefault = g_body;
    io.ConfigErrorRecovery = true;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0;
    st.FrameRounding = 0;
    st.WindowBorderSize = 1;
    st.Colors[ImGuiCol_WindowBg] = ImColor(col::panel);
    st.Colors[ImGuiCol_Border] = ImColor(col::line);
    st.Colors[ImGuiCol_Text] = ImColor(col::text);
    st.Colors[ImGuiCol_TextDisabled] = ImColor(col::dim);
    st.Colors[ImGuiCol_Button] = ImVec4(0.14f, 0.13f, 0.11f, 1);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.45f, 0.32f, 0.12f, 1);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.91f, 0.66f, 0.23f, 1);
    st.Colors[ImGuiCol_Separator] = ImColor(col::line);
    st.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_ScrollbarGrab] = ImColor(col::frameLo);
    st.Colors[ImGuiCol_ScrollbarGrabHovered] = ImColor(col::line);
    st.Colors[ImGuiCol_ScrollbarGrabActive] = ImColor(col::gold);
    st.Colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_FrameBg] = ImVec4(0.09f, 0.085f, 0.075f, 1);
    st.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.14f, 0.11f, 1);
    st.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.18f, 0.12f, 1);
    st.Colors[ImGuiCol_Tab] = ImVec4(0.12f, 0.11f, 0.09f, 1);
    st.Colors[ImGuiCol_TabHovered] = ImVec4(0.45f, 0.32f, 0.12f, 1);
    st.Colors[ImGuiCol_TabSelected] = ImVec4(0.55f, 0.43f, 0.22f, 1);
    st.Colors[ImGuiCol_TabSelectedOverline] = ImColor(col::accent);
    st.Colors[ImGuiCol_Header] = ImVec4(0.30f, 0.24f, 0.14f, 1);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.45f, 0.32f, 0.12f, 1);
    st.Colors[ImGuiCol_PlotHistogram] = ImColor(col::gold);
    st.Colors[ImGuiCol_CheckMark] = ImColor(col::accent);
    st.Colors[ImGuiCol_PopupBg] = ImColor(col::panel);
    st.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.55f, 0.43f, 0.22f, 0.6f);
    st.ScrollbarSize = 10;
    st.TabBarBorderSize = 1;
}

static std::string upper(const std::string& s) {
    std::string o = s;
    for (char& c : o)
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    return o;
}

static void image(const char* asset, int maxSide, ImVec2 size, ImU32 tint = IM_COL32_WHITE) {
    int w = 0, h = 0;
    void* tex = texture_for(asset, maxSide, &w, &h);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (tex && w > 0) {
        float k = std::min(size.x / w, size.y / h);
        ImVec2 sz(w * k, h * k), o(p.x + (size.x - sz.x) / 2, p.y + (size.y - sz.y) / 2);
        dl->AddImage((ImTextureID)(uintptr_t)tex, o, ImVec2(o.x + sz.x, o.y + sz.y), ImVec2(0, 0), ImVec2(1, 1), tint);
    } else {
        dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), col::line);
    }
    ImGui::Dummy(size);
}

static void title(const char* s, ImU32 c = col::text) {
    ImGui::PushFont(g_title, 34.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImColor(c).Value);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// A small gold diamond, MHW's list and header mark.
static void diamond(ImDrawList* dl, ImVec2 c, float r, ImU32 fill) {
    dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), fill);
}

// Section heading, MHW style: a diamond, the words, a thin gold rule.
static void label(const char* s, ImU32 c = col::dim) {
    ImGui::PushFont(g_bold, 14.0f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    diamond(dl, ImVec2(p.x + 4, p.y + h * 0.5f), 3.5f, col::gold);
    ImGui::SetCursorScreenPos(ImVec2(p.x + 13, p.y));
    ImGui::PushStyleColor(ImGuiCol_Text, ImColor(c).Value);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
    ImVec2 e = ImGui::GetItemRectMax();
    float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (right - e.x > 40) dl->AddLine(ImVec2(e.x + 8, p.y + h * 0.5f), ImVec2(right, p.y + h * 0.5f), col::frameLo);
    ImGui::PopFont();
}

static void quote(const std::string& s) {
    if (s.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImColor(col::dim).Value);
    ImGui::TextWrapped("\"%s\"", s.c_str());
    ImGui::PopStyleColor();
}

// A button in MHW's style: a dark plate in a thin gold frame; the amber cursor (a lit frame and a diamond) on hover.
static bool remnant_button(const char* text, ImVec2 size = ImVec2(0, 0), bool enabled = true) {
    ImGui::PushFont(g_bold, 18.0f);
    const char* end = strstr(text, "##");  // ImGui's hidden-id suffix is not drawn
    if (!end) end = text + strlen(text);
    ImVec2 ts = ImGui::CalcTextSize(text, end);
    float avail = ImGui::GetContentRegionAvail().x;
    ImVec2 sz(size.x > 0 ? size.x : size.x < 0 ? std::max(10.0f, avail + size.x) : ts.x + 28,
              size.y > 0 ? size.y : ts.y + 12);
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + sz.x, a.y + sz.y);
    bool r = ImGui::InvisibleButton(text, sz);
    bool hov = enabled && ImGui::IsItemHovered(), held = enabled && ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 top = held ? IM_COL32(120, 86, 34, 255) : hov ? IM_COL32(74, 58, 32, 255) : IM_COL32(38, 35, 29, 255);
    ImU32 bot = held ? IM_COL32(90, 64, 26, 255) : hov ? IM_COL32(52, 41, 24, 255) : IM_COL32(26, 24, 20, 255);
    dl->AddRectFilledMultiColor(a, b, top, top, bot, bot);
    dl->AddRect(a, b, !enabled ? col::frameLo : hov ? col::accent : col::line, 0, 0, hov ? 1.5f : 1.0f);
    if (hov) diamond(dl, ImVec2(a.x + 9, (a.y + b.y) * 0.5f), 4.0f, col::accent);
    ImU32 tc = !enabled ? IM_COL32(110, 102, 88, 255) : hov ? IM_COL32(255, 244, 220, 255) : col::text;
    dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), tc, text, end);
    ImGui::PopFont();
    return r && enabled;
}

// Full-screen Remnant window: darkened game behind, corner brackets.
static bool begin_screen(const char* id, float wFrac, float hFrac) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                  IM_COL32(0, 0, 0, 170));
    ImVec2 size(vp->Size.x * wFrac, vp->Size.y * hFrac);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - size.x) / 2, vp->Pos.y + (vp->Size.y - size.y) / 2));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImColor(col::bg).Value);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28, 24));
    bool open = ImGui::Begin(id, nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    // MHW's menu frame: a darker band at the top, a double gold border with diamonds at its corners and on the top
    // and bottom edges, and the back hint at the foot.
    ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + s.x, p.y + s.y);
    dl->AddRectFilledMultiColor(p, ImVec2(q.x, p.y + 64), IM_COL32(40, 35, 27, 255), IM_COL32(40, 35, 27, 255),
                                IM_COL32(17, 16, 14, 0), IM_COL32(17, 16, 14, 0));
    dl->AddRect(p, q, col::frameHi, 0, 0, 1.5f);
    dl->AddRect(ImVec2(p.x + 5, p.y + 5), ImVec2(q.x - 5, q.y - 5), col::frameLo, 0, 0, 1.0f);
    for (ImVec2 c : {p, ImVec2(q.x, p.y), ImVec2(p.x, q.y), q, ImVec2((p.x + q.x) * 0.5f, p.y), ImVec2((p.x + q.x) * 0.5f, q.y)})
        diamond(dl, c, 6.0f, col::frameHi);
    ImGui::PushFont(g_body, 15.0f);
    const char* hint = "[Esc]  Back";
    ImVec2 hs = ImGui::CalcTextSize(hint);
    dl->AddText(ImVec2(q.x - hs.x - 24, q.y - hs.y - 10), col::dim, hint);
    ImGui::PopFont();
    return open;
}
static void end_screen() {
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- screens

enum Screen { SCR_NONE, SCR_INTRO, SCR_ARCHETYPE, SCR_TERMINAL, SCR_MENU, SCR_TRAITS, SCR_PRISM };
static Screen g_screen = SCR_NONE;
static double g_screenOpened = 0;
static int g_menu = -1;          // menus row (or -1 with g_menuRole set, opened from the terminal)
static int g_menuRole = 0;       // MENUS_ROLE_*
static int g_greet = 0;
static std::string g_npcSays;
static ItemView g_pick;          // selected item in a list
static int g_slotPick = -1;      // loadout slot being changed
static char g_filter[64] = {0};

static void ui_sound(const char* id) {
    int i = ui_sounds_index(id);
    if (i >= 0) audio_play(kUi_sounds[i].event, 0.7f);
}

static void open(Screen s) {
    if (s != SCR_NONE && g_screen == SCR_NONE) ui_sound("open");
    g_screen = s;
    g_screenOpened = now_s();
    g_greet = (int)(GetTickCount() / 7) % 3;
    g_npcSays.clear();
    g_pick = ItemView();
    g_slotPick = -1;
    g_filter[0] = 0;
}

static const sheets::R2Text& greet_of(const Npcs& n, int k) { return k == 0 ? n.greet_1 : k == 1 ? n.greet_2 : n.greet_3; }

static int host_of_role(int role) {
    for (int i = 0; i < kMenusCount; i++)
        if (kMenus[i].role == role) return kMenus[i].host;
    return 0;
}
static const char* role_place(int role) {
    return role == MENUS_ROLE_SMITHY ? "the Smithy" : role == MENUS_ROLE_MELDER ? "the Elder Melder" : "the Provisions Manager";
}
static bool role_has_spot(int role) {
    for (int i = 0; i < kMenusCount; i++)
        if (kMenus[i].role == role && (kMenus[i].x != 0 || kMenus[i].z != 0)) return true;
    return false;
}

static void open_menu(int menuRow, int role) {
    open(SCR_MENU);
    g_menu = menuRow;
    g_menuRole = role;
}

static std::string text_or(const R2Text* t, const char* fallback = "...") {
    if (!t || !t->asset) return "";
    const std::string& s = r2_text(*t);
    return s.empty() ? fallback : s;
}

static void screen_intro() {
    if (!begin_screen("##intro", 0.56f, 0.62f)) return end_screen();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX((w - 140) / 2 + 28);
    image(kUi[UI_INTRO].art, 256, ImVec2(140, 140));
    ImGui::Spacing();
    title("THE WORLDSTONE");
    ImGui::Separator();
    ImGui::Spacing();
    double t = now_s() - g_screenOpened;
    const char* lines[] = {
        "The worldstone in Ward 13 hums, and the red haze of the Root folds around you.",
        "When it clears, the air is warm and green. Something enormous calls over a sea of trees.",
        "This is not Earth. The people here call it the New World, and they hunt monsters for a living.",
        "Your weapons, your training and the Dragon Heart came through with you. The hunt carries on here."};
    for (int i = 0; i < 4; i++) {
        float a = (float)std::min(1.0, std::max(0.0, (t - i * 0.9) / 0.8));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.875f, 0.82f, a));
        ImGui::TextWrapped("%s", lines[i]);
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 70);
    bool ready = t > 3.0;  // the button is always submitted (ImGui needs an item after SetCursorPos)
    if ((remnant_button("CONTINUE", ImVec2(200, 40), ready) || (ready && ImGui::IsKeyPressed(ImGuiKey_Enter))))
        open(SCR_ARCHETYPE);
    end_screen();
}

// ---- shared pieces

static void effect_lines(const char* dsl) {
    for (const Effect& e : effects_of(dsl)) {
        ImGui::Bullet();
        ImGui::TextWrapped("%s", describe(e).c_str());
    }
}

static void stats_line(const char* k, const char* fmt, double v) {
    ImGui::TextColored(ImColor(col::dim).Value, "%s", k);
    ImGui::SameLine(170);
    ImGui::Text(fmt, v);
}

static const char* material_label(int m) {
    int id = (int)kMaterials[m].mhw_item_id;
    const char* n = id > 0 ? mhw_item_name(id) : nullptr;
    return n ? n : kMaterials[m].note;
}

// Recipe in MHW materials and zenny, with what the hunter has. Returns true when it can be paid.
static bool recipe_box(const char* recipe, int zenny) {
    bool ok = true;
    label("COST");
    bool inv = mhw_inventory_ready();
    for (const Need& n : recipe_of(recipe)) {
        int id = (int)kMaterials[n.material].mhw_item_id;
        int have = id > 0 && inv ? mhw_item_count(id) : -1;
        bool enough = have >= n.count;
        ok &= enough;
        ImU32 c = have < 0 ? col::dim : enough ? col::text : col::red;
        ImGui::TextColored(ImColor(c).Value, "%d x %s", n.count, material_label(n.material));
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90);
        if (have >= 0) ImGui::TextColored(ImColor(col::dim).Value, "have %d", have);
        else ImGui::TextColored(ImColor(col::dim).Value, "not found yet");
    }
    int z = mhw_zenny();
    bool zok = z >= zenny;
    ok &= zok;
    if (zenny > 0) ImGui::TextColored(ImColor(zok ? col::gold : col::red).Value, "%d zenny", zenny);
    return ok;
}

static void weapon_stats(const Weapons& w, int level) {
    float m = upgrade_mult((int)(&w - kWeapons), level);
    stats_line("Damage", "%.0f", w.damage * std::max(1.0f, w.pellets) * m);
    if (w.slot != WEAPONS_SLOT_MELEE) {
        stats_line("Fire rate", "%.0f rpm", w.rpm);
        stats_line("Magazine", "%.0f", w.mag);
        stats_line("Reserve", "%.0f", w.reserve);
        stats_line("Reload", "%.1f s", w.reload_s);
        stats_line("Ideal range", "%.0f m", w.range_m);
    }
    stats_line("Critical chance", "%.0f%%", w.crit);
    stats_line("Weakspot", "x%.2f", w.weakspot_mult);
    if (w.mod >= 0) {
        label("MOD");
        image(kMods[w.mod].icon, 128, ImVec2(34, 34));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text_or(&kMods[w.mod].name).c_str());
        effect_lines(kMods[w.mod].effects);
    }
}

// ---- small pieces for the progression screens

template <class F>
static void change_save(F f) {
    Shared& sh = S();
    AcquireSRWLockExclusive(&sh.lock);
    if (f(sh.save)) {
        sh.live.loadoutChanged = true;
        sh.live.dirty = true;
    }
    ReleaseSRWLockExclusive(&sh.lock);
}

static void dim_wrapped(const char* s) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImColor(col::dim).Value);
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
}

static void bar(float k, ImU32 c) {
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImColor(c).Value);
    ImGui::ProgressBar(std::max(0.0f, std::min(1.0f, k)), ImVec2(-1, 6), "");
    ImGui::PopStyleColor();
}

static void level_line(const SaveData& s) {
    int lv = remnant_level(s.xp);
    double a = s.xp - level_xp(lv), b = level_xp(lv + 1) - level_xp(lv);
    ImGui::TextColored(ImColor(col::gold).Value, "REMNANT LEVEL %d", lv);
    ImGui::SameLine();
    ImGui::TextColored(ImColor(col::dim).Value, "  %.0f / %.0f experience to the next", a, b);
    bar((float)(a / std::max(1.0, b)), col::gold);
}

// Ten pips: the archetypes' own points (dim gold), then the hunter's (gold), then empty.
static void pips(int n, int of, int automatic = 0) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    for (int i = 0; i < of; i++) {
        ImVec2 a(p.x + i * 12.0f, p.y + 4), b(a.x + 9, a.y + 9);
        if (i < automatic) dl->AddRectFilled(a, b, IM_COL32(150, 122, 70, 255));
        else if (i < n) dl->AddRectFilled(a, b, col::gold);
        else dl->AddRect(a, b, col::line);
    }
    ImGui::Dummy(ImVec2(of * 12.0f, 16));
}

static const char* perk_kind_label(int perk) {
    switch (kPerks[perk].type) {
        case PERKS_TYPE_PRIME: return "PRIME";
        case PERKS_TYPE_DAMAGE: return "DAMAGE";
        case PERKS_TYPE_TEAM: return "TEAM";
        case PERKS_TYPE_UTILITY: return "UTILITY";
        default: return "RELIC";
    }
}

static void archetype_detail(int row, const SaveData& s, bool chooseSkill) {
    const Archetypes& a = kArchetypes[row];
    int level = archetype_level(s, row);
    int slot = row == s.archetype ? 0 : row == s.archetype2 ? 1 : -1;
    // Level and experience (Remnant II: 1 to 10, the two equipped archetypes share the experience)
    ImGui::TextColored(ImColor(col::gold).Value, "LEVEL %d", level);
    double next = archetype_next_xp(level);
    if (next > 0) {
        ImGui::SameLine();
        ImGui::TextColored(ImColor(col::dim).Value, "  %.0f / %.0f experience", s.archXp[row], next);
        bar((float)(s.archXp[row] / next), col::gold);
    } else {
        ImGui::SameLine();
        ImGui::TextColored(ImColor(col::dim).Value, "  mastered: its trait can be used with any archetype");
    }
    if (a.prime.asset) {
        label("PRIME PERK");
        ImGui::TextWrapped("%s", text_or(&a.prime).c_str());
    }
    std::vector<int> sk = skills_of(row);
    label("SKILLS");
    int open = skills_unlocked(level);
    static const int kSkillLevel[3] = {1, 5, 10};
    for (size_t i = 0; i < sk.size(); i++) {
        const Skills& k = kSkills[sk[i]];
        ImGui::PushID((int)i);
        image(k.icon, 128, ImVec2(36, 36), (int)i < open ? IM_COL32_WHITE : IM_COL32(110, 110, 110, 200));
        ImGui::SameLine();
        ImGui::BeginGroup();
        bool chosen = slot >= 0 && skill_index(s, slot) == (int)i;
        bool unlocked = (int)i < open;
        ImGui::TextColored(ImColor(!unlocked ? col::dim : chosen ? col::gold : col::text).Value, "%s  (cooldown %.0fs)%s",
                           text_or(&k.name).c_str(), k.cooldown_s, chosen ? "  - in use" : "");
        if (!unlocked) ImGui::TextColored(ImColor(col::dim).Value, "Unlocks at level %d", kSkillLevel[std::min<size_t>(2, i)]);
        effect_lines(k.effects);
        if (chooseSkill && slot >= 0 && !chosen && unlocked && remnant_button("USE THIS SKILL", ImVec2(180, 28))) {
            Shared& sh = S();
            AcquireSRWLockExclusive(&sh.lock);
            sh.save.skill[slot] = (int)i;
            sh.live.dirty = true;
            ReleaseSRWLockExclusive(&sh.lock);
        }
        ImGui::EndGroup();
        ImGui::PopID();
    }
    std::vector<int> pk = perks_of(row);
    label("PERKS");
    for (int p : pk) {
        float k = perk_scale(p, level);
        bool prime = kPerks[p].kind == PERKS_KIND_PRIME;
        bool active = k > 0 && (!prime || slot == 0 || (slot == 1 && prime_time(s)));
        ImGui::TextColored(ImColor(active ? (prime ? col::gold : col::text) : col::dim).Value, "%s", text_or(&kPerks[p].name).c_str());
        ImGui::SameLine();
        char st[96];
        if (k <= 0) snprintf(st, sizeof(st), "%s  -  unlocks at level %d", perk_kind_label(p), perk_unlock_level(p));
        else if (prime && slot != 0 && !prime_time(s)) snprintf(st, sizeof(st), "%s  -  only as the primary archetype", perk_kind_label(p));
        else snprintf(st, sizeof(st), "%s  -  %d%% strength%s", perk_kind_label(p), (int)lroundf(k * 100), k < 1 ? " (grows with level)" : "");
        ImGui::TextColored(ImColor(col::dim).Value, "%s", st);
        ImGui::Indent();
        effect_lines(kPerks[p].effects);
        ImGui::Unindent();
    }
    if (a.trait >= 0) {
        label("ARCHETYPE TRAIT");
        ImGui::TextUnformatted(text_or(&kTraits[a.trait].name).c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImColor(col::dim).Value, "  level %d of 10 while equipped (one per archetype level)", level);
        ImGui::Indent();
        effect_lines(kTraits[a.trait].effects);
        ImGui::Unindent();
    }
    label("CORE TRAITS AS PRIMARY");
    ImGui::TextColored(ImColor(col::dim).Value, "Vigor %d  -  Endurance %d  -  Spirit %d  -  Expertise %d", (int)a.core_vigor,
                       (int)a.core_endurance, (int)a.core_spirit, (int)a.core_expertise);
}

// MHW shows an item's rarity (RARE 1-12) in its own colour; the mod's tiers 1-5 map onto it.
static int rarity_of(int tier) { return std::max(1, std::min(12, tier * 2)); }
static ImU32 rarity_color(int tier) {
    switch (std::max(1, std::min(5, tier))) {
        case 1: return IM_COL32(226, 226, 220, 255);  // RARE 2: white
        case 2: return IM_COL32(126, 200, 96, 255);   // RARE 4: green
        case 3: return IM_COL32(98, 150, 232, 255);   // RARE 6: blue
        case 4: return IM_COL32(236, 150, 60, 255);   // RARE 8: orange
        default: return IM_COL32(190, 112, 226, 255); // RARE 10: purple
    }
}

static void item_detail(const ItemView& v, const SaveData& s) {
    float w = ImGui::GetContentRegionAvail().x;
    image(v.icon, 512, ImVec2(w, v.cat == CAT_WEAPON ? 110 : 96));
    title(upper(text_or(v.name)).c_str());
    ImGui::TextColored(ImColor(rarity_color(v.tier)).Value, "%s  -  RARE %d", cat_label(v.cat), rarity_of(v.tier));
    if (v.desc && v.desc->asset) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImColor(col::dim).Value);
        ImGui::TextWrapped("%s", text_or(v.desc).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    if (v.cat == CAT_ARCHETYPE) {
        archetype_detail(v.row, s, false);
        return;
    }
    if (v.cat == CAT_PRISM)
        dim_wrapped("A prism holds five relic fragments that grow as you hunt with it equipped, fuse in pairs and end "
                    "in a legendary bonus. Equip it and choose its fragments in the PRISM screen at Ward 13.");
    if (v.effects && *v.effects) {
        label(v.cat == CAT_TRAIT ? "AT LEVEL 10 (TRAIT POINTS FROM REMNANT LEVELS)" : "EFFECTS");
        effect_lines(v.effects);
    }
    if (v.cat == CAT_WEAPON) weapon_stats(kWeapons[v.row], s.level[v.row]);
    if (v.cat == CAT_MOD) stats_line("Charges after", "%.0f damage", kMods[v.row].charge_damage);
}

static bool matches_filter(const ItemView& v) {
    if (!g_filter[0]) return true;
    std::string n = upper(text_or(v.name, "")), f = upper(g_filter);
    return n.find(f) != std::string::npos;
}

// A list of rows; returns true when one was clicked.
static void item_list(Cat c, int weaponSlot, const SaveData& s, bool ownedOnly, int menuRole) {
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Search", g_filter, sizeof(g_filter));
    ImGui::BeginChild("rows", ImVec2(0, 0));
    for (int i = 0; i < cat_count(c); i++) {
        ItemView v = item(c, i);
        if (menuRole >= 0 && v.menu != menuRole) continue;
        if (c == CAT_WEAPON && weaponSlot >= 0 && kWeapons[i].slot != weaponSlot) continue;
        bool own = c == CAT_CONSUMABLE ? s.stock[i] > 0 : s.has(c, i);
        if (ownedOnly && !own) continue;
        if (!matches_filter(v)) continue;
        ImGui::PushID(i);
        bool sel = g_pick.cat == c && g_pick.row == i;
        ImGui::BeginChild("r", ImVec2(0, 52), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        image(v.icon, 128, ImVec2(c == CAT_WEAPON ? 90 : 40, 40));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextColored(ImColor(sel ? col::gold : col::text).Value, "%s", text_or(v.name).c_str());
        if (c == CAT_CONSUMABLE) ImGui::TextColored(ImColor(col::dim).Value, "carried %d  -  %d zenny", s.stock[i], v.zenny);
        else if (own) ImGui::TextColored(ImColor(col::dim).Value, "owned");
        else ImGui::TextColored(ImColor(rarity_color(v.tier)).Value, "RARE %d", rarity_of(v.tier));
        ImGui::EndGroup();
        ImGui::EndChild();
        if (ImGui::IsItemClicked()) g_pick = v;
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// ---- the Remnant menu at an MHW NPC

static void screen_menu(const SaveData& s, const Live& l) {
    int hostRow = g_menu >= 0 ? kMenus[g_menu].host : host_of_role(g_menuRole);
    const Npcs& n = kNpcs[hostRow];
    if (!begin_screen("##menu", 0.80f, 0.84f)) return end_screen();
    if (g_npcSays.empty()) g_npcSays = r2_text(greet_of(n, g_greet));
    title(upper(r2_name(n.name)).c_str(), col::gold);
    ImGui::SameLine();
    ImGui::TextColored(ImColor(col::dim).Value, "  at %s", role_place(g_menuRole));
    ImGui::SameLine(ImGui::GetWindowWidth() - 260);
    ImGui::TextColored(ImColor(col::gold).Value, "%d zenny", mhw_zenny());
    quote(g_npcSays);
    if (l.said[0] && strcmp(l.said, "done") != 0) ImGui::TextColored(ImColor(col::red).Value, "%s", l.said);
    ImGui::Separator();
    struct Tab { const char* name; Cat cat; int slot; };
    std::vector<Tab> tabs;
    if (g_menuRole == MENUS_ROLE_SMITHY)
        tabs = {{"LONG GUNS", CAT_WEAPON, WEAPONS_SLOT_LONG}, {"HAND GUNS", CAT_WEAPON, WEAPONS_SLOT_HAND},
                {"MELEE", CAT_WEAPON, WEAPONS_SLOT_MELEE},    {"MODS", CAT_MOD, -1},
                {"MUTATORS", CAT_MUTATOR, -1},                {"ARMOR", CAT_ARMOR, -1},
                {"UPGRADES", CAT_WEAPON, -2}};
    else if (g_menuRole == MENUS_ROLE_MELDER)
        tabs = {{"ARCHETYPES", CAT_ARCHETYPE, -1}, {"RINGS", CAT_RING, -1}, {"AMULETS", CAT_AMULET, -1},
                {"RELICS", CAT_RELIC, -1},         {"FRAGMENTS", CAT_FRAGMENT, -1}, {"TRAITS", CAT_TRAIT, -1},
                {"PRISMS", CAT_PRISM, -1}};
    else
        tabs = {{"CONSUMABLES", CAT_CONSUMABLE, -1}};
    float bottom = 52;
    if (ImGui::BeginTabBar("tabs")) {
        for (const Tab& tb : tabs) {
            if (!ImGui::BeginTabItem(tb.name)) continue;
            bool upgrades = tb.slot == -2;
            float w = ImGui::GetContentRegionAvail().x;
            ImGui::BeginChild("list", ImVec2(w * 0.42f, -bottom));
            item_list(tb.cat, upgrades ? -1 : tb.slot, s, upgrades, upgrades ? -1 : g_menuRole);
            ImGui::EndChild();
            ImGui::SameLine(0, 20);
            ImGui::BeginChild("detail", ImVec2(0, -bottom));
            if (g_pick.row >= 0 && g_pick.cat == tb.cat) {
                const ItemView& v = g_pick;
                item_detail(v, s);
                ImGui::Spacing();
                ImGui::Separator();
                Request r{Request::CRAFT, v.cat, v.row};
                if (upgrades) {
                    int lvl = s.level[v.row], top = weapon_max_level(v.row);
                    if (lvl < top) {
                        int zen = 0;
                        std::string rec = upgrade_cost(v.row, lvl, &zen);
                        ImGui::Text("+%d  ->  +%d of %d  (damage x%.2f -> x%.2f)", lvl, lvl + 1, top, upgrade_mult(v.row, lvl),
                                    upgrade_mult(v.row, lvl + 1));
                        bool ok = recipe_box(rec.c_str(), zen);
                        char b[48];
                        snprintf(b, sizeof(b), "UPGRADE TO +%d", lvl + 1);
                        if (remnant_button(b, ImVec2(-1, 40), ok)) { r.kind = Request::UPGRADE; request(r); g_npcSays = r2_text(n.done_line); ui_sound("upgrade"); }
                    } else {
                        ImGui::TextColored(ImColor(col::gold).Value, "Fully upgraded (+%d)", lvl);
                    }
                } else if (v.cat == CAT_CONSUMABLE) {
                    bool ok = recipe_box("", v.zenny);
                    char b[48];
                    snprintf(b, sizeof(b), "BUY  (%d ZENNY)", v.zenny);
                    if (remnant_button(b, ImVec2(-1, 40), ok)) { r.kind = Request::BUY_CONSUMABLE; request(r); g_npcSays = r2_text(n.done_line); ui_sound("buy"); }
                } else if (!s.has(v.cat, v.row)) {
                    bool free = v.cat == CAT_ARCHETYPE && v.zenny <= 0;
                    bool ok = free || recipe_box(v.recipe, v.zenny);
                    if (remnant_button(free ? "TAKE" : "CRAFT", ImVec2(-1, 40), ok)) {
                        if (free) choose_archetype(v.row, s.archetype < 0 ? 0 : 1);
                        else request(r);
                        g_npcSays = r2_text(n.done_line);
                        ui_sound("craft");
                    }
                } else {
                    ImGui::TextColored(ImColor(col::gold).Value, "Owned. Equip it at the Ward 13 terminal [%c].", (char)keys().terminal);
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 62);
    ImGui::Dummy(ImVec2(1, 1));
    ImGui::SameLine(ImGui::GetWindowWidth() - 300);
    if (remnant_button(r2_text(n.leave_option).c_str(), ImVec2(240, 38))) open(SCR_NONE);
    end_screen();
}

// ---- the Ward 13 terminal: the Remnant loadout

struct Slot { const char* label; Cat cat; int sub; };  // sub: weapon slot, or index into a slot array
static const Slot kSlots[] = {
    {"LONG GUN", CAT_WEAPON, WEAPONS_SLOT_LONG}, {"LONG GUN MOD", CAT_MOD, 0},   {"LONG GUN MUTATOR", CAT_MUTATOR, 0},
    {"HAND GUN", CAT_WEAPON, WEAPONS_SLOT_HAND}, {"HAND GUN MOD", CAT_MOD, 1},   {"HAND GUN MUTATOR", CAT_MUTATOR, 1},
    {"MELEE", CAT_WEAPON, WEAPONS_SLOT_MELEE},   {"MELEE MUTATOR", CAT_MUTATOR, 2}, {"RELIC", CAT_RELIC, 0},
    {"FRAGMENT 1", CAT_FRAGMENT, 0},             {"FRAGMENT 2", CAT_FRAGMENT, 1}, {"FRAGMENT 3", CAT_FRAGMENT, 2},
    {"AMULET", CAT_AMULET, 0},                   {"RING 1", CAT_RING, 0},         {"RING 2", CAT_RING, 1},
    {"RING 3", CAT_RING, 2},                     {"RING 4", CAT_RING, 3},         {"HEAD", CAT_ARMOR, 0},
    {"BODY", CAT_ARMOR, 1},                      {"GLOVES", CAT_ARMOR, 2},        {"LEGS", CAT_ARMOR, 3}};

static int slot_row(const SaveData& s, const Slot& sl) {
    switch (sl.cat) {
        case CAT_WEAPON: return sl.sub == WEAPONS_SLOT_LONG ? s.longGun : sl.sub == WEAPONS_SLOT_HAND ? s.handGun : s.melee;
        case CAT_MOD: {
            int g = sl.sub == 0 ? s.longGun : s.handGun;
            return g >= 0 && kWeapons[g].mod >= 0 ? kWeapons[g].mod : s.gunMod[sl.sub];
        }
        case CAT_MUTATOR: return s.mutator[sl.sub];
        case CAT_RELIC: return s.relic;
        case CAT_FRAGMENT: return s.fragment[sl.sub];
        case CAT_AMULET: return s.amulet;
        case CAT_RING: return s.ring[sl.sub];
        case CAT_ARMOR: return s.armor[sl.sub];
        default: return -1;
    }
}

static void screen_terminal(const SaveData& s, const Live& l) {
    if (!begin_screen("##terminal", 0.84f, 0.86f)) return end_screen();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("left", ImVec2(w * 0.40f, 0));
    image(kUi[UI_WARD13_TERMINAL].art, 1024, ImVec2(ImGui::GetContentRegionAvail().x, 110));
    title("WARD 13");
    ImGui::Separator();
    for (int slot = 0; slot < 2; slot++) {
        int a = slot == 0 ? s.archetype : s.archetype2;
        ImGui::PushID(100 + slot);
        label(slot == 0 ? "ARCHETYPE" : "SECOND ARCHETYPE");
        if (a >= 0) {
            image(kArchetypes[a].emblem, 128, ImVec2(40, 40));
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextUnformatted(text_or(&kArchetypes[a].name).c_str());
            ImGui::SameLine();
            ImGui::TextColored(ImColor(col::gold).Value, "  Lv %d", archetype_level(s, a));
            std::vector<int> sk = skills_of(a);
            if (!sk.empty()) {
                const Skills& k = kSkills[sk[skill_index(s, slot)]];
                ImGui::TextColored(ImColor(col::dim).Value, "[%c] %s", (char)(slot == 0 ? keys().skill : keys().skill2),
                                   text_or(&k.name).c_str());
            }
            ImGui::EndGroup();
        } else {
            ImGui::TextColored(ImColor(col::dim).Value, slot == 0 ? "none" : "none (craft engrams at the Elder Melder)");
        }
        ImGui::PopID();
    }
    if (remnant_button("ARCHETYPES AND SKILLS", ImVec2(-1, 34))) open(SCR_ARCHETYPE);
    level_line(s);
    {
        int free = trait_points(s) - trait_points_spent(s);
        bool ready = s.prismOn >= 0 && s.prism[s.prismOn].offers > 0;
        char tb[48], pb[48];
        snprintf(tb, sizeof(tb), free > 0 ? "TRAITS (%d)" : "TRAITS", free);
        snprintf(pb, sizeof(pb), ready ? "PRISM (LEVEL UP)" : "PRISM");
        float half = (ImGui::GetContentRegionAvail().x - 8) / 2;
        if (remnant_button(tb, ImVec2(half, 30))) open(SCR_TRAITS);
        ImGui::SameLine(0, 8);
        if (remnant_button(pb, ImVec2(half, 30))) open(SCR_PRISM);
    }
    // Remnant II's difficulty for the Remnant monsters met from now on
    label("REMNANT DIFFICULTY");
    {
        float bw = (ImGui::GetContentRegionAvail().x - 3 * 6) / kDifficultiesCount;
        for (int i = 0; i < kDifficultiesCount; i++) {
            ImGui::PushID(400 + i);
            if (i) ImGui::SameLine(0, 6);
            bool on = s.difficulty == i;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImColor(col::accent).Value);
            if (remnant_button(kDifficulties[i].label, ImVec2(bw, 30)) && !on)
                change_save([i](SaveData& d) { d.difficulty = i; return true; });
            if (on) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                const Difficulties& d = kDifficulties[i];
                ImGui::SetTooltip("Remnant monsters: health x%.2f, damage x%.2f, pace x%.2f%s; experience x%.1f", d.hp_mult,
                                  d.dmg_mult, d.tempo, d.enrage_frac > 0 ? ", enraged sooner" : ", never enraged", d.xp_mult);
            }
            ImGui::PopID();
        }
    }
    ImGui::Spacing();
    ImGui::BeginChild("slots", ImVec2(0, -150));
    for (int i = 0; i < (int)(sizeof(kSlots) / sizeof(kSlots[0])); i++) {
        const Slot& sl = kSlots[i];
        int row = slot_row(s, sl);
        ImGui::PushID(i);
        bool sel = g_slotPick == i;
        ImGui::BeginChild("s", ImVec2(0, 44), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        ItemView v = row >= 0 ? item(sl.cat, row) : ItemView();
        image(v.icon, 128, ImVec2(sl.cat == CAT_WEAPON ? 80 : 32, 32));
        ImGui::SameLine();
        ImGui::BeginGroup();
        label(sl.label, sel ? col::gold : col::dim);
        if (row >= 0) {
            bool fixed = sl.cat == CAT_MOD && kWeapons[sl.sub == 0 ? std::max(0, s.longGun) : std::max(0, s.handGun)].mod >= 0;
            ImGui::Text("%s%s", text_or(v.name).c_str(), fixed ? "  (built in)" : "");
            if (sl.cat == CAT_WEAPON && s.level[row]) { ImGui::SameLine(); ImGui::TextColored(ImColor(col::gold).Value, "+%d", s.level[row]); }
        } else {
            ImGui::TextColored(ImColor(col::dim).Value, "empty");
        }
        ImGui::EndGroup();
        ImGui::EndChild();
        if (ImGui::IsItemClicked()) { g_slotPick = i; g_pick = ItemView(); g_filter[0] = 0; }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::TextColored(ImColor(col::gold).Value, "Relic charges %d   -   %d zenny", s.relicCharges, mhw_zenny());
    for (int role = 0; role < 3; role++) {
        if (role_has_spot(role)) continue;  // visit them in person once their spot is known
        ImGui::PushID(200 + role);
        std::string b = "VISIT " + upper(r2_name(kNpcs[host_of_role(role)].name));
        if (remnant_button(b.c_str(), ImVec2(-1, 30))) open_menu(-1, role);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 24);
    ImGui::BeginChild("right", ImVec2(0, 0));
    if (g_slotPick >= 0) {
        const Slot& sl = kSlots[g_slotPick];
        title(sl.label);
        float rw = ImGui::GetContentRegionAvail().x;
        ImGui::BeginChild("choices", ImVec2(rw * 0.45f, -50));
        item_list(sl.cat, sl.cat == CAT_WEAPON ? sl.sub : -1, s, true, -1);
        ImGui::EndChild();
        ImGui::SameLine(0, 16);
        ImGui::BeginChild("pick", ImVec2(0, -50));
        if (g_pick.row >= 0 && g_pick.cat == sl.cat) {
            item_detail(g_pick, s);
            bool okHere = can_equip_here(l);
            if (remnant_button("EQUIP", ImVec2(-1, 38), okHere)) {
                equip(sl.cat, g_pick.row, sl.sub);
                ui_sound(sl.cat == CAT_FRAGMENT ? "socket" : "equip");
            }
        }
        ImGui::EndChild();
        if (slot_row(s, sl) >= 0 && sl.cat != CAT_WEAPON && remnant_button("REMOVE", ImVec2(200, 36))) equip(sl.cat, -1, sl.sub);
    } else {
        title("CONSUMABLES");
        bool any = false;
        for (int i = 0; i < kConsumablesCount; i++) {
            if (s.stock[i] <= 0) continue;
            any = true;
            ImGui::PushID(i);
            image(kConsumables[i].icon, 128, ImVec2(36, 36));
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::Text("%s  x%d%s", text_or(&kConsumables[i].name).c_str(), s.stock[i], s.concoction == i ? "  (active)" : "");
            effect_lines(kConsumables[i].effects);
            ImGui::EndGroup();
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 100);
            if (remnant_button("USE", ImVec2(90, 30))) use_consumable(i);
            ImGui::PopID();
        }
        if (!any) ImGui::TextColored(ImColor(col::dim).Value, "None carried. %s sells them at %s.",
                                     r2_name(kNpcs[host_of_role(MENUS_ROLE_PROVISIONS)].name).c_str(), role_place(MENUS_ROLE_PROVISIONS));
        ImGui::Spacing();
        ImGui::TextColored(ImColor(col::dim).Value, "Pick a slot on the left to change it.");
    }
    ImGui::EndChild();
    end_screen();
}

// ---- traits and prisms (progress.cpp)

static int g_traitPick = -1;

static void screen_traits(const SaveData& s) {
    if (!begin_screen("##traits", 0.80f, 0.86f)) return end_screen();
    title("TRAITS");
    level_line(s);
    int earned = trait_points(s), spent = trait_points_spent(s);
    ImGui::TextColored(ImColor(spent < earned ? col::gold : col::dim).Value, "%d trait point%s to spend   (%d of %d used)",
                       earned - spent, earned - spent == 1 ? "" : "s", spent, earned);
    dim_wrapped("As in Remnant II: your primary archetype puts 5 points into the core traits, each equipped archetype "
                "raises its own trait by one per archetype level, and every Remnant level earns a trait point to spend "
                "(85 at most). A trait goes up to 10. Your own points can be taken back here at any time. Learn more "
                "traits from the Elder Melder or from Remnant bosses.");
    ImGui::Separator();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("list", ImVec2(w * 0.52f, -46));
    static const char* kPass[4] = {"CORE TRAITS", "ARCHETYPE TRAITS", "TRAITS", "NOT LEARNED YET"};
    for (int pass = 0; pass < 4; pass++) {  // core, archetype, learned, not yet learned
        bool header = false;
        for (int i = 0; i < kTraitsCount; i++) {
            const Traits& t = kTraits[i];
            bool known = trait_known(s, i);
            int want = t.kind == TRAITS_KIND_CORE ? 0 : t.kind == TRAITS_KIND_ARCHETYPE ? (known ? 1 : -1) : known ? 2 : 3;
            if (want != pass) continue;
            if (!header) { header = true; label(kPass[pass]); }
            ImGui::PushID(i);
            bool sel = g_traitPick == i;
            ImGui::BeginChild("t", ImVec2(0, 34), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
            int lv = trait_level(s, i), au = std::min(10, trait_auto(s, i)), mine = i < (int)s.trait.size() ? s.trait[i] : 0;
            ImU32 c = !known ? col::dim : sel ? col::gold : col::text;
            ImGui::TextColored(ImColor(c).Value, "%s", text_or(&t.name).c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.42f);
            if (known) {
                pips(lv, 10, au);
                bool spendable = t.kind != TRAITS_KIND_ARCHETYPE || au < 10;
                if (spendable) {
                    ImGui::SameLine();
                    if (remnant_button("-", ImVec2(26, 22), mine > 0)) change_save([i](SaveData& d) { return trait_set(d, i, -1); });
                    ImGui::SameLine();
                    if (remnant_button("+", ImVec2(26, 22), lv < 10 && spent < earned))
                        change_save([i](SaveData& d) { return trait_set(d, i, 1); });
                }
            } else {
                ImGui::TextColored(ImColor(col::dim).Value, "not learned yet");
            }
            ImGui::EndChild();
            if (ImGui::IsItemClicked()) g_traitPick = i;
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 20);
    ImGui::BeginChild("detail", ImVec2(0, -46));
    if (s.archetype >= 0 && kArchetypes[s.archetype].trait >= 0) {
        const Traits& at = kTraits[kArchetypes[s.archetype].trait];
        label("ARCHETYPE TRAIT");
        ImGui::TextColored(ImColor(col::gold).Value, "%s", text_or(&at.name).c_str());
        dim_wrapped(("From your primary archetype, " + text_or(&kArchetypes[s.archetype].name) + ": one point per archetype level, "
                     "free. Your second archetype's trait works the same way. Once an archetype reaches level 10, its "
                     "trait can be used with any archetype for trait points.").c_str());
        effect_lines(at.effects);
        ImGui::Separator();
    }
    if (g_traitPick >= 0 && g_traitPick < kTraitsCount) {
        const Traits& t = kTraits[g_traitPick];
        title(upper(text_or(&t.name)).c_str());
        if (t.desc.asset) dim_wrapped(text_or(&t.desc).c_str());
        label("AT LEVEL 10");
        effect_lines(t.effects);
        int lv = trait_level(s, g_traitPick), au = std::min(10, trait_auto(s, g_traitPick));
        ImGui::TextColored(ImColor(col::dim).Value, "Now level %d: %d%% of that%s.", lv, lv * 10,
                           au > 0 ? (std::string(" (") + std::to_string(au) + " from your archetypes)").c_str() : "");
        if (!trait_known(s, g_traitPick))
            dim_wrapped(("Learn it from " + r2_name(kNpcs[host_of_role(MENUS_ROLE_MELDER)].name) + " (" +
                         role_place(MENUS_ROLE_MELDER) + "), or from a Remnant boss.").c_str());
    } else {
        dim_wrapped("Pick a trait to see what it does.");
    }
    ImGui::EndChild();
    if (remnant_button("TAKE ALL POINTS BACK", ImVec2(260, 36), spent > 0)) change_save([](SaveData& d) { traits_reset(d); return true; });
    ImGui::SameLine(ImGui::GetWindowWidth() - 250);
    if (remnant_button("BACK", ImVec2(220, 36))) open(SCR_TERMINAL);
    end_screen();
}

static int g_prismPick = -1;
static double g_cleanseArmed = 0;

static void slot_line(const SaveData::PSlot& sl, int i) {
    if (sl.kind == PS_FRAGMENT) {
        ImGui::TextColored(ImColor(col::text).Value, "%d.  %s", i + 1, text_or(&kFragments[sl.row].name).c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImColor(col::gold).Value, "+%d", sl.lv);
        ImGui::Indent();
        effect_lines(kFragments[sl.row].effects);
        ImGui::Unindent();
    } else if (sl.kind == PS_FUSION) {
        const Fusions& f = kFusions[sl.row];
        ImGui::TextColored(ImColor(col::gold).Value, "%d.  %s", i + 1, text_or(&f.name).c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImColor(col::dim).Value, "(%s + %s)", text_or(&kFragments[f.a].name).c_str(), text_or(&kFragments[f.b].name).c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImColor(col::gold).Value, "+%d", sl.lv);
        ImGui::Indent();
        effect_lines(kFragments[f.a].effects);
        effect_lines(kFragments[f.b].effects);
        ImGui::Unindent();
    } else {
        ImGui::TextColored(ImColor(col::dim).Value, "%d.  empty", i + 1);
    }
}

static void offer_card(const SaveData::Prism& p, const SaveData::PSlot& o) {
    switch (o.kind) {
        case PS_FRAGMENT:
            label("NEW FRAGMENT", col::gold);
            ImGui::TextWrapped("%s  +1", text_or(&kFragments[o.row].name).c_str());
            effect_lines(kFragments[o.row].effects);
            break;
        case PS_RAISE: {
            const SaveData::PSlot& sl = p.slot[o.row];
            label("STRENGTHEN", col::gold);
            std::string n = sl.kind == PS_FUSION ? text_or(&kFusions[sl.row].name) : text_or(&kFragments[sl.row].name);
            ImGui::TextWrapped("%s  +%d -> +%d", n.c_str(), sl.lv, sl.lv + 1);
            break;
        }
        case PS_FUSION: {
            const Fusions& f = kFusions[o.row];
            label("FUSION", col::gold);
            ImGui::TextWrapped("%s: %s and %s become one fragment (+1) with both", text_or(&f.name).c_str(),
                               text_or(&kFragments[f.a].name).c_str(), text_or(&kFragments[f.b].name).c_str());
            break;
        }
        case PS_LEGENDARY:
            label("LEGENDARY", col::gold);
            ImGui::TextWrapped("%s", text_or(&kLegendaries[o.row].name).c_str());
            if (kLegendaries[o.row].desc.asset) dim_wrapped(text_or(&kLegendaries[o.row].desc).c_str());
            effect_lines(kLegendaries[o.row].effects);
            break;
        default: break;
    }
}

static void screen_prism(const SaveData& s) {
    if (!begin_screen("##prism", 0.82f, 0.86f)) return end_screen();
    title("PRISMS");
    dim_wrapped("The equipped prism gains experience as you hunt. Each prism level offers three choices: a new relic "
                "fragment for one of its five slots, one more level for a fragment (up to +10), or a fusion of two "
                "fragments that are both +5 or more. With all five at +10 it offers a legendary bonus. Fragments show "
                "their +10 values.");
    ImGui::Separator();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("owned", ImVec2(w * 0.28f, -46));
    bool any = false;
    for (int i = 0; i < kPrismsCount; i++) {
        if (!s.has(CAT_PRISM, i)) continue;
        any = true;
        if (g_prismPick < 0 || !s.has(CAT_PRISM, g_prismPick)) g_prismPick = s.prismOn >= 0 ? s.prismOn : i;
        ImGui::PushID(i);
        ImGui::BeginChild("p", ImVec2(0, 52), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        ImGui::TextColored(ImColor(g_prismPick == i ? col::gold : col::text).Value, "%s", text_or(&kPrisms[i].name).c_str());
        const SaveData::Prism& p = s.prism[i];
        ImGui::TextColored(ImColor(col::dim).Value, "+%d%s%s", prism_level(p), s.prismOn == i ? "   equipped" : "",
                           p.offers > 0 ? "   level up!" : "");
        ImGui::EndChild();
        if (ImGui::IsItemClicked()) g_prismPick = i;
        ImGui::PopID();
    }
    if (!any)
        dim_wrapped(("No prism yet. " + r2_name(kNpcs[host_of_role(MENUS_ROLE_MELDER)].name) +
                     " crafts them, and the first Remnant boss you defeat leaves one.").c_str());
    ImGui::EndChild();
    ImGui::SameLine(0, 20);
    ImGui::BeginChild("detail", ImVec2(0, -46));
    if (any && g_prismPick >= 0) {
        int row = g_prismPick;
        const SaveData::Prism& p = s.prism[row];
        int lv = prism_level(p);
        double need = prism_need(p);
        title(upper(text_or(&kPrisms[row].name)).c_str());
        ImGui::TextColored(ImColor(col::gold).Value, "+%d of 51", lv);
        if (need > 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImColor(col::dim).Value, "   %.0f / %.0f experience", std::min(p.xp, need), need);
            bar((float)(p.xp / need), col::accent);
        }
        if (s.prismOn != row) {
            if (remnant_button("EQUIP THIS PRISM", ImVec2(260, 34)))
                change_save([row](SaveData& d) { d.prismOn = row; prism_offer(d.prism[row]); return true; });
        } else if (need > 0 && p.offers == 0) {
            dim_wrapped("Equipped: it gains experience as you hunt.");
        }
        if (p.offers > 0) {
            ImGui::Spacing();
            label("LEVEL UP: CHOOSE ONE", col::gold);
            float cw = (ImGui::GetContentRegionAvail().x - 24) / 3.0f;
            for (int k = 0; k < p.offers; k++) {
                ImGui::PushID(300 + k);
                if (k) ImGui::SameLine(0, 12);
                ImGui::BeginChild("o", ImVec2(cw, 170), ImGuiChildFlags_Borders);
                offer_card(p, p.offer[k]);
                ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 40);
                if (remnant_button("CHOOSE", ImVec2(-1, 30))) {
                    change_save([row, k](SaveData& d) { return prism_pick(d, row, k); });
                    ui_sound("socket");
                }
                ImGui::EndChild();
                ImGui::PopID();
            }
        }
        ImGui::Spacing();
        label("FRAGMENTS");
        for (int i = 0; i < 5; i++) slot_line(p.slot[i], i);
        if (p.legendary >= 0) {
            label("LEGENDARY", col::gold);
            ImGui::TextUnformatted(text_or(&kLegendaries[p.legendary].name).c_str());
            if (kLegendaries[p.legendary].desc.asset) dim_wrapped(text_or(&kLegendaries[p.legendary].desc).c_str());
            effect_lines(kLegendaries[p.legendary].effects);
        }
    }
    ImGui::EndChild();
    if (any && g_prismPick >= 0) {
        bool armed = now_s() - g_cleanseArmed < 4.0;
        int row = g_prismPick;
        if (remnant_button(armed ? "CLICK AGAIN TO CLEANSE" : "CLEANSE (from +10)", ImVec2(300, 36), prism_level(s.prism[row]) >= 10)) {
            if (armed) {
                change_save([row](SaveData& d) { return prism_cleanse(d, row); });
                g_cleanseArmed = 0;
            } else {
                g_cleanseArmed = now_s();
            }
        }
        ImGui::SameLine();
    }
    ImGui::SameLine(ImGui::GetWindowWidth() - 250);
    if (remnant_button("BACK", ImVec2(220, 36))) open(SCR_TERMINAL);
    end_screen();
}

// ---- archetypes

static void screen_archetype(const SaveData& s) {
    if (!begin_screen("##archetype", 0.84f, 0.86f)) return end_screen();
    title(s.archetype < 0 ? "CHOOSE YOUR ARCHETYPE" : "ARCHETYPES");
    label("Your archetype brings its skills, perks and trait. Starting archetypes are free; the others are engrams the "
          "Elder Melder's Remnant guest crafts from MHW materials. A second archetype adds its perks and a second skill.");
    ImGui::Separator();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("list", ImVec2(w * 0.30f, -50));
    for (int i = 0; i < kArchetypesCount; i++) {
        const Archetypes& a = kArchetypes[i];
        bool own = s.has(CAT_ARCHETYPE, i) || a.zenny <= 0;
        if (s.archetype < 0 && a.zenny > 0) continue;  // first choice: the starting four
        ImGui::PushID(i);
        bool sel = g_pick.cat == CAT_ARCHETYPE && g_pick.row == i;
        ImGui::BeginChild("a", ImVec2(0, 58), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        image(a.emblem, 128, ImVec2(46, 46), own ? IM_COL32_WHITE : IM_COL32(120, 115, 108, 255));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextColored(ImColor(sel ? col::gold : own ? col::text : col::dim).Value, "%s", upper(text_or(&a.name)).c_str());
        const char* st = i == s.archetype ? "primary" : i == s.archetype2 ? "second" : own ? "available" : "craft at the Elder Melder";
        ImGui::TextColored(ImColor(col::dim).Value, "%s", st);
        ImGui::EndGroup();
        ImGui::EndChild();
        if (ImGui::IsItemClicked()) g_pick = item(CAT_ARCHETYPE, i);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 20);
    ImGui::BeginChild("detail", ImVec2(0, -50));
    if (g_pick.cat == CAT_ARCHETYPE && g_pick.row >= 0) {
        const Archetypes& a = kArchetypes[g_pick.row];
        image(a.emblem, 256, ImVec2(110, 110));
        ImGui::SameLine();
        ImGui::BeginGroup();
        title(upper(text_or(&a.name)).c_str());
        ImGui::TextWrapped("%s", text_or(&a.desc).c_str());
        ImGui::EndGroup();
        archetype_detail(g_pick.row, s, true);
    }
    ImGui::EndChild();
    if (g_pick.cat == CAT_ARCHETYPE && g_pick.row >= 0) {
        const Archetypes& a = kArchetypes[g_pick.row];
        bool own = s.has(CAT_ARCHETYPE, g_pick.row) || a.zenny <= 0;
        if (remnant_button(s.archetype < 0 ? "CHOOSE" : "MAKE PRIMARY", ImVec2(220, 40), own && g_pick.row != s.archetype)) {
            bool first = s.archetype < 0;
            choose_archetype(g_pick.row, 0);
            if (first) open(SCR_NONE);
        }
        if (s.archetype >= 0) {
            ImGui::SameLine();
            if (remnant_button("MAKE SECOND", ImVec2(220, 40), own && g_pick.row != s.archetype && g_pick.row != s.archetype2))
                choose_archetype(g_pick.row, 1);
        }
    }
    if (s.archetype >= 0) {
        ImGui::SameLine(ImGui::GetWindowWidth() - 280);
        if (remnant_button("BACK", ImVec2(220, 40))) open(SCR_TERMINAL);
    }
    end_screen();
}

// ---------------------------------------------------------------- HUD (MHW colours)

// World (cm) -> screen, with MHW's row-vector view and projection matrices.
static bool project(const GameView& v, const Vec3& p, ImVec2 vpPos, ImVec2 vpSize, ImVec2* out) {
    float in[4] = {p.x, p.y, p.z, 1}, a[4], c[4];
    for (int j = 0; j < 4; j++) a[j] = in[0] * v.view[0 * 4 + j] + in[1] * v.view[1 * 4 + j] + in[2] * v.view[2 * 4 + j] + in[3] * v.view[3 * 4 + j];
    for (int j = 0; j < 4; j++) c[j] = a[0] * v.proj[0 * 4 + j] + a[1] * v.proj[1 * 4 + j] + a[2] * v.proj[2 * 4 + j] + a[3] * v.proj[3 * 4 + j];
    if (c[3] <= 1.0f) return false;
    out->x = vpPos.x + (c[0] / c[3] * 0.5f + 0.5f) * vpSize.x;
    out->y = vpPos.y + (0.5f - c[1] / c[3] * 0.5f) * vpSize.y;
    return true;
}

static void hud(const SaveData& s, const Live& l) {
    if (!archetype() || !l.snap.inGame) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    double t = now_s();
    float sc = vp->Size.y / 1080.0f;
    // Toast.
    if (t < l.toastUntil && l.toast[0]) {
        ImVec2 ts = ImGui::CalcTextSize(l.toast);
        ImVec2 p(vp->Pos.x + (vp->Size.x - ts.x) / 2, vp->Pos.y + vp->Size.y * 0.16f);
        fg->AddRectFilled(ImVec2(p.x - 18, p.y - 8), ImVec2(p.x + ts.x + 18, p.y + ts.y + 8), col::mhwFrame);
        fg->AddRect(ImVec2(p.x - 18, p.y - 8), ImVec2(p.x + ts.x + 18, p.y + ts.y + 8), col::mhwGold);
        fg->AddText(p, col::mhwText, l.toast);
    }
    // At an MHW NPC with a Remnant menu: a Remnant-styled prompt.
    if (l.nearMenu >= 0 && g_screen == SCR_NONE) {
        const Menus& m = kMenus[l.nearMenu];
        char b[160];
        snprintf(b, sizeof(b), "[%c]  %s's Remnant wares", (char)keys().terminal, r2_name(kNpcs[m.host].name).c_str());
        ImVec2 ts = g_bold->CalcTextSizeA(20 * sc, FLT_MAX, 0, b);
        ImVec2 p(vp->Pos.x + (vp->Size.x - ts.x) / 2, vp->Pos.y + vp->Size.y * 0.70f);
        fg->AddRectFilled(ImVec2(p.x - 20, p.y - 10), ImVec2(p.x + ts.x + 20, p.y + ts.y + 10), col::bg);
        fg->AddRectFilled(ImVec2(p.x - 20, p.y - 10), ImVec2(p.x - 16, p.y + ts.y + 10), col::accent);
        fg->AddText(g_bold, 20 * sc, p, col::text, b);
    }
    if (!l.combatAllowed) return;
    ImVec2 c(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y / 2);
    int gun = equipped_gun(s, l);
    if (l.gunMode) {
        float spread = gun >= 0 ? kWeapons[gun].spread_deg * (l.aiming ? tune(TUNING_ADS_SPREAD_MULT) : 1.0f) : 1;
        float r = (6 + spread * 6) * sc;
        fg->AddCircle(c, r, IM_COL32(255, 255, 255, 170), 32, 1.5f);
        fg->AddCircleFilled(c, 2.0f * sc, IM_COL32(255, 255, 255, 230));
        ImVec2 muzzle(c.x + vp->Size.x * 0.11f, c.y + vp->Size.y * 0.17f);
        for (const Live::Tracer& tr : l.tracers) {
            if (t >= tr.until || !l.snap.view.valid) continue;
            ImVec2 to;
            if (!project(l.snap.view, tr.to, vp->Pos, vp->Size, &to)) continue;
            float k = (float)((tr.until - t) / 0.09);
            fg->AddLine(muzzle, to, IM_COL32(255, 214, 150, (int)(200 * k)), 2.0f * sc);
            if (tr.hit) fg->AddCircleFilled(to, 5 * sc * (1.5f - k), IM_COL32(255, 190, 110, (int)(230 * k)));
        }
        if (t < l.muzzleUntil) {
            fg->AddCircleFilled(muzzle, 14 * sc, IM_COL32(255, 220, 160, 200));
            fg->AddCircleFilled(muzzle, 7 * sc, IM_COL32(255, 255, 235, 255));
        }
        if (t < l.hitMarkerUntil) {
            ImU32 hc = l.hitMarkerWeak ? col::red : IM_COL32(255, 255, 255, 240);
            float a1 = 7 * sc, a2 = 14 * sc;
            for (int i = 0; i < 4; i++) {
                float sx = (i & 1) ? 1.f : -1.f, sy = (i & 2) ? 1.f : -1.f;
                fg->AddLine(ImVec2(c.x + sx * a1, c.y + sy * a1), ImVec2(c.x + sx * a2, c.y + sy * a2), hc, 2.5f);
            }
        }
    }
    // Gun panel, bottom centre, clear of MHW's item bar and slinger (bottom right) until the guns live in the bar.
    ImVec2 size(360 * sc, 132 * sc);
    ImVec2 p(vp->Pos.x + (vp->Size.x - size.x) / 2, vp->Pos.y + vp->Size.y - size.y - 36 * sc);
    fg->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), col::mhwFrame);
    fg->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), col::mhwGold, 0, 0, 1.5f);
    if (!l.gunMode) {
        char hint[64];
        snprintf(hint, sizeof(hint), "[%c] Remnant guns", (char)keys().gun);
        fg->AddText(ImVec2(p.x + 14 * sc, p.y + 12 * sc), col::mhwText, hint);
    }
    if (gun >= 0) {
        int w = 0, h = 0;
        void* tex = texture_for(kWeapons[gun].icon, 256, &w, &h);
        if (tex && w) {
            float k = std::min(150 * sc / w, 60 * sc / h);
            ImVec2 a0(p.x + 12 * sc, p.y + 40 * sc);
            fg->AddImage((ImTextureID)(uintptr_t)tex, a0, ImVec2(a0.x + w * k, a0.y + h * k));
        }
        fg->AddText(g_bold, 16 * sc, ImVec2(p.x + 14 * sc, p.y + size.y - 26 * sc), col::mhwText, r2_text(kWeapons[gun].name).c_str());
        char ammo[32];
        snprintf(ammo, sizeof(ammo), "%d", l.mag[gun]);
        fg->AddText(g_title, 44 * sc, ImVec2(p.x + 200 * sc, p.y + 30 * sc), col::mhwText, ammo);
        snprintf(ammo, sizeof(ammo), "/ %d", l.reserve[gun]);
        fg->AddText(g_body, 20 * sc, ImVec2(p.x + 270 * sc, p.y + 52 * sc), col::mhwGold, ammo);
        if (l.reloadDone > 0) {
            float k = (float)std::min(1.0, std::max(0.0, 1.0 - (l.reloadDone - t) / std::max(0.1f, kWeapons[gun].reload_s)));
            ImVec2 b0(p.x + 200 * sc, p.y + 96 * sc);
            fg->AddRectFilled(b0, ImVec2(b0.x + 140 * sc * k, b0.y + 5 * sc), col::mhwGold);
            fg->AddText(ImVec2(b0.x, b0.y - 20 * sc), col::mhwText, "Reloading");
        }
    }
    // Mod meter.
    int mod = gun_mod(s, gun);
    if (mod >= 0) {
        ImVec2 m0(p.x + 200 * sc, p.y + size.y - 22 * sc);
        float k = t < l.modUntil ? 1.0f : std::min(1.0f, l.modCharge / std::max(1.0f, kMods[mod].charge_damage));
        fg->AddRectFilled(m0, ImVec2(m0.x + 140 * sc, m0.y + 8 * sc), IM_COL32(60, 52, 40, 255));
        fg->AddRectFilled(m0, ImVec2(m0.x + 140 * sc * k, m0.y + 8 * sc), k >= 1 ? col::accent : col::mhwGold);
        int w = 0, h = 0;
        if (void* tex = texture_for(kMods[mod].icon, 128, &w, &h))
            fg->AddImage((ImTextureID)(uintptr_t)tex, ImVec2(m0.x - 30 * sc, m0.y - 11 * sc), ImVec2(m0.x - 4 * sc, m0.y + 15 * sc));
    }
    // Skills and relic, bottom left above MHW's own panels.
    ImVec2 q(vp->Pos.x + 40 * sc, vp->Pos.y + vp->Size.y - 300 * sc);
    auto icon_box = [&](const char* asset, ImVec2 at, float frac, const char* tag) {
        ImVec2 e(at.x + 64 * sc, at.y + 64 * sc);
        fg->AddRectFilled(at, e, col::mhwFrame);
        int w = 0, h = 0;
        if (void* tex = texture_for(asset, 128, &w, &h)) fg->AddImage((ImTextureID)(uintptr_t)tex, at, e);
        if (frac < 1) fg->AddRectFilled(ImVec2(at.x, at.y + 64 * sc * frac), e, IM_COL32(0, 0, 0, 160));
        fg->AddRect(at, e, col::mhwGold, 0, 0, 1.5f);
        if (tag) fg->AddText(g_bold, 16 * sc, ImVec2(at.x + 4 * sc, e.y + 2 * sc), col::mhwText, tag);
    };
    char tag[48];
    float x = q.x;
    for (int slot = 0; slot < 2; slot++) {
        int ar = slot == 0 ? s.archetype : s.archetype2;
        std::vector<int> sk = skills_of(ar);
        if (sk.empty()) continue;
        const Skills& k = kSkills[sk[skill_index(s, slot)]];
        int key = slot == 0 ? keys().skill : keys().skill2;
        float cd = std::max(1.0f, k.cooldown_s);
        float sf = t >= l.skillReady[slot] ? 1.0f : (float)std::max(0.0, 1.0 - (l.skillReady[slot] - t) / cd);
        if (t < l.skillUntil[slot]) snprintf(tag, sizeof(tag), "[%c] %.0fs", (char)key, l.skillUntil[slot] - t);
        else if (t < l.skillReady[slot]) snprintf(tag, sizeof(tag), "%.0fs", l.skillReady[slot] - t);
        else snprintf(tag, sizeof(tag), "[%c]", (char)key);
        icon_box(k.icon, ImVec2(x, q.y), sf, tag);
        x += 80 * sc;
    }
    if (s.relic >= 0) {
        snprintf(tag, sizeof(tag), "[%c] x%d", (char)keys().relic, s.relicCharges);
        icon_box(kRelics[s.relic].icon, ImVec2(x, q.y), s.relicCharges > 0 ? 1.0f : 0.0f, tag);
    }
    if (l.bonusMax > 0 || l.bonusHealth > 0) {
        float mx = std::max(l.bonusMax, l.bonusHealth);
        ImVec2 b0(q.x, q.y - 16 * sc);
        fg->AddRectFilled(b0, ImVec2(b0.x + 224 * sc, b0.y + 6 * sc), IM_COL32(60, 52, 40, 255));
        fg->AddRectFilled(b0, ImVec2(b0.x + 224 * sc * (l.bonusHealth / std::max(1.0f, mx)), b0.y + 6 * sc), IM_COL32(120, 190, 210, 255));
    }
}

// ---------------------------------------------------------------- per frame

void ui_draw() {}

void mod_frame() {
    Shared& sh = S();
    AcquireSRWLockShared(&sh.lock);
    SaveData s = sh.save;
    bool inGame = sh.live.snap.inGame, inBase = sh.live.snap.inBase, combat = sh.live.combatAllowed;
    Vec3 pos = sh.live.snap.pos;
    bool viewValid = sh.live.snap.view.valid;
    bool gunMode = sh.live.gunMode;
    int nearMenu = sh.live.nearMenu;
    ReleaseSRWLockShared(&sh.lock);

    const Keys& k = keys();
    // First visit: wait until the hunter is really in town (zone settled and the hunter has moved), not while the
    // loading screen still shows.
    static double baseSince = 0;
    static Vec3 basePos{};
    static bool moved = false;
    if (inGame && inBase) {
        if (baseSince == 0) { baseSince = now_s(); basePos = pos; moved = false; }
        float dx = pos.x - basePos.x, dz = pos.z - basePos.z;
        if (dx * dx + dz * dz > 50.0f * 50.0f) moved = true;
    } else {
        baseSince = 0;
    }
    bool settled = baseSince > 0 && now_s() - baseSince > 4.0 && viewValid && (moved || now_s() - baseSince > 90.0);
    if (g_screen == SCR_NONE && settled && !s.introDone && r2_ready()) open(SCR_INTRO);
    if (g_screen != SCR_NONE) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && g_screen != SCR_INTRO && (g_screen != SCR_ARCHETYPE || s.archetype >= 0))
            open(g_screen == SCR_ARCHETYPE || g_screen == SCR_TRAITS || g_screen == SCR_PRISM ? SCR_TERMINAL : SCR_NONE);
        if (!inGame) open(SCR_NONE);
    } else if (inGame) {
        if (key_pressed(k.terminal) && inBase && s.archetype >= 0) {
            if (nearMenu >= 0) open_menu(nearMenu, kMenus[nearMenu].role);
            else open(SCR_TERMINAL);
        }
        if (key_pressed(k.gun) && combat) {
            AcquireSRWLockExclusive(&sh.lock);
            sh.live.gunMode = !sh.live.gunMode;
            gunMode = sh.live.gunMode;
            ReleaseSRWLockExclusive(&sh.lock);
        }
        if (key_pressed(k.skill)) post(CMD_SKILL);
        if (key_pressed(k.skill2)) post(CMD_SKILL2);
        if (key_pressed(k.relic)) post(CMD_RELIC);
        if (key_pressed(k.mod)) post(CMD_MOD);
        if (gunMode && key_pressed(k.reload)) post(CMD_RELOAD);
        if (gunMode && key_pressed(k.swap)) post(CMD_SWAP);
    }

    InputBlock b;
    if (g_screen != SCR_NONE) {
        b.keyboard = b.mouse = b.mouseButtons = true;
    } else if (inGame && s.archetype >= 0) {
        b.keys[k.skill] = b.keys[k.relic] = b.keys[k.mod] = 1;
        if (s.archetype2 >= 0) b.keys[k.skill2] = 1;
        if (combat) b.keys[k.gun] = 1;
        if (inBase) b.keys[k.terminal] = 1;
        if (gunMode) {
            b.mouseButtons = true;
            b.keys[k.reload] = b.keys[k.swap] = 1;
        }
    }
    set_input_block(b);
    ImGui::GetIO().MouseDrawCursor = g_screen != SCR_NONE;

    if (g_screen == SCR_NONE) {
        AcquireSRWLockShared(&sh.lock);
        Live l = sh.live;
        ReleaseSRWLockShared(&sh.lock);
        hud(s, l);
        bosses_hud();
        return;
    }
    AcquireSRWLockShared(&sh.lock);
    Live l = sh.live;
    ReleaseSRWLockShared(&sh.lock);
    switch (g_screen) {
        case SCR_INTRO: screen_intro(); break;
        case SCR_ARCHETYPE: screen_archetype(s); break;
        case SCR_TERMINAL: screen_terminal(s, l); break;
        case SCR_MENU: screen_menu(s, l); break;
        case SCR_TRAITS: screen_traits(s); break;
        case SCR_PRISM: screen_prism(s); break;
        default: break;
    }
}

}  // namespace rh

#ifdef RH_PREVIEW
// Dev preview (tests/preview_ui.cpp): one screen drawn with a sample save, outside the game.
namespace rh {
void ui_preview(int which, const SaveData& s, const Live& l) {
    switch (which) {
        case 0: screen_terminal(s, l); break;
        case 1: screen_traits(s); break;
        case 2: screen_prism(s); break;
        case 3: g_pick = item(CAT_ARCHETYPE, s.archetype); screen_archetype(s); break;
        default: g_menuRole = MENUS_ROLE_MELDER; g_menu = -1; screen_menu(s, l); break;
    }
}
}  // namespace rh
#endif
