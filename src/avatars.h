// Profile icons: Sanctum's own artwork (tools/make_avatars.py draws assets/avatars.png and avatars.txt), each drawn in
// white and see-through tones over a background colour the player picks, like the console's user icons.
#pragma once
#include <SDL3/SDL.h>

#include <string>
#include <vector>

#include "imgui.h"

namespace sanctum {

struct AvatarColour { const char *name; ImU32 col; };

void avatars_init(SDL_Renderer *r);   // loads the atlas beside the program
int avatar_count();
const std::string &avatar_name(int i);
const std::string &avatar_category(int i);
const std::vector<std::string> &avatar_categories();   // in the atlas order
int avatar_find(const std::string &name);              // -1 if none has that name
const std::vector<AvatarColour> &avatar_colours();

// icon i on background colour bg, as a circle of radius r centred on (cx, cy) (canvas points)
void avatar_draw(int icon, int bg, float cx, float cy, float r, float alpha = 1);
void avatar_draw_player(float cx, float cy, float r);   // the player's own (Settings: avatar, avatar_bg)

}  // namespace sanctum
