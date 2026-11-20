#include "covers.h"

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "catalog.h"
#include "config.h"

#define STBI_WINDOWS_UTF8
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include "stb_image.h"

namespace sanctum {

struct Decoded {
    std::string id;
    int w, h;
    std::vector<unsigned char> rgba;
};

static SDL_Renderer *g_r;
static std::map<std::string, SDL_Texture *> g_tex;
static std::mutex g_lock;
static std::vector<Decoded> g_ready;
static std::set<std::string> g_matched_ids;   // set by a finished worker: the ports this folder has art for
static bool g_finished = false;
static std::atomic<int> g_generation{0};
static std::atomic<int> g_matched{0};
static std::atomic<bool> g_running{false};
static std::string g_watch_sig;               // the folder's pictures (names, sizes, times) when last read
static double g_watch_next = 0;

static std::string key_of(const char *name) {   // letters and digits, lower case (also drops "꞉", "’" and the like)
    std::string k;
    for (const unsigned char *c = (const unsigned char *)name; *c; ++c)
        if (SDL_isalnum(*c)) k += (char)SDL_tolower(*c);
    return k;
}

static bool is_image(const std::string &lower_name) {
    for (const char *e : {".png", ".jpg", ".jpeg", ".bmp"}) {
        size_t n = SDL_strlen(e);
        if (lower_name.size() > n && lower_name.compare(lower_name.size() - n, n, e) == 0) return true;
    }
    return false;
}

static std::string lower(const char *s) {
    std::string o;
    for (; *s; ++s) o += (char)SDL_tolower((unsigned char)*s);
    return o;
}

// How well a file's name fits a port: 3 its full title, 2 the port's own name, 1 the short name on its own;
// 0 no match. The short name must be the whole name ("OoT.png"), so "ff" never matches "Diff.png".
static int score(const Port &p, const std::string &file) {
    std::string stem = file.substr(0, file.find_last_of('.'));
    std::string k = key_of(stem.c_str());
    if (k.find(p.cover_key) != std::string::npos) return 3;
    std::string name = key_of(p.name);
    if (name.size() >= 4 && k.find(name) != std::string::npos) return 2;
    if (k == key_of(p.mono)) return 1;
    return 0;
}

struct Job {
    std::string dir;
    int generation;
};

static int worker(void *p) {
    Job *job = (Job *)p;
    int count = 0;
    char **files = SDL_GlobDirectory(job->dir.c_str(), "*", SDL_GLOB_CASEINSENSITIVE, &count);
    std::set<std::string> matched;
    for (Port &port : ports()) {
        std::string best;
        int best_score = 0;
        for (int i = 0; files && i < count; ++i) {
            if (!is_image(lower(files[i]))) continue;
            int s = score(port, files[i]);
            if (s > best_score || (s && s == best_score && SDL_strlen(files[i]) < best.size()))
                best = files[i], best_score = s;
        }
        if (best.empty()) continue;
        int w, h, n;
        unsigned char *px = stbi_load((job->dir + "\\" + best).c_str(), &w, &h, &n, 4);
        if (!px) continue;
        matched.insert(port.id);
        Decoded d{port.id, w, h, {}};
        // halve big covers (1024 -> 512): sharp at tile size, a quarter of the memory, uploads in no time
        int f = (w >= 1024 && h >= 1024) ? 2 : 1;
        d.w = w / f, d.h = h / f;
        d.rgba.resize((size_t)d.w * d.h * 4);
        for (int y = 0; y < d.h; ++y)
            for (int x = 0; x < d.w; ++x)
                for (int c = 0; c < 4; ++c) {
                    int sum = 0;
                    for (int yy = 0; yy < f; ++yy)
                        for (int xx = 0; xx < f; ++xx) sum += px[((size_t)(y * f + yy) * w + (x * f + xx)) * 4 + c];
                    d.rgba[((size_t)y * d.w + x) * 4 + c] = (unsigned char)(sum / (f * f));
                }
        stbi_image_free(px);
        std::lock_guard<std::mutex> l(g_lock);
        if (job->generation == g_generation) g_ready.push_back(std::move(d));
    }
    SDL_free(files);
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (job->generation == g_generation) {
            g_matched_ids = matched;
            g_finished = true;
            g_matched = (int)matched.size();
        }
    }
    g_running = false;
    delete job;
    return 0;
}

// the pictures in the folder, as one string: changes when one is added, removed, renamed or replaced
static std::string signature(const std::string &dir) {
    int count = 0;
    char **files = SDL_GlobDirectory(dir.c_str(), "*", SDL_GLOB_CASEINSENSITIVE, &count);
    std::string sig;
    for (int i = 0; files && i < count; ++i) {
        if (!is_image(lower(files[i]))) continue;
        SDL_PathInfo info;
        if (!SDL_GetPathInfo((dir + "\\" + files[i]).c_str(), &info)) continue;
        sig += files[i];
        sig += '|' + std::to_string(info.size) + '|' + std::to_string(info.modify_time) + '\n';
    }
    SDL_free(files);
    return sig;
}

void covers_init(SDL_Renderer *r) {
    g_r = r;
    covers_reload();
}

void covers_reload() {
    // the covers on screen stay until their replacements are ready (no flash of empty tiles)
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_ready.clear();
        g_finished = false;
    }
    int gen = ++g_generation;
    if (cfg().cover_dir.empty()) {
        for (auto &t : g_tex) SDL_DestroyTexture(t.second);
        g_tex.clear();
        g_matched = 0;
        g_watch_sig.clear();
        return;
    }
    g_watch_sig = signature(cfg().cover_dir);
    g_running = true;
    Job *job = new Job{cfg().cover_dir, gen};
    SDL_Thread *t = SDL_CreateThread(worker, "covers", job);
    if (t) SDL_DetachThread(t);
    else { delete job; g_running = false; }
}

void covers_poll(double now) {
    std::vector<Decoded> ready;
    std::set<std::string> keep;
    bool finished = false;
    {
        std::lock_guard<std::mutex> l(g_lock);
        ready.swap(g_ready);
        if (g_finished) finished = true, keep = g_matched_ids, g_finished = false;
    }
    for (Decoded &d : ready) {
        SDL_Texture *t = SDL_CreateTexture(g_r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, d.w, d.h);
        if (!t) continue;
        SDL_UpdateTexture(t, nullptr, d.rgba.data(), d.w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
        if (g_tex.count(d.id)) SDL_DestroyTexture(g_tex[d.id]);
        g_tex[d.id] = t;
    }
    if (finished) {   // a port whose picture left the folder goes back to its colours
        for (auto it = g_tex.begin(); it != g_tex.end();) {
            if (!keep.count(it->first)) SDL_DestroyTexture(it->second), it = g_tex.erase(it);
            else ++it;
        }
    }
    // watch the folder: new or changed pictures appear on their own, every two seconds at most
    if (now >= g_watch_next && !g_running && !cfg().cover_dir.empty()) {
        g_watch_next = now + 2.0;
        if (signature(cfg().cover_dir) != g_watch_sig) covers_reload();
    }
}

SDL_Texture *cover(const char *id) {
    auto it = g_tex.find(id);
    return it == g_tex.end() ? nullptr : it->second;
}

int covers_matched() { return g_matched; }

int count_pngs(const std::string &dir) {
    int count = 0, n = 0;
    char **f = SDL_GlobDirectory(dir.c_str(), "*", SDL_GLOB_CASEINSENSITIVE, &count);
    for (int i = 0; f && i < count; ++i) n += is_image(lower(f[i]));
    SDL_free(f);
    return n;
}

}  // namespace sanctum
