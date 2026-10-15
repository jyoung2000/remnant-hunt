#pragma once

#include <array>
#include <cstddef>

namespace rh {

// Do not synthesize a key press when the game gains focus with a key already held.
struct KeyEdgeState {
    std::array<bool, 256> previous{};
    std::array<bool, 256> pressed{};
    bool focused = false;

    void sample(bool hasFocus, const std::array<bool, 256>& down) {
        pressed.fill(false);
        if (!hasFocus) {
            focused = false;
            return;
        }
        if (!focused) {
            previous = down;
            focused = true;
            return;
        }
        for (size_t key = 1; key < down.size(); ++key) {
            pressed[key] = down[key] && !previous[key];
            previous[key] = down[key];
        }
    }

    bool wasPressed(int key) const { return key > 0 && key < (int)pressed.size() && pressed[(size_t)key]; }
};

}  // namespace rh
