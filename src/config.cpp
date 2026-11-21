#include "config.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app.h"

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
        else if (k == "sound_volume") g_cfg.sound_volume = v == "low" ? 0 : v == "high" ? 2 : 1;
        else if (k == "hide_dev") g_cfg.hide_dev = v == "1";   // (the older show_dev=0 is left behind on purpose)
        else if (k.rfind("prefer.", 0) == 0 && !v.empty()) g_cfg.prefer[k.substr(7)] = v;
        else if (k == "fullscreen") g_cfg.fullscreen = v == "1";
        else if (k == "hide_cursor") g_cfg.hide_cursor = v == "1";
        else if (k == "pad_layout") g_cfg.pad_layout = v == "nintendo" ? 1 : v == "xbox" ? 2 : 0;
        else if (k == "button_icons") g_cfg.button_icons = v == "nintendo" ? 1 : v == "xbox" ? 2 : v == "playstation" ? 3 : 0;
        else if (k == "cover_dir") g_cfg.cover_dir = v;
        else if (k == "library_dir") g_cfg.library_dir = v;
        else if (k == "profile" && !v.empty()) g_cfg.profile = v;
        else if (k == "auto_update") g_cfg.auto_update = v == "1";
        else if (k == "avatar" && !v.empty()) g_cfg.avatar = v;
        else if (k == "avatar_bg") g_cfg.avatar_bg = SDL_clamp(atoi(v.c_str()), 0, 15);
    }
    if (demo_mode()) g_cfg.cover_dir.clear(), g_cfg.library_dir.clear();   // README screenshots: no cover art
}

void config_save() {
    if (demo_mode()) return;   // screenshots never touch the player's settings
    std::string s = "# SteelTemple Sanctum: this player's settings\n";
    s += std::string("theme=") + (g_cfg.theme ? "light" : "dark") + "\n";
    s += std::string("follow=") + (g_cfg.follow ? "1" : "0") + "\n";
    s += std::string("large_tiles=") + (g_cfg.large_tiles ? "1" : "0") + "\n";
    static const char *const vols[] = {"low", "medium", "high"};
    s += std::string("sounds=") + (g_cfg.sounds ? "1" : "0") + "\n";
    s += std::string("sound_volume=") + vols[g_cfg.sound_volume % 3] + "\n";
    s += std::string("hide_dev=") + (g_cfg.hide_dev ? "1" : "0") + "\n";
    for (auto &e : g_cfg.prefer) s += "prefer." + e.first + "=" + e.second + "\n";
    s += std::string("fullscreen=") + (g_cfg.fullscreen ? "1" : "0") + "\n";
    s += std::string("hide_cursor=") + (g_cfg.hide_cursor ? "1" : "0") + "\n";
    static const char *const layouts[] = {"auto", "nintendo", "xbox"}, *const icons[] = {"auto", "nintendo", "xbox", "playstation"};
    s += std::string("pad_layout=") + layouts[g_cfg.pad_layout % 3] + "\n";
    s += std::string("button_icons=") + icons[g_cfg.button_icons % 4] + "\n";
    s += "cover_dir=" + g_cfg.cover_dir + "\n";
    s += "library_dir=" + g_cfg.library_dir + "\n";
    s += "profile=" + g_cfg.profile + "\n";
    s += std::string("auto_update=") + (g_cfg.auto_update ? "1" : "0") + "\n";
    s += "avatar=" + g_cfg.avatar + "\n";
    s += "avatar_bg=" + std::to_string(g_cfg.avatar_bg) + "\n";
    std::string tmp = path() + ".tmp";
    if (SDL_SaveFile(tmp.c_str(), s.data(), s.size())) SDL_RenamePath(tmp.c_str(), path().c_str());
}

}  // namespace sanctum
