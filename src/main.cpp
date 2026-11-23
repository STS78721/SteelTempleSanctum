// SteelTemple Sanctum: a console-style home for STS78721's PC ports.
//
// One window, SDL_Renderer and Dear ImGui's draw lists; everything on a 1280x720 canvas scaled into the window.
// Start-up is instant: the ports found last time (library.ini) show at once, the search of this PC and the cover art
// load on worker threads and slide in when ready.
//
// Test hooks (environment):
//   SANCTUM_NAV="R R D A"   presses, one every 8 frames from frame 30: L R U D A B X Y LB RB + - (H = Home)
//   SANCTUM_SHOT=<file.bmp> saves the frame SANCTUM_SHOT_FRAME (default 120) and quits
//   SANCTUM_THEME=light     this run only
//   SANCTUM_WINDOWED=1      start in a 1280x720 window whatever the setting
//   SANCTUM_SHOT_IDLE=1     take the shot once the downloads queue is done (from SANCTUM_SHOT_FRAME on)
//   SANCTUM_LIBRARY=<dir>   install new ports there this run (tests)
//   SANCTUM_FOCUS=<port id> Home starts focused on that port (tests: never count tiles, the order changes)
#include <SDL3/SDL.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

#include "app.h"
#include "avatars.h"
#include "catalog.h"
#include "config.h"
#include "covers.h"
#include "discord_rpc.h"
#include "draw.h"
#include "github.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "input.h"
#include "install.h"
#include "json.h"
#include "launch.h"
#include "mods.h"
#include "selfupdate.h"
#include "sound.h"
#include "stb_image.h"   // implemented in covers.cpp
#include "theme.h"

namespace sanctum {

static App g_app;
App &app() { return g_app; }

void go(Screen s, bool silent) {
    if (s == g_app.screen) return;
    if (!silent) sound_play(SFX_OK);
    g_app.back.push_back(g_app.screen);
    g_app.screen = s;
    g_app.trans_t = g_app.now;
    g_app.trans_dir = 1;
    g_app.nav = NAV_NONE;
    g_app.clicked = false;
}

void go_back(bool silent) {
    if (!silent) sound_play(SFX_BACK);
    g_app.screen = g_app.back.empty() ? SCR_HOME : g_app.back.back();
    if (!g_app.back.empty()) g_app.back.pop_back();
    g_app.trans_t = g_app.now;
    g_app.trans_dir = -1;
    g_app.nav = NAV_NONE;
    g_app.clicked = false;
}

void toast(const std::string &t, double seconds) { g_app.toast = {t, g_app.now + seconds, g_app.now}; }

void sanctum_log(const char *fmt, ...) {
    static SDL_Mutex *lock = SDL_CreateMutex();
    const char *path = getenv("SANCTUM_LOG");
    if (!path) return;
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    SDL_vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    SDL_LockMutex(lock);
    if (FILE *f = fopen(path, "a")) {
        fprintf(f, "%8.3f %s\n", SDL_GetTicks() / 1000.0, line);
        fclose(f);
    }
    SDL_UnlockMutex(lock);
}

void set_fullscreen(bool on) {
    SDL_SetWindowFullscreen(g_app.window, on);   // borderless at the desktop's resolution: instant, no mode change
    if (cfg().fullscreen != on) {
        cfg().fullscreen = on;
        config_save();
    }
}

void apply_display(const std::string &display_name) {
    if (!g_app.window) return;
    SDL_DisplayID target = SDL_GetPrimaryDisplay();
    if (!display_name.empty()) {
        int cnt = 0;
        if (SDL_DisplayID *ids = SDL_GetDisplays(&cnt)) {
            for (int i = 0; i < cnt; ++i) {
                const char *nm = SDL_GetDisplayName(ids[i]);
                if (nm && display_name == nm) { target = ids[i]; break; }
            }
            SDL_free(ids);
        }
    }
    SDL_Rect b{};
    if (!SDL_GetDisplayBounds(target, &b)) return;
    bool was_fs = (SDL_GetWindowFlags(g_app.window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (was_fs) SDL_SetWindowFullscreen(g_app.window, false);
    int ww = 1280, wh = 720;
    SDL_GetWindowSize(g_app.window, &ww, &wh);
    SDL_SetWindowPosition(g_app.window, b.x + std::max(0, (b.w - ww) / 2), b.y + std::max(0, (b.h - wh) / 2));
    if (was_fs) SDL_SetWindowFullscreen(g_app.window, true);
}

bool hover(float x, float y, float w, float h) {
    ImVec2 m = g_app.mouse;
    return m.x >= x && m.x < x + w && m.y >= y && m.y < y + h;
}

// what the workers finished, taken in every frame, and while a game runs too
static void background_poll(SDL_Window *win) {
    if (game_running() && input_poll_force_close(g_app.now)) {
        sanctum_log("force-close: controller chord held 2 s");
        close_game();
        toast("Force-closed game via controller chord", 3.5);
    }
    launch_poll(win);
    install_poll(win);
    bool found = scan_poll();
    bool fresh = github_poll();
    if (found || fresh) version_probe_start();   // new copies or new tags: read the programs' versions again
    version_probe_poll();
}

bool demo_mode() {
    static const bool on = getenv("SANCTUM_DEMO") != nullptr;
    return on;
}

// a folder as the screen shows it: in demo mode a stand-in, so screenshots never show this PC's folders
std::string shown_path(const std::string &real, const char *port_title) {
    if (!demo_mode()) return real;
    return port_title ? std::string("D:\\Games\\") + port_title : std::string("D:\\Games\\SteelTemple");
}

static Nav nav_of(const std::string &t) {
    static const struct { const char *s; Nav n; } k[] = {
        {"L", NAV_LEFT}, {"R", NAV_RIGHT}, {"U", NAV_UP}, {"D", NAV_DOWN}, {"A", NAV_A}, {"B", NAV_B}, {"X", NAV_X},
        {"Y", NAV_Y}, {"LB", NAV_L}, {"RB", NAV_R}, {"+", NAV_PLUS}, {"-", NAV_MINUS}, {"H", NAV_HOME}};
    for (auto &e : k) if (t == e.s) return e.n;
    return NAV_NONE;
}

// ---- one-time "What's New" card shown after Sanctum auto-updates to a newer version
static bool g_whatsnew = false;
static std::string g_whatsnew_from;
static std::vector<std::string> g_whatsnew_lines;

static void whatsnew_check() {
    if (demo_mode() || getenv("SANCTUM_SHOT")) return;
    std::string cur = sanctum_version();
    if (cfg().last_seen_version.empty()) {
        // Fresh first-ever install: record current version silently without showing a popup
        cfg().last_seen_version = cur;
        config_save();
        return;
    }
    if (cfg().last_seen_version == cur) return;
    g_whatsnew_from = cfg().last_seen_version;
    g_whatsnew_lines.clear();
    // Try reading highlights from cached _sanctum.json for the current release
    std::string jpath = local_dir() + "releases\\_sanctum.json";
    size_t n = 0;
    if (char *d = (char *)SDL_LoadFile(jpath.c_str(), &n)) {
        Json j;
        if (json_parse(std::string(d, n), j) && j.type == Json::ARR) {
            for (const Json &rel : j.arr) {
                if (rel["tag_name"].str() != cur) continue;
                std::string body = rel["body"].str();
                size_t at = 0;
                while (at < body.size() && g_whatsnew_lines.size() < 8) {
                    size_t end = body.find('\n', at);
                    if (end == std::string::npos) end = body.size();
                    std::string l = body.substr(at, end - at);
                    at = end + 1;
                    if (!l.empty() && l.back() == '\r') l.pop_back();
                    if (l.rfind("- ", 0) == 0 || l.rfind("* ", 0) == 0) {
                        std::string clean;
                        for (size_t i = 2; i < l.size(); ++i)
                            if (l[i] != '*' && l[i] != '`') clean += l[i];
                        if (!clean.empty()) g_whatsnew_lines.push_back(clean);
                    }
                }
                break;
            }
        }
        SDL_free(d);
    }
    if (g_whatsnew_lines.empty()) {
        g_whatsnew_lines = {
            "Sanctum has updated cleanly to version " + cur + ".",
            "Check System Settings and your port pages for the latest features and fixes.",
        };
    }
    g_whatsnew = true;
}

static void handle_drop_file(const std::string &path) {
    if (path.empty() || demo_mode()) return;
    auto ends_ci = [&](const char *suf) {
        size_t sl = SDL_strlen(suf);
        return path.size() > sl && SDL_strcasecmp(path.c_str() + path.size() - sl, suf) == 0;
    };
    App &a = app();
    if (ends_ci(".zip")) {
        int pid = a.screen == SCR_MODS ? mods_active_port() : a.game;
        if (pid >= 0 && pid < (int)ports().size() && !ports()[pid].installs.empty()) {
            mods_install_zip(pid, path);
        } else {
            sound_play(SFX_ERROR);
            toast("Select an installed port before dropping a mod .zip");
        }
        return;
    }
    if (ends_ci(".png") || ends_ci(".jpg") || ends_ci(".jpeg") || ends_ci(".bmp")) {
        int pid = a.game;
        if ((a.screen == SCR_GAME || a.screen == SCR_HOME) && pid >= 0 && pid < (int)ports().size()) {
            cfg().port_covers[ports()[pid].id] = path;
            config_save();
            covers_reload();
            sound_play(SFX_TOGGLE_ON);
            toast("Updated cover art for " + std::string(ports()[pid].title));
        }
    }
}

}  // namespace sanctum

using namespace sanctum;

int main(int, char **) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SteelTemple Sanctum", SDL_GetError(), nullptr);
        return 1;
    }
    config_load();
    if (const char *t = getenv("SANCTUM_THEME")) cfg().theme = !strcmp(t, "light");
    theme_set(cfg().theme);
    sound_init();
    sound_set_volume(cfg().sound_volume);
    sound_set_theme(cfg().sound_theme);
    sound_set_enabled(cfg().sounds);
    sound_set_ambient(cfg().ambient_bgm, cfg().bgm_path);

    App &a = app();
    SDL_Window *win = SDL_CreateWindow("SteelTemple Sanctum", 1280, 720, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    SDL_Renderer *ren = win ? SDL_CreateRenderer(win, nullptr) : nullptr;
    if (!ren) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SteelTemple Sanctum", SDL_GetError(), nullptr);
        return 1;
    }
    a.window = win;
    // the window's icon (the exe's own comes from assets/sanctum.rc)
    {
        int w, h, n;
        std::string png = std::string(SDL_GetBasePath()) + "assets\\icon.png";
        if (unsigned char *px = stbi_load(png.c_str(), &w, &h, &n, 4)) {
            if (SDL_Surface *s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_ABGR8888, px, w * 4)) {
                SDL_SetWindowIcon(win, s);
                SDL_DestroySurface(s);
            }
            stbi_image_free(px);
        }
    }
    SDL_SetWindowMinimumSize(win, 640, 360);
    SDL_SetRenderVSync(ren, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    std::string font = std::string(SDL_GetBasePath()) + "assets\\fonts\\Roboto-Medium.ttf";
    ImFontConfig fc;
    fc.OversampleH = 2;
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0};   // Latin-1, dashes, quotes, "..."
    float px = 64;
    if (const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay()))
        px = SDL_clamp(42.0f * SDL_min(m->w / 1280.0f, m->h / 720.0f), 64.0f, 128.0f);
    a.font = io.Fonts->AddFontFromFileTTF(font.c_str(), px, &fc, ranges);
    if (!a.font) a.font = io.Fonts->AddFontDefault();
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);

    library_load();
    playtime_load();
    github_load_saved();
    version_probe_start();   // the saved copies' versions, from their programs
    if (!demo_mode()) scan_start();
    github_refresh();
    covers_init(ren);
    avatars_init(ren);
    album_init(ren);
    discord_rpc_init();
    self_update_start();   // Sanctum's own releases: a newer one downloads in the background
    whatsnew_check();

    // the test script
    std::vector<Nav> script;
    if (const char *s = getenv("SANCTUM_NAV")) {
        std::istringstream in(s);
        for (std::string t; in >> t;) script.push_back(nav_of(t));
    }
    const char *shot = getenv("SANCTUM_SHOT");
    if (const char *f = getenv("SANCTUM_FOCUS"))
        for (int i = 0; i < (int)ports().size(); ++i)
            if (!strcmp(ports()[i].id, f)) a.game = i;
    int shot_frame = getenv("SANCTUM_SHOT_FRAME") ? atoi(getenv("SANCTUM_SHOT_FRAME")) : 120;

    if (!cfg().display_name.empty() && !getenv("SANCTUM_WINDOWED")) apply_display(cfg().display_name);
    if (cfg().fullscreen && !getenv("SANCTUM_WINDOWED")) SDL_SetWindowFullscreen(win, true);
    SDL_ShowWindow(win);
    Uint64 t0 = SDL_GetTicksNS(), last = t0;
    float dim_alpha = 0.0f;
    bool was_dimmed = false;

    for (int frame = 0; !a.quit; ++frame) {
        // while a game runs Sanctum sleeps: no drawing, a glance at the game four times a second
        if (game_running() && (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED)) {
            SDL_Event e;
            if (SDL_WaitEventTimeout(&e, 250)) {
                if (e.type == SDL_EVENT_QUIT) a.quit = true;
                ImGui_ImplSDL3_ProcessEvent(&e);
            }
            a.now = (SDL_GetTicksNS() - t0) / 1e9;
            background_poll(win);   // downloads, the search, GitHub, and force-close chord go on while the game runs
            last = SDL_GetTicksNS();
            continue;
        }
        a.mouse_moved = a.clicked = false;
        a.typed.clear();
        a.key_back = a.key_enter = a.key_escape = false;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) a.quit = true;
            if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) {
                handle_drop_file(e.drop.data);
                continue;
            }
            if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
                set_fullscreen(!(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN));
                continue;
            }
            input_event(e);
        }
        Uint64 t = SDL_GetTicksNS();
        a.now = (t - t0) / 1e9;
        a.dt = (float)SDL_min((t - last) / 1e9, 0.1);
        last = t;

        input_frame();
        if (frame >= 30 && (frame - 30) % 8 == 0 && (size_t)((frame - 30) / 8) < script.size())
            a.nav = script[(frame - 30) / 8];

        // OLED idle dim: wake smoothly on first input without firing an unintended action
        bool should_dim = !demo_mode() && !shot && !game_running() && cfg().idle_dim_min > 0 &&
                          (a.now - input_last_activity()) >= cfg().idle_dim_min * 60.0;
        if (was_dimmed && !should_dim) {
            a.nav = NAV_NONE;
            a.clicked = false;
        }
        was_dimmed = should_dim;
        dim_alpha = ease(dim_alpha, should_dim ? 1.0f : 0.0f, should_dim ? 2.0f : 12.0f);

        // Hold screen input while the one-time What's New overlay is open
        Nav wn_nav = NAV_NONE;
        bool wn_click = false;
        if (g_whatsnew) {
            wn_nav = a.nav, wn_click = a.clicked;
            a.nav = NAV_NONE, a.clicked = false;
        }

        if (a.nav == NAV_HOME && a.text_entry) { a.text_entry = false; SDL_StopTextInput(win); }
        if (a.nav == NAV_HOME) {
            if (a.screen != SCR_HOME) {
                sound_play(SFX_HOME);
                a.trans_t = a.now;
                a.trans_dir = -1;
            }
            a.back.clear();
            a.screen = SCR_HOME;
            a.nav = NAV_NONE;
        }
        background_poll(win);
        covers_poll(a.now);
        album_poll();
        if (self_update_poll()) {
            sound_play(SFX_NOTIFY);
            toast("Sanctum " + self_update_tag() + " is ready: it installs when you close Sanctum (or System Settings > About)", 6);
        }

        // fit the canvas into the window
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImVec2 ds = io.DisplaySize;
        a.scale = SDL_min(ds.x / 1280.0f, ds.y / 720.0f);
        a.ox = (ds.x - 1280 * a.scale) / 2, a.oy = (ds.y - 720 * a.scale) / 2;
        float trans_fade = 0.0f;
        if (!shot) {
            float tp = (float)((a.now - a.trans_t) / 0.14);
            if (tp >= 0.0f && tp < 1.0f) {
                float u = 1.0f - tp;
                float rem = u * u * u;
                a.oy += rem * (a.trans_dir >= 0 ? 12.0f : -10.0f) * a.scale;
                trans_fade = rem;
            }
        }

        bool starting = launch_starting();
        Nav start_nav = a.nav;
        if (starting) a.nav = NAV_NONE, a.clicked = false;

        switch (a.screen) {
        case SCR_HOME: home_frame(); break;
        case SCR_GAME: game_frame(); break;
        case SCR_SETTINGS: settings_frame(); break;
        case SCR_FOLDER: folder_frame(); break;
        case SCR_CONTROLLERS: controllers_frame(); break;
        case SCR_SOON: soon_frame(); break;
        case SCR_PORTS: ports_frame(); break;
        case SCR_DOWNLOADS: downloads_frame(); break;
        case SCR_NEWS: news_frame(); break;
        case SCR_PORT_SETTINGS: port_settings_frame(); break;
        case SCR_PROFILE: profile_frame(); break;
        case SCR_MODS: mods_frame(); break;
        case SCR_ALBUM: album_frame(); break;
        }
        if (trans_fade > 0.005f)
            dl()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, with_alpha(pal().bg, (int)(trans_fade * 165)));
        if (starting) {
            const Palette &p = pal();
            int pi = running_port();
            if (start_nav == NAV_B) { sound_play(SFX_BACK); close_game(); toast("Start cancelled"); }
            if (pi >= 0) {
                const Port &port = ports()[pi];
                float in = SDL_min(1.0f, (float)running_seconds() * 4);
                dl()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, with_alpha(mix(p.bg, port.colours.deep, 0.5f), (int)(255 * in)));
                tile(port, 640 - 130, 150, 260, in, false);
                text(640, 438, 28, with_alpha(p.text, (int)(255 * in)), fit(28, port.title, 1000).c_str(), 0.5f);
                float tt = (float)a.now * 5;
                for (int i = 0; i < 3; ++i) {
                    float k = 0.5f + 0.5f * SDL_sinf(tt - i * 0.9f);
                    circle(616 + i * 24, 506, 5 + 2 * k, with_alpha(port.colours.accent, (int)((90 + 165 * k) * in)));
                }
                text(640, 532, 16, with_alpha(p.soft, (int)(255 * in)), "Starting the game...", 0.5f);
                hint_bar({{NAV_B, "Cancel"}});
            }
        }

        // One-time What's New card after auto-update
        if (g_whatsnew) {
            const Palette &p = pal();
            float mw = 700, mh = 180 + (float)g_whatsnew_lines.size() * 42;
            if (mh > 560) mh = 560;
            float mx = 640 - mw / 2, my = 360 - mh / 2;
            rect(0, 0, 1280, 720, IM_COL32(0, 0, 0, 165));
            rect(mx, my, mw, mh, p.bg2, 22);
            rect(mx, my, mw, 5, p.accent, 2);
            text(mx + 32, my + 24, 14, p.accent_text, ("UPDATED FROM " + g_whatsnew_from).c_str());
            text(mx + 32, my + 46, 26, p.text, ("Welcome to Sanctum " + std::string(sanctum_version())).c_str());
            float ly = my + 96;
            for (const std::string &line : g_whatsnew_lines) {
                if (ly > my + mh - 92) break;
                circle(mx + 42, ly + 10, 3, p.accent);
                ly += text_wrap(mx + 56, ly, 15, p.soft, line.c_str(), mw - 92) + 10;
            }
            float bw = 180, bx = mx + mw - bw - 28, by = my + mh - 66;
            rect(bx, by, bw, 44, p.accent, 22);
            text(bx + bw / 2, by + 11, 17, IM_COL32_WHITE, "Continue", 0.5f);
            focus_ring(bx, by, bw, 44, 22, 4);
            if (wn_nav == NAV_A || wn_nav == NAV_B || wn_click) {
                g_whatsnew = false;
                cfg().last_seen_version = sanctum_version();
                config_save();
                sound_play(SFX_OK);
            }
        }

        // OLED Idle Dim overlay (gently drifting low-brightness clock + handheld battery)
        if (dim_alpha > 0.01f) {
            int bg_a = (int)(dim_alpha * 228);
            int fg_a = (int)(dim_alpha * 155);
            dl()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, bg_a));
            time_t now_t = time(nullptr);
            struct tm lt{};
#ifdef _WIN32
            localtime_s(&lt, &now_t);
#else
            localtime_r(&lt, &now_t);
#endif
            char clk[24];
            if (cfg().clock_12h) {
                int hr = lt.tm_hour % 12;
                if (hr == 0) hr = 12;
                SDL_snprintf(clk, sizeof clk, "%d:%02d %s", hr, lt.tm_min, lt.tm_hour >= 12 ? "PM" : "AM");
            } else {
                strftime(clk, sizeof clk, "%H:%M", &lt);
            }
            // Slow figure-8 drift so OLED pixels never burn in
            float ox = 640 + std::sin((float)(a.now * 0.05)) * 180.0f;
            float oy = 320 + std::sin((float)(a.now * 0.08)) * 80.0f;
            text(ox, oy, 64, IM_COL32(220, 224, 232, fg_a), clk, 0.5f);
            int bp = -1;
            bool bc = false;
            if (system_battery_info(&bp, &bc) && bp >= 0) {
                char bs[40];
                SDL_snprintf(bs, sizeof bs, bc ? "Battery %d%% (Charging)" : "Battery %d%%", bp);
                text(ox, oy + 82, 16, IM_COL32(170, 178, 190, fg_a), bs, 0.5f);
            }
            text(ox, oy + 112, 14, IM_COL32(130, 136, 148, fg_a), "Press any button to wake", 0.5f);
        }

        toast_draw();

        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        bool idle_shot = getenv("SANCTUM_SHOT_IDLE") != nullptr;
        if (shot && (idle_shot ? frame >= shot_frame && !install_busy() : frame == shot_frame)) {
            if (SDL_Surface *s = SDL_RenderReadPixels(ren, nullptr)) {
                SDL_SaveBMP(s, shot);
                SDL_DestroySurface(s);
            }
            a.quit = true;
        }
        SDL_RenderPresent(ren);
    }

    // a downloaded update goes in now, for the next start (a restart from About has put it in already)
    if (self_update_state() == UPD_READY) self_update_apply(false);
    discord_rpc_shutdown();
    sound_shutdown();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
