// Sanctum Unified Smart Cache (%LOCALAPPDATA%\SteelTemple\Sanctum\cache\):
//   - Binary decoded RGBA thumbnail cache for cover art & Album screenshots (10-30x faster cold load)
//   - Persistent folder size & file count cache (eliminates re-walking 100k+ files every session)
//   - Unified cache footprint reporting & one-click cleanup (never touches saves, settings, or games)
#pragma once
#include <string>
#include <vector>

namespace sanctum {

void cache_init();    // loads persistent folder-size index before first screen
void cache_flush();   // writes dirty folder-size index cleanly on exit

// Binary thumbnail cache (keyed by path + max_dim, verified against source file size & modify_time)
bool cache_thumb_load(const std::string &src_path, int max_dim, int &w_out, int &h_out, std::vector<unsigned char> &rgba_out);
void cache_thumb_save(const std::string &src_path, int max_dim, int w, int h, const std::vector<unsigned char> &rgba);

// Persistent folder size cache (verified against top-level dir modify_time and max_age_sec = 900 s)
bool cache_size_get(const std::string &dir, long long &bytes_out, int &files_out);
void cache_size_put(const std::string &dir, long long bytes, int files);
void cache_size_invalidate(const std::string &dir);

struct CacheStats {
    long long thumb_bytes = 0;
    int thumb_files = 0;
    long long meta_bytes = 0;     // sizes.ini + catalog.json + releases + remote settings
    long long download_bytes = 0;
    long long total_bytes = 0;
    int thumb_hits = 0;           // session counter
    int thumb_misses = 0;         // session counter
};

CacheStats cache_stats();
void cache_clear_sanctum();   // clears thumbs + sizes.ini + downloads (leaves saves, settings, games untouched)

}  // namespace sanctum
