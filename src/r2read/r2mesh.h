// r2read: Remnant II skeletal meshes, skeletons and animations. GPL-3.0-or-later.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "r2pak.h"

namespace r2 {

struct Bone {
    std::string name;
    int32_t parent = -1;
    float rot[4] = {0, 0, 0, 1}, pos[3] = {0, 0, 0}, scale[3] = {1, 1, 1};  // reference pose, local to the parent
};
struct Section { int16_t material = 0; uint32_t baseIndex = 0, numTriangles = 0, baseVertex = 0, numVertices = 0; };
struct Vertex {
    float pos[3];
    int16_t normal[4] = {0, 0, 0, 0};  // snorm16
    float uv[2] = {0, 0};
    uint16_t bones[8] = {0};            // reference skeleton indexes
    uint8_t weights[8] = {0};           // sum 255
};
struct Mesh {
    std::vector<Bone> bones;
    std::vector<std::string> materials;  // slot names
    std::vector<Section> sections;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};
struct Animation {
    uint32_t numTracks = 0, numSamples = 0;
    float rate = 30;
    std::vector<int32_t> trackToBone;    // skeleton asset bone index per track
    std::vector<float> samples;          // samples x tracks x {quat xyzw, translation xyz, scale xyz}
};

bool readMesh(const Bytes& pkg, Mesh* out, std::string* err);
bool readSkeletonAsset(const Bytes& pkg, std::vector<Bone>* bones, std::string* err);
bool readAnimation(const Bytes& pkg, Animation* out, std::string* err);

}  // namespace r2
