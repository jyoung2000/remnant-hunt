// r2read: Remnant II's Wwise sounds, decoded in memory. GPL-3.0-or-later.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "r2pak.h"

namespace r2 {

// The sound sources of an event package's embedded soundbank, grouped into layers of variations.
bool eventLayers(const Bytes& eventPkg, std::vector<std::vector<uint32_t>>* layers, std::string* err);
// A Wwise Vorbis .wem -> interleaved 16-bit PCM.
bool wemToPcm(const Bytes& wem, std::vector<int16_t>* pcm, int* channels, int* rate, std::string* err);
// One play of an event (a random variation of each layer, mixed to stereo 16-bit).
bool eventPcm(const Paks& paks, const Bytes& eventPkg, uint32_t seed, std::vector<int16_t>* out, int* rate,
              std::string* err);

}  // namespace r2
