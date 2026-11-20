// Sanctum's themes: the same model as the ports' menus (Universal-NS2-to-PC runtime/ui/theme.h, itself the
// Mario Kart World PC menu's): two universal themes, Dark (near-black, red accent) and Light (the Switch 2 system
// settings look, cyan selection), plus each port's own palette. With "Follow selection" on, the focused port's
// palette tints Home and colours its game page. Colours and simple shapes only: no game artwork.
#pragma once
#include <stdint.h>

#include "imgui.h"

namespace sanctum {

struct Palette {
    ImU32 bg, bg2, panel, panel2, line;
    ImU32 text, soft, dim;
    ImU32 accent, accent_text, focus, ok, warn;
    bool light;
};

// Universal Dark: theme.h kDark's colours, laid out for a full-screen shell
inline const Palette kDark = {
    IM_COL32(20, 20, 24, 255), IM_COL32(28, 28, 33, 255), IM_COL32(35, 35, 41, 255), IM_COL32(43, 43, 51, 255),
    IM_COL32(58, 58, 68, 255), IM_COL32(236, 236, 240, 255), IM_COL32(182, 182, 192, 255), IM_COL32(128, 128, 140, 255),
    IM_COL32(232, 36, 48, 255), IM_COL32(255, 92, 98, 255), IM_COL32(63, 224, 255, 255), IM_COL32(60, 207, 122, 255),
    IM_COL32(255, 156, 64, 255), false};
// Universal Light: theme.h kLight's
inline const Palette kLight = {
    IM_COL32(235, 235, 238, 255), IM_COL32(244, 244, 246, 255), IM_COL32(255, 255, 255, 255), IM_COL32(240, 240, 242, 255),
    IM_COL32(206, 207, 212, 255), IM_COL32(38, 38, 44, 255), IM_COL32(88, 88, 96, 255), IM_COL32(138, 138, 146, 255),
    IM_COL32(0, 176, 214, 255), IM_COL32(0, 128, 168, 255), IM_COL32(0, 176, 214, 255), IM_COL32(29, 158, 87, 255),
    IM_COL32(206, 92, 20, 255), true};

// A port's own colours: its accent, a deep partner for gradients and its monogram tile
struct PortColours {
    ImU32 accent, deep;
};

enum ThemeId { THEME_DARK = 0, THEME_LIGHT = 1 };

const Palette &pal();                 // the theme in effect
void theme_set(int id);               // THEME_DARK / THEME_LIGHT
int theme_id();

inline ImU32 with_alpha(ImU32 c, int a) { return (c & 0x00FFFFFF) | ((ImU32)a << 24); }
ImU32 mix(ImU32 a, ImU32 b, float t);

}  // namespace sanctum
