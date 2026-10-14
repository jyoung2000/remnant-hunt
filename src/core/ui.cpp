// Remnant x MHW mashup core: screens and HUD. GPL-3.0-or-later (see LICENSE).
//
// The screens for Remnant tasks: the worldstone intro, archetypes, the Ward 13 terminal (the Remnant loadout), traits,
// prisms and the Remnant menus at MHW's own NPCs (crafting Remnant gear from MHW materials), all drawn in Monster
// Hunter: World's menu style: dark warm panels framed in gold, an amber cursor, cream text and MHW's rarity colours;
// a small HUD in the same colours for the guns, skills and relic. Names, descriptions, lines and icons are read from the player's own
// Remnant II install; effect lines are our own wording; MHW's own UI is left as it is.
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "bosses.h"
#include "state.h"

namespace rh {

// ---------------------------------------------------------------- look

// Monster Hunter: World's menu colours.
namespace col {
constexpr ImU32 bg = IM_COL32(0, 0, 0, 170), panel = IM_COL32(0, 0, 0, 200), line = IM_COL32(96, 138, 162, 220);
constexpr ImU32 text = IM_COL32(240, 232, 211, 255), dim = IM_COL32(163, 152, 130, 255);
constexpr ImU32 accent = IM_COL32(232, 168, 58, 255), gold = IM_COL32(216, 186, 118, 255);
constexpr ImU32 frameHi = IM_COL32(158, 208, 236, 255), frameLo = IM_COL32(78, 118, 142, 200);  // World's pale-blue outlines
constexpr ImU32 mhwFrame = IM_COL32(0, 0, 0, 175), mhwGold = IM_COL32(158, 208, 236, 235);  // HUD plate and its outline
constexpr ImU32 mhwText = IM_COL32(242, 232, 207, 255), red = IM_COL32(214, 58, 46, 255);
constexpr ImU32 plate = IM_COL32(0, 0, 0, 175);  // a panel lets the game show through
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

static void title_text(const char* s, ImU32 c = col::text) {
    ImGui::PushFont(g_title, 24.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImColor(c).Value);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
// Every title carries its own rule, so no screen stacks two separators.
static void title(const char* s, ImU32 c = col::text) {
    title_text(s, c);
    ImGui::Separator();
}

// A small gold diamond, MHW's list and header mark.
static void diamond(ImDrawList* dl, ImVec2 c, float r, ImU32 fill) {
    dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y), fill);
}

// A diamond and the words, with a thin gold rule to the right edge when it heads a section.
static void label(const char* s, ImU32 c = col::dim, bool rule = false) {
    ImGui::PushFont(g_bold, 14.0f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::Dummy(ImVec2(9, h));
    diamond(dl, ImVec2(p.x + 4, p.y + h * 0.5f), 3.5f, col::gold);
    ImGui::SameLine(0, 4);
    ImGui::PushStyleColor(ImGuiCol_Text, ImColor(c).Value);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
    if (rule) {
        ImVec2 e = ImGui::GetItemRectMax();
        float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        if (right - e.x > 40) dl->AddLine(ImVec2(e.x + 8, p.y + h * 0.5f), ImVec2(right, p.y + h * 0.5f), col::frameLo);
    }
    ImGui::PopFont();
}
static void section(const char* s, ImU32 c = col::dim) {
    ImGui::Dummy(ImVec2(0, 4));
    label(s, c, true);
}

static void quote(const std::string& s) {
    if (s.empty()) return;
    ImGui::PushFont(g_body, 16.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImColor(col::dim).Value);
    ImGui::TextWrapped("\"%s\"", s.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

static std::string zenny_str(int z) {
    char b[24];
    snprintf(b, sizeof(b), "%d", z < 0 ? -z : z);
    std::string d = b, o = z < 0 ? "-" : "";
    for (size_t i = 0; i < d.size(); i++) {
        if (i && (d.size() - i) % 3 == 0) o += ',';
        o += d[i];
    }
    return o + " z";
}

// A button in MHW's style: a dark plate in a thin gold frame; the amber cursor (a lit frame and a diamond) on hover.
static bool remnant_button(const char* text, ImVec2 size = ImVec2(0, 0), bool enabled = true, bool selected = false) {
    ImGui::PushFont(g_bold, 18.0f);
    const char* end = strstr(text, "##");  // ImGui's hidden-id suffix is not drawn
    if (!end) end = text + strlen(text);
    ImVec2 ts = ImGui::CalcTextSize(text, end);
    float avail = ImGui::GetContentRegionAvail().x;
    ImVec2 sz(size.x > 0 ? size.x : size.x < 0 ? std::max(10.0f, avail + size.x) : ts.x + 28,
              size.y > 0 ? size.y : ts.y + 12);
    ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + sz.x, a.y + sz.y);
    bool r = ImGui::InvisibleButton(text, sz);
    bool hov = (enabled && ImGui::IsItemHovered()) || selected, held = enabled && ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 top = held ? IM_COL32(120, 86, 34, 255) : hov ? IM_COL32(74, 58, 32, 255) : IM_COL32(12, 22, 30, 215);
    ImU32 bot = held ? IM_COL32(90, 64, 26, 255) : hov ? IM_COL32(52, 41, 24, 255) : IM_COL32(0, 0, 0, 215);
    dl->AddRectFilledMultiColor(a, b, top, top, bot, bot);
    dl->AddRect(a, b, !enabled ? col::frameLo : hov ? col::accent : col::line, 0, 0, hov ? 1.5f : 1.0f);
    if (hov) diamond(dl, ImVec2(a.x + 9, (a.y + b.y) * 0.5f), 4.0f, col::accent);
    ImU32 tc = !enabled ? IM_COL32(110, 102, 88, 255) : hov ? IM_COL32(255, 244, 220, 255) : col::text;
    dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), tc, text, end);
    ImGui::PopFont();
    return r && enabled;
}

// Button heights, one set for every screen.
constexpr float kBtnPrimary = 40, kBtnSecondary = 34, kBtnSmall = 26, kBtnBack = 38;
constexpr float kFoot = 84;                      // the foot band: buttons on top, the hint pill and prompts below
constexpr float kPadX = 28, kPadY = 24;          // the screen window's own padding
constexpr float kRowPad = 24;                    // left edge of a row's content, clear of the cursor diamond

static float text_w(ImFont* f, float size, const char* s) { return f->CalcTextSizeA(size, FLT_MAX, 0, s).x; }

// Draws text at a point and returns its right edge, for rows that lay their pieces out by hand.
static float put(ImFont* f, float size, ImVec2 p, ImU32 c, const char* s) {
    ImGui::GetWindowDrawList()->AddText(f, size, p, c, s);
    return p.x + text_w(f, size, s);
}

// A list row: a flat plate with a rule under it, and the amber cursor (lit frame, diamond) when selected. The
// caller draws its content with row_at and put between list_row and end_list_row.
struct RowBox { ImVec2 a; float w, h; };
static RowBox g_row;
static bool list_row(const char* id, float h, bool selected, bool enabled = true) {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x, 0));
    ImVec2 a = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemAllowOverlap();  // buttons inside the row sit over it
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    ImGui::PopStyleVar();
    ImVec2 b(a.x + w, a.y + h);
    bool hov = enabled && ImGui::IsMouseHoveringRect(a, b);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected) {
        dl->AddRectFilledMultiColor(a, b, IM_COL32(74, 58, 32, 225), IM_COL32(74, 58, 32, 225), IM_COL32(52, 41, 24, 225),
                                    IM_COL32(52, 41, 24, 225));
        dl->AddRect(a, b, col::accent, 0, 0, 1.5f);
        float cy = a.y + h * 0.5f;  // World's small pointer at the left of the picked row
        dl->AddTriangleFilled(ImVec2(a.x + 4, cy - 3), ImVec2(a.x + 4, cy + 3), ImVec2(a.x + 10, cy), col::accent);
    } else {
        dl->AddRectFilled(a, b, hov ? IM_COL32(30, 52, 68, 170) : IM_COL32(0, 0, 0, 120));
        dl->AddLine(ImVec2(a.x, b.y - 1), ImVec2(b.x, b.y - 1), col::frameLo);
    }
    dl->PushClipRect(a, b, true);
    g_row = {a, w, h};
    return clicked && enabled;
}
static void end_list_row() {
    ImGui::GetWindowDrawList()->PopClipRect();
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x, 0));
    ImGui::SetCursorScreenPos(ImVec2(g_row.a.x, g_row.a.y + g_row.h));
    ImGui::Dummy(ImVec2(1, 0));
    ImGui::PopStyleVar();
}
static void row_at(float x, float y) { ImGui::SetCursorScreenPos(ImVec2(g_row.a.x + x, g_row.a.y + y)); }

// A dim "?" that explains itself on hover, so the screens keep one short line instead of a paragraph.
static void help_mark(const char* tip) {
    ImGui::SameLine(0, 10);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float off = std::max(0.0f, (ImGui::GetTextLineHeight() - 18) * 0.5f);
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + off));
    ImGui::PushID(tip);
    ImGui::InvisibleButton("##help", ImVec2(18, 18));
    ImGui::PopID();
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(p.x + 9, p.y + off + 9);
    dl->AddCircle(c, 9, hov ? col::gold : col::line, 20, 1.0f);
    float w = text_w(g_bold, 13, "?");
    dl->AddText(g_bold, 13, ImVec2(c.x - w * 0.5f, c.y - 8), hov ? col::gold : col::dim, "?");
    if (hov) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(520);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static void empty_hint(const char* s) {
    ImVec2 av = ImGui::GetContentRegionAvail();
    float w = ImGui::CalcTextSize(s).x;
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + std::max(0.0f, (av.x - w) * 0.5f), ImGui::GetCursorPosY() + std::min(av.y * 0.35f, 140.0f)));
    ImGui::TextColored(ImColor(col::dim).Value, "%s", s);
}

// MHW's tabs: small pills; the active one is a gold plate with dark words. Q/E and the arrow keys cycle. Returns
// true when the active tab changed.
static bool g_tabsPrompt = false;  // set by tab_strip so the foot can show the Q/E prompt
static bool tab_strip(const std::vector<const char*>& names, int& active) {
    int n = (int)names.size(), was = active;
    ImGuiIO& io = ImGui::GetIO();
    g_tabsPrompt = n > 1;
    if (n > 1 && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_E) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) active = (active + 1) % n;
        if (ImGui::IsKeyPressed(ImGuiKey_Q) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) active = (active + n - 1) % n;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < n; i++) {
        if (i) ImGui::SameLine(0, 6);
        ImGui::PushID(i);
        float tw = text_w(g_bold, 15, names[i]);
        float w = std::max(84.0f, tw + 32);
        ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + 30);
        if (ImGui::InvisibleButton("##tab", ImVec2(w, 30))) active = i;
        bool on = i == active, hov = ImGui::IsItemHovered();
        dl->AddRectFilled(a, b, on ? col::gold : hov ? IM_COL32(52, 44, 30, 235) : IM_COL32(28, 25, 20, 220), 6.0f);
        dl->AddRect(a, b, on ? col::frameHi : col::frameLo, 6.0f);
        dl->AddText(g_bold, 15, ImVec2(a.x + (w - tw) * 0.5f, a.y + 6), on ? IM_COL32(32, 26, 16, 255) : hov ? col::text : col::dim, names[i]);
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 4));
    return active != was;
}

static bool g_escPrompt = true;  // whether this screen can be left with Esc

// Full-screen Remnant window: a light tint over the game and one thin frame; the panels inside carry the colour.
static bool begin_screen(const char* id, float wFrac, float hFrac, bool hint = true) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                                                  IM_COL32(0, 0, 0, 110));
    ImVec2 size(vp->Size.x * wFrac, vp->Size.y * hFrac);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - size.x) / 2, vp->Pos.y + (vp->Size.y - size.y) / 2));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 120));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kPadX, kPadY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    bool open = ImGui::Begin(id, nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);  // the wide padding is the screen's own; its rows and panels keep a small one
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + sz.x, p.y + sz.y);
    dl->AddRect(p, q, col::frameLo, 0, 0, 1.0f);
    for (ImVec2 c : {ImVec2((p.x + q.x) * 0.5f, p.y), ImVec2((p.x + q.x) * 0.5f, q.y)}) diamond(dl, c, 6.0f, col::frameHi);
    g_escPrompt = hint;
    g_tabsPrompt = false;
    return open;
}
static void end_screen() {
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// Top-left of a screen: a small emblem plate and "WARD 13  >  TRAITS", as MHW names the menu you are in.
static void breadcrumb(const char* first, const char* second) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a(p.x, p.y + 2), b(a.x + 22, a.y + 22);
    dl->AddRectFilled(a, b, IM_COL32(24, 21, 16, 230));
    ImGui::SetCursorScreenPos(a);
    image(kUi[UI_WARD13_TERMINAL].art, 128, ImVec2(22, 22));
    dl->AddRect(a, b, col::mhwGold);
    float x = put(g_bold, 16, ImVec2(p.x + 32, p.y + 3), col::dim, upper(first).c_str());
    x = put(g_bold, 16, ImVec2(x, p.y + 3), col::dim, "  >  ");
    put(g_bold, 16, ImVec2(x, p.y + 3), col::text, upper(second).c_str());
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 36));
    ImGui::Dummy(ImVec2(1, 0));
}

// Top-right of a screen: the hunter's zenny on a small dark plate with a coin mark.
static void zenny_plate() {
    std::string z = zenny_str(mhw_zenny());
    float tw = text_w(g_bold, 17, z.c_str()), w = tw + 52;
    ImVec2 p = ImGui::GetWindowPos();
    ImVec2 a(p.x + ImGui::GetWindowWidth() - kPadX - w, p.y + kPadY), b(a.x + w, a.y + 26);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, IM_COL32(16, 14, 11, 225));
    dl->AddRect(a, b, col::frameLo);
    ImVec2 c(a.x + 15, a.y + 13);
    dl->AddCircleFilled(c, 8, IM_COL32(60, 50, 30, 255));
    dl->AddCircle(c, 8, col::gold, 20, 1.2f);
    dl->AddText(g_bold, 11, ImVec2(c.x - text_w(g_bold, 11, "z") * 0.5f, c.y - 7), col::gold, "z");
    dl->AddText(g_bold, 17, ImVec2(b.x - 12 - tw, a.y + 3), col::gold, z.c_str());
}

// A panel: a translucent plate in a gold line with a title band (faint gold gradient, diamonds at both ends, a rule
// under it). The content scrolls in a child of its own so the band stays put.
constexpr float kBand = 28;
static void panel_begin(const char* id, const char* title, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::plate);
    ImGui::PushStyleColor(ImGuiCol_Border, col::mhwGold);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    ImVec2 p = ImGui::GetWindowPos();
    float w = ImGui::GetWindowWidth();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a(p.x + 1, p.y + 1), b(p.x + w - 1, p.y + kBand);
    dl->AddRectFilledMultiColor(a, b, IM_COL32(22, 40, 54, 245), IM_COL32(22, 40, 54, 245), IM_COL32(0, 0, 0, 215), IM_COL32(0, 0, 0, 215));
    dl->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), col::frameHi);
    float tw = text_w(g_title, 18, title);
    dl->AddText(g_title, 18, ImVec2(p.x + (w - tw) * 0.5f, p.y + (kBand - 18) * 0.5f - 1), col::gold, title);
    diamond(dl, ImVec2(a.x + 9, (a.y + b.y) * 0.5f), 3.0f, col::frameHi);
    diamond(dl, ImVec2(b.x - 9, (a.y + b.y) * 0.5f), 3.0f, col::frameHi);
    ImGui::SetCursorPos(ImVec2(8, kBand + 8));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 2));
    ImGui::BeginChild("##body", ImVec2(-8, -8));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}
static void panel_end() {
    ImGui::EndChild();
    ImGui::EndChild();
}

// The foot strip: primary buttons from the left edge, BACK at the right edge, all centred on one line.
static void foot_at(float x, float h) {
    ImGui::SetCursorPos(ImVec2(x, ImGui::GetWindowHeight() - kPadY - kFoot + 4 + (kBtnPrimary - h) * 0.5f));
}
static void foot_next(float h) { foot_at(ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 10, h); }
static bool foot_back(const char* text = "BACK") {
    ImGui::PushFont(g_bold, 18.0f);
    float w = std::max(200.0f, ImGui::CalcTextSize(text).x + 40);
    ImGui::PopFont();
    foot_at(ImGui::GetWindowWidth() - kPadX - w, kBtnBack);
    return remnant_button(text, ImVec2(w, kBtnBack));
}

// The foot line: the hint in a centred pill, the button prompts at the right edge.
static void foot_hint(const char* s) {
    ImVec2 p = ImGui::GetWindowPos();
    float W = ImGui::GetWindowWidth(), y = p.y + ImGui::GetWindowHeight() - kPadY - 24;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float tw = text_w(g_body, 15, s), w = tw + 44, x = p.x + (W - w) * 0.5f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + 24), IM_COL32(20, 18, 15, 225), 12.0f);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + 24), col::line, 12.0f);
    dl->AddText(g_body, 15, ImVec2(x + 22, y + 3), col::dim, s);
    float right = p.x + W - kPadX;
    if (g_escPrompt) {
        right -= text_w(g_body, 15, "[Esc]  Back");
        put(g_body, 15, ImVec2(right, y + 3), col::dim, "[Esc]  Back");
        right -= 24;
    }
    if (g_tabsPrompt) put(g_body, 15, ImVec2(right - text_w(g_body, 15, "[Q]/[E]  Tabs"), y + 3), col::dim, "[Q]/[E]  Tabs");
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
static bool g_fromDock = false;   // opened from the Item Box dock: Esc returns to the dock, not to the terminal
static bool g_itemsView = false;  // the terminal's right panel shows only the items (consumables), MHW's Item Box "Manage Items"

static void ui_sound(const char* id) {
    int i = ui_sounds_index(id);
    if (i >= 0) audio_play(kUi_sounds[i].event, 0.7f);
}

static void open(Screen s) {
    if (s != SCR_NONE && g_screen == SCR_NONE) ui_sound("open");
    g_screen = s;
    if (s == SCR_NONE) g_fromDock = false;
    g_screenOpened = now_s();
    g_greet = (int)(GetTickCount() / 7) % 3;
    g_npcSays.clear();
    g_pick = ItemView();
    g_slotPick = -1;
    g_itemsView = false;  // the section row sets it again after opening the terminal for ITEMS
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
    if (!begin_screen("##intro", 0.56f, 0.62f, false)) return end_screen();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX((w - 140) / 2 + 28);
    image(kUi[UI_INTRO].art, 256, ImVec2(140, 140));
    ImGui::Spacing();
    title("THE WORLDSTONE");
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
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - kPadY - kBtnPrimary - 6);
    bool ready = t > 3.0;  // the button is always submitted (ImGui needs an item after SetCursorPos)
    if ((remnant_button("CONTINUE", ImVec2(200, kBtnPrimary), ready) || (ready && ImGui::IsKeyPressed(ImGuiKey_Enter))))
        open(SCR_ARCHETYPE);
    end_screen();
}

// The Ward 13 section row under the breadcrumb, like MHW's Item Box menu: one pill per screen, the current one lit.
// Q/E cycle it. Not shown on the NPC menus (they carry their own tab strip).
static void section_row(int active) {  // active: 0 equipment, 1 items, 2 archetypes, 3 traits, 4 prisms
    int was = active;
    tab_strip({"EQUIPMENT", "ITEMS", "ARCHETYPES", "TRAITS", "PRISMS"}, active);
    if (active == was) return;
    static const Screen kTarget[5] = {SCR_TERMINAL, SCR_TERMINAL, SCR_ARCHETYPE, SCR_TRAITS, SCR_PRISM};
    open(kTarget[active]);
    g_itemsView = active == 1;
}

// ---- shared pieces

static void effect_lines(const char* dsl) {
    for (const Effect& e : effects_of(dsl)) {
        ImGui::Bullet();
        ImGui::TextWrapped("%s", describe(e).c_str());
    }
}

// A stat row as in MHW's info panels: the label left, the value right-aligned to the panel edge.
static void stat_row(const char* k, const char* v, ImU32 vc = col::text) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float right = p.x + ImGui::GetContentRegionAvail().x;
    ImGui::TextColored(ImColor(col::dim).Value, "%s", k);
    ImGui::SetCursorScreenPos(ImVec2(right - text_w(ImGui::GetFont(), ImGui::GetFontSize(), v), p.y));
    ImGui::TextColored(ImColor(vc).Value, "%s", v);
}
static void stats_line(const char* k, const char* fmt, double v) {
    char b[48];
    snprintf(b, sizeof(b), fmt, v);
    stat_row(k, b);
}

static const char* material_label(int m) {
    int id = (int)kMaterials[m].mhw_item_id;
    const char* n = id > 0 ? mhw_item_name(id) : nullptr;
    return n ? n : kMaterials[m].note;
}

// A small plate standing in for an item icon: MHW's item art is not ours to draw, so the row keeps its slot.
static void item_plate(float size) {
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, ImVec2(a.x + size, a.y + size), IM_COL32(34, 30, 24, 255), 3.0f);
    dl->AddRect(a, ImVec2(a.x + size, a.y + size), col::line, 3.0f);
    diamond(dl, ImVec2(a.x + size * 0.5f, a.y + size * 0.5f), size * 0.2f, col::frameLo);
    ImGui::Dummy(ImVec2(size, size));
}

// Recipe in MHW materials and zenny, with what the hunter has, as two banded panels like MHW's forge. Returns true
// when it can be paid.
static bool recipe_box(const char* recipe, int zenny) {
    bool ok = true;
    bool inv = mhw_inventory_ready();
    std::vector<Need> needs = recipe_of(recipe);
    if (!needs.empty()) {
        ImGui::Spacing();
        panel_begin("##mats", "REQUIRED MATERIALS", ImVec2(0, kBand + 26 + needs.size() * 34.0f));
        float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        for (const Need& n : needs) {
            int id = (int)kMaterials[n.material].mhw_item_id;
            int have = id > 0 && inv ? mhw_item_count(id) : -1;
            bool enough = have < 0 || have >= n.count;  // a material with no MHW id yet is not charged (remnant.cpp skips it too)
            ok &= enough;
            ImU32 c = have < 0 ? col::dim : enough ? col::text : col::red;
            ImVec2 p = ImGui::GetCursorScreenPos();
            item_plate(22);
            float y = p.y + 2;
            char cnt[16];
            snprintf(cnt, sizeof(cnt), "  x%d", n.count);
            float x = put(g_body, 17, ImVec2(p.x + 32, y), col::text, material_label(n.material));
            put(g_body, 16, ImVec2(x, y + 1), col::dim, cnt);
            char hv[32];
            if (have >= 0) snprintf(hv, sizeof(hv), "%d", have);
            else snprintf(hv, sizeof(hv), "not tracked yet");
            put(g_bold, 16, ImVec2(right - text_w(g_bold, 16, hv), y + 1), c, hv);
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 30));
            ImGui::Dummy(ImVec2(1, 0));
        }
        panel_end();
    }
    int z = mhw_zenny();
    bool zok = z >= zenny;
    ok &= zok;
    if (zenny > 0) {
        ImGui::Spacing();
        panel_begin("##cost", "REQUIRED COST", ImVec2(0, kBand + 26 + 34));
        std::string zs = zenny_str(zenny);
        float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        put(g_bold, 18, ImVec2(right - text_w(g_bold, 18, zs.c_str()), ImGui::GetCursorScreenPos().y), zok ? col::gold : col::red, zs.c_str());
        ImGui::Dummy(ImVec2(1, 22));
        panel_end();
    }
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
        section("MOD");
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
    char xp[64];
    snprintf(xp, sizeof(xp), "%.0f / %.0f exp", a, b);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float right = p.x + ImGui::GetContentRegionAvail().x;
    ImGui::TextColored(ImColor(col::gold).Value, "Remnant Level %d", lv);
    put(g_body, 18, ImVec2(right - text_w(g_body, 18, xp), p.y), col::dim, xp);
    bar((float)(a / std::max(1.0, b)), col::gold);
}

// Ten pips: the archetypes' own points (dim gold), then the hunter's (gold), then empty.
static void pips_at(ImVec2 p, int n, int of, int automatic = 0) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < of; i++) {
        ImVec2 a(p.x + i * 12.0f, p.y), b(a.x + 9, a.y + 9);
        if (i < automatic) dl->AddRectFilled(a, b, IM_COL32(150, 122, 70, 255));
        else if (i < n) dl->AddRectFilled(a, b, col::gold);
        else dl->AddRect(a, b, col::line);
    }
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
    ImVec2 lp = ImGui::GetCursorScreenPos();
    float lright = lp.x + ImGui::GetContentRegionAvail().x;
    ImGui::TextColored(ImColor(col::gold).Value, "Level %d", level);
    double next = archetype_next_xp(level);
    if (next > 0) {
        char xp[64];
        snprintf(xp, sizeof(xp), "%.0f / %.0f exp", s.archXp[row], next);
        put(g_body, 18, ImVec2(lright - text_w(g_body, 18, xp), lp.y), col::dim, xp);
        bar((float)(s.archXp[row] / next), col::gold);
    } else {
        const char* m = "mastered: its trait works with any archetype";
        put(g_body, 16, ImVec2(lright - text_w(g_body, 16, m), lp.y + 1), col::dim, m);
    }
    if (a.prime.asset) {
        section("PRIME PERK");
        ImGui::TextWrapped("%s", text_or(&a.prime).c_str());
    }
    std::vector<int> sk = skills_of(row);
    section("SKILLS");
    int open = skills_unlocked(level);
    static const int kSkillLevel[3] = {1, 5, 10};
    for (size_t i = 0; i < sk.size(); i++) {
        const Skills& k = kSkills[sk[i]];
        ImGui::PushID((int)i);
        bool chosen = slot >= 0 && skill_index(s, slot) == (int)i;
        bool unlocked = (int)i < open;
        ImVec2 p = ImGui::GetCursorScreenPos();
        float right = p.x + ImGui::GetContentRegionAvail().x;
        image(k.icon, 128, ImVec2(40, 40), unlocked ? IM_COL32_WHITE : IM_COL32(110, 110, 110, 200));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImVec2 g = ImGui::GetCursorScreenPos();
        ImGui::TextColored(ImColor(!unlocked ? col::dim : chosen ? col::gold : col::text).Value, "%s", text_or(&k.name).c_str());
        ImGui::SameLine(0, 12);
        ImGui::PushFont(g_body, 14.0f);
        ImGui::TextColored(ImColor(col::dim).Value, "cooldown %.0f s", k.cooldown_s);
        ImGui::PopFont();
        if (chosen) {
            float w = text_w(g_bold, 14, "IN USE");
            put(g_bold, 14, ImVec2(right - w - 8, g.y + 4), col::gold, "IN USE");
        } else if (chooseSkill && slot >= 0 && unlocked) {
            ImGui::SetCursorScreenPos(ImVec2(right - 160, g.y - 2));
            if (remnant_button("USE THIS SKILL", ImVec2(160, kBtnSmall))) {
                Shared& sh = S();
                AcquireSRWLockExclusive(&sh.lock);
                sh.save.skill[slot] = (int)i;
                sh.live.dirty = true;
                ReleaseSRWLockExclusive(&sh.lock);
            }
        }
        ImGui::SetCursorScreenPos(ImVec2(g.x, g.y + 28));
        if (!unlocked) ImGui::TextColored(ImColor(col::dim).Value, "Unlocks at level %d", kSkillLevel[std::min<size_t>(2, i)]);
        effect_lines(k.effects);
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PopID();
    }
    std::vector<int> pk = perks_of(row);
    section("PERKS");
    for (int p : pk) {
        float k = perk_scale(p, level);
        bool prime = kPerks[p].kind == PERKS_KIND_PRIME;
        bool primaryOnly = prime && slot != 0 && !prime_time(s);
        bool active = k > 0 && (!prime || slot == 0 || (slot == 1 && prime_time(s)));
        ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::TextColored(ImColor(active ? (prime ? col::gold : col::text) : col::dim).Value, "%s", text_or(&kPerks[p].name).c_str());
        float x = put(g_bold, 13, ImVec2(at.x + 240, at.y + 4), col::dim, perk_kind_label(p)) + 14;
        char st[64];
        if (k <= 0) {
            snprintf(st, sizeof(st), "unlocks at level %d", perk_unlock_level(p));
            put(g_body, 14, ImVec2(x, at.y + 3), col::dim, st);
        } else if (primaryOnly) {
            put(g_body, 14, ImVec2(x, at.y + 3), col::dim, "only as the primary archetype");
        } else {
            snprintf(st, sizeof(st), "%d%%", (int)lroundf(k * 100));
            x = put(g_bold, 14, ImVec2(x, at.y + 3), col::gold, st) + 10;
            if (k < 1) put(g_body, 14, ImVec2(x, at.y + 3), col::dim, "grows with level");
        }
        ImGui::Indent();
        effect_lines(kPerks[p].effects);
        ImGui::Unindent();
    }
    if (a.trait >= 0) {
        section("ARCHETYPE TRAIT");
        ImGui::TextUnformatted(text_or(&kTraits[a.trait].name).c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImColor(col::dim).Value, "  level %d of 10 while equipped (one per archetype level)", level);
        ImGui::Indent();
        effect_lines(kTraits[a.trait].effects);
        ImGui::Unindent();
    }
    section("CORE TRAITS AS PRIMARY");
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

static void item_detail(const ItemView& v, const SaveData& s, const std::function<void()>& beforeStats = nullptr) {
    float w = ImGui::GetContentRegionAvail().x;
    image(v.icon, 512, ImVec2(w, v.cat == CAT_WEAPON ? 110 : 96));
    title(upper(text_or(v.name)).c_str());
    ImGui::TextColored(ImColor(col::dim).Value, "%s", cat_label(v.cat));
    char rar[8];
    snprintf(rar, sizeof(rar), "%d", rarity_of(v.tier));
    stat_row("Rarity", rar, rarity_color(v.tier));
    if (v.desc && v.desc->asset) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImColor(col::dim).Value);
        ImGui::TextWrapped("%s", text_or(v.desc).c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    if (beforeStats) beforeStats();  // the cost sits by the name, as World's Required Materials panel does
    if (v.cat == CAT_ARCHETYPE) {
        archetype_detail(v.row, s, false);
        return;
    }
    if (v.cat == CAT_PRISM) {
        ImGui::TextColored(ImColor(col::dim).Value, "Equip it and choose its fragments in the PRISM screen at Ward 13.");
        help_mark("A prism holds five relic fragments that grow as you hunt with it equipped, fuse in pairs and end "
                  "in a legendary bonus.");
    }
    if (v.effects && *v.effects) {
        section(v.cat == CAT_TRAIT ? "AT LEVEL 10 (TRAIT POINTS FROM REMNANT LEVELS)" : "EFFECTS");
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

// The rows of one category; the first visible row is picked when nothing (visible) is.
static void item_list(Cat c, int weaponSlot, const SaveData& s, bool ownedOnly, int menuRole) {
    ImGui::SetNextItemWidth(-1);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 8));
    ImGui::InputTextWithHint("##filter", "Search", g_filter, sizeof(g_filter));  // 34 high
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, 2));
    std::vector<int> rows;
    for (int i = 0; i < cat_count(c); i++) {
        ItemView v = item(c, i);
        // archetype engrams (menu "archetype") are the Elder Melder's; the free starting ones are given, not sold
        int sells = c == CAT_ARCHETYPE && v.menu == ARCHETYPES_MENU_ARCHETYPE ? (v.zenny > 0 ? MENUS_ROLE_MELDER : -2) : v.menu;
        if (menuRole >= 0 && sells != menuRole) continue;
        if (c == CAT_WEAPON && weaponSlot >= 0 && kWeapons[i].slot != weaponSlot) continue;
        bool own = c == CAT_CONSUMABLE ? s.stock[i] > 0 : s.has(c, i);
        if (ownedOnly && !own) continue;
        if (!matches_filter(v)) continue;
        rows.push_back(i);
    }
    bool picked = false;
    for (int i : rows) picked |= g_pick.cat == c && g_pick.row == i;
    if (!picked) g_pick = rows.empty() ? ItemView() : item(c, rows[0]);
    ImGui::BeginChild("rows", ImVec2(0, 0));
    if (rows.empty()) empty_hint("Nothing here yet");
    for (int i : rows) {
        ItemView v = item(c, i);
        bool own = c == CAT_CONSUMABLE ? s.stock[i] > 0 : s.has(c, i);
        ImGui::PushID(i);
        bool sel = g_pick.cat == c && g_pick.row == i;
        if (list_row("r", 52, sel)) g_pick = v;
        float iw = c == CAT_WEAPON ? 72 : 40;
        row_at(kRowPad, 6);
        image(v.icon, 128, ImVec2(iw, 40));
        float x = g_row.a.x + kRowPad + iw + 10;
        put(g_body, 18, ImVec2(x, g_row.a.y + 5), sel ? col::gold : col::text, text_or(v.name).c_str());
        char sub[64];
        ImU32 sc = col::dim;
        if (c == CAT_CONSUMABLE) snprintf(sub, sizeof(sub), "carried %d  -  %s", s.stock[i], zenny_str(v.zenny).c_str());
        else if (own) snprintf(sub, sizeof(sub), "owned");
        else { snprintf(sub, sizeof(sub), "RARE %d", rarity_of(v.tier)); sc = rarity_color(v.tier); }
        put(g_body, 14, ImVec2(x, g_row.a.y + 29), sc, sub);
        end_list_row();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// ---- the Remnant menu at an MHW NPC

static void screen_menu(const SaveData& s, const Live& l) {
    int hostRow = g_menu >= 0 ? kMenus[g_menu].host : host_of_role(g_menuRole);
    const Npcs& n = kNpcs[hostRow];
    if (!begin_screen("##menu", 0.80f, 0.86f)) return end_screen();
    if (g_npcSays.empty()) g_npcSays = r2_text(greet_of(n, g_greet));
    breadcrumb(role_place(g_menuRole) + 4, "Remnant Wares");  // "the Smithy" -> "Smithy"
    zenny_plate();
    quote(g_npcSays);
    if (l.said[0] && strcmp(l.said, "done") != 0) ImGui::TextColored(ImColor(col::red).Value, "%s", l.said);
    ImGui::Dummy(ImVec2(0, 2));
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
    static int active[3] = {0, 0, 0};
    int& cur = active[std::max(0, std::min(2, g_menuRole))];
    std::vector<const char*> names;
    for (const Tab& tb : tabs) names.push_back(tb.name);
    cur = std::min(cur, (int)tabs.size() - 1);
    if (tab_strip(names, cur)) { g_pick = ItemView(); g_filter[0] = 0; }
    const Tab& tb = tabs[cur];
    bool upgrades = tb.slot == -2;
    float w = ImGui::GetContentRegionAvail().x;
    const char* hint = "Select an item.";
    panel_begin("list", tb.name, ImVec2(w * 0.42f, -kFoot));
    item_list(tb.cat, upgrades ? -1 : tb.slot, s, upgrades, upgrades ? -1 : g_menuRole);
    panel_end();
    ImGui::SameLine(0, 20);
    panel_begin("detail", "EQUIPMENT INFO", ImVec2(0, -kFoot));
    // The action button is pinned under a scrolling detail, MHW style: the cost is always one glance from the button.
    const float actionH = kBtnPrimary + 12;
    ImGui::BeginChild("detailscroll", ImVec2(0, -actionH));
    char action[48] = {0};
    bool actionOk = false;
    int actionKind = -1;  // Request kind, or -2 = take a free archetype
    if (g_pick.row >= 0 && g_pick.cat == tb.cat) {
        const ItemView& v = g_pick;
        auto cost = [&]() {
            if (upgrades) {
                int lvl = s.level[v.row], top = weapon_max_level(v.row);
                if (lvl < top) {
                    int zen = 0;
                    std::string rec = upgrade_cost(v.row, lvl, &zen);
                    ImGui::Text("+%d  ->  +%d of %d  (damage x%.2f -> x%.2f)", lvl, lvl + 1, top, upgrade_mult(v.row, lvl),
                                upgrade_mult(v.row, lvl + 1));
                    actionOk = recipe_box(rec.c_str(), zen);
                    snprintf(action, sizeof(action), "UPGRADE TO +%d", lvl + 1);
                    actionKind = Request::UPGRADE;
                    hint = actionOk ? "Can be upgraded." : "You lack materials.";
                } else {
                    ImGui::TextColored(ImColor(col::gold).Value, "Fully upgraded (+%d)", lvl);
                    hint = "Fully upgraded.";
                }
            } else if (v.cat == CAT_CONSUMABLE) {
                actionOk = recipe_box("", v.zenny);
                snprintf(action, sizeof(action), "BUY  (%s)", zenny_str(v.zenny).c_str());
                actionKind = Request::BUY_CONSUMABLE;
                hint = actionOk ? "Can be bought." : "You lack zenny.";
            } else if (!s.has(v.cat, v.row)) {
                bool free = v.cat == CAT_ARCHETYPE && v.zenny <= 0;
                actionOk = free || recipe_box(v.recipe, v.zenny);
                snprintf(action, sizeof(action), "%s", free ? "TAKE" : "CRAFT");
                actionKind = free ? -2 : Request::CRAFT;
                hint = actionOk ? "Can be crafted." : "You lack materials.";
            } else {
                ImGui::TextColored(ImColor(col::gold).Value, "Owned. Equip it at the Ward 13 terminal [%c].", (char)keys().terminal);
                hint = "Owned.";
            }
            ImGui::Spacing();
        };
        item_detail(v, s, cost);
    } else {
        empty_hint("Select an item to see its details");
    }
    ImGui::EndChild();
    if (action[0]) {
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - kBtnPrimary - 4);
        if (remnant_button(action, ImVec2(-1, kBtnPrimary), actionOk)) {
            const ItemView& v = g_pick;
            if (actionKind == -2) choose_archetype(v.row, s.archetype < 0 ? 0 : 1);
            else request(Request{(Request::Kind)actionKind, v.cat, v.row});
            g_npcSays = r2_text(n.done_line);
            ui_sound(actionKind == Request::UPGRADE ? "upgrade" : actionKind == Request::BUY_CONSUMABLE ? "buy" : "craft");
        }
    }
    panel_end();
    std::string leave = r2_text(n.leave_option);
    ImGui::PushFont(g_bold, 18.0f);
    float lw = std::max(240.0f, ImGui::CalcTextSize(leave.c_str()).x + 40);  // a long farewell still fits its button
    ImGui::PopFont();
    foot_at(ImGui::GetWindowWidth() - kPadX - lw, kBtnBack);
    if (remnant_button(leave.c_str(), ImVec2(lw, kBtnBack))) open(SCR_NONE);
    foot_hint(hint);
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

static const char* role_label(int role) {
    return role == MENUS_ROLE_SMITHY ? "SMITHY" : role == MENUS_ROLE_MELDER ? "ELDER MELDER" : "PROVISIONS";
}

static void screen_terminal(const SaveData& s, const Live& l) {
    if (!begin_screen("##terminal", 0.84f, 0.92f)) return end_screen();
    breadcrumb("Ward 13", g_itemsView ? "Items" : "Loadout");
    zenny_plate();
    section_row(g_itemsView ? 1 : 0);
    float w = ImGui::GetContentRegionAvail().x;
    panel_begin("left", "WARD 13", ImVec2(w * 0.40f, -kFoot));
    image(kUi[UI_WARD13_TERMINAL].art, 1024, ImVec2(ImGui::GetContentRegionAvail().x, 72));
    section("ARCHETYPES");
    for (int slot = 0; slot < 2; slot++) {
        int a = slot == 0 ? s.archetype : s.archetype2;
        ImGui::PushID(100 + slot);
        if (list_row("a", 52, false)) open(SCR_ARCHETYPE);
        if (a >= 0) {
            row_at(kRowPad, 6);
            image(kArchetypes[a].emblem, 128, ImVec2(40, 40));
            float x = g_row.a.x + kRowPad + 50;
            float e = put(g_body, 18, ImVec2(x, g_row.a.y + 5), col::text, text_or(&kArchetypes[a].name).c_str());
            char lv[16];
            snprintf(lv, sizeof(lv), "Lv %d", archetype_level(s, a));
            put(g_bold, 14, ImVec2(e + 10, g_row.a.y + 9), col::gold, lv);
            std::vector<int> sk = skills_of(a);
            if (!sk.empty()) {
                char b[96];
                snprintf(b, sizeof(b), "[%c] %s", (char)(slot == 0 ? keys().skill : keys().skill2), text_or(&kSkills[sk[skill_index(s, slot)]].name).c_str());
                put(g_body, 14, ImVec2(x, g_row.a.y + 29), col::dim, b);
            }
        } else {
            put(g_body, 16, ImVec2(g_row.a.x + kRowPad, g_row.a.y + 16), col::dim, slot == 0 ? "none" : "none (craft engrams at the Elder Melder)");
        }
        end_list_row();
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 4));
    int vendors = 0;
    for (int role = 0; role < 3; role++) vendors += !role_has_spot(role);  // visit them in person once their spot is known
    ImGui::BeginChild("slots", ImVec2(0, vendors ? -(26 + vendors * (kBtnSecondary + 4) + 8) : 0));
    for (int i = 0; i < (int)(sizeof(kSlots) / sizeof(kSlots[0])); i++) {
        const Slot& sl = kSlots[i];
        if (i == 0) section("WEAPONS");
        else if (sl.cat == CAT_RELIC) section("RELIC");
        else if (sl.cat == CAT_AMULET) section("ACCESSORIES");
        else if (sl.cat == CAT_ARMOR && sl.sub == 0) section("ARMOR");
        int row = slot_row(s, sl);
        ImGui::PushID(i);
        bool sel = g_slotPick == i;
        if (list_row("s", 52, sel)) { g_slotPick = i; g_itemsView = false; g_pick = ItemView(); g_filter[0] = 0; }
        ItemView v = row >= 0 ? item(sl.cat, row) : ItemView();
        row_at(kRowPad, 6);
        image(v.icon, 128, ImVec2(40, 40));
        float x = g_row.a.x + kRowPad + 50;
        put(g_bold, 14, ImVec2(x, g_row.a.y + 5), sel ? col::gold : col::dim, sl.label);
        if (row >= 0) {
            bool fixed = sl.cat == CAT_MOD && kWeapons[sl.sub == 0 ? std::max(0, s.longGun) : std::max(0, s.handGun)].mod >= 0;
            float e = put(g_body, 18, ImVec2(x, g_row.a.y + 25), col::text, text_or(v.name).c_str());
            if (sl.cat == CAT_WEAPON && s.level[row]) {
                char lv[16];
                snprintf(lv, sizeof(lv), "+%d", s.level[row]);
                e = put(g_bold, 16, ImVec2(e + 8, g_row.a.y + 27), col::gold, lv);
            }
            if (fixed) put(g_body, 14, ImVec2(e + 8, g_row.a.y + 29), col::dim, "(built in)");
        } else {
            put(g_body, 16, ImVec2(x, g_row.a.y + 27), col::dim, "empty");
        }
        end_list_row();
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (vendors) {
        section("REMNANT VENDORS");
        for (int role = 0; role < 3; role++) {
            if (role_has_spot(role)) continue;
            ImGui::PushID(200 + role);
            std::string b = std::string(role_label(role)) + "  -  " + r2_name(kNpcs[host_of_role(role)].name);
            if (remnant_button(b.c_str(), ImVec2(-1, kBtnSecondary))) open_menu(-1, role);
            ImGui::PopID();
        }
    }
    panel_end();
    ImGui::SameLine(0, 24);
    panel_begin("right", g_slotPick >= 0 ? kSlots[g_slotPick].label : g_itemsView ? "ITEMS" : "HUNTER", ImVec2(0, -kFoot));
    if (g_slotPick >= 0) {
        const Slot& sl = kSlots[g_slotPick];
        float rw = ImGui::GetContentRegionAvail().x;
        ImGui::BeginChild("choices", ImVec2(rw * 0.45f, 0));
        item_list(sl.cat, sl.cat == CAT_WEAPON ? sl.sub : -1, s, true, -1);
        ImGui::EndChild();
        ImGui::SameLine(0, 16);
        ImGui::BeginChild("pick", ImVec2(0, 0));
        if (g_pick.row >= 0 && g_pick.cat == sl.cat) item_detail(g_pick, s);
        else empty_hint("Select an item to see its details");
        ImGui::EndChild();
    } else {
        char rc[16];
        snprintf(rc, sizeof(rc), "%d", s.relicCharges);
        if (!g_itemsView) level_line(s);
        stat_row("Relic charges", rc, col::gold);
        ImGui::Spacing();
        if (!g_itemsView) {
            int free = trait_points(s) - trait_points_spent(s);
            bool ready = s.prismOn >= 0 && s.prism[s.prismOn].offers > 0;
            char tb[48], pb[48];
            snprintf(tb, sizeof(tb), free > 0 ? "TRAITS (%d)" : "TRAITS", free);
            snprintf(pb, sizeof(pb), ready ? "PRISM (LEVEL UP)" : "PRISM");
            float half = (ImGui::GetContentRegionAvail().x - 8) / 2;
            if (remnant_button(tb, ImVec2(half, kBtnSecondary))) open(SCR_TRAITS);
            ImGui::SameLine(0, 8);
            if (remnant_button(pb, ImVec2(half, kBtnSecondary))) open(SCR_PRISM);
        }
        // Remnant II's difficulty for the Remnant monsters met from now on
        if (!g_itemsView) section("REMNANT DIFFICULTY");
        if (!g_itemsView) {
            float bw = (ImGui::GetContentRegionAvail().x - 3 * 6) / kDifficultiesCount;
            for (int i = 0; i < kDifficultiesCount; i++) {
                ImGui::PushID(400 + i);
                if (i) ImGui::SameLine(0, 6);
                bool on = s.difficulty == i;
                if (remnant_button(kDifficulties[i].label, ImVec2(bw, kBtnSecondary), true, on) && !on)
                    change_save([i](SaveData& d) { d.difficulty = i; return true; });
                if (ImGui::IsItemHovered()) {
                    const Difficulties& d = kDifficulties[i];
                    ImGui::SetTooltip("Remnant monsters: health x%.2f, damage x%.2f, pace x%.2f%s; experience x%.1f", d.hp_mult,
                                      d.dmg_mult, d.tempo, d.enrage_frac > 0 ? ", enraged sooner" : ", never enraged", d.xp_mult);
                }
                ImGui::PopID();
            }
        }
        if (!g_itemsView) section("EQUIPPED");
        if (!g_itemsView) {
            ImVec2 g0 = ImGui::GetCursorScreenPos();
            float cw = ImGui::GetContentRegionAvail().x / 2;
            int cnt = (int)(sizeof(kSlots) / sizeof(kSlots[0]));
            for (int i = 0; i < cnt; i++) {
                const Slot& sl = kSlots[i];
                int row = slot_row(s, sl);
                ImVec2 at(g0.x + (i % 2) * cw, g0.y + (i / 2) * 38.0f);
                put(g_body, 14, at, col::dim, sl.label);
                std::string nm = row >= 0 ? text_or(item(sl.cat, row).name) : "empty";
                if (nm.size() > 30) nm = nm.substr(0, 28) + "..";
                put(g_body, 16, ImVec2(at.x, at.y + 16), row >= 0 ? col::text : IM_COL32(110, 102, 88, 255), nm.c_str());
            }
            ImGui::Dummy(ImVec2(1, ((cnt + 1) / 2) * 38.0f));
        }
        section("CONSUMABLES");
        bool any = false;
        for (int i = 0; i < kConsumablesCount; i++) {
            if (s.stock[i] <= 0) continue;
            any = true;
            ImGui::PushID(i);
            list_row("c", 50, false);
            row_at(kRowPad, 7);
            image(kConsumables[i].icon, 128, ImVec2(36, 36));
            float x = g_row.a.x + kRowPad + 46;
            float e = put(g_body, 18, ImVec2(x, g_row.a.y + 4), col::text, text_or(&kConsumables[i].name).c_str());
            char q[32];
            snprintf(q, sizeof(q), "x%d%s", s.stock[i], s.concoction == i ? "  (active)" : "");
            put(g_bold, 14, ImVec2(e + 10, g_row.a.y + 8), col::gold, q);
            std::vector<Effect> fx = effects_of(kConsumables[i].effects);
            if (!fx.empty()) put(g_body, 14, ImVec2(x, g_row.a.y + 28), col::dim, describe(fx[0]).c_str());
            ImGui::SetCursorScreenPos(ImVec2(g_row.a.x + g_row.w - 8 - 90, g_row.a.y + (50 - kBtnSmall) * 0.5f));
            if (remnant_button("USE", ImVec2(90, kBtnSmall))) use_consumable(i);
            end_list_row();
            ImGui::PopID();
        }
        if (!any) ImGui::TextColored(ImColor(col::dim).Value, "None carried. %s sells them at %s.",
                                     r2_name(kNpcs[host_of_role(MENUS_ROLE_PROVISIONS)].name).c_str(), role_place(MENUS_ROLE_PROVISIONS));
    }
    panel_end();
    if (g_slotPick >= 0) {
        const Slot& sl = kSlots[g_slotPick];
        float nx = kPadX;
        if (g_pick.row >= 0 && g_pick.cat == sl.cat) {
            foot_at(kPadX, kBtnPrimary);
            if (remnant_button("EQUIP", ImVec2(200, kBtnPrimary), can_equip_here(l))) {
                equip(sl.cat, g_pick.row, sl.sub);
                ui_sound(sl.cat == CAT_FRAGMENT ? "socket" : "equip");
            }
            nx = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 10;
        }
        if (slot_row(s, sl) >= 0 && sl.cat != CAT_WEAPON) {
            foot_at(nx, kBtnSecondary);
            if (remnant_button("REMOVE", ImVec2(200, kBtnSecondary))) equip(sl.cat, -1, sl.sub);
        }
        if (foot_back("BACK TO HUNTER")) { g_slotPick = -1; g_pick = ItemView(); }
    } else if (foot_back("CLOSE")) {
        open(SCR_NONE);
    }
    foot_hint(g_slotPick >= 0 ? "Choose an item to equip." : g_itemsView ? "Use a consumable." : "Pick a slot to change it.");
    end_screen();
}

// ---- traits and prisms (progress.cpp)

static int g_traitPick = -1;

static void screen_traits(const SaveData& s) {
    if (!begin_screen("##traits", 0.80f, 0.86f)) return end_screen();
    breadcrumb("Ward 13", "Traits");
    zenny_plate();
    section_row(3);
    int earned = trait_points(s), spent = trait_points_spent(s);
    static const char* kPass[4] = {"CORE TRAITS", "ARCHETYPE TRAITS", "LEARNED TRAITS", "NOT LEARNED YET"};
    std::vector<int> order[4];  // core, archetype, learned, not yet learned
    for (int i = 0; i < kTraitsCount; i++) {
        const Traits& t = kTraits[i];
        bool known = trait_known(s, i);
        int want = t.kind == TRAITS_KIND_CORE ? 0 : t.kind == TRAITS_KIND_ARCHETYPE ? (known ? 1 : -1) : known ? 2 : 3;
        if (want >= 0) order[want].push_back(i);
    }
    if (g_traitPick < 0 || g_traitPick >= kTraitsCount)
        for (int pass = 0; pass < 4 && g_traitPick < 0; pass++)
            if (!order[pass].empty()) g_traitPick = order[pass][0];
    float w = ImGui::GetContentRegionAvail().x;
    panel_begin("list", "TRAITS", ImVec2(w * 0.52f, -kFoot));
    level_line(s);
    ImGui::TextColored(ImColor(spent < earned ? col::gold : col::dim).Value, "%d trait point%s to spend   (%d of %d used)",
                       earned - spent, earned - spent == 1 ? "" : "s", spent, earned);
    help_mark("As in Remnant II: your primary archetype puts 5 points into the core traits, each equipped archetype "
              "raises its own trait by one per archetype level, and every Remnant level earns a trait point to spend "
              "(85 at most). A trait goes up to 10. Your own points can be taken back here at any time. Learn more "
              "traits from the Elder Melder or from Remnant bosses.");
    ImGui::Spacing();
    ImGui::BeginChild("rows", ImVec2(0, 0));
    for (int pass = 0; pass < 4; pass++) {
        if (order[pass].empty()) continue;
        section(kPass[pass]);
        for (int i : order[pass]) {
            const Traits& t = kTraits[i];
            bool known = trait_known(s, i);
            ImGui::PushID(i);
            bool sel = g_traitPick == i;
            if (list_row("t", 36, sel)) g_traitPick = i;
            int lv = trait_level(s, i), au = std::min(10, trait_auto(s, i)), mine = i < (int)s.trait.size() ? s.trait[i] : 0;
            float y = g_row.a.y + 8;
            ImGui::SetCursorScreenPos(ImVec2(g_row.a.x + kRowPad, g_row.a.y + 4));
            image(t.icon, 128, ImVec2(28, 28), known ? IM_COL32_WHITE : IM_COL32(120, 115, 108, 255));
            put(g_body, 18, ImVec2(g_row.a.x + kRowPad + 36, y - 1), !known ? col::dim : sel ? col::gold : col::text, text_or(&t.name).c_str());
            float px = g_row.a.x + g_row.w * 0.46f;
            if (known) {
                pips_at(ImVec2(px, g_row.a.y + 13), lv, 10, au);
                bool spendable = trait_can_set(s, i, 1) || mine > 0;
                float bx = g_row.a.x + g_row.w - 8 - kBtnSmall;
                if (spendable) {
                    ImGui::SetCursorScreenPos(ImVec2(bx - 4 - kBtnSmall, g_row.a.y + 5));
                    if (remnant_button("-", ImVec2(kBtnSmall, kBtnSmall), mine > 0)) change_save([i](SaveData& d) { return trait_set(d, i, -1); });
                    ImGui::SetCursorScreenPos(ImVec2(bx, g_row.a.y + 5));
                    if (remnant_button("+", ImVec2(kBtnSmall, kBtnSmall), trait_can_set(s, i, 1)))
                        change_save([i](SaveData& d) { return trait_set(d, i, 1); });
                } else if (t.kind == TRAITS_KIND_ARCHETYPE) {
                    const char* m = "with its archetype";
                    put(g_body, 14, ImVec2(g_row.a.x + g_row.w - 8 - text_w(g_body, 14, m), y + 2), col::dim, m);
                }
            } else {
                put(g_body, 14, ImVec2(px, y + 2), col::dim, "not learned yet");
            }
            end_list_row();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    panel_end();
    ImGui::SameLine(0, 20);
    panel_begin("detail", "TRAIT INFO", ImVec2(0, -kFoot));
    if (g_traitPick >= 0 && g_traitPick < kTraitsCount) {
        const Traits& t = kTraits[g_traitPick];
        image(t.icon, 256, ImVec2(72, 72));
        ImGui::SameLine();
        ImGui::BeginGroup();
        title(upper(text_or(&t.name)).c_str());
        if (t.desc.asset) dim_wrapped(text_or(&t.desc).c_str());
        ImGui::EndGroup();
        section("AT LEVEL 10");
        effect_lines(t.effects);
        int lv = trait_level(s, g_traitPick), au = std::min(10, trait_auto(s, g_traitPick));
        ImGui::TextColored(ImColor(col::dim).Value, "Now level %d: %d%% of that%s.", lv, lv * 10,
                           au > 0 ? (std::string(" (") + std::to_string(au) + " from your archetypes)").c_str() : "");
        if (!trait_known(s, g_traitPick))
            dim_wrapped(("Learn it from " + r2_name(kNpcs[host_of_role(MENUS_ROLE_MELDER)].name) + " (" +
                         role_place(MENUS_ROLE_MELDER) + "), or from a Remnant boss.").c_str());
    } else {
        empty_hint("Pick a trait to see what it does");
    }
    if (s.archetype >= 0 && kArchetypes[s.archetype].trait >= 0) {
        const Traits& at = kTraits[kArchetypes[s.archetype].trait];
        ImGui::Spacing();
        ImGui::Separator();
        section("ARCHETYPE TRAIT");
        ImGui::TextColored(ImColor(col::gold).Value, "%s", text_or(&at.name).c_str());
        ImGui::SameLine(0, 12);
        ImGui::TextColored(ImColor(col::dim).Value, "one point per archetype level, free");
        help_mark(("From your primary archetype, " + text_or(&kArchetypes[s.archetype].name) + ": one point per archetype level, "
                   "free. Your second archetype's trait works the same way. Once an archetype reaches level 10, its "
                   "trait can be used with any archetype for trait points.").c_str());
        effect_lines(at.effects);
    }
    panel_end();
    foot_at(kPadX, kBtnSecondary);
    if (remnant_button("TAKE ALL POINTS BACK", ImVec2(260, kBtnSecondary), spent > 0)) change_save([](SaveData& d) { traits_reset(d); return true; });
    if (foot_back()) open(SCR_TERMINAL);
    foot_hint("Spend or take back trait points.");
    end_screen();
}

static int g_prismPick = -1;
static double g_cleanseArmed = 0;

// The effect lines of a fragment or fusion in a slot or offer.
static void slot_effects(const SaveData::PSlot& sl) {
    if (sl.kind == PS_FUSION) {
        effect_lines(kFragments[kFusions[sl.row].a].effects);
        effect_lines(kFragments[kFusions[sl.row].b].effects);
    } else if (sl.kind == PS_FRAGMENT) {
        effect_lines(kFragments[sl.row].effects);
    }
}

static std::string slot_name(const SaveData::PSlot& sl) {
    return sl.kind == PS_FUSION ? text_or(&kFusions[sl.row].name) : text_or(&kFragments[sl.row].name);
}

static void slot_line(const SaveData::PSlot& sl, int i) {
    if (sl.kind == PS_FRAGMENT) {
        ImGui::TextColored(ImColor(col::text).Value, "%d.  %s", i + 1, text_or(&kFragments[sl.row].name).c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImColor(col::gold).Value, "+%d", sl.lv);
        ImGui::Indent();
        slot_effects(sl);
        ImGui::Unindent();
    } else if (sl.kind == PS_FUSION) {
        const Fusions& f = kFusions[sl.row];
        ImGui::TextColored(ImColor(col::gold).Value, "%d.  %s", i + 1, text_or(&f.name).c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImColor(col::dim).Value, "(%s + %s)", text_or(&kFragments[f.a].name).c_str(), text_or(&kFragments[f.b].name).c_str());
        ImGui::SameLine();
        ImGui::TextColored(ImColor(col::gold).Value, "+%d", sl.lv);
        ImGui::Indent();
        slot_effects(sl);
        ImGui::Unindent();
    }
}

// The five fixed slots of a prism as a row of cells.
static void fragment_cells(const SaveData::Prism& p) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float cw = (ImGui::GetContentRegionAvail().x - 4 * 8) / 5;
    for (int i = 0; i < 5; i++) {
        const SaveData::PSlot& sl = p.slot[i];
        ImVec2 a(o.x + i * (cw + 8), o.y), b(a.x + cw, a.y + 64);
        bool full = sl.kind == PS_FRAGMENT || sl.kind == PS_FUSION;
        dl->AddRectFilled(a, b, full ? IM_COL32(34, 31, 25, 255) : IM_COL32(20, 19, 16, 255));
        dl->AddRect(a, b, full ? col::line : col::frameLo);
        dl->PushClipRect(a, b, true);
        char n[8];
        snprintf(n, sizeof(n), "%d", i + 1);
        dl->AddText(g_body, 13, ImVec2(a.x + 7, a.y + 4), col::dim, n);
        if (full) {
            dl->AddText(g_bold, 16, ImVec2(a.x + 10, a.y + 22), sl.kind == PS_FUSION ? col::gold : col::text, slot_name(sl).c_str());
            char lv[8];
            snprintf(lv, sizeof(lv), "+%d", sl.lv);
            dl->AddText(g_bold, 14, ImVec2(b.x - 8 - text_w(g_bold, 14, lv), a.y + 4), col::gold, lv);
        } else {
            dl->AddText(g_body, 14, ImVec2(a.x + (cw - text_w(g_body, 14, "empty")) * 0.5f, a.y + 24), col::dim, "empty");
        }
        dl->PopClipRect();
    }
    ImGui::Dummy(ImVec2(1, 64));
}

// One of the three level-up cards: the same height and weight, the choice pinned at the foot.
static void offer_card(const SaveData::Prism& p, const SaveData::PSlot& o) {
    ImGui::PushFont(g_body, 16.0f);
    switch (o.kind) {
        case PS_FRAGMENT:
            label("NEW FRAGMENT", col::gold);
            ImGui::TextWrapped("%s  +1", text_or(&kFragments[o.row].name).c_str());
            effect_lines(kFragments[o.row].effects);
            break;
        case PS_RAISE: {
            const SaveData::PSlot& sl = p.slot[o.row];
            label("STRENGTHEN", col::gold);
            ImGui::TextWrapped("%s  +%d -> +%d", slot_name(sl).c_str(), sl.lv, sl.lv + 1);
            slot_effects(sl);
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
    ImGui::PopFont();
}

static void screen_prism(const SaveData& s) {
    if (!begin_screen("##prism", 0.82f, 0.86f)) return end_screen();
    breadcrumb("Ward 13", "Prisms");
    zenny_plate();
    section_row(4);
    float w = ImGui::GetContentRegionAvail().x;
    panel_begin("owned", "PRISMS", ImVec2(w * 0.28f, -kFoot));
    bool any = false;
    for (int i = 0; i < kPrismsCount; i++) {
        if (!s.has(CAT_PRISM, i)) continue;
        any = true;
        if (g_prismPick < 0 || !s.has(CAT_PRISM, g_prismPick)) g_prismPick = s.prismOn >= 0 ? s.prismOn : i;
        ImGui::PushID(i);
        bool sel = g_prismPick == i;
        if (list_row("p", 52, sel)) g_prismPick = i;
        const SaveData::Prism& p = s.prism[i];
        put(g_body, 18, ImVec2(g_row.a.x + kRowPad, g_row.a.y + 6), sel ? col::gold : col::text, text_or(&kPrisms[i].name).c_str());
        char lv[16];
        snprintf(lv, sizeof(lv), "+%d", prism_level(p));
        float x = put(g_body, 14, ImVec2(g_row.a.x + kRowPad, g_row.a.y + 29), col::dim, lv);
        if (s.prismOn == i) x = put(g_body, 14, ImVec2(x, g_row.a.y + 29), col::dim, "  |  equipped");
        if (p.offers > 0) put(g_bold, 14, ImVec2(x, g_row.a.y + 29), col::accent, "  |  LEVEL UP");
        end_list_row();
        ImGui::PopID();
    }
    if (!any)
        dim_wrapped(("No prism yet. " + r2_name(kNpcs[host_of_role(MENUS_ROLE_MELDER)].name) +
                     " crafts them, and the first Remnant boss you defeat leaves one.").c_str());
    panel_end();
    ImGui::SameLine(0, 20);
    panel_begin("detail", "PRISM INFO", ImVec2(0, -kFoot));
    if (any && g_prismPick >= 0) {
        int row = g_prismPick;
        const SaveData::Prism& p = s.prism[row];
        int lv = prism_level(p);
        double need = prism_need(p);
        title(upper(text_or(&kPrisms[row].name)).c_str());
        float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        ImVec2 l0 = ImGui::GetCursorScreenPos();
        ImGui::TextColored(ImColor(col::gold).Value, "+%d of 51", lv);
        help_mark("The equipped prism gains experience as you hunt. Each prism level offers three choices: a new relic "
                  "fragment for one of its five slots, one more level for a fragment (up to +10), or a fusion of two "
                  "fragments that are both +5 or more. With all five at +10 it offers a legendary bonus. Fragments show "
                  "their +10 values.");
        if (need > 0) {
            char xp[64];
            snprintf(xp, sizeof(xp), "%.0f / %.0f experience", std::min(p.xp, need), need);
            put(g_body, 18, ImVec2(right - text_w(g_body, 18, xp), l0.y), col::dim, xp);
            bar((float)(p.xp / need), col::accent);
        }
        if (s.prismOn != row) {
            ImGui::Spacing();
            if (remnant_button("EQUIP THIS PRISM", ImVec2(260, kBtnSecondary)))
                change_save([row](SaveData& d) { d.prismOn = row; prism_offer(d.prism[row]); return true; });
        } else if (need > 0 && p.offers == 0) {
            dim_wrapped("Equipped: it gains experience as you hunt.");
        }
        if (p.offers > 0) {
            section("LEVEL UP: CHOOSE ONE", col::gold);
            float cw = (ImGui::GetContentRegionAvail().x - 24) / 3.0f;
            for (int k = 0; k < p.offers; k++) {
                ImGui::PushID(300 + k);
                if (k) ImGui::SameLine(0, 12);
                ImVec2 a = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + cw, a.y + 190), IM_COL32(26, 24, 20, 255));
                ImGui::GetWindowDrawList()->AddRect(a, ImVec2(a.x + cw, a.y + 190), col::line);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
                ImGui::BeginChild("o", ImVec2(cw, 190), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
                ImGui::PopStyleVar();
                offer_card(p, p.offer[k]);
                ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 10 - kBtnSecondary);
                if (remnant_button("CHOOSE", ImVec2(-1, kBtnSecondary))) {
                    change_save([row, k](SaveData& d) { return prism_pick(d, row, k); });
                    ui_sound("socket");
                }
                ImGui::EndChild();
                ImGui::PopID();
            }
        }
        section("FRAGMENTS");
        fragment_cells(p);
        ImGui::Spacing();
        for (int i = 0; i < 5; i++) slot_line(p.slot[i], i);
        if (p.legendary >= 0) {
            section("LEGENDARY", col::gold);
            ImGui::TextUnformatted(text_or(&kLegendaries[p.legendary].name).c_str());
            if (kLegendaries[p.legendary].desc.asset) dim_wrapped(text_or(&kLegendaries[p.legendary].desc).c_str());
            effect_lines(kLegendaries[p.legendary].effects);
        }
    } else {
        empty_hint("Select a prism to see its fragments");
    }
    panel_end();
    if (any && g_prismPick >= 0) {
        bool armed = now_s() - g_cleanseArmed < 4.0;
        int row = g_prismPick;
        foot_at(kPadX, kBtnSecondary);
        if (remnant_button(armed ? "CLICK AGAIN TO CLEANSE" : "CLEANSE (from +10)", ImVec2(300, kBtnSecondary), prism_level(s.prism[row]) >= 10)) {
            if (armed) {
                change_save([row](SaveData& d) { return prism_cleanse(d, row); });
                g_cleanseArmed = 0;
            } else {
                g_cleanseArmed = now_s();
            }
        }
    }
    if (foot_back()) open(SCR_TERMINAL);
    foot_hint(any && g_prismPick >= 0 && s.prism[g_prismPick].offers > 0 ? "Choose one of the three offers."
                                                                          : "The equipped prism gains experience as you hunt.");
    end_screen();
}

// ---- archetypes

static void screen_archetype(const SaveData& s) {
    if (!begin_screen("##archetype", 0.84f, 0.86f, s.archetype >= 0)) return end_screen();
    breadcrumb("Ward 13", "Archetypes");
    zenny_plate();
    if (s.archetype >= 0) section_row(2);
    std::vector<int> rows;
    for (int i = 0; i < kArchetypesCount; i++)
        if (!(s.archetype < 0 && kArchetypes[i].zenny > 0)) rows.push_back(i);  // first choice: the starting four
    bool picked = false;
    for (int i : rows) picked |= g_pick.cat == CAT_ARCHETYPE && g_pick.row == i;
    if (!picked && !rows.empty()) g_pick = item(CAT_ARCHETYPE, rows[0]);
    float w = ImGui::GetContentRegionAvail().x;
    panel_begin("list", "ARCHETYPES", ImVec2(w * 0.30f, -kFoot));
    for (int i : rows) {
        const Archetypes& a = kArchetypes[i];
        bool own = s.has(CAT_ARCHETYPE, i) || a.zenny <= 0;
        ImGui::PushID(i);
        bool sel = g_pick.cat == CAT_ARCHETYPE && g_pick.row == i;
        if (list_row("a", 58, sel)) g_pick = item(CAT_ARCHETYPE, i);
        row_at(kRowPad, 7);
        image(a.emblem, 128, ImVec2(44, 44), own ? IM_COL32_WHITE : IM_COL32(120, 115, 108, 255));
        float x = g_row.a.x + kRowPad + 54;
        put(g_body, 18, ImVec2(x, g_row.a.y + 8), sel ? col::gold : own ? col::text : col::dim, upper(text_or(&a.name)).c_str());
        const char* st = i == s.archetype ? "PRIMARY" : i == s.archetype2 ? "SECOND" : own ? "AVAILABLE" : "ENGRAM AT THE ELDER MELDER";
        put(g_body, 13, ImVec2(x, g_row.a.y + 33), col::dim, st);
        end_list_row();
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextColored(ImColor(col::dim).Value, "Starting archetypes are free.");
    help_mark("Your archetype brings its skills, perks and trait. Starting archetypes are free; the others are engrams the "
              "Elder Melder's Remnant guest crafts from MHW materials. A second archetype adds its perks and a second skill.");
    panel_end();
    ImGui::SameLine(0, 20);
    panel_begin("detail", "ARCHETYPE INFO", ImVec2(0, -kFoot));
    if (g_pick.cat == CAT_ARCHETYPE && g_pick.row >= 0) {
        const Archetypes& a = kArchetypes[g_pick.row];
        image(a.emblem, 256, ImVec2(96, 96));
        ImGui::SameLine();
        ImGui::BeginGroup();
        title(upper(text_or(&a.name)).c_str());
        ImGui::TextWrapped("%s", text_or(&a.desc).c_str());
        ImGui::EndGroup();
        ImGui::Spacing();
        archetype_detail(g_pick.row, s, true);
    } else {
        empty_hint("Select an archetype to see its details");
    }
    panel_end();
    float nx = kPadX;
    if (g_pick.cat == CAT_ARCHETYPE && g_pick.row >= 0) {
        const Archetypes& a = kArchetypes[g_pick.row];
        bool own = s.has(CAT_ARCHETYPE, g_pick.row) || a.zenny <= 0;
        foot_at(kPadX, kBtnPrimary);
        if (remnant_button(s.archetype < 0 ? "CHOOSE" : "MAKE PRIMARY", ImVec2(220, kBtnPrimary), own && g_pick.row != s.archetype)) {
            bool first = s.archetype < 0;
            choose_archetype(g_pick.row, 0);
            if (first) open(SCR_NONE);
        }
        nx = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + 10;
        if (s.archetype >= 0) {
            foot_at(nx, kBtnSecondary);
            if (remnant_button("MAKE SECOND", ImVec2(220, kBtnSecondary), own && g_pick.row != s.archetype && g_pick.row != s.archetype2))
                choose_archetype(g_pick.row, 1);
        }
    }
    if (s.archetype >= 0 && foot_back()) open(SCR_TERMINAL);
    foot_hint(s.archetype < 0 ? "Choose your first archetype." : "Choose a primary and a second archetype.");
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

// ---------------------------------------------------------------- item-obtained notices
// MHW's "item obtained" stack: one dark plate per row at the right edge, mid-height, the icon on the left, the name, and
// "x N" on the right. Rows slide in from the right, hold, then fade; the newest is at the bottom. The game thread adds
// rows (loot_notify), the render thread draws them, so the list is behind a mutex.
namespace {
struct LootRow {
    std::string name, icon;
    int count = 1, mhw = 0;
    double t0 = 0;
};
std::mutex g_lootMx;
std::vector<LootRow> g_loot;
constexpr double kLootIn = 0.28, kLootHold = 4.0, kLootOut = 0.6;
constexpr size_t kLootMax = 5;

void loot_push(const char* name, int count, const char* icon, int mhw, double t0) {
    if (!name || !name[0] || count <= 0) return;
    std::lock_guard<std::mutex> lk(g_lootMx);
    for (LootRow& r : g_loot)  // the same item again while its row is up: one row, the count added
        if (r.name == name && r.mhw == mhw && now_s() - r.t0 < kLootIn + kLootHold) {
            r.count += count;
            r.t0 = t0;
            return;
        }
    g_loot.push_back({name, icon ? icon : "", count, mhw, t0});
    if (g_loot.size() > kLootMax) g_loot.erase(g_loot.begin(), g_loot.end() - kLootMax);
}

ImU32 with_alpha(ImU32 c, float a) {
    return (c & 0x00FFFFFF) | ((ImU32)(((c >> 24) & 255) * std::max(0.0f, std::min(1.0f, a))) << 24);
}
}  // namespace

void loot_notify(const char* name, int count, const char* r2icon, int mhwItemId) {
    loot_push(name, count, r2icon, mhwItemId, now_s());
}

// MHW has no icon we can read at run time: a material is a small rounded plate with a rarity-coloured gem on it, a
// scrap (no icon, no item id) a plain cog.
static void loot_glyph(ImDrawList* fg, ImVec2 a, float sz, int mhw, float al) {
    ImVec2 c(a.x + sz / 2, a.y + sz / 2);
    fg->AddRectFilled(a, ImVec2(a.x + sz, a.y + sz), with_alpha(IM_COL32(28, 32, 38, 235), al), 4.0f);
    fg->AddRect(a, ImVec2(a.x + sz, a.y + sz), with_alpha(col::frameLo, al), 4.0f, 0, 1.0f);
    ImU32 rc = mhw > 0 ? rarity_color(1 + (mhw % 5)) : IM_COL32(176, 168, 150, 255);
    float r = sz * 0.3f;
    if (mhw > 0) {  // a cut gem: a pentagon with a lighter crown
        ImVec2 pt[5] = {{c.x - r, c.y - r * 0.35f}, {c.x - r * 0.55f, c.y - r}, {c.x + r * 0.55f, c.y - r},
                        {c.x + r, c.y - r * 0.35f}, {c.x, c.y + r}};
        fg->AddConvexPolyFilled(pt, 5, with_alpha(rc, al));
        ImVec2 top[4] = {pt[0], pt[1], pt[2], pt[3]};
        fg->AddConvexPolyFilled(top, 4, with_alpha(IM_COL32(255, 255, 255, 90), al));
        fg->AddPolyline(pt, 5, with_alpha(IM_COL32(20, 20, 24, 200), al), ImDrawFlags_Closed, 1.0f);
        fg->AddLine(pt[0], pt[3], with_alpha(IM_COL32(20, 20, 24, 160), al), 1.0f);
    } else {  // a cog
        fg->AddCircleFilled(c, r, with_alpha(rc, al), 8);
        fg->AddCircleFilled(c, r * 0.4f, with_alpha(IM_COL32(28, 32, 38, 255), al), 12);
    }
}

static void loot_draw(ImDrawList* fg, ImGuiViewport* vp, float sc) {
    double t = now_s();
    std::vector<LootRow> rows;
    {
        std::lock_guard<std::mutex> lk(g_lootMx);
        g_loot.erase(std::remove_if(g_loot.begin(), g_loot.end(),
                                    [&](const LootRow& r) { return t - r.t0 > kLootIn + kLootHold + kLootOut; }),
                     g_loot.end());
        rows = g_loot;
    }
    if (rows.empty()) return;
    // Right-aligned, 24 px in from the edge, ending at 55% of the height: above the gun panel, clear of the toast.
    float rw = 300 * sc, rh = 44 * sc, gap = 4 * sc, ih = 36 * sc;
    float xr = vp->Pos.x + vp->Size.x - 24 * sc;
    float yb = vp->Pos.y + vp->Size.y * 0.55f;
    for (size_t i = 0; i < rows.size(); i++) {
        const LootRow& r = rows[i];
        double age = t - r.t0;
        float k = (float)std::max(0.0, std::min(1.0, age / kLootIn));
        float ease = 1 - (1 - k) * (1 - k);
        float al = ease * (age > kLootIn + kLootHold ? (float)(1.0 - (age - kLootIn - kLootHold) / kLootOut) : 1.0f);
        float x1 = xr + (1 - ease) * 70 * sc, x0 = x1 - rw;
        float y1 = yb - (float)(rows.size() - 1 - i) * (rh + gap), y0 = y1 - rh;
        fg->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), with_alpha(col::mhwFrame, al));
        fg->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), with_alpha(col::mhwGold, al), 0, 0, 1.0f);
        ImVec2 ia(x0 + 6 * sc, y0 + (rh - ih) / 2);
        int tw = 0, th = 0;
        void* tex = r.icon.empty() ? nullptr : texture_for(r.icon.c_str(), 128, &tw, &th);
        if (tex)
            fg->AddImage((ImTextureID)(uintptr_t)tex, ia, ImVec2(ia.x + ih, ia.y + ih), ImVec2(0, 0), ImVec2(1, 1),
                         with_alpha(IM_COL32_WHITE, al));
        else
            loot_glyph(fg, ia, ih, r.mhw, al);
        char cnt[16];
        snprintf(cnt, sizeof(cnt), "x %d", r.count);
        float cs = 18 * sc;
        ImVec2 cz = g_bold->CalcTextSizeA(cs, FLT_MAX, 0, cnt);
        fg->AddText(g_bold, cs, ImVec2(x1 - 12 * sc - cz.x, y0 + (rh - cz.y) / 2), with_alpha(col::mhwText, al), cnt);
        float nx = ia.x + ih + 10 * sc, fs = 16 * sc;
        ImVec2 nz = g_body->CalcTextSizeA(fs, FLT_MAX, 0, r.name.c_str());
        ImVec4 clip(nx, y0, x1 - 20 * sc - cz.x, y1);
        fg->AddText(g_body, fs, ImVec2(nx, y0 + (rh - nz.y) / 2), with_alpha(col::mhwText, al), r.name.c_str(), nullptr,
                    0.0f, &clip);
    }
}

static void hud(const SaveData& s, const Live& l) {
    if (!archetype() || !l.snap.inGame) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    double t = now_s();
    float sc = vp->Size.y / 1080.0f;
    // Toast: a dark band in a thin gold frame, a little below the top.
    if (t < l.toastUntil && l.toast[0]) {
        float fs = 18 * sc;
        ImVec2 ts = g_body->CalcTextSizeA(fs, FLT_MAX, 0, l.toast);
        ImVec2 p(vp->Pos.x + (vp->Size.x - ts.x) / 2, vp->Pos.y + vp->Size.y * 0.16f);
        ImVec2 a(p.x - 22 * sc, p.y - 10 * sc), b(p.x + ts.x + 22 * sc, p.y + ts.y + 10 * sc);
        fg->AddRectFilled(a, b, col::mhwFrame);
        fg->AddRect(a, b, col::mhwGold, 0, 0, 1.0f);
        fg->AddText(g_body, fs, p, col::mhwText, l.toast);
    }
    loot_draw(fg, vp, sc);
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
    // A standing prompt in town, so the Remnant menu is findable without standing at an NPC.
    if (l.snap.inBase && g_screen == SCR_NONE && l.nearMenu < 0 && s.archetype >= 0) {
        char b[160];
        snprintf(b, sizeof(b), "[%c]  Remnant gear: equipment, items, archetypes, traits, prisms", (char)keys().terminal);
        ImVec2 ts = g_bold->CalcTextSizeA(18 * sc, FLT_MAX, 0, b);
        ImVec2 p(vp->Pos.x + vp->Size.x - 40 * sc - 20 - ts.x, vp->Pos.y + vp->Size.y - 190 * sc);
        fg->AddRectFilled(ImVec2(p.x - 20, p.y - 10), ImVec2(p.x + ts.x + 20, p.y + ts.y + 10), col::bg);
        fg->AddRectFilled(ImVec2(p.x - 20, p.y - 10), ImVec2(p.x - 16, p.y + ts.y + 10), col::accent);
        fg->AddText(g_bold, 18 * sc, p, col::text, b);
    }
    // Legend of the proxy items in MHW's item bar: which MHW item stands for which Remnant item.
    if (g_screen == SCR_NONE && s.archetype >= 0 && bar_proxies()) {
        struct Line { std::string text; const char* icon; };
        std::vector<Line> lines;
        std::vector<int> carried;
        for (int i = 0; i < kConsumablesCount && carried.size() < 4; i++)
            if (s.stock[i] > 0) carried.push_back(i);
        for (int b = 0; b < kBar_itemsCount; b++) {
            int id = (int)kBar_items[b].mhw_item_id;
            int n = id > 0 ? mhw_pouch_count(id) : 0;
            if (n <= 0) continue;
            std::string rn;
            const char* icon = nullptr;
            if (b == BAR_ITEMS_DRAGON_HEART && s.relic >= 0) { rn = r2_text(kRelics[s.relic].name); icon = kRelics[s.relic].icon; }
            else if (b == BAR_ITEMS_ARCHETYPE_SKILL) {
                std::vector<int> sk = skills_of(s.archetype);
                if (!sk.empty()) { const Skills& k = kSkills[sk[skill_index(s, 0)]]; rn = r2_text(k.name); icon = k.icon; }
            } else if (b == BAR_ITEMS_LONG_GUN && s.longGun >= 0) { rn = r2_text(kWeapons[s.longGun].name); icon = kWeapons[s.longGun].icon; }
            else if (b == BAR_ITEMS_HAND_GUN && s.handGun >= 0) { rn = r2_text(kWeapons[s.handGun].name); icon = kWeapons[s.handGun].icon; }
            else if (b == BAR_ITEMS_WEAPON_MOD) {
                int md = gun_mod(s, equipped_gun(s, l) >= 0 ? equipped_gun(s, l) : s.longGun);
                if (md >= 0) { rn = r2_text(kMods[md].name); icon = kMods[md].icon; }
            } else if (b >= BAR_ITEMS_CONSUMABLE_1) {
                int k = b - BAR_ITEMS_CONSUMABLE_1;
                if (k < (int)carried.size()) { rn = r2_text(kConsumables[carried[k]].name); icon = kConsumables[carried[k]].icon; }
            }
            if (rn.empty()) continue;
            const char* mn = mhw_item_name(id);
            char buf[200];
            snprintf(buf, sizeof(buf), "%s  =  %s x%d", mn ? mn : kBar_items[b].what, rn.c_str(), n);
            lines.push_back({buf, icon});
        }
        if (!lines.empty()) {
            float fs = 15 * sc, ih = 20 * sc, lh = 24 * sc, w = 0;
            for (const Line& ln : lines) w = std::max(w, g_body->CalcTextSizeA(fs, FLT_MAX, 0, ln.text.c_str()).x);
            w += ih + 8 * sc;
            ImVec2 pe(vp->Pos.x + vp->Size.x - 40 * sc, vp->Pos.y + vp->Size.y - 230 * sc);
            ImVec2 p(pe.x - w - 24 * sc, pe.y - lines.size() * lh - 12 * sc);
            fg->AddRectFilled(p, pe, col::mhwFrame);
            fg->AddRect(p, pe, col::mhwGold, 0, 0, 1.0f);
            fg->AddRectFilled(p, ImVec2(p.x + 3 * sc, pe.y), col::accent);
            float y = p.y + 6 * sc;
            for (const Line& ln : lines) {
                int tw = 0, th = 0;
                if (ln.icon)
                    if (void* tex = texture_for(ln.icon, 128, &tw, &th))
                        fg->AddImage((ImTextureID)(uintptr_t)tex, ImVec2(p.x + 12 * sc, y), ImVec2(p.x + 12 * sc + ih, y + ih));
                fg->AddText(g_body, fs, ImVec2(p.x + 12 * sc + ih + 8 * sc, y + (ih - fs) / 2), IM_COL32(200, 190, 170, 200), ln.text.c_str());
                y += lh;
            }
        }
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
    // Gun panel, raised and right of centre so MHW's centre prompts ("Change or organize your equipment.") stay readable.
    ImVec2 size(360 * sc, 120 * sc);
    ImVec2 p(vp->Pos.x + (vp->Size.x - size.x) / 2 + vp->Size.x * 0.12f, vp->Pos.y + vp->Size.y - size.y - 150 * sc);
    ImVec2 pe(p.x + size.x, p.y + size.y);
    fg->AddRectFilled(p, pe, col::mhwFrame);
    fg->AddRect(p, pe, col::mhwGold, 0, 0, 1.5f);
    fg->AddRectFilled(p, ImVec2(p.x + 3 * sc, pe.y), col::accent);
    float cx = p.x + 190 * sc;  // the right column: ammo, reload
    if (!l.gunMode) {
        char hint[64];
        snprintf(hint, sizeof(hint), "[%c] Remnant guns", (char)keys().gun);
        fg->AddText(g_body, 14 * sc, ImVec2(cx, p.y + 96 * sc), col::mhwText, hint);
    }
    if (gun >= 0) {
        int w = 0, h = 0;
        void* tex = texture_for(kWeapons[gun].icon, 256, &w, &h);
        if (tex && w) {
            float k = std::min(160 * sc / w, 56 * sc / h);
            ImVec2 a0(p.x + 16 * sc, p.y + 10 * sc);
            fg->AddImage((ImTextureID)(uintptr_t)tex, a0, ImVec2(a0.x + w * k, a0.y + h * k));
        }
        fg->AddText(g_bold, 16 * sc, ImVec2(p.x + 16 * sc, p.y + 70 * sc), col::mhwText, r2_text(kWeapons[gun].name).c_str());
        char ammo[32];
        snprintf(ammo, sizeof(ammo), "%d", l.mag[gun]);
        fg->AddText(g_title, 40 * sc, ImVec2(cx, p.y + 8 * sc), col::mhwText, ammo);
        float aw = g_title->CalcTextSizeA(40 * sc, FLT_MAX, 0, ammo).x;
        snprintf(ammo, sizeof(ammo), "/ %d", l.reserve[gun]);
        fg->AddText(g_body, 18 * sc, ImVec2(cx + aw + 10 * sc, p.y + 30 * sc), col::mhwGold, ammo);
        if (l.reloadDone > 0) {
            float k = (float)std::min(1.0, std::max(0.0, 1.0 - (l.reloadDone - t) / std::max(0.1f, kWeapons[gun].reload_s)));
            ImVec2 b0(cx, p.y + 72 * sc);
            fg->AddRectFilled(b0, ImVec2(b0.x + 140 * sc, b0.y + 5 * sc), IM_COL32(60, 52, 40, 255));
            fg->AddRectFilled(b0, ImVec2(b0.x + 140 * sc * k, b0.y + 5 * sc), col::mhwGold);
            fg->AddText(g_bold, 13 * sc, ImVec2(b0.x, b0.y - 18 * sc), col::mhwText, "RELOADING");
        }
    }
    // Mod meter, under the gun's name.
    int mod = gun_mod(s, gun);
    if (mod >= 0) {
        ImVec2 m0(p.x + 46 * sc, p.y + 98 * sc);
        float k = t < l.modUntil ? 1.0f : std::min(1.0f, l.modCharge / std::max(1.0f, kMods[mod].charge_damage));
        fg->AddRectFilled(m0, ImVec2(m0.x + 140 * sc, m0.y + 6 * sc), IM_COL32(60, 52, 40, 255));
        fg->AddRectFilled(m0, ImVec2(m0.x + 140 * sc * k, m0.y + 6 * sc), k >= 1 ? col::accent : col::mhwGold);
        int w = 0, h = 0;
        if (void* tex = texture_for(kMods[mod].icon, 128, &w, &h))
            fg->AddImage((ImTextureID)(uintptr_t)tex, ImVec2(p.x + 16 * sc, m0.y - 8 * sc), ImVec2(p.x + 38 * sc, m0.y + 14 * sc));
    }
    // Skills and relic, bottom left above MHW's own panels: 60 px boxes, the key under each, seconds inside.
    ImVec2 q(vp->Pos.x + 40 * sc, vp->Pos.y + vp->Size.y - 300 * sc);
    auto icon_box = [&](const char* asset, ImVec2 at, float frac, const char* key, const char* inside, ImU32 insideCol) {
        float bs = 60 * sc;
        ImVec2 e(at.x + bs, at.y + bs);
        fg->AddRectFilled(at, e, col::mhwFrame);
        int w = 0, h = 0;
        if (void* tex = texture_for(asset, 128, &w, &h)) fg->AddImage((ImTextureID)(uintptr_t)tex, at, e);
        if (frac < 1) fg->AddRectFilled(at, ImVec2(e.x, at.y + bs * (1 - frac)), IM_COL32(0, 0, 0, 170));
        fg->AddRect(at, e, col::mhwGold, 0, 0, 1.5f);
        if (inside && inside[0]) {
            float fs = 16 * sc;
            ImVec2 ts = g_bold->CalcTextSizeA(fs, FLT_MAX, 0, inside);
            fg->AddText(g_bold, fs, ImVec2(at.x + (bs - ts.x) / 2, at.y + (bs - ts.y) / 2), insideCol, inside);
        }
        float fs = 14 * sc;
        ImVec2 ks = g_bold->CalcTextSizeA(fs, FLT_MAX, 0, key);
        fg->AddText(g_bold, fs, ImVec2(at.x + (bs - ks.x) / 2, e.y + 4 * sc), col::mhwText, key);
    };
    char tag[48], key[16], in[24];
    float x = q.x;
    for (int slot = 0; slot < 2; slot++) {
        int ar = slot == 0 ? s.archetype : s.archetype2;
        std::vector<int> sk = skills_of(ar);
        if (sk.empty()) continue;
        const Skills& k = kSkills[sk[skill_index(s, slot)]];
        float cd = std::max(1.0f, k.cooldown_s);
        float sf = t >= l.skillReady[slot] ? 1.0f : (float)std::max(0.0, 1.0 - (l.skillReady[slot] - t) / cd);
        snprintf(key, sizeof(key), "[%c]", (char)(slot == 0 ? keys().skill : keys().skill2));
        in[0] = 0;
        ImU32 ic = col::mhwText;
        if (t < l.skillUntil[slot]) { snprintf(in, sizeof(in), "%.0fs", l.skillUntil[slot] - t); ic = col::accent; }
        else if (t < l.skillReady[slot]) snprintf(in, sizeof(in), "%.0fs", l.skillReady[slot] - t);
        icon_box(k.icon, ImVec2(x, q.y), sf, key, in, ic);
        x += 80 * sc;
    }
    if (s.relic >= 0) {
        snprintf(key, sizeof(key), "[%c]", (char)keys().relic);
        snprintf(tag, sizeof(tag), "x%d", s.relicCharges);
        icon_box(kRelics[s.relic].icon, ImVec2(x, q.y), s.relicCharges > 0 ? 1.0f : 0.0f, key, tag, col::mhwGold);
    }
    if (l.bonusMax > 0 || l.bonusHealth > 0) {
        float mx = std::max(l.bonusMax, l.bonusHealth);
        ImVec2 b0(q.x, q.y - 16 * sc);
        fg->AddRectFilled(b0, ImVec2(b0.x + 224 * sc, b0.y + 6 * sc), IM_COL32(60, 52, 40, 255));
        fg->AddRectFilled(b0, ImVec2(b0.x + 224 * sc * (l.bonusHealth / std::max(1.0f, mx)), b0.y + 6 * sc), IM_COL32(120, 190, 210, 255));
    }
}

// ---------------------------------------------------------------- the dock in MHW's Item Box > Change Equipment screen
// The mod cannot add rows to Capcom's list, so while that menu is open in town a Remnant panel sits under it (left
// column, below the armor rows): the Remnant equipment at a glance, a click opens the matching Remnant screen.
// "Menu open" is the [menu] flag byte when the ini names one, else the input-rate heuristic: MHW stops polling
// gameplay input while a menu is open, so the reads per second fall below 20% of their running maximum for 0.5 s.
static std::atomic<bool> g_menuHeur{false};
bool menu_heuristic_open() { return g_menuHeur.load(); }
static bool g_dockShown = false, g_dockHover = false;
static bool g_dockManual = false;  // opened by hand with 9 (no menu flag known)

static bool dock_enabled() {
    static int on = -1;
    if (on < 0) {
        char ini[MAX_PATH];
        snprintf(ini, sizeof(ini), "%s\\remnant_hunt.ini", mod_dir());
        on = GetPrivateProfileIntA("menu", "dock", 1, ini) != 0;
    }
    return on == 1;
}

// eligible: in town, settled, not loading. Called every frame (keeps the rate window moving while a screen is open).
static bool menu_open_signal(bool eligible, double now) {
    static double belowSince = 0;
    static int maxRate = 0;
    int rate = input_reads_per_second();
    HWND fg = GetForegroundWindow();  // an alt-tab also silences the input
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    bool mine = pid == GetCurrentProcessId();
    if (g_screen != SCR_NONE) return g_menuHeur.load();  // our own screen is up: keep what we knew
    if (!eligible || !mine) { belowSince = 0; g_menuHeur = false; return false; }
    if (rate > maxRate) maxRate = rate;
    bool low = maxRate >= 20 && rate < maxRate * 0.2f;
    if (!low) {
        belowSince = 0;
        g_menuHeur = false;
    } else {
        if (belowSince == 0) belowSince = now;
        if (now - belowSince >= 0.5) g_menuHeur = true;
    }
    return g_menuHeur.load();
}

static int slot_index(const char* label) {
    for (int i = 0; i < (int)(sizeof(kSlots) / sizeof(kSlots[0])); i++)
        if (strcmp(kSlots[i].label, label) == 0) return i;
    return 0;
}

// A dock row opens the terminal on a slot (>= 0), or another screen.
static void dock_go(Screen sc, int slot) {
    open(sc);
    g_slotPick = slot;
    g_fromDock = true;
}

static std::string slot_name(const SaveData& s, int i) {
    int row = slot_row(s, kSlots[i]);
    if (row < 0) return "";
    return text_or(item(kSlots[i].cat, row).name);
}

// Draws the dock; returns whether the pointer is over it.
static bool dock_draw(const SaveData& s) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float W = vp->Size.x, H = vp->Size.y;
    ImVec2 pos(vp->Pos.x + W * 0.10f, vp->Pos.y + H * 0.52f), size(W * 0.36f, H * 0.38f);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##dock", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);
    const float kHint = 26;
    panel_begin("##dockpanel", "REMNANT EQUIPMENT", ImVec2(0, -kHint));
    struct Row { const char* label; Screen sc; };
    static const Row kRows[] = {{"LONG GUN", SCR_TERMINAL}, {"HAND GUN", SCR_TERMINAL}, {"MELEE", SCR_TERMINAL},
                                {"HEAD", SCR_TERMINAL},     {"BODY", SCR_TERMINAL},     {"GLOVES", SCR_TERMINAL},
                                {"LEGS", SCR_TERMINAL},     {"RELIC", SCR_TERMINAL},    {"AMULET", SCR_TERMINAL},
                                {"RINGS", SCR_TERMINAL},    {"ARCHETYPES", SCR_ARCHETYPE}, {"TRAITS", SCR_TRAITS}};
    const int n = (int)(sizeof(kRows) / sizeof(kRows[0]));
    float rowH = std::min(30.0f, std::floor(ImGui::GetContentRegionAvail().y / n));
    float fs = rowH >= 26 ? 16.0f : 14.0f;
    for (int i = 0; i < n; i++) {
        const Row& r = kRows[i];
        bool rings = strcmp(r.label, "RINGS") == 0;
        int slot = r.sc == SCR_TERMINAL ? slot_index(rings ? "RING 1" : r.label) : -1;
        std::string txt;
        ImU32 tc = col::text;
        if (rings) {
            for (const char* rl : {"RING 1", "RING 2", "RING 3", "RING 4"}) {
                std::string rn = slot_name(s, slot_index(rl));
                if (!rn.empty()) txt += (txt.empty() ? "" : ", ") + rn;
            }
        } else if (r.sc == SCR_TERMINAL) {
            txt = slot_name(s, slot);
            int row = slot_row(s, kSlots[slot]);
            if (kSlots[slot].cat == CAT_WEAPON && row >= 0 && s.level[row]) txt += " +" + std::to_string(s.level[row]);
        } else if (r.sc == SCR_ARCHETYPE) {
            char b[160];
            snprintf(b, sizeof(b), "%s Lv %d", text_or(&kArchetypes[s.archetype].name).c_str(), archetype_level(s, s.archetype));
            txt = b;
            if (s.archetype2 >= 0) {
                snprintf(b, sizeof(b), " / %s Lv %d", text_or(&kArchetypes[s.archetype2].name).c_str(), archetype_level(s, s.archetype2));
                txt += b;
            }
        } else {
            int pts = 0;
            for (uint8_t t : s.trait) pts += t;
            txt = std::to_string(pts) + (pts == 1 ? " point spent" : " points spent");
        }
        if (txt.empty()) { txt = "empty"; tc = col::dim; }
        ImGui::PushID(i);
        if (list_row("d", rowH, false)) dock_go(r.sc, slot);
        float ty = g_row.a.y + (rowH - fs) * 0.5f - 1;
        put(g_bold, fs - 2, ImVec2(g_row.a.x + 12, ty + 1), col::dim, r.label);
        put(g_body, fs, ImVec2(g_row.a.x + 12 + 100, ty), tc, txt.c_str());
        end_list_row();
        ImGui::PopID();
    }
    panel_end();
    {  // the foot hint, in the same pill as the screens' foot line
        const char* hint = g_dockManual ? "[9] Close    click a row to edit" : "[9] Remnant equipment";
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float tw = text_w(g_body, 15, hint), w = tw + 44, x = pos.x + (size.x - w) * 0.5f, y = pos.y + size.y - 24;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + 22), IM_COL32(20, 18, 15, 225), 11.0f);
        dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + 22), col::line, 11.0f);
        dl->AddText(g_body, 15, ImVec2(x + 22, y + 2), col::dim, hint);
    }
    ImGui::End();
    return ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), false);
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
    bool aiming = sh.live.aiming;  // down the sights: V goes to MHW (Aim Slinger / clutch claw), not the weapon mod
    int nearMenu = sh.live.nearMenu;
    ReleaseSRWLockShared(&sh.lock);

    g_remnantScreenOpen = g_screen != SCR_NONE;  // for the game thread's item bar proxies (remnant.cpp)
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
    // MHW's Item Box / Change Equipment menu: the ini's flag byte when set, else the input-rate heuristic.
    bool heur = menu_open_signal(inGame && inBase && viewValid && baseSince > 0 && now_s() - baseSince > 4.0, now_s());
    int flagByte = menu_flag_read();
    // The input-rate heuristic flashes the dock in play (any pause in MHW's input polling trips it), so it only ever
    // shows the dock when the ini opts in ([menu] heuristic=1); otherwise the dock follows the flag byte, or the 9 key
    // toggles it by hand.
    static int heurOn = -1;
    if (heurOn < 0) {
        char ini[MAX_PATH];
        snprintf(ini, sizeof(ini), "%s\remnant_hunt.ini", mod_dir());
        heurOn = GetPrivateProfileIntA("menu", "heuristic", 0, ini) != 0;
    }
    if (inGame && g_screen == SCR_NONE && key_pressed('9') && !g_dockShown) g_dockManual = true;
    if (!inGame || !inBase || g_screen != SCR_NONE) g_dockManual = false;
    bool menuOpen = flagByte >= 0 ? flagByte != 0 : (heurOn && heur);
    bool dockOn = dock_enabled() && inGame && inBase && s.archetype >= 0 && (menuOpen || g_dockManual) && g_screen == SCR_NONE;
    if (g_screen == SCR_NONE && settled && !s.introDone && r2_ready()) open(SCR_INTRO);
    if (g_screen != SCR_NONE) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && g_fromDock && g_screen != SCR_INTRO) {
            open(SCR_NONE);  // back to the dock
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape) && g_screen == SCR_TERMINAL && g_slotPick >= 0) {
            g_slotPick = -1;
            g_pick = ItemView();
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape) && g_screen != SCR_INTRO && (g_screen != SCR_ARCHETYPE || s.archetype >= 0))
            open(g_screen == SCR_ARCHETYPE || g_screen == SCR_TRAITS || g_screen == SCR_PRISM ? SCR_TERMINAL : SCR_NONE);
        if (!inGame) open(SCR_NONE);
    } else if (inGame) {
        if (g_dockShown && dockOn && key_pressed('9')) {
            if (g_dockManual && flagByte < 0 && !(heurOn && heur)) g_dockManual = false;  // a hand-opened dock: 9 closes it
            else dock_go(SCR_TERMINAL, slot_index("LONG GUN"));
        }
        else if (key_pressed(k.terminal) && inBase && s.archetype >= 0) {
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
        if (key_pressed(k.mod) && !(gunMode && aiming)) post(CMD_MOD);
        if (gunMode && aiming && key_pressed(k.aimSlinger)) inject_claw(0.12);
        if (gunMode && key_pressed(k.reload)) post(CMD_RELOAD);
        if (gunMode && key_pressed(k.swap)) post(CMD_SWAP);
    }

    InputBlock b;
    if (g_screen != SCR_NONE) {
        b.keyboard = b.mouse = b.mouseButtons = true;
    } else if (inGame && s.archetype >= 0) {
        b.keys[k.skill] = b.keys[k.relic] = 1;
        if (!(gunMode && aiming)) b.keys[k.mod] = 1;
        if (s.archetype2 >= 0) b.keys[k.skill2] = 1;
        if (combat) b.keys[k.gun] = 1;
        if (inBase) b.keys[k.terminal] = 1;
        if (dockOn) {
            b.keys['9'] = 1;
            if (g_dockHover) b.mouseButtons = true;
        }
        if (gunMode) {
            if (aiming) b.mouseButton[0] = b.mouseButton[1] = 1;  // fire and aim stay with the gun; the claw button reaches MHW
            else b.mouseButtons = true;
            b.keys[k.reload] = b.keys[k.swap] = 1;
        }
    }
    set_input_block(b);
    ImGui::GetIO().MouseDrawCursor = g_screen != SCR_NONE || (dockOn && g_dockHover);

    if (g_screen == SCR_NONE) {
        AcquireSRWLockShared(&sh.lock);
        Live l = sh.live;
        ReleaseSRWLockShared(&sh.lock);
        hud(s, l);
        bosses_hud();
        g_dockShown = dockOn;
        g_dockHover = dockOn && dock_draw(s);
        return;
    }
    AcquireSRWLockShared(&sh.lock);
    Live l = sh.live;
    ReleaseSRWLockShared(&sh.lock);
    g_dockShown = g_dockHover = false;
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
        case 5: {  // the HUD, with an item-obtained stack: a Remnant item with its icon, an MHW material, a scrap
            static bool seeded = false;
            if (!seeded) {
                seeded = true;
                double t0 = now_s() - 1.0;
                if (kConsumablesCount > 0) loot_push(r2_text(kConsumables[0].name).c_str(), 2, kConsumables[0].icon, 0, t0);
                loot_push("Iron Ore", 3, nullptr, 2, t0);
                loot_push("Scrap", 14, nullptr, 0, t0);
            }
            hud(s, l);
            break;
        }
        case 6: g_slotPick = 0; screen_terminal(s, l); break;
        case 9: dock_draw(s); break;  // the dock under MHW's Item Box / Change Equipment screen
        case 7: g_menuRole = MENUS_ROLE_SMITHY; g_menu = -1; screen_menu(s, l); break;
        case 8: {  // a gun's diffuse map as the renderer sees it (virtual-texture stitching check)
            int w = weapons_index("huntmaster_m1");
            std::string all = w >= 0 ? kWeapons[w].mesh_tex : "";
            size_t eq = all.find('=');
            std::string tex = eq == std::string::npos ? "" : all.substr(eq + 1, all.find(',', eq) - eq - 1);
            ImGui::SetNextWindowPos(ImVec2(40, 40));
            ImGui::SetNextWindowSize(ImVec2(1100, 820));
            ImGui::Begin("##tex", nullptr, ImGuiWindowFlags_NoDecoration);
            ImGui::TextUnformatted(tex.c_str());
            image(tex.c_str(), 1024, ImVec2(768, 768));
            ImGui::End();
            break;
        }
        default: g_menuRole = MENUS_ROLE_MELDER; g_menu = -1; screen_menu(s, l); break;
    }
}
}  // namespace rh
#endif
