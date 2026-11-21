// System Settings (the Switch 2 layout: categories on the left, their rows on the right), the folder picker the
// cover-art row opens (a controller can reach any folder: drives, the usual places, up a level with B), the
// Controllers page (each connected pad and a live test of its buttons) and the placeholder for pages to come.
#include <algorithm>
#include <cmath>

#include "config.h"
#include "covers.h"
#include "draw.h"
#include "input.h"
#include "install.h"

namespace sanctum {

// ======================= System Settings
enum Cat { CAT_DISPLAY, CAT_THEMES, CAT_COVERS, CAT_LIBRARY, CAT_CONTROLLERS, CAT_ABOUT, CAT_N };
static const char *kCatName[] = {"Display", "Themes", "Cover art", "Ports on this PC", "Controllers", "About Sanctum"};
static const Icon kCatIcon[] = {IC_DISPLAY, IC_PALETTE, IC_FOLDER, IC_LIBRARY, IC_PAD, IC_INFO};
static int g_cat = 0, g_row = 0;
static bool g_in_rows = false;

struct Row {
    std::string label, value;
    int id;          // what A does
    bool toggle = false, on = false;
};
enum { R_THEME, R_FOLLOW, R_TILES, R_FULLSCREEN, R_HIDE_CURSOR, R_FOLDER, R_RELOAD, R_SEARCH, R_DEV, R_LIBDIR, R_PORT, R_PADS, R_LAYOUT, R_ICONS, R_INFO };

static std::vector<Row> rows_of(int cat) {
    std::vector<Row> r;
    Config &c = cfg();
    switch (cat) {
    case CAT_DISPLAY: {
        r.push_back({"Fullscreen", "", R_FULLSCREEN, true, c.fullscreen});
        r.push_back({"Hide the mouse pointer while using a controller", "", R_HIDE_CURSOR, true, c.hide_cursor});
        SDL_DisplayID d = SDL_GetDisplayForWindow(app().window);
        const SDL_DisplayMode *m = d ? SDL_GetDesktopDisplayMode(d) : nullptr;
        const char *name = d && !demo_mode() ? SDL_GetDisplayName(d) : nullptr;
        if (m) r.push_back({"Screen", std::string(name ? name : "Display") + "  ·  " + std::to_string(m->w) + " x " +
                                          std::to_string(m->h) + "  ·  " + std::to_string((int)(m->refresh_rate + 0.5f)) + " Hz", R_INFO});
        r.push_back({"Shortcut", "F11 or Alt+Enter switches fullscreen at any time", R_INFO});
        break;
    }
    case CAT_THEMES:
        r.push_back({"Theme", c.theme ? "Light" : "Dark", R_THEME});
        r.push_back({"Home follows the selected port's colours", "", R_FOLLOW, true, c.follow});
        r.push_back({"Home tile size", c.large_tiles ? "Large" : "Medium", R_TILES});
        break;
    case CAT_COVERS:
        r.push_back({"Cover art folder", c.cover_dir.empty() ? "Choose..." : c.cover_dir, R_FOLDER});
        r.push_back({"Ports with cover art", std::to_string(covers_matched()) + " of " + std::to_string(ports().size()), R_INFO});
        r.push_back({"Read the folder again", "", R_RELOAD});
        break;
    case CAT_LIBRARY:
        r.push_back({scan_running() ? "Searching this PC..." : "Search this PC again", "", R_SEARCH});
        r.push_back({"Install new ports into", shown_path(library_dir()), R_LIBDIR});
        r.push_back({"Include copies in build and backup folders", "", R_DEV, true, !c.hide_dev});
        for (auto &p : ports()) {
            std::string dir = p.installs.empty() ? "" : shown_path(p.installs[0].dir, p.title);
            std::string v = p.installs.empty() ? (p.hidden ? "Only in build folders" : "Not found")
                          : p.installs.size() == 1 || demo_mode() ? dir
                          : std::to_string(p.installs.size()) + " copies, using " + dir;
            r.push_back({p.title, v, R_PORT});
        }
        break;
    case CAT_CONTROLLERS: {
        static const char *const layouts[] = {"Automatic", "Nintendo (A right)", "Xbox (A bottom)"};
        static const char *const icons[] = {"Automatic", "Nintendo", "Xbox", "PlayStation"};
        r.push_back({"Button layout", layouts[c.pad_layout % 3], R_LAYOUT});
        r.push_back({"Button icons", icons[c.button_icons % 4], R_ICONS});
        r.push_back({"Test controllers", "", R_PADS});
        r.push_back({"Automatic layout", "A confirms where each pad prints it", R_INFO});
        break;
    }
    case CAT_ABOUT:
        r.push_back({"SteelTemple Sanctum", "Version 0.3.0", R_INFO});
        r.push_back({"Settings folder", demo_mode() ? "%APPDATA%\\SteelTemple\\Sanctum" : config_dir(), R_INFO});
        r.push_back({"Game files", "Never downloaded or shipped: every port uses your own copy", R_INFO});
        break;
    }
    return r;
}

static void act(const Row &r) {
    Config &c = cfg();
    switch (r.id) {
    case R_THEME: c.theme ^= 1; theme_set(c.theme); break;
    case R_FOLLOW: c.follow = !c.follow; break;
    case R_TILES: c.large_tiles = !c.large_tiles; break;
    case R_FULLSCREEN: set_fullscreen(!c.fullscreen); return;
    case R_HIDE_CURSOR: c.hide_cursor = !c.hide_cursor; if (!c.hide_cursor) SDL_ShowCursor(); break;
    case R_FOLDER: folder_open(c.cover_dir); return;
    case R_RELOAD: covers_reload(); toast("Reading your cover art"); return;
    case R_SEARCH: scan_start(); return;
    case R_DEV: c.hide_dev = !c.hide_dev; catalog_refilter(); break;
    case R_LIBDIR: folder_open(library_dir(), FOLDER_LIBRARY); return;
    case R_PORT: {
        for (int i = 0; i < (int)ports().size(); ++i)
            if (r.label == ports()[i].title) { app().game = i; go(SCR_GAME); }
        return;
    }
    case R_PADS: go(SCR_CONTROLLERS); return;
    case R_LAYOUT:   // takes effect on the next press: the press that changed it was read the old way
        c.pad_layout = (c.pad_layout + 1) % 3;
        toast(c.pad_layout == 1 ? "Switch style: A on the right confirms" : c.pad_layout == 2 ? "Xbox style: A at the bottom confirms"
                                                                               : "Confirm follows each pad's own labels");
        break;
    case R_ICONS: c.button_icons = (c.button_icons + 1) % 4; input_refresh_style(); break;
    default: return;
    }
    config_save();
}

void settings_frame() {
    App &a = app();
    const Palette &p = pal();
    std::vector<Row> rows = rows_of(g_cat);
    auto focusable = [&](int i) { return rows[i].id != R_INFO; };
    auto first_focusable = [&]() {
        for (int i = 0; i < (int)rows.size(); ++i) if (focusable(i)) return i;
        return -1;
    };

    switch (a.nav) {
    case NAV_UP:
        if (!g_in_rows) g_cat = std::max(0, g_cat - 1);
        else for (int i = g_row - 1; i >= 0; --i) if (focusable(i)) { g_row = i; break; }
        break;
    case NAV_DOWN:
        if (!g_in_rows) g_cat = std::min(CAT_N - 1, g_cat + 1);
        else for (int i = g_row + 1; i < (int)rows.size(); ++i) if (focusable(i)) { g_row = i; break; }
        break;
    case NAV_RIGHT:
    case NAV_A:
        if (!g_in_rows) {
            int f = first_focusable();
            if (f >= 0) g_in_rows = true, g_row = f;
        } else if (a.nav == NAV_A && g_row < (int)rows.size()) {
            act(rows[g_row]);
        }
        break;
    case NAV_LEFT: g_in_rows = false; break;
    case NAV_B:
        if (g_in_rows) g_in_rows = false;
        else { go_back(); return; }
        break;
    default: break;
    }
    rows = rows_of(g_cat);
    if (g_row >= (int)rows.size()) g_row = std::max(0, first_focusable());

    background(0);
    status_bar();
    text(58, 96, 26, p.text, "System Settings");
    rect(340, 76, 1, 580, p.line);
    for (int i = 0; i < CAT_N; ++i) {
        float y = 146 + i * 54;
        if (hover(44, y, 280, 46)) {
            if (a.mouse_moved) g_cat = i, g_in_rows = false;
            if (a.clicked) g_cat = i, g_in_rows = false;
        }
        bool sel = i == g_cat;
        if (sel) { rect(44, y, 280, 46, p.panel, 12); rect(44, y + 8, 4, 30, p.accent, 2); }
        icon(kCatIcon[i], 76, y + 23, 22, sel ? p.text : p.soft);
        text(100, y + 12, 18, sel ? p.text : p.soft, kCatName[i]);
        if (sel && !g_in_rows) focus_ring(44, y, 280, 46, 12, 3);
    }

    float x = 384, w = 852;
    text(x, 96, 26, p.text, kCatName[g_cat]);
    for (int i = 0; i < (int)rows.size() && i < 9; ++i) {
        const Row &r = rows[i];
        float y = 146 + i * 56;
        bool f = g_in_rows && i == g_row;
        if (focusable(i) && hover(x, y, w, 48)) {
            if (a.mouse_moved) g_in_rows = true, g_row = i;
            if (a.clicked) { g_in_rows = true, g_row = i; act(r); }
        }
        rect(x, y, w, 48, p.panel, 12);
        text(x + 20, y + 13, 17, r.id == R_INFO ? p.soft : p.text, fit(17, r.label, 420).c_str());
        if (r.toggle) {
            float tx = x + w - 76;
            rect(tx, y + 10, 56, 28, r.on ? p.accent : p.line, 14);
            circle(r.on ? tx + 42 : tx + 14, y + 24, 11, IM_COL32_WHITE);
        } else if (!r.value.empty()) {
            ImU32 c = r.id == R_INFO || r.id == R_PORT || r.id == R_FOLDER || r.id == R_LIBDIR ? p.soft : p.accent_text;
            text(x + w - 20, y + 14, 16, c, fit(16, r.value, w - 480).c_str(), 1);
        }
        if (f) focus_ring(x, y, w, 48, 12, 3);
    }
    if (g_in_rows) hint_bar({{NAV_A, "OK"}, {NAV_B, "Back"}});
    else hint_bar({{NAV_A, "Open"}, {NAV_B, "Back"}});
}

// ======================= the folder picker
struct Entry {
    std::string name, path;
    Icon icon;
};
static std::string g_dir;              // empty: "This PC" (drives and the usual places)
static std::vector<Entry> g_entries;
static int g_sel = 0, g_top = 0, g_pngs = 0;
static FolderFor g_for = FOLDER_COVERS;

static void list_dir(const std::string &dir) {
    g_dir = dir, g_entries.clear(), g_sel = 0, g_top = 0;
    if (dir.empty()) {
        struct { SDL_Folder f; const char *n; } places[] = {
            {SDL_FOLDER_DESKTOP, "Desktop"}, {SDL_FOLDER_PICTURES, "Pictures"}, {SDL_FOLDER_DOWNLOADS, "Downloads"},
            {SDL_FOLDER_DOCUMENTS, "Documents"}};
        for (auto &pl : places)
            if (const char *p = SDL_GetUserFolder(pl.f)) {
                std::string s = p;
                while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
                g_entries.push_back({pl.n, s, IC_FOLDER});
            }
        for (char d = 'A'; d <= 'Z'; ++d) {
            std::string root = std::string(1, d) + ":\\";
            if (SDL_GetPathInfo(root.c_str(), nullptr)) g_entries.push_back({std::string(1, d) + ":", std::string(1, d) + ":", IC_DRIVE});
        }
        g_pngs = 0;
        return;
    }
    int n = 0;
    char **names = SDL_GlobDirectory((dir + "\\").c_str(), "*", 0, &n);
    std::vector<std::string> dirs;
    for (int i = 0; names && i < n; ++i) {
        SDL_PathInfo info;
        std::string full = dir + "\\" + names[i];
        if (names[i][0] != '.' && names[i][0] != '$' && SDL_GetPathInfo(full.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY)
            dirs.push_back(names[i]);
    }
    SDL_free(names);
    std::sort(dirs.begin(), dirs.end(), [](const std::string &a, const std::string &b) { return SDL_strcasecmp(a.c_str(), b.c_str()) < 0; });
    for (auto &d : dirs) g_entries.push_back({d, dir + "\\" + d, IC_FOLDER});
    g_pngs = count_pngs(dir);
}

static std::string parent_of(const std::string &d) {
    size_t s = d.find_last_of("\\/");
    if (s == std::string::npos || d.size() <= 2) return "";   // a drive's parent is This PC
    return d.substr(0, s);
}

void folder_open(const std::string &start, FolderFor what) {
    g_for = what;
    std::string s = start;
    if (!s.empty() && !SDL_GetPathInfo(s.c_str(), nullptr)) s.clear();
    list_dir(s);
    go(SCR_FOLDER);
}

void folder_frame() {
    App &a = app();
    const Palette &p = pal();
    int n = (int)g_entries.size();
    const int kVisible = 8;
    switch (a.nav) {
    case NAV_UP: if (g_sel > 0) g_sel--; break;
    case NAV_DOWN: if (g_sel < n - 1) g_sel++; break;
    case NAV_L: g_sel = std::max(0, g_sel - kVisible); break;
    case NAV_R: g_sel = std::min(n - 1, g_sel + kVisible); break;
    case NAV_A: if (n) { list_dir(g_entries[g_sel].path); return; } break;
    case NAV_B:
        if (g_dir.empty()) { go_back(); return; }
        else {
            std::string from = g_dir;
            list_dir(parent_of(g_dir));
            for (int i = 0; i < (int)g_entries.size(); ++i) if (g_entries[i].path == from) g_sel = i;
        }
        break;
    case NAV_X:
        if (!g_dir.empty()) {
            if (g_for == FOLDER_COVERS) {
                cfg().cover_dir = g_dir;
                covers_reload();
                toast("Cover art folder set");
            } else {
                cfg().library_dir = g_dir;
                toast("New ports will install into " + g_dir, 3.5);
            }
            config_save();
            go_back();
            return;
        }
        break;
    default: break;
    }
    if (g_sel < g_top) g_top = g_sel;
    if (g_sel >= g_top + kVisible) g_top = g_sel - kVisible + 1;

    background(0);
    status_bar();
    text(96, 92, 26, p.text, g_for == FOLDER_COVERS ? "Choose your cover-art folder" : "Choose where new ports install");
    text(96, 130, 16, p.soft, g_dir.empty() ? "This PC" : fit(16, g_dir, 1000).c_str());
    if (!g_dir.empty() && g_for == FOLDER_COVERS) {
        std::string s = std::to_string(g_pngs) + (g_pngs == 1 ? " picture here" : " pictures here");
        text(1184, 130, 16, g_pngs ? p.ok : p.dim, s.c_str(), 1);
    }
    for (int i = g_top; i < n && i < g_top + kVisible; ++i) {
        float y = 168 + (i - g_top) * 56;
        if (hover(96, y, 1088, 48)) {
            if (a.mouse_moved) g_sel = i;
            if (a.clicked) { list_dir(g_entries[i].path); return; }
        }
        rect(96, y, 1088, 48, p.panel, 12);
        icon(g_entries[i].icon, 128, y + 24, 22, p.soft);
        text(156, y + 13, 18, p.text, fit(18, g_entries[i].name, 980).c_str());
        if (i == g_sel) focus_ring(96, y, 1088, 48, 12, 3);
    }
    if (!n) text(96, 180, 18, p.dim, "No folders here.");
    if (n > kVisible) {   // where in the list
        float h = 56 * kVisible - 8, bh = h * kVisible / n;
        rect(1196, 168, 4, h, p.line, 2);
        rect(1196, 168 + (h - bh) * g_top / std::max(1, n - kVisible), 4, bh, p.soft, 2);
    }
    if (g_dir.empty()) hint_bar({{NAV_A, "Open"}, {NAV_B, "Cancel"}});
    else hint_bar({{NAV_X, "Use this folder"}, {NAV_A, "Open"}, {NAV_B, "Up"}});
}

// ======================= Controllers
void controllers_frame() {
    App &a = app();
    const Palette &p = pal();
    if (a.nav == NAV_B) {   // B leaves (held, it still lights in the test below)
        go_back();
        return;
    }
    background(0);
    status_bar();
    text(96, 92, 26, p.text, "Controllers");
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (!count) text(96, 140, 18, p.soft, "No controller connected. Sanctum also works with a keyboard and mouse.");
    for (int i = 0; ids && i < count && i < 4; ++i) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
        if (!g) continue;
        float y = 140 + i * 120;
        rect(96, y, 1088, 104, p.panel, 16);
        icon(IC_PAD, 150, y + 52, 52, p.text);
        const char *name = SDL_GetGamepadName(g);
        text(200, y + 18, 20, p.text, name ? name : "Controller");
        // a live test: each button lights while held
        struct { SDL_GamepadButton b; const char *l; } bs[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, "S"}, {SDL_GAMEPAD_BUTTON_EAST, "E"}, {SDL_GAMEPAD_BUTTON_WEST, "W"},
            {SDL_GAMEPAD_BUTTON_NORTH, "N"}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, "L"}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, "R"},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, "Up"}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, "Dn"}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, "Lt"},
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, "Rt"}, {SDL_GAMEPAD_BUTTON_START, "+"}, {SDL_GAMEPAD_BUTTON_BACK, "-"}};
        float bx = 200;
        for (auto &b : bs) {
            bool down = SDL_GetGamepadButton(g, b.b);
            rect(bx, y + 56, 52, 32, down ? p.accent : p.panel2, 16);
            text(bx + 26, y + 62, 15, down ? IM_COL32_WHITE : p.soft, b.l, 0.5f);
            bx += 60;
        }
        float lx = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f, ly = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        circle(1120, y + 52, 30, p.panel2);
        circle(1120 + lx * 20, y + 52 + ly * 20, 10, p.focus);
    }
    SDL_free(ids);
    text(96, 640 - 26, 15, p.dim, cfg().pad_layout == 1 ? "Button layout: Nintendo, so the right face button confirms and the bottom one goes back."
                                  : cfg().pad_layout == 2 ? "Button layout: Xbox, so the bottom face button confirms and the right one goes back."
                                  : "Button layout: Automatic, so A (or Cross) confirms where the pad prints it. System Settings > Controllers changes it.");
    hint_bar({{NAV_B, "Back"}});
}

// ======================= pages still to come
void soon_frame() {
    App &a = app();
    const Palette &p = pal();
    if (a.nav == NAV_B || a.nav == NAV_A || a.clicked) { go_back(); return; }
    background(0);
    status_bar();
    text(640, 280, 32, p.text, a.soon_title.c_str(), 0.5f);
    text(640, 330, 18, p.soft, "This page comes in a later build step of SteelTemple Sanctum.", 0.5f);
    hint_bar({{NAV_B, "Back"}});
}

}  // namespace sanctum
