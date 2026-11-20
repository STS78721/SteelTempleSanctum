// Home: the profile at the top left, the row of ports, the round buttons below, the hints along the bottom.
// Focus lives in one of three zones (profile, row, buttons); up and down move between them, left and right inside.
#include <algorithm>
#include <cmath>

#include "config.h"
#include "covers.h"
#include "draw.h"
#include "github.h"
#include "install.h"
#include "launch.h"

namespace sanctum {

enum { Z_PROFILE, Z_ROW, Z_DOCK };

struct DockItem {
    Icon icon;
    const char *label;
};
static const DockItem kDock[] = {
    {IC_NEWS, "Releases & news"}, {IC_STORE, "Get ports"}, {IC_DOWNLOAD, "Downloads"}, {IC_MODS, "Mods"}, {IC_PAD, "Controllers"},
    {IC_GEAR, "System Settings"}, {IC_POWER, "Power"},
};
static const int kDockN = sizeof kDock / sizeof kDock[0];

static int g_zone = Z_ROW, g_dock = 0;
static float g_scroll = 0, g_tint_t = 0;
static ImU32 g_tint = 0;
static float g_pop[16];             // each tile's grow when focused
static bool g_power = false;        // the Power menu is open
static int g_power_sel = 0;

static void open_dock(int i) {
    switch (i) {
    case 0: go(SCR_NEWS); break;
    case 1: go(SCR_PORTS); break;
    case 2: go(SCR_DOWNLOADS); break;
    case 3: app().soon_title = "Mods"; go(SCR_SOON); break;
    case 4: go(SCR_CONTROLLERS); break;
    case 5: go(SCR_SETTINGS); break;
    case 6:   // the press that opens the menu must not also pick in it
        g_power = true, g_power_sel = 1;   // Cancel is the safe default
        app().nav = NAV_NONE, app().clicked = false;
        break;
    }
}

static void power_frame() {
    App &a = app();
    const Palette &p = pal();
    const char *items[] = {install_busy() ? "Close Sanctum (stops the downloads)" : "Close SteelTemple Sanctum", "Cancel"};
    if (a.nav == NAV_UP || a.nav == NAV_DOWN) g_power_sel ^= 1;
    for (int i = 0; i < 2; ++i)
        if (a.mouse_moved && hover(470, 330 + i * 64, 340, 54)) g_power_sel = i;
    bool pick = a.nav == NAV_A || (a.clicked && hover(470, 330 + g_power_sel * 64, 340, 54));
    if (pick && g_power_sel == 0) a.quit = true;
    if ((pick && g_power_sel == 1) || a.nav == NAV_B || (a.clicked && !hover(430, 240, 420, 250))) g_power = false;
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

void home_frame() {
    App &a = app();
    const Palette &p = pal();
    auto &ps = ports();
    int n = (int)ps.size() + 1;   // the ports and "Get ports"
    if (a.game >= n) a.game = n - 1;
    // the row's order, like the console's: the ports played last first, then the rest installed, then the ones not on
    // this PC, then Get ports. a.game is a port (ps.size() for Get ports); pos is where it sits in the row.
    std::vector<int> order;
    for (int i = 0; i < (int)ps.size(); ++i) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](int x, int y) {
        bool hx = !ps[x].installs.empty(), hy = !ps[y].installs.empty();
        if (hx != hy) return hx;
        return last_played(x) > last_played(y);
    });
    order.push_back((int)ps.size());
    int pos = 0;
    for (int i = 0; i < n; ++i)
        if (order[i] == a.game) pos = i;

    if (!g_power) {
        // ---- input
        switch (a.nav) {
        case NAV_LEFT:
            if (g_zone == Z_ROW && pos > 0) a.game = order[--pos];
            if (g_zone == Z_DOCK && g_dock > 0) g_dock--;
            break;
        case NAV_RIGHT:
            if (g_zone == Z_ROW && pos < n - 1) a.game = order[++pos];
            if (g_zone == Z_DOCK && g_dock < kDockN - 1) g_dock++;
            break;
        case NAV_UP: if (g_zone > Z_PROFILE) g_zone--; break;
        case NAV_DOWN: if (g_zone < Z_DOCK) g_zone++; break;
        case NAV_A:
            if (g_zone == Z_PROFILE) { a.soon_title = "Profiles"; go(SCR_SOON); }
            if (g_zone == Z_ROW) {
                if (a.game < (int)ps.size() && (!ps[a.game].installs.empty() || install_queued(a.game))) go(SCR_GAME);
                else if (a.game < (int)ps.size()) get_ports_select(a.game);   // not on this PC: straight to getting it
                else go(SCR_PORTS);
            }
            if (g_zone == Z_DOCK) open_dock(g_dock);
            break;
        case NAV_X:
            if (g_zone == Z_ROW && a.game < (int)ps.size()) go(SCR_GAME);
            break;
        case NAV_PLUS:   // look again: this PC and GitHub
            if (!scan_running()) scan_start();
            github_refresh();
            toast("Looking for ports on this PC and new releases on GitHub");
            break;
        case NAV_B: if (g_zone != Z_ROW) g_zone = Z_ROW; break;
        default: break;
        }
    }

    // ---- layout
    float size = cfg().large_tiles ? 236 : 196, gap = 18;
    float row_y = cfg().large_tiles ? 170 : 190;
    // keep the focused tile in view: the row scrolls once focus passes the fourth tile
    int visible = (int)((1280 - 96 - 60) / (size + gap));
    float want = std::max(0.0f, (float)(pos - (visible - 2))) * (size + gap);
    want = std::min(want, std::max(0.0f, n * (size + gap) - (1280 - 96 - 60)));
    g_scroll = ease(g_scroll, want, 14);

    // mouse: hover focuses, click opens
    if (!g_power) {
        if (hover(52, 23, 46, 46) && (a.mouse_moved || a.clicked)) {
            g_zone = Z_PROFILE;
            if (a.clicked) { a.soon_title = "Profiles"; go(SCR_SOON); }
        }
        for (int i = 0; i < n; ++i) {
            float x = 96 + i * (size + gap) - g_scroll;
            if (hover(x, row_y, size, size) && (a.mouse_moved || a.clicked)) {
                g_zone = Z_ROW, a.game = order[i], pos = i;
                if (a.clicked) { if (order[i] < (int)ps.size()) go(SCR_GAME); else go(SCR_PORTS); }
            }
        }
        float dx0 = 640 - (kDockN * 70 + (kDockN - 1) * 26) / 2.0f;
        for (int i = 0; i < kDockN; ++i)
            if (hover(dx0 + i * 96, 488, 70, 70) && (a.mouse_moved || a.clicked)) {
                g_zone = Z_DOCK, g_dock = i;
                if (a.clicked) open_dock(i);
            }
    }

    // ---- draw: the backdrop takes the focused port's colour
    const Port *fp = g_zone == Z_ROW && a.game < (int)ps.size() ? &ps[a.game] : nullptr;
    ImU32 want_tint = cfg().follow && fp ? fp->colours.accent : p.accent;
    if (want_tint != g_tint) { g_tint_t = 0; }
    g_tint_t = std::min(1.0f, g_tint_t + a.dt * 4);
    g_tint = mix(g_tint ? g_tint : want_tint, want_tint, g_tint_t);
    background(g_tint);
    status_bar();
    if (g_zone == Z_PROFILE) focus_ring(44, 23, 46, 46, 23, 4);

    // the focused title above the row
    std::string title, sub;
    if (g_zone == Z_ROW) {
        if (fp) {
            title = fp->title;
            PortState st = fp->state();
            const Install *b = fp->best();
            sub = std::string("SteelTemple ") + fp->name + "  ·  " +
                  (st == PORT_ABSENT ? std::string("not installed  ·  ") + fp->latest_tag()
                   : st == PORT_UPDATE ? b->version + "  ·  update " + fp->latest_tag() + " available"
                   : b->version.empty() ? std::string("installed") : b->version);
            if (fp->installs.size() > 1) sub += "  ·  " + std::to_string(fp->installs.size()) + " copies found";
            if (running_port() == a.game) sub += "  ·  playing now";
            else if (!played_text(a.game).empty()) sub += "  ·  " + played_text(a.game);
        } else {
            title = "Get ports", sub = "Choose which ports to download";
        }
    } else if (g_zone == Z_DOCK) {
        title = kDock[g_dock].label;
    } else {
        title = cfg().profile, sub = "Profiles";
    }
    text(96, row_y - 54, 27, p.text, fit(27, title, 700).c_str());
    if (!sub.empty()) text(96 + text_w(27, fit(27, title, 700).c_str()) + 16, row_y - 46, 16, p.soft, fit(16, sub, 1100 - text_w(27, title.c_str())).c_str());

    // the row (clipped so tiles slide under the screen edge)
    dl()->PushClipRect(P(0, row_y - 30), P(1280, row_y + size + 40), true);
    for (int i = 0; i < n; ++i) {
        int id = order[i];
        bool f = g_zone == Z_ROW && i == pos;
        g_pop[i] = ease(g_pop[i], f ? 1.0f : 0.0f, 18);
        float grow = 1 + 0.07f * g_pop[i];
        float s = size * grow;
        float x = 96 + i * (size + gap) - g_scroll - (s - size) / 2, y = row_y - (s - size) / 2;
        // the start: tiles rise in one after another
        float intro = std::min(1.0f, std::max(0.0f, (float)(a.now - 0.08 * i) * 3.2f));
        y += (1 - intro) * (1 - intro) * 40;
        if (id < (int)ps.size()) {
            tile(ps[id], x, y, s, intro, true, g_pop[i]);
            if (running_port() == id) {   // the game in play, as the console marks a suspended one
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

    if (cfg().cover_dir.empty() && g_zone == Z_ROW && fp)
        text(96, row_y + size + 30, 15, p.dim, "Your own cover art goes on these tiles: System Settings > Cover art.");

    // the round buttons
    float dx0 = 640 - (kDockN * 70 + (kDockN - 1) * 26) / 2.0f;
    for (int i = 0; i < kDockN; ++i) {
        float cx = dx0 + i * 96 + 35, cy = 523;
        bool f = g_zone == Z_DOCK && i == g_dock;
        circle(cx, cy + 3, 35, with_alpha(IM_COL32_BLACK, p.light ? 25 : 70));
        circle(cx, cy, 35, p.panel);
        icon(kDock[i].icon, cx, cy, 30, p.text);
        if (i == 1) {   // Get ports: how many ports are not on this PC yet
            int absent = 0;
            for (int k = 0; k < (int)ps.size(); ++k) absent += ps[k].state() == PORT_ABSENT && !install_queued(k);
            if (absent) {
                circle(cx + 25, cy - 25, 11, p.accent);
                text(cx + 25, cy - 34, 14, IM_COL32_WHITE, std::to_string(absent).c_str(), 0.5f);
            }
        }
        if (i == 2 && install_busy()) {   // Downloads: a ring fills with the running download
            float frac = -1;
            for (auto &j : install_jobs())
                if (j.phase == PH_DOWNLOAD && j.total) frac = (float)j.done / j.total;
            if (frac < 0) frac = (float)std::fmod(a.now * 0.5, 1.0);
            dl()->PathArcTo(P(cx, cy), S(31), -1.5708f, -1.5708f + 6.2832f * frac, 40);
            dl()->PathStroke(p.accent, 0, S(4));
        }
        if (f) {
            focus_ring(cx - 35, cy - 35, 70, 70, 35, 5);
            text(cx, cy + 50, 16, p.text, kDock[i].label, 0.5f);
        }
    }

    // hints
    if (g_zone == Z_ROW && fp)
        hint_bar({{NAV_A, fp->state() == PORT_ABSENT ? "Download" : "Start"}, {NAV_X, "Options"}, {NAV_PLUS, "Check for ports"}});
    else
        hint_bar({{NAV_A, "OK"}, {NAV_PLUS, "Check for ports"}});

    if (g_power) power_frame();
}

}  // namespace sanctum
