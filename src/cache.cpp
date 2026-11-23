#include "cache.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>

#include "app.h"
#include "github.h"
#include "install.h"
#include "platform.h"

namespace sanctum {

static std::string cache_dir() { return local_dir() + "cache\\"; }
static std::string thumbs_dir() { return cache_dir() + "thumbs\\"; }
static std::string sizes_path() { return cache_dir() + "sizes.ini"; }

static uint64_t fnv1a64(const std::string &s, uint64_t seed = 14695981039346656037ULL) {
    uint64_t h = seed;
    for (unsigned char c : s) {
        h ^= (uint64_t)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        h *= 1099511628211ULL;
    }
    return h;
}

static std::string norm_key(std::string p) {
    for (char &c : p) {
        if (c == '/') c = '\\';
        else if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    }
    while (p.size() > 3 && p.back() == '\\') p.pop_back();
    return p;
}

// ======================= Binary thumbnail cache (.sthm)
#pragma pack(push, 1)
struct ThumbHead {
    uint32_t magic;
    uint32_t w;
    uint32_t h;
    uint32_t max_dim;
    uint64_t src_size;
    int64_t src_mtime;
    uint64_t path_hash;
};
#pragma pack(pop)
static constexpr uint32_t kThumbMagic = 0x4D485453u;   // "STHM"

static std::atomic<int> g_thumb_hits{0};
static std::atomic<int> g_thumb_misses{0};

static std::string thumb_file_for(const std::string &src_path, int max_dim, uint64_t &hash_out) {
    std::string k = norm_key(src_path);
    hash_out = fnv1a64(k);
    uint64_t fh = fnv1a64(std::to_string(max_dim), hash_out);
    char hex[32];
    SDL_snprintf(hex, sizeof hex, "%016llx.sthm", (unsigned long long)fh);
    return thumbs_dir() + hex;
}

bool cache_thumb_load(const std::string &src_path, int max_dim, int &w_out, int &h_out, std::vector<unsigned char> &rgba_out) {
    if (max_dim <= 0 || src_path.empty()) return false;
    SDL_PathInfo spi{};
    if (!SDL_GetPathInfo(src_path.c_str(), &spi) || spi.type != SDL_PATHTYPE_FILE || spi.size == 0) return false;
    uint64_t ph = 0;
    std::string cpath = thumb_file_for(src_path, max_dim, ph);
    size_t n = 0;
    void *raw = SDL_LoadFile(cpath.c_str(), &n);
    if (!raw) {
        ++g_thumb_misses;
        return false;
    }
    if (n < sizeof(ThumbHead)) { SDL_free(raw); ++g_thumb_misses; return false; }
    ThumbHead hd{};
    SDL_memcpy(&hd, raw, sizeof hd);
    bool valid = hd.magic == kThumbMagic &&
                 hd.w > 0 && hd.w <= 1024 && hd.h > 0 && hd.h <= 1024 &&
                 hd.max_dim == (uint32_t)max_dim &&
                 hd.src_size == (uint64_t)spi.size &&
                 hd.src_mtime == (int64_t)spi.modify_time &&
                 hd.path_hash == ph &&
                 n == sizeof(ThumbHead) + (size_t)hd.w * hd.h * 4;
    if (!valid) {
        SDL_free(raw);
        ++g_thumb_misses;
        return false;
    }
    w_out = (int)hd.w;
    h_out = (int)hd.h;
    rgba_out.resize((size_t)hd.w * hd.h * 4);
    SDL_memcpy(rgba_out.data(), (const unsigned char *)raw + sizeof(ThumbHead), rgba_out.size());
    SDL_free(raw);
    ++g_thumb_hits;
    return true;
}

void cache_thumb_save(const std::string &src_path, int max_dim, int w, int h, const std::vector<unsigned char> &rgba) {
    if (demo_mode() || max_dim <= 0 || w <= 0 || w > 1024 || h <= 0 || h > 1024) return;
    if (rgba.size() != (size_t)w * h * 4) return;
    SDL_PathInfo spi{};
    if (!SDL_GetPathInfo(src_path.c_str(), &spi) || spi.type != SDL_PATHTYPE_FILE) return;
    SDL_CreateDirectory(local_dir().c_str());
    SDL_CreateDirectory(cache_dir().c_str());
    SDL_CreateDirectory(thumbs_dir().c_str());
    uint64_t ph = 0;
    std::string cpath = thumb_file_for(src_path, max_dim, ph);
    ThumbHead hd{kThumbMagic, (uint32_t)w, (uint32_t)h, (uint32_t)max_dim,
                 (uint64_t)spi.size, (int64_t)spi.modify_time, ph};
    std::vector<unsigned char> blob(sizeof(ThumbHead) + rgba.size());
    SDL_memcpy(blob.data(), &hd, sizeof hd);
    SDL_memcpy(blob.data() + sizeof hd, rgba.data(), rgba.size());
    std::string tmp = cpath + ".tmp";
    if (SDL_SaveFile(tmp.c_str(), blob.data(), blob.size())) SDL_RenamePath(tmp.c_str(), cpath.c_str());
}

// ======================= Persistent folder size cache (sizes.ini)
struct SizeRec {
    long long bytes = 0;
    int files = 0;
    int64_t dir_mtime = 0;
    long long checked = 0;
};
static std::mutex g_sz_lock;
static std::map<std::string, SizeRec> g_sz;
static bool g_sz_dirty = false;
static double g_sz_last_flush = 0.0;

static void save_sizes_locked() {
    if (!g_sz_dirty || demo_mode()) return;
    SDL_CreateDirectory(local_dir().c_str());
    SDL_CreateDirectory(cache_dir().c_str());
    std::string out = "# SteelTemple Sanctum: cached folder sizes (path=bytes|files|mtime|checked)\n";
    for (const auto &[k, r] : g_sz) {
        out += k + "=" + std::to_string(r.bytes) + "|" + std::to_string(r.files) + "|" +
               std::to_string((long long)r.dir_mtime) + "|" + std::to_string(r.checked) + "\n";
    }
    std::string tmp = sizes_path() + ".tmp";
    if (SDL_SaveFile(tmp.c_str(), out.data(), out.size())) {
        SDL_RenamePath(tmp.c_str(), sizes_path().c_str());
        g_sz_dirty = false;
    }
}

void cache_init() {
    std::lock_guard<std::mutex> l(g_sz_lock);
    g_sz.clear();
    size_t n = 0;
    char *d = (char *)SDL_LoadFile(sizes_path().c_str(), &n);
    if (!d) return;
    std::string all(d, n);
    SDL_free(d);
    size_t at = 0;
    while (at < all.size()) {
        size_t end = all.find('\n', at);
        if (end == std::string::npos) end = all.size();
        std::string line = all.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        long long b = 0, mt = 0, ck = 0;
        int f = 0;
        if (sscanf(v.c_str(), "%lld|%d|%lld|%lld", &b, &f, &mt, &ck) == 4 && b >= 0 && f >= 0)
            g_sz[k] = {b, f, (int64_t)mt, ck};
    }
    g_sz_dirty = false;
}

void cache_flush() {
    std::lock_guard<std::mutex> l(g_sz_lock);
    save_sizes_locked();
}

bool cache_size_get(const std::string &dir, long long &bytes_out, int &files_out) {
    if (dir.empty()) return false;
    SDL_PathInfo pi{};
    if (!SDL_GetPathInfo(dir.c_str(), &pi) || pi.type != SDL_PATHTYPE_DIRECTORY) return false;
    std::string k = norm_key(dir);
    long long now = (long long)time(nullptr);
    std::lock_guard<std::mutex> l(g_sz_lock);
    auto it = g_sz.find(k);
    if (it == g_sz.end()) return false;
    // Valid if directory mtime matches and entry is fresher than 15 minutes
    if (it->second.dir_mtime != (int64_t)pi.modify_time || now - it->second.checked > 900) return false;
    bytes_out = it->second.bytes;
    files_out = it->second.files;
    return true;
}

void cache_size_put(const std::string &dir, long long bytes, int files) {
    if (dir.empty() || demo_mode()) return;
    SDL_PathInfo pi{};
    int64_t mt = SDL_GetPathInfo(dir.c_str(), &pi) ? (int64_t)pi.modify_time : 0;
    std::string k = norm_key(dir);
    long long now = (long long)time(nullptr);
    std::lock_guard<std::mutex> l(g_sz_lock);
    g_sz[k] = {bytes, files, mt, now};
    g_sz_dirty = true;
    double t = SDL_GetTicks() / 1000.0;
    if (t - g_sz_last_flush >= 3.0) {
        g_sz_last_flush = t;
        save_sizes_locked();
    }
}

void cache_size_invalidate(const std::string &dir) {
    if (dir.empty()) return;
    std::lock_guard<std::mutex> l(g_sz_lock);
    if (g_sz.erase(norm_key(dir))) g_sz_dirty = true;
}

CacheStats cache_stats() {
    CacheStats s{};
    s.thumb_hits = g_thumb_hits.load();
    s.thumb_misses = g_thumb_misses.load();
    s.thumb_bytes = folder_size_bytes(thumbs_dir(), &s.thumb_files);
    s.meta_bytes = folder_size_bytes(local_dir() + "releases") + folder_size_bytes(local_dir() + "settings");
    for (const std::string &f : {local_dir() + "catalog.json", sizes_path()}) {
        SDL_PathInfo pi{};
        if (SDL_GetPathInfo(f.c_str(), &pi)) s.meta_bytes += pi.size;
    }
    s.download_bytes = folder_size_bytes(local_dir() + "downloads");
    s.total_bytes = s.thumb_bytes + s.meta_bytes + s.download_bytes;
    return s;
}

void cache_clear_sanctum() {
    if (demo_mode()) return;
    remove_tree(thumbs_dir());
    remove_tree(local_dir() + "downloads");
    SDL_RemovePath(sizes_path().c_str());
    {
        std::lock_guard<std::mutex> l(g_sz_lock);
        g_sz.clear();
        g_sz_dirty = false;
    }
    g_thumb_hits.store(0);
    g_thumb_misses.store(0);
}

}  // namespace sanctum
