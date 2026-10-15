// Remnant x MHW mashup core: hidden host World monsters under the Remnant bosses, for the clutch claw. GPL-3.0-or-later.
//
// MHW's clutch claw, mounts and part breaks only act on real World monsters. Each Remnant boss gets one host monster
// (Jalon's approval, 2026-10-14): borrowed from the zone (mode=borrow) or spawned (mode=spawn, through the game's own
// spawn routine once [host] spawn_fn names it). Every tick the host is pinned to the boss (position, collision position
// and facing written), hidden (its transparency at 0; scale and depth only for testing), kept from hurting the hunter
// (its target nulled and its attacks replaced with its idle action through MHW's own DoAction) and kept alive (its HP
// written high). Its part breaks become the boss's. On the boss's death it is never killed: refilled, sunk 3 m under
// its own spot and frozen for 30 s, then let go where it was (borrowed) or left sunk (spawned), so it drops nothing.
// What the hunter does to the host (a claw latch, a mount) is turned into Remnant-side data for the boss (bosses.cpp).
#pragma once
#include "core.h"

namespace rh {

// ---------------------------------------------------------------- pure parts (tests/test_host.cpp)

struct HostQuat { float x, y, z, w; };
// The host's rotation for a Remnant boss facing yaw (atan2(x, z), as bosses.cpp). fwd: which way of the rotation's z is
// the monsters' forward (turf.cpp learns it: +1 or -1; 0 = unknown, taken as +1).
HostQuat host_quat_from_yaw(float yaw, int fwd);
float host_yaw_of_quat(const HostQuat& q, int fwd);

constexpr float kHostLatchCm = 250.0f;     // the hunter this close to the host's body
constexpr double kHostPressWindow = 1.5;  // s after an aim + claw press: the next action is the claw's
constexpr double kHostTenderS = 90.0;      // a tenderised spot lasts this long
constexpr double kHostMountS = 8.0;        // a mount staggers the boss this long
constexpr float kHostClawFrac = 0.03f;     // a claw hit takes this share of the boss's max health

enum HostLatchEv : int { HL_LATCH = 1, HL_UNLATCH = 2, HL_CLAW_HIT = 4, HL_MOUNT = 8, HL_LEARNED = 16 };
bool host_claw_family(int clawSet, int clawId, int set, int id);  // the same set, the id within 4 of the learned one

// Latch detection from the hunter's state, one step per game tick. gap: cm from the hunter to the nearest host's body;
// above: cm the hunter is above the host's feet; set/id: the hunter's current action; press: an aim + claw press this
// tick; hurt: the host lost HP this tick. Returns HostLatchEv bits.
struct HostLatch {
    int clawSet = -1, clawId = -1;  // the learned claw action (-1 = not yet)
    bool latched = false;
    int prevSet = -1, prevId = -1;
    double pressAt = -100, lastHitAt = -100, highSince = -1, mountAt = -100;
    int step(double t, float gap, float above, int set, int id, bool press, bool hurt);
};

// The Remnant side of a claw hit and a mount: one tenderised bone (damage there doubled) and a mounted stagger.
struct HostBossFx {
    int tenderBone = -1;
    double tenderUntil = 0, mountedUntil = 0;
    void tenderise(int bone, double t) { tenderBone = bone; tenderUntil = t + kHostTenderS; }
    float mult(int bone, double t) const { return bone >= 0 && bone == tenderBone && t < tenderUntil ? 2.0f : 1.0f; }
    void mount(double t) { mountedUntil = t + kHostMountS; }
    bool mounted(double t) const { return t < mountedUntil; }
};

// The transparency field (+0x314, UNVERIFIED: the 15.11 DTI dump) is only written when its 4 bytes read as an alpha:
// a float 0 or in [1e-4, 1] (HOST_ALPHA_FLOAT), or a whole word 1..255 (HOST_ALPHA_BYTE: the low byte is the alpha).
enum HostAlpha : int { HOST_ALPHA_NONE = 0, HOST_ALPHA_FLOAT = 1, HOST_ALPHA_BYTE = 2 };
int host_alpha_class(uint32_t bits);

// The monster parts table ([uEnemy+0x1D058], HunterPie): normal parts from +0x40, 0x1F8 bytes each, with max HP +0x0C
// (f32), HP +0x10 (f32) and the break count +0x18 (i32); severable parts from +0x1FC8, 0x78 bytes each.
constexpr size_t kHostPartsAt = 0x40, kHostPartStride = 0x1F8, kHostSeverAt = 0x1FC8, kHostSeverStride = 0x78;
constexpr int kHostMaxParts = 16;
struct HostPart { float maxHp, hp; int breaks; };
// The leading run of plausible parts in a copy of the table (len bytes): max HP in (0, 1e6), HP finite and at most max,
// break count 0..99. Returns how many (0 = not a parts table).
int host_parts_read(const uint8_t* table, size_t len, HostPart* out, int maxParts);
// part i -> the i-th largest of n hitbox spheres (radii), wrapping round; -1 when n is 0.
int host_part_cluster(int part, const float* radii, int n);

// "em001_00" -> id 1, sub 0.
bool host_parse_em(const char* s, int* id, int* sub);
// The spawn routine's guards: rva lies inside the .text section of the PE image whose headers are in image[0..len),
// and the bytes there look like the start of an x64 function.
bool host_rva_in_text(const uint8_t* image, size_t len, uint32_t rva);
bool host_looks_like_prologue(const uint8_t* b, size_t n);

// ---------------------------------------------------------------- game thread (host.cpp)

enum HostEvKind : int { HOST_EV_CLAW = 1, HOST_EV_MOUNT = 2, HOST_EV_PART = 3 };
struct HostEvent { int kind; int part; Vec3 at; };

bool host_enabled();                                                 // [host] enabled
void host_bind(int key, const char* bossId, Vec3 pos, float yaw);   // a boss is out: find it a host (key = boss slot)
void host_follow(int key, Vec3 pos, float yaw);                     // the boss's place this tick
void host_release(int key);                                          // the boss died or left: the host is given back
bool host_take(int key, HostEvent* ev);                              // the next claw / mount / part event for the boss
bool host_is(const uint8_t* em);                                     // a bound host (not a monster to fight or show)
// Every game tick, first thing in bosses_tick: pins and hides the hosts, disarms them, watches the hunter, and takes
// the hosts out of g.monsters so nothing else in the mod sees them.
void host_tick(GameSnapshot& g, double t, float dt);
// mode=spawn: calls the game's SpawnMonsterInit(sEnemy, emId, subId, flag) at [host] spawn_fn (an RVA; 0 = off), on the
// game thread, under SEH. The monster that appears near pos within 2 s becomes the host (host_tick picks it up).
// Returns false when the call is off, refused by its guards, faulted or already pending.
bool host_spawn_request(int emId, int subId, Vec3 pos);

// turf.cpp, for the host's disarm and facing.
int turf_forward();                                       // +1 / -1: the monsters' forward along their rotation's z; 0 unknown
bool turf_is_attack(const char* kind, int set, int id);  // a learned attack of that monster kind

}  // namespace rh
