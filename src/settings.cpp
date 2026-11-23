// System Settings (Switch 2 layout: categories on the left, rows on the right), the controller folder picker,
// the live dual-stick & trigger Controllers tester, Storage overview, and placeholder screen.
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

#include "cache.h"
#include "config.h"
#include "covers.h"
#include "discord_rpc.h"
#include "draw.h"
#include "github.h"
#include "input.h"
#include "install.h"
#include "launch.h"
#include "overlay.h"
#include "perf.h"
#include "platform.h"
#include "selfupdate.h"
#include "sound.h"
#include "sync.h"

namespace sanctum {

// ---- async storage scanner for System Settings > Storage
struct StorEntry {
    bool ready = false, running = false;
    long long bytes = 0;
    int files = 0;
};
static std::mutex g_stor_lock;
static std::map<std::string, StorEntry> g_stor_sizes;

static int stor_worker(void *p) {
    auto *dir = (std::string *)p;
    int files = 0;
    long long bytes = folder_size_bytes(*dir, &files);
    cache_size_put(*dir, bytes, files);
    {
        std::lock_guard<std::mutex> l(g_stor_lock);
        g_stor_sizes[*dir] = {true, false, bytes, files};
    }
    delete dir;
    return 0;
}

static StorEntry stor_size_of(const std::string &dir, bool force = false) {
    if (dir.empty()) return {true, false, 0, 0};
    std::lock_guard<std::mutex> l(g_stor_lock);
    StorEntry &e = g_stor_sizes[dir];
    if (!force && !e.ready) {
        long long cb = 0;
        int cf = 0;
        if (cache_size_get(dir, cb, cf)) {
            e = {true, false, cb, cf};
            return e;
        }
    }
    if (!e.running && (!e.ready || force)) {
        if (force) cache_size_invalidate(dir);
        e.running = true;
        if (force) e.ready = false;
        SDL_Thread *t = SDL_CreateThread(stor_worker, "storsize", new std::string(dir));
        if (t) SDL_DetachThread(t);
        else e.running = false;
    }
    return e;
}

static CacheStats g_cache_st{};
static double g_cache_at = -100.0;
static double g_clear_cache_armed = -10.0;

static const CacheStats &cached_cache_stats(bool force = false) {
    if (demo_mode()) {
        static const CacheStats mock{4200000, 8, 380000, 0, 4580000, 8, 0};
        return mock;
    }
    if (force || app().now - g_cache_at > 3.0) {
        g_cache_st = cache_stats();
        g_cache_at = app().now;
    }
    return g_cache_st;
}

static std::vector<DriveInfo> g_drives;
static double g_drives_at = -100.0;

static const std::vector<DriveInfo> &cached_drives(bool force = false) {
    if (demo_mode()) {
        static const std::vector<DriveInfo> mock = {
            {'C', "System SSD", "NTFS", (long long)(931.5 * 1024.0 * 1024.0 * 1024.0),
             (long long)(412.8 * 1024.0 * 1024.0 * 1024.0), false},
            {'D', "Games SSD", "NTFS", (long long)(1863.0 * 1024.0 * 1024.0 * 1024.0),
             (long long)(1248.4 * 1024.0 * 1024.0 * 1024.0), false},
        };
        return mock;
    }
    if (force || g_drives.empty() || app().now - g_drives_at > 3.0) {
        g_drives = list_drives();
        g_drives_at = app().now;
    }
    return g_drives;
}

// ======================= System Settings
enum Cat { CAT_DISPLAY, CAT_SOUND, CAT_THEMES, CAT_COVERS, CAT_LIBRARY, CAT_STORAGE, CAT_PERF, CAT_CONTROLLERS, CAT_ABOUT, CAT_N };
static const char *kCatName[] = {"Display", "Sound", "Themes", "Cover art", "Ports on this PC", "Storage", "Performance", "Controllers", "About Sanctum"};
static const Icon kCatIcon[] = {IC_DISPLAY, IC_SOUND, IC_PALETTE, IC_FOLDER, IC_LIBRARY, IC_STORAGE, IC_UP, IC_PAD, IC_INFO};
static int g_cat = 0, g_row = 0;
static float g_scroll = 0.0f;
static bool g_in_rows = false;
static bool g_startup_loaded = false, g_startup_on = false;

struct Row {
    std::string label, value;
    int id;          // what A does
    bool toggle = false, on = false;
    float bar_frac = -1.0f;    // primary bar (port usage OR drive total used fraction)
    ImU32 bar_col = 0;
    int aux = -1;              // port index or drive letter
    std::string sub, badge, badge2;
    float bar_frac2 = -1.0f;   // overlaid secondary bar (SteelTemple slice on a drive)
    ImU32 val_col = 0;
    bool drive_card = false;
    bool section_hdr = false;
};
enum {
    R_THEME_PRESET, R_THEME, R_FOLLOW, R_TILES, R_HOME_SORT,
    R_BORDER_STYLE, R_BORDER_SPEED, R_TILE_MOTION, R_DOCK_STYLE,
    R_FULLSCREEN, R_DISPLAY_PICK, R_CLOCK_12H, R_IDLE_DIM, R_HIDE_CURSOR,
    R_SOUNDS, R_SOUND_VOL, R_SOUND_THEME, R_SOUND_PITCH, R_AMBIENT_BGM, R_BGM_FILE,
    R_FOLDER, R_SCREENSHOTS_DIR, R_RELOAD,
    R_SEARCH, R_DEV, R_LIBDIR, R_ADD_PORT, R_ADD_CUSTOM, R_PORT,
    R_STOR_DRIVE, R_STOR_CLEAR_DL, R_STOR_OPEN_BAK, R_STOR_PORT,
    R_SYNC_DIR, R_SYNC_AUTO, R_SYNC_ALL, R_SYNC_CLEAR,
    R_PERF_MODE, R_PERF_HUD, R_PERF_POWER, R_PERF_TRIM, R_PERF_CLEAR_CACHE,
    R_PADS, R_LAYOUT, R_ICONS, R_RUMBLE, R_RUMBLE_STR, R_DEADZONE, R_GUIDE_OVERLAY, R_GUIDE_GAMEBAR, R_FORCE_CHORD,
    R_UPDATE, R_AUTO_UPDATE, R_DISCORD_RPC, R_STARTUP, R_SHORTCUT_DESKTOP, R_SHORTCUT_START, R_COPY_DIAG,
    R_INFO
};

static std::vector<std::string> connected_display_names() {
    std::vector<std::string> out;
    int cnt = 0;
    if (SDL_DisplayID *ids = SDL_GetDisplays(&cnt)) {
        for (int i = 0; i < cnt; ++i) {
            const char *nm = SDL_GetDisplayName(ids[i]);
            std::string s = nm && *nm ? nm : ("Display " + std::to_string(i + 1));
            out.push_back(s);
        }
        SDL_free(ids);
    }
    return out;
}

static std::string build_support_info() {
    const SystemDiag &d = system_diagnostics();
    std::string s = "SteelTemple Sanctum " + std::string(sanctum_version()) + "\n";
    s += "OS: " + d.os + "\n";
    s += "CPU: " + d.cpu + "\n";
    s += "RAM: " + std::to_string(d.ram_gb) + " GB\n";
    s += "GPU: " + d.gpu + (d.gpu_driver.empty() ? "" : (" (Driver " + d.gpu_driver + ")")) + "\n";
    int cnt = 0;
    if (SDL_JoystickID *ids = SDL_GetGamepads(&cnt)) {
        s += "Controllers: " + std::to_string(cnt);
        for (int i = 0; i < cnt; ++i)
            if (SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i])) {
                const char *nm = SDL_GetGamepadName(g);
                if (nm) s += std::string(i == 0 ? " (" : ", ") + nm;
            }
        if (cnt > 0) s += ")";
        s += "\n";
        SDL_free(ids);
    }
    s += "Installed ports:\n";
    for (const Port &p : ports()) {
        if (p.installs.empty()) continue;
        s += "  - " + std::string(p.title) + " (" + p.name + "): " +
             (p.installs[0].version.empty() ? "version unknown" : p.installs[0].version) + "\n";
    }
    return s;
}

static std::vector<Row> rows_of(int cat) {
    std::vector<Row> r;
    Config &c = cfg();
    switch (cat) {
    case CAT_DISPLAY: {
        if (!g_startup_loaded) { g_startup_loaded = true; g_startup_on = demo_mode() ? false : startup_is_enabled(); }
        r.push_back({"Fullscreen", "", R_FULLSCREEN, true, c.fullscreen});
        std::string mon_lbl = c.display_name.empty() ? "Automatic (Primary)" : c.display_name;
        if (demo_mode()) mon_lbl = "Automatic (Primary)";
        r.push_back({"Monitor / TV", mon_lbl, R_DISPLAY_PICK});
        r.push_back({"Start Sanctum with Windows (Console Mode)", "", R_STARTUP, true, g_startup_on});
        r.push_back({"12-hour clock", "", R_CLOCK_12H, true, c.clock_12h});
        std::string dim_lbl = c.idle_dim_min == 5 ? "After 5 min" : c.idle_dim_min == 15 ? "After 15 min" : "Off";
        r.push_back({"Idle dim (OLED protection)", dim_lbl, R_IDLE_DIM});
        r.push_back({"Auto-hide mouse cursor", "", R_HIDE_CURSOR, true, c.hide_cursor});
        SDL_DisplayID d = SDL_GetDisplayForWindow(app().window);
        const SDL_DisplayMode *m = d ? SDL_GetDesktopDisplayMode(d) : nullptr;
        const char *name = d && !demo_mode() ? SDL_GetDisplayName(d) : nullptr;
        if (m) r.push_back({"Active screen", std::string(name ? name : "Display") + "  ·  " + std::to_string(m->w) + " x " +
                                                 std::to_string(m->h) + "  ·  " + std::to_string((int)(m->refresh_rate + 0.5f)) + " Hz", R_INFO});
        {
            std::string hz_hint = "Set per title in Game > Files & storage";
            if (!demo_mode()) {
                auto rates = display_supported_refresh_rates();
                if (!rates.empty()) {
                    std::string list;
                    for (size_t i = 0; i < rates.size(); ++i) {
                        if (i) list += " / ";
                        list += std::to_string(rates[i]);
                    }
                    hz_hint = list + " Hz  ·  set per game";
                }
            }
            r.push_back({"Per-game refresh rate", hz_hint, R_INFO});
        }
        r.push_back({"Shortcut", "F11 or Alt+Enter switches fullscreen at any time", R_INFO});
        break;
    }
    case CAT_SOUND: {
        static const char *const vols[] = {"Low", "Medium", "High"};
        static const char *const themes[] = {"Tactile Click", "Bubble Pop", "Warm Retro", "Soft Minimal"};
        static const char *const bgms[] = {"Off", "Soft console hum", "Custom WAV"};
        r.push_back({"Menu sounds", "", R_SOUNDS, true, c.sounds});
        r.push_back({"Sound profile", themes[SDL_clamp(c.sound_theme, 0, 3)], R_SOUND_THEME});
        r.push_back({"Dynamic pitch & stereo pan", "", R_SOUND_PITCH, true, c.sound_pitch_pan});
        r.push_back({"Volume", vols[c.sound_volume % 3], R_SOUND_VOL});
        r.push_back({"Home ambient audio", bgms[c.ambient_bgm % 3], R_AMBIENT_BGM});
        std::string bname = "Choose .wav...";
        if (!c.bgm_path.empty() && !demo_mode()) {
            size_t sl = c.bgm_path.find_last_of("\\/");
            bname = sl == std::string::npos ? c.bgm_path : c.bgm_path.substr(sl + 1);
        }
        r.push_back({"Custom ambient WAV file", bname, R_BGM_FILE});
        r.push_back({"Audio behaviour", "Pauses automatically while a game runs", R_INFO});
        break;
    }
    case CAT_THEMES: {
        static const char *const sorts[] = {"Last played", "Most played", "A to Z"};
        static const char *const bstyles[] = {"Aurora Prism", "Spectrum Wave", "Port Accent", "Classic Cyan"};
        static const char *const bspeeds[] = {"Static", "Calm", "Standard", "Lively"};
        static const char *const tmotions[] = {"Off", "Subtle", "Elastic"};
        static const char *const vols[] = {"Low", "Medium", "High"};
        static const char *const sthemes[] = {"Tactile Click", "Bubble Pop", "Warm Retro", "Soft Minimal"};
        static const char *const bgms[] = {"Off", "Soft console hum", "Custom WAV"};
        auto cur_preset = [&]() -> const char * {
            if (c.border_style == 0 && c.border_speed == 2 && c.tile_motion == 2 && c.dock_style == 0 && c.sound_theme == 0)
                return "Sanctum Prism";
            if (c.border_style == 3 && c.border_speed == 1 && c.tile_motion == 1 && c.dock_style == 1 && c.sound_theme == 1)
                return "Aqua Bubble";
            if (c.border_style == 2 && c.border_speed == 2 && c.tile_motion == 2 && c.dock_style == 0 && c.sound_theme == 0)
                return "Port Accent";
            if (c.border_style == 3 && c.border_speed == 0 && c.tile_motion == 0 && c.dock_style == 1 && c.sound_theme == 3)
                return "Clean Minimal";
            return "Custom";
        };
        {
            Row hdr;
            hdr.label = "APPEARANCE & PRESET";
            hdr.value = "Style, animations & sound pack";
            hdr.id = R_INFO;
            hdr.section_hdr = true;
            r.push_back(std::move(hdr));
        }
        r.push_back({"Theme preset", cur_preset(), R_THEME_PRESET});
        r.push_back({"Color mode", c.theme ? "Light" : "Dark", R_THEME});
        r.push_back({"Home follows the selected port's colours", "", R_FOLLOW, true, c.follow});
        r.push_back({"Home tile size", c.large_tiles ? "Large" : "Medium", R_TILES});
        r.push_back({"Home sort order", sorts[c.home_sort % 3], R_HOME_SORT});

        {
            Row hdr;
            hdr.label = "BORDER & ANIMATIONS";
            hdr.value = "Live preview on active highlight";
            hdr.id = R_INFO;
            hdr.section_hdr = true;
            r.push_back(std::move(hdr));
        }
        r.push_back({"Selection border style", bstyles[SDL_clamp(c.border_style, 0, 3)], R_BORDER_STYLE});
        r.push_back({"Border animation speed", bspeeds[SDL_clamp(c.border_speed, 0, 3)], R_BORDER_SPEED});
        r.push_back({"Tile motion & bounce", tmotions[SDL_clamp(c.tile_motion, 0, 2)], R_TILE_MOTION});
        r.push_back({"Home dock style", c.dock_style == 0 ? "Floating Pill" : "Classic Circles", R_DOCK_STYLE});

        {
            Row hdr;
            hdr.label = "MENU SOUNDS & AUDIO";
            hdr.value = c.sounds ? sthemes[SDL_clamp(c.sound_theme, 0, 3)] : "Muted";
            hdr.id = R_INFO;
            hdr.section_hdr = true;
            r.push_back(std::move(hdr));
        }
        r.push_back({"Menu sounds", "", R_SOUNDS, true, c.sounds});
        r.push_back({"Sound profile", sthemes[SDL_clamp(c.sound_theme, 0, 3)], R_SOUND_THEME});
        r.push_back({"Dynamic pitch & stereo pan", "", R_SOUND_PITCH, true, c.sound_pitch_pan});
        r.push_back({"Sound volume", vols[c.sound_volume % 3], R_SOUND_VOL});
        r.push_back({"Home ambient audio", bgms[c.ambient_bgm % 3], R_AMBIENT_BGM});
        std::string bname = "Choose .wav...";
        if (!c.bgm_path.empty() && !demo_mode()) {
            size_t sl = c.bgm_path.find_last_of("\\/");
            bname = sl == std::string::npos ? c.bgm_path : c.bgm_path.substr(sl + 1);
        }
        r.push_back({"Custom ambient WAV file", bname, R_BGM_FILE});
        r.push_back({"Pinned titles", c.pinned.empty() ? "None (press X on a Home tile to pin)" : (std::to_string(c.pinned.size()) + " pinned to front"), R_INFO});
        break;
    }
    case CAT_COVERS:
        r.push_back({"Cover art folder", c.cover_dir.empty() ? "Choose..." : c.cover_dir, R_FOLDER});
        r.push_back({"Ports with cover art", std::to_string(covers_matched()) + " of " + std::to_string(ports().size()), R_INFO});
        r.push_back({"Read the folder again", "", R_RELOAD});
        r.push_back({"Album screenshots folder", c.screenshots_dir.empty() ? "Auto (ports + Pictures)" : c.screenshots_dir, R_SCREENSHOTS_DIR});
        break;
    case CAT_LIBRARY:
        r.push_back({scan_running() ? "Searching this PC..." : "Search this PC again", "", R_SEARCH});
        r.push_back({"Install new ports into", shown_path(library_dir()), R_LIBDIR});
        r.push_back({"Add a port folder manually", "Choose folder...", R_ADD_PORT});
        r.push_back({"Add a custom PC game", "Choose .exe...", R_ADD_CUSTOM});
        r.push_back({"Include copies in build and backup folders", "", R_DEV, true, !c.hide_dev});
        for (int i = 0; i < (int)ports().size(); ++i) {
            auto &p = ports()[i];
            std::string dir = p.installs.empty() ? "" : shown_path(p.installs[0].dir, p.title);
            std::string v = p.installs.empty() ? (p.hidden ? "Only in build folders" : "Not found")
                          : p.installs.size() == 1 || demo_mode() ? dir
                          : std::to_string(p.installs.size()) + " copies, using " + dir;
            Row row{p.title, v, R_PORT};
            row.aux = i;
            r.push_back(std::move(row));
        }
        break;
    case CAT_STORAGE: {
        long long total_ports = 0, max_port = 1;
        struct Item { int idx; long long bytes; bool ready; char drive; };
        std::vector<Item> items;
        std::map<char, long long> lib_per_drive;
        for (int i = 0; i < (int)ports().size(); ++i) {
            const Port &p = ports()[i];
            if (p.installs.empty()) continue;
            StorEntry se = demo_mode() ? StorEntry{true, false, (long long)(850 + i * 310) * 1024 * 1024, 180}
                                       : stor_size_of(p.installs[0].dir);
            char drv = demo_mode() ? 'D'
                     : (p.installs[0].dir.size() >= 2 && p.installs[0].dir[1] == ':' ? (char)SDL_toupper((unsigned char)p.installs[0].dir[0]) : 0);
            if (se.ready) {
                total_ports += se.bytes;
                if (se.bytes > max_port) max_port = se.bytes;
                if (drv) lib_per_drive[drv] += se.bytes;
            }
            items.push_back({i, se.bytes, se.ready, drv});
        }
        std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.bytes > b.bytes; });

        // ---- Drives section
        const auto &drives = cached_drives();
        long long all_total = 0, all_free = 0;
        for (const DriveInfo &di : drives) { all_total += di.total_bytes; all_free += di.free_bytes; }
        std::string lib_path = shown_path(library_dir());
        char lib_drv = lib_path.size() >= 2 && lib_path[1] == ':' ? (char)SDL_toupper((unsigned char)lib_path[0]) : 0;

        if (!drives.empty()) {
            Row hdr;
            hdr.label = "DRIVES";
            hdr.value = std::to_string(drives.size()) + (drives.size() == 1 ? " drive  ·  " : " drives  ·  ") +
                        mb(all_free) + " free of " + mb(all_total);
            hdr.id = R_INFO;
            hdr.section_hdr = true;
            r.push_back(std::move(hdr));

            for (const DriveInfo &di : drives) {
                long long used = std::max(0LL, di.total_bytes - di.free_bytes);
                long long lib_b = lib_per_drive.count(di.letter) ? lib_per_drive[di.letter] : 0;
                long long other_b = std::max(0LL, used - lib_b);
                int pct_used = di.total_bytes > 0 ? (int)((used * 100LL + di.total_bytes / 2) / di.total_bytes) : 0;
                int pct_free = std::max(0, 100 - pct_used);
                float used_frac = di.total_bytes > 0 ? (float)((double)used / (double)di.total_bytes) : 0.0f;
                float lib_frac = di.total_bytes > 0 ? (float)((double)std::min(lib_b, used) / (double)di.total_bytes) : 0.0f;

                Row card;
                card.label = di.label + " (" + std::string(1, di.letter) + ":)";
                card.value = mb(di.free_bytes) + " free of " + mb(di.total_bytes);
                card.badge = di.fs;
                if (di.letter == lib_drv) card.badge2 = "Library";
                else if (di.removable) card.badge2 = "Removable";
                if (lib_b > 0)
                    card.sub = "SteelTemple " + mb(lib_b) + "  ·  Other " + mb(other_b) + "  ·  " +
                               std::to_string(pct_used) + "% used (" + std::to_string(pct_free) + "% free)";
                else
                    card.sub = "Used " + mb(used) + " (" + std::to_string(pct_used) + "%)  ·  " +
                               std::to_string(pct_free) + "% free";
                card.id = R_STOR_DRIVE;
                card.aux = (int)(unsigned char)di.letter;
                card.drive_card = true;
                card.bar_frac = used_frac;
                card.bar_frac2 = lib_b > 0 ? lib_frac : 0.0f;
                const long long k10G = 10LL * 1024 * 1024 * 1024;
                const long long k25G = 25LL * 1024 * 1024 * 1024;
                const Palette &dp = pal();
                if (di.free_bytes < k10G || pct_free < 5)
                    card.val_col = dp.light ? IM_COL32(204, 44, 52, 255) : IM_COL32(244, 92, 98, 255);
                else if (di.free_bytes < k25G || pct_free < 12) card.val_col = dp.warn;
                else card.val_col = dp.ok;
                r.push_back(std::move(card));
            }
        }

        // ---- Cloud Save Sync Bridge section
        {
            Row hdr;
            hdr.label = "CLOUD SAVE SYNC";
            hdr.value = c.save_sync_dir.empty() ? "OneDrive · Google Drive · Syncthing · NAS"
                                                : (c.save_sync_auto ? "Auto-sync after sessions" : "Manual sync");
            hdr.id = R_INFO;
            hdr.section_hdr = true;
            r.push_back(std::move(hdr));
        }
        {
            Row sdir;
            sdir.label = "Cloud save sync folder";
            sdir.value = c.save_sync_dir.empty() ? "Not set (Choose folder...)" : shown_path(c.save_sync_dir);
            sdir.id = R_SYNC_DIR;
            if (!c.save_sync_dir.empty()) sdir.val_col = pal().soft;
            r.push_back(std::move(sdir));
        }
        if (!c.save_sync_dir.empty()) {
            r.push_back({"Auto-sync saves after playing", "", R_SYNC_AUTO, true, c.save_sync_auto});
            r.push_back({"Sync all local saves now", "Mirror to cloud folder", R_SYNC_ALL});
            r.push_back({"Turn off cloud save sync", "Keeps cloud files untouched", R_SYNC_CLEAR});
        }

        // ---- Library & cache section
        std::string dl_dir = local_dir() + "downloads";
        std::string bak_dir = config_dir() + "backups";
        StorEntry dl_se = demo_mode() ? StorEntry{true, false, 0, 0} : stor_size_of(dl_dir);
        StorEntry bak_se = demo_mode() ? StorEntry{true, false, 18 * 1024 * 1024, 4} : stor_size_of(bak_dir);

        {
            Row hdr;
            hdr.label = "STEELTEMPLE LIBRARY";
            hdr.value = total_ports > 0 ? (mb(total_ports) + " across " + std::to_string(items.size()) +
                                           (items.size() == 1 ? " title" : " titles"))
                                        : (items.empty() ? "No ports installed" : "Calculating...");
            hdr.id = R_INFO;
            hdr.section_hdr = true;
            r.push_back(std::move(hdr));
        }
        r.push_back({"Clear download cache", dl_se.ready ? (dl_se.bytes > 0 ? mb(dl_se.bytes) + "  ·  Clean" : "Empty") : "Checking...", R_STOR_CLEAR_DL});
        r.push_back({"Save backups folder", bak_se.ready ? (bak_se.bytes > 0 ? mb(bak_se.bytes) + "  ·  Open" : "No backups yet") : "Checking...", R_STOR_OPEN_BAK});

        for (const Item &it : items) {
            const Port &p = ports()[it.idx];
            Row row;
            row.label = p.title;
            row.value = it.ready ? mb(it.bytes) : "Calculating...";
            if (it.drive) row.badge = std::string(1, it.drive) + ":";
            row.id = R_STOR_PORT;
            row.aux = it.idx;
            row.bar_frac = it.ready ? (float)it.bytes / (float)max_port : 0.0f;
            row.bar_col = p.colours.accent;
            r.push_back(std::move(row));
        }
        break;
    }
    case CAT_PERF: {
        static const char *const pmodes[] = {"Off", "Auto (while gaming)", "Always on"};
        static const char *const huds[] = {"Off", "Compact (FPS)", "Detailed (FPS · CPU · GPU · RAM)"};
        {
            Row hdr;
            hdr.label = "GAMING BOOST";
            hdr.value = perf_game_boost_active() ? "Active now" : "Reversible · zero admin";
            hdr.id = R_INFO;
            hdr.section_hdr = true;
            r.push_back(std::move(hdr));
        }
        r.push_back({"Performance mode", pmodes[c.perf_mode % 3], R_PERF_MODE});
        r.push_back({"In-game performance HUD", huds[SDL_clamp(c.perf_hud, 0, 2)], R_PERF_HUD});
        r.push_back({"High-Performance Windows power plan (AC power)", "", R_PERF_POWER, true, c.perf_power_plan});
        r.push_back({"Trim Sanctum RAM & EcoQoS while in-game", "", R_PERF_TRIM, true, c.perf_trim_ram});
        r.push_back({"Active boost tweaks", "Above Normal · EcoQoS off · 1 ms timer · Sleep off", R_INFO});

        const CacheStats &cs = cached_cache_stats();
        {
            Row hdr;
            hdr.label = "SMART CACHE";
            hdr.value = mb(cs.total_bytes) + " total on disk";
            hdr.id = R_INFO;
            hdr.section_hdr = true;
            r.push_back(std::move(hdr));
        }
        std::string th_val = mb(cs.thumb_bytes) + "  ·  " + std::to_string(cs.thumb_files) + " thumbs";
        if (cs.thumb_hits > 0) th_val += " (" + std::to_string(cs.thumb_hits) + " instant hits)";
        r.push_back({"Cover & Album thumbnail cache", th_val, R_INFO});
        r.push_back({"Catalog, releases & folder-size index", mb(cs.meta_bytes) + "  ·  Instant load", R_INFO});
        r.push_back({"Download package cache", cs.download_bytes > 0 ? mb(cs.download_bytes) : "Empty", R_INFO});
        bool armed = app().now - g_clear_cache_armed < 4.0;
        r.push_back({"Clear Sanctum caches", armed ? "Press again to clear (keeps saves & games)" : "Thumbnails, size index & downloads", R_PERF_CLEAR_CACHE});
        break;
    }
    case CAT_CONTROLLERS: {
        static const char *const layouts[] = {"Automatic", "Nintendo (A right)", "Xbox (A bottom)"};
        static const char *const icons[] = {"Automatic", "Nintendo", "Xbox", "PlayStation"};
        static const char *const strengths[] = {"Low", "Medium", "High"};
        static const char *const dzs[] = {"Low (35%)", "Default (55%)", "High (72%)"};
        r.push_back({"Button layout", layouts[c.pad_layout % 3], R_LAYOUT});
        r.push_back({"Button icons", icons[c.button_icons % 4], R_ICONS});
        r.push_back({"Controller vibration", "", R_RUMBLE, true, c.rumble});
        r.push_back({"Vibration strength", strengths[c.rumble_strength % 3], R_RUMBLE_STR});
        r.push_back({"Menu stick deadzone", dzs[c.stick_deadzone % 3], R_DEADZONE});
        r.push_back({"Guide / Home button Quick Overlay", "", R_GUIDE_OVERLAY, true, c.guide_overlay});
        if (c.guide_overlay)
            r.push_back({"Override Xbox Game Bar button while open", "", R_GUIDE_GAMEBAR, true, c.guide_override_gamebar});
        r.push_back({"Force-close chord (Back + Start + L + R for 2 s)", "", R_FORCE_CHORD, true, c.force_close_chord});
        r.push_back({"Test controllers", "Sticks, triggers, buttons & battery", R_PADS});
        if (!demo_mode() && is_steam_running())
            r.push_back({"Steam detected", "Turn off 'Guide Button Focuses Steam' in Steam > Controller", R_INFO});
        r.push_back({"Automatic layout", "A confirms where each pad prints it", R_INFO});
        break;
    }
    case CAT_ABOUT: {
        if (!g_startup_loaded) { g_startup_loaded = true; g_startup_on = demo_mode() ? false : startup_is_enabled(); }
        r.push_back({"SteelTemple Sanctum", std::string("Version ") + sanctum_version(), R_INFO});
        {
            std::string t = self_update_tag(), v;
            switch (self_update_state()) {
            case UPD_IDLE: v = "Check for updates"; break;
            case UPD_CHECKING: v = "Checking GitHub..."; break;
            case UPD_CURRENT: v = "Up to date  ·  check again"; break;
            case UPD_AVAILABLE: v = t + " is out  ·  download it"; break;
            case UPD_DOWNLOADING: v = "Downloading " + t + "  " + std::to_string((int)(self_update_progress() * 100)) + "%"; break;
            case UPD_READY: v = t + " is ready  ·  restart and update"; break;
            case UPD_FAILED: v = fit(16, self_update_error(), 520) + "  ·  try again"; break;
            }
            r.push_back({"Updates", demo_mode() ? "Up to date" : v, R_UPDATE});
        }
        r.push_back({"Update Sanctum automatically", "", R_AUTO_UPDATE, true, c.auto_update});
        r.push_back({"Discord Rich Presence", "", R_DISCORD_RPC, true, c.discord_rpc});
        r.push_back({"Start Sanctum with Windows", "", R_STARTUP, true, g_startup_on});
        r.push_back({"Desktop shortcut", "Create / repair", R_SHORTCUT_DESKTOP});
        r.push_back({"Start Menu shortcut", "Create / repair", R_SHORTCUT_START});
        const SystemDiag &sd = system_diagnostics();
        r.push_back({"Copy support info", "Copies specs & port versions to clipboard", R_COPY_DIAG});
        r.push_back({"GPU", demo_mode() ? "DirectX / Vulkan GPU" : sd.gpu, R_INFO});
        r.push_back({"CPU & RAM", demo_mode() ? "Desktop CPU  ·  32 GB RAM" : (sd.cpu + "  ·  " + std::to_string(sd.ram_gb) + " GB RAM"), R_INFO});
        r.push_back({"Operating system", demo_mode() ? "Windows 11" : sd.os, R_INFO});
        r.push_back({"Settings folder", demo_mode() ? "%APPDATA%\\SteelTemple\\Sanctum" : config_dir(), R_INFO});
        r.push_back({"Game files", "Never downloaded or shipped: every port uses your own copy", R_INFO});
        break;
    }
    }
    return r;
}

static void act(const Row &r) {
    Config &c = cfg();
    switch (r.id) {
    case R_THEME_PRESET: {
        int cur = -1;
        if (c.border_style == 0 && c.border_speed == 2 && c.tile_motion == 2 && c.dock_style == 0 && c.sound_theme == 0) cur = 0;
        else if (c.border_style == 3 && c.border_speed == 1 && c.tile_motion == 1 && c.dock_style == 1 && c.sound_theme == 1) cur = 1;
        else if (c.border_style == 2 && c.border_speed == 2 && c.tile_motion == 2 && c.dock_style == 0 && c.sound_theme == 0) cur = 2;
        else if (c.border_style == 3 && c.border_speed == 0 && c.tile_motion == 0 && c.dock_style == 1 && c.sound_theme == 3) cur = 3;
        int next = (cur + 1) % 4;
        static const char *const kNames[] = {"Sanctum Prism", "Aqua Bubble", "Port Accent", "Clean Minimal"};
        if (next == 0) { c.border_style = 0; c.border_speed = 2; c.tile_motion = 2; c.dock_style = 0; c.sound_theme = 0; c.sound_pitch_pan = true; }
        else if (next == 1) { c.border_style = 3; c.border_speed = 1; c.tile_motion = 1; c.dock_style = 1; c.sound_theme = 1; c.sound_pitch_pan = true; }
        else if (next == 2) { c.border_style = 2; c.border_speed = 2; c.tile_motion = 2; c.dock_style = 0; c.sound_theme = 0; c.sound_pitch_pan = true; }
        else { c.border_style = 3; c.border_speed = 0; c.tile_motion = 0; c.dock_style = 1; c.sound_theme = 3; c.sound_pitch_pan = false; }
        sound_set_theme(c.sound_theme);
        sound_play(SFX_OK);
        toast(std::string("Theme preset: ") + kNames[next]);
        break;
    }
    case R_THEME: c.theme ^= 1; theme_set(c.theme); sound_play(SFX_OK); break;
    case R_FOLLOW: c.follow = !c.follow; sound_play(c.follow ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); break;
    case R_TILES: c.large_tiles = !c.large_tiles; sound_play(SFX_OK); break;
    case R_HOME_SORT: c.home_sort = (c.home_sort + 1) % 3; sound_play(SFX_OK); break;
    case R_BORDER_STYLE: {
        static const char *const kB[] = {"Aurora Prism", "Spectrum Wave", "Port Accent", "Classic Cyan"};
        c.border_style = (c.border_style + 1) % 4;
        sound_play(SFX_OK);
        toast(std::string("Selection border: ") + kB[c.border_style]);
        break;
    }
    case R_BORDER_SPEED: {
        static const char *const kS[] = {"Static", "Calm", "Standard", "Lively"};
        c.border_speed = (c.border_speed + 1) % 4;
        sound_play(SFX_OK);
        toast(std::string("Border animation: ") + kS[c.border_speed]);
        break;
    }
    case R_TILE_MOTION: {
        static const char *const kM[] = {"Off", "Subtle", "Elastic"};
        c.tile_motion = (c.tile_motion + 1) % 3;
        sound_play(SFX_OK);
        toast(std::string("Tile motion: ") + kM[c.tile_motion]);
        break;
    }
    case R_DOCK_STYLE:
        c.dock_style ^= 1;
        sound_play(SFX_OK);
        toast(c.dock_style == 0 ? "Home dock: Floating Pill" : "Home dock: Classic Circles");
        break;
    case R_FULLSCREEN: set_fullscreen(!c.fullscreen); sound_play(c.fullscreen ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); return;
    case R_DISPLAY_PICK: {
        auto mons = connected_display_names();
        if (mons.empty()) { sound_play(SFX_ERROR); return; }
        int cur = -1;
        for (int i = 0; i < (int)mons.size(); ++i) if (mons[i] == c.display_name) { cur = i; break; }
        if (cur + 1 < (int)mons.size()) c.display_name = mons[cur + 1];
        else c.display_name.clear();   // wrap back to Automatic (Primary)
        apply_display(c.display_name);
        sound_play(SFX_OK);
        toast(c.display_name.empty() ? "Display: Automatic (Primary)" : ("Moved to " + c.display_name));
        break;
    }
    case R_CLOCK_12H: c.clock_12h = !c.clock_12h; sound_play(c.clock_12h ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); break;
    case R_IDLE_DIM:
        c.idle_dim_min = c.idle_dim_min == 0 ? 5 : c.idle_dim_min == 5 ? 15 : 0;
        sound_play(SFX_OK);
        break;
    case R_HIDE_CURSOR:
        c.hide_cursor = !c.hide_cursor;
        if (c.hide_cursor) SDL_HideCursor();
        else SDL_ShowCursor();
        sound_play(c.hide_cursor ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
        break;
    case R_SOUNDS:
        c.sounds = !c.sounds;
        sound_set_enabled(c.sounds);
        if (c.sounds) sound_play(SFX_TOGGLE_ON);
        break;
    case R_SOUND_VOL:
        c.sound_volume = (c.sound_volume + 1) % 3;
        sound_set_volume(c.sound_volume);
        sound_play(SFX_OK);
        break;
    case R_SOUND_THEME: {
        static const char *const kSt[] = {"Tactile Click", "Bubble Pop", "Warm Retro", "Soft Minimal"};
        c.sound_theme = (c.sound_theme + 1) % 4;
        sound_set_theme(c.sound_theme);
        sound_play(SFX_OK);
        toast(std::string("Sound profile: ") + kSt[c.sound_theme]);
        break;
    }
    case R_SOUND_PITCH:
        c.sound_pitch_pan = !c.sound_pitch_pan;
        sound_play(c.sound_pitch_pan ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
        break;
    case R_AMBIENT_BGM:
        c.ambient_bgm = (c.ambient_bgm + 1) % 3;
        if (c.ambient_bgm == 2 && c.bgm_path.empty()) {
            std::string f = pick_open_file(app().window, "Choose ambient background audio (.wav)", "WAV audio", "*.wav");
            if (!f.empty()) c.bgm_path = f;
            else c.ambient_bgm = 0;
        }
        sound_set_ambient(c.ambient_bgm, c.bgm_path);
        sound_play(c.ambient_bgm > 0 ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
        break;
    case R_BGM_FILE: {
        if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: files unchanged"); return; }
        std::string f = pick_open_file(app().window, "Choose ambient background audio (.wav)", "WAV audio", "*.wav");
        if (!f.empty()) {
            c.bgm_path = f;
            c.ambient_bgm = 2;
            sound_set_ambient(c.ambient_bgm, c.bgm_path);
            sound_play(SFX_TOGGLE_ON);
            toast("Custom ambient WAV loaded");
        }
        break;
    }
    case R_FOLDER: folder_open(c.cover_dir, FOLDER_COVERS); return;
    case R_SCREENSHOTS_DIR: folder_open(c.screenshots_dir, FOLDER_SCREENSHOTS); return;
    case R_RELOAD: covers_reload(); toast("Reading your cover art"); sound_play(SFX_OK); return;
    case R_SEARCH: scan_start(); sound_play(SFX_OK); return;
    case R_DEV: c.hide_dev = !c.hide_dev; catalog_refilter(); sound_play(!c.hide_dev ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); break;
    case R_LIBDIR: folder_open(library_dir(), FOLDER_LIBRARY); return;
    case R_ADD_PORT: folder_open("", FOLDER_ADD_PORT); return;
    case R_ADD_CUSTOM: {
        if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: custom games unchanged"); return; }
        std::string exe = pick_open_file(app().window, "Choose a PC game executable", "Programs", "*.exe");
        if (!exe.empty()) {
            int idx = catalog_add_custom(exe);
            if (idx >= 0) {
                sound_play(SFX_TOGGLE_ON);
                toast("Added " + std::string(ports()[idx].title) + " to Home");
                app().game = idx;
                go(SCR_GAME);
                return;
            }
            sound_play(SFX_ERROR);
            toast("Could not add executable");
        }
        return;
    }
    case R_PORT:
        if (r.aux >= 0 && r.aux < (int)ports().size()) { app().game = r.aux; go(SCR_GAME); }
        return;
    case R_STOR_DRIVE: {
        if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: drives don't open"); return; }
        if (r.aux >= 'A' && r.aux <= 'Z') {
            std::string root = std::string("file:///") + (char)r.aux + ":/";
            sound_play(SFX_OK);
            SDL_OpenURL(root.c_str());
        }
        return;
    }
    case R_STOR_CLEAR_DL: {
        if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: cache unchanged"); return; }
        if (install_busy()) { sound_play(SFX_ERROR); toast("Wait for active downloads to finish first"); return; }
        std::string dl_dir = local_dir() + "downloads";
        remove_tree(dl_dir);
        SDL_CreateDirectory(dl_dir.c_str());
        stor_size_of(dl_dir, true);
        cached_drives(true);
        sound_play(SFX_TOGGLE_ON);
        toast("Download cache cleared");
        return;
    }
    case R_STOR_OPEN_BAK: {
        if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: folders don't open"); return; }
        std::string bdir = config_dir() + "backups";
        SDL_CreateDirectory(bdir.c_str());
        sound_play(SFX_OK);
        SDL_OpenURL(("file:///" + bdir).c_str());
        return;
    }
    case R_STOR_PORT:
        if (r.aux >= 0 && r.aux < (int)ports().size()) game_open_tab(r.aux, 4);
        return;
    case R_SYNC_DIR:
        folder_open(c.save_sync_dir, FOLDER_SAVE_SYNC);
        return;
    case R_SYNC_AUTO:
        c.save_sync_auto = !c.save_sync_auto;
        sound_play(c.save_sync_auto ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
        toast(c.save_sync_auto ? "Saves will mirror to cloud folder after playing" : "Cloud save sync set to manual");
        break;
    case R_SYNC_ALL: {
        if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: sync unchanged"); return; }
        int skipped = 0;
        int pushed = sync_push_all(&skipped);
        if (pushed > 0) {
            sound_play(SFX_TOGGLE_ON);
            std::string msg = "Synced " + std::to_string(pushed) + (pushed == 1 ? " title" : " titles") + " to cloud folder";
            if (skipped > 0) msg += " (" + std::to_string(skipped) + " newer in cloud — check Game page)";
            toast(msg, 4.0);
        } else if (skipped > 0) {
            sound_play(SFX_ERROR);
            toast(std::to_string(skipped) + (skipped == 1 ? " title has" : " titles have") + " a newer cloud save — pull on its Game page", 4.5);
        } else {
            sound_play(SFX_OK);
            toast("All local saves are already up to date");
        }
        return;
    }
    case R_SYNC_CLEAR:
        if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: sync unchanged"); return; }
        c.save_sync_dir.clear();
        sync_invalidate();
        sound_play(SFX_TOGGLE_OFF);
        toast("Cloud save sync turned off (cloud files untouched)");
        break;
    case R_PERF_MODE:
        c.perf_mode = (c.perf_mode + 1) % 3;
        perf_sync_idle();
        sound_play(c.perf_mode ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
        toast("Performance mode: " + perf_status_short());
        break;
    case R_PERF_HUD: {
        static const char *const huds[] = {"Off", "Compact (FPS)", "Detailed (FPS · CPU · GPU · RAM)"};
        c.perf_hud = (c.perf_hud + 1) % 3;
        overlay_sync_hud();
        sound_play(c.perf_hud ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
        toast(std::string("Performance HUD: ") + huds[c.perf_hud]);
        break;
    }
    case R_PERF_POWER:
        c.perf_power_plan = !c.perf_power_plan;
        perf_sync_idle();
        sound_play(c.perf_power_plan ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
        toast(c.perf_power_plan ? "High-Performance power plan enabled on AC power" : "Windows power plan left unchanged");
        break;
    case R_PERF_TRIM:
        c.perf_trim_ram = !c.perf_trim_ram;
        sound_play(c.perf_trim_ram ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
        break;
    case R_PERF_CLEAR_CACHE:
        if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: caches unchanged"); return; }
        if (app().now - g_clear_cache_armed < 4.0) {
            g_clear_cache_armed = -10.0;
            cache_clear_sanctum();
            {
                std::lock_guard<std::mutex> l(g_stor_lock);
                g_stor_sizes.clear();
            }
            cached_cache_stats(true);
            covers_reload();
            sound_play(SFX_TOGGLE_ON);
            toast("Cleared Sanctum thumbnail, size, and download caches", 3.5);
        } else {
            g_clear_cache_armed = app().now;
            sound_play(SFX_ERROR);
            toast("Press again to clear Sanctum caches (saves & games stay safe)", 4);
        }
        return;
    case R_PADS: go(SCR_CONTROLLERS); return;
    case R_RUMBLE:
        c.rumble = !c.rumble;
        sound_play(c.rumble ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
        if (c.rumble) input_rumble(HAP_OK, true);
        break;
    case R_RUMBLE_STR:
        c.rumble_strength = (c.rumble_strength + 1) % 3;
        sound_play(SFX_OK);
        if (c.rumble) input_rumble(HAP_OK, true);
        break;
    case R_DEADZONE:
        c.stick_deadzone = (c.stick_deadzone + 1) % 3;
        sound_play(SFX_OK);
        break;
    case R_GUIDE_OVERLAY:
        c.guide_overlay = !c.guide_overlay;
        if (!c.guide_overlay) overlay_close();
        if (!demo_mode()) gamebar_override_set(c.guide_overlay && c.guide_override_gamebar);
        sound_play(c.guide_overlay ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
        toast(c.guide_overlay ? "Guide / Home button opens Sanctum Quick Overlay" : "Guide / Home button returns to Home");
        break;
    case R_GUIDE_GAMEBAR:
        c.guide_override_gamebar = !c.guide_override_gamebar;
        if (!demo_mode()) gamebar_override_set(c.guide_overlay && c.guide_override_gamebar);
        sound_play(c.guide_override_gamebar ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
        toast(c.guide_override_gamebar ? "Xbox Game Bar controller button suppressed while Sanctum runs"
                                       : "Restored Windows Xbox Game Bar controller button", 3.5);
        break;
    case R_FORCE_CHORD:
        c.force_close_chord = !c.force_close_chord;
        sound_play(c.force_close_chord ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
        break;
    case R_UPDATE:
        switch (self_update_state()) {
        case UPD_READY: sound_play(SFX_OK); self_update_apply(true); return;
        case UPD_AVAILABLE: sound_play(SFX_OK); self_update_download(); return;
        case UPD_CHECKING: case UPD_DOWNLOADING: return;
        default: sound_play(SFX_OK); self_update_start(true); return;
        }
    case R_AUTO_UPDATE: c.auto_update = !c.auto_update; sound_play(c.auto_update ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF); break;
    case R_DISCORD_RPC:
        c.discord_rpc = !c.discord_rpc;
        discord_rpc_set_enabled(c.discord_rpc);
        sound_play(c.discord_rpc ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
        toast(c.discord_rpc ? "Discord Rich Presence enabled" : "Discord Rich Presence disabled");
        break;
    case R_STARTUP:
        if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: startup unchanged"); return; }
        if (startup_set_enabled(!g_startup_on)) {
            g_startup_on = !g_startup_on;
            sound_play(g_startup_on ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
            toast(g_startup_on ? "Sanctum will start with Windows" : "Removed from Windows startup");
        } else {
            sound_play(SFX_ERROR);
            toast("Could not update Windows startup");
        }
        return;
    case R_SHORTCUT_DESKTOP:
        if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: shortcuts unchanged"); return; }
        if (shortcut_create_desktop()) { sound_play(SFX_TOGGLE_ON); toast("Desktop shortcut created"); }
        else { sound_play(SFX_ERROR); toast("Could not create Desktop shortcut"); }
        return;
    case R_SHORTCUT_START:
        if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: shortcuts unchanged"); return; }
        if (shortcut_create_startmenu()) { sound_play(SFX_TOGGLE_ON); toast("Start Menu shortcut created"); }
        else { sound_play(SFX_ERROR); toast("Could not create Start Menu shortcut"); }
        return;
    case R_COPY_DIAG: {
        std::string info = build_support_info();
        if (SDL_SetClipboardText(info.c_str())) { sound_play(SFX_TOGGLE_ON); toast("Copied support diagnostics to clipboard"); }
        else { sound_play(SFX_ERROR); toast("Could not copy to clipboard"); }
        return;
    }
    case R_LAYOUT:
        c.pad_layout = (c.pad_layout + 1) % 3;
        toast(c.pad_layout == 1 ? "Switch style: A on the right confirms" : c.pad_layout == 2 ? "Xbox style: A at the bottom confirms"
                                                                               : "Confirm follows each pad's own labels");
        sound_play(SFX_OK);
        break;
    case R_ICONS: c.button_icons = (c.button_icons + 1) % 4; input_refresh_style(); sound_play(SFX_OK); break;
    default: return;
    }
    config_save();
}

void settings_frame() {
    App &a = app();
    const Palette &p = pal();
    std::vector<Row> rows = rows_of(g_cat);
    auto focusable = [&](int i) { return rows[i].id != R_INFO; };
    auto first_focusable = [&]() {
        for (int i = 0; i < (int)rows.size(); ++i) if (focusable(i)) return i;
        return -1;
    };

    bool nav_moved = false;
    switch (a.nav) {
    case NAV_UP:
        if (!g_in_rows) {
            if (g_cat > 0) { g_cat--; g_scroll = 0.0f; sound_play_move(-1, -0.22f); }
            else sound_play(SFX_EDGE);
        } else {
            int prev = g_row;
            for (int i = g_row - 1; i >= 0; --i) if (focusable(i)) { g_row = i; break; }
            nav_moved = (g_row != prev);
            if (nav_moved) sound_play_move(-1, 0.14f);
            else sound_play(SFX_EDGE);
        }
        break;
    case NAV_DOWN:
        if (!g_in_rows) {
            if (g_cat < CAT_N - 1) { g_cat++; g_scroll = 0.0f; sound_play_move(1, -0.22f); }
            else sound_play(SFX_EDGE);
        } else {
            int prev = g_row;
            for (int i = g_row + 1; i < (int)rows.size(); ++i) if (focusable(i)) { g_row = i; break; }
            nav_moved = (g_row != prev);
            if (nav_moved) sound_play_move(1, 0.14f);
            else sound_play(SFX_EDGE);
        }
        break;
    case NAV_RIGHT:
    case NAV_A:
        if (!g_in_rows) {
            int f = first_focusable();
            if (f >= 0) { g_in_rows = true, g_row = f; nav_moved = true; sound_play(SFX_OK); }
        } else if (a.nav == NAV_A && g_row < (int)rows.size()) {
            act(rows[g_row]);
        }
        break;
    case NAV_LEFT:
        if (g_in_rows) { g_in_rows = false; sound_play(SFX_BACK); }
        break;
    case NAV_B:
        if (g_in_rows) { g_in_rows = false; sound_play(SFX_BACK); }
        else { go_back(); return; }
        break;
    default: break;
    }
    rows = rows_of(g_cat);
    if (g_row >= (int)rows.size() || !focusable(g_row)) g_row = std::max(0, first_focusable());

    const float x = 384, w = 852, top_y = 144.0f, view_h = 500.0f;
    std::vector<float> ry(rows.size()), rh(rows.size());
    float cur_y = 0.0f;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].section_hdr && i > 0) cur_y += 8.0f;
        ry[i] = cur_y;
        rh[i] = rows[i].section_hdr ? 24.0f : rows[i].drive_card ? 86.0f : 48.0f;
        cur_y += rh[i] + (rows[i].section_hdr ? 6.0f : 8.0f);
    }
    float total_h = cur_y > 0.0f ? cur_y - 8.0f : 0.0f;
    float max_scroll = std::max(0.0f, total_h - view_h);

    if (hover(x, top_y, w, view_h) && ImGui::GetIO().MouseWheel != 0.0f)
        g_scroll -= ImGui::GetIO().MouseWheel * 56.0f;
    if (g_in_rows && nav_moved && g_row < (int)rows.size()) {
        if (g_row == first_focusable()) g_scroll = 0.0f;
        else {
            if (ry[g_row] < g_scroll) g_scroll = ry[g_row];
            if (ry[g_row] + rh[g_row] > g_scroll + view_h) g_scroll = ry[g_row] + rh[g_row] - view_h;
        }
    }
    g_scroll = SDL_clamp(g_scroll, 0.0f, max_scroll);

    background(0);
    status_bar();
    text(58, 96, 26, p.text, "System Settings");
    rect(340, 76, 1, 580, p.line);
    for (int i = 0; i < CAT_N; ++i) {
        float y = 144 + i * 52;
        if (hover(44, y, 280, 44)) {
            if (a.mouse_moved && !a.clicked && (g_cat != i || g_in_rows)) { g_cat = i, g_in_rows = false, g_scroll = 0.0f; sound_play(SFX_MOVE); }
            if (a.clicked) { if (g_cat != i) sound_play(SFX_MOVE); g_cat = i, g_in_rows = false, g_scroll = 0.0f; }
        }
        bool sel = i == g_cat;
        if (sel) { rect(44, y, 280, 44, p.panel, 12); rect(44, y + 7, 4, 30, p.accent, 2); }
        icon(kCatIcon[i], 76, y + 22, 21, sel ? p.text : p.soft);
        text(100, y + 11, 18, sel ? p.text : p.soft, kCatName[i]);
        if (sel && !g_in_rows) focus_ring(44, y, 280, 44, 12, 3);
    }

    text(x, 96, 26, p.text, kCatName[g_cat]);
    bool mouse_in_view = a.mouse.y >= top_y && a.mouse.y < top_y + view_h;
    dl()->PushClipRect(P(x - 16, top_y - 14), P(x + w + 16, top_y + view_h + 14), true);
    for (int i = 0; i < (int)rows.size(); ++i) {
        const Row &r = rows[i];
        float y = top_y + ry[i] - g_scroll;
        float h = rh[i];
        if (y + h < top_y - 16 || y > top_y + view_h + 16) continue;
        if (r.section_hdr) {
            text(x + 4, y + 4, 13, p.soft, r.label.c_str());
            if (!r.value.empty()) text(x + w - 4, y + 4, 13, p.soft, r.value.c_str(), 1);
            continue;
        }
        bool f = g_in_rows && i == g_row;
        if (mouse_in_view && focusable(i) && hover(x, y, w, h)) {
            if (a.mouse_moved && !a.clicked && (!g_in_rows || g_row != i)) { g_in_rows = true, g_row = i; sound_play(SFX_MOVE); }
            if (a.clicked) { g_in_rows = true, g_row = i; act(r); }
        }
        rect(x, y, w, h, p.panel, 12);
        if (r.drive_card) {
            icon(IC_DRIVE, x + 28, y + 23, 20, p.text);
            float lw = text(x + 48, y + 13, 17, p.text, fit(17, r.label, 340).c_str());
            float px = x + 48 + lw + 10;
            if (!r.badge.empty()) {
                float bw = text_w(12, r.badge.c_str()) + 14;
                rect(px, y + 13, bw, 20, p.panel2, 6);
                text(px + 7, y + 16, 12, p.soft, r.badge.c_str());
                px += bw + 6;
            }
            if (!r.badge2.empty()) {
                float bw = text_w(12, r.badge2.c_str()) + 14;
                bool is_lib = r.badge2 == "Library";
                rect(px, y + 13, bw, 20, is_lib ? with_alpha(p.accent, 42) : p.panel2, 6);
                text(px + 7, y + 16, 12, is_lib ? p.accent_text : p.soft, r.badge2.c_str());
            }
            text(x + w - 20, y + 14, 16, r.val_col ? r.val_col : p.accent_text, r.value.c_str(), 1);
            float bx = x + 20, bw = w - 40;
            ImU32 used_col = cfg().theme ? IM_COL32(126, 144, 172, 230) : IM_COL32(118, 136, 166, 220);
            rect(bx, y + 43, bw, 10, p.line, 5);
            if (r.bar_frac > 0.0f)
                rect(bx, y + 43, std::max(10.0f, bw * SDL_clamp(r.bar_frac, 0.0f, 1.0f)), 10, used_col, 5);
            if (r.bar_frac2 > 0.001f)
                rect(bx, y + 43, std::max(8.0f, bw * SDL_clamp(r.bar_frac2, 0.0f, 1.0f)), 10, p.accent, 5);
            circle(x + 25, y + 68, 4, r.bar_frac2 > 0.001f ? p.accent : used_col);
            text(x + 36, y + 60, 13, p.soft, fit(13, r.sub, w - 180).c_str());
            if (f) text(x + w - 20, y + 60, 13, p.soft, "Open in Explorer", 1);
        } else {
            float lw = text(x + 20, y + 13, 17, r.id == R_INFO ? p.soft : p.text,
                            fit(17, r.label, r.bar_frac >= 0 ? (r.badge.empty() ? 340 : 300) : 440).c_str());
            if (!r.badge.empty()) {
                float px = x + 20 + lw + 10;
                float bw = text_w(12, r.badge.c_str()) + 12;
                rect(px, y + 14, bw, 20, p.panel2, 6);
                text(px + 6, y + 17, 12, p.soft, r.badge.c_str());
            }
            if (r.bar_frac >= 0.0f) {
                float bx = x + 380, bw = 310;
                rect(bx, y + 20, bw, 8, p.line, 4);
                if (r.bar_frac > 0.0f) rect(bx, y + 20, std::max(8.0f, bw * SDL_clamp(r.bar_frac, 0.0f, 1.0f)), 8, r.bar_col ? r.bar_col : p.accent, 4);
            }
            if (r.toggle) {
                float tx = x + w - 76;
                rect(tx, y + 10, 56, 28, r.on ? p.accent : p.line, 14);
                circle(r.on ? tx + 42 : tx + 14, y + 24, 11, IM_COL32_WHITE);
            } else if (!r.value.empty()) {
                ImU32 c = r.val_col ? r.val_col
                        : (r.id == R_INFO || r.id == R_PORT || r.id == R_FOLDER || r.id == R_LIBDIR || r.id == R_SCREENSHOTS_DIR
                               ? p.soft : p.accent_text);
                text(x + w - 20, y + 14, 16, c, fit(16, r.value, r.bar_frac >= 0 ? 140 : (w - 480)).c_str(), 1);
            }
        }
        if (f) focus_ring(x, y, w, h, 12, 3);
    }
    dl()->PopClipRect();
    if (total_h > view_h) {
        float sh = view_h, thumb_h = std::max(36.0f, sh * view_h / total_h);
        rect(x + w + 12, top_y, 4, sh, p.line, 2);
        rect(x + w + 12, top_y + (sh - thumb_h) * (g_scroll / std::max(1.0f, max_scroll)), 4, thumb_h, p.soft, 2);
    }
    if (g_in_rows) hint_bar({{NAV_A, "OK"}, {NAV_B, "Back"}});
    else hint_bar({{NAV_A, "Open"}, {NAV_B, "Back"}});
}

// ======================= the folder picker
struct Entry {
    std::string name, path;
    Icon icon;
};
static std::string g_dir, g_port_match;
static std::vector<Entry> g_entries;
static int g_sel = 0, g_top = 0, g_pngs = 0;
static FolderFor g_for = FOLDER_COVERS;

static void list_dir(const std::string &dir) {
    g_dir = dir, g_entries.clear(), g_sel = 0, g_top = 0, g_port_match.clear();
    if (dir.empty()) {
        struct { SDL_Folder f; const char *n; } places[] = {
            {SDL_FOLDER_DESKTOP, "Desktop"}, {SDL_FOLDER_PICTURES, "Pictures"}, {SDL_FOLDER_DOWNLOADS, "Downloads"},
            {SDL_FOLDER_DOCUMENTS, "Documents"}};
        for (auto &pl : places)
            if (const char *p = SDL_GetUserFolder(pl.f)) {
                std::string s = p;
                while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
                g_entries.push_back({pl.n, s, IC_FOLDER});
            }
        for (char d = 'A'; d <= 'Z'; ++d) {
            std::string root = std::string(1, d) + ":\\";
            if (SDL_GetPathInfo(root.c_str(), nullptr)) g_entries.push_back({std::string(1, d) + ":", std::string(1, d) + ":", IC_DRIVE});
        }
        g_pngs = 0;
        return;
    }
    int n = 0;
    char **names = SDL_GlobDirectory((dir + "\\").c_str(), "*", 0, &n);
    std::vector<std::string> dirs;
    for (int i = 0; names && i < n; ++i) {
        SDL_PathInfo info;
        std::string full = dir + "\\" + names[i];
        if (names[i][0] != '.' && names[i][0] != '$' && SDL_GetPathInfo(full.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY)
            dirs.push_back(names[i]);
    }
    SDL_free(names);
    std::sort(dirs.begin(), dirs.end(), [](const std::string &a, const std::string &b) { return SDL_strcasecmp(a.c_str(), b.c_str()) < 0; });
    for (auto &d : dirs) g_entries.push_back({d, dir + "\\" + d, IC_FOLDER});
    g_pngs = (g_for == FOLDER_COVERS || g_for == FOLDER_SCREENSHOTS) ? count_pngs(dir) : 0;
    g_port_match = g_for == FOLDER_ADD_PORT ? catalog_match_folder(dir) : std::string();
}

static std::string parent_of(const std::string &d) {
    size_t s = d.find_last_of("\\/");
    if (s == std::string::npos || d.size() <= 2) return "";
    return d.substr(0, s);
}

void folder_open(const std::string &start, FolderFor what) {
    g_for = what;
    std::string s = start;
    if (!s.empty() && !SDL_GetPathInfo(s.c_str(), nullptr)) s.clear();
    list_dir(s);
    go(SCR_FOLDER);
}

void folder_frame() {
    App &a = app();
    const Palette &p = pal();
    int n = (int)g_entries.size();
    const int kVisible = 8;
    switch (a.nav) {
    case NAV_UP: if (g_sel > 0) { g_sel--; sound_play(SFX_MOVE); } else if (n) sound_play(SFX_EDGE); break;
    case NAV_DOWN: if (g_sel < n - 1) { g_sel++; sound_play(SFX_MOVE); } else if (n) sound_play(SFX_EDGE); break;
    case NAV_L: if (n) { int prev = g_sel; g_sel = std::max(0, g_sel - kVisible); sound_play(g_sel != prev ? SFX_MOVE : SFX_EDGE); } break;
    case NAV_R: if (n) { int prev = g_sel; g_sel = std::min(n - 1, g_sel + kVisible); sound_play(g_sel != prev ? SFX_MOVE : SFX_EDGE); } break;
    case NAV_A: if (n) { sound_play(SFX_OK); list_dir(g_entries[g_sel].path); return; } break;
    case NAV_B:
        if (g_dir.empty()) { go_back(); return; }
        else {
            sound_play(SFX_BACK);
            std::string from = g_dir;
            list_dir(parent_of(g_dir));
            for (int i = 0; i < (int)g_entries.size(); ++i) if (g_entries[i].path == from) g_sel = i;
        }
        break;
    case NAV_X:
        if (!g_dir.empty()) {
            if (g_for == FOLDER_ADD_PORT) {
                int added = catalog_add_folder(g_dir);
                if (added > 0) {
                    sound_play(SFX_TOGGLE_ON);
                    toast("Added " + g_port_match, 3.5);
                    go_back(true);
                    return;
                }
                sound_play(SFX_ERROR);
                toast("No SteelTemple port found in this folder");
                break;
            }
            if (g_for == FOLDER_COVERS) {
                cfg().cover_dir = g_dir;
                covers_reload();
                toast("Cover art folder set");
            } else if (g_for == FOLDER_SCREENSHOTS) {
                cfg().screenshots_dir = g_dir;
                toast("Album screenshots folder set");
            } else if (g_for == FOLDER_SAVE_SYNC) {
                cfg().save_sync_dir = g_dir;
                sync_invalidate();
                toast("Cloud save sync folder set", 3.5);
            } else {
                cfg().library_dir = g_dir;
                toast("New ports will install into " + g_dir, 3.5);
            }
            config_save();
            sound_play(SFX_TOGGLE_ON);
            go_back(true);
            return;
        }
        break;
    default: break;
    }
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + kVisible) g_top = g_sel - kVisible + 1;

    background(0);
    status_bar();
    const char *heading = g_for == FOLDER_COVERS ? "Choose your cover-art folder"
                        : g_for == FOLDER_SCREENSHOTS ? "Choose your screenshots folder"
                        : g_for == FOLDER_SAVE_SYNC ? "Choose your cloud save sync folder"
                        : g_for == FOLDER_ADD_PORT ? "Choose a folder holding a port"
                        : "Choose where new ports install";
    text(96, 92, 26, p.text, heading);
    text(96, 130, 16, p.soft, g_dir.empty() ? "This PC" : fit(16, g_dir, 780).c_str());
    if (!g_dir.empty() && (g_for == FOLDER_COVERS || g_for == FOLDER_SCREENSHOTS)) {
        std::string s = std::to_string(g_pngs) + (g_pngs == 1 ? " picture here" : " pictures here");
        text(1184, 130, 16, g_pngs ? p.ok : p.dim, s.c_str(), 1);
    } else if (!g_dir.empty() && g_for == FOLDER_SAVE_SYNC) {
        text(1184, 130, 16, p.dim, "OneDrive · Google Drive · Syncthing · NAS", 1);
    } else if (!g_dir.empty() && g_for == FOLDER_ADD_PORT) {
        std::string s = g_port_match.empty() ? "Open a folder with a port's .exe" : "Found: " + g_port_match;
        text(1184, 130, 16, g_port_match.empty() ? p.dim : p.ok, s.c_str(), 1);
    }
    for (int i = g_top; i < n && i < g_top + kVisible; ++i) {
        float y = 168 + (i - g_top) * 56;
        if (hover(96, y, 1088, 48)) {
            if (a.mouse_moved && !a.clicked && g_sel != i) { g_sel = i; sound_play(SFX_MOVE); }
            if (a.clicked) { sound_play(SFX_OK); list_dir(g_entries[i].path); return; }
        }
        rect(96, y, 1088, 48, p.panel, 12);
        icon(g_entries[i].icon, 128, y + 24, 22, p.soft);
        text(156, y + 13, 18, p.text, fit(18, g_entries[i].name, 980).c_str());
        if (i == g_sel) focus_ring(96, y, 1088, 48, 12, 3);
    }
    if (!n) text(96, 180, 18, p.dim, "No folders here.");
    if (n > kVisible) {
        float h = 56 * kVisible - 8, bh = h * kVisible / n;
        rect(1196, 168, 4, h, p.line, 2);
        rect(1196, 168 + (h - bh) * g_top / std::max(1, n - kVisible), 4, bh, p.soft, 2);
    }
    if (g_dir.empty()) hint_bar({{NAV_A, "Open"}, {NAV_B, "Cancel"}});
    else hint_bar({{NAV_X, g_for == FOLDER_ADD_PORT ? "Add this folder" : "Use this folder"}, {NAV_A, "Open"}, {NAV_B, "Up"}});
}

// ======================= Controllers tester (dual sticks + deadzone ring + analog triggers + buttons + battery)
void controllers_frame() {
    App &a = app();
    const Palette &p = pal();
    if (a.nav == NAV_B) { go_back(); return; }
    if (a.nav == NAV_Y) {
        cfg().stick_deadzone = (cfg().stick_deadzone + 1) % 3;
        config_save();
        sound_play(SFX_OK);
    }
    if (a.nav == NAV_X) {
        input_rumble(HAP_LAUNCH, true);
        toast("Sent test vibration pulse");
    }

    background(0);
    status_bar();
    text(96, 86, 26, p.text, "Controllers & Tester");
    float dz = input_deadzone_threshold();
    char dz_lbl[48];
    SDL_snprintf(dz_lbl, sizeof dz_lbl, "Deadzone: %d%%  (Y cycles)", (int)(dz * 100 + 0.5f));
    text(1184, 96, 15, p.soft, dz_lbl, 1);

    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (!count) {
        rect(96, 140, 1088, 110, p.panel, 16);
        icon(IC_PAD, 154, 195, 42, p.soft);
        text(204, 168, 20, p.text, "No controller connected");
        text(204, 198, 15, p.soft, "Connect an Xbox, PlayStation, Switch Pro, or generic pad to test sticks, triggers, buttons, and vibration.");
    }
    for (int i = 0; ids && i < count && i < 3; ++i) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
        if (!g) continue;
        float y = 136 + i * 158;
        rect(96, y, 1088, 144, p.panel, 16);
        icon(IC_PAD, 142, y + 40, 38, p.text);
        const char *name = SDL_GetGamepadName(g);
        text(178, y + 16, 19, p.text, fit(19, name ? name : "Controller", 520).c_str());

        // Wireless battery status on the card
        int bpct = -1;
        SDL_PowerState bps = SDL_GetGamepadPowerInfo(g, &bpct);
        if (bps == SDL_POWERSTATE_ON_BATTERY || bps == SDL_POWERSTATE_CHARGING || bps == SDL_POWERSTATE_CHARGED) {
            bool chg = bps == SDL_POWERSTATE_CHARGING || bps == SDL_POWERSTATE_CHARGED;
            char bstr[48];
            if (bpct >= 0) SDL_snprintf(bstr, sizeof bstr, chg ? "Battery %d%% (Charging)" : "Battery %d%%", bpct);
            else SDL_snprintf(bstr, sizeof bstr, chg ? "Charging" : "On battery");
            text(900, y + 18, 14, chg ? p.ok : (bpct >= 0 && bpct < 20 ? p.warn : p.soft), bstr, 1);
        } else {
            text(900, y + 18, 14, p.dim, "Wired / USB", 1);
        }

        // Buttons row
        struct { SDL_GamepadButton b; const char *l; } bs[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, "S"}, {SDL_GAMEPAD_BUTTON_EAST, "E"}, {SDL_GAMEPAD_BUTTON_WEST, "W"},
            {SDL_GAMEPAD_BUTTON_NORTH, "N"}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "L1"}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "R1"},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, "L3"}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, "R3"},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, "Up"}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, "Dn"}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, "Lt"},
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "Rt"}, {SDL_GAMEPAD_BUTTON_START, "+"}, {SDL_GAMEPAD_BUTTON_BACK, "-"}};
        float bx = 178;
        for (auto &b : bs) {
            bool down = SDL_GetGamepadButton(g, b.b);
            rect(bx, y + 52, 46, 30, down ? p.accent : p.panel2, 15);
            text(bx + 23, y + 58, 13, down ? IM_COL32_WHITE : p.soft, b.l, 0.5f);
            bx += 51;
        }

        // L2 / R2 analog trigger bars
        float lt = SDL_clamp(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f, 0.0f, 1.0f);
        float rt = SDL_clamp(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f, 0.0f, 1.0f);
        text(178, y + 100, 13, p.dim, "L2");
        rect(206, y + 104, 240, 10, p.panel2, 5);
        if (lt > 0.01f) rect(206, y + 104, std::max(8.0f, 240 * lt), 10, p.accent, 5);
        text(480, y + 100, 13, p.dim, "R2");
        rect(508, y + 104, 240, 10, p.panel2, 5);
        if (rt > 0.01f) rect(508, y + 104, std::max(8.0f, 240 * rt), 10, p.accent, 5);

        // Dual sticks with live deadzone ring
        auto draw_stick = [&](float cx, float cy, float ax, float ay, const char *lbl) {
            float R = 36;
            circle(cx, cy, R, p.panel2);
            dl()->AddCircle(P(cx, cy), S(R * dz), with_alpha(p.soft, 110), 32, S(1.5f));
            bool active = std::fabs(ax) > dz || std::fabs(ay) > dz;
            circle(cx + ax * (R - 8), cy + ay * (R - 8), 8, active ? p.accent : p.focus);
            text(cx, cy + R + 6, 12, p.dim, lbl, 0.5f);
        };
        float lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        float ly = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        float rx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
        float ry = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
        draw_stick(1010, y + 64, lx, ly, "L Stick");
        draw_stick(1112, y + 64, rx, ry, "R Stick");
    }
    SDL_free(ids);

    text(96, 616, 14, p.dim,
         cfg().pad_layout == 1 ? "Button layout: Nintendo (right confirms, bottom backs). Force-close chord: hold Back + Start + L + R for 2 s."
       : cfg().pad_layout == 2 ? "Button layout: Xbox (bottom confirms, right backs). Force-close chord: hold Back + Start + L + R for 2 s."
       : "Button layout: Automatic. Force-close chord: hold Back + Start + L + R for 2 s while a game is frozen.");
    hint_bar({{NAV_X, "Test vibration"}, {NAV_Y, "Deadzone"}, {NAV_B, "Back"}});
}

// ======================= pages still to come
void soon_frame() {
    App &a = app();
    const Palette &p = pal();
    if (a.nav == NAV_B || a.nav == NAV_A || a.clicked) { go_back(); return; }
    background(0);
    status_bar();
    text(640, 280, 32, p.text, a.soon_title.c_str(), 0.5f);
    text(640, 330, 18, p.soft, "This page comes in a later build step of SteelTemple Sanctum.", 0.5f);
    hint_bar({{NAV_B, "Back"}});
}

}  // namespace sanctum
