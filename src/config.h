// The player's own settings: %APPDATA%\SteelTemple\Sanctum\settings.ini (SDL_GetPrefPath), key=value lines.
// Never next to the program and never in a release: a package carries defaults only.
#pragma once
#include <map>
#include <string>

namespace sanctum {

struct Config {
    int theme = 0;                // THEME_DARK / THEME_LIGHT
    bool follow = true;           // the focused port tints Home
    bool large_tiles = true;
    bool sounds = true;
    bool hide_dev = false;        // leave out copies inside build / staging / backup folders (they rank last anyway)
    std::map<std::string, std::string> prefer;   // port id -> the copy the player chose ("Use this copy")
    bool fullscreen = true;       // starts fullscreen, like a console
    bool hide_cursor = true;      // the pointer hides while a pad is in use
    // like the ports' F1 menus: which face button confirms, and which buttons the hints show
    int pad_layout = 0;           // 0 automatic (the label printed on the button), 1 Nintendo (A right), 2 Xbox (A bottom)
    int button_icons = 0;         // 0 automatic (the pad's own), 1 Nintendo, 2 Xbox, 3 PlayStation
    std::string cover_dir;        // the player's own folder of cover art; empty until they pick one
    std::string library_dir;      // where Get ports installs; empty: <home>\Games\SteelTemple
    std::string profile = "Player 1";
    std::string avatar = "Sword";  // the profile icon, by its name in assets/avatars.txt
    int avatar_bg = 0;             // its background colour (avatars.cpp, kColours)
};

Config &cfg();
void config_load();
void config_save();
std::string config_dir();         // with a trailing separator

}  // namespace sanctum
