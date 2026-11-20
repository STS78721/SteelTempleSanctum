#include "theme.h"

namespace sanctum {

static int g_theme = THEME_DARK;

const Palette &pal() { return g_theme == THEME_LIGHT ? kLight : kDark; }
void theme_set(int id) { g_theme = id == THEME_LIGHT ? THEME_LIGHT : THEME_DARK; }
int theme_id() { return g_theme; }

ImU32 mix(ImU32 a, ImU32 b, float t) {
    auto ch = [&](int s) {
        float x = (float)((a >> s) & 255), y = (float)((b >> s) & 255);
        return (ImU32)(x + (y - x) * t + 0.5f) << s;
    };
    return ch(0) | ch(8) | ch(16) | ch(24);
}

}  // namespace sanctum
