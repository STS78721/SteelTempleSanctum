// The player's profile: the icon on Home and every screen, picked from Sanctum's own artwork by category, and its
// background colour. Browsing previews the choice on the large icon at the left; A keeps it, B leaves it as it was.
#include <algorithm>
#include <cmath>
#include <vector>

#include "app.h"
#include "avatars.h"
#include "catalog.h"
#include "config.h"
#include "draw.h"
#include "launch.h"
#include "sound.h"

namespace sanctum {

static int g_tab = 0, g_sel = 0, g_act_sel = 0;
static float g_tab_x = 0;
static double g_del_armed = -10;

static int tab_count() { return (int)avatar_categories().size() + 2; }   // categories, Colour, Activity
static bool colour_tab() { return g_tab == (int)avatar_categories().size(); }
static bool activity_tab() { return g_tab == (int)avatar_categories().size() + 1; }
static const char *tab_name(int t) {
    int nc = (int)avatar_categories().size();
    return t < nc ? avatar_categories()[t].c_str() : t == nc ? "Colour" : "Activity";
}

static std::string fmt_duration(long long secs) {
    if (secs <= 0) return "0 min";
    if (secs < 60) return "under 1 min";
    long long m = secs / 60;
    if (m < 60) return std::to_string(m) + " min";
    return std::to_string(m / 60) + " h " + std::to_string(m % 60) + " min";
}

static long long total_playtime_seconds(int *ports_played = nullptr) {
    long long tot = 0;
    int cnt = 0;
    for (int i = 0; i < (int)ports().size(); ++i) {
        long long s = played_seconds(i);
        if (s > 0) tot += s, ++cnt;
    }
    if (ports_played) *ports_played = cnt;
    return tot;
}

// the icons of a category, as atlas indices
static std::vector<int> icons_of(int t) {
    std::vector<int> out;
    if (t >= (int)avatar_categories().size()) return out;
    for (int i = 0; i < avatar_count(); ++i)
        if (avatar_category(i) == avatar_categories()[t]) out.push_back(i);
    return out;
}

static int items() { return activity_tab() ? 0 : colour_tab() ? (int)avatar_colours().size() : (int)icons_of(g_tab).size(); }
static int cols() { return colour_tab() ? 8 : 6; }

// where the selection should start on a page: on the player's own choice when it is there
static void land() {
    g_sel = 0;
    g_act_sel = 0;
    if (activity_tab()) return;
    if (colour_tab()) { g_sel = cfg().avatar_bg; return; }
    auto v = icons_of(g_tab);
    int mine = avatar_find(cfg().avatar);
    for (int i = 0; i < (int)v.size(); ++i)
        if (v[i] == mine) g_sel = i;
}

void profile_open() {
    g_tab = 0;
    g_del_armed = -10;
    int mine = avatar_find(cfg().avatar);
    for (int t = 0; mine >= 0 && t < (int)avatar_categories().size(); ++t)
        if (avatar_categories()[t] == avatar_category(mine)) g_tab = t;
    land();
    go(SCR_PROFILE);
}

static void choose() {
    if (activity_tab()) return;
    if (colour_tab()) {
        cfg().avatar_bg = g_sel;
        toast(std::string("Background: ") + avatar_colours()[g_sel].name);
    } else {
        auto v = icons_of(g_tab);
        if (g_sel >= (int)v.size()) return;
        cfg().avatar = avatar_name(v[g_sel]);
        toast("Icon: " + cfg().avatar);
    }
    sound_play(SFX_TOGGLE_ON);
    config_save();
}

// ======================= the name: an on-screen keyboard (a pad types key by key; a keyboard types straight in)
static const int kMaxName = 16;
static bool g_kb = false, g_shift = true;   // starts with a capital, then lower case (like the console)
static int g_kx = 0, g_ky = 0;
static std::string g_name;
enum { K_CHAR, K_SHIFT, K_SPACE, K_BACK, K_CANCEL, K_OK };
struct Key { int row, col, w, kind; char ch; };
static const char *kRows[] = {"1234567890-", "qwertyuiop'", "asdfghjkl._", "zxcvbnm!?&"};

static std::vector<Key> keys() {
    std::vector<Key> k;
    for (int r = 0; r < 4; ++r)
        for (int i = 0; kRows[r][i]; ++i) k.push_back({r, i, 1, K_CHAR, kRows[r][i]});
    k.push_back({3, 10, 1, K_BACK, 0});
    k.push_back({4, 0, 2, K_SHIFT, 0});
    k.push_back({4, 2, 4, K_SPACE, ' '});
    k.push_back({4, 6, 2, K_CANCEL, 0});
    k.push_back({4, 8, 3, K_OK, 0});
    return k;
}

static int name_len(const std::string &s) {   // in characters (UTF-8)
    int n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}
static void name_pop() {
    while (!g_name.empty() && ((unsigned char)g_name.back() & 0xC0) == 0x80) g_name.pop_back();
    if (!g_name.empty()) g_name.pop_back();
}
static void name_add(const std::string &utf8) {
    // only what the font has (Latin-1), and no control characters
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = utf8[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        unsigned cp = len == 1 ? c : len == 2 ? ((c & 0x1F) << 6) | (utf8[i + 1] & 0x3F) : 0xFFFF;
        if (cp >= 0x20 && cp != 0x7F && cp != '|' && cp <= 0xFF && name_len(g_name) < kMaxName) g_name += utf8.substr(i, len);
        i += len;
    }
}

static void kb_open() {
    g_kb = true, g_name = cfg().profile, g_shift = g_name.empty(), g_kx = 0, g_ky = 1;
    app().text_entry = true;
    SDL_StartTextInput((::SDL_Window *)app().window);
    sound_play(SFX_OK);
}
static void kb_close(bool keep) {
    std::string n = g_name;
    while (!n.empty() && n.back() == ' ') n.pop_back();
    while (!n.empty() && n.front() == ' ') n.erase(0, 1);
    if (keep && !n.empty() && n != cfg().profile) {
        cfg().profile = n;
        config_save();
        toast("Name: " + n);
    }
    g_kb = false;
    app().text_entry = false;
    SDL_StopTextInput((::SDL_Window *)app().window);
}

static void kb_press(const Key &k) {
    switch (k.kind) {
    case K_CHAR: {
        char ch = g_shift && k.ch >= 'a' && k.ch <= 'z' ? (char)(k.ch - 32) : k.ch;
        name_add(std::string(1, ch));
        if (g_shift && name_len(g_name) == 1) g_shift = false;   // after the first capital
        sound_play(SFX_KEY);
        break;
    }
    case K_SHIFT: g_shift = !g_shift; sound_play(SFX_TAB); break;
    case K_SPACE: name_add(" "); sound_play(SFX_KEY); break;
    case K_BACK: name_pop(); sound_play(SFX_BACK); break;
    case K_CANCEL: sound_play(SFX_BACK); kb_close(false); break;
    case K_OK: sound_play(SFX_OK); kb_close(true); break;
    }
}

// the key under (g_kx, g_ky): wide keys cover several columns
static int kb_at(const std::vector<Key> &ks, int row, int col) {
    for (int i = 0; i < (int)ks.size(); ++i)
        if (ks[i].row == row && col >= ks[i].col && col < ks[i].col + ks[i].w) return i;
    int best = -1;   // a short row: its last key
    for (int i = 0; i < (int)ks.size(); ++i)
        if (ks[i].row == row && (best < 0 || ks[i].col > ks[best].col)) best = i;
    return best;
}

static void kb_frame() {
    App &a = app();
    const Palette &p = pal();
    auto ks = keys();
    // a physical keyboard
    if (!a.typed.empty()) { name_add(a.typed); g_shift = false; sound_play(SFX_KEY); }
    if (a.key_back) { name_pop(); sound_play(SFX_BACK); }
    if (a.key_enter) { sound_play(SFX_OK); kb_close(true); return; }
    if (a.key_escape) { sound_play(SFX_BACK); kb_close(false); return; }
    // the pad
    int cur = kb_at(ks, g_ky, g_kx);
    int pkx = g_kx, pky = g_ky;
    switch (a.nav) {
    case NAV_LEFT: if (ks[cur].col > 0) g_kx = ks[cur].col - 1; else g_kx = 10; sound_play(g_kx != pkx ? SFX_MOVE : SFX_EDGE); break;
    case NAV_RIGHT: g_kx = ks[cur].col + ks[cur].w <= 10 ? ks[cur].col + ks[cur].w : 0; sound_play(g_kx != pkx ? SFX_MOVE : SFX_EDGE); break;
    case NAV_UP: g_ky = (g_ky + 4) % 5; sound_play(g_ky != pky ? SFX_MOVE : SFX_EDGE); break;
    case NAV_DOWN: g_ky = (g_ky + 1) % 5; sound_play(g_ky != pky ? SFX_MOVE : SFX_EDGE); break;
    case NAV_A: kb_press(ks[cur]); break;
    case NAV_B: sound_play(SFX_BACK); if (g_name.empty()) kb_close(false); else name_pop(); break;   // B deletes; empty, it leaves
    case NAV_X: g_shift = !g_shift; sound_play(SFX_TAB); break;
    case NAV_Y: name_add(" "); sound_play(SFX_KEY); break;
    case NAV_PLUS: sound_play(SFX_OK); kb_close(true); break;
    default: break;
    }
    if (!g_kb) return;
    cur = kb_at(ks, g_ky, g_kx);

    // draw: the page dims, the keyboard rises from the bottom
    rect(0, 0, 1280, 720, with_alpha(IM_COL32_BLACK, 150));
    rect(150, 96, 980, 548, p.panel, 24);
    text(190, 124, 22, p.text, "Your name");
    text(1090, 128, 15, p.dim, (std::to_string(name_len(g_name)) + " / " + std::to_string(kMaxName)).c_str(), 1);
    rect(190, 162, 900, 64, p.panel2, 14);
    float tw = text(214, 178, 26, p.text, g_name.empty() ? "" : g_name.c_str());
    if (g_name.empty()) text(214, 178, 26, p.dim, "Type a name");
    if (std::fmod(a.now, 1.0) < 0.6) rect(216 + tw, 176, 3, 34, p.focus, 1);

    float kx0 = 190, ky0 = 252, kw = 900.0f / 11, kh = 68;
    for (int i = 0; i < (int)ks.size(); ++i) {
        const Key &k = ks[i];
        float x = kx0 + k.col * kw + 4, y = ky0 + k.row * (kh + 8), w = k.w * kw - 8;
        if (hover(x, y, w, kh)) {
            if (a.mouse_moved && !a.clicked && (g_kx != k.col || g_ky != k.row)) { g_kx = k.col, g_ky = k.row; sound_play(SFX_MOVE); }
            if (a.clicked) { g_kx = k.col, g_ky = k.row; kb_press(k); if (!g_kb) return; }
        }
        bool special = k.kind != K_CHAR && k.kind != K_SPACE;
        ImU32 bgc = k.kind == K_OK ? p.accent : (k.kind == K_SHIFT && g_shift) ? p.soft : special ? p.panel2 : p.bg;
        rect(x, y, w, kh, bgc, 12);
        std::string label;
        switch (k.kind) {
        case K_CHAR: label = std::string(1, g_shift && k.ch >= 'a' && k.ch <= 'z' ? (char)(k.ch - 32) : k.ch); break;
        case K_SHIFT: label = "Shift"; break;
        case K_SPACE: label = "Space"; break;
        case K_BACK: label = "Del"; break;
        case K_CANCEL: label = "Cancel"; break;
        case K_OK: label = "OK"; break;
        }
        ImU32 fg = k.kind == K_OK ? IM_COL32_WHITE : (k.kind == K_SHIFT && g_shift) ? p.bg : p.text;
        text(x + w / 2, y + (k.kind == K_CHAR ? 16 : 22), k.kind == K_CHAR ? 30 : 19, fg, label.c_str(), 0.5f);
        if (i == cur) focus_ring(x, y, w, kh, 12, 4);
    }
    if (a.pad == PAD_KEYBOARD) {   // typing on a keyboard: Enter keeps it, Esc leaves (the keys are Enter / Esc)
        text(640, 128, 15, p.dim, "Type on your keyboard, or pick the keys", 0.5f);
        hint_bar({{NAV_A, "OK"}, {NAV_B, "Cancel"}});
    } else {
        hint_bar({{NAV_A, "Type"}, {NAV_B, "Delete"}, {NAV_X, "Shift"}, {NAV_Y, "Space"}, {NAV_PLUS, "OK"}});
    }
}

void profile_frame() {
    App &a = app();
    const Palette &p = pal();
    int n = items(), c = cols();
    Nav kb_nav = NAV_NONE;
    bool kb_click = false, kb_moved = false;
    if (g_kb) {   // the keyboard has the input; the page only draws under it
        kb_nav = a.nav, kb_click = a.clicked, kb_moved = a.mouse_moved;
        a.nav = NAV_NONE, a.clicked = a.mouse_moved = false;
    }

    // build play activity list (sorted by most played)
    std::vector<std::pair<int, long long>> act;
    for (int i = 0; i < (int)ports().size(); ++i) {
        long long s = played_seconds(i);
        if (s > 0) act.push_back({i, s});
    }
    std::stable_sort(act.begin(), act.end(), [](const auto &x, const auto &y) { return x.second > y.second; });
    if (g_act_sel >= (int)act.size()) g_act_sel = std::max(0, (int)act.size() - 1);

    switch (a.nav) {
    case NAV_Y: kb_open(); break;
    case NAV_X:
        if (cfg().profiles.size() > 1) {
            if (a.now - g_del_armed < 4.0) {
                g_del_armed = -10;
                if (profile_delete(cfg().active_profile)) {
                    sound_play(SFX_BACK);
                    toast("Profile removed");
                    go_back(true);
                    return;
                }
            } else {
                g_del_armed = a.now;
                sound_play(SFX_ERROR);
                toast("Press X again to delete \"" + cfg().profile + "\"", 4);
            }
        }
        break;
    case NAV_B: go_back(); return;
    case NAV_L: g_tab = (g_tab + tab_count() - 1) % tab_count(); land(); sound_play(SFX_TAB); break;
    case NAV_R: g_tab = (g_tab + 1) % tab_count(); land(); sound_play(SFX_TAB); break;
    case NAV_LEFT:
        if (!activity_tab()) { if (g_sel > 0) { g_sel--; sound_play(SFX_MOVE); } else sound_play(SFX_EDGE); }
        break;
    case NAV_RIGHT:
        if (!activity_tab()) { if (g_sel < n - 1) { g_sel++; sound_play(SFX_MOVE); } else sound_play(SFX_EDGE); }
        break;
    case NAV_UP:
        if (activity_tab()) {
            if (g_act_sel > 0) { g_act_sel--; sound_play(SFX_MOVE); }
            else if (!act.empty()) sound_play(SFX_EDGE);
        } else {
            if (g_sel >= c) { g_sel -= c; sound_play(SFX_MOVE); } else sound_play(SFX_EDGE);
        }
        break;
    case NAV_DOWN:
        if (activity_tab()) {
            if (g_act_sel < (int)act.size() - 1) { g_act_sel++; sound_play(SFX_MOVE); }
            else if (!act.empty()) sound_play(SFX_EDGE);
        } else {
            if (g_sel + c < n) { g_sel += c; sound_play(SFX_MOVE); }
            else if (g_sel / c < (n - 1) / c) { g_sel = n - 1; sound_play(SFX_MOVE); }
            else sound_play(SFX_EDGE);
        }
        break;
    case NAV_A:
        if (activity_tab()) {
            if (!act.empty()) { a.game = act[g_act_sel].first; go(SCR_GAME); return; }
        } else {
            choose();
        }
        break;
    default: break;
    }
    n = items(), c = cols();
    g_sel = std::min(std::max(g_sel, 0), std::max(0, n - 1));

    // what the large icon shows: the one under the selection
    int show = avatar_find(cfg().avatar), bg = cfg().avatar_bg;
    std::vector<int> v = icons_of(g_tab);
    if (colour_tab()) bg = g_sel;
    else if (!activity_tab() && g_sel < (int)v.size()) show = v[g_sel];

    background(avatar_colours()[bg].col);
    status_bar();
    text(96, 92, 26, p.text, "Profile");

    // left: the preview
    rect(96, 140, 340, 496, p.panel, 22);
    avatar_draw(show, bg, 266, 294, 110);
    text(266, 426, 26, p.text, fit(26, cfg().profile, 300).c_str(), 0.5f);
    if (!g_kb && a.clicked && hover(116, 416, 300, 40)) kb_open();   // click the name to change it
    std::string what = (show >= 0 ? avatar_name(show) : std::string("No icon")) + "  ·  " + avatar_colours()[bg].name;
    text(266, 464, 15, p.soft, what.c_str(), 0.5f);
    int played_n = 0;
    long long tot_secs = total_playtime_seconds(&played_n);
    if (tot_secs > 0)
        text(266, 490, 14, p.dim, ("Total played: " + fmt_duration(tot_secs)).c_str(), 0.5f);
    {
        float gw = glyph_w(NAV_Y, 22), lw = text_w(15, "Change name"), x = 266 - (gw + 8 + lw) / 2;
        glyph(NAV_Y, x, 530, 22);
        text(x + gw + 8, 532, 15, p.soft, "Change name");
    }
    bool same = show == avatar_find(cfg().avatar) && bg == cfg().avatar_bg;
    text(266, 584, 14, p.dim, same ? "Your icon on Home and every screen" : "A keeps this one", 0.5f);

    // right: the pages
    float x0 = 486, tx = x0;
    for (int t = 0; t < tab_count(); ++t) {
        float w = text_w(16, tab_name(t)) + 24;
        if (a.clicked && hover(tx, 140, w, 40)) { if (g_tab != t) sound_play(SFX_TAB); g_tab = t; land(); }
        text(tx + 12, 149, 16, t == g_tab ? p.text : p.dim, tab_name(t));
        if (t == g_tab) g_tab_x = ease(g_tab_x ? g_tab_x : tx, tx, 20);
        tx += w;
    }
    rect(x0, 182, 750, 1, p.line);
    rect(g_tab_x + 8, 179, text_w(16, tab_name(g_tab)) + 8, 4, avatar_colours()[bg].col, 2);

    if (activity_tab()) {
        rect(x0, 202, 750, 58, p.panel, 14);
        text(x0 + 20, 214, 18, p.text, "Play activity");
        std::string sum = tot_secs > 0 ? (fmt_duration(tot_secs) + " across " + std::to_string(played_n) + (played_n == 1 ? " port" : " ports"))
                                       : "No playtime recorded yet";
        text(x0 + 730, 222, 16, tot_secs > 0 ? p.accent_text : p.soft, sum.c_str(), 1);
        text(x0 + 20, 238, 13, p.dim, "Tracked automatically whenever you play a port from Sanctum");
        if (act.empty()) {
            text(x0 + 375, 340, 17, p.soft, "Start any port from Sanctum to see your play history here.", 0.5f);
        } else {
            const int kVis = 5;
            int top = std::max(0, std::min(g_act_sel - 2, (int)act.size() - kVis));
            long long top_s = act[0].second;
            for (int i = top; i < (int)act.size() && i < top + kVis; ++i) {
                int pi = act[i].first;
                long long secs = act[i].second;
                const Port &port = ports()[pi];
                float y = 276 + (i - top) * 70;
                if (hover(x0, y, 750, 62)) {
                    if (a.mouse_moved && !a.clicked && g_act_sel != i) { g_act_sel = i; sound_play(SFX_MOVE); }
                    if (a.clicked) { a.game = pi; go(SCR_GAME); return; }
                }
                rect(x0, y, 750, 62, p.panel, 12);
                tile(port, x0 + 8, y + 8, 46, 1, false);
                text(x0 + 68, y + 8, 16, p.text, fit(16, port.title, 420).c_str());
                text(x0 + 734, y + 10, 15, port.colours.accent, fmt_duration(secs).c_str(), 1);
                text(x0 + 68, y + 32, 13, p.dim, fit(13, played_text(pi), 380).c_str());
                float frac = top_s > 0 ? (float)secs / (float)top_s : 0.0f;
                rect(x0 + 460, y + 40, 274, 6, p.line, 3);
                rect(x0 + 460, y + 40, std::max(6.0f, 274 * frac), 6, port.colours.accent, 3);
                if (i == g_act_sel) focus_ring(x0, y, 750, 62, 12, 3);
            }
        }
    } else {
        n = items(), c = cols();
        v = icons_of(g_tab);
        float cell = colour_tab() ? 92 : 122, r = colour_tab() ? 36 : 50;
        float gx = x0 + (750 - c * cell) / 2, gy = 210;
        int mine_icon = avatar_find(cfg().avatar);
        for (int i = 0; i < n; ++i) {
            float cx = gx + (i % c) * cell + cell / 2, cy = gy + (i / c) * cell + cell / 2;
            if (hover(cx - r, cy - r, 2 * r, 2 * r)) {
                if (a.mouse_moved && !a.clicked && g_sel != i) { g_sel = i; sound_play(SFX_MOVE); }
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
    }

    if (g_kb) {   // the keyboard over the page, with this frame's input
        a.nav = kb_nav, a.clicked = kb_click, a.mouse_moved = kb_moved;
        kb_frame();
        return;
    }
    bool can_del = cfg().profiles.size() > 1;
    if (activity_tab() && can_del) hint_bar({{NAV_A, "Open port"}, {NAV_X, "Delete profile"}, {NAV_Y, "Name"}, {NAV_L, "Page"}, {NAV_B, "Back"}});
    else if (activity_tab()) hint_bar({{NAV_A, "Open port"}, {NAV_Y, "Name"}, {NAV_L, "Page"}, {NAV_B, "Back"}});
    else if (can_del) hint_bar({{NAV_A, "Choose"}, {NAV_X, "Delete profile"}, {NAV_Y, "Name"}, {NAV_L, "Page"}, {NAV_B, "Back"}});
    else hint_bar({{NAV_A, "Choose"}, {NAV_Y, "Name"}, {NAV_L, "Page"}, {NAV_B, "Back"}});
}

}  // namespace sanctum
