// Cover art from the player's own folder (System Settings > Cover art) and optional per-game overrides.
// Nothing is copied or shipped: files are read where they are, decoded on a worker thread, and kept as textures.
#pragma once
#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace sanctum {

void covers_init(SDL_Renderer *r);
void covers_reload();                     // after the folder or per-game cover changes
void covers_poll(double now);             // UI thread: decoded images into textures; watches the folder
SDL_Texture *cover(const char *port_id);  // nullptr until loaded (or if none)
int covers_matched();                     // how many ports have cover art
int count_pngs(const std::string &dir);   // the pictures in a folder (png, jpg, jpeg, bmp)

// Shared image decoder (PNG/JPEG/BMP -> RGBA, optional box-downscale to fit max_dim) for Album thumbnails & lightbox
bool load_image_rgba(const std::string &path, int max_dim, int &w_out, int &h_out, std::vector<unsigned char> &rgba_out);

}  // namespace sanctum
