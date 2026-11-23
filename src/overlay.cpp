#include "overlay.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>

#include "avatars.h"
#include "catalog.h"
#include "config.h"
#include "draw.h"
#include "github.h"
#include "input.h"
#include "install.h"
#include "launch.h"
#include "perf.h"
#include "platform.h"
#include "selfupdate.h"
#include "sound.h"
#include "theme.h"

namespace sanctum {

static bool g_open = false;
static bool g_from_game = false;
static float g_anim = 0.0f;
static int g_sel = 0;
static double g_close_armed = -10.0;
static double g_force_armed = -10.0;
static double g_quit_armed = -10.0;

// Cached Windows master volume (refreshed when overlay opens and every 0.5 s while open)
static bool g_vol_ok = false;
static int g_vol_pct = 50;
static bool g_vol_muted = false;
static double g_vol_checked = -10.0;

static void refresh_volume(double now, bool force = false) {
    if (!force && now - g_vol_checked < 0.5) return;
    g_vol_checked = now;
    if (demo_mode()) { g_vol_ok = true; g_vol_pct = 70; g_vol_muted = false; return; }
    g_vol_ok = system_volume_get(&g_vol_pct, &g_vol_muted);
}

bool overlay_is_open() { return g_open; }

void overlay_close() {
    g_open = false;
    g_from_game = false;
    g_close_armed = g_force_armed = g_quit_armed = -10.0;
}

void overlay_on_game_exit() {
    if (g_from_game) overlay_close();
    else g_close_armed = g_force_armed = -10.0;
}

void overlay_toggle(SDL_Window *w) {
    App &a = app();
    if (g_open) {
        bool should_resume = g_from_game && game_running();
        overlay_close();
        sound_play(SFX_BACK);
        if (should_resume) {
            input_suppress_guide(a.now);
            resume_game(w);
        }
        return;
    }
    if (game_running() && !game_suspended()) launch_overlay_pause(true);
    g_open = true;
    g_from_game = false;
    g_sel = 0;
    g_close_armed = g_force_armed = g_quit_armed = -10.0;
    refresh_volume(a.now, true);
    sound_play(SFX_HOME);
}

void overlay_open_from_game(SDL_Window *w) {
    App &a = app();
    // Pause the game immediately before waking Sanctum so overlay navigation never bleeds controller input into the game
    launch_overlay_pause(true);
    launch_wake_sanctum(w);
    g_open = true;
    g_from_game = true;
    g_sel = 0;
    g_anim = 0.0f;
    g_close_armed = g_force_armed = g_quit_armed = -10.0;
    refresh_volume(a.now, true);
    sound_play(SFX_HOME);
    sanctum_log("overlay: opened from running game via Guide button");
}

enum OvItemId {
    O_RESUME,
    O_SUSPEND,
    O_HOME,
    O_VOLUME,
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

void overlay_frame(Nav nav, bool clicked) {
    App &a = app();
    g_anim = ease(g_anim, g_open ? 1.0f : 0.0f, 18.0f);
    if (!g_open && g_anim < 0.01f) { g_anim = 0.0f; return; }
    if (g_open) refresh_volume(a.now);

    const Palette &p = pal();
    bool running = game_running();
    int rp = running_port();
    const Port *active_port = (running && rp >= 0 && rp < (int)ports().size()) ? &ports()[rp]
                            : (a.game >= 0 && a.game < (int)ports().size() && a.screen == SCR_GAME) ? &ports()[a.game]
                            : nullptr;
    ImU32 acc = active_port ? active_port->colours.accent : p.accent;
    ImU32 deep = active_port ? active_port->colours.deep : p.bg2;

    // Build context-aware items (in-game vs inside Sanctum)
    std::vector<OvItem> items;
    int active_dls = 0;
    for (const auto &j : install_jobs()) if (j.phase < PH_DONE) ++active_dls;
    int pad_pct = -1;
    bool pad_chg = false;
    bool has_pad_bat = input_pad_battery(&pad_pct, &pad_chg);
    std::string pad_val = has_pad_bat && pad_pct >= 0
                        ? (std::to_string(pad_pct) + "%" + (pad_chg ? " chg" : ""))
                        : (a.pad == PAD_KEYBOARD ? "Keyboard" : "Connected");

    if (running) {
        bool susp = game_suspended();
        items.push_back({O_RESUME, IC_PLAY, "Resume game", susp ? "Quick Resume" : "Return", true, false, false});
        if (!susp)
            items.push_back({O_SUSPEND, IC_PAUSE, "Suspend game", "Pause CPU & GPU", false, false, false});
        else if (a.screen != SCR_HOME)
            items.push_back({O_HOME, IC_LIBRARY, "Go to Home", "Keep suspended", false, false, false});
        if (g_vol_ok)
            items.push_back({O_VOLUME, IC_SOUND, "System volume", g_vol_muted ? "Muted" : (std::to_string(g_vol_pct) + "%"), false, false, true});
        if (!susp)
            items.push_back({O_BROWSE, IC_LIBRARY, "Browse Sanctum", "Game paused", false, false, false});
        items.push_back({O_CONTROLLERS, IC_PAD, "Controllers", pad_val, false, false, false});
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
        items.push_back({O_PERF, IC_UP, "Performance mode", perf_status_short(), false, false, false});
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

    auto adjust_volume = [&](int delta) {
        if (!g_vol_ok) return;
        int next = SDL_clamp(g_vol_pct + delta, 0, 100);
        if (next == g_vol_pct && !g_vol_muted) { sound_play(SFX_EDGE); return; }
        g_vol_pct = next;
        g_vol_muted = false;
        if (!demo_mode()) system_volume_set(g_vol_pct, false);
        sound_play(SFX_KEY);
    };

    auto trigger = [&](OvItemId id) {
        switch (id) {
        case O_RESUME:
            overlay_close();
            sound_play(SFX_OK);
            input_suppress_guide(a.now);
            resume_game(a.window);
            break;
        case O_SUSPEND: {
            std::string gname = (rp >= 0 && rp < (int)ports().size()) ? ports()[rp].title : "game";
            if (suspend_game(a.window)) {
                overlay_close();
                sound_play(SFX_TOGGLE_ON);
                toast("Suspended " + gname + " (Quick Resume ready)", 3.5);
            } else {
                sound_play(SFX_ERROR);
                toast("Could not suspend game");
            }
            break;
        }
        case O_HOME:
            overlay_close();
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
            overlay_close();
            sound_play(SFX_OK);
            break;
        case O_DOWNLOADS:
            overlay_close();
            go(SCR_DOWNLOADS);
            break;
        case O_CONTROLLERS:
            overlay_close();
            go(SCR_CONTROLLERS);
            break;
        case O_ALBUM:
            overlay_close();
            go(SCR_ALBUM);
            break;
        case O_REFRESH:
            github_refresh(true);
            self_update_start(true);
            sound_play(SFX_OK);
            toast("Checking GitHub & live catalog for updates...");
            break;
        case O_SETTINGS:
            overlay_close();
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
            else if (items[g_sel].id == O_THEME || items[g_sel].id == O_FULLSCREEN || items[g_sel].id == O_PERF) trigger(items[g_sel].id);
            break;
        case NAV_RIGHT:
            if (items[g_sel].id == O_VOLUME) adjust_volume(5);
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
                resume_game(a.window);
            }
            break;
        }
        default: break;
        }
    }

    // ---- Draw console slide-in drawer
    float dw = 420.0f;
    float dx = 1280.0f - dw * g_anim;
    int alpha = (int)(255 * g_anim);

    // Dimmed backdrop scrim across the whole display
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    dl()->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(0, 0, 0, (int)(165 * g_anim)));

    // Click outside the drawer closes it (or resumes in-game)
    if (g_open && clicked && a.mouse.x < dx) {
        bool should_resume = g_from_game && running;
        overlay_close();
        sound_play(SFX_BACK);
        if (should_resume) {
            input_suppress_guide(a.now);
            resume_game(a.window);
        }
        return;
    }

    // Soft left drop shadow + drawer body + accent edge
    dl()->AddRectFilledMultiColor(P(dx - 26, 0), P(dx, 720), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, (int)(125 * g_anim)),
                                  IM_COL32(0, 0, 0, (int)(125 * g_anim)), IM_COL32(0, 0, 0, 0));
    ImU32 body_bg = mix(p.bg2, deep, p.light ? 0.08f : 0.34f);
    rect(dx, 0, dw + 4, 720, with_alpha(body_bg, 250));
    rect(dx, 0, 3, 720, with_alpha(acc, alpha));

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
        tile(gp, dx + 24, 24, 68, g_anim, false);
        text(dx + 106, 26, 12, susp ? p.warn : p.ok, susp ? "SUSPENDED  ·  QUICK RESUME" : "NOW PLAYING");
        text(dx + dw - 24, 25, 14, p.soft, clk, 1);
        text(dx + 106, 46, 20, p.text, fit(20, gp.title, dw - 130).c_str());
        int mins = (int)running_seconds() / 60;
        std::string dur = susp ? "Paused  ·  0% CPU & GPU"
                        : (mins > 0 ? ("Session  ·  " + std::to_string(mins) + " min") : "Session  ·  < 1 min");
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
    float stride = items.size() >= 10 ? 50.0f : 54.0f;
    float rh = items.size() >= 10 ? 43.0f : 46.0f;
    for (int i = 0; i < (int)items.size(); ++i) {
        const OvItem &it = items[i];
        float ry = 120.0f + i * stride;
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
        if (g_open && i == g_sel) focus_ring(rx, ry, rw, rh, 11, 3);
    }

    // ---- Footer hint bar inside drawer
    rect(dx + 22, 660, dw - 44, 1, p.line);
    const char *foot = (g_from_game && running) ? "A Select  ·  Left/Right Volume  ·  B / Guide Resume"
                                                : "A Select  ·  Left/Right Adjust  ·  B / Guide Close";
    text(dx + dw / 2, 678, 13, p.dim, foot, 0.5f);
}

}  // namespace sanctum
