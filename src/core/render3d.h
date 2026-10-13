// Remnant x MHW mashup core: drawing Remnant II's own 3D models into MHW's frame. GPL-3.0-or-later.
#pragma once
#include <vector>

#include "model.h"

namespace rh {

struct DrawItem {
    const Model* model = nullptr;
    float world[16];                       // model (Remnant II units, cm) -> MHW world, row-vector convention
    std::vector<float> palette;            // bones x 12 (see pose())
    std::vector<const char*> diffuse;      // per material slot: Remnant II texture asset, or nullptr
    float tint[4] = {1, 1, 1, 0};
};

// Render thread, Direct3D 11 only: draws the items into rtv with MHW's view and projection (row-vector matrices).
void render3d_frame(void* device, void* context, void* rtv, int width, int height, const float view[16],
                    const float proj[16], const std::vector<DrawItem>& items);
// MHW's scene depth as a shader resource (nullptr = not found), whether it is reversed (near = 1), and its size.
void render3d_set_game_depth(void* srv, bool reversed, int width, int height);

}  // namespace rh
