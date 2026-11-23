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
};
static const DockItem kDock[] = {
    {IC_NEWS, "Releases & news"}, {IC_STORE, "Get ports"}, {IC_DOWNLOAD, "Downloads"}, {IC_ALBUM, "Album"},
    {IC_MODS, "Mods"}, {IC_PAD, "Controllers"}, {IC_GEAR, "System Settings"}, {IC_POWER, "Power"},
};
static const int kDockN = sizeof kDock / sizeof kDock[0];

static int g_zone = Z_ROW, g_dock = 0, g_prof_sel = 0;
static float g_scroll = 0, g_tint_t = 0;
static ImU32 g_tint = 0;
static float g_pop[64];             // each tile's grow when focused
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

static void open_dock(int i) {
    switch (i) {
    case 0: go(SCR_NEWS); break;
    case 1: go(SCR_PORTS); break;
    case 2: go(SCR_DOWNLOADS); break;
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
    int n = (int)ps.size() + 1;   // the ports and "Get ports"
    if (a.game >= n) a.game = n - 1;
    // the row's order: pinned first, then installed (by cfg().home_sort: Last played, Most played, A-Z), then absent, then Get ports
    std::vector<int> order;
    for (int i = 0; i < (int)ps.size(); ++i) order.push_back(i);
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
    order.push_back((int)ps.size());
    int pos = 0;
    for (int i = 0; i < n; ++i)
        if (order[i] == a.game) pos = i;

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
        // ---- input
        switch (a.nav) {
        case NAV_LEFT:
            if (g_zone == Z_PROFILE) {
                if (g_prof_sel > 0) { g_prof_sel--; sound_play(SFX_MOVE); }
                else sound_play(SFX_EDGE);
            } else if (g_zone == Z_ROW) {
                if (pos > 0) { a.game = order[--pos]; sound_play(SFX_MOVE); }
                else sound_play(SFX_EDGE);
            } else if (g_zone == Z_DOCK) {
                if (g_dock > 0) { g_dock--; sound_play(SFX_MOVE); }
                else sound_play(SFX_EDGE);
            }
            break;
        case NAV_RIGHT:
            if (g_zone == Z_PROFILE) {
                if (g_prof_sel < prof_slots - 1) { g_prof_sel++; sound_play(SFX_MOVE); }
                else sound_play(SFX_EDGE);
            } else if (g_zone == Z_ROW) {
                if (pos < n - 1) { a.game = order[++pos]; sound_play(SFX_MOVE); }
                else sound_play(SFX_EDGE);
            } else if (g_zone == Z_DOCK) {
                if (g_dock < kDockN - 1) { g_dock++; sound_play(SFX_MOVE); }
                else sound_play(SFX_EDGE);
            }
            break;
        case NAV_UP:
            if (g_zone > Z_PROFILE) {
                g_zone--;
                if (g_zone == Z_PROFILE) g_prof_sel = cfg().active_profile;
                sound_play(SFX_MOVE);
            } else sound_play(SFX_EDGE);
            break;
        case NAV_DOWN:
            if (g_zone < Z_DOCK) { g_zone++; sound_play(SFX_MOVE); }
            else sound_play(SFX_EDGE);
            break;
        case NAV_A:
            if (g_zone == Z_PROFILE) pick_profile(g_prof_sel);
            if (g_zone == Z_ROW) {
                if (a.game < (int)ps.size() && (!ps[a.game].installs.empty() || install_queued(a.game))) go(SCR_GAME);
                else if (a.game < (int)ps.size()) get_ports_select(a.game);   // not on this PC: straight to getting it
                else go(SCR_PORTS);
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

    // dock geometry (8 clean round buttons, centered)
    float dsz = 66, dgap = 22, dstep = dsz + dgap;
    float dx0 = 640 - (kDockN * dsz + (kDockN - 1) * dgap) / 2.0f;

    // mouse: hover focuses, click opens
    if (!g_power && !g_quick) {
        for (int i = 0; i < prof_slots; ++i) {
            float cx = 67 + i * 58, cy = 46;
            if (hover(cx - 23, cy - 23, 46, 46)) {
                if (a.mouse_moved && !a.clicked && (g_zone != Z_PROFILE || g_prof_sel != i)) {
                    g_zone = Z_PROFILE, g_prof_sel = i;
                    sound_play(SFX_MOVE);
                }
                if (a.clicked) { g_zone = Z_PROFILE, g_prof_sel = i; pick_profile(i); }
            }
        }
        for (int i = 0; i < n; ++i) {
            float x = 96 + i * (size + gap) - g_scroll;
            if (hover(x, row_y, size, size)) {
                if (a.mouse_moved && !a.clicked && (g_zone != Z_ROW || a.game != order[i])) {
                    g_zone = Z_ROW, a.game = order[i], pos = i;
                    sound_play(SFX_MOVE);
                }
                if (a.clicked) {
                    g_zone = Z_ROW, a.game = order[i], pos = i;
                    if (order[i] < (int)ps.size()) go(SCR_GAME);
                    else go(SCR_PORTS);
                }
            }
        }
        for (int i = 0; i < kDockN; ++i)
            if (hover(dx0 + i * dstep, 490, dsz, dsz)) {
                if (a.mouse_moved && !a.clicked && (g_zone != Z_DOCK || g_dock != i)) {
                    g_zone = Z_DOCK, g_dock = i;
                    sound_play(SFX_MOVE);
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

    // the focused title above the row
    std::string title, sub;
    if (g_zone == Z_ROW) {
        if (fp) {
            title = fp->title;
            PortState st = fp->state();
            const Install *b = fp->best();
            if (fp->custom) {
                sub = "Custom PC Game";
            } else {
                sub = std::string("SteelTemple ") + fp->name + "  ·  " +
                      (st == PORT_ABSENT ? std::string("not installed  ·  ") + fp->latest_tag()
                       : st == PORT_UPDATE ? b->version + "  ·  update " + fp->latest_tag() + " available"
                       : b->version.empty() ? std::string("installed") : b->version);
                if (fp->installs.size() > 1) sub += "  ·  " + std::to_string(fp->installs.size()) + " copies found";
            }
            if (running_port() == a.game) sub += "  ·  playing now";
            else if (!played_text(a.game).empty()) sub += "  ·  " + played_text(a.game);
        } else {
            title = "Get ports", sub = "Choose which ports to download or update";
        }
    } else if (g_zone == Z_DOCK) {
        title = kDock[g_dock].label;
    } else if (g_prof_sel == prof_n) {
        title = "New profile", sub = "Add another local profile (up to 4)";
    } else if (g_prof_sel != cfg().active_profile) {
        title = cfg().profiles[g_prof_sel].name, sub = "A switches to this profile";
    } else {
        title = cfg().profile, sub = "Your name, icon and play activity";
    }
    text(96, row_y - 54, 27, p.text, fit(27, title, 700).c_str());
    if (!sub.empty()) text(96 + text_w(27, fit(27, title, 700).c_str()) + 16, row_y - 46, 16, p.soft, fit(16, sub, 1100 - text_w(27, title.c_str())).c_str());

    // the row (clipped so tiles slide under the screen edge)
    dl()->PushClipRect(P(0, row_y - 30), P(1280, row_y + size + 40), true);
    for (int i = 0; i < n; ++i) {
        int id = order[i];
        bool f = g_zone == Z_ROW && i == pos;
        int pop_i = i < 64 ? i : 63;
        g_pop[pop_i] = ease(g_pop[pop_i], f ? 1.0f : 0.0f, 18);
        float grow = 1 + 0.07f * g_pop[pop_i];
        float s = size * grow;
        float x = 96 + i * (size + gap) - g_scroll - (s - size) / 2, y = row_y - (s - size) / 2;
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
        } else {   // Get ports
            rect(x, y, s, s, with_alpha(p.panel2, (int)(255 * intro)), s * 0.065f);
            icon(IC_DOWNLOAD, x + s / 2, y + s * 0.4f, s * 0.24f, p.text);
            text(x + s / 2, y + s * 0.62f, s * 0.08f, p.soft, "Get ports", 0.5f);
        }
        if (f) focus_ring(x, y, s, s, s * 0.065f, 8);
    }
    dl()->PopClipRect();

    if (cfg().cover_dir.empty() && cfg().port_covers.empty() && !demo_mode() && g_zone == Z_ROW && fp)
        text(96, row_y + size + 30, 15, p.dim, "Your own cover art goes on these tiles: System Settings > Cover art.");

    // the round buttons
    int bn = builtin_port_count();
    int absent = 0, updates = 0;
    for (int k = 0; k < bn; ++k) {
        if (ps[k].state() == PORT_ABSENT && !install_queued(k)) ++absent;
        if (ps[k].state() == PORT_UPDATE && !install_queued(k)) ++updates;
    }
    for (int i = 0; i < kDockN; ++i) {
        float cx = dx0 + i * dstep + dsz / 2, cy = 490 + dsz / 2;
        bool f = g_zone == Z_DOCK && i == g_dock;
        circle(cx, cy + 3, dsz / 2, with_alpha(IM_COL32_BLACK, p.light ? 25 : 70));
        circle(cx, cy, dsz / 2, p.panel);
        icon(kDock[i].icon, cx, cy, 28, p.text);
        if (i == 1 && (updates > 0 || absent > 0)) {   // Get ports: updates or available count
            int badge = updates > 0 ? updates : absent;
            circle(cx + 23, cy - 23, 11, updates > 0 ? p.ok : p.accent);
            text(cx + 23, cy - 32, 13, IM_COL32_WHITE, std::to_string(badge).c_str(), 0.5f);
        }
        if (i == 2 && install_busy()) {   // Downloads: a ring fills with the running download
            float frac = -1;
            for (auto &j : install_jobs())
                if (j.phase == PH_DOWNLOAD && j.total) frac = (float)j.done / j.total;
            if (frac < 0) frac = (float)std::fmod(a.now * 0.5, 1.0);
            dl()->PathArcTo(P(cx, cy), S(29), -1.5708f, -1.5708f + 6.2832f * frac, 40);
            dl()->PathStroke(p.accent, 0, S(4));
        }
        if (f) {
            focus_ring(cx - dsz / 2, cy - dsz / 2, dsz, dsz, dsz / 2, 5);
            text(cx, cy + 48, 16, p.text, kDock[i].label, 0.5f);
        }
    }

    // hints
    if (g_zone == Z_ROW && fp)
        hint_bar({{NAV_A, fp->state() == PORT_ABSENT ? "Download" : "Open"}, {NAV_X, "Quick menu"}, {NAV_PLUS, "Refresh"}});
    else
        hint_bar({{NAV_A, "OK"}, {NAV_PLUS, "Refresh"}});

    if (g_power) power_frame();
    if (g_quick) quick_frame();
}

}  // namespace sanctum
