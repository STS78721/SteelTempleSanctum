// The player's own settings: %APPDATA%\SteelTemple\Sanctum\settings.ini (SDL_GetPrefPath), key=value lines.
// Never next to the program and never in a release: a package carries defaults only.
#pragma once
#include <string>

namespace sanctum {

struct Config {
    int theme = 0;                // THEME_DARK / THEME_LIGHT
    bool follow = true;           // the focused port tints Home
    bool large_tiles = true;
    bool sounds = true;
    bool show_dev = false;        // list copies inside build / staging / backup folders too
    bool fullscreen = true;       // starts fullscreen, like a console
    bool hide_cursor = true;      // the pointer hides while a pad is in use
    std::string cover_dir;        // the player's own folder of cover art; empty until they pick one
    std::string library_dir;      // where Get ports installs; empty: <home>\Games\SteelTemple
    std::string profile = "Player 1";
};

Config &cfg();
void config_load();
void config_save();
std::string config_dir();         // with a trailing separator

}  // namespace sanctum
