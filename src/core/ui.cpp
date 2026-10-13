// Remnant x MHW mashup core: screens and HUD. GPL-3.0-or-later (see LICENSE).
//
// Remnant-styled screens for Remnant tasks: the worldstone intro, archetypes, the Ward 13 terminal (the Remnant
// loadout) and the Remnant menus at MHW's own NPCs (crafting Remnant gear from MHW materials); a small HUD in MHW's
// colours for the guns, skills and relic. Names, descriptions, lines and icons are read from the player's own
// Remnant II install; effect lines are our own wording; MHW's own UI is left as it is.
#include <cfloat>
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

enum Screen { SCR_NONE, SCR_INTRO, SCR_ARCHETYPE, SCR_TERMINAL, SCR_MENU };
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
    float m = upgrade_mult(level);
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

static void archetype_detail(int row, const SaveData& s, bool chooseSkill) {
    const Archetypes& a = kArchetypes[row];
    if (a.prime.asset) {
        label("PRIME PERK");
        ImGui::TextWrapped("%s", text_or(&a.prime).c_str());
    }
    std::vector<int> sk = skills_of(row);
    label("SKILLS");
    for (size_t i = 0; i < sk.size(); i++) {
        const Skills& k = kSkills[sk[i]];
        ImGui::PushID((int)i);
        image(k.icon, 128, ImVec2(36, 36));
        ImGui::SameLine();
        ImGui::BeginGroup();
        int slot = row == s.archetype ? 0 : row == s.archetype2 ? 1 : -1;
        bool chosen = slot >= 0 && s.skill[slot] == (int)i;
        ImGui::TextColored(ImColor(chosen ? col::gold : col::text).Value, "%s  (cooldown %.0fs)", text_or(&k.name).c_str(), k.cooldown_s);
        effect_lines(k.effects);
        if (chooseSkill && slot >= 0 && !chosen && remnant_button("USE THIS SKILL", ImVec2(180, 28))) {
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
        ImGui::TextColored(ImColor(kPerks[p].kind == PERKS_KIND_PRIME ? col::gold : col::text).Value, "%s",
                           text_or(&kPerks[p].name).c_str());
        ImGui::Indent();
        effect_lines(kPerks[p].effects);
        ImGui::Unindent();
    }
    if (a.trait >= 0) {
        label("TRAIT");
        ImGui::TextUnformatted(text_or(&kTraits[a.trait].name).c_str());
        ImGui::Indent();
        effect_lines(kTraits[a.trait].effects);
        ImGui::Unindent();
    }
}

static void item_detail(const ItemView& v, const SaveData& s) {
    float w = ImGui::GetContentRegionAvail().x;
    image(v.icon, 512, ImVec2(w, v.cat == CAT_WEAPON ? 110 : 96));
    title(upper(text_or(v.name)).c_str());
    ImGui::TextColored(ImColor(col::dim).Value, "%s  -  tier %d", cat_label(v.cat), v.tier);
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
    if (v.effects && *v.effects) {
        label("EFFECTS");
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
        else ImGui::TextColored(ImColor(col::dim).Value, "tier %d", v.tier);
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
                {"RELICS", CAT_RELIC, -1},         {"FRAGMENTS", CAT_FRAGMENT, -1}};
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
                    int lvl = s.level[v.row];
                    if (lvl < kUpgradesCount) {
                        const Upgrades& u = kUpgrades[lvl];
                        ImGui::Text("+%d  ->  +%d  (damage x%.2f)", lvl, lvl + 1, u.damage_mult);
                        bool ok = recipe_box(u.recipe, (int)u.zenny);
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
            std::vector<int> sk = skills_of(a);
            if (!sk.empty()) {
                const Skills& k = kSkills[sk[std::min((int)sk.size() - 1, s.skill[slot])]];
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
        const Skills& k = kSkills[sk[std::min((int)sk.size() - 1, s.skill[slot])]];
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
            open(g_screen == SCR_ARCHETYPE ? SCR_TERMINAL : SCR_NONE);
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
        default: break;
    }
}

}  // namespace rh
