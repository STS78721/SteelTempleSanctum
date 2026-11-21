// The player's profile: the icon on Home and every screen, picked from Sanctum's own artwork by category, and its
// background colour. Browsing previews the choice on the large icon at the left; A keeps it, B leaves it as it was.
#include <algorithm>
#include <vector>

#include "app.h"
#include "avatars.h"
#include "config.h"
#include "draw.h"

namespace sanctum {

static int g_tab = 0, g_sel = 0;
static float g_tab_x = 0;

static int tab_count() { return (int)avatar_categories().size() + 1; }   // the categories, then Colour
static bool colour_tab() { return g_tab == tab_count() - 1; }
static const char *tab_name(int t) {
    return t < (int)avatar_categories().size() ? avatar_categories()[t].c_str() : "Colour";
}

// the icons of a category, as atlas indices
static std::vector<int> icons_of(int t) {
    std::vector<int> out;
    if (t >= (int)avatar_categories().size()) return out;
    for (int i = 0; i < avatar_count(); ++i)
        if (avatar_category(i) == avatar_categories()[t]) out.push_back(i);
    return out;
}

static int items() { return colour_tab() ? (int)avatar_colours().size() : (int)icons_of(g_tab).size(); }
static int cols() { return colour_tab() ? 8 : 6; }

// where the selection should start on a page: on the player's own choice when it is there
static void land() {
    g_sel = 0;
    if (colour_tab()) { g_sel = cfg().avatar_bg; return; }
    auto v = icons_of(g_tab);
    int mine = avatar_find(cfg().avatar);
    for (int i = 0; i < (int)v.size(); ++i)
        if (v[i] == mine) g_sel = i;
}

void profile_open() {
    g_tab = 0;
    int mine = avatar_find(cfg().avatar);
    for (int t = 0; mine >= 0 && t < (int)avatar_categories().size(); ++t)
        if (avatar_categories()[t] == avatar_category(mine)) g_tab = t;
    land();
    go(SCR_PROFILE);
}

static void choose() {
    if (colour_tab()) {
        cfg().avatar_bg = g_sel;
        toast(std::string("Background: ") + avatar_colours()[g_sel].name);
    } else {
        auto v = icons_of(g_tab);
        if (g_sel >= (int)v.size()) return;
        cfg().avatar = avatar_name(v[g_sel]);
        toast("Icon: " + cfg().avatar);
    }
    config_save();
}

void profile_frame() {
    App &a = app();
    const Palette &p = pal();
    int n = items(), c = cols();

    switch (a.nav) {
    case NAV_B: go_back(); return;
    case NAV_L: g_tab = (g_tab + tab_count() - 1) % tab_count(); land(); break;
    case NAV_R: g_tab = (g_tab + 1) % tab_count(); land(); break;
    case NAV_LEFT: if (g_sel > 0) g_sel--; break;
    case NAV_RIGHT: if (g_sel < n - 1) g_sel++; break;
    case NAV_UP: if (g_sel >= c) g_sel -= c; break;
    case NAV_DOWN: if (g_sel + c < n) g_sel += c; else if (g_sel / c < (n - 1) / c) g_sel = n - 1; break;
    case NAV_A: choose(); break;
    default: break;
    }
    n = items(), c = cols();
    g_sel = std::min(std::max(g_sel, 0), std::max(0, n - 1));

    // what the large icon shows: the one under the selection
    int show = avatar_find(cfg().avatar), bg = cfg().avatar_bg;
    std::vector<int> v = icons_of(g_tab);
    if (colour_tab()) bg = g_sel;
    else if (g_sel < (int)v.size()) show = v[g_sel];

    background(avatar_colours()[bg].col);
    status_bar();
    text(96, 92, 26, p.text, "Profile");

    // left: the preview
    rect(96, 140, 340, 496, p.panel, 22);
    avatar_draw(show, bg, 266, 300, 112);
    text(266, 436, 26, p.text, fit(26, cfg().profile, 300).c_str(), 0.5f);
    std::string what = (show >= 0 ? avatar_name(show) : std::string("No icon")) + "  ·  " + avatar_colours()[bg].name;
    text(266, 474, 16, p.soft, what.c_str(), 0.5f);
    bool same = show == avatar_find(cfg().avatar) && bg == cfg().avatar_bg;
    text(266, 580, 14, p.dim, same ? "Your icon on Home and every screen" : "A keeps this one", 0.5f);

    // right: the pages
    float x0 = 486, tx = x0;
    for (int t = 0; t < tab_count(); ++t) {
        float w = text_w(17, tab_name(t)) + 32;
        if (a.clicked && hover(tx, 140, w, 40)) { g_tab = t; land(); }
        text(tx + 16, 148, 17, t == g_tab ? p.text : p.dim, tab_name(t));
        if (t == g_tab) g_tab_x = ease(g_tab_x ? g_tab_x : tx, tx, 20);
        tx += w;
    }
    rect(x0, 182, 750, 1, p.line);
    rect(g_tab_x + 12, 179, text_w(17, tab_name(g_tab)) + 8, 4, avatar_colours()[bg].col, 2);

    n = items(), c = cols();
    v = icons_of(g_tab);
    float cell = colour_tab() ? 92 : 122, r = colour_tab() ? 36 : 50;
    float gx = x0 + (750 - c * cell) / 2, gy = 210;
    int mine_icon = avatar_find(cfg().avatar);
    for (int i = 0; i < n; ++i) {
        float cx = gx + (i % c) * cell + cell / 2, cy = gy + (i / c) * cell + cell / 2;
        if (hover(cx - r, cy - r, 2 * r, 2 * r)) {
            if (a.mouse_moved) g_sel = i;
            if (a.clicked) { g_sel = i; choose(); }
        }
        bool focused = i == g_sel;
        float rr = r * (focused ? 1.06f : 1.0f);
        bool chosen;
        if (colour_tab()) {
            avatar_draw(mine_icon, i, cx, cy, rr);
            chosen = i == cfg().avatar_bg;
        } else {
            avatar_draw(v[i], cfg().avatar_bg, cx, cy, rr);
            chosen = v[i] == mine_icon;
        }
        if (chosen) {   // the one in use: a tick at the lower right
            circle(cx + r * 0.72f, cy + r * 0.72f, 13, p.bg);
            circle(cx + r * 0.72f, cy + r * 0.72f, 11, p.ok);
            icon(IC_CHECK, cx + r * 0.72f, cy + r * 0.72f, 14, IM_COL32_WHITE);
        }
        if (focused) focus_ring(cx - rr, cy - rr, 2 * rr, 2 * rr, rr, 5);
    }
    if (!colour_tab() && n)
        text(x0 + 375, 618, 15, p.soft, (std::to_string(n) + " icons  ·  " + std::to_string(avatar_count()) +
                                         " in all, on " + std::to_string(avatar_colours().size()) + " colours").c_str(), 0.5f);
    if (!avatar_count()) text(x0 + 375, 300, 17, p.soft, "The icons are missing: assets\\avatars.png", 0.5f);

    hint_bar({{NAV_A, "Choose"}, {NAV_L, "Page"}, {NAV_B, "Back"}});
}

}  // namespace sanctum
