// Sanctum keeps itself up to date from its own GitHub releases (STS78721/SteelTempleSanctum).
//
//   Check     at start (at most every 15 minutes, with the saved ETag) and from System Settings > About
//   Download  with "Update Sanctum automatically" on (the default), a newer release's Portable zip downloads in the
//             background into %LOCALAPPDATA%\SteelTemple\Sanctum\downloads and is checked against GitHub's SHA-256
//   Ready     unpacked beside it; Home says so once. It is put in place when Sanctum closes, or at once with
//             "Restart and update" (About): every replaced file is first renamed *.old and put back if anything
//             fails, so a failed update leaves the working Sanctum as it was
//
// Only Sanctum's own files are replaced: the player's settings live in %APPDATA% and are never touched.
#pragma once
#include <string>

#define SANCTUM_VERSION "1.0.4b"

namespace sanctum {

enum UpdState { UPD_IDLE, UPD_CHECKING, UPD_CURRENT, UPD_AVAILABLE, UPD_DOWNLOADING, UPD_READY, UPD_FAILED };

void self_update_start(bool force = false);   // a check (and, if automatic, the download) on a worker
void self_update_download();                  // the player asked for the update found (automatic updates off)
UpdState self_update_state();
std::string self_update_tag();                // the newer release, e.g. "1.0.2"
float self_update_progress();                 // 0..1 while downloading
std::string self_update_error();
bool self_update_poll();                      // UI thread: true once when an update has just become ready
bool self_update_apply(bool restart);         // puts the ready update in place; restart: then starts it
const char *sanctum_version();                // this build's (SANCTUM_SELF_VERSION overrides it, for tests)

}  // namespace sanctum
