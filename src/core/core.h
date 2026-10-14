// Remnant x MHW mashup core. GPL-3.0-or-later (see LICENSE).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../common/log.h"
#include "gen/sheets.gen.h"

struct ID3D11Device;
struct ID3D11ShaderResourceView;

namespace rh {

// ---------------------------------------------------------------- memory (mem.cpp)
bool mem_readable(const void* p, size_t n);
bool sig_match(uintptr_t at, const char* sig);
template <class T> inline bool rd(uintptr_t a, T* out) {
    if (!mem_readable((const void*)a, sizeof(T))) return false;
    *out = *(const T*)a;
    return true;
}
inline uint8_t* rd_ptr(uintptr_t a) {
    uint8_t* p = nullptr;
    return rd(a, &p) ? p : nullptr;
}

// ---------------------------------------------------------------- hooks (mhw.cpp)
// One flag per row of the hooks sheet, set by check_hooks() after MHW has decrypted its code.
bool hook_ok(int row);
bool wait_for_game_code(int timeoutMs);
int check_hooks();  // returns how many hooks failed

// ---------------------------------------------------------------- game state (mhw.cpp)
struct Vec3 { float x, y, z; };
struct Monster {
    uint8_t* em;          // uEnemy*
    uint8_t* health;      // cpHealthManager*
    float hp, maxHp;
    bool large;
    float rot[3][4];      // OBB rows (world)
    Vec3 center, half;    // cm
    bool obbOk;
    char kind[16];        // MHW's model id, "em101_00" (names: sheets/mhw_monsters.json)
};
struct GameView {
    bool valid = false;
    Vec3 camPos{}, camTarget{};
    float fovY = 0;
    float view[16]{}, proj[16]{};
};
struct GameSnapshot {
    bool inGame = false;     // a hunter exists
    int zone = -1;
    bool inBase = false;     // Astera, Seliana, gathering hubs
    bool solo = true;
    uint8_t* player = nullptr;
    uint8_t* playerHealth = nullptr;
    float hp = 0, maxHp = 0;
    Vec3 pos{};
    GameView view;
    float hand[16] = {};     // the hunter's hand joint, a row-vector world matrix (translation in floats 12..14, cm)
    bool handValid = false;
    std::string handJoint;   // which chain it came from, for the log
    std::vector<Monster> monsters;
};

// Game thread only.
bool game_tick_installed();
bool install_game_tick();
void remove_game_tick();
void snapshot(GameSnapshot* out);                 // fills from live memory
bool add_hp(uint8_t* health, float delta);
bool reactions_play(int set, int id);               // game thread: DoAction on the hunter (faint sequence)
bool reactions_faint(int* set, int* id);            // the faint action learned from MHW (false = none yet)
bool reactions_carve(int* set, int* id);            // the carving action learned from MHW (false = none yet)
void reactions_own_pouch_change(double t);     // the mod is about to change the pouch itself (masks carve learning for 1 s)
void show_damage(uint8_t* em, int amount, const Vec3& pos, bool crit);
bool fire_slinger(const Vec3& origin, const Vec3& target);
bool world_ray(const Vec3& from, const Vec3& to, Vec3* hit);  // true = blocked by level geometry
void set_camera_fov(float fovY);                  // aim zoom; 0 = leave the game's own value
void set_first_person(bool on);                  // aiming down sights: the camera moves to the hunter's head
float first_person_amount();                     // 0 third person .. 1 first person (blends in and out)
bool camera_view(GameView* out);                  // any thread: MHW's camera right now (read-only)
int player_weapon_type();                         // game thread: 0 great sword .. 13 light bowgun, -1 unknown
bool zone_is_base(int zone);

// ---------------------------------------------------------------- MHW's scene depth as the bosses' terrain
// overlay.cpp (render thread, at Present, Direct3D 11): every 4th pixel of MHW's scene depth is copied into a small
// staging texture each frame and read back without stalling (DO_NOT_WAIT); the last finished copy is kept with the
// camera it was rendered with, so a probe reads one consistent frame. MHW's depth never holds the Remnant bosses.
struct DepthFrame {
    int w = 0, h = 0;              // cells
    int fullW = 0, fullH = 0;      // the depth buffer; cell (x, y) is its pixel (min(4x+2, fullW-1), min(4y+2, fullH-1))
    std::vector<float> ndcZ;       // per cell: the depth as this frame's projection's NDC z; NaN = sky / far plane
    float viewProj[16] = {};       // row vectors: clip = world * view * proj
    float invViewProj[16] = {};
    Vec3 eye{};                    // the camera (cm)
    uint64_t tickMs = 0;           // GetTickCount64 when it was rendered
};
std::shared_ptr<const DepthFrame> depth_frame();  // any thread: the latest finished copy, nullptr until one exists
bool terrain_enabled();                           // [bosses] terrain=1 (default on)
// Pure terrain geometry (bosses.cpp; host test tests/test_terrain.cpp).
void mat4_mul(const float a[16], const float b[16], float out[16]);
bool mat4_inverse(const float m[16], float out[16]);
bool depth_project(const float vp[16], Vec3 p, float* nx, float* ny, float* ndcZ);  // false behind the camera
Vec3 depth_unproject(const float inv[16], float nx, float ny, float ndcZ);
enum ProbeResult { PROBE_OK, PROBE_OFFSCREEN, PROBE_SKY, PROBE_FAR, PROBE_OCCLUDED };
// The terrain under (x, z): project at yGuess, read the depth there, unproject, re-project at the found height (twice).
// OCCLUDED = the depth there belongs to something between the camera and the point (hit is that object).
ProbeResult depth_probe(const DepthFrame& f, float x, float z, float yGuess, Vec3* hit);
float median_of(float* v, int n);  // reorders v
enum StepVerdict { STEP_OK, STEP_WALL, STEP_CLIFF };
// A boss stepping from ground curGround towards a point lookahead cm ahead whose ground is destGround (when destOk):
// a rise over 120 cm (+ a 35 degree slope over the lookahead) is a wall, a drop over 300 cm (+ 45 degrees) a cliff;
// blocker = the probe hit an object standing at the destination.
StepVerdict step_verdict(float curGround, bool destOk, float destGround, float lookahead, bool blocker);
bool ground_probe(float x, float z, float yGuess, float* yOut);  // game thread (bosses.cpp): the depth probe, filtered

// ---------------------------------------------------------------- Remnant II content (r2client.cpp)
bool r2_start();  // finds the Remnant II folder, starts r2read.exe
void r2_stop();
bool r2_ready();
const char* r2_status();
// Text/names resolve synchronously on first use and are cached (empty string when unavailable).
const std::string& r2_text(const sheets::R2Text& t);
const std::string& r2_name(const sheets::R2Name& n);
struct R2Image { int w = 0, h = 0; std::string format; std::vector<uint8_t> pixels; bool failed = false; };
const R2Image* r2_image(const char* asset, int maxSide);  // nullptr until loaded (loads on a worker thread)
bool r2_make_texture(ID3D11Device* dev, const R2Image& img, const char* asset, ID3D11ShaderResourceView** srv);  // texture.cpp
bool r2_make_texture(ID3D11Device* dev, const R2Image& img, const char* asset, ID3D11ShaderResourceView** srv, bool srgb);  // albedo: _SRGB formats
int r2_cells_checked(int* failed);  // test mode: resolves every Remnant II cell of the sheets
// A play of a Remnant II sound event (one variation of each layer), stereo 16-bit; nullptr until decoded.
struct R2Sound { int rate = 0; std::vector<int16_t> pcm; bool failed = false; };
const R2Sound* r2_sound(const char* event, int variant);

// Binary replies of r2read (MESH, SKEL, ANIM): raw bytes, decoded in model.cpp; nullptr until read.
struct R2Blob { std::vector<uint8_t> data; bool failed = false; };
const R2Blob* r2_blob(const char* kind, const char* asset);

// Remnant II sounds through the mod's own audio (audio.cpp, XAudio2). Any thread.
void audio_play(const char* event, float volume = 1.0f);
void audio_prefetch(const char* event);
void audio_stop(const char* event);  // stops that event's playing voices (trigger released)

// ---------------------------------------------------------------- overlay + input (overlay.cpp)
bool overlay_install();
void overlay_remove();
HWND game_window();
void* texture_for(const char* asset, int maxSide, int* w, int* h, bool srgb = false);  // ImTextureID or nullptr (render thread)
// Input routing decided by the mod each frame.
// mouseButtons blocks every mouse button; mouseButton[i] blocks one (0 left, 1 right, 2 middle, 3 X1, 4 X2).
struct InputBlock { bool keyboard = false, mouse = false, mouseButtons = false; uint8_t mouseButton[8] = {}; uint8_t keys[256] = {}; };
// Reports the clutch-claw mouse button (ini [keys] clutch_claw) pressed to MHW for ~80 ms, starting delaySeconds from now.
void inject_claw(double delaySeconds);
void set_input_block(const InputBlock& b);
bool key_down(int vk);       // reads the physical key state while the game window has focus
bool key_pressed(int vk);    // edge, once per press (render-thread frame)
int input_reads_per_second();  // input reads the game made in the last 0.25 s windows, per second (render thread)

// ---------------------------------------------------------------- the mod (remnant.cpp / ui.cpp)
void mod_init();
void mod_shutdown();
void mod_game_tick();   // game thread
void mod_frame();       // render thread, inside an ImGui frame
void ui_init_fonts();   // render thread, before the first frame
void ui_draw();         // render thread

// ---------------------------------------------------------------- MHW engine API found by pattern (devdiag.cpp)
bool game_api_init();   // blocking; reads only
void game_api_start();  // runs game_api_init on a worker thread once
bool game_api_ready();
void* find_dti(const char* name);
void* get_resource(const char* dtiName, const char* path);  // game thread
const char* dti_name_of(const void* obj);                     // class name of an MtObject (read-only)
int menu_flag_read();      // [menu] flag=<singleton>+0x<off>: that byte (0..255), or -1 when not set or unreadable (any thread)
bool menu_heuristic_open();  // ui.cpp: the input-rate signal says an MHW menu is open
uintptr_t singleton_static(const char* name);                 // static holding a singleton, after game_api_init
uint8_t* user_data(); uint8_t* player_ptr();                                         // current save slot's cUserData
const char* gmd_message(void* gmd, int id);
std::vector<uintptr_t> scan_text(const std::string& pattern, int max);
uint32_t mt_crc(const char* s);
void devconsole_tick(double now, bool inGame, bool inBase, int zone, const Vec3& pos);                             // game thread; read-only report when dev_diag exists

// ---------------------------------------------------------------- real Remnant items in MHW's item bar (nativepc.cpp)
// Reads MHW's loaded item table and item text, writes <game>/nativePC/common/item/itemData.itm and
// .../common/text/steam/item_eng.gmd with extra rows, and remembers the new ids in <mod folder>/items.json.
struct NpGmd {  // item_eng.gmd: header, one 32-byte entry per message (hash chain), 256 buckets, label text, message text
    uint8_t head[20] = {};            // "GMD", version, language, 8 zero bytes
    std::string name;                 // the file's own name ("item")
    std::vector<std::string> labels, msgs;
};
bool np_gmd_parse(const uint8_t* d, size_t n, NpGmd* g);
std::vector<uint8_t> np_gmd_write(const NpGmd& g);
void np_gmd_add(NpGmd* g, const std::string& label, const std::string& text);
// itemData.itm: 10-byte header (u16 count @6), then 32-byte rows. Appends n rows cloned from row cloneId; new ids follow the old count.
bool np_itm_add(std::vector<uint8_t>* itm, int cloneId, int n);
void nativepc_tick();                // init thread, until it has finished; reads game memory only
int nativepc_item_id(int barRow);    // the real MHW item id of a bar item (kBar_items row), 0 = not loaded in this game session
const char* nativepc_take_toast();   // a one-time message for the game thread to toast, or nullptr

// Settings (remnant_hunt.ini in the mod folder).
// Defaults avoid MHW's own default PC bindings (B, G, V, T, P are free or unused in solo play).
struct Keys { int gun = 'B', reload = 'R', swap = 'X', mod = 'V', aimSlinger = 'V', clutchClaw = VK_MBUTTON, skill = 'G', skill2 = 'H', relic = 'T', terminal = 'P', boss = VK_F9; };
const Keys& keys();
// [gun] in the ini: where the hunter's hand joint is read from, and how the gun sits in it.
struct GunCfg { int handL1 = 0x90, handL2 = 0x2B0, handEntry = 396, handStride = 0x40; float off[3] = {0, 0, 0}; int axis = 2; bool axisNeg = false; };
const GunCfg& gun_cfg();
// [actions] in the ini: an MHW action (set:id) played through DoAction when a Remnant item is used. set < 0 = none.
struct ActionCfg { int set[3] = {-1, -1, -1}, id[3] = {0, 0, 0}; };  // 0 relic, 1 skill, 2 consumable
const ActionCfg& action_cfg();
bool bar_proxies();  // [bar] proxies=1: Remnant stand-ins in MHW's item bar (default off)
bool test_mode();  // MELTY_TEST=1 or test=1 in the ini

}  // namespace rh
