// Remnant x MHW mashup. GPL-3.0-or-later (see LICENSE).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace rh {
// The mod's folder inside the game folder ({game}\remnant_hunt), set by the loader.
void set_mod_dir(const char* dir);
const char* mod_dir();
// Appends to remnant_hunt.log in the mod folder (shared by loader and core).
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Melty's test log: <mod folder>\melty.log (one line per check).
void melty_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
}  // namespace rh
