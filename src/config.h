// The player's own settings: %APPDATA%\SteelTemple\Sanctum\settings.ini (SDL_GetPrefPath), key=value lines.
// Never next to the program and never in a release: a package carries defaults only.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

namespace sanctum {

struct PlayerProfile {
    std::string name = "Player 1";
    std::string avatar = "Sword";
    int avatar_bg = 0;
};

struct Config {
    int theme = 0;                // THEME_DARK / THEME_LIGHT
    bool follow = true;           // the focused port tints Home
    bool large_tiles = true;
    int home_sort = 0;            // 0 last played, 1 most played, 2 A-Z
    int border_style = 0;         // 0 Switch 2 Aurora, 1 Rainbow Spectrum, 2 Port Accent Glow, 3 Classic Cyan
    int border_speed = 2;         // 0 Off (static), 1 Calm, 2 Switch 2, 3 Lively
    int tile_motion = 2;          // 0 Off, 1 Subtle, 2 Switch 2 Elastic (spring glide + micro-bounce)
    int dock_style = 0;           // 0 Switch 2 Floating Pill (colored icons), 1 Classic Circles
    std::set<std::string> pinned; // port ids pinned to the front of Home
    bool sounds = true;
    int sound_volume = 1;         // 0 low, 1 medium, 2 high
    int sound_theme = 0;          // 0 Switch 2 Clicky, 1 Switch 1 Bubbly, 2 Warm Retro, 3 Soft Minimal
    bool sound_pitch_pan = true;  // subtle Switch 2 dynamic pitch stepping & stereo panning on navigation
    int ambient_bgm = 0;          // 0 Off, 1 Soft console hum, 2 Custom WAV
    std::string bgm_path;         // custom .wav path when ambient_bgm == 2
    bool rumble = true;           // subtle controller vibration on focus & confirm
    int rumble_strength = 1;      // 0 low, 1 medium, 2 high
    int stick_deadzone = 1;       // 0 low (0.35), 1 default (0.55), 2 high (0.72)
    bool force_close_chord = true;// hold Back+Start+L+R for 2 s while a game runs to close it
    bool guide_overlay = true;    // Guide / PS / Home button opens Sanctum's Quick Overlay (in Sanctum & in-game)
    bool guide_override_gamebar = true; // temporarily suppresses Windows' Xbox Game Bar controller button while open
    int perf_mode = 1;            // 0 Off, 1 Auto (boost while gaming), 2 Always On
    bool perf_power_plan = true;  // switch to High Performance Windows power plan/overlay on AC power
    bool perf_trim_ram = true;    // drop Sanctum to EcoQoS + trim working set RAM while minimized in-game
    int perf_hud = 0;             // 0 Off, 1 Compact (FPS), 2 Detailed (FPS + CPU + GPU + RAM + Battery)
    std::string save_sync_dir;    // optional cloud-synced folder (OneDrive / Google Drive / Syncthing / NAS)
    bool save_sync_auto = true;   // auto-mirror save snapshot to save_sync_dir after each session
    std::map<std::string, int> refresh_hz; // port id -> optional target display Hz while running (0 = system default)
    bool hide_dev = false;        // leave out copies inside build / staging / backup folders (they rank last anyway)
    std::map<std::string, std::string> prefer;   // port id -> the copy the player chose ("Use this copy")
    bool fullscreen = true;       // starts fullscreen, like a console
    bool hide_cursor = true;      // the pointer hides while a pad is in use
    bool clock_12h = false;       // status bar clock: false = 24-hour, true = 12-hour AM/PM
    std::string display_name;     // empty = primary display; else SDL display name to open on
    int idle_dim_min = 0;         // OLED idle dim timeout: 0 off, 5 min, 15 min
    // like the ports' F1 menus: which face button confirms, and which buttons the hints show
    int pad_layout = 0;           // 0 automatic (the label printed on the button), 1 Nintendo (A right), 2 Xbox (A bottom)
    int button_icons = 0;         // 0 automatic (the pad's own), 1 Nintendo, 2 Xbox, 3 PlayStation
    std::string cover_dir;        // the player's own folder of cover art; empty until they pick one
    std::string screenshots_dir;  // optional extra folder for Album screenshots
    std::string library_dir;      // where Get ports installs; empty: <home>\Games\SteelTemple
    std::string profile = "Player 1";
    bool auto_update = true;      // Sanctum downloads its own updates and puts them in place when it closes
    bool discord_rpc = false;     // opt-in Discord Rich Presence (off by default for privacy)
    std::string discord_app_id;   // optional Discord application ID override
    std::string last_seen_version;// last version whose What's New card was seen (empty on first install)
    std::string avatar = "Sword"; // the profile icon, by its name in assets/avatars.txt
    int avatar_bg = 0;            // its background colour (avatars.cpp, kColours)
    std::vector<PlayerProfile> profiles = {{"Player 1", "Sword", 0}};
    int active_profile = 0;
    std::map<std::string, std::string> port_covers; // port id -> custom single cover image path
    std::map<std::string, std::string> launch_args; // port id -> custom extra command-line args
    std::map<std::string, std::string> launch_env;  // port id -> custom KEY=VAL;KEY2=VAL2 overrides
};

Config &cfg();
void config_load();
void config_save();
std::string config_dir();         // with a trailing separator
void profile_sync_to_active();
void profile_switch(int index);
int profile_add();                // adds up to 4 profiles, switches to it, returns index (or -1 if full)
bool profile_delete(int index);   // deletes a profile when more than one exists

}  // namespace sanctum
