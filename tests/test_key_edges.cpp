#include "key_edges.h"

#include <cstdio>

using rh::KeyEdgeState;

int main() {
    KeyEdgeState keys;
    std::array<bool, 256> down{};
    down['9'] = true;
    keys.sample(false, down);
    if (keys.wasPressed('9')) return 1;
    keys.sample(true, down);  // focus returns while 9 is still held
    if (keys.wasPressed('9')) return 2;
    down['9'] = false;
    keys.sample(true, down);
    down['9'] = true;
    keys.sample(true, down);
    if (!keys.wasPressed('9')) return 3;
    keys.sample(true, down);  // key repeat / later render frame
    if (keys.wasPressed('9')) return 4;
    std::puts("key edges: focus regain does not invent a press; fresh press is one-shot");
    return 0;
}
