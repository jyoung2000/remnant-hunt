// Remnant x MHW mashup: Dear ImGui configuration. GPL-3.0-or-later (see LICENSE).
// A failed ImGui check must never stop the game: it is logged once per place instead of raising a dialog.
#pragma once
void rh_imgui_assert(const char* expr, const char* file, int line);
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : rh_imgui_assert(#_EXPR, __FILE__, __LINE__))
