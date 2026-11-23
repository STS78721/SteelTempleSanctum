#include "covers.h"

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "cache.h"
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
static std::map<std::string, std::string> g_paths;
static std::mutex g_lock;
static std::vector<Decoded> g_ready;
static std::set<std::string> g_matched_ids;   // set by a finished worker: the ports that currently have art
static bool g_finished = false;
static std::atomic<int> g_generation{0};
static std::atomic<int> g_matched{0};
static std::atomic<bool> g_running{false};
static std::string g_watch_sig;               // the folder's pictures (names, sizes, times) when last read
static double g_watch_next = 0;

bool load_image_rgba(const std::string &path, int max_dim, int &w_out, int &h_out, std::vector<unsigned char> &rgba_out) {
    if (max_dim > 0 && cache_thumb_load(path, max_dim, w_out, h_out, rgba_out)) return true;
    int w = 0, h = 0, n = 0;
    unsigned char *px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return false;
    }
    int f = 1;
    if (max_dim > 0) {
        int big = w > h ? w : h;
        while (big / (f * 2) >= max_dim && f < 8) f *= 2;
    }
    w_out = std::max(1, w / f);
    h_out = std::max(1, h / f);
    rgba_out.resize((size_t)w_out * h_out * 4);
    if (f == 1) {
        SDL_memcpy(rgba_out.data(), px, (size_t)w * h * 4);
    } else {
        for (int y = 0; y < h_out; ++y)
            for (int x = 0; x < w_out; ++x)
                for (int c = 0; c < 4; ++c) {
                    int sum = 0;
                    for (int yy = 0; yy < f; ++yy)
                        for (int xx = 0; xx < f; ++xx)
                            sum += px[((size_t)(y * f + yy) * w + (x * f + xx)) * 4 + c];
                    rgba_out[((size_t)y * w_out + x) * 4 + c] = (unsigned char)(sum / (f * f));
                }
    }
    stbi_image_free(px);
    if (max_dim > 0) cache_thumb_save(path, max_dim, w_out, h_out, rgba_out);
    return true;
}

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

struct ImgFile {
    std::string rel;    // relative to cover_dir, e.g. "Nintendo Switch 2 Icons\\Fast Fusion.png"
    std::string name;   // file basename only, e.g. "Fast Fusion.png"
    int depth = 0;
};

static void collect_images_walk(const std::string &root, const std::string &rel, int depth, int max_depth,
                                std::vector<ImgFile> &out) {
    std::string dir = rel.empty() ? root : root + "\\" + rel;
    int count = 0;
    char **entries = SDL_GlobDirectory((dir + "\\").c_str(), "*", SDL_GLOB_CASEINSENSITIVE, &count);
    for (int i = 0; entries && i < count; ++i) {
        const char *en = entries[i];
        if (!en || !en[0] || en[0] == '.' || en[0] == '$') continue;
        std::string sub_rel = rel.empty() ? std::string(en) : rel + "\\" + en;
        std::string full = root + "\\" + sub_rel;
        SDL_PathInfo info{};
        if (!SDL_GetPathInfo(full.c_str(), &info)) continue;
        if (info.type == SDL_PATHTYPE_DIRECTORY) {
            if (depth < max_depth) collect_images_walk(root, sub_rel, depth + 1, max_depth, out);
        } else if (info.type == SDL_PATHTYPE_FILE && is_image(lower(en))) {
            out.push_back({sub_rel, en, depth});
        }
    }
    SDL_free(entries);
}

static std::vector<ImgFile> collect_images(const std::string &dir, int max_depth = 3) {
    std::vector<ImgFile> out;
    if (!dir.empty()) collect_images_walk(dir, "", 0, max_depth, out);
    return out;
}

struct PortSnap {
    std::string id, cover_key, name, mono, custom_cover;
};

// How well a file's name fits a port: 3 its full title, 2 the port's own name, 1 the short name on its own;
// 0 no match. The short name must be the whole name ("OoT.png"), so "ff" never matches "Diff.png".
static int score(const PortSnap &p, const std::string &file) {
    std::string stem = file.substr(0, file.find_last_of('.'));
    std::string k = key_of(stem.c_str());
    if (!p.cover_key.empty() && k.find(p.cover_key) != std::string::npos) return 3;
    std::string name = key_of(p.name.c_str());
    if (name.size() >= 4 && name != "pcgame" && k.find(name) != std::string::npos) return 2;
    if (!p.mono.empty() && k == key_of(p.mono.c_str())) return 1;
    return 0;
}

struct Job {
    std::string dir;
    std::vector<PortSnap> snaps;
    int generation;
};

static int worker(void *p) {
    Job *job = (Job *)p;
    std::vector<ImgFile> files = collect_images(job->dir, 3);
    std::set<std::string> matched;
    for (const PortSnap &port : job->snaps) {
        std::string chosen_path;
        if (!port.custom_cover.empty() && SDL_GetPathInfo(port.custom_cover.c_str(), nullptr)) {
            chosen_path = port.custom_cover;
        } else if (!files.empty()) {
            const ImgFile *best = nullptr;
            int best_score = 0;
            for (const ImgFile &f : files) {
                int s = score(port, f.name);
                if (!s) continue;
                bool better = s > best_score ||
                              (s == best_score && best &&
                               (f.name.size() < best->name.size() ||
                                (f.name.size() == best->name.size() && f.depth < best->depth)));
                if (better) best = &f, best_score = s;
            }
            if (best) chosen_path = job->dir + "\\" + best->rel;
        }
        if (chosen_path.empty()) continue;
        Decoded d;
        d.id = port.id;
        if (!load_image_rgba(chosen_path, 512, d.w, d.h, d.rgba)) continue;
        matched.insert(port.id);
        std::lock_guard<std::mutex> l(g_lock);
        if (job->generation == g_generation) {
            g_paths[port.id] = chosen_path;
            g_ready.push_back(std::move(d));
        }
    }
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

// the pictures in the folder (and subfolders), as one string: changes when one is added, removed, renamed or replaced
static std::string signature(const std::string &dir) {
    std::string sig;
    for (const ImgFile &f : collect_images(dir, 3)) {
        SDL_PathInfo info{};
        if (!SDL_GetPathInfo((dir + "\\" + f.rel).c_str(), &info)) continue;
        sig += f.rel;
        sig += '|' + std::to_string(info.size) + '|' + std::to_string(info.modify_time) + '\n';
    }
    return sig;
}

void covers_init(SDL_Renderer *r) {
    g_r = r;
    covers_reload();
}

void covers_reload() {
    if (!g_r) return;
    // the covers on screen stay until their replacements are ready (no flash of empty tiles)
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_ready.clear();
        g_finished = false;
    }
    int gen = ++g_generation;
    bool has_custom = false;
    for (auto &e : cfg().port_covers) if (!e.second.empty()) { has_custom = true; break; }
    if (cfg().cover_dir.empty() && !has_custom) {
        for (auto &t : g_tex) SDL_DestroyTexture(t.second);
        g_tex.clear();
        g_matched = 0;
        g_watch_sig.clear();
        return;
    }
    g_watch_sig = cfg().cover_dir.empty() ? std::string() : signature(cfg().cover_dir);
    std::vector<PortSnap> snaps;
    for (const Port &port : ports()) {
        auto it = cfg().port_covers.find(port.id);
        snaps.push_back({port.id, port.cover_key ? port.cover_key : "", port.name ? port.name : "",
                         port.mono ? port.mono : "", it == cfg().port_covers.end() ? std::string() : it->second});
    }
    g_running = true;
    Job *job = new Job{cfg().cover_dir, std::move(snaps), gen};
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
    // watch the folder: new or changed pictures appear on their own, every 8 seconds (low UI-thread I/O)
    if (now >= g_watch_next && !g_running && !cfg().cover_dir.empty()) {
        g_watch_next = now + 8.0;
        if (signature(cfg().cover_dir) != g_watch_sig) covers_reload();
    }
}

SDL_Texture *cover(const char *id) {
    auto it = g_tex.find(id);
    return it == g_tex.end() ? nullptr : it->second;
}

std::string cover_path_of(const char *id) {
    if (!id) return {};
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_paths.find(id);
    return it == g_paths.end() ? std::string() : it->second;
}

int covers_matched() { return g_matched; }

int count_pngs(const std::string &dir) {
    return (int)collect_images(dir, 3).size();
}

}  // namespace sanctum
