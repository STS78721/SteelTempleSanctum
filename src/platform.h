// Windows platform helpers: Desktop/Start Menu shortcuts, Run-at-startup registry entry, Recycle Bin delete,
// and fast recursive folder size calculation.
#pragma once
#include <string>

namespace sanctum {

bool shortcut_create_desktop();
bool shortcut_create_startmenu();
bool startup_is_enabled();
bool startup_set_enabled(bool on);
bool recycle_path(const std::string &path);
long long folder_size_bytes(const std::string &dir, int *file_count = nullptr);

}  // namespace sanctum
