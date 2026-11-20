#include "config.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>

namespace sanctum {

static Config g_cfg;
Config &cfg() { return g_cfg; }

std::string config_dir() {
    char *p = SDL_GetPrefPath("SteelTemple", "Sanctum");
    std::string s = p ? p : "";
    SDL_free(p);
    return s;
}

static std::string path() { return config_dir() + "settings.ini"; }

void config_load() {
    size_t n = 0;
    char *data = (char *)SDL_LoadFile(path().c_str(), &n);
    if (!data) return;
    std::string all(data, n);
    SDL_free(data);
    size_t at = 0;
    while (at < all.size()) {
        size_t end = all.find('\n', at);
        if (end == std::string::npos) end = all.size();
        std::string line = all.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "theme") g_cfg.theme = v == "light" ? 1 : 0;
        else if (k == "follow") g_cfg.follow = v == "1";
        else if (k == "large_tiles") g_cfg.large_tiles = v == "1";
        else if (k == "sounds") g_cfg.sounds = v == "1";
        else if (k == "show_dev") g_cfg.show_dev = v == "1";
        else if (k == "fullscreen") g_cfg.fullscreen = v == "1";
        else if (k == "hide_cursor") g_cfg.hide_cursor = v == "1";
        else if (k == "cover_dir") g_cfg.cover_dir = v;
        else if (k == "library_dir") g_cfg.library_dir = v;
        else if (k == "profile" && !v.empty()) g_cfg.profile = v;
    }
}

void config_save() {
    std::string s = "# SteelTemple Sanctum: this player's settings\n";
    s += std::string("theme=") + (g_cfg.theme ? "light" : "dark") + "\n";
    s += std::string("follow=") + (g_cfg.follow ? "1" : "0") + "\n";
    s += std::string("large_tiles=") + (g_cfg.large_tiles ? "1" : "0") + "\n";
    s += std::string("sounds=") + (g_cfg.sounds ? "1" : "0") + "\n";
    s += std::string("show_dev=") + (g_cfg.show_dev ? "1" : "0") + "\n";
    s += std::string("fullscreen=") + (g_cfg.fullscreen ? "1" : "0") + "\n";
    s += std::string("hide_cursor=") + (g_cfg.hide_cursor ? "1" : "0") + "\n";
    s += "cover_dir=" + g_cfg.cover_dir + "\n";
    s += "library_dir=" + g_cfg.library_dir + "\n";
    s += "profile=" + g_cfg.profile + "\n";
    std::string tmp = path() + ".tmp";
    if (SDL_SaveFile(tmp.c_str(), s.data(), s.size())) SDL_RenamePath(tmp.c_str(), path().c_str());
}

}  // namespace sanctum
