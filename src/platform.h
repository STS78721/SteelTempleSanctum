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
// filter_spec: semicolon-separated patterns, e.g. "*.png;*.jpg;*.jpeg;*.bmp" or "*.zip" or "*.exe"
std::string pick_open_file(SDL_Window *win, const char *title, const char *filter_name, const char *filter_spec);

// Pure C++ standard ZIP archive writer for recursive directory backups (stores files with CRC-32 & UTF-8 names).
bool zip_folder(const std::string &src_dir, const std::string &dst_zip, int *files_written = nullptr);

// Cached host hardware/OS summary (for About > System diagnostics & Copy support info).
const SystemDiag &system_diagnostics();

// Mounted fixed and removable drives with volume labels, filesystem, total and free bytes.
std::vector<DriveInfo> list_drives();

}  // namespace sanctum
