// Remnant x MHW mashup core: the Remnant player on the MHW hunter (armour drawn on the hunter's joints, Remnant
// animations retargeted onto them). GPL-3.0-or-later.
//
// hunter_rig.cpp is the pure part (no game): the hunter's skeleton from sheets/hunter_bones.json, the matcher that
// finds those bones in MHW's joint block by geometry, and the retarget between Remnant II's skeleton and the hunter's.
// hunter.cpp is the game part: reads the block, draws the armour, writes the joints while a Remnant animation plays.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rh {

struct Model;
struct DrawItem;
struct SaveData;
struct Live;

constexpr int kHunterBones = 86;

// One bone of the hunter's .mod3 skeleton (sheet order = .mod3 index). The bind pose is a T-pose, cm, +Y up, facing
// +Z, the left hand at +X; every bind rotation is the identity, so a bone's frame in the bind pose is the model's.
struct HBone {
    int id = -1;          // function id
    int parent = -1;      // index, -1 = a root
    float len = 0;        // distance to the parent in the bind pose
    float local[3] = {};  // offset from the parent, in the parent's frame
    float bind[3] = {};   // bind pose position (model space)
    std::string r2;       // the Remnant II Player_M_Skeleton bone it maps to, "" = none
};
const std::vector<HBone>& hunter_bones();  // from the generated sheet
int hunter_bone_by_id(int id);             // index of a function id, -1 = none

// Which joint block entry is each hunter bone. aliases: entries with the same matrix as a matched bone (the
// armour pieces' .mod3 each carry a copy of the skeleton), as {entry, bone}.
struct HunterMap {
    int entry[kHunterBones];
    bool valid = false;
    int matched = 0, root = -1;
    std::vector<std::pair<int, int>> aliases;
    HunterMap() { for (int& e : entry) e = -1; }
};

// The joint block as read: n entries of 16 floats (row-vector world matrices: rows 0..2 the axes, row 3 the
// translation, cm), ok[i] = readable and rigid. Tries every entry within 20 cm of the player as the root and keeps
// the best assignment; valid when at least minMatched bones were found.
bool hunter_match(const float* block, const uint8_t* ok, int n, const float playerPos[3], HunterMap* out,
                  int minMatched = 60);

// The retarget between Remnant II's player skeleton and the hunter's, built from Remnant's reference pose.
// Matrices here are 3x4 row-major, column-vector (p' = R p + t), as in model.cpp.
struct HunterRig {
    bool ok = false;
    int r2[kHunterBones];        // Remnant skeleton bone of each hunter bone, -1 = none
    float A[kHunterBones][9];    // turns the Remnant bind bone direction onto the hunter's (MHW model frame)
    float CR[kHunterBones][9];   // the Remnant bind rotation in the MHW model frame (C * R)
    float pelvisR2[3] = {};      // the Remnant pelvis bind position (Remnant model space)
    int pelvisR2Bone = -1, pelvisH = -1;  // the pelvis in both skeletons
    int mapped = 0;
    HunterRig() { for (int& b : r2) b = -1; }
};
// names/parents/bind globals (n x 12) of Remnant's skeleton.
bool hunter_rig_build(const std::vector<std::string>& names, const float* bindGlobal, int n, HunterRig* rig);
// Remnant (UE: +Z up, facing +Y, cm) to the hunter's model frame (+Y up, facing +Z): (x, y, z) -> (x, z, y).
void hunter_r2_to_mhw(const float in[3], float out[3]);

// The hunter's world matrices (row-vector 4x4 each) for a Remnant pose: rotations from the Remnant bones through the
// rig, positions by forward kinematics down the hunter's own skeleton (its own limb lengths), the root and the pelvis
// from the game's own root and pelvis entries (rootM, pelvisM, row-vector), plus the animation's pelvis motion.
// set[i] = written for bone i (mapped bones only; the rest only carry the chain).
void hunter_retarget(const HunterRig& rig, const float* r2Global, const float* r2BindGlobal, const float rootM[16],
                     const float pelvisM[16], float out[kHunterBones][16], bool set[kHunterBones]);

// The skin palette (bones x 12) of a Remnant mesh on the hunter: hw = the hunter's world matrices (row-vector),
// hv = which are known. Mapped bones follow their hunter bone, the rest their parent with their reference offset.
bool hunter_mesh_palette(const HunterRig& rig, const Model& mesh, const float hw[kHunterBones][16],
                         const bool hv[kHunterBones], std::vector<float>* palette);

// ---------------------------------------------------------------- game side (hunter.cpp)
// Game thread, in mod_game_tick after the snapshot: matching, armour palettes, pose writes.
void hunter_tick(const SaveData& s, const Live& l, double t);
// A Remnant animation on the hunter (relic, skill, consumable use); "none"/empty = nothing. Any thread.
void hunter_play(const char* anim);
// The relic key was pressed (starts [hunter] pose_test). Any thread.
void hunter_relic_key();
// Render thread: the equipped Remnant armour as items of the 3D pass.
void hunter_append(std::vector<DrawItem>* items);

}  // namespace rh
