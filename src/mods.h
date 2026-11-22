// Per-port mod manager (SCR_MODS) and mod summary helper for each port's Mods tab.
// Active mods live in <port>\mods\<ModName>; disabled mods are moved atomically to <port>\mods\.disabled\<ModName>.
#pragma once
#include <string>

namespace sanctum {

int mods_summary(const std::string &dir, int *enabled_out = nullptr);   // cached total count (and active count)
void mods_invalidate_cache();

}  // namespace sanctum
