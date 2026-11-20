// Cover art from the player's own folder (System Settings > Cover art). Nothing is copied or shipped: the files are
// read where they are, matched to ports by name (the game's title, the port's name, or its short name alone, e.g.
// "Mario Kart World.png", "THWOMP.jpg", "OoT.png"), decoded on a worker thread and kept as textures. The folder is
// watched: a picture added, replaced or removed shows within two seconds. A port with no cover is drawn as its colours
// and monogram.
#pragma once
#include <SDL3/SDL.h>

#include <string>

namespace sanctum {

void covers_init(SDL_Renderer *r);
void covers_reload();                     // after the folder changes
void covers_poll(double now);             // UI thread: decoded images into textures; watches the folder
SDL_Texture *cover(const char *port_id);  // nullptr until loaded (or if none)
int covers_matched();                     // how many ports the folder has art for
int count_pngs(const std::string &dir);   // the pictures in a folder (png, jpg, jpeg, bmp)

}  // namespace sanctum
