// Optional Discord Rich Presence via local Windows named pipe (\\.\pipe\discord-ipc-0..9).
// Zero external DLLs, completely non-blocking background worker, opt-in (off by default in Config).
#pragma once
#include <string>

namespace sanctum {

void discord_rpc_init();
void discord_rpc_shutdown();
void discord_rpc_set_enabled(bool on);
void discord_rpc_set_activity(const std::string &details, const std::string &state);

}  // namespace sanctum
