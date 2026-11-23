// Switch-style Album: scans installed ports' screenshot folders + optional user screenshots folder,
// decodes thumbnails asynchronously via covers.h's shared stb_image loader, and provides a full-screen lightbox.
#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "app.h"
#include "catalog.h"
#include "config.h"
#include "covers.h"
#include "draw.h"
#include "github.h"
#include "platform.h"
#include "sound.h"

namespace sanctum {

struct Photo {
    std::string path, name, source, date;
    long long mtime = 0;
};

struct DecodedPhoto {
    std::string path;
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
    bool full = false;
};

static SDL_Renderer *g_ren = nullptr;
static std::vector<Photo> g_photos;
static std::vector<std::string> g_sources;   // "All", plus each source with >= 1 photo
static int g_src_idx = 0, g_sel = 0, g_top = 0;
static bool g_lightbox = false;
static double g_del_armed = -10;

static std::mutex g_lock;
static std::vector<DecodedPhoto> g_ready;
static std::map<std::string, SDL_Texture *> g_thumbs;
static SDL_Texture *g_full_tex = nullptr;
static std::string g_full_path, g_full_req;
static std::atomic<int> g_gen{0};
static std::atomic<bool> g_scanning{false};

static bool is_img_file(const std::string &n) {
    auto end = [&](const char *e) {
        size_t l = SDL_strlen(e);
        return n.size() > l && SDL_strcasecmp(n.c_str() + n.size() - l, e) == 0;
    };
    return end(".png") || end(".jpg") || end(".jpeg") || end(".bmp");
}

static std::string fmt_date(long long ns) {
    time_t t = (time_t)(ns / 1000000000LL);
    if (t <= 0) return "";
    struct tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d  %H:%M", &lt);
    return b;
}

static void collect_dir(const std::string &dir, const std::string &source, std::vector<Photo> &out,
                        std::set<std::string> &seen, int depth) {
    if (dir.empty() || depth < 0) return;
    int n = 0;
    char **names = SDL_GlobDirectory((dir + "\\").c_str(), "*", SDL_GLOB_CASEINSENSITIVE, &n);
    for (int i = 0; names && i < n; ++i) {
        if (!names[i] || names[i][0] == '.' || names[i][0] == '$') continue;
        std::string full = dir + "\\" + names[i];
        SDL_PathInfo pi{};
        if (!SDL_GetPathInfo(full.c_str(), &pi)) continue;
        if (pi.type == SDL_PATHTYPE_DIRECTORY) {
            if (depth > 0) collect_dir(full, source, out, seen, depth - 1);
        } else if (pi.type == SDL_PATHTYPE_FILE && is_img_file(names[i])) {
            std::string low = full;
            for (char &c : low) c = (char)SDL_tolower((unsigned char)c);
            if (!seen.insert(low).second) continue;
            out.push_back({full, names[i], source, fmt_date(pi.modify_time), (long long)pi.modify_time});
        }
    }
    SDL_free(names);
}

struct ThumbJob {
    int gen;
    std::vector<std::string> paths;
};

static int thumb_worker(void *p) {
    ThumbJob *j = (ThumbJob *)p;
    for (const std::string &path : j->paths) {
        if (j->gen != g_gen.load()) break;
        DecodedPhoto dp;
        dp.path = path;
        if (load_image_rgba(path, 320, dp.w, dp.h, dp.rgba)) {
            std::lock_guard<std::mutex> l(g_lock);
            if (j->gen == g_gen.load()) g_ready.push_back(std::move(dp));
        }
    }
    delete j;
    g_scanning = false;
    return 0;
}

struct FullJob {
    int gen;
    std::string path;
};

static int full_worker(void *p) {
    FullJob *j = (FullJob *)p;
    DecodedPhoto dp;
    dp.path = j->path;
    dp.full = true;
    if (load_image_rgba(j->path, 1280, dp.w, dp.h, dp.rgba)) {
        std::lock_guard<std::mutex> l(g_lock);
        if (j->gen == g_gen.load()) g_ready.push_back(std::move(dp));
    }
    delete j;
    return 0;
}

static void request_full(const std::string &path) {
    if (path == g_full_req) return;
    g_full_req = path;
    if (g_full_tex) { SDL_DestroyTexture(g_full_tex); g_full_tex = nullptr; }
    g_full_path.clear();
    if (path.empty()) return;
    FullJob *fj = new FullJob{g_gen.load(), path};
    SDL_Thread *t = SDL_CreateThread(full_worker, "album-full", fj);
    if (t) SDL_DetachThread(t);
    else delete fj;
}

static void clear_textures() {
    for (auto &e : g_thumbs) SDL_DestroyTexture(e.second);
    g_thumbs.clear();
    if (g_full_tex) { SDL_DestroyTexture(g_full_tex); g_full_tex = nullptr; }
    g_full_path.clear();
    g_full_req.clear();
}

static void album_scan() {
    int gen = ++g_gen;
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_ready.clear();
    }
    clear_textures();
    g_photos.clear();
    g_sources = {"All"};
    g_src_idx = 0, g_sel = 0, g_top = 0, g_lightbox = false;

    if (demo_mode()) return;

    std::set<std::string> seen;
    // Sanctum's own instant-capture Album directory (%LOCALAPPDATA%\SteelTemple\Sanctum\screenshots\<port_id>\)
    {
        std::string own = local_dir() + "screenshots";
        if (SDL_GetPathInfo(own.c_str(), nullptr)) {
            for (const Port &p : ports())
                collect_dir(own + "\\" + p.id, p.name, g_photos, seen, 1);
            collect_dir(own + "\\sanctum", "Sanctum", g_photos, seen, 1);
            collect_dir(own, "Sanctum", g_photos, seen, 0);
        }
    }
    for (const Port &p : ports()) {
        const Install *in = p.best();
        if (!in) continue;
        for (const char *sub : {"screenshots", "captures", "user\\screenshots"})
            collect_dir(in->dir + "\\" + sub, p.name, g_photos, seen, 1);
    }
    if (!cfg().screenshots_dir.empty())
        collect_dir(cfg().screenshots_dir, "Screenshots", g_photos, seen, 2);
    else if (const char *pics = SDL_GetUserFolder(SDL_FOLDER_PICTURES)) {
        std::string s = std::string(pics) + "Screenshots";
        if (SDL_GetPathInfo(s.c_str(), nullptr)) collect_dir(s, "Screenshots", g_photos, seen, 1);
    }

    std::sort(g_photos.begin(), g_photos.end(), [](const Photo &a, const Photo &b) { return a.mtime > b.mtime; });
    if (g_photos.size() > 120) g_photos.resize(120);

    std::set<std::string> src_set;
    for (const Photo &ph : g_photos)
        if (src_set.insert(ph.source).second) g_sources.push_back(ph.source);

    if (!g_photos.empty()) {
        ThumbJob *tj = new ThumbJob{gen, {}};
        for (const Photo &ph : g_photos) tj->paths.push_back(ph.path);
        g_scanning = true;
        SDL_Thread *t = SDL_CreateThread(thumb_worker, "album-thumbs", tj);
        if (t) SDL_DetachThread(t);
        else { delete tj; g_scanning = false; }
    }
}

void album_init(SDL_Renderer *r) { g_ren = r; }

std::string album_take_screenshot(void *target_hwnd, int port_idx, bool allow_print_window) {
    if (demo_mode()) return {};
    std::string sub = "sanctum";
    if (port_idx >= 0 && port_idx < (int)ports().size()) sub = ports()[port_idx].id;
    std::string root = local_dir() + "screenshots";
    std::string dir = root + "\\" + sub;
    SDL_CreateDirectory(root.c_str());
    SDL_CreateDirectory(dir.c_str());

    time_t now = time(nullptr);
    struct tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    localtime_r(&lt, &now);
#endif
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &lt);
    std::string base = dir + "\\" + sub + "-" + stamp;
    std::string dst = base + ".png";
    for (int k = 1; k < 20 && SDL_GetPathInfo(dst.c_str(), nullptr); ++k)
        dst = base + "-" + std::to_string(k) + ".png";

    int w = 0, h = 0;
    if (!capture_screenshot_png(target_hwnd, dst, &w, &h, allow_print_window)) {
        sanctum_log("album: screenshot capture failed (dst=%s)", dst.c_str());
        return {};
    }
    sanctum_log("album: captured screenshot %s (%dx%d)", dst.c_str(), w, h);
    return dst;
}

void album_open() {
    album_scan();
    go(SCR_ALBUM);
}

void album_poll() {
    if (!g_ren) return;
    std::vector<DecodedPhoto> ready;
    {
        std::lock_guard<std::mutex> l(g_lock);
        ready.swap(g_ready);
    }
    for (DecodedPhoto &d : ready) {
        SDL_Texture *t = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, d.w, d.h);
        if (!t) continue;
        SDL_UpdateTexture(t, nullptr, d.rgba.data(), d.w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
        if (d.full) {
            if (d.path == g_full_req) {
                if (g_full_tex) SDL_DestroyTexture(g_full_tex);
                g_full_tex = t;
                g_full_path = d.path;
            } else {
                SDL_DestroyTexture(t);
            }
        } else {
            if (g_thumbs.count(d.path)) SDL_DestroyTexture(g_thumbs[d.path]);
            g_thumbs[d.path] = t;
        }
    }
}

static void open_photo_folder(const std::string &path) {
    size_t sl = path.find_last_of("\\/");
    std::string dir = sl == std::string::npos ? path : path.substr(0, sl);
    sound_play(SFX_OK);
    SDL_OpenURL(("file:///" + dir).c_str());
}

void album_frame() {
    App &a = app();
    const Palette &p = pal();

    // Filtered photo indices
    std::vector<int> vis;
    const std::string &want_src = g_sources[g_src_idx < (int)g_sources.size() ? g_src_idx : 0];
    for (int i = 0; i < (int)g_photos.size(); ++i)
        if (g_src_idx == 0 || g_photos[i].source == want_src) vis.push_back(i);
    int n = (int)vis.size();
    if (g_sel >= n) g_sel = std::max(0, n - 1);
    if (n == 0) g_lightbox = false;

    const int kCols = 4, kVisRows = 3;

    if (g_lightbox && n > 0) {
        const Photo &ph = g_photos[vis[g_sel]];
        request_full(ph.path);
        switch (a.nav) {
        case NAV_LEFT:
        case NAV_L:
            if (g_sel > 0) { g_sel--; sound_play(SFX_MOVE); } else sound_play(SFX_EDGE);
            break;
        case NAV_RIGHT:
        case NAV_R:
            if (g_sel < n - 1) { g_sel++; sound_play(SFX_MOVE); } else sound_play(SFX_EDGE);
            break;
        case NAV_X: open_photo_folder(ph.path); break;
        case NAV_Y:
            if (a.now - g_del_armed < 4.0) {
                g_del_armed = -10;
                std::string target = ph.path;
                if (recycle_path(target)) {
                    sound_play(SFX_BACK);
                    toast("Moved screenshot to Recycle Bin");
                    g_photos.erase(g_photos.begin() + vis[g_sel]);
                    if (g_thumbs.count(target)) { SDL_DestroyTexture(g_thumbs[target]); g_thumbs.erase(target); }
                    if (g_photos.empty()) g_lightbox = false;
                    return;
                } else {
                    sound_play(SFX_ERROR);
                    toast("Could not delete screenshot");
                }
            } else {
                g_del_armed = a.now;
                sound_play(SFX_ERROR);
                toast("Press Y again to move screenshot to Recycle Bin", 4);
            }
            break;
        case NAV_B:
        case NAV_A:
            g_lightbox = false;
            sound_play(SFX_BACK);
            break;
        default: break;
        }

        // Draw full-screen lightbox
        rect(0, 0, 1280, 720, IM_COL32(10, 10, 14, 255));
        SDL_Texture *tex = (g_full_tex && g_full_path == ph.path) ? g_full_tex
                         : (g_thumbs.count(ph.path) ? g_thumbs[ph.path] : nullptr);
        if (tex) {
            float tw = 0, th = 0;
            SDL_GetTextureSize(tex, &tw, &th);
            float max_w = 1160, max_h = 540;
            float sc = std::min(max_w / std::max(1.0f, tw), max_h / std::max(1.0f, th));
            float dw = tw * sc, dh = th * sc;
            float dx = 640 - dw / 2, dy = 352 - dh / 2;
            dl()->AddImageRounded((ImTextureID)(intptr_t)tex, P(dx, dy), P(dx + dw, dy + dh), ImVec2(0, 0), ImVec2(1, 1),
                                  IM_COL32_WHITE, S(12));
        } else {
            text(640, 340, 18, p.soft, "Loading...", 0.5f);
        }
        // Header bar
        rect(0, 0, 1280, 68, IM_COL32(0, 0, 0, 140));
        text(48, 16, 19, IM_COL32_WHITE, fit(19, ph.name, 640).c_str());
        text(48, 42, 13, IM_COL32(200, 200, 210, 255), (ph.source + (ph.date.empty() ? "" : "  ·  " + ph.date)).c_str());
        std::string idx_lbl = std::to_string(g_sel + 1) + " / " + std::to_string(n);
        text(1232, 24, 16, IM_COL32(220, 220, 230, 255), idx_lbl.c_str(), 1);
        if (a.clicked && hover(40, 72, 1200, 560)) { g_lightbox = false; sound_play(SFX_BACK); }
        hint_bar({{NAV_L, "Prev"}, {NAV_R, "Next"}, {NAV_X, "Open folder"}, {NAV_Y, "Delete"}, {NAV_B, "Back"}});
        return;
    }

    switch (a.nav) {
    case NAV_LEFT:
        if (n && g_sel % kCols > 0) { g_sel--; sound_play(SFX_MOVE); } else if (n) sound_play(SFX_EDGE);
        break;
    case NAV_RIGHT:
        if (n && g_sel < n - 1 && g_sel % kCols < kCols - 1) { g_sel++; sound_play(SFX_MOVE); } else if (n) sound_play(SFX_EDGE);
        break;
    case NAV_UP:
        if (n && g_sel >= kCols) { g_sel -= kCols; sound_play(SFX_MOVE); } else if (n) sound_play(SFX_EDGE);
        break;
    case NAV_DOWN:
        if (n && g_sel + kCols < n) { g_sel += kCols; sound_play(SFX_MOVE); } else if (n) sound_play(SFX_EDGE);
        break;
    case NAV_L:
        if (g_sources.size() > 1) { g_src_idx = (g_src_idx + (int)g_sources.size() - 1) % (int)g_sources.size(); g_sel = 0, g_top = 0; sound_play(SFX_TAB); }
        break;
    case NAV_R:
        if (g_sources.size() > 1) { g_src_idx = (g_src_idx + 1) % (int)g_sources.size(); g_sel = 0, g_top = 0; sound_play(SFX_TAB); }
        break;
    case NAV_A:
        if (n > 0) { g_lightbox = true; sound_play(SFX_OK); }
        else folder_open(cfg().screenshots_dir, FOLDER_SCREENSHOTS);
        break;
    case NAV_X:
        if (n > 0) open_photo_folder(g_photos[vis[g_sel]].path);
        break;
    case NAV_Y:
        folder_open(cfg().screenshots_dir, FOLDER_SCREENSHOTS);
        return;
    case NAV_PLUS:
        album_scan();
        sound_play(SFX_OK);
        toast("Refreshed Album");
        break;
    case NAV_B:
        go_back();
        return;
    default: break;
    }

    int row = g_sel / kCols;
    if (row < g_top) g_top = row;
    if (row >= g_top + kVisRows) g_top = row - kVisRows + 1;

    background(0);
    status_bar();
    text(96, 86, 28, p.text, "Album");
    std::string cnt_lbl = n ? (std::to_string(n) + (n == 1 ? " screenshot" : " screenshots")) : "No screenshots found";
    text(1184, 96, 15, p.soft, cnt_lbl.c_str(), 1);

    // Filter pills
    float fx = 96;
    for (int i = 0; i < (int)g_sources.size(); ++i) {
        float w = text_w(14, g_sources[i].c_str()) + 28;
        if (fx + w > 1184) break;
        if (a.clicked && hover(fx, 128, w, 30)) { g_src_idx = i, g_sel = 0, g_top = 0; sound_play(SFX_TAB); }
        bool sel = i == g_src_idx;
        rect(fx, 128, w, 30, sel ? p.accent : p.panel, 15);
        text(fx + w / 2, 135, 14, sel ? IM_COL32_WHITE : p.soft, g_sources[i].c_str(), 0.5f);
        fx += w + 10;
    }

    if (n == 0) {
        rect(96, 184, 1088, 150, p.panel, 18);
        icon(IC_ALBUM, 154, 259, 44, p.accent);
        text(204, 218, 21, p.text, "Your screenshots show up here");
        text(204, 252, 15, p.soft, "Sanctum checks each installed port's screenshots folder and your Windows Screenshots folder.");
        text(204, 278, 15, p.dim, "Press Y to choose a custom screenshots folder.");
        hint_bar({{NAV_Y, "Screenshots folder"}, {NAV_PLUS, "Refresh"}, {NAV_B, "Back"}});
        return;
    }

    float cw = 258, ch = 148, gx = 18, gy = 16;
    for (int r = g_top; r < g_top + kVisRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            int idx = r * kCols + c;
            if (idx >= n) break;
            const Photo &ph = g_photos[vis[idx]];
            float x = 96 + c * (cw + gx), y = 176 + (r - g_top) * (ch + gy);
            if (hover(x, y, cw, ch)) {
                if (a.mouse_moved && !a.clicked && g_sel != idx) { g_sel = idx; sound_play(SFX_MOVE); }
                if (a.clicked) { g_sel = idx, g_lightbox = true; sound_play(SFX_OK); }
            }
            rect(x, y, cw, ch, p.panel, 12);
            if (g_thumbs.count(ph.path)) {
                dl()->AddImageRounded((ImTextureID)(intptr_t)g_thumbs[ph.path], P(x, y), P(x + cw, y + ch), ImVec2(0, 0), ImVec2(1, 1),
                                      IM_COL32_WHITE, S(12));
            } else {
                icon(IC_ALBUM, x + cw / 2, y + ch / 2 - 8, 32, p.dim);
            }
            // subtle bottom gradient strip with source + date
            dl()->AddRectFilled(P(x, y + ch - 30), P(x + cw, y + ch), IM_COL32(0, 0, 0, 175), S(12), ImDrawFlags_RoundCornersBottom);
            text(x + 10, y + ch - 23, 12, IM_COL32_WHITE, fit(12, ph.source, 110).c_str());
            text(x + cw - 10, y + ch - 23, 12, IM_COL32(210, 210, 220, 255), ph.date.substr(0, 10).c_str(), 1);
            if (idx == g_sel) focus_ring(x, y, cw, ch, 12, 4);
        }
    }

    int total_rows = (n + kCols - 1) / kCols;
    if (total_rows > kVisRows) {
        float h = (ch + gy) * kVisRows - gy, bh = h * kVisRows / (float)total_rows;
        rect(1200, 176, 4, h, p.line, 2);
        rect(1200, 176 + (h - bh) * g_top / std::max(1, total_rows - kVisRows), 4, bh, p.soft, 2);
    }

    hint_bar({{NAV_A, "View"}, {NAV_L, "Filter"}, {NAV_X, "Open folder"}, {NAV_Y, "Folder"}, {NAV_B, "Back"}});
}

}  // namespace sanctum
