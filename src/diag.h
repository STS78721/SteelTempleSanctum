// One-Click Sanitized Crash Diagnostics Bundle:
// Packages system specs + GPU driver version, non-default port INI diff, and sanitized log tail (last 64 KB,
// with personal Windows usernames and local paths scrubbed) into a ready-to-attach .zip for GitHub Issues.
#pragma once
#include <string>

#include "catalog.h"
#include "launch.h"

namespace sanctum {

// Exports a sanitized diagnostics zip into %LOCALAPPDATA%\SteelTemple\Sanctum\diagnostics\ and returns its path (or "").
std::string export_crash_diagnostics(int port_idx, const Install *in, const LastRun *lr);

}  // namespace sanctum
