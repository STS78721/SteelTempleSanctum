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

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "app.h"
#include "catalog.h"
#include "config.h"
#include "covers.h"
#include "draw.h"
#include "github.h"
#include "install.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "input.h"
#include "stb_image.h"   // implemented in covers.cpp
#include "launch.h"
#include "theme.h"

namespace sanctum {

static App g_app;
App &app() { return g_app; }

void go(Screen s) {
    if (s == g_app.screen) return;
    g_app.back.push_back(g_app.screen);
    g_app.screen = s;
    g_app.nav = NAV_NONE;
    g_app.clicked = false;
}

void go_back() {
    g_app.screen = g_app.back.empty() ? SCR_HOME : g_app.back.back();
    if (!g_app.back.empty()) g_app.back.pop_back();
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

bool hover(float x, float y, float w, float h) {
    ImVec2 m = g_app.mouse;
    return m.x >= x && m.x < x + w && m.y >= y && m.y < y + h;
}

// what the workers finished, taken in every frame, and while a game runs too
static void background_poll(SDL_Window *win) {
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

}  // namespace sanctum

using namespace sanctum;

int main(int, char **) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "SteelTemple Sanctum", SDL_GetError(), nullptr);
        return 1;
    }
    config_load();
    if (const char *t = getenv("SANCTUM_THEME")) cfg().theme = !strcmp(t, "light");
    theme_set(cfg().theme);

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
    // the font is rasterised for this screen: the largest text (about 40 canvas points) stays sharp on a 4K
    // fullscreen canvas (scale 3) as on 1080p (1.5); drawn smaller, it is scaled down, which is always clean
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
    scan_start();
    github_refresh();
    covers_init(ren);

    // the test script
    std::vector<Nav> script;
    if (const char *s = getenv("SANCTUM_NAV")) {
        std::istringstream in(s);
        for (std::string t; in >> t;) script.push_back(nav_of(t));
    }
    const char *shot = getenv("SANCTUM_SHOT");
    // tests name the port to start on (never a tile count: the row's order follows what was played last)
    if (const char *f = getenv("SANCTUM_FOCUS"))
        for (int i = 0; i < (int)ports().size(); ++i)
            if (!strcmp(ports()[i].id, f)) a.game = i;
    int shot_frame = getenv("SANCTUM_SHOT_FRAME") ? atoi(getenv("SANCTUM_SHOT_FRAME")) : 120;

    // fullscreen unless the player turned it off (tests run windowed: SANCTUM_WINDOWED=1)
    if (cfg().fullscreen && !getenv("SANCTUM_WINDOWED")) SDL_SetWindowFullscreen(win, true);
    SDL_ShowWindow(win);
    Uint64 t0 = SDL_GetTicksNS(), last = t0;
    for (int frame = 0; !a.quit; ++frame) {
        // while a game runs Sanctum sleeps: no drawing, a glance at the game four times a second
        if (game_running() && (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED)) {
            SDL_Event e;
            if (SDL_WaitEventTimeout(&e, 250)) {
                if (e.type == SDL_EVENT_QUIT) a.quit = true;
                ImGui_ImplSDL3_ProcessEvent(&e);
                input_event(e);
            }
            background_poll(win);   // downloads, the search, GitHub go on while the game is in front
            last = SDL_GetTicksNS();
            continue;
        }
        a.mouse_moved = a.clicked = false;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) a.quit = true;
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
        if (a.nav == NAV_HOME) { a.back.clear(); a.screen = SCR_HOME; a.nav = NAV_NONE; }
        background_poll(win);
        covers_poll(a.now);

        // fit the canvas into the window
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImVec2 ds = io.DisplaySize;
        a.scale = SDL_min(ds.x / 1280.0f, ds.y / 720.0f);
        a.ox = (ds.x - 1280 * a.scale) / 2, a.oy = (ds.y - 720 * a.scale) / 2;

        // a game starting: the screens wait under the Starting screen (B cancels a start that hangs)
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
        }
        if (starting) {
            const Palette &p = pal();
            int pi = running_port();
            if (start_nav == NAV_B) { close_game(); toast("Start cancelled"); }
            if (pi >= 0) {
                const Port &port = ports()[pi];
                float in = SDL_min(1.0f, (float)running_seconds() * 4);   // fades in over a quarter second
                dl()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, with_alpha(mix(p.bg, port.colours.deep, 0.5f), (int)(255 * in)));
                tile(port, 640 - 130, 150, 260, in, false);
                text(640, 438, 28, with_alpha(p.text, (int)(255 * in)), fit(28, port.title, 1000).c_str(), 0.5f);
                float t = (float)a.now * 5;
                for (int i = 0; i < 3; ++i) {   // three dots, the console's "loading"
                    float k = 0.5f + 0.5f * SDL_sinf(t - i * 0.9f);
                    circle(616 + i * 24, 506, 5 + 2 * k, with_alpha(port.colours.accent, (int)((90 + 165 * k) * in)));
                }
                text(640, 532, 16, with_alpha(p.soft, (int)(255 * in)), "Starting the game...", 0.5f);
                hint_bar({{NAV_B, "Cancel"}});
            }
        }
        toast_draw();

        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        // SANCTUM_SHOT_IDLE=1: the first frame from SANCTUM_SHOT_FRAME on with the downloads queue finished
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

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
