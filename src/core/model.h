// Remnant x MHW mashup core: Remnant II models and animations decoded from r2read's replies. GPL-3.0-or-later.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rh {

struct Xform {  // rotation (quaternion xyzw), translation, scale
    float q[4] = {0, 0, 0, 1}, t[3] = {0, 0, 0}, s[3] = {1, 1, 1};
};
struct ModelBone { std::string name; int parent = -1; Xform ref; };
#pragma pack(push, 1)
struct ModelVertex {  // must match r2read's Vertex (52 bytes)
    float pos[3];
    int16_t normal[4];
    float uv[2];
    uint16_t bones[8];
    uint8_t weights[8];
};
#pragma pack(pop)
static_assert(sizeof(ModelVertex) == 52, "vertex layout");
struct ModelSection { int16_t material; uint32_t baseIndex, numTriangles, baseVertex, numVertices; };

struct Model {
    std::vector<ModelBone> bones;
    std::vector<std::string> materials;
    std::vector<ModelSection> sections;
    std::vector<ModelVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<float> invBind;  // bones x 12 (3x4, row-major): inverse of the reference pose in model space
    float boundsMin[3] = {0, 0, 0}, boundsMax[3] = {0, 0, 0};
};

struct Anim {
    uint32_t tracks = 0, samples = 0;
    float rate = 30;
    std::vector<int> trackToModelBone;  // filled when bound to a model (through the skeleton's bone names)
    std::vector<int32_t> trackToSkelBone;
    std::vector<float> data;            // samples x tracks x 10
    float length() const { return samples > 1 ? (samples - 1) / rate : 0; }
};

// Decoded and cached; nullptr while r2read is still reading (or when it failed: *failed is set).
const Model* model_get(const char* meshAsset, bool* failed = nullptr);
const Anim* anim_get(const char* animAsset, const char* skeletonAsset, const Model* m, bool* failed = nullptr);

// Bone palette (bones x 12 floats, 3x4 row-major, model space) for an animation at time t; blend mixes a second one.
void pose(const Model& m, const Anim* a, float t, bool loop, const Anim* b, float tb, float blend, std::vector<float>* palette,
          float rootDelta[3] = nullptr);

// A skeleton alone (SKEL reply: bones and inverse bind, no vertices), for posing the player's skeleton without a mesh.
const Model* skeleton_model(const char* skelAsset, bool* failed = nullptr);
// Bone globals (bones x 12 floats, 3x4 row-major, model space) of an animation at time t (a = nullptr: the reference
// pose). The root's x/y translation stays at the reference, as in pose().
void pose_global(const Model& m, const Anim* a, float t, bool loop, std::vector<float>* global);

// Root bone translation (model space) of an animation at time t, for driving the creature by the animation's own motion.
bool anim_root(const Anim& a, float t, bool loop, float out[3]);

}  // namespace rh
