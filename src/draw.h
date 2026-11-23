// Drawing on the 1280x720 canvas: text, rounded shapes, button glyphs that match the pad in use, the vector icons,
// game tiles and the pulsing focus frame. All positions and sizes are canvas points.
#pragma once
#include <SDL3/SDL.h>

#include <initializer_list>
#include <string>
#include <utility>

#include "app.h"
#include "catalog.h"
#include "theme.h"

namespace sanctum {

ImDrawList *dl();
float text_w(float size, const char *s);
// align: 0 left, 0.5 centre, 1 right; returns the width drawn
float text(float x, float y, float size, ImU32 col, const char *s, float align = 0);
// text wrapped at width w; returns the height it takes (draw = false only measures)
float text_wrap(float x, float y, float size, ImU32 col, const char *s, float w, bool draw = true);
void progress_bar(float x, float y, float w, float h, float frac, ImU32 col);   // frac < 0: busy, no figure
// the text cut with "..." to fit max_w
std::string fit(float size, const std::string &s, float max_w);
void rect(float x, float y, float w, float h, ImU32 col, float r = 0);
void frame(float x, float y, float w, float h, ImU32 col, float r, float thick);
void circle(float x, float y, float rad, ImU32 col);
// the selection: Switch 2 animated prismatic aurora (or chosen theme style) frame just outside (x, y, w, h)
void focus_ring(float x, float y, float w, float h, float r, float pad = 7, ImU32 accent_override = 0);

// a button as the pad in use prints it (A/B/X/Y in Xbox colours, Nintendo's plain letters, PlayStation's shapes)
float glyph(Nav n, float x, float y, float size);   // returns its width
float glyph_w(Nav n, float size);
// the hint bar along the bottom: [glyph label] pairs from the right
void hint_bar(std::initializer_list<std::pair<Nav, const char *>> hints);
void status_bar(bool show_profile = true);

enum Icon { IC_NEWS, IC_DOWNLOAD, IC_MODS, IC_PROFILE, IC_PAD, IC_GEAR, IC_POWER, IC_FOLDER, IC_PALETTE, IC_LIBRARY,
            IC_INFO, IC_PLAY, IC_SEARCH, IC_DRIVE, IC_UP, IC_CHECK, IC_DISPLAY, IC_SOUND, IC_PAUSE, IC_CLOSE, IC_STORE,
            IC_ALBUM, IC_PIN, IC_STORAGE, IC_COPY, IC_GRID };
void icon(Icon i, float cx, float cy, float size, ImU32 col);

// Handheld / laptop system battery (returns true on Legion Go / ROG Ally / laptops with a battery, false on desktops)
bool system_battery_info(int *pct_out, bool *charging_out);

// a port's tile: its cover, else its colours and monogram; badges for its state
void tile(const Port &p, float x, float y, float size, float alpha = 1, bool badges = true, float lit = 1);
void toast_draw();
void background(ImU32 tint);   // the theme's backdrop, tinted from the top-left by the focused port

float ease(float current, float target, float speed);   // frame-rate independent approach
// a picture fading out to the left and bottom (the game page's backdrop), at most alpha a
void image_fade(SDL_Texture *t, float x0, float y0, float x1, float y1, int a);

}  // namespace sanctum
