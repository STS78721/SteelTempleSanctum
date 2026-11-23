#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>

#include "avatars.h"
#include "catalog.h"
#include "config.h"
#include "covers.h"
#include "draw.h"
#include "github.h"
#include "hud.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "input.h"
#include "install.h"
#include "launch.h"
#include "perf.h"
#include "platform.h"
#include "selfupdate.h"
#include "sound.h"
#include "theme.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace sanctum {

static bool g_open = false;
static bool g_from_game = false;
static float g_anim = 0.0f;
static int g_sel = 0;
static double g_close_armed = -10.0;
static double g_force_armed = -10.0;
static double g_quit_armed = -10.0;

// Cached Windows master volume & audio playback endpoints (refreshed on open and every 0.5-1.5 s while open)
static bool g_vol_ok = false;
static int g_vol_pct = 50;
static bool g_vol_muted = false;
static double g_vol_checked = -10.0;
static std::vector<AudioOutput> g_audio_outs;
static int g_audio_idx = -1;
static double g_audio_checked = -10.0;

// ---- Dedicated transparent WS_EX_TOPMOST in-game overlay window (zero main-window switch flash)
static SDL_Window *g_ov_win = nullptr;
static SDL_Renderer *g_ov_ren = nullptr;
static ImGuiContext *g_ov_ctx = nullptr;
static ImFont *g_ov_font = nullptr;
static bool g_ov_interactive = false;   // true = transparent Quick Overlay drawer owns input over paused game
static bool g_ov_showing = false;       // true = g_ov_win is currently visible on screen
static SDL_Texture *g_ov_cover = nullptr;
static std::string g_ov_cover_id;

// Click-through in-game Switch-style screenshot notification toast (rendered in g_ov_win without stealing focus)
static double g_cap_toast_start = -10.0;
static double g_cap_toast_until = -10.0;
static std::string g_cap_toast_title, g_cap_toast_sub;
static ImU32 g_cap_toast_acc = IM_COL32(72, 168, 255, 255);

#ifdef _WIN32
static HWND ov_hwnd() {
    if (!g_ov_win) return nullptr;
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(g_ov_win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
}

static void dwm_flush_safe() {
    using DwmFlushFn = HRESULT (WINAPI *)();
    static DwmFlushFn fn = []() -> DwmFlushFn {
        HMODULE m = LoadLibraryW(L"dwmapi.dll");
        return m ? (DwmFlushFn)(void *)GetProcAddress(m, "DwmFlush") : nullptr;
    }();
    if (fn) fn();
}

static void ov_set_clickthrough(bool clickthrough) {
    HWND h = ov_hwnd();
    if (!h) return;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    ex |= WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    if (clickthrough) ex |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    else ex &= ~(LONG_PTR)(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    SetWindowLongPtrW(h, GWL_EXSTYLE, ex);
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
#endif

static bool ensure_overlay_window() {
#ifdef _WIN32
    if (g_ov_win && g_ov_ren && g_ov_ctx) return true;
    if (demo_mode() || getenv("SANCTUM_SHOT")) return false;

    int mx = 0, my = 0, mw = 1280, mh = 720;
    monitor_rect_for_hwnd(launch_game_hwnd(), mx, my, mw, mh);

    SDL_WindowFlags fl = SDL_WINDOW_BORDERLESS | SDL_WINDOW_TRANSPARENT | SDL_WINDOW_ALWAYS_ON_TOP |
                         SDL_WINDOW_UTILITY | SDL_WINDOW_HIDDEN;
    g_ov_win = SDL_CreateWindow("Sanctum Overlay", mw, mh, fl);
    if (!g_ov_win) {
        sanctum_log("overlay: transparent window creation failed (%s)", SDL_GetError());
        return false;
    }
    SDL_SetWindowPosition(g_ov_win, mx, my);
    g_ov_ren = SDL_CreateRenderer(g_ov_win, "direct3d11");
    if (!g_ov_ren) g_ov_ren = SDL_CreateRenderer(g_ov_win, nullptr);
    if (!g_ov_ren) {
        sanctum_log("overlay: transparent renderer creation failed (%s)", SDL_GetError());
        SDL_DestroyWindow(g_ov_win);
        g_ov_win = nullptr;
        return false;
    }
    SDL_SetRenderVSync(g_ov_ren, 1);
    SDL_SetRenderDrawBlendMode(g_ov_ren, SDL_BLENDMODE_BLEND);

    ImGuiContext *prev = ImGui::GetCurrentContext();
    g_ov_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ov_ctx);
    ImGuiIO &oio = ImGui::GetIO();
    oio.IniFilename = nullptr;
    oio.LogFilename = nullptr;
    oio.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    std::string font = std::string(SDL_GetBasePath()) + "assets\\fonts\\Roboto-Medium.ttf";
    ImFontConfig fc;
    fc.OversampleH = 2;
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0};
    float px = SDL_clamp(42.0f * SDL_min(mw / 1280.0f, mh / 720.0f), 64.0f, 128.0f);
    g_ov_font = oio.Fonts->AddFontFromFileTTF(font.c_str(), px, &fc, ranges);
    if (!g_ov_font) g_ov_font = oio.Fonts->AddFontDefault();
    ImGui_ImplSDL3_InitForSDLRenderer(g_ov_win, g_ov_ren);
    ImGui_ImplSDLRenderer3_Init(g_ov_ren);
    ImGui::SetCurrentContext(prev);
    sanctum_log("overlay: initialized transparent WS_EX_TOPMOST window (%dx%d at %d,%d)", mw, mh, mx, my);
    return true;
#else
    return false;
#endif
}

static void ov_position_on_game_monitor() {
#ifdef _WIN32
    if (!g_ov_win) return;
    int mx = 0, my = 0, mw = 1280, mh = 720;
    if (!monitor_rect_for_hwnd(launch_game_hwnd(), mx, my, mw, mh)) return;
    SDL_SetWindowPosition(g_ov_win, mx, my);
    SDL_SetWindowSize(g_ov_win, mw, mh);
    if (HWND h = ov_hwnd()) {
        SetWindowPos(h, HWND_TOPMOST, mx, my, mw, mh,
                     SWP_NOACTIVATE | (g_ov_showing ? SWP_SHOWWINDOW : 0));
    }
    App &a = app();
    a.scale = SDL_min(mw / 1280.0f, mh / 720.0f);
    a.ox = (mw - 1280.0f * a.scale) / 2.0f;
    a.oy = (mh - 720.0f * a.scale) / 2.0f;
#endif
}

static void ov_win_hide() {
    g_ov_interactive = false;
    g_ov_showing = false;
#ifdef _WIN32
    if (HWND h = ov_hwnd()) ShowWindow(h, SW_HIDE);
#endif
    if (g_ov_win) SDL_HideWindow(g_ov_win);
}

static SDL_Texture *ov_port_cover(const char *port_id) {
    if (!g_ov_ren || !port_id || !port_id[0]) return nullptr;
    if (g_ov_cover_id == port_id) return g_ov_cover;
    if (g_ov_cover) { SDL_DestroyTexture(g_ov_cover); g_ov_cover = nullptr; }
    g_ov_cover_id = port_id;
    std::string path = cover_path_of(port_id);
    if (path.empty()) return nullptr;
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
    if (!load_image_rgba(path, 256, w, h, rgba) || w <= 0 || h <= 0) return nullptr;
    SDL_Texture *t = SDL_CreateTexture(g_ov_ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return nullptr;
    SDL_UpdateTexture(t, nullptr, rgba.data(), w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
    g_ov_cover = t;
    return g_ov_cover;
}

static void draw_ov_tile(const Port &gp, float x, float y, float sz, float alpha) {
    int a8 = (int)(255 * SDL_clamp(alpha, 0.0f, 1.0f));
    if (SDL_Texture *t = ov_port_cover(gp.id)) {
        dl()->AddImageRounded((ImTextureID)t, P(x, y), P(x + sz, y + sz), ImVec2(0, 0), ImVec2(1, 1),
                              IM_COL32(255, 255, 255, a8), S(10));
        frame(x, y, sz, sz, with_alpha(IM_COL32_WHITE, a8 / 5), 10, 1.0f);
    } else {
        rect(x, y, sz, sz, with_alpha(gp.colours.deep, a8), 10);
        rect(x, y, sz, 4, with_alpha(gp.colours.accent, a8), 2);
        text(x + sz * 0.5f, y + sz * 0.5f - 11, 20, with_alpha(IM_COL32_WHITE, a8), gp.mono ? gp.mono : "ST", 0.5f);
    }
}

void overlay_shutdown() {
    ov_win_hide();
    if (g_ov_cover) { SDL_DestroyTexture(g_ov_cover); g_ov_cover = nullptr; }
    g_ov_cover_id.clear();
    if (g_ov_ctx) {
        ImGuiContext *prev = ImGui::GetCurrentContext();
        ImGuiContext *killed = g_ov_ctx;
        ImGui::SetCurrentContext(killed);
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(killed);
        g_ov_ctx = nullptr;
        g_ov_font = nullptr;
        if (prev && prev != killed) ImGui::SetCurrentContext(prev);
    }
    if (g_ov_ren) { SDL_DestroyRenderer(g_ov_ren); g_ov_ren = nullptr; }
    if (g_ov_win) { SDL_DestroyWindow(g_ov_win); g_ov_win = nullptr; }
}

static bool hud_should_show() {
    if (!game_running() || launch_starting() || game_suspended() || cfg().perf_hud <= 0 || demo_mode()) return false;
    return app().window && (SDL_GetWindowFlags(app().window) & SDL_WINDOW_MINIMIZED) != 0;
}

bool overlay_win_visible() {
    if (!g_ov_win || !g_ov_showing) return false;
    return g_ov_interactive || (app().now < g_cap_toast_until) || hud_should_show();
}

bool overlay_win_is_interactive() { return g_ov_win != nullptr && g_ov_showing && g_ov_interactive; }
bool overlay_win_has_toast() { return app().now < g_cap_toast_until; }

void overlay_sync_hud() {
    if (g_ov_interactive) return;
    if (hud_should_show()) {
        hud_sample(launch_game_proc(), launch_game_pid(), launch_game_hwnd(), launch_game_log_path(),
                   app().now, cfg().perf_hud >= 2);
#ifdef _WIN32
        if (!g_ov_showing && ensure_overlay_window()) {
            ov_position_on_game_monitor();
            ov_set_clickthrough(true);
            g_ov_interactive = false;
            g_ov_showing = true;
            if (HWND h = ov_hwnd()) {
                int mx = 0, my = 0, mw = 1280, mh = 720;
                monitor_rect_for_hwnd(launch_game_hwnd(), mx, my, mw, mh);
                SetWindowPos(h, HWND_TOPMOST, mx, my, mw, mh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                ShowWindow(h, SW_SHOWNOACTIVATE);
            }
        }
#endif
    } else if (g_ov_showing && app().now >= g_cap_toast_until) {
        ov_win_hide();
    }
}

SDL_WindowID overlay_win_id() { return g_ov_win ? SDL_GetWindowID(g_ov_win) : 0; }

void overlay_win_event(const SDL_Event &e) {
    if (!g_ov_ctx) return;
    ImGuiContext *prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(g_ov_ctx);
    ImGui_ImplSDL3_ProcessEvent(&e);
    ImGui::SetCurrentContext(prev);
}

static void refresh_volume(double now, bool force = false) {
    if (!force && now - g_vol_checked < 0.5) return;
    g_vol_checked = now;
    if (demo_mode()) { g_vol_ok = true; g_vol_pct = 70; g_vol_muted = false; return; }
    g_vol_ok = system_volume_get(&g_vol_pct, &g_vol_muted);
}

static void refresh_audio_outputs(double now, bool force = false) {
    if (!force && now - g_audio_checked < 1.5) return;
    g_audio_checked = now;
    if (demo_mode()) {
        g_audio_outs = {{"demo-tv", "Living Room TV (HDMI)", true}, {"demo-hp", "Headphones", false}};
        g_audio_idx = 0;
        return;
    }
    g_audio_outs = system_audio_outputs();
    g_audio_idx = -1;
    for (int i = 0; i < (int)g_audio_outs.size(); ++i)
        if (g_audio_outs[i].active) { g_audio_idx = i; break; }
    if (g_audio_idx < 0 && !g_audio_outs.empty()) g_audio_idx = 0;
}

static void show_ingame_capture_toast(const std::string &title, const std::string &sub, ImU32 acc) {
    App &a = app();
    g_cap_toast_title = title;
    g_cap_toast_sub = sub;
    g_cap_toast_acc = acc;
    g_cap_toast_start = a.now;
    g_cap_toast_until = a.now + 2.5;
    if (g_ov_interactive) return;   // interactive overlay renders its own toast_draw()
#ifdef _WIN32
    if (!ensure_overlay_window()) return;
    ov_position_on_game_monitor();
    ov_set_clickthrough(true);
    g_ov_interactive = false;
    g_ov_showing = true;
    if (HWND h = ov_hwnd()) {
        int mx = 0, my = 0, mw = 1280, mh = 720;
        monitor_rect_for_hwnd(launch_game_hwnd(), mx, my, mw, mh);
        SetWindowPos(h, HWND_TOPMOST, mx, my, mw, mh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(h, SW_SHOWNOACTIVATE);
    }
#endif
}

void overlay_capture_screenshot(SDL_Window *main_win) {
    if (demo_mode()) {
        sound_play(SFX_SHUTTER);
        toast("Demo mode: screenshot preview");
        return;
    }
    bool running = game_running() && !launch_starting();
    int rp = running ? running_port() : -1;
    void *target_hwnd = nullptr;
    if (running) {
        target_hwnd = launch_game_hwnd();
    } else if (main_win) {
        target_hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(main_win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    }

    // If our transparent overlay is currently visible over the game, briefly hide it so the shot is 100% unobstructed
    bool hid_ov = false;
#ifdef _WIN32
    if (running && g_ov_showing && ov_hwnd()) {
        ShowWindow(ov_hwnd(), SW_HIDE);
        dwm_flush_safe();
        SDL_Delay(35);
        hid_ov = true;
    }
#endif

    // Never send synchronous PrintWindow(WM_PRINT) to a paused/suspended game process (BitBlt reads DWM directly with zero IPC)
    std::string saved = album_take_screenshot(target_hwnd, rp, !(running && game_frozen()));

#ifdef _WIN32
    if (hid_ov && ov_hwnd()) {
        ShowWindow(ov_hwnd(), g_ov_interactive ? SW_SHOW : SW_SHOWNOACTIVATE);
        if (g_ov_interactive) launch_wake_hwnd(ov_hwnd(), true);
    }
#endif

    if (saved.empty()) {
        if (running && !g_ov_interactive) sound_play_oneshot(SFX_ERROR);
        else sound_play(SFX_ERROR);
        toast("Could not capture screenshot");
        return;
    }

    std::string label = (rp >= 0 && rp < (int)ports().size()) ? ports()[rp].name : "Sanctum";
    ImU32 acc = (rp >= 0 && rp < (int)ports().size()) ? ports()[rp].colours.accent : pal().accent;
    if (running && !g_ov_interactive) {
        sound_play_oneshot(SFX_SHUTTER);
        show_ingame_capture_toast("Capture taken", "Saved to Album  ·  " + label, acc);
    } else {
        sound_play(SFX_SHUTTER);
        toast("Capture taken  ·  Saved to Album (" + label + ")", 2.8);
    }
}

bool overlay_is_open() { return g_open; }

void overlay_close() {
    g_open = false;
    g_from_game = false;
    g_close_armed = g_force_armed = g_quit_armed = -10.0;
    if (g_ov_interactive) ov_win_hide();
}

void overlay_on_game_exit() {
    g_cap_toast_until = -10.0;
    if (g_ov_showing) ov_win_hide();
    if (g_from_game) overlay_close();
    else g_close_armed = g_force_armed = -10.0;
}

void overlay_toggle(SDL_Window *main_win) {
    App &a = app();
    if (g_open) {
        bool should_resume = g_from_game && game_running();
        overlay_close();
        sound_play(SFX_BACK);
        if (should_resume) {
            input_suppress_guide(a.now);
            resume_game(main_win);
        }
        return;
    }
    if (game_running() && !game_suspended()) launch_overlay_pause(true);
    g_open = true;
    g_from_game = false;
    g_sel = 0;
    g_close_armed = g_force_armed = g_quit_armed = -10.0;
    refresh_volume(a.now, true);
    refresh_audio_outputs(a.now, true);
    sound_play(SFX_HOME);
}

void overlay_open_from_game(SDL_Window *main_win) {
    App &a = app();
    // Pause the game immediately before showing the overlay so navigation never bleeds controller input into the game
    launch_overlay_pause(true);
    g_open = true;
    g_from_game = true;
    g_sel = 0;
    g_anim = 0.0f;
    g_close_armed = g_force_armed = g_quit_armed = -10.0;
    refresh_volume(a.now, true);
    refresh_audio_outputs(a.now, true);

    bool use_transparent = false;
#ifdef _WIN32
    if (ensure_overlay_window()) {
        ov_position_on_game_monitor();
        ov_set_clickthrough(false);
        g_ov_interactive = true;
        g_ov_showing = true;
        perf_sanctum_foreground(true);
        sound_set_paused(false);
        SDL_ShowWindow(g_ov_win);
        if (HWND h = ov_hwnd()) {
            int mx = 0, my = 0, mw = 1280, mh = 720;
            monitor_rect_for_hwnd(launch_game_hwnd(), mx, my, mw, mh);
            SetWindowPos(h, HWND_TOPMOST, mx, my, mw, mh, SWP_SHOWWINDOW);
            launch_wake_hwnd(h, true);
        }
        SDL_RaiseWindow(g_ov_win);
        if (cfg().hide_cursor && SDL_CursorVisible()) SDL_HideCursor();
        use_transparent = true;
    }
#endif
    if (!use_transparent) {
        launch_wake_sanctum(main_win);
    }
    sound_play(SFX_HOME);
    sanctum_log("overlay: opened from running game via Guide button (%s)",
                use_transparent ? "transparent topmost" : "main window fallback");
}

enum OvItemId {
    O_RESUME,
    O_SUSPEND,
    O_SCREENSHOT,
    O_HOME,
    O_VOLUME,
    O_AUDIO_OUT,
    O_PERF_HUD,
    O_PERF,
    O_SOUNDS,
    O_THEME,
    O_FULLSCREEN,
    O_BROWSE,
    O_DOWNLOADS,
    O_CONTROLLERS,
    O_ALBUM,
    O_REFRESH,
    O_SETTINGS,
    O_CLOSE_GAME,
    O_FORCE_QUIT,
    O_QUIT,
};

struct OvItem {
    OvItemId id;
    Icon icon;
    std::string label;
    std::string value;
    bool primary = false;
    bool danger = false;
    bool slider = false;
};

static std::string short_audio_name(const std::string &full) {
    // Trim overly verbose Windows suffixes like " (High Definition Audio Device)" if name is very long
    if (full.size() <= 24) return full;
    size_t lp = full.find(" (");
    if (lp != std::string::npos && lp >= 4 && lp <= 24) return full.substr(0, lp);
    return full;
}

static void overlay_frame_impl(SDL_Window *main_win, Nav nav, bool clicked, bool transparent_mode) {
    App &a = app();
    g_anim = ease(g_anim, g_open ? 1.0f : 0.0f, 18.0f);
    if (!g_open && g_anim < 0.01f) { g_anim = 0.0f; return; }
    if (g_open) {
        refresh_volume(a.now);
        refresh_audio_outputs(a.now);
    }

    const Palette &p = pal();
    bool running = game_running();
    int rp = running_port();
    const Port *active_port = (running && rp >= 0 && rp < (int)ports().size()) ? &ports()[rp]
                            : (a.game >= 0 && a.game < (int)ports().size() && a.screen == SCR_GAME) ? &ports()[a.game]
                            : nullptr;
    ImU32 acc = active_port ? active_port->colours.accent : p.accent;
    ImU32 deep = active_port ? active_port->colours.deep : p.bg2;

    std::vector<OvItem> items;
    int active_dls = 0;
    for (const auto &j : install_jobs()) if (j.phase < PH_DONE) ++active_dls;
    int pad_pct = -1;
    bool pad_chg = false;
    bool has_pad_bat = input_pad_battery(&pad_pct, &pad_chg);
    std::string pad_val = has_pad_bat && pad_pct >= 0
                        ? (std::to_string(pad_pct) + "%" + (pad_chg ? " chg" : ""))
                        : (a.pad == PAD_KEYBOARD ? "Keyboard" : "Connected");

    std::string cur_audio;
    if (g_audio_idx >= 0 && g_audio_idx < (int)g_audio_outs.size()) {
        std::string nm = short_audio_name(g_audio_outs[g_audio_idx].name);
        cur_audio = g_audio_outs.size() > 1 ? ("< " + fit(13, nm, 155) + " >") : fit(13, nm, 175);
    }

    static const char *const kHudNames[] = {"Off", "Compact (FPS)", "Detailed"};
    std::string hud_val = kHudNames[SDL_clamp(cfg().perf_hud, 0, 2)];

    if (running) {
        bool susp = game_suspended();
        items.push_back({O_RESUME, IC_PLAY, "Resume game", susp ? "Quick Resume" : "Return", true, false, false});
        if (!susp)
            items.push_back({O_SUSPEND, IC_PAUSE, "Suspend game", "Pause CPU & GPU", false, false, false});
        else if (a.screen != SCR_HOME)
            items.push_back({O_HOME, IC_LIBRARY, "Go to Home", "Keep suspended", false, false, false});
        items.push_back({O_SCREENSHOT, IC_ALBUM, "Take screenshot", "Save to Album", false, false, false});
        if (g_vol_ok)
            items.push_back({O_VOLUME, IC_SOUND, "System volume", g_vol_muted ? "Muted" : (std::to_string(g_vol_pct) + "%"), false, false, true});
        if (!g_audio_outs.empty())
            items.push_back({O_AUDIO_OUT, IC_SOUND, "Sound output", cur_audio, false, false, false});
        items.push_back({O_PERF_HUD, IC_UP, "Performance HUD", hud_val, false, false, false});
        if (!susp)
            items.push_back({O_BROWSE, IC_LIBRARY, "Browse Sanctum", "Open launcher", false, false, false});
        items.push_back({O_ALBUM, IC_ALBUM, "Screenshot album", "Open", false, false, false});
        bool c_armed = a.now - g_close_armed < 3.5;
        items.push_back({O_CLOSE_GAME, IC_CLOSE, "Close game", c_armed ? "Press again" : "Polite close", false, false, false});
        bool f_armed = a.now - g_force_armed < 3.5;
        items.push_back({O_FORCE_QUIT, IC_POWER, "Force quit game", f_armed ? "Press again to kill" : "Frozen game", false, true, false});
    } else {
        if (a.screen != SCR_HOME)
            items.push_back({O_HOME, IC_LIBRARY, "Go to Home", "Library", true, false, false});
        if (g_vol_ok)
            items.push_back({O_VOLUME, IC_SOUND, "System volume", g_vol_muted ? "Muted" : (std::to_string(g_vol_pct) + "%"), false, false, true});
        if (!g_audio_outs.empty())
            items.push_back({O_AUDIO_OUT, IC_SOUND, "Sound output", cur_audio, false, false, false});
        items.push_back({O_PERF, IC_UP, "Performance mode", perf_status_short(), false, false, false});
        items.push_back({O_PERF_HUD, IC_DISPLAY, "Performance HUD", hud_val, false, false, false});
        items.push_back({O_THEME, IC_PALETTE, "Theme", cfg().theme ? "Light" : "Dark", false, false, false});
        items.push_back({O_FULLSCREEN, IC_DISPLAY, "Display mode", cfg().fullscreen ? "Fullscreen" : "Windowed", false, false, false});
        items.push_back({O_DOWNLOADS, IC_DOWNLOAD, "Downloads", active_dls ? (std::to_string(active_dls) + " active") : "Queue", false, false, false});
        items.push_back({O_CONTROLLERS, IC_PAD, "Controllers", pad_val, false, false, false});
        items.push_back({O_REFRESH, IC_SEARCH, "Check for updates", github_busy() ? "Checking..." : "GitHub & catalog", false, false, false});
        items.push_back({O_SETTINGS, IC_GEAR, "System Settings", "All options", false, false, false});
        bool q_armed = a.now - g_quit_armed < 3.5;
        items.push_back({O_QUIT, IC_POWER, "Exit Sanctum", q_armed ? "Press again to exit" : "Close launcher", false, true, false});
    }
    if (g_sel >= (int)items.size()) g_sel = std::max(0, (int)items.size() - 1);

    auto switch_to_full_sanctum = [&]() {
        overlay_close();
        if (transparent_mode && main_win) launch_wake_sanctum(main_win);
    };

    auto adjust_volume = [&](int delta) {
        if (!g_vol_ok) return;
        int next = SDL_clamp(g_vol_pct + delta, 0, 100);
        if (next == g_vol_pct && !g_vol_muted) { sound_play(SFX_EDGE); return; }
        g_vol_pct = next;
        g_vol_muted = false;
        if (!demo_mode()) system_volume_set(g_vol_pct, false);
        sound_play(SFX_KEY);
    };

    auto cycle_audio_output = [&](int dir) {
        if (g_audio_outs.size() <= 1) { sound_play(SFX_EDGE); return; }
        int n = (int)g_audio_outs.size();
        int next = (g_audio_idx + dir + n) % n;
        if (demo_mode() || system_audio_output_set(g_audio_outs[next].id)) {
            for (auto &ao : g_audio_outs) ao.active = false;
            g_audio_outs[next].active = true;
            g_audio_idx = next;
            refresh_volume(a.now, true);
            sound_play(SFX_TOGGLE_ON);
            toast("Audio output: " + short_audio_name(g_audio_outs[next].name), 2.5);
            sanctum_log("overlay: switched audio output to \"%s\"", g_audio_outs[next].name.c_str());
        } else {
            sound_play(SFX_ERROR);
            toast("Could not switch audio output");
        }
    };

    auto trigger = [&](OvItemId id) {
        switch (id) {
        case O_RESUME:
            overlay_close();
            sound_play(SFX_OK);
            input_suppress_guide(a.now);
            resume_game(main_win);
            break;
        case O_SUSPEND: {
            std::string gname = (rp >= 0 && rp < (int)ports().size()) ? ports()[rp].title : "game";
            overlay_close();
            if (suspend_game(main_win)) {
                sound_play(SFX_TOGGLE_ON);
                toast("Suspended " + gname + " (Quick Resume ready)", 3.5);
            } else {
                if (transparent_mode && main_win) launch_wake_sanctum(main_win);
                sound_play(SFX_ERROR);
                toast("Could not suspend game");
            }
            break;
        }
        case O_SCREENSHOT:
            overlay_capture_screenshot(main_win);
            break;
        case O_HOME:
            switch_to_full_sanctum();
            sound_play(SFX_HOME);
            a.back.clear();
            a.screen = SCR_HOME;
            a.trans_t = a.now;
            a.trans_dir = -1;
            break;
        case O_VOLUME:
            if (!g_vol_ok) break;
            g_vol_muted = !g_vol_muted;
            if (!demo_mode()) system_volume_set(g_vol_pct, g_vol_muted);
            sound_play(g_vol_muted ? SFX_TOGGLE_OFF : SFX_TOGGLE_ON);
            break;
        case O_AUDIO_OUT:
            cycle_audio_output(+1);
            break;
        case O_PERF_HUD:
            cfg().perf_hud = (cfg().perf_hud + 1) % 3;
            config_save();
            sound_play(cfg().perf_hud ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
            toast(std::string("Performance HUD: ") + kHudNames[cfg().perf_hud]);
            break;
        case O_PERF:
            cfg().perf_mode = (cfg().perf_mode + 1) % 3;
            perf_sync_idle();
            config_save();
            sound_play(cfg().perf_mode ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
            toast("Performance mode: " + perf_status_short());
            break;
        case O_SOUNDS:
            cfg().sounds = !cfg().sounds;
            sound_set_enabled(cfg().sounds);
            config_save();
            sound_play(cfg().sounds ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
            break;
        case O_THEME:
            cfg().theme = cfg().theme ? 0 : 1;
            theme_set(cfg().theme);
            config_save();
            sound_play(SFX_OK);
            break;
        case O_FULLSCREEN:
            set_fullscreen(!cfg().fullscreen);
            sound_play(SFX_OK);
            break;
        case O_BROWSE:
            switch_to_full_sanctum();
            sound_play(SFX_OK);
            break;
        case O_DOWNLOADS:
            switch_to_full_sanctum();
            go(SCR_DOWNLOADS);
            break;
        case O_CONTROLLERS:
            switch_to_full_sanctum();
            go(SCR_CONTROLLERS);
            break;
        case O_ALBUM:
            switch_to_full_sanctum();
            album_open();
            break;
        case O_REFRESH:
            github_refresh(true);
            self_update_start(true);
            sound_play(SFX_OK);
            toast("Checking GitHub & live catalog for updates...");
            break;
        case O_SETTINGS:
            switch_to_full_sanctum();
            go(SCR_SETTINGS);
            break;
        case O_CLOSE_GAME:
            if (a.now - g_close_armed < 3.5) {
                g_close_armed = -10.0;
                overlay_close();
                sound_play(SFX_BACK);
                close_game(false);
                toast("Closing game...");
            } else {
                g_close_armed = a.now;
                sound_play(SFX_ERROR);
                toast("Press again to close the running game", 3.5);
            }
            break;
        case O_FORCE_QUIT:
            if (a.now - g_force_armed < 3.5) {
                g_force_armed = -10.0;
                overlay_close();
                sound_play(SFX_BACK);
                close_game(true);
                toast("Force-closed game");
            } else {
                g_force_armed = a.now;
                sound_play(SFX_ERROR);
                toast("Press again to force-quit immediately (unsaved progress lost)", 3.5);
            }
            break;
        case O_QUIT:
            if (a.now - g_quit_armed < 3.5) {
                g_quit_armed = -10.0;
                sound_play(SFX_BACK);
                a.quit = true;
            } else {
                g_quit_armed = a.now;
                sound_play(SFX_ERROR);
                toast("Press again to exit Sanctum", 3.5);
            }
            break;
        }
    };

    if (g_open) {
        switch (nav) {
        case NAV_UP:
            if (g_sel > 0) { g_sel--; sound_play(SFX_MOVE); }
            else sound_play(SFX_EDGE);
            break;
        case NAV_DOWN:
            if (g_sel < (int)items.size() - 1) { g_sel++; sound_play(SFX_MOVE); }
            else sound_play(SFX_EDGE);
            break;
        case NAV_LEFT:
            if (items[g_sel].id == O_VOLUME) adjust_volume(-5);
            else if (items[g_sel].id == O_AUDIO_OUT) cycle_audio_output(-1);
            else if (items[g_sel].id == O_PERF_HUD) {
                cfg().perf_hud = (cfg().perf_hud + 2) % 3;
                config_save();
                sound_play(cfg().perf_hud ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
                toast(std::string("Performance HUD: ") + kHudNames[cfg().perf_hud]);
            }
            else if (items[g_sel].id == O_THEME || items[g_sel].id == O_FULLSCREEN || items[g_sel].id == O_PERF) trigger(items[g_sel].id);
            break;
        case NAV_RIGHT:
            if (items[g_sel].id == O_VOLUME) adjust_volume(5);
            else if (items[g_sel].id == O_AUDIO_OUT) cycle_audio_output(+1);
            else if (items[g_sel].id == O_PERF_HUD) trigger(O_PERF_HUD);
            else if (items[g_sel].id == O_THEME || items[g_sel].id == O_FULLSCREEN || items[g_sel].id == O_PERF) trigger(items[g_sel].id);
            break;
        case NAV_A:
            trigger(items[g_sel].id);
            break;
        case NAV_B: {
            bool should_resume = g_from_game && running;
            overlay_close();
            sound_play(SFX_BACK);
            if (should_resume) {
                input_suppress_guide(a.now);
                resume_game(main_win);
            }
            break;
        }
        default: break;
        }
    }
    if (transparent_mode && !g_open && !g_ov_interactive) return;

    // ---- Draw console slide-in drawer (flush to physical right edge on 16:9, 16:10, and ultrawide displays)
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    float right_edge = (a.scale > 0.01f) ? (ds.x - a.ox) / a.scale : 1280.0f;
    float dw = 428.0f;
    float dx = right_edge - dw * g_anim;
    int alpha = (int)(255 * g_anim);

    // Subtle dimmed backdrop scrim (lighter in transparent in-game mode so the live paused game frame shines through!)
    int scrim_a = (int)((transparent_mode ? 118.0f : 165.0f) * g_anim);
    dl()->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(0, 0, 0, scrim_a));

    // Click outside the drawer closes it (or resumes in-game)
    if (g_open && clicked && a.mouse.x < dx) {
        bool should_resume = g_from_game && running;
        overlay_close();
        sound_play(SFX_BACK);
        if (should_resume) {
            input_suppress_guide(a.now);
            resume_game(main_win);
        }
        return;
    }

    // Soft left drop shadow + drawer body + accent edge
    float top_y = (a.scale > 0.01f) ? (-a.oy / a.scale) : 0.0f;
    float full_h = (a.scale > 0.01f) ? (ds.y / a.scale) : 720.0f;
    dl()->AddRectFilledMultiColor(P(dx - 32, top_y), P(dx, top_y + full_h),
                                  IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, (int)(135 * g_anim)),
                                  IM_COL32(0, 0, 0, (int)(135 * g_anim)), IM_COL32(0, 0, 0, 0));
    ImU32 body_bg = mix(p.bg2, deep, p.light ? 0.08f : 0.34f);
    rect(dx, top_y, dw + 40, full_h, with_alpha(body_bg, transparent_mode ? 242 : 250));
    rect(dx, top_y, 3, full_h, with_alpha(acc, alpha));

    // ---- Header
    time_t t = time(nullptr);
    struct tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&lt, &t);
#endif
    char clk[24];
    if (cfg().clock_12h) {
        int hr = lt.tm_hour % 12;
        if (hr == 0) hr = 12;
        SDL_snprintf(clk, sizeof clk, "%d:%02d %s", hr, lt.tm_min, lt.tm_hour >= 12 ? "PM" : "AM");
    } else {
        strftime(clk, sizeof clk, "%H:%M", &lt);
    }

    if (running && rp >= 0 && rp < (int)ports().size()) {
        const Port &gp = ports()[rp];
        bool susp = game_suspended();
        if (transparent_mode) draw_ov_tile(gp, dx + 24, 24, 68, g_anim);
        else tile(gp, dx + 24, 24, 68, g_anim, false);
        text(dx + 106, 26, 12, susp ? p.warn : p.ok, susp ? "SUSPENDED  ·  QUICK RESUME" : "NOW PLAYING");
        text(dx + dw - 24, 25, 14, p.soft, clk, 1);
        text(dx + 106, 46, 20, p.text, fit(20, gp.title, dw - 130).c_str());
        int mins = (int)running_seconds() / 60;
        std::string dur = susp ? "Paused  ·  0% CPU & GPU"
                        : (mins > 0 ? ("Paused in menu  ·  " + std::to_string(mins) + " min") : "Paused in menu  ·  < 1 min");
        text(dx + 106, 74, 13, p.soft, dur.c_str());
    } else {
        avatar_draw_player(dx + 48, 56, 22);
        text(dx + 82, 28, 12, acc, "SANCTUM QUICK MENU");
        text(dx + dw - 24, 27, 15, p.soft, clk, 1);
        text(dx + 82, 46, 20, p.text, fit(20, demo_mode() ? "Player 1" : cfg().profile, dw - 170).c_str());
        int sys_pct = -1;
        bool sys_chg = false;
        std::string sub = a.pad == PAD_KEYBOARD ? "Keyboard & mouse" : fit(13, a.pad_name, 190);
        if (system_battery_info(&sys_pct, &sys_chg) && sys_pct >= 0)
            sub += "  ·  " + std::to_string(sys_pct) + "%";
        text(dx + 82, 73, 13, p.dim, fit(13, sub, dw - 106).c_str());
    }
    rect(dx + 22, 108, dw - 44, 1, p.line);

    // ---- Rows
    float rx = dx + 22, rw = dw - 44;
    float stride = items.size() >= 11 ? 45.0f : items.size() >= 10 ? 48.0f : items.size() >= 9 ? 51.0f : 54.0f;
    float rh = items.size() >= 11 ? 39.0f : items.size() >= 10 ? 41.0f : items.size() >= 9 ? 43.0f : 46.0f;
    for (int i = 0; i < (int)items.size(); ++i) {
        const OvItem &it = items[i];
        float ry = 118.0f + i * stride;
        bool hov = g_open && hover(rx, ry, rw, rh);
        if (hov && a.mouse_moved && !clicked && g_sel != i) { g_sel = i; sound_play(SFX_MOVE); }

        ImU32 row_bg = it.primary ? acc : (i == g_sel ? p.panel2 : p.panel);
        bool bright = it.primary && (((acc & 255) * 299 + ((acc >> 8) & 255) * 587 + ((acc >> 16) & 255) * 114) / 1000 > 158);
        ImU32 fg = it.primary ? (bright ? IM_COL32(20, 20, 20, 255) : IM_COL32_WHITE) : (it.danger ? p.warn : p.text);
        ImU32 sub_fg = it.primary ? with_alpha(fg, 200) : (it.danger ? p.warn : p.soft);

        rect(rx, ry, rw, rh, row_bg, 11);
        icon(it.icon, rx + 24, ry + rh * 0.5f, 18, it.primary ? fg : (it.danger ? p.warn : acc));
        text(rx + 46, ry + (rh - 18) * 0.5f, 16, fg, it.label.c_str());

        if (it.slider) {
            float bx = rx + rw - 178, bw = 112, by = ry + rh * 0.5f - 2.5f;
            float frac = g_vol_muted ? 0.0f : (g_vol_pct / 100.0f);
            rect(bx, by, bw, 5, p.line, 2.5f);
            rect(bx, by, bw * frac, 5, g_vol_muted ? p.dim : acc, 2.5f);
            circle(bx + bw * frac, by + 2.5f, 7.0f, g_vol_muted ? p.dim : (i == g_sel ? acc : p.text));
            text(rx + rw - 14, ry + (rh - 16) * 0.5f, 14, g_vol_muted ? p.warn : p.soft, it.value.c_str(), 1);
            if (hov && clicked) {
                g_sel = i;
                if (a.mouse.x >= bx - 6 && a.mouse.x <= bx + bw + 6) {
                    int np = (int)std::lround(SDL_clamp((a.mouse.x - bx) / bw, 0.0f, 1.0f) * 20.0f) * 5;
                    g_vol_pct = np;
                    g_vol_muted = false;
                    if (!demo_mode()) system_volume_set(g_vol_pct, false);
                    sound_play(SFX_KEY);
                } else {
                    trigger(it.id);
                }
            }
        } else {
            text(rx + rw - 14, ry + (rh - 15) * 0.5f, 13, sub_fg, it.value.c_str(), 1);
            if (hov && clicked) { g_sel = i; trigger(it.id); }
        }
        if (g_open && i == g_sel) focus_ring(rx, ry, rw, rh, 11, 3, acc);
    }

    // ---- Footer hint bar inside drawer
    rect(dx + 22, 660, dw - 44, 1, p.line);
    const char *foot = (g_from_game && running) ? "A Select  ·  Left/Right Sound  ·  B / Guide Resume"
                                                : "A Select  ·  Left/Right Adjust  ·  B / Guide Close";
    text(dx + dw / 2, 678, 13, p.dim, foot, 0.5f);
}

void overlay_frame(Nav nav, bool clicked) {
    if (g_ov_interactive) return;   // rendered in dedicated transparent topmost window instead
    overlay_frame_impl(app().window, nav, clicked, false);
}

void overlay_win_render(SDL_Window *main_win, Nav nav, bool clicked) {
    if (!g_ov_win || !g_ov_ren || !g_ov_ctx || !g_ov_showing) return;
    App &a = app();

    // Auto-hide once both click-through screenshot toast and corner HUD are inactive
    if (!g_ov_interactive && a.now >= g_cap_toast_until && !hud_should_show()) {
        ov_win_hide();
        return;
    }

    ImGuiContext *prev_ctx = ImGui::GetCurrentContext();
    ImFont *prev_font = a.font;
    float prev_scale = a.scale, prev_ox = a.ox, prev_oy = a.oy;

    ImGui::SetCurrentContext(g_ov_ctx);
    a.font = g_ov_font;

    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    ImVec2 ds = ImGui::GetIO().DisplaySize;
    a.scale = SDL_min(ds.x / 1280.0f, ds.y / 720.0f);
    a.ox = (ds.x - 1280.0f * a.scale) / 2.0f;
    a.oy = (ds.y - 720.0f * a.scale) / 2.0f;

    if (g_ov_interactive) {
        overlay_frame_impl(main_win, nav, clicked, true);
        toast_draw();
    } else {
        if (a.now < g_cap_toast_until) {
            // Sleek click-through Switch-style top-left capture pill over the live running game
            double el = a.now - g_cap_toast_start;
            double rem = g_cap_toast_until - a.now;
            float k_in = (float)SDL_clamp(el / 0.16, 0.0, 1.0);
            k_in = 1.0f - (1.0f - k_in) * (1.0f - k_in) * (1.0f - k_in);
            float k_out = (float)SDL_clamp(rem / 0.35, 0.0, 1.0);
            float vis = k_in * k_out;
            float px = 26.0f - (1.0f - k_in) * 28.0f, py = 24.0f, pw = 356.0f, ph = 58.0f;
            rect(px, py, pw, ph, IM_COL32(18, 22, 30, (int)(236 * vis)), 14);
            rect(px, py + 10, 4, ph - 20, with_alpha(g_cap_toast_acc, (int)(255 * vis)), 2);
            icon(IC_ALBUM, px + 30, py + ph * 0.5f, 20, with_alpha(g_cap_toast_acc, (int)(255 * vis)));
            text(px + 54, py + 11, 16, IM_COL32(245, 248, 255, (int)(255 * vis)), g_cap_toast_title.c_str());
            text(px + 54, py + 33, 13, IM_COL32(175, 186, 204, (int)(245 * vis)), fit(13, g_cap_toast_sub, pw - 68).c_str());
        }
        if (hud_should_show()) {
            // Zero-inject click-through top-right Performance HUD pill (Compact FPS or Detailed FPS+CPU+GPU+RAM+Battery)
            const HudMetrics &hm = hud_metrics();
            float right_edge = (a.scale > 0.01f) ? (ds.x - a.ox) / a.scale : 1280.0f;
            std::string body;
            char fbuf[32];
            if (hm.has_fps) SDL_snprintf(fbuf, sizeof fbuf, "%.0f FPS", hm.fps);
            else SDL_snprintf(fbuf, sizeof fbuf, "-- FPS");
            body = fbuf;
            if (cfg().perf_hud >= 2) {
                char extra[160];
                if (hm.cpu_pct >= 0) {
                    SDL_snprintf(extra, sizeof extra, "  ·  CPU %d%%", hm.cpu_pct);
                    body += extra;
                }
                if (hm.gpu_pct >= 0) {
                    SDL_snprintf(extra, sizeof extra, "  ·  GPU %d%%", hm.gpu_pct);
                    body += extra;
                }
                if (hm.ram_gb > 0.05f) {
                    SDL_snprintf(extra, sizeof extra, "  ·  RAM %.1fG", hm.ram_gb);
                    body += extra;
                }
                if (hm.bat_pct >= 0) {
                    SDL_snprintf(extra, sizeof extra, "  ·  %d%%%s", hm.bat_pct, hm.bat_chg ? "+" : "");
                    body += extra;
                }
            }
            float pw = text_w(14, body.c_str()) + 38.0f;
            float ph = 32.0f;
            float px = right_edge - pw - 20.0f, py = 20.0f;
            ImU32 dot_c = !hm.has_fps ? IM_COL32(140, 152, 170, 255)
                        : hm.fps >= 55.0f ? IM_COL32(76, 214, 130, 255)
                        : hm.fps >= 29.0f ? IM_COL32(235, 240, 248, 255)
                        : IM_COL32(246, 178, 62, 255);
            rect(px, py, pw, ph, IM_COL32(14, 18, 24, 216), 16);
            frame(px, py, pw, ph, IM_COL32(255, 255, 255, 32), 16, 1.0f);
            circle(px + 15.0f, py + ph * 0.5f, 4.0f, dot_c);
            text(px + 26.0f, py + 8.0f, 14, IM_COL32(240, 244, 250, 248), body.c_str());
        }
    }

    ImGui::Render();
    // Clear to full alpha=0 so DWM composites the live game seamlessly behind the overlay
    SDL_SetRenderDrawColor(g_ov_ren, 0, 0, 0, 0);
    SDL_RenderClear(g_ov_ren);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_ov_ren);
    SDL_RenderPresent(g_ov_ren);

    a.font = prev_font;
    // Keep a.scale/ox/oy aligned to g_ov_win while interactive so input.cpp mouse coords stay exact next frame
    if (!g_ov_interactive) {
        a.scale = prev_scale;
        a.ox = prev_ox;
        a.oy = prev_oy;
    }
    ImGui::SetCurrentContext(prev_ctx);
}

}  // namespace sanctum
