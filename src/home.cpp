// Home: the profile at the top left, the row of ports, the round buttons below, the hints along the bottom.
// Focus lives in one of three zones (profile, row, buttons); up and down move between them, left and right inside.
#include <algorithm>
#include <cmath>

#include "avatars.h"
#include "config.h"
#include "covers.h"
#include "draw.h"
#include "github.h"
#include "install.h"
#include "launch.h"
#include "port_settings.h"
#include "sound.h"

namespace sanctum {

enum { Z_PROFILE, Z_ROW, Z_DOCK };

struct DockItem {
    Icon icon;
    const char *label;
    ImU32 dark_col;
    ImU32 light_col;
    bool featured = false;   // bold filled circular accent button (left end of Floating Pill)
};
static const DockItem kDock[] = {
    {IC_STORE,    "Get ports",       IM_COL32(226, 32, 46, 255),   IM_COL32(216, 28, 42, 255), true},
    {IC_DOWNLOAD, "Downloads",       IM_COL32(244, 146, 48, 255),  IM_COL32(214, 114, 20, 255), false},
    {IC_NEWS,     "Releases & news", IM_COL32(34, 206, 102, 255),  IM_COL32(18, 162, 78, 255), false},
    {IC_ALBUM,    "Album",           IM_COL32(90, 164, 245, 255),  IM_COL32(42, 124, 220, 255), false},
    {IC_MODS,     "Mods",            IM_COL32(38, 202, 178, 255),  IM_COL32(16, 158, 138, 255), false},
    {IC_PAD,      "Controllers",     0, 0, false},
    {IC_GEAR,     "System Settings", 0, 0, false},
    {IC_POWER,    "Power",           0, 0, false},
};
static const int kDockN = sizeof kDock / sizeof kDock[0];

static int g_zone = Z_ROW, g_dock = 0, g_prof_sel = 0;
static float g_scroll = 0, g_tint_t = 0;
static ImU32 g_tint = 0;
static float g_pop[64];             // each tile's grow when focused
static float g_ring_x = 0, g_ring_y = 0, g_ring_s = 0;
static bool g_ring_init = false;
static double g_bounce_at = -10.0;
static int g_bounce_pos = -1;
static bool g_power = false;        // the Power menu is open
static int g_power_sel = 0;

// ---- Home quick-options menu (X on a port tile)
static bool g_quick = false;
static int g_quick_port = -1, g_quick_sel = 0;
enum { Q_PLAY, Q_UPDATE, Q_DETAILS, Q_SETTINGS, Q_MODS, Q_PIN, Q_FOLDER, Q_REMOVE_CUSTOM };
struct QuickItem {
    Icon icon;
    std::string label;
    int id;
};

static void quick_launch_port(int idx) {
    App &a = app();
    auto &ps = ports();
    if (idx < 0 || idx >= (int)ps.size()) return;
    Port &port = ps[idx];
    if (running_port() == idx) {
        sound_play(SFX_OK);
        resume_game(a.window);
        return;
    }
    const Install *in = port.best();
    if (!in) {
        get_ports_select(idx);
        return;
    }
    launch(a.window, idx, *in, !can_play(port, *in));
}

static void open_dock(int i) {
    switch (i) {
    case 0: go(SCR_PORTS); break;
    case 1: go(SCR_DOWNLOADS); break;
    case 2: go(SCR_NEWS); break;
    case 3: album_open(); break;
    case 4: mods_open(-1); break;
    case 5: go(SCR_CONTROLLERS); break;
    case 6: go(SCR_SETTINGS); break;
    case 7:   // the press that opens the menu must not also pick in it
        g_power = true, g_power_sel = 1;   // Cancel is the safe default
        sound_play(SFX_OK);
        app().nav = NAV_NONE, app().clicked = false;
        break;
    }
}

static void power_frame() {
    App &a = app();
    const Palette &p = pal();
    const char *items[] = {install_busy() ? "Close Sanctum (stops the downloads)" : "Close SteelTemple Sanctum", "Cancel"};
    if (a.nav == NAV_UP || a.nav == NAV_DOWN) { g_power_sel ^= 1; sound_play(SFX_MOVE); }
    for (int i = 0; i < 2; ++i)
        if (a.mouse_moved && !a.clicked && hover(470, 330 + i * 64, 340, 54) && g_power_sel != i) {
            g_power_sel = i;
            sound_play(SFX_MOVE);
        }
    bool pick = a.nav == NAV_A || (a.clicked && hover(470, 330 + g_power_sel * 64, 340, 54));
    if (pick && g_power_sel == 0) { sound_play(SFX_BACK); a.quit = true; }
    if ((pick && g_power_sel == 1) || a.nav == NAV_B || (a.clicked && !hover(430, 240, 420, 250))) {
        sound_play(SFX_BACK);
        g_power = false;
    }
    a.nav = NAV_NONE;
    a.clicked = false;

    rect(0, 0, 1280, 720, IM_COL32(0, 0, 0, 140));
    rect(430, 240, 420, 250, p.bg2, 22);
    icon(IC_POWER, 640, 280, 34, p.text);
    text(640, 300, 15, p.dim, "Power options", 0.5f);
    for (int i = 0; i < 2; ++i) {
        float y = 330 + i * 64;
        rect(470, y, 340, 54, p.panel, 27);
        if (i == g_power_sel) focus_ring(470, y, 340, 54, 27, 4);
        text(640, y + 15, 20, p.text, items[i], 0.5f);
    }
}

static void quick_frame() {
    App &a = app();
    const Palette &p = pal();
    if (g_quick_port < 0 || g_quick_port >= (int)ports().size()) { g_quick = false; return; }
    Port &port = ports()[g_quick_port];
    const Install *in = port.best();
    PortState st = port.state();
    bool pinned = cfg().pinned.count(port.id) > 0;

    std::vector<QuickItem> items;
    if (in) items.push_back({IC_PLAY, can_play(port, *in) ? "Play now" : "Set up", Q_PLAY});
    else items.push_back({IC_DOWNLOAD, "Download port", Q_PLAY});
    if (st == PORT_UPDATE && !install_queued(g_quick_port))
        items.push_back({IC_UP, "Update to " + port.latest_tag(), Q_UPDATE});
    items.push_back({IC_INFO, "Open game page", Q_DETAILS});
    if (in && !port.custom) items.push_back({IC_GEAR, "Port settings", Q_SETTINGS});
    if (in && !port.custom) items.push_back({IC_MODS, "Manage mods", Q_MODS});
    items.push_back({IC_PIN, pinned ? "Unpin from front" : "Pin to front", Q_PIN});
    if (in) items.push_back({IC_FOLDER, "Open install folder", Q_FOLDER});
    if (port.custom) items.push_back({IC_CLOSE, "Remove shortcut", Q_REMOVE_CUSTOM});

    int n = (int)items.size();
    if (g_quick_sel >= n) g_quick_sel = std::max(0, n - 1);

    auto run = [&](int id) {
        g_quick = false;
        a.nav = NAV_NONE, a.clicked = false;
        switch (id) {
        case Q_PLAY:
            if (in) launch(a.window, g_quick_port, *in, !can_play(port, *in));
            else get_ports_select(g_quick_port);
            break;
        case Q_UPDATE:
            if (const Release *r = port.latest(); r && in) {
                if (const Asset *up = r->find(ASSET_UPDATE)) install_enqueue(g_quick_port, *r, *up, JOB_UPDATE, in->dir);
                else if (const Asset *po = r->find(ASSET_PORTABLE)) install_enqueue(g_quick_port, *r, *po, JOB_UPDATE, in->dir);
                else if (const Asset *w = r->find(ASSET_WIZARD)) install_enqueue(g_quick_port, *r, *w, JOB_WIZARD);
                sound_play(SFX_OK);
                toast("Queued update for " + std::string(port.name));
                go(SCR_DOWNLOADS);
            }
            break;
        case Q_DETAILS:
            a.game = g_quick_port;
            go(SCR_GAME);
            break;
        case Q_SETTINGS:
            if (port_settings(port.id)) port_settings_open(g_quick_port);
            else game_open_tab(g_quick_port, 2);
            break;
        case Q_MODS:
            mods_open(g_quick_port);
            break;
        case Q_PIN:
            if (cfg().pinned.count(port.id)) {
                cfg().pinned.erase(port.id);
                sound_play(SFX_TOGGLE_OFF);
                toast("Unpinned " + std::string(port.title));
            } else {
                cfg().pinned.insert(port.id);
                sound_play(SFX_TOGGLE_ON);
                toast("Pinned " + std::string(port.title) + " to front");
            }
            config_save();
            break;
        case Q_FOLDER:
            if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: folders don't open"); }
            else if (in) { sound_play(SFX_OK); SDL_OpenURL(("file:///" + in->dir).c_str()); }
            break;
        case Q_REMOVE_CUSTOM:
            if (catalog_remove_custom(g_quick_port)) {
                sound_play(SFX_BACK);
                toast("Removed custom shortcut");
                if (a.game >= (int)ports().size()) a.game = std::max(0, (int)ports().size() - 1);
            }
            break;
        }
    };

    float mw = 420, mh = 114 + n * 52, mx = 640 - mw / 2, my = 360 - mh / 2;
    if (a.nav == NAV_UP) { if (g_quick_sel > 0) { g_quick_sel--; sound_play(SFX_MOVE); } else sound_play(SFX_EDGE); }
    else if (a.nav == NAV_DOWN) { if (g_quick_sel < n - 1) { g_quick_sel++; sound_play(SFX_MOVE); } else sound_play(SFX_EDGE); }
    else if (a.nav == NAV_A) { run(items[g_quick_sel].id); return; }
    else if (a.nav == NAV_B || a.nav == NAV_X || (a.clicked && !hover(mx, my, mw, mh))) {
        sound_play(SFX_BACK);
        g_quick = false;
        a.nav = NAV_NONE, a.clicked = false;
        return;
    }

    rect(0, 0, 1280, 720, IM_COL32(0, 0, 0, 150));
    rect(mx, my, mw, mh, p.bg2, 22);
    tile(port, mx + 24, my + 20, 56, 1, false);
    text(mx + 96, my + 24, 19, p.text, fit(19, port.title, mw - 120).c_str());
    text(mx + 96, my + 50, 14, port.colours.accent,
         fit(14, port.custom ? "Custom PC Game" : (std::string("SteelTemple ") + port.name), mw - 120).c_str());
    for (int i = 0; i < n; ++i) {
        float y = my + 94 + i * 52;
        if (hover(mx + 22, y, mw - 44, 44)) {
            if (a.mouse_moved && !a.clicked && g_quick_sel != i) { g_quick_sel = i; sound_play(SFX_MOVE); }
            if (a.clicked) { run(items[i].id); return; }
        }
        rect(mx + 22, y, mw - 44, 44, p.panel, 12);
        ImU32 ic = items[i].id == Q_REMOVE_CUSTOM ? p.warn : (items[i].id == Q_PLAY ? port.colours.accent : p.soft);
        icon(items[i].icon, mx + 48, y + 22, 20, ic);
        text(mx + 74, y + 12, 17, items[i].id == Q_REMOVE_CUSTOM ? p.warn : p.text, items[i].label.c_str());
        if (i == g_quick_sel) focus_ring(mx + 22, y, mw - 44, 44, 12, 3);
    }
    a.nav = NAV_NONE;
    a.clicked = false;
}

void home_frame() {
    App &a = app();
    const Palette &p = pal();
    auto &ps = ports();
    // Home row: installed & actively-downloading titles (pinned first, then sort order, capped at 12 so Home stays sleek),
    // followed by the "Show more" tile (which opens SCR_LIBRARY with all installed ports, custom PC games, & Add custom game).
    // If nothing is installed yet (fresh install), show all catalog ports so Home is never empty.
    std::vector<int> installed_all;
    for (int i = 0; i < (int)ps.size(); ++i)
        if (!ps[i].installs.empty() || install_queued(i)) installed_all.push_back(i);
    std::vector<int> order;
    if (!installed_all.empty()) order = installed_all;
    else for (int i = 0; i < (int)ps.size(); ++i) order.push_back(i);

    int sort_mode = cfg().home_sort;
    std::stable_sort(order.begin(), order.end(), [&](int x, int y) {
        bool px = cfg().pinned.count(ps[x].id) > 0, py = cfg().pinned.count(ps[y].id) > 0;
        if (px != py) return px;
        bool hx = !ps[x].installs.empty(), hy = !ps[y].installs.empty();
        if (hx != hy) return hx;
        if (sort_mode == 1) {
            long long sx = played_seconds(x), sy = played_seconds(y);
            if (sx != sy) return sx > sy;
        } else if (sort_mode == 2) {
            int c = SDL_strcasecmp(ps[x].title, ps[y].title);
            if (c != 0) return c < 0;
        }
        return last_played(x) > last_played(y);
    });
    const int kHomeRowCap = 12;
    if ((int)order.size() > kHomeRowCap) {
        // Keep focused game visible if SANCTUM_FOCUS targeted an item beyond the cap
        bool has_cur = false;
        for (int i = 0; i < kHomeRowCap; ++i) if (order[i] == a.game) { has_cur = true; break; }
        order.resize(kHomeRowCap);
        if (!has_cur && a.game >= 0 && a.game < (int)ps.size() && (!ps[a.game].installs.empty() || install_queued(a.game)))
            order.back() = a.game;
    }
    const int kShowMoreId = (int)ps.size();
    order.push_back(kShowMoreId);
    int n = (int)order.size();
    int pos = 0;
    bool found_pos = false;
    for (int i = 0; i < n; ++i)
        if (order[i] == a.game) { pos = i; found_pos = true; break; }
    if (!found_pos) { pos = 0; a.game = order[0]; }
    int total_library = (int)installed_all.size();

    int prof_n = (int)cfg().profiles.size();
    int prof_slots = prof_n + (prof_n < 4 ? 1 : 0);
    if (g_prof_sel >= prof_slots) g_prof_sel = std::max(0, prof_slots - 1);

    auto pick_profile = [&](int idx) {
        if (idx == prof_n) {
            if (profile_add() >= 0) { g_prof_sel = cfg().active_profile; profile_open(); }
        } else if (idx != cfg().active_profile) {
            profile_switch(idx);
            sound_play(SFX_TOGGLE_ON);
            toast("Switched to " + cfg().profile);
        } else {
            profile_open();
        }
    };

    if (!g_power && !g_quick) {
        auto pan_of = [](float cx) { return SDL_clamp((cx - 640.0f) / 640.0f * 0.32f, -0.32f, 0.32f); };
        float tile_step = (cfg().large_tiles ? 236.0f : 196.0f) + 18.0f;
        // ---- input
        switch (a.nav) {
        case NAV_LEFT:
            if (g_zone == Z_PROFILE) {
                if (g_prof_sel > 0) { g_prof_sel--; sound_play_move(-1, pan_of(67.0f + g_prof_sel * 58.0f)); }
                else sound_play(SFX_EDGE);
            } else if (g_zone == Z_ROW) {
                if (pos > 0) {
                    a.game = order[--pos];
                    g_bounce_at = a.now;
                    g_bounce_pos = pos;
                    sound_play_move(-1, pan_of(96.0f + pos * tile_step - g_scroll + 110.0f));
                } else sound_play(SFX_EDGE);
            } else if (g_zone == Z_DOCK) {
                if (g_dock > 0) { g_dock--; sound_play_move(-1, ((float)g_dock / (float)(kDockN - 1) - 0.5f) * 0.56f); }
                else sound_play(SFX_EDGE);
            }
            break;
        case NAV_RIGHT:
            if (g_zone == Z_PROFILE) {
                if (g_prof_sel < prof_slots - 1) { g_prof_sel++; sound_play_move(1, pan_of(67.0f + g_prof_sel * 58.0f)); }
                else sound_play(SFX_EDGE);
            } else if (g_zone == Z_ROW) {
                if (pos < n - 1) {
                    a.game = order[++pos];
                    g_bounce_at = a.now;
                    g_bounce_pos = pos;
                    sound_play_move(1, pan_of(96.0f + pos * tile_step - g_scroll + 110.0f));
                } else sound_play(SFX_EDGE);
            } else if (g_zone == Z_DOCK) {
                if (g_dock < kDockN - 1) { g_dock++; sound_play_move(1, ((float)g_dock / (float)(kDockN - 1) - 0.5f) * 0.56f); }
                else sound_play(SFX_EDGE);
            }
            break;
        case NAV_UP:
            if (g_zone > Z_PROFILE) {
                g_zone--;
                if (g_zone == Z_PROFILE) g_prof_sel = cfg().active_profile;
                else if (g_zone == Z_ROW) { g_bounce_at = a.now; g_bounce_pos = pos; g_ring_init = false; }
                sound_play_move(-1, 0.0f);
            } else sound_play(SFX_EDGE);
            break;
        case NAV_DOWN:
            if (g_zone < Z_DOCK) {
                g_zone++;
                if (g_zone == Z_ROW) { g_bounce_at = a.now; g_bounce_pos = pos; g_ring_init = false; }
                sound_play_move(1, 0.0f);
            } else sound_play(SFX_EDGE);
            break;
        case NAV_A:
            if (g_zone == Z_PROFILE) pick_profile(g_prof_sel);
            if (g_zone == Z_ROW) {
                if (a.game < (int)ps.size() && (!ps[a.game].installs.empty() || install_queued(a.game))) go(SCR_GAME);
                else if (a.game < (int)ps.size()) get_ports_select(a.game);   // not on this PC: straight to getting it
                else library_open();
            }
            if (g_zone == Z_DOCK) open_dock(g_dock);
            break;
        case NAV_X:
            if (g_zone == Z_ROW && a.game < (int)ps.size()) {
                g_quick = true, g_quick_port = a.game, g_quick_sel = 0;
                sound_play(SFX_OK);
                a.nav = NAV_NONE, a.clicked = false;
            }
            break;
        case NAV_Y:
            if (g_zone == Z_ROW && a.game < (int)ps.size()) {
                quick_launch_port(a.game);
                a.nav = NAV_NONE, a.clicked = false;
            }
            break;
        case NAV_PLUS:   // look again: this PC and GitHub
            sound_play(SFX_OK);
            if (!scan_running()) scan_start();
            github_refresh(true);
            toast("Looking for ports on this PC and new releases on GitHub");
            break;
        case NAV_B:
            if (g_zone != Z_ROW) { g_zone = Z_ROW; sound_play(SFX_BACK); }
            break;
        default: break;
        }
    }

    // ---- layout
    float size = cfg().large_tiles ? 236 : 196, gap = 18;
    float row_y = cfg().large_tiles ? 170 : 190;
    int visible = (int)((1280 - 96 - 60) / (size + gap));
    float want = std::max(0.0f, (float)(pos - (visible - 2))) * (size + gap);
    want = std::min(want, std::max(0.0f, n * (size + gap) - (1280 - 96 - 60)));
    g_scroll = ease(g_scroll, want, 14);

    // dock geometry (Floating Pill capsule vs Classic Circles)
    bool pill_dock = (cfg().dock_style == 0);
    float dsz = pill_dock ? 62.0f : 66.0f;
    float dgap = pill_dock ? 14.0f : 22.0f;
    float dstep = dsz + dgap;
    float dx0 = 640.0f - (kDockN * dsz + (kDockN - 1) * dgap) / 2.0f;
    float dock_y = pill_dock ? 492.0f : 490.0f;

    // mouse: hover focuses, click opens
    if (!g_power && !g_quick) {
        for (int i = 0; i < prof_slots; ++i) {
            float cx = 67 + i * 58, cy = 46;
            if (hover(cx - 23, cy - 23, 46, 46)) {
                if (a.mouse_moved && !a.clicked && (g_zone != Z_PROFILE || g_prof_sel != i)) {
                    int dir = i > g_prof_sel ? 1 : -1;
                    g_zone = Z_PROFILE, g_prof_sel = i;
                    sound_play_move(dir, SDL_clamp((cx - 640.0f) / 640.0f * 0.32f, -0.32f, 0.32f));
                }
                if (a.clicked) { g_zone = Z_PROFILE, g_prof_sel = i; pick_profile(i); }
            }
        }
        for (int i = 0; i < n; ++i) {
            float x = 96 + i * (size + gap) - g_scroll;
            if (hover(x, row_y, size, size)) {
                if (a.mouse_moved && !a.clicked && (g_zone != Z_ROW || a.game != order[i])) {
                    int dir = i > pos ? 1 : -1;
                    g_zone = Z_ROW, a.game = order[i], pos = i;
                    g_bounce_at = a.now;
                    g_bounce_pos = pos;
                    sound_play_move(dir, SDL_clamp((x + size * 0.5f - 640.0f) / 640.0f * 0.32f, -0.32f, 0.32f));
                }
                if (a.clicked) {
                    g_zone = Z_ROW, a.game = order[i], pos = i;
                    int id = order[i];
                    if (id < (int)ps.size()) {
                        bool can_quick = !ps[id].installs.empty();
                        float pbx = x + size - 32.0f, pby = row_y + size - 32.0f;
                        if (can_quick && a.pad == PAD_KEYBOARD && std::hypot(a.mouse.x - pbx, a.mouse.y - pby) <= 24.0f) {
                            quick_launch_port(id);
                        } else {
                            go(SCR_GAME);
                        }
                    } else {
                        library_open();
                    }
                }
            }
        }
        for (int i = 0; i < kDockN; ++i)
            if (hover(dx0 + i * dstep, dock_y, dsz, dsz)) {
                if (a.mouse_moved && !a.clicked && (g_zone != Z_DOCK || g_dock != i)) {
                    int dir = i > g_dock ? 1 : -1;
                    g_zone = Z_DOCK, g_dock = i;
                    sound_play_move(dir, ((float)i / (float)(kDockN - 1) - 0.5f) * 0.56f);
                }
                if (a.clicked) { g_zone = Z_DOCK, g_dock = i; open_dock(i); }
            }
    }

    // ---- draw: the backdrop takes the focused port's colour
    const Port *fp = g_zone == Z_ROW && a.game < (int)ps.size() ? &ps[a.game] : nullptr;
    ImU32 want_tint = cfg().follow && fp ? fp->colours.accent : p.accent;
    if (want_tint != g_tint) { g_tint_t = 0; }
    g_tint_t = std::min(1.0f, g_tint_t + a.dt * 4);
    g_tint = mix(g_tint ? g_tint : want_tint, want_tint, g_tint_t);
    background(g_tint);
    status_bar(false);
    // top-left profile row (Switch style: active + other local profiles + "+")
    prof_n = (int)cfg().profiles.size();
    prof_slots = prof_n + (prof_n < 4 ? 1 : 0);
    for (int i = 0; i < prof_n; ++i) {
        const PlayerProfile &pr = cfg().profiles[i];
        float cx = 67 + i * 58, cy = 46;
        bool active = i == cfg().active_profile;
        avatar_draw(avatar_find(pr.avatar), pr.avatar_bg, cx, cy, 23, active ? 1.0f : 0.72f);
        if (active && prof_n > 1) dl()->AddCircle(P(cx, cy), S(25), with_alpha(p.text, 160), 32, S(2));
        if (g_zone == Z_PROFILE && g_prof_sel == i) focus_ring(cx - 23, cy - 23, 46, 46, 23, 4);
    }
    if (prof_n < 4) {
        float cx = 67 + prof_n * 58, cy = 46;
        bool f = g_zone == Z_PROFILE && g_prof_sel == prof_n;
        bool show_plus = g_zone == Z_PROFILE || prof_n > 1 || hover(cx - 23, cy - 23, 46, 46);
        if (show_plus) {
            circle(cx, cy, 20, f ? p.panel2 : with_alpha(p.panel, 150));
            text(cx, cy - 13, 22, f ? p.text : p.soft, "+", 0.5f);
            if (f) focus_ring(cx - 21, cy - 21, 42, 42, 21, 4);
        }
    }

    // the row (clipped so tiles slide under the screen edge; focused tile drawn last so elastic pop sits in front)
    int tmot = SDL_clamp(cfg().tile_motion, 0, 2);
    float foc_x = 0, foc_y = 0, foc_s = 0;
    bool has_foc = false;
    ImU32 foc_acc = fp ? fp->colours.accent : p.accent;
    auto draw_row_tile = [&](int i) {
        int id = order[i];
        bool f = g_zone == Z_ROW && i == pos;
        int pop_i = i < 64 ? i : 63;
        if (tmot == 0) g_pop[pop_i] = f ? 1.0f : 0.0f;
        else g_pop[pop_i] = ease(g_pop[pop_i], f ? 1.0f : 0.0f, tmot == 2 ? 20.0f : 15.0f);
        float bounce = 0.0f;
        if (tmot == 2 && f && i == g_bounce_pos) {
            float bt = (float)(a.now - g_bounce_at);
            if (bt >= 0.0f && bt < 0.42f)
                bounce = std::sin(bt * 22.0f) * std::exp(-bt * 10.5f) * 0.028f;
        }
        float grow = tmot == 0 ? (f ? 1.045f : 1.0f)
                   : tmot == 1 ? (1.0f + 0.055f * g_pop[pop_i])
                   : (1.0f + 0.072f * g_pop[pop_i] + bounce);
        float lift = (tmot == 2) ? (-4.0f * g_pop[pop_i]) : 0.0f;
        float s = size * grow;
        float x = 96 + i * (size + gap) - g_scroll - (s - size) / 2.0f;
        float y = row_y - (s - size) / 2.0f + lift;
        float intro = std::min(1.0f, std::max(0.0f, (float)(a.now - 0.08 * i) * 3.2f));
        y += (1 - intro) * (1 - intro) * 40;
        if (id < (int)ps.size()) {
            tile(ps[id], x, y, s, intro, true, g_pop[pop_i]);
            if (running_port() == id) {
                float w = text_w(s * 0.06f, "Playing") + s * 0.16f, h = s * 0.12f;
                rect(x + s * 0.05f, y + s * 0.05f, w, h, with_alpha(p.ok, 235), h / 2);
                icon(IC_PLAY, x + s * 0.05f + h * 0.55f, y + s * 0.05f + h / 2, h * 0.5f, IM_COL32_WHITE);
                text(x + s * 0.05f + h, y + s * 0.05f + h * 0.18f, s * 0.06f, IM_COL32_WHITE, "Playing");
            }
            // Hover-only quick-play circle for mouse users (keeps cover art 100% clean for controllers)
            if (!g_power && !g_quick && a.pad == PAD_KEYBOARD && !ps[id].installs.empty() && hover(x, y, s, s)) {
                float pbx = x + s - 32.0f, pby = y + s - 32.0f;
                bool ph = std::hypot(a.mouse.x - pbx, a.mouse.y - pby) <= 24.0f;
                circle(pbx, pby + 2.0f, 22.0f, IM_COL32(0, 0, 0, 120));
                circle(pbx, pby, ph ? 23.0f : 21.0f, ph ? ps[id].colours.accent : with_alpha(IM_COL32(18, 22, 30, 255), 225));
                dl()->AddCircle(P(pbx, pby), S(ph ? 23.0f : 21.0f), with_alpha(IM_COL32_WHITE, ph ? 210 : 120), 32, S(1.5f));
                icon(IC_PLAY, pbx + 1.5f, pby, 17.0f, IM_COL32_WHITE);
            }
        } else {   // "Show more" tile: bespoke 2x2 glass cover mosaic + "+" badge
            int a8 = (int)(255 * intro);
            float rad = s * 0.065f;
            // Soft tile drop shadow matching game covers
            rect(x + 2, y + 8, s - 4, s, with_alpha(IM_COL32_BLACK, (int)((p.light ? 30 : 90) * intro)), rad + 4);
            // Rich frosted glass card backdrop
            ImU32 card_bg = p.light ? IM_COL32(246, 248, 252, a8) : IM_COL32(34, 37, 45, a8);
            ImU32 card_hi = p.light ? IM_COL32(255, 255, 255, a8) : IM_COL32(46, 51, 62, a8);
            rect(x, y, s, s, card_bg, rad);
            dl()->AddRectFilledMultiColor(P(x + 2, y + 2), P(x + s - 2, y + s * 0.55f),
                                          with_alpha(card_hi, a8), with_alpha(card_hi, a8),
                                          with_alpha(card_bg, 0), with_alpha(card_bg, 0));
            frame(x, y, s, s, with_alpha(p.light ? IM_COL32(180, 188, 204, 255) : IM_COL32_WHITE, p.light ? (int)(90 * intro) : (int)(28 * intro)), rad, 1.2f);

            // 2x2 mini-tile mosaic centered in upper 66% of the tile (uses real covers if available, else sleek prism tiles)
            float m_sz = s * 0.215f, m_gap = s * 0.042f;
            float m_tot = m_sz * 2.0f + m_gap;
            float mx0 = x + (s - m_tot) * 0.5f;
            float my0 = y + s * 0.145f;
            static const ImU32 kMosaicCols[4] = {
                IM_COL32(66, 212, 246, 255),   // sky cyan
                IM_COL32(124, 108, 240, 255),  // periwinkle indigo
                IM_COL32(44, 206, 152, 255),   // mint emerald
                IM_COL32(242, 114, 158, 255),  // rose coral
            };
            // Gather up to 4 cover textures (prefer custom games + installed titles for a live personal preview)
            SDL_Texture *m_tex[4] = {};
            int m_found = 0;
            for (int pass = 0; pass < 2 && m_found < 4; ++pass) {
                for (int k = 0; k < (int)ps.size() && m_found < 4; ++k) {
                    if (ps[k].installs.empty()) continue;
                    if ((pass == 0) != ps[k].custom) continue;
                    if (SDL_Texture *t = cover(ps[k].id)) m_tex[m_found++] = t;
                }
            }
            for (int cell = 0; cell < 4; ++cell) {
                int cx_i = cell % 2, cy_i = cell / 2;
                float cx = mx0 + cx_i * (m_sz + m_gap);
                float cy = my0 + cy_i * (m_sz + m_gap);
                float cr = m_sz * 0.22f;
                rect(cx + 1, cy + 3, m_sz - 2, m_sz, with_alpha(IM_COL32_BLACK, (int)((p.light ? 26 : 85) * intro)), cr);
                if (m_tex[cell]) {
                    dl()->AddImageRounded((ImTextureID)(intptr_t)m_tex[cell], P(cx, cy), P(cx + m_sz, cy + m_sz),
                                          ImVec2(0, 0), ImVec2(1, 1), IM_COL32(240, 244, 252, a8), S(cr));
                    frame(cx, cy, m_sz, m_sz, with_alpha(IM_COL32_WHITE, (int)(55 * intro)), cr, 1.0f);
                } else {
                    rect(cx, cy, m_sz, m_sz, with_alpha(kMosaicCols[cell], (int)(225 * intro)), cr);
                    rect(cx + m_sz * 0.14f, cy + m_sz * 0.14f, m_sz * 0.72f, m_sz * 0.16f,
                         with_alpha(IM_COL32_WHITE, (int)(70 * intro)), cr * 0.5f);
                }
            }
            // Sleek circular "+" accent badge overlapping bottom-right of the 2x2 mosaic
            float bx = mx0 + m_tot - s * 0.02f, by = my0 + m_tot - s * 0.02f, br = s * 0.088f;
            circle(bx, by, br + 2.5f, card_bg);
            circle(bx, by, br, with_alpha(p.light ? IM_COL32(0, 162, 214, 255) : IM_COL32(72, 214, 248, 255), a8));
            float arm = br * 0.48f;
            ImU32 plus_c = with_alpha(p.light ? IM_COL32_WHITE : IM_COL32(14, 20, 28, 255), a8);
            dl()->AddLine(P(bx - arm, by), P(bx + arm, by), plus_c, S(2.6f));
            dl()->AddLine(P(bx, by - arm), P(bx, by + arm), plus_c, S(2.6f));

            text(x + s * 0.5f, y + s * 0.72f, s * 0.082f, with_alpha(p.text, a8), "Show more", 0.5f);
            std::string sub_m = total_library > 0
                ? (std::to_string(total_library) + (total_library == 1 ? " title · Custom" : " titles · Custom"))
                : "All games & custom";
            text(x + s * 0.5f, y + s * 0.845f, s * 0.056f, with_alpha(p.soft, a8), sub_m.c_str(), 0.5f);
        }
        if (f) { foc_x = x; foc_y = y; foc_s = s; has_foc = true; }
    };
    dl()->PushClipRect(P(0, row_y - 34), P(1280, row_y + size + 44), true);
    for (int i = 0; i < n; ++i) {
        if (g_zone == Z_ROW && i == pos) continue;
        draw_row_tile(i);
    }
    if (g_zone == Z_ROW && pos >= 0 && pos < n) draw_row_tile(pos);
    if (has_foc) {
        if (!g_ring_init || tmot == 0) {
            g_ring_x = foc_x; g_ring_y = foc_y; g_ring_s = foc_s;
            g_ring_init = true;
        } else {
            float spd = tmot == 2 ? 26.0f : 22.0f;
            g_ring_x = ease(g_ring_x, foc_x, spd);
            g_ring_y = ease(g_ring_y, foc_y, spd);
            g_ring_s = ease(g_ring_s, foc_s, spd);
        }
        // Crisp dark matte separator frame between artwork and luminous border (keeps prism border popping on bright art)
        frame(g_ring_x - 3.5f, g_ring_y - 3.5f, g_ring_s + 7.0f, g_ring_s + 7.0f,
              with_alpha(p.bg, p.light ? 190 : 230), g_ring_s * 0.065f + 3.5f, 5.5f);
        focus_ring(g_ring_x, g_ring_y, g_ring_s, g_ring_s, g_ring_s * 0.065f, 7.5f, foc_acc);
    }
    dl()->PopClipRect();

    // Focused title hovering directly centered above the highlighted icon (clamped safely inside screen edges)
    if (g_zone == Z_ROW && has_foc) {
        std::string title = fp ? std::string(fp->title) : std::string("Show more");
        ImU32 title_col = cfg().border_style == 0
                            ? (p.light ? IM_COL32(0, 134, 182, 255) : IM_COL32(84, 218, 246, 255))
                            : p.text;
        std::string fit_title = fit(26, title, 1160.0f);
        float tw = text_w(26, fit_title.c_str());
        float cx = g_ring_x + g_ring_s * 0.5f;
        float tx = SDL_clamp(cx - tw * 0.5f, 48.0f, 1232.0f - tw);
        text(tx, row_y - 62.0f, 26, title_col, fit_title.c_str());
    } else if (g_zone == Z_PROFILE) {
        std::string title, sub;
        if (g_prof_sel == prof_n) {
            title = "New profile", sub = "Add another local profile (up to 4)";
        } else if (g_prof_sel != cfg().active_profile) {
            title = cfg().profiles[g_prof_sel].name, sub = "A switches to this profile";
        } else {
            title = cfg().profile, sub = "Your name, icon and play activity";
        }
        std::string fit_title = fit(24, title, 640.0f);
        text(67, row_y - 62.0f, 24, p.text, fit_title.c_str());
        if (!sub.empty())
            text(67 + text_w(24, fit_title.c_str()) + 16, row_y - 56.0f, 16, p.soft,
                 fit(16, sub, 1100.0f - text_w(24, fit_title.c_str())).c_str());
    }

    if (cfg().cover_dir.empty() && cfg().port_covers.empty() && !demo_mode() && g_zone == Z_ROW && fp)
        text(96, row_y + size + 30, 15, p.dim, "Your own cover art goes on these tiles: System Settings > Cover art.");

    // the dock buttons (Floating Pill capsule with color-coded icons, or Classic Circles)
    int bn = builtin_port_count();
    int absent = 0, updates = 0;
    for (int k = 0; k < bn; ++k) {
        if (ps[k].state() == PORT_ABSENT && !install_queued(k)) ++absent;
        if (ps[k].state() == PORT_UPDATE && !install_queued(k)) ++updates;
    }
    if (pill_dock) {
        float px = dx0 - 18.0f, py = dock_y - 9.0f;
        float pw = kDockN * dsz + (kDockN - 1) * dgap + 36.0f, ph = dsz + 18.0f;
        rect(px, py + 5.0f, pw, ph, with_alpha(IM_COL32_BLACK, p.light ? 26 : 85), ph * 0.5f);
        rect(px, py, pw, ph, p.light ? IM_COL32(252, 252, 255, 248) : IM_COL32(42, 42, 47, 242), ph * 0.5f);
        frame(px, py, pw, ph, with_alpha(p.light ? IM_COL32(180, 184, 196, 255) : IM_COL32_WHITE, p.light ? 110 : 24), ph * 0.5f, 1.0f);
    }
    for (int i = 0; i < kDockN; ++i) {
        float cx = dx0 + i * dstep + dsz / 2.0f, cy = dock_y + dsz / 2.0f;
        bool f = g_zone == Z_DOCK && i == g_dock;
        ImU32 ic_col = p.text;
        ImU32 ring_col = p.accent;
        if (pill_dock) {
            ImU32 themed = p.light ? kDock[i].light_col : kDock[i].dark_col;
            if (kDock[i].featured && themed) {
                // Bold filled circular accent icon on the left end of the Floating Pill (Get ports)
                circle(cx, cy, dsz * (f && tmot > 0 ? 0.45f : 0.43f), themed);
                ic_col = IM_COL32_WHITE;
                ring_col = themed;
            } else {
                if (themed) { ic_col = themed; ring_col = themed; }
                else ring_col = p.text;
                if (f) circle(cx, cy, dsz * 0.44f, with_alpha(ic_col, p.light ? 30 : 38));
            }
        } else {
            if (kDock[i].featured) {
                ImU32 themed = p.light ? kDock[i].light_col : kDock[i].dark_col;
                circle(cx, cy + 3, dsz / 2, with_alpha(IM_COL32_BLACK, p.light ? 25 : 70));
                circle(cx, cy, dsz / 2, themed);
                ic_col = IM_COL32_WHITE;
                ring_col = themed;
            } else {
                circle(cx, cy + 3, dsz / 2, with_alpha(IM_COL32_BLACK, p.light ? 25 : 70));
                circle(cx, cy, dsz / 2, p.panel);
            }
        }
        icon(kDock[i].icon, cx, cy, f && tmot > 0 ? (kDock[i].featured ? 28.0f : 30.0f) : (kDock[i].featured ? 26.0f : 27.0f), ic_col);
        if (i == 0 && (updates > 0 || absent > 0)) {   // Get ports: updates or available count badge
            int badge = updates > 0 ? updates : absent;
            circle(cx + 21, cy - 21, 10.5f, updates > 0 ? p.ok : IM_COL32(66, 206, 246, 255));
            text(cx + 21, cy - 29.5f, 12.5f, updates > 0 ? IM_COL32_WHITE : IM_COL32(14, 22, 32, 255), std::to_string(badge).c_str(), 0.5f);
        }
        if (i == 1 && install_busy()) {   // Downloads: a ring fills with the running download
            float frac = -1;
            for (auto &j : install_jobs())
                if (j.phase == PH_DOWNLOAD && j.total) frac = (float)j.done / j.total;
            if (frac < 0) frac = (float)std::fmod(a.now * 0.5, 1.0);
            dl()->PathArcTo(P(cx, cy), S(27), -1.5708f, -1.5708f + 6.2832f * frac, 40);
            dl()->PathStroke(kDock[1].dark_col, 0, S(3.5f));
        }
        if (f) {
            float rr = pill_dock ? dsz * 0.46f : dsz * 0.5f;
            focus_ring(cx - rr, cy - rr, rr * 2.0f, rr * 2.0f, rr, pill_dock ? 3.5f : 5.0f, ring_col);
            text(cx, dock_y + dsz + (pill_dock ? 20.0f : 15.0f), 16,
                 cfg().border_style == 0 ? (p.light ? IM_COL32(0, 134, 182, 255) : IM_COL32(84, 218, 246, 255)) : p.text,
                 kDock[i].label, 0.5f);
        }
    }

    // hints
    if (g_zone == Z_ROW && fp) {
        if (!fp->installs.empty()) {
            const char *plbl = running_port() == a.game ? "Resume"
                             : can_play(*fp, *fp->best()) ? "Play" : "Set up";
            hint_bar({{NAV_Y, plbl}, {NAV_A, "Open"}, {NAV_X, "Quick menu"}, {NAV_PLUS, "Refresh"}});
        } else {
            hint_bar({{NAV_A, "Download"}, {NAV_X, "Quick menu"}, {NAV_PLUS, "Refresh"}});
        }
    } else {
        hint_bar({{NAV_A, "OK"}, {NAV_PLUS, "Refresh"}});
    }

    if (g_power) power_frame();
    if (g_quick) quick_frame();
}

}  // namespace sanctum
