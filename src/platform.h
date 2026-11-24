// Windows platform helpers: Desktop/Start Menu shortcuts, Run-at-startup registry entry, Recycle Bin delete,
// fast recursive folder size calculation, native file picker, ZIP archiver (for save backups), and system diagnostics.
#pragma once
#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace sanctum {

struct SystemDiag {
    std::string os;
    std::string cpu;
    std::string gpu;
    std::string gpu_driver;
    int ram_gb = 0;
};

struct DriveInfo {
    char letter = 'C';
    std::string label;
    std::string fs;
    long long total_bytes = 0;
    long long free_bytes = 0;
    bool removable = false;
};

bool shortcut_create_desktop();
bool shortcut_create_startmenu();
bool startup_is_enabled();
bool startup_set_enabled(bool on);
bool recycle_path(const std::string &path);
long long folder_size_bytes(const std::string &dir, int *file_count = nullptr);

// Native Windows file open dialog (returns empty string if cancelled).
// filter_spec: semicolon-separated patterns, e.g. "*.png;*.jpg;*.jpeg;*.bmp" or "*.zip" or "*.exe;*.lnk"
// no_deref_links: keep .lnk shortcuts as .lnk instead of letting Windows resolve them to their target executable.
std::string pick_open_file(SDL_Window *win, const char *title, const char *filter_name, const char *filter_spec,
                           bool no_deref_links = false);

// Windows .lnk shortcut metadata (target executable, launch arguments, working directory, icon location).
struct ShortcutInfo {
    std::string target;
    std::string args;
    std::string working_dir;
    std::string icon_path;
    int icon_index = 0;
    std::string description;
};
bool resolve_shortcut(const std::string &lnk_path, ShortcutInfo &out);

// Extracts high-resolution (up to 256x256 RGBA) cover/icon art from a .lnk shortcut, .ico file, image file, or .exe
// and saves it as a PNG at dst_png. Returns true on success.
bool extract_shortcut_cover(const std::string &src_path, const std::string &icon_path, int icon_index,
                            const std::string &dst_png);

// Pure C++ standard ZIP archive writer for recursive directory backups (stores files with CRC-32 & UTF-8 names).
bool zip_folder(const std::string &src_dir, const std::string &dst_zip, int *files_written = nullptr);

// Cached host hardware/OS summary (for About > System diagnostics & Copy support info).
const SystemDiag &system_diagnostics();

// Mounted fixed and removable drives with volume labels, filesystem, total and free bytes.
std::vector<DriveInfo> list_drives();

// Windows default playback endpoint master volume (0..100) and mute state (for the Quick Overlay slider).
bool system_volume_get(int *pct_out, bool *muted_out);
bool system_volume_set(int pct, bool mute);

// Active Windows audio playback devices and live default output switcher (for the Quick Overlay).
struct AudioOutput {
    std::string id;
    std::string name;
    bool active = false;
};
std::vector<AudioOutput> system_audio_outputs();
bool system_audio_output_set(const std::string &device_id);

// Captures a visible window (or the primary monitor if target_hwnd is null) into a PNG file via Windows Imaging Component.
// allow_print_window must be false if target_hwnd belongs to a suspended/frozen process (PrintWindow sends synchronous WM_PRINT).
bool capture_screenshot_png(void *target_hwnd, const std::string &dst_png, int *w_out = nullptr, int *h_out = nullptr,
                            bool allow_print_window = true);

// Monitor bounds (physical screen coords) for a given Win32 HWND (or primary monitor if null).
bool monitor_rect_for_hwnd(void *target_hwnd, int &x, int &y, int &w, int &h);

// Reversible per-game display refresh rate switching (session-only, never modifies registry defaults).
std::vector<int> display_supported_refresh_rates(void *hwnd_opt = nullptr);
int display_current_refresh_rate(void *hwnd_opt = nullptr);
bool display_refresh_override_push(void *hwnd_opt, int target_hz);
void display_refresh_override_pop();

// Temporarily suppresses Windows' Xbox Game Bar controller Guide button (HKCU\Software\Microsoft\GameBar\
// UseNexusForGameBarEnabled) while Sanctum is running, and restores the user's exact original setting on exit.
void gamebar_override_set(bool active);

// True if steam.exe is currently running (cached, used for a helpful Controller settings hint).
bool is_steam_running();

}  // namespace sanctum
