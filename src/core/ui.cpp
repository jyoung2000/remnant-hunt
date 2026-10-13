// Remnant x MHW mashup core: screens and HUD. MIT License (see LICENSE).
//
// Remnant-styled screens for Remnant tasks (the worldstone intro, archetypes, the Ward 13 terminal, Brabus and
// McCabe); a small HUD in MHW's colours for the guns, skill and relic. Names, descriptions, lines and icons are read
// from the player's own Remnant II install; MHW's own UI is left as it is.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "state.h"

namespace rh {

// ---------------------------------------------------------------- look

namespace col {
constexpr ImU32 bg = IM_COL32(14, 13, 12, 240), panel = IM_COL32(24, 22, 19, 245), line = IM_COL32(64, 58, 48, 255);
constexpr ImU32 text = IM_COL32(230, 223, 209, 255), dim = IM_COL32(154, 146, 132, 255);
constexpr ImU32 accent = IM_COL32(200, 88, 43, 255), gold = IM_COL32(209, 168, 94, 255);
constexpr ImU32 mhwFrame = IM_COL32(28, 23, 18, 205), mhwGold = IM_COL32(184, 154, 94, 255);
constexpr ImU32 mhwText = IM_COL32(242, 232, 207, 255), red = IM_COL32(214, 58, 46, 255);
}  // namespace col

static ImFont* g_body = nullptr;   // Montserrat Medium
static ImFont* g_bold = nullptr;   // Montserrat Bold
static ImFont* g_title = nullptr;  // EB Garamond Bold

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
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0;
    st.FrameRounding = 0;
    st.WindowBorderSize = 1;
    st.Colors[ImGuiCol_WindowBg] = ImColor(col::panel);
    st.Colors[ImGuiCol_Border] = ImColor(col::line);
    st.Colors[ImGuiCol_Text] = ImColor(col::text);
    st.Colors[ImGuiCol_TextDisabled] = ImColor(col::dim);
    st.Colors[ImGuiCol_Button] = ImVec4(0.12f, 0.11f, 0.10f, 1);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.42f, 0.18f, 0.09f, 1);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.78f, 0.34f, 0.17f, 1);
    st.Colors[ImGuiCol_Separator] = ImColor(col::line);
    st.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
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

static void label(const char* s, ImU32 c = col::dim) {
    ImGui::PushFont(g_bold, 14.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImColor(c).Value);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

static void quote(const std::string& s) {
    if (s.empty()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImColor(col::dim).Value);
    ImGui::TextWrapped("\"%s\"", s.c_str());
    ImGui::PopStyleColor();
}

static bool remnant_button(const char* text, ImVec2 size = ImVec2(0, 0), bool enabled = true) {
    if (!enabled) ImGui::BeginDisabled();
    ImGui::PushFont(g_bold, 18.0f);
    bool r = ImGui::Button(text, size);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + 3, b.y), ImGui::IsItemHovered() ? col::accent : col::line);
    ImGui::PopFont();
    if (!enabled) ImGui::EndDisabled();
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
    ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float L = 26;
    for (int i = 0; i < 4; i++) {
        float x = (i & 1) ? p.x + s.x - 1 : p.x, y = (i & 2) ? p.y + s.y - 1 : p.y;
        float dx = (i & 1) ? -L : L, dy = (i & 2) ? -L : L;
        dl->AddLine(ImVec2(x, y), ImVec2(x + dx, y), col::accent, 2);
        dl->AddLine(ImVec2(x, y), ImVec2(x, y + dy), col::accent, 2);
    }
    return open;
}
static void end_screen() {
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- screens

enum Screen { SCR_NONE, SCR_INTRO, SCR_ARCHETYPE, SCR_TERMINAL, SCR_VENDOR, SCR_WORKBENCH };
static Screen g_screen = SCR_NONE;
static double g_screenOpened = 0;
static int g_greet[kNpcsCount];
static std::string g_npcSays;
static int g_pick = -1;

static void open(Screen s) {
    g_screen = s;
    g_screenOpened = now_s();
    if (s == SCR_TERMINAL)
        for (int i = 0; i < kNpcsCount; i++) g_greet[i] = (int)(GetTickCount() / 7 + i) % 3;
    g_npcSays.clear();
    g_pick = -1;
}

static const sheets::R2Text& greet_of(const Npcs& n, int k) { return k == 0 ? n.greet_1 : k == 1 ? n.greet_2 : n.greet_3; }

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
    if (t > 3.0 && (remnant_button("CONTINUE", ImVec2(200, 40)) || ImGui::IsKeyPressed(ImGuiKey_Enter))) open(SCR_ARCHETYPE);
    end_screen();
}

static void screen_archetype(const SaveData& s) {
    if (!begin_screen("##archetype", 0.86f, 0.80f)) return end_screen();
    title("CHOOSE YOUR ARCHETYPE");
    label("Your archetype brings its skill, perks and starting guns into the New World. You can change it at the "
          "Ward 13 terminal.");
    ImGui::Separator();
    float colW = (ImGui::GetContentRegionAvail().x - 3 * 16) / 4;
    for (int i = 0; i < kArchetypesCount; i++) {
        const Archetypes& a = kArchetypes[i];
        if (i) ImGui::SameLine(0, 16);
        ImGui::BeginChild(a.id, ImVec2(colW, ImGui::GetContentRegionAvail().y - 4), ImGuiChildFlags_Borders);
        bool hover = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        ImGui::SetCursorPosX((colW - 150) / 2);
        image(a.emblem, 256, ImVec2(150, 150), hover || s.archetype == i ? IM_COL32_WHITE : IM_COL32(190, 185, 175, 255));
        title(upper(r2_text(a.name)).c_str(), s.archetype == i ? col::gold : col::text);
        ImGui::TextWrapped("%s", r2_text(a.desc).c_str());
        ImGui::Spacing();
        label("PRIME PERK");
        ImGui::TextWrapped("%s", r2_text(a.prime).c_str());
        ImGui::Spacing();
        label("SKILL");
        const Skills& sk = kSkills[a.skill];
        image(sk.icon, 128, ImVec2(36, 36));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(r2_text(sk.name).c_str());
        label("STARTING GUNS");
        for (int g : {a.long_gun, a.hand_gun}) {
            image(kWeapons[g].icon, 256, ImVec2(96, 40));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(r2_text(kWeapons[g].name).c_str());
        }
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 52);
        char b[64];
        snprintf(b, sizeof(b), "CHOOSE##%d", i);
        if (remnant_button(b, ImVec2(-1, 40))) {
            choose_archetype(i);
            open(SCR_NONE);
        }
        ImGui::EndChild();
    }
    end_screen();
}

static void stats_line(const char* k, const char* fmt, double v) {
    ImGui::TextColored(ImColor(col::dim).Value, "%s", k);
    ImGui::SameLine(150);
    ImGui::Text(fmt, v);
}

static void screen_terminal(const SaveData& s) {
    if (!begin_screen("##terminal", 0.74f, 0.76f)) return end_screen();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("left", ImVec2(w * 0.58f, 0));
    image(kUi[UI_WARD13_TERMINAL].art, 1024, ImVec2(ImGui::GetContentRegionAvail().x, 150));
    title("WARD 13");
    ImGui::Separator();
    for (int i = 0; i < kNpcsCount; i++) {
        const Npcs& n = kNpcs[i];
        ImGui::PushID(i);
        ImGui::BeginChild("npc", ImVec2(0, 110), ImGuiChildFlags_Borders);
        title(upper(r2_name(n.name)).c_str(), col::gold);
        quote(r2_text(greet_of(n, g_greet[i])));
        ImGui::EndChild();
        if (ImGui::IsItemClicked() || (ImGui::IsItemHovered() && ImGui::IsMouseReleased(0))) {}
        if (remnant_button(r2_text(n.open_option).c_str(), ImVec2(-1, 34))) {
            open(n.role == NPCS_ROLE_GUNS ? SCR_VENDOR : SCR_WORKBENCH);
            g_npcSays = r2_text(n.shop_line);
        }
        ImGui::PopID();
        ImGui::Spacing();
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 24);
    ImGui::BeginChild("right", ImVec2(0, 0));
    if (const Archetypes* a = archetype()) {
        image(a->emblem, 128, ImVec2(72, 72));
        ImGui::SameLine();
        ImGui::BeginGroup();
        label("ARCHETYPE");
        title(upper(r2_text(a->name)).c_str());
        ImGui::EndGroup();
    }
    ImGui::Separator();
    label("SCRAP");
    ImGui::PushFont(g_title, 34.0f);
    ImGui::TextColored(ImColor(col::gold).Value, "%d", s.scrap);
    ImGui::PopFont();
    ImGui::Spacing();
    for (int slot = 0; slot < 2; slot++) {
        int g = slot == 0 ? s.equippedLong : s.equippedHand;
        label(slot == 0 ? "LONG GUN" : "HAND GUN");
        if (g >= 0) {
            image(kWeapons[g].icon, 256, ImVec2(120, 50));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s%s", r2_text(kWeapons[g].name).c_str(),
                        s.upgrade[g] ? (" +" + std::to_string(s.upgrade[g])).c_str() : "");
        }
    }
    label("RELIC");
    image(kRelics[RELICS_DRAGON_HEART].icon, 128, ImVec2(40, 40));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s  x%d", r2_text(kRelics[RELICS_DRAGON_HEART].name).c_str(), s.relicCharges);
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 92);
    if (remnant_button("CHANGE ARCHETYPE", ImVec2(-1, 38))) open(SCR_ARCHETYPE);
    if (remnant_button(r2_text(kNpcs[0].leave_option).c_str(), ImVec2(-1, 38))) open(SCR_NONE);
    ImGui::EndChild();
    end_screen();
}

static void screen_vendor(SaveData snapshotSave) {
    const Npcs& n = kNpcs[NPCS_BRABUS];
    if (!begin_screen("##vendor", 0.74f, 0.76f)) return end_screen();
    title(upper(r2_name(n.name)).c_str(), col::gold);
    quote(g_npcSays);
    ImGui::Separator();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("list", ImVec2(w * 0.52f, -50));
    for (int i = 0; i < kWeaponsCount; i++) {
        const Weapons& wp = kWeapons[i];
        if (wp.sold_by != NPCS_BRABUS) continue;
        ImGui::PushID(i);
        bool sel = g_pick == i;
        ImGui::BeginChild("row", ImVec2(0, 64), ImGuiChildFlags_Borders);
        image(wp.icon, 256, ImVec2(120, 48));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextColored(ImColor(sel ? col::gold : col::text).Value, "%s", r2_text(wp.name).c_str());
        if (snapshotSave.owned[i]) ImGui::TextColored(ImColor(col::dim).Value, "%s  -  owned", wp.slot == WEAPONS_SLOT_LONG ? "Long gun" : "Hand gun");
        else ImGui::TextColored(ImColor(col::gold).Value, "%s  -  %d scrap", wp.slot == WEAPONS_SLOT_LONG ? "Long gun" : "Hand gun", (int)wp.price_scrap);
        ImGui::EndGroup();
        ImGui::EndChild();
        if (ImGui::IsItemClicked()) g_pick = i;
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine(0, 20);
    ImGui::BeginChild("detail", ImVec2(0, -50));
    if (g_pick >= 0) {
        const Weapons& wp = kWeapons[g_pick];
        image(wp.icon, 512, ImVec2(ImGui::GetContentRegionAvail().x, 120));
        title(upper(r2_text(wp.name)).c_str());
        ImGui::TextWrapped("%s", r2_text(wp.desc).c_str());
        ImGui::Spacing();
        stats_line("Damage", "%.0f", wp.damage * (wp.pellets > 1 ? wp.pellets : 1));
        stats_line("Fire rate", "%.0f rpm", wp.rpm);
        stats_line("Magazine", "%.0f", wp.mag);
        stats_line("Reload", "%.1f s", wp.reload_s);
        stats_line("Weakspot", "x%.2f", wp.weakspot_mult);
        stats_line("Ideal range", "%.0f m", wp.range_m);
        if (wp.mod >= 0) {
            label("MOD");
            image(kMods[wp.mod].icon, 128, ImVec2(36, 36));
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(r2_text(kMods[wp.mod].name).c_str());
        }
        ImGui::Spacing();
        Shared& sh = S();
        if (!snapshotSave.owned[g_pick]) {
            bool afford = snapshotSave.scrap >= (int)wp.price_scrap;
            char b[64];
            snprintf(b, sizeof(b), "BUY  (%d SCRAP)", (int)wp.price_scrap);
            if (remnant_button(b, ImVec2(-1, 40), afford)) {
                AcquireSRWLockExclusive(&sh.lock);
                if (sh.save.scrap >= (int)wp.price_scrap && !sh.save.owned[g_pick]) {
                    sh.save.scrap -= (int)wp.price_scrap;
                    sh.save.owned[g_pick] = true;
                    sh.live.mag[g_pick] = (int)wp.mag;
                    sh.live.reserve[g_pick] = (int)wp.reserve;
                    (wp.slot == WEAPONS_SLOT_LONG ? sh.save.equippedLong : sh.save.equippedHand) = g_pick;
                    sh.live.dirty = true;
                    g_npcSays = r2_text(n.done_line);
                }
                ReleaseSRWLockExclusive(&sh.lock);
            }
        } else if (remnant_button("EQUIP", ImVec2(-1, 40))) {
            AcquireSRWLockExclusive(&sh.lock);
            (wp.slot == WEAPONS_SLOT_LONG ? sh.save.equippedLong : sh.save.equippedHand) = g_pick;
            sh.live.dirty = true;
            ReleaseSRWLockExclusive(&sh.lock);
        }
    }
    ImGui::EndChild();
    ImGui::TextColored(ImColor(col::gold).Value, "Scrap: %d", snapshotSave.scrap);
    ImGui::SameLine(ImGui::GetWindowWidth() - 300);
    if (remnant_button(r2_text(n.leave_option).c_str(), ImVec2(240, 38))) open(SCR_TERMINAL);
    end_screen();
}

static void screen_workbench(SaveData sv) {
    const Npcs& n = kNpcs[NPCS_MCCABE];
    if (!begin_screen("##workbench", 0.70f, 0.74f)) return end_screen();
    title(upper(r2_name(n.name)).c_str(), col::gold);
    quote(g_npcSays);
    ImGui::Separator();
    ImGui::BeginChild("guns", ImVec2(0, -50));
    for (int i = 0; i < kWeaponsCount; i++) {
        if (!sv.owned[i]) continue;
        const Weapons& wp = kWeapons[i];
        ImGui::PushID(i);
        ImGui::BeginChild("g", ImVec2(0, 92), ImGuiChildFlags_Borders);
        image(wp.icon, 256, ImVec2(150, 60));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("%s  +%d", r2_text(wp.name).c_str(), sv.upgrade[i]);
        float mult = sv.upgrade[i] > 0 ? kUpgrades[sv.upgrade[i] - 1].damage_mult : 1.0f;
        ImGui::TextColored(ImColor(col::dim).Value, "Damage x%.2f", mult);
        if (wp.mod >= 0) {
            image(kMods[wp.mod].icon, 128, ImVec2(24, 24));
            ImGui::SameLine();
            ImGui::TextColored(ImColor(col::dim).Value, "%s", r2_text(kMods[wp.mod].name).c_str());
        }
        ImGui::EndGroup();
        int lvl = sv.upgrade[i];
        if (lvl < kUpgradesCount) {
            const Upgrades& u = kUpgrades[lvl];
            ImGui::SameLine(ImGui::GetWindowWidth() - 260);
            char b[64];
            snprintf(b, sizeof(b), "UPGRADE +%d  (%d)", lvl + 1, (int)u.cost_scrap);
            if (remnant_button(b, ImVec2(240, 40), sv.scrap >= (int)u.cost_scrap)) {
                Shared& sh = S();
                AcquireSRWLockExclusive(&sh.lock);
                if (sh.save.scrap >= (int)u.cost_scrap && sh.save.upgrade[i] == lvl) {
                    sh.save.scrap -= (int)u.cost_scrap;
                    sh.save.upgrade[i] = lvl + 1;
                    sh.live.dirty = true;
                    g_npcSays = r2_text(n.done_line);
                }
                ReleaseSRWLockExclusive(&sh.lock);
            }
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::TextColored(ImColor(col::gold).Value, "Scrap: %d", sv.scrap);
    ImGui::SameLine(ImGui::GetWindowWidth() - 300);
    if (remnant_button(r2_text(n.leave_option).c_str(), ImVec2(240, 38))) open(SCR_TERMINAL);
    end_screen();
}

// ---------------------------------------------------------------- HUD (MHW colours)

static void hud(const SaveData& s, const Live& l) {
    const Archetypes* a = archetype();
    if (!a || !l.snap.inGame) return;
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
    if (!l.combatAllowed) return;
    ImVec2 c(vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y / 2);
    if (l.gunMode) {
        int gun = equipped_gun(s, l);
        float spread = gun >= 0 ? kWeapons[gun].spread_deg * (l.aiming ? tune(TUNING_ADS_SPREAD_MULT) : 1.0f) : 1;
        float r = (6 + spread * 6) * sc;
        fg->AddCircle(c, r, IM_COL32(255, 255, 255, 170), 32, 1.5f);
        fg->AddCircleFilled(c, 2.0f * sc, IM_COL32(255, 255, 255, 230));
        if (t < l.hitMarkerUntil) {
            ImU32 hc = l.hitMarkerWeak ? col::red : IM_COL32(255, 255, 255, 240);
            float a1 = 7 * sc, a2 = 14 * sc;
            for (int i = 0; i < 4; i++) {
                float sx = (i & 1) ? 1.f : -1.f, sy = (i & 2) ? 1.f : -1.f;
                fg->AddLine(ImVec2(c.x + sx * a1, c.y + sy * a1), ImVec2(c.x + sx * a2, c.y + sy * a2), hc, 2.5f);
            }
        }
    }
    // Gun panel, bottom right.
    ImVec2 size(360 * sc, 132 * sc);
    ImVec2 p(vp->Pos.x + vp->Size.x - size.x - 40 * sc, vp->Pos.y + vp->Size.y - size.y - 210 * sc);
    fg->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), col::mhwFrame);
    fg->AddRect(p, ImVec2(p.x + size.x, p.y + size.y), col::mhwGold, 0, 0, 1.5f);
    int gun = equipped_gun(s, l);
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
        fg->AddText(g_bold, 16 * sc, ImVec2(p.x + 14 * sc, p.y + size.y - 26 * sc), col::mhwText,
                    r2_text(kWeapons[gun].name).c_str());
        char ammo[32];
        snprintf(ammo, sizeof(ammo), "%d", l.mag[gun]);
        fg->AddText(g_title, 44 * sc, ImVec2(p.x + 200 * sc, p.y + 30 * sc), col::mhwText, ammo);
        snprintf(ammo, sizeof(ammo), "/ %d", l.reserve[gun]);
        fg->AddText(g_body, 20 * sc, ImVec2(p.x + 270 * sc, p.y + 52 * sc), col::mhwGold, ammo);
        if (l.reloadDone > 0) {
            float k = (float)std::min(1.0, std::max(0.0, 1.0 - (l.reloadDone - t) / (kWeapons[gun].reload_s * a->reload_mult)));
            ImVec2 b0(p.x + 200 * sc, p.y + 96 * sc);
            fg->AddRectFilled(b0, ImVec2(b0.x + 140 * sc * k, b0.y + 5 * sc), col::mhwGold);
            fg->AddText(ImVec2(b0.x, b0.y - 20 * sc), col::mhwText, "Reloading");
        }
    }
    // Mod meter.
    int mod = s.equippedLong >= 0 ? kWeapons[s.equippedLong].mod : -1;
    if (mod >= 0) {
        ImVec2 m0(p.x + 200 * sc, p.y + size.y - 22 * sc);
        float k = t < l.modUntil ? 1.0f : l.modCharge / kMods[mod].charge_damage;
        fg->AddRectFilled(m0, ImVec2(m0.x + 140 * sc, m0.y + 8 * sc), IM_COL32(60, 52, 40, 255));
        fg->AddRectFilled(m0, ImVec2(m0.x + 140 * sc * k, m0.y + 8 * sc), k >= 1 ? col::accent : col::mhwGold);
        int w = 0, h = 0;
        if (void* tex = texture_for(kMods[mod].icon, 128, &w, &h))
            fg->AddImage((ImTextureID)(uintptr_t)tex, ImVec2(m0.x - 30 * sc, m0.y - 11 * sc), ImVec2(m0.x - 4 * sc, m0.y + 15 * sc));
    }
    // Skill and relic, bottom left above MHW's item bar.
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
    const Skills& sk = kSkills[a->skill];
    float sf = t >= l.skillReady ? 1.0f : (float)(1.0 - (l.skillReady - t) / sk.cooldown_s);
    char tag[48];
    if (t < l.skillUntil) snprintf(tag, sizeof(tag), "[%c] %.0fs", (char)keys().skill, l.skillUntil - t);
    else if (t < l.skillReady) snprintf(tag, sizeof(tag), "%.0fs", l.skillReady - t);
    else snprintf(tag, sizeof(tag), "[%c]", (char)keys().skill);
    icon_box(sk.icon, q, sf, tag);
    snprintf(tag, sizeof(tag), "[%c] x%d", (char)keys().relic, s.relicCharges);
    icon_box(kRelics[RELICS_DRAGON_HEART].icon, ImVec2(q.x + 80 * sc, q.y), s.relicCharges > 0 ? 1.0f : 0.0f, tag);
    if (a->hp_bonus > 0) {
        ImVec2 b0(q.x, q.y - 16 * sc);
        fg->AddRectFilled(b0, ImVec2(b0.x + 144 * sc, b0.y + 6 * sc), IM_COL32(60, 52, 40, 255));
        fg->AddRectFilled(b0, ImVec2(b0.x + 144 * sc * (l.bonusHealth / a->hp_bonus), b0.y + 6 * sc), IM_COL32(120, 190, 210, 255));
    }
}

// ---------------------------------------------------------------- per frame

void ui_draw() {}

void mod_frame() {
    Shared& sh = S();
    AcquireSRWLockShared(&sh.lock);
    SaveData s = sh.save;
    bool inGame = sh.live.snap.inGame, inBase = sh.live.snap.inBase, combat = sh.live.combatAllowed;
    bool gunMode = sh.live.gunMode;
    ReleaseSRWLockShared(&sh.lock);

    const Keys& k = keys();
    if (g_screen == SCR_NONE && inGame && inBase && !s.introDone && r2_ready()) open(SCR_INTRO);
    if (g_screen != SCR_NONE) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && g_screen != SCR_INTRO && (g_screen != SCR_ARCHETYPE || s.archetype >= 0))
            open(g_screen == SCR_VENDOR || g_screen == SCR_WORKBENCH ? SCR_TERMINAL : SCR_NONE);
        if (!inGame) open(SCR_NONE);
    } else if (inGame) {
        if (key_pressed(k.terminal) && inBase && s.archetype >= 0) open(SCR_TERMINAL);
        if (key_pressed(k.gun) && combat) {
            AcquireSRWLockExclusive(&sh.lock);
            sh.live.gunMode = !sh.live.gunMode;
            gunMode = sh.live.gunMode;
            ReleaseSRWLockExclusive(&sh.lock);
        }
        if (key_pressed(k.skill)) post(CMD_SKILL);
        if (key_pressed(k.relic)) post(CMD_RELIC);
        if (key_pressed(k.mod)) post(CMD_MOD);
        if (gunMode && key_pressed(k.reload)) post(CMD_RELOAD);
        if (gunMode && (key_pressed(k.swap) || ImGui::GetIO().MouseWheel != 0)) post(CMD_SWAP);
    }

    InputBlock b;
    if (g_screen != SCR_NONE) {
        b.keyboard = b.mouse = b.mouseButtons = true;
    } else if (inGame && s.archetype >= 0) {
        b.keys[k.skill] = b.keys[k.relic] = b.keys[k.mod] = 1;
        if (combat) b.keys[k.gun] = 1;
        if (inBase) b.keys[k.terminal] = 1;
        if (gunMode) {
            b.mouseButtons = true;
            b.keys[k.reload] = b.keys[k.swap] = 1;
        }
    }
    set_input_block(b);
    ImGui::GetIO().MouseDrawCursor = g_screen != SCR_NONE;

    switch (g_screen) {
        case SCR_INTRO: screen_intro(); break;
        case SCR_ARCHETYPE: screen_archetype(s); break;
        case SCR_TERMINAL: screen_terminal(s); break;
        case SCR_VENDOR: screen_vendor(s); break;
        case SCR_WORKBENCH: screen_workbench(s); break;
        default: break;
    }
    if (g_screen == SCR_NONE) {
        AcquireSRWLockShared(&sh.lock);
        Live l = sh.live;
        ReleaseSRWLockShared(&sh.lock);
        hud(s, l);
    }
}

}  // namespace rh
