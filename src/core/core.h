// Remnant x MHW mashup core. GPL-3.0-or-later (see LICENSE).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../common/log.h"
#include "gen/sheets.gen.h"

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
    std::vector<Monster> monsters;
};

// Game thread only.
bool game_tick_installed();
bool install_game_tick();
void remove_game_tick();
void snapshot(GameSnapshot* out);                 // fills from live memory
bool add_hp(uint8_t* health, float delta);
void show_damage(uint8_t* em, int amount, const Vec3& pos, bool crit);
bool fire_slinger(const Vec3& origin, const Vec3& target);
bool world_ray(const Vec3& from, const Vec3& to, Vec3* hit);  // true = blocked by level geometry
void set_camera_fov(float fovY);                  // aim zoom; 0 = leave the game's own value
void set_first_person(bool on);                  // aiming down sights: the camera moves to the hunter's head
float first_person_amount();                     // 0 third person .. 1 first person (blends in and out)
bool camera_view(GameView* out);                  // any thread: MHW's camera right now (read-only)
int player_weapon_type();                         // game thread: 0 great sword .. 13 light bowgun, -1 unknown
bool zone_is_base(int zone);

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

// ---------------------------------------------------------------- overlay + input (overlay.cpp)
bool overlay_install();
void overlay_remove();
HWND game_window();
void* texture_for(const char* asset, int maxSide, int* w, int* h);  // ImTextureID or nullptr (render thread)
// Input routing decided by the mod each frame.
struct InputBlock { bool keyboard = false, mouse = false, mouseButtons = false; uint8_t keys[256] = {}; };
void set_input_block(const InputBlock& b);
bool key_down(int vk);       // reads the physical key state while the game window has focus
bool key_pressed(int vk);    // edge, once per press (render-thread frame)

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
uintptr_t singleton_static(const char* name);                 // static holding a singleton, after game_api_init
uint8_t* user_data(); uint8_t* player_ptr();                                         // current save slot's cUserData
const char* gmd_message(void* gmd, int id);
std::vector<uintptr_t> scan_text(const std::string& pattern, int max);
uint32_t mt_crc(const char* s);
void devconsole_tick(double now, bool inGame, bool inBase, int zone, const Vec3& pos);                             // game thread; read-only report when dev_diag exists

// Settings (remnant_hunt.ini in the mod folder).
// Defaults avoid MHW's own default PC bindings (B, G, V, T, P are free or unused in solo play).
struct Keys { int gun = 'B', reload = 'R', swap = 'X', mod = 'V', skill = 'G', skill2 = 'H', relic = 'T', terminal = 'P', boss = VK_F9; };
const Keys& keys();
bool test_mode();  // MELTY_TEST=1 or test=1 in the ini

}  // namespace rh
