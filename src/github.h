// Each port's releases from GitHub. At start the saved copy (%LOCALAPPDATA%\SteelTemple\Sanctum\releases) shows at
// once; a worker then asks GitHub with the saved ETag, so an unchanged list costs nothing against the hourly limit.
// No account or token: the releases are public.
#pragma once
#include <string>

namespace sanctum {

void github_refresh();    // a worker: every port's releases
bool github_busy();
bool github_poll();       // UI thread: 1 when new release lists were merged into ports()
std::string github_status();   // "" when fine, else why the lists may be old (offline, limit)
std::string local_dir();       // %LOCALAPPDATA%\SteelTemple\Sanctum\ (downloads and caches: not roaming)

}  // namespace sanctum
