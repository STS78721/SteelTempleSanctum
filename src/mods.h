// Per-port mod manager: lists mod folders under <port>\mods and toggles them safely
// between <port>\mods\<Mod> and <port>\mods\.disabled\<Mod>, plus .zip mod installation.
#pragma once
#include <string>

namespace sanctum {

int mods_summary(const std::string &install_dir, int *enabled_out = nullptr);
void mods_invalidate_cache();
int mods_active_port();   // port currently selected in SCR_MODS (or -1)
bool mods_install_zip(int port_id, const std::string &zip_path);

}  // namespace sanctum
