// Show more / All Games & Ports grid (SCR_LIBRARY):
// Sleek 6-column pure cover-art grid with top [L]/[R] tab bar (All Games / Ports / Custom Games / Pinned),
// left utility sidebar (Filter / Sort / Add PC game), orbiting Aurora Prism selection border, and a
// floating speech-bubble title pill anchored directly to the highlighted tile.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "app.h"
#include "catalog.h"
#include "config.h"
#include "draw.h"
#include "install.h"
#include "launch.h"
#include "platform.h"
#include "sound.h"

namespace sanctum {

static int g_lib_tab = 0;         // 0 All Games, 1 Ports, 2 Custom Games, 3 Pinned
static bool g_in_side = false;    // true when focus is on the left icon rail (0 Filter, 1 Sort, 2 Add .exe)
static int g_side_sel = 0;
static int g_lib_sel = 0;
static float g_lib_scroll = 0.0f;
static float g_ring_x = 0.0f, g_ring_y = 0.0f;
static bool g_ring_init = false;
static const int kLibCols = 6;

void library_open() {
    g_in_side = false;
    g_lib_sel = 0;
    g_lib_scroll = 0.0f;
    g_ring_init = false;
    go(SCR_LIBRARY);
}

static void lib_quick_launch(int idx) {
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

static void add_custom_game_action() {
    if (demo_mode()) {
        sound_play(SFX_ERROR);
        toast("Demo mode: custom games unchanged");
        return;
    }
    std::string exe = pick_open_file(app().window, "Choose a PC game executable", "Programs", "*.exe");
    if (exe.empty()) return;
    int idx = catalog_add_custom(exe);
    if (idx >= 0) {
        sound_play(SFX_TOGGLE_ON);
        toast("Added " + std::string(ports()[idx].title) + " to your library");
        app().game = idx;
        go(SCR_GAME);
    } else {
        sound_play(SFX_ERROR);
        toast("Could not add executable");
    }
}

static void draw_side_icon(int kind, float cx, float cy, float sz, ImU32 col) {
    float h = sz * 0.5f, t = S(2.1f);
    ImDrawList *d = dl();
    auto L = [&](float x0, float y0, float x1, float y1) {
        d->AddLine(P(cx + x0 * h, cy + y0 * h), P(cx + x1 * h, cy + y1 * h), col, t);
    };
    if (kind == 0) {
        // Funnel (Filter)
        L(-0.82f, -0.72f, 0.82f, -0.72f);
        L(-0.82f, -0.72f, -0.18f, 0.05f);
        L(0.82f, -0.72f, 0.18f, 0.05f);
        L(-0.18f, 0.05f, -0.18f, 0.78f);
        L(0.18f, 0.05f, 0.18f, 0.56f);
        L(-0.18f, 0.78f, 0.18f, 0.56f);
    } else if (kind == 1) {
        // Dual vertical arrows (Sort)
        L(-0.36f, -0.70f, -0.36f, 0.70f);
        L(-0.36f, 0.70f, -0.72f, 0.34f);
        L(-0.36f, 0.70f, 0.0f, 0.34f);
        L(0.36f, 0.70f, 0.36f, -0.70f);
        L(0.36f, -0.70f, 0.0f, -0.34f);
        L(0.36f, -0.70f, 0.72f, -0.34f);
    } else {
        // Plus inside subtle rounded frame (Add custom game)
        L(-0.56f, 0.0f, 0.56f, 0.0f);
        L(0.0f, -0.56f, 0.0f, 0.56f);
    }
}

void library_frame() {
    App &a = app();
    const Palette &p = pal();
    auto &ps = ports();

    // Build filtered, sorted list of indices (-1 sentinel = "+ Add custom game" card at end)
    std::vector<int> items;
    for (int i = 0; i < (int)ps.size(); ++i) {
        bool has = !ps[i].installs.empty() || install_queued(i);
        if (!has) continue;
        if (g_lib_tab == 1 && ps[i].custom) continue;
        if (g_lib_tab == 2 && !ps[i].custom) continue;
        if (g_lib_tab == 3 && cfg().pinned.count(ps[i].id) == 0) continue;
        items.push_back(i);
    }
    int sort_mode = cfg().home_sort;
    std::stable_sort(items.begin(), items.end(), [&](int x, int y) {
        bool px = cfg().pinned.count(ps[x].id) > 0, py = cfg().pinned.count(ps[y].id) > 0;
        if (px != py) return px;
        if (sort_mode == 1) {
            long long sx = played_seconds(x), sy = played_seconds(y);
            if (sx != sy) return sx > sy;
        } else if (sort_mode == 2) {
            int c = SDL_strcasecmp(ps[x].title, ps[y].title);
            if (c != 0) return c < 0;
        }
        return last_played(x) > last_played(y);
    });
    if (g_lib_tab == 0 || g_lib_tab == 2) items.push_back(-1);

    int n = (int)items.size();
    if (n == 0) { g_lib_sel = 0; g_in_side = true; }
    else if (g_lib_sel >= n) g_lib_sel = n - 1;

    static const char *const kTabNames[] = {"All Games", "Ports", "Custom Games", "Pinned"};
    static const char *const kSortLabels[] = {"By Recently Played", "By Longest Play Time", "By Title (A-Z)"};

    auto cycle_tab = [&](int delta) {
        g_lib_tab = (g_lib_tab + delta + 4) % 4;
        g_lib_sel = 0;
        g_lib_scroll = 0.0f;
        g_ring_init = false;
        sound_play(SFX_TAB);
    };
    auto cycle_sort = [&]() {
        cfg().home_sort = (cfg().home_sort + 1) % 3;
        config_save();
        sound_play(SFX_OK);
        toast(std::string("Sorted ") + kSortLabels[cfg().home_sort]);
    };

    // Input handling
    bool moved = false;
    switch (a.nav) {
    case NAV_LEFT:
        if (!g_in_side) {
            if (n > 0 && g_lib_sel % kLibCols > 0) {
                g_lib_sel--;
                moved = true;
                sound_play_move(-1, ((float)(g_lib_sel % kLibCols) / (float)(kLibCols - 1) - 0.5f) * 0.56f);
            } else {
                g_in_side = true;
                sound_play_move(-1, -0.32f);
            }
        } else {
            sound_play(SFX_EDGE);
        }
        break;
    case NAV_RIGHT:
        if (g_in_side) {
            if (n > 0) {
                g_in_side = false;
                g_ring_init = false;
                sound_play_move(1, -0.20f);
            } else sound_play(SFX_EDGE);
        } else if (n > 0 && g_lib_sel + 1 < n && g_lib_sel % kLibCols < kLibCols - 1) {
            g_lib_sel++;
            moved = true;
            sound_play_move(1, ((float)(g_lib_sel % kLibCols) / (float)(kLibCols - 1) - 0.5f) * 0.56f);
        } else {
            sound_play(SFX_EDGE);
        }
        break;
    case NAV_UP:
        if (g_in_side) {
            if (g_side_sel > 0) { g_side_sel--; sound_play_move(-1, -0.32f); }
            else sound_play(SFX_EDGE);
        } else if (n > 0 && g_lib_sel >= kLibCols) {
            g_lib_sel -= kLibCols;
            moved = true;
            sound_play_move(-1, 0.0f);
        } else {
            sound_play(SFX_EDGE);
        }
        break;
    case NAV_DOWN:
        if (g_in_side) {
            if (g_side_sel < 2) { g_side_sel++; sound_play_move(1, -0.32f); }
            else sound_play(SFX_EDGE);
        } else if (n > 0 && g_lib_sel + kLibCols < n) {
            g_lib_sel += kLibCols;
            moved = true;
            sound_play_move(1, 0.0f);
        } else if (n > 0 && (g_lib_sel / kLibCols) < ((n - 1) / kLibCols)) {
            g_lib_sel = n - 1;
            moved = true;
            sound_play_move(1, 0.0f);
        } else {
            sound_play(SFX_EDGE);
        }
        break;
    case NAV_L: cycle_tab(-1); break;
    case NAV_R: cycle_tab(1); break;
    case NAV_MINUS: cycle_sort(); break;
    case NAV_Y:
        if (!g_in_side && n > 0 && items[g_lib_sel] >= 0) {
            lib_quick_launch(items[g_lib_sel]);
            a.nav = NAV_NONE, a.clicked = false;
        }
        break;
    case NAV_X:
        if (!g_in_side && n > 0 && items[g_lib_sel] >= 0) {
            const Port &pt = ps[items[g_lib_sel]];
            if (cfg().pinned.count(pt.id)) {
                cfg().pinned.erase(pt.id);
                sound_play(SFX_TOGGLE_OFF);
                toast("Unpinned " + std::string(pt.title));
            } else {
                cfg().pinned.insert(pt.id);
                sound_play(SFX_TOGGLE_ON);
                toast("Pinned " + std::string(pt.title) + " to front");
            }
            config_save();
        }
        break;
    case NAV_PLUS:
        go(SCR_PORTS);
        return;
    case NAV_A:
        if (g_in_side) {
            if (g_side_sel == 0) cycle_tab(1);
            else if (g_side_sel == 1) cycle_sort();
            else { add_custom_game_action(); return; }
        } else if (n > 0) {
            int id = items[g_lib_sel];
            if (id < 0) { add_custom_game_action(); return; }
            a.game = id;
            go(SCR_GAME);
            return;
        }
        break;
    case NAV_B:
        go_back();
        return;
    default: break;
    }

    // ---- Background (clean dark/light console canvas, gently tinted by focused cover)
    const Port *fp = (!g_in_side && n > 0 && items[g_lib_sel] >= 0) ? &ps[items[g_lib_sel]] : nullptr;
    background(cfg().follow && fp ? fp->colours.accent : 0);

    // ---- Top centered tab bar with [L] and [R] glyphs + crisp cyan active underline
    ImU32 tab_hi = cfg().border_style == 0
                     ? (p.light ? IM_COL32(0, 146, 198, 255) : IM_COL32(84, 218, 246, 255))
                     : p.accent_text;
    const float tab_y = 26.0f, bar_y = 74.0f;
    float tab_w[4];
    float total_tw = 0.0f;
    const float tab_gap = 44.0f;
    for (int t = 0; t < 4; ++t) {
        tab_w[t] = text_w(20, kTabNames[t]) + 28.0f;
        total_tw += tab_w[t] + (t ? tab_gap : 0.0f);
    }
    float tx0 = 640.0f - total_tw * 0.5f;
    // Left [L] and right [R] shoulder hints flanking the centered tabs
    float lw = glyph_w(NAV_L, 26);
    glyph(NAV_L, tx0 - lw - 42.0f, tab_y + 2.0f, 26);
    glyph(NAV_R, tx0 + total_tw + 42.0f, tab_y + 2.0f, 26);

    float tx = tx0;
    for (int t = 0; t < 4; ++t) {
        bool active = (t == g_lib_tab);
        if (hover(tx - 8, 12, tab_w[t] + 16, 58) && a.clicked && !active) {
            g_lib_tab = t;
            g_lib_sel = 0;
            g_lib_scroll = 0.0f;
            g_ring_init = false;
            sound_play(SFX_TAB);
        }
        text(tx + tab_w[t] * 0.5f, tab_y + 4.0f, 20, active ? tab_hi : p.soft, kTabNames[t], 0.5f);
        if (active) rect(tx - 6.0f, bar_y - 3.0f, tab_w[t] + 12.0f, 3.5f, tab_hi, 2.0f);
        tx += tab_w[t] + tab_gap;
    }
    rect(46, bar_y, 1188, 1, p.line);

    // ---- Grid geometry: 6 tight square cover columns, left utility rail, right-aligned sort label
    const float card_s = 158.0f, gap = 14.0f, step = card_s + gap;
    const float grid_x0 = 166.0f;
    const float grid_w = kLibCols * card_s + (kLibCols - 1) * gap;
    const float top_y = 128.0f, view_h = 514.0f;

    // Right-aligned sort label right above the 6th column
    const char *slbl = kSortLabels[cfg().home_sort % 3];
    float slw = text_w(16, slbl);
    if (hover(grid_x0 + grid_w - slw - 12, 86, slw + 24, 30) && a.clicked) cycle_sort();
    text(grid_x0 + grid_w, 92, 16, p.soft, slbl, 1.0f);

    // ---- Left utility icon rail (Filter, Sort, Add PC game)
    static const char *const kSideTips[] = {"Filter tab", "Change sort", "Add PC game (.exe)"};
    for (int s = 0; s < 3; ++s) {
        float scx = 96.0f, scy = top_y + 38.0f + s * 74.0f;
        bool f = g_in_side && g_side_sel == s;
        if (hover(scx - 26, scy - 26, 52, 52)) {
            if (a.mouse_moved && !a.clicked && (!g_in_side || g_side_sel != s)) {
                int dir = s > g_side_sel ? 1 : -1;
                g_in_side = true;
                g_side_sel = s;
                sound_play_move(dir, -0.32f);
            }
            if (a.clicked) {
                g_in_side = true;
                g_side_sel = s;
                if (s == 0) cycle_tab(1);
                else if (s == 1) cycle_sort();
                else { add_custom_game_action(); return; }
            }
        }
        if (f) circle(scx, scy, 24.0f, with_alpha(p.panel2, 220));
        draw_side_icon(s, scx, scy, 26.0f, f ? tab_hi : p.soft);
        if (f) focus_ring(scx - 25.0f, scy - 25.0f, 50.0f, 50.0f, 25.0f, 3.0f);
    }

    // ---- Scroll math
    int rows = (n + kLibCols - 1) / kLibCols;
    float total_h = rows > 0 ? rows * step - gap : 0.0f;
    float max_scroll = std::max(0.0f, total_h - view_h);

    if (hover(grid_x0 - 20, top_y, grid_w + 40, view_h) && ImGui::GetIO().MouseWheel != 0.0f)
        g_lib_scroll -= ImGui::GetIO().MouseWheel * 80.0f;
    if (moved && !g_in_side && n > 0) {
        int r = g_lib_sel / kLibCols;
        float ry = r * step;
        if (ry < g_lib_scroll) g_lib_scroll = ry;
        if (ry + card_s > g_lib_scroll + view_h) g_lib_scroll = ry + card_s - view_h;
    }
    g_lib_scroll = SDL_clamp(g_lib_scroll, 0.0f, max_scroll);

    // ---- Draw 6-column cover grid (focused tile + floating title tooltip drawn LAST on top)
    bool mouse_in_view = a.mouse.y >= top_y - 6 && a.mouse.y < top_y + view_h + 6;
    float foc_x = 0.0f, foc_y = 0.0f;
    bool has_foc = false;
    ImU32 foc_acc = p.accent;
    std::string foc_title;

    auto draw_card = [&](int i) {
        int col = i % kLibCols, row = i / kLibCols;
        float x = grid_x0 + col * step;
        float y = top_y + row * step - g_lib_scroll;
        if (y + card_s < top_y - 24 || y > top_y + view_h + 24) return;
        bool f = (!g_in_side && i == g_lib_sel);
        int id = items[i];
        if (mouse_in_view && hover(x, y, card_s, card_s)) {
            if (a.mouse_moved && !a.clicked && (g_in_side || g_lib_sel != i)) {
                int dir = i > g_lib_sel ? 1 : -1;
                g_in_side = false;
                g_lib_sel = i;
                sound_play_move(dir, ((float)col / (float)(kLibCols - 1) - 0.5f) * 0.56f);
            }
            if (a.clicked) {
                g_in_side = false;
                g_lib_sel = i;
                if (id < 0) { add_custom_game_action(); return; }
                float pbx = x + card_s - 25.0f, pby = y + card_s - 25.0f;
                if (!ps[id].installs.empty() && a.pad == PAD_KEYBOARD && std::hypot(a.mouse.x - pbx, a.mouse.y - pby) <= 20.0f) {
                    lib_quick_launch(id);
                    return;
                }
                a.game = id;
                go(SCR_GAME);
                return;
            }
        }
        if (id >= 0) {
            const Port &pt = ps[id];
            tile(pt, x, y, card_s, 1.0f, true, f ? 1.0f : 0.85f);
            if (pt.custom) {
                float bw = 32, bh = 18;
                rect(x + card_s - bw - 7, y + 7, bw, bh, IM_COL32(0, 0, 0, 185), 9);
                text(x + card_s - bw * 0.5f - 7, y + 9.5f, 10.5f, IM_COL32(232, 238, 248, 255), "PC", 0.5f);
            }
            if (mouse_in_view && a.pad == PAD_KEYBOARD && !pt.installs.empty() && hover(x, y, card_s, card_s)) {
                float pbx = x + card_s - 25.0f, pby = y + card_s - 25.0f;
                bool ph = std::hypot(a.mouse.x - pbx, a.mouse.y - pby) <= 20.0f;
                circle(pbx, pby + 1.5f, 18.0f, IM_COL32(0, 0, 0, 120));
                circle(pbx, pby, ph ? 19.0f : 17.0f, ph ? pt.colours.accent : with_alpha(IM_COL32(18, 22, 30, 255), 225));
                dl()->AddCircle(P(pbx, pby), S(ph ? 19.0f : 17.0f), with_alpha(IM_COL32_WHITE, ph ? 210 : 120), 28, S(1.3f));
                icon(IC_PLAY, pbx + 1.2f, pby, 14.0f, IM_COL32_WHITE);
            }
            if (f) {
                foc_x = x; foc_y = y; has_foc = true;
                foc_acc = pt.colours.accent;
                foc_title = pt.title;
            }
        } else {
            // Sleek "+ Add custom game" card
            float rad = card_s * 0.065f;
            rect(x, y, card_s, card_s, p.panel, rad);
            frame(x, y, card_s, card_s, p.line, rad, 1.4f);
            float cx = x + card_s * 0.5f, cy = y + card_s * 0.44f;
            circle(cx, cy, 24.0f, p.panel2);
            dl()->AddLine(P(cx - 10, cy), P(cx + 10, cy), p.text, S(2.8f));
            dl()->AddLine(P(cx, cy - 10), P(cx, cy + 10), p.text, S(2.8f));
            text(cx, y + card_s * 0.72f, 13.5f, p.soft, "Add custom", 0.5f);
            if (f) {
                foc_x = x; foc_y = y; has_foc = true;
                foc_acc = p.accent;
                foc_title = "Add custom PC game (.exe)";
            }
        }
    };

    dl()->PushClipRect(P(grid_x0 - 20, top_y - 16), P(grid_x0 + grid_w + 20, top_y + view_h + 10), true);
    if (n == 0) {
        const char *msg = g_lib_tab == 3 ? "No pinned games yet — press X on any title to pin it."
                                         : "No games in this tab yet.";
        text(grid_x0 + grid_w * 0.5f, top_y + 140, 18, p.soft, msg, 0.5f);
    }
    for (int i = 0; i < n; ++i) {
        if (!g_in_side && i == g_lib_sel) continue;
        draw_card(i);
    }
    if (!g_in_side && n > 0) draw_card(g_lib_sel);

    if (has_foc) {
        int tmot = SDL_clamp(cfg().tile_motion, 0, 2);
        if (!g_ring_init || tmot == 0) {
            g_ring_x = foc_x; g_ring_y = foc_y;
            g_ring_init = true;
        } else {
            float spd = tmot == 2 ? 28.0f : 22.0f;
            g_ring_x = ease(g_ring_x, foc_x, spd);
            g_ring_y = ease(g_ring_y, foc_y, spd);
        }
        frame(g_ring_x - 3.0f, g_ring_y - 3.0f, card_s + 6.0f, card_s + 6.0f,
              with_alpha(p.bg, p.light ? 190 : 230), card_s * 0.065f + 3.0f, 4.5f);
        focus_ring(g_ring_x, g_ring_y, card_s, card_s, card_s * 0.065f, 5.5f, foc_acc);
    }
    dl()->PopClipRect();

    // ---- Floating speech-bubble title tooltip anchored directly above (or below row 0) the focused card!
    if (has_foc && !foc_title.empty()) {
        std::string cap = fit(19, foc_title, 460.0f);
        float tw = text_w(19, cap.c_str());
        float bw = tw + 40.0f, bh = 44.0f;
        float anchor_x = g_ring_x + card_s * 0.5f;
        float bx = SDL_clamp(anchor_x - bw * 0.5f, grid_x0 - 12.0f, grid_x0 + grid_w + 12.0f - bw);
        bool place_above = (g_ring_y - top_y) > 40.0f;
        float by = place_above ? (g_ring_y - bh - 16.0f) : (g_ring_y + card_s + 16.0f);
        float ptr_x = SDL_clamp(anchor_x, bx + 20.0f, bx + bw - 20.0f);

        ImU32 bub_bg = p.light ? IM_COL32(252, 253, 255, 248) : IM_COL32(44, 47, 54, 246);
        ImU32 bub_rim = with_alpha(p.light ? IM_COL32(170, 178, 194, 255) : IM_COL32_WHITE, p.light ? 130 : 34);
        // Drop shadow
        rect(bx + 2, by + 5, bw, bh, with_alpha(IM_COL32_BLACK, p.light ? 42 : 135), 14.0f);
        // Bubble body + subtle rim
        rect(bx, by, bw, bh, bub_bg, 14.0f);
        frame(bx, by, bw, bh, bub_rim, 14.0f, 1.0f);
        // Pointer triangle stem toward the highlighted tile
        if (place_above) {
            dl()->AddTriangleFilled(P(ptr_x - 8, by + bh - 1), P(ptr_x + 8, by + bh - 1), P(ptr_x, by + bh + 8), bub_bg);
        } else {
            dl()->AddTriangleFilled(P(ptr_x - 8, by + 1), P(ptr_x + 8, by + 1), P(ptr_x, by - 8), bub_bg);
        }
        text(bx + bw * 0.5f, by + 11.5f, 19, tab_hi, cap.c_str(), 0.5f);
    } else if (g_in_side) {
        // Floating tooltip right beside the focused sidebar icon
        float scy = top_y + 38.0f + g_side_sel * 74.0f;
        const char *tip = kSideTips[g_side_sel];
        float bw = text_w(16, tip) + 30.0f, bh = 36.0f;
        float bx = 136.0f, by = scy - bh * 0.5f;
        ImU32 bub_bg = p.light ? IM_COL32(252, 253, 255, 248) : IM_COL32(44, 47, 54, 246);
        rect(bx + 2, by + 4, bw, bh, with_alpha(IM_COL32_BLACK, p.light ? 40 : 120), 12.0f);
        rect(bx, by, bw, bh, bub_bg, 12.0f);
        text(bx + bw * 0.5f, by + 9.0f, 16, tab_hi, tip, 0.5f);
    }

    // ---- Scrollbar
    if (total_h > view_h) {
        float thumb_h = std::max(36.0f, view_h * view_h / total_h);
        rect(1222, top_y, 4, view_h, p.line, 2);
        rect(1222, top_y + (view_h - thumb_h) * (g_lib_scroll / std::max(1.0f, max_scroll)), 4, thumb_h, p.soft, 2);
    }

    // ---- Bottom hint bar
    if (g_in_side) {
        hint_bar({{NAV_A, "Select"}, {NAV_PLUS, "Get ports"}, {NAV_B, "Back"}});
    } else if (n > 0 && items[g_lib_sel] >= 0) {
        const Port &pt = ps[items[g_lib_sel]];
        bool pinned = cfg().pinned.count(pt.id) > 0;
        if (!pt.installs.empty()) {
            const char *plbl = running_port() == items[g_lib_sel] ? "Resume"
                             : can_play(pt, *pt.best()) ? "Play" : "Set up";
            hint_bar({{NAV_Y, plbl}, {NAV_A, "Open"}, {NAV_X, pinned ? "Unpin" : "Pin"}, {NAV_PLUS, "Get ports"}, {NAV_B, "Back"}});
        } else {
            hint_bar({{NAV_A, "Open"}, {NAV_X, pinned ? "Unpin" : "Pin"}, {NAV_PLUS, "Get ports"}, {NAV_B, "Back"}});
        }
    } else {
        hint_bar({{NAV_A, "Add .exe"}, {NAV_PLUS, "Get ports"}, {NAV_B, "Back"}});
    }
}

}  // namespace sanctum
