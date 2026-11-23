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

static void profile_sync_from_active() {
    if (g_cfg.profiles.empty()) g_cfg.profiles.push_back({"Player 1", "Sword", 0});
    g_cfg.active_profile = SDL_clamp(g_cfg.active_profile, 0, (int)g_cfg.profiles.size() - 1);
    const PlayerProfile &p = g_cfg.profiles[g_cfg.active_profile];
    g_cfg.profile = p.name;
    g_cfg.avatar = p.avatar;
    g_cfg.avatar_bg = SDL_clamp(p.avatar_bg, 0, 15);
}

void profile_sync_to_active() {
    if (g_cfg.profiles.empty()) g_cfg.profiles.push_back({"Player 1", "Sword", 0});
    g_cfg.active_profile = SDL_clamp(g_cfg.active_profile, 0, (int)g_cfg.profiles.size() - 1);
    PlayerProfile &p = g_cfg.profiles[g_cfg.active_profile];
    p.name = g_cfg.profile.empty() ? "Player 1" : g_cfg.profile;
    p.avatar = g_cfg.avatar.empty() ? "Sword" : g_cfg.avatar;
    p.avatar_bg = SDL_clamp(g_cfg.avatar_bg, 0, 15);
}

void profile_switch(int index) {
    if (index < 0 || index >= (int)g_cfg.profiles.size()) return;
    profile_sync_to_active();
    g_cfg.active_profile = index;
    profile_sync_from_active();
    config_save();
}

int profile_add() {
    if (g_cfg.profiles.size() >= 4) return -1;
    profile_sync_to_active();
    static const char *const kDefIcons[] = {"Sword", "Controller", "Star", "Crown"};
    static const int kDefBgs[] = {0, 4, 8, 12};
    int idx = (int)g_cfg.profiles.size();
    PlayerProfile p;
    p.name = "Player " + std::to_string(idx + 1);
    p.avatar = kDefIcons[idx % 4];
    p.avatar_bg = kDefBgs[idx % 4];
    g_cfg.profiles.push_back(p);
    g_cfg.active_profile = idx;
    profile_sync_from_active();
    config_save();
    return idx;
}

bool profile_delete(int index) {
    if (g_cfg.profiles.size() <= 1 || index < 0 || index >= (int)g_cfg.profiles.size()) return false;
    g_cfg.profiles.erase(g_cfg.profiles.begin() + index);
    if (g_cfg.active_profile >= (int)g_cfg.profiles.size()) g_cfg.active_profile = (int)g_cfg.profiles.size() - 1;
    profile_sync_from_active();
    config_save();
    return true;
}

void config_load() {
    size_t n = 0;
    char *data = (char *)SDL_LoadFile(path().c_str(), &n);
    if (!data) return;
    std::string all(data, n);
    SDL_free(data);
    std::map<int, PlayerProfile> loaded_profiles;
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
        else if (k == "home_sort") g_cfg.home_sort = v == "most_played" ? 1 : v == "az" ? 2 : 0;
        else if (k == "border_style") g_cfg.border_style = v == "rainbow" ? 1 : v == "accent" ? 2 : v == "classic" ? 3 : 0;
        else if (k == "border_speed") g_cfg.border_speed = v == "off" ? 0 : v == "calm" ? 1 : v == "lively" ? 3 : 2;
        else if (k == "tile_motion") g_cfg.tile_motion = v == "off" ? 0 : v == "subtle" ? 1 : 2;
        else if (k == "dock_style") g_cfg.dock_style = v == "classic" ? 1 : 0;
        else if (k == "pinned" && !v.empty()) {
            size_t p = 0;
            while (p < v.size()) {
                size_t c = v.find(',', p);
                if (c == std::string::npos) c = v.size();
                std::string id = v.substr(p, c - p);
                if (!id.empty()) g_cfg.pinned.insert(id);
                p = c + 1;
            }
        }
        else if (k == "sounds") g_cfg.sounds = v == "1";
        else if (k == "sound_volume") g_cfg.sound_volume = v == "low" ? 0 : v == "high" ? 2 : 1;
        else if (k == "sound_theme") g_cfg.sound_theme = v == "bubbly" ? 1 : v == "retro" ? 2 : v == "minimal" ? 3 : 0;
        else if (k == "sound_pitch_pan") g_cfg.sound_pitch_pan = v == "1";
        else if (k == "ambient_bgm") g_cfg.ambient_bgm = v == "hum" ? 1 : v == "custom" ? 2 : 0;
        else if (k == "bgm_path") g_cfg.bgm_path = v;
        else if (k == "rumble") g_cfg.rumble = v == "1";
        else if (k == "rumble_strength") g_cfg.rumble_strength = v == "low" ? 0 : v == "high" ? 2 : 1;
        else if (k == "stick_deadzone") g_cfg.stick_deadzone = v == "low" ? 0 : v == "high" ? 2 : 1;
        else if (k == "force_close_chord") g_cfg.force_close_chord = v == "1";
        else if (k == "guide_overlay") g_cfg.guide_overlay = v == "1";
        else if (k == "guide_override_gamebar") g_cfg.guide_override_gamebar = v == "1";
        else if (k == "perf_mode") g_cfg.perf_mode = v == "off" ? 0 : v == "always" ? 2 : 1;
        else if (k == "perf_power_plan") g_cfg.perf_power_plan = v == "1";
        else if (k == "perf_trim_ram") g_cfg.perf_trim_ram = v == "1";
        else if (k == "perf_hud") g_cfg.perf_hud = v == "compact" ? 1 : v == "detailed" ? 2 : 0;
        else if (k == "save_sync_dir") g_cfg.save_sync_dir = v;
        else if (k == "save_sync_auto") g_cfg.save_sync_auto = v == "1";
        else if (k.rfind("hz.", 0) == 0 && !v.empty()) {
            int hz = atoi(v.c_str());
            if (hz >= 30 && hz <= 500) g_cfg.refresh_hz[k.substr(3)] = hz;
        }
        else if (k == "hide_dev") g_cfg.hide_dev = v == "1";   // (the older show_dev=0 is left behind on purpose)
        else if (k.rfind("prefer.", 0) == 0 && !v.empty()) g_cfg.prefer[k.substr(7)] = v;
        else if (k.rfind("cover.", 0) == 0 && !v.empty()) g_cfg.port_covers[k.substr(6)] = v;
        else if (k.rfind("args.", 0) == 0 && !v.empty()) g_cfg.launch_args[k.substr(5)] = v;
        else if (k.rfind("env.", 0) == 0 && !v.empty()) g_cfg.launch_env[k.substr(4)] = v;
        else if (k == "fullscreen") g_cfg.fullscreen = v == "1";
        else if (k == "hide_cursor") g_cfg.hide_cursor = v == "1";
        else if (k == "clock_12h") g_cfg.clock_12h = v == "1";
        else if (k == "display_name") g_cfg.display_name = v;
        else if (k == "idle_dim_min") { int m = atoi(v.c_str()); g_cfg.idle_dim_min = (m == 5 || m == 15) ? m : 0; }
        else if (k == "pad_layout") g_cfg.pad_layout = v == "nintendo" ? 1 : v == "xbox" ? 2 : 0;
        else if (k == "button_icons") g_cfg.button_icons = v == "nintendo" ? 1 : v == "xbox" ? 2 : v == "playstation" ? 3 : 0;
        else if (k == "cover_dir") g_cfg.cover_dir = v;
        else if (k == "screenshots_dir") g_cfg.screenshots_dir = v;
        else if (k == "library_dir") g_cfg.library_dir = v;
        else if (k == "profile" && !v.empty()) g_cfg.profile = v;
        else if (k == "auto_update") g_cfg.auto_update = v == "1";
        else if (k == "discord_rpc") g_cfg.discord_rpc = v == "1";
        else if (k == "discord_app_id") g_cfg.discord_app_id = v;
        else if (k == "last_seen_version") g_cfg.last_seen_version = v;
        else if (k == "avatar" && !v.empty()) g_cfg.avatar = v;
        else if (k == "avatar_bg") g_cfg.avatar_bg = SDL_clamp(atoi(v.c_str()), 0, 15);
        else if (k == "active_profile") g_cfg.active_profile = atoi(v.c_str());
        else if (k.rfind("player.", 0) == 0) {
            int idx = atoi(k.substr(7).c_str());
            size_t b1 = v.find('|'), b2 = b1 == std::string::npos ? std::string::npos : v.find('|', b1 + 1);
            if (idx >= 0 && idx < 4 && b1 != std::string::npos && b2 != std::string::npos) {
                PlayerProfile p;
                p.name = v.substr(0, b1);
                p.avatar = v.substr(b1 + 1, b2 - b1 - 1);
                p.avatar_bg = SDL_clamp(atoi(v.substr(b2 + 1).c_str()), 0, 15);
                if (!p.name.empty()) loaded_profiles[idx] = p;
            }
        }
    }
    if (!loaded_profiles.empty()) {
        g_cfg.profiles.clear();
        for (auto &e : loaded_profiles) g_cfg.profiles.push_back(e.second);
        profile_sync_from_active();
    } else {
        g_cfg.profiles = {{g_cfg.profile, g_cfg.avatar, g_cfg.avatar_bg}};
        g_cfg.active_profile = 0;
    }
    if (demo_mode()) {
        g_cfg.cover_dir.clear();
        g_cfg.library_dir.clear();
        g_cfg.screenshots_dir.clear();
        g_cfg.save_sync_dir.clear();
        g_cfg.port_covers.clear();
        g_cfg.refresh_hz.clear();
        g_cfg.discord_rpc = false;
        g_cfg.idle_dim_min = 0;
    }
}

void config_save() {
    if (demo_mode()) return;   // screenshots never touch the player's settings
    profile_sync_to_active();
    std::string s = "# SteelTemple Sanctum: this player's settings\n";
    s += std::string("theme=") + (g_cfg.theme ? "light" : "dark") + "\n";
    s += std::string("follow=") + (g_cfg.follow ? "1" : "0") + "\n";
    s += std::string("large_tiles=") + (g_cfg.large_tiles ? "1" : "0") + "\n";
    static const char *const sorts[] = {"last_played", "most_played", "az"};
    static const char *const bstyles[] = {"aurora", "rainbow", "accent", "classic"};
    static const char *const bspeeds[] = {"off", "calm", "normal", "lively"};
    static const char *const tmotions[] = {"off", "subtle", "elastic"};
    s += std::string("home_sort=") + sorts[g_cfg.home_sort % 3] + "\n";
    s += std::string("border_style=") + bstyles[SDL_clamp(g_cfg.border_style, 0, 3)] + "\n";
    s += std::string("border_speed=") + bspeeds[SDL_clamp(g_cfg.border_speed, 0, 3)] + "\n";
    s += std::string("tile_motion=") + tmotions[SDL_clamp(g_cfg.tile_motion, 0, 2)] + "\n";
    s += std::string("dock_style=") + (g_cfg.dock_style ? "classic" : "pill") + "\n";
    if (!g_cfg.pinned.empty()) {
        std::string pin;
        for (const std::string &id : g_cfg.pinned) pin += (pin.empty() ? "" : ",") + id;
        s += "pinned=" + pin + "\n";
    }
    static const char *const vols[] = {"low", "medium", "high"};
    static const char *const sthemes[] = {"clicky", "bubbly", "retro", "minimal"};
    static const char *const bgms[] = {"off", "hum", "custom"};
    s += std::string("sounds=") + (g_cfg.sounds ? "1" : "0") + "\n";
    s += std::string("sound_volume=") + vols[g_cfg.sound_volume % 3] + "\n";
    s += std::string("sound_theme=") + sthemes[SDL_clamp(g_cfg.sound_theme, 0, 3)] + "\n";
    s += std::string("sound_pitch_pan=") + (g_cfg.sound_pitch_pan ? "1" : "0") + "\n";
    s += std::string("ambient_bgm=") + bgms[g_cfg.ambient_bgm % 3] + "\n";
    if (!g_cfg.bgm_path.empty()) s += "bgm_path=" + g_cfg.bgm_path + "\n";
    s += std::string("rumble=") + (g_cfg.rumble ? "1" : "0") + "\n";
    s += std::string("rumble_strength=") + vols[g_cfg.rumble_strength % 3] + "\n";
    s += std::string("stick_deadzone=") + vols[g_cfg.stick_deadzone % 3] + "\n";
    s += std::string("force_close_chord=") + (g_cfg.force_close_chord ? "1" : "0") + "\n";
    s += std::string("guide_overlay=") + (g_cfg.guide_overlay ? "1" : "0") + "\n";
    s += std::string("guide_override_gamebar=") + (g_cfg.guide_override_gamebar ? "1" : "0") + "\n";
    static const char *const pmodes[] = {"off", "auto", "always"};
    static const char *const hmodes[] = {"off", "compact", "detailed"};
    s += std::string("perf_mode=") + pmodes[g_cfg.perf_mode % 3] + "\n";
    s += std::string("perf_power_plan=") + (g_cfg.perf_power_plan ? "1" : "0") + "\n";
    s += std::string("perf_trim_ram=") + (g_cfg.perf_trim_ram ? "1" : "0") + "\n";
    s += std::string("perf_hud=") + hmodes[g_cfg.perf_hud % 3] + "\n";
    if (!g_cfg.save_sync_dir.empty()) s += "save_sync_dir=" + g_cfg.save_sync_dir + "\n";
    s += std::string("save_sync_auto=") + (g_cfg.save_sync_auto ? "1" : "0") + "\n";
    for (auto &e : g_cfg.refresh_hz) if (e.second > 0) s += "hz." + e.first + "=" + std::to_string(e.second) + "\n";
    s += std::string("hide_dev=") + (g_cfg.hide_dev ? "1" : "0") + "\n";
    for (auto &e : g_cfg.prefer) s += "prefer." + e.first + "=" + e.second + "\n";
    for (auto &e : g_cfg.port_covers) if (!e.second.empty()) s += "cover." + e.first + "=" + e.second + "\n";
    for (auto &e : g_cfg.launch_args) if (!e.second.empty()) s += "args." + e.first + "=" + e.second + "\n";
    for (auto &e : g_cfg.launch_env) if (!e.second.empty()) s += "env." + e.first + "=" + e.second + "\n";
    s += std::string("fullscreen=") + (g_cfg.fullscreen ? "1" : "0") + "\n";
    s += std::string("hide_cursor=") + (g_cfg.hide_cursor ? "1" : "0") + "\n";
    s += std::string("clock_12h=") + (g_cfg.clock_12h ? "1" : "0") + "\n";
    if (!g_cfg.display_name.empty()) s += "display_name=" + g_cfg.display_name + "\n";
    s += "idle_dim_min=" + std::to_string(g_cfg.idle_dim_min) + "\n";
    static const char *const layouts[] = {"auto", "nintendo", "xbox"}, *const icons[] = {"auto", "nintendo", "xbox", "playstation"};
    s += std::string("pad_layout=") + layouts[g_cfg.pad_layout % 3] + "\n";
    s += std::string("button_icons=") + icons[g_cfg.button_icons % 4] + "\n";
    s += "cover_dir=" + g_cfg.cover_dir + "\n";
    if (!g_cfg.screenshots_dir.empty()) s += "screenshots_dir=" + g_cfg.screenshots_dir + "\n";
    s += "library_dir=" + g_cfg.library_dir + "\n";
    s += "profile=" + g_cfg.profile + "\n";
    s += std::string("auto_update=") + (g_cfg.auto_update ? "1" : "0") + "\n";
    s += std::string("discord_rpc=") + (g_cfg.discord_rpc ? "1" : "0") + "\n";
    if (!g_cfg.discord_app_id.empty()) s += "discord_app_id=" + g_cfg.discord_app_id + "\n";
    if (!g_cfg.last_seen_version.empty()) s += "last_seen_version=" + g_cfg.last_seen_version + "\n";
    s += "avatar=" + g_cfg.avatar + "\n";
    s += "avatar_bg=" + std::to_string(g_cfg.avatar_bg) + "\n";
    s += "active_profile=" + std::to_string(g_cfg.active_profile) + "\n";
    for (size_t i = 0; i < g_cfg.profiles.size(); ++i) {
        const PlayerProfile &p = g_cfg.profiles[i];
        s += "player." + std::to_string(i) + "=" + p.name + "|" + p.avatar + "|" + std::to_string(p.avatar_bg) + "\n";
    }
    std::string tmp = path() + ".tmp";
    if (SDL_SaveFile(tmp.c_str(), s.data(), s.size())) SDL_RenamePath(tmp.c_str(), path().c_str());
}

}  // namespace sanctum
