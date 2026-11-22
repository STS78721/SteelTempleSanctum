#include "mods.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <map>
#include <vector>

#include "app.h"
#include "catalog.h"
#include "config.h"
#include "draw.h"
#include "launch.h"
#include "sound.h"

namespace sanctum {

struct ModEntry {
    std::string name;
    std::string path;
    bool enabled = true;
    bool suffix_disabled = false;   // true if stored as mods\<name>.disabled rather than mods\.disabled\<name>
};

static bool ends_with_ci(const std::string &s, const char *suf) {
    size_t sl = SDL_strlen(suf);
    return s.size() > sl && SDL_strcasecmp(s.c_str() + s.size() - sl, suf) == 0;
}

static std::vector<ModEntry> scan_mods(const std::string &install_dir) {
    std::vector<ModEntry> out;
    if (install_dir.empty()) return out;
    std::string root = install_dir + "\\mods";
    auto add_from = [&](const std::string &base, bool enabled_dir) {
        int n = 0;
        char **names = SDL_GlobDirectory((base + "\\").c_str(), "*", 0, &n);
        for (int i = 0; names && i < n; ++i) {
            std::string nm = names[i];
            if (nm.empty() || nm[0] == '.' || nm[0] == '$') continue;
            std::string full = base + "\\" + nm;
            SDL_PathInfo info{};
            if (!SDL_GetPathInfo(full.c_str(), &info) || info.type != SDL_PATHTYPE_DIRECTORY) continue;
            ModEntry e;
            if (enabled_dir && ends_with_ci(nm, ".disabled")) {
                e.name = nm.substr(0, nm.size() - 9);
                e.path = full;
                e.enabled = false;
                e.suffix_disabled = true;
            } else {
                e.name = nm;
                e.path = full;
                e.enabled = enabled_dir;
                e.suffix_disabled = false;
            }
            out.push_back(std::move(e));
        }
        SDL_free(names);
    };
    add_from(root, true);
    add_from(root + "\\.disabled", false);
    std::sort(out.begin(), out.end(), [](const ModEntry &a, const ModEntry &b) {
        int c = SDL_strcasecmp(a.name.c_str(), b.name.c_str());
        if (c != 0) return c < 0;
        return a.enabled && !b.enabled;
    });
    return out;
}

// ---- cached summary for Game page / sidebar (avoids hitting disk every frame)
struct ModCache {
    int total = 0, enabled = 0;
    double at = -100;
};
static std::map<std::string, ModCache> g_cache;

void mods_invalidate_cache() { g_cache.clear(); }

int mods_summary(const std::string &dir, int *enabled_out) {
    if (dir.empty()) {
        if (enabled_out) *enabled_out = 0;
        return 0;
    }
    double now = app().now;
    auto it = g_cache.find(dir);
    if (it != g_cache.end() && now - it->second.at < 2.0) {
        if (enabled_out) *enabled_out = it->second.enabled;
        return it->second.total;
    }
    auto list = scan_mods(dir);
    int en = 0;
    for (const ModEntry &m : list) en += m.enabled ? 1 : 0;
    g_cache[dir] = {(int)list.size(), en, now};
    if (enabled_out) *enabled_out = en;
    return (int)list.size();
}

// ---- SCR_MODS state
static int g_port_idx = 0;          // index into installed ports list
static int g_row = 0, g_top = 0;
static bool g_in_rows = false;
static int g_loaded_port = -1;      // port id whose mods are currently in g_mods
static std::vector<ModEntry> g_mods;

static std::vector<int> installed_ports() {
    std::vector<int> v;
    for (int i = 0; i < (int)ports().size(); ++i)
        if (!ports()[i].installs.empty()) v.push_back(i);
    return v;
}

static void reload_mods(int port_id) {
    g_loaded_port = port_id;
    g_mods.clear();
    if (port_id >= 0 && port_id < (int)ports().size()) {
        if (const Install *in = ports()[port_id].best()) {
            g_mods = scan_mods(in->dir);
            int en = 0;
            for (const ModEntry &m : g_mods) en += m.enabled ? 1 : 0;
            g_cache[in->dir] = {(int)g_mods.size(), en, app().now};
        }
    }
    if (g_row >= (int)g_mods.size()) g_row = std::max(0, (int)g_mods.size() - 1);
    if (g_mods.empty()) g_in_rows = false, g_row = 0, g_top = 0;
}

void mods_open(int port) {
    auto inst = installed_ports();
    g_port_idx = 0;
    g_in_rows = false;
    g_row = 0, g_top = 0;
    for (int i = 0; i < (int)inst.size(); ++i)
        if (inst[i] == port) { g_port_idx = i; break; }
    g_loaded_port = -1;
    if (!inst.empty()) {
        reload_mods(inst[g_port_idx]);
        if (port >= 0 && !g_mods.empty()) g_in_rows = true;
    }
    go(SCR_MODS);
}

static void toggle_mod(int port_id, int idx) {
    if (idx < 0 || idx >= (int)g_mods.size()) return;
    if (running_port() == port_id) {
        sound_play(SFX_ERROR);
        toast("Close the game to change its mods");
        return;
    }
    if (demo_mode()) {
        sound_play(SFX_ERROR);
        toast("Demo mode: mods are not changed");
        return;
    }
    const Install *in = ports()[port_id].best();
    if (!in) return;
    ModEntry &m = g_mods[idx];
    std::string mods_dir = in->dir + "\\mods";
    std::string dis_dir = mods_dir + "\\.disabled";
    std::string dst;
    if (m.enabled) {
        SDL_CreateDirectory(dis_dir.c_str());
        dst = dis_dir + "\\" + m.name;
    } else {
        dst = mods_dir + "\\" + m.name;
    }
    if (SDL_GetPathInfo(dst.c_str(), nullptr)) {
        sound_play(SFX_ERROR);
        toast("A folder named \"" + m.name + "\" already exists at the destination");
        return;
    }
    if (!SDL_RenamePath(m.path.c_str(), dst.c_str())) {
        sound_play(SFX_ERROR);
        toast("Could not move mod (is a file open in it?)");
        return;
    }
    bool now_on = !m.enabled;
    sound_play(now_on ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
    reload_mods(port_id);
}

static void open_mods_folder(int port_id) {
    if (demo_mode()) {
        sound_play(SFX_ERROR);
        toast("Demo mode: folders don't open");
        return;
    }
    const Install *in = ports()[port_id].best();
    if (!in) return;
    std::string mods_dir = in->dir + "\\mods";
    SDL_CreateDirectory(mods_dir.c_str());
    sound_play(SFX_OK);
    SDL_OpenURL(("file:///" + mods_dir).c_str());
}

void mods_frame() {
    App &a = app();
    const Palette &p = pal();
    auto inst = installed_ports();
    if (inst.empty()) {
        if (a.nav == NAV_B) { go_back(); return; }
        background(0);
        status_bar();
        text(96, 92, 28, p.text, "Mods");
        text(96, 148, 18, p.soft, "No ports installed on this PC yet. Download a port in Get ports first.");
        hint_bar({{NAV_B, "Back"}});
        return;
    }
    if (g_port_idx >= (int)inst.size()) g_port_idx = (int)inst.size() - 1;
    int pid = inst[g_port_idx];
    if (g_loaded_port != pid) reload_mods(pid);

    const Port &port = ports()[pid];
    const Install *in = port.best();
    int n = (int)g_mods.size();
    const int kVisible = 8;

    switch (a.nav) {
    case NAV_UP:
        if (!g_in_rows) {
            if (g_port_idx > 0) { g_port_idx--; reload_mods(inst[g_port_idx]); sound_play(SFX_MOVE); }
            else sound_play(SFX_EDGE);
        } else {
            if (g_row > 0) { g_row--; sound_play(SFX_MOVE); }
            else sound_play(SFX_EDGE);
        }
        break;
    case NAV_DOWN:
        if (!g_in_rows) {
            if (g_port_idx < (int)inst.size() - 1) { g_port_idx++; reload_mods(inst[g_port_idx]); sound_play(SFX_MOVE); }
            else sound_play(SFX_EDGE);
        } else {
            if (g_row < n - 1) { g_row++; sound_play(SFX_MOVE); }
            else sound_play(SFX_EDGE);
        }
        break;
    case NAV_RIGHT:
    case NAV_A:
        if (!g_in_rows) {
            if (n > 0) { g_in_rows = true; sound_play(SFX_OK); }
            else if (a.nav == NAV_A) open_mods_folder(pid);
            else sound_play(SFX_EDGE);
        } else if (a.nav == NAV_A) {
            toggle_mod(pid, g_row);
        }
        break;
    case NAV_LEFT:
        if (g_in_rows) { g_in_rows = false; sound_play(SFX_BACK); }
        break;
    case NAV_X:
        open_mods_folder(pid);
        break;
    case NAV_Y:
        mods_invalidate_cache();
        reload_mods(pid);
        sound_play(SFX_OK);
        toast("Refreshed mods");
        break;
    case NAV_B:
        if (g_in_rows) { g_in_rows = false; sound_play(SFX_BACK); }
        else { go_back(); return; }
        break;
    default: break;
    }

    pid = inst[g_port_idx];
    const Port &cur = ports()[pid];
    in = cur.best();
    n = (int)g_mods.size();
    if (g_row < g_top) g_top = g_row;
    if (g_row >= g_top + kVisible) g_top = g_row - kVisible + 1;

    background(cfg().follow ? cur.colours.accent : p.accent);
    status_bar();
    text(58, 96, 26, p.text, "Mods");
    rect(340, 76, 1, 580, p.line);

    for (int i = 0; i < (int)inst.size() && i < 9; ++i) {
        const Port &sp = ports()[inst[i]];
        float y = 146 + i * 54;
        if (hover(44, y, 280, 46)) {
            if (a.mouse_moved && !a.clicked && (g_port_idx != i || g_in_rows)) {
                g_port_idx = i, g_in_rows = false;
                reload_mods(inst[i]);
                sound_play(SFX_MOVE);
            }
            if (a.clicked) {
                if (g_port_idx != i) { g_port_idx = i; reload_mods(inst[i]); sound_play(SFX_MOVE); }
                g_in_rows = false;
            }
        }
        bool sel = i == g_port_idx;
        if (sel) { rect(44, y, 280, 46, p.panel, 12); rect(44, y + 8, 4, 30, sp.colours.accent, 2); }
        text(66, y + 12, 17, sel ? p.text : p.soft, fit(17, sp.name, 195).c_str());
        int en = 0, tot = sp.best() ? mods_summary(sp.best()->dir, &en) : 0;
        std::string badge = tot ? (std::to_string(en) + "/" + std::to_string(tot)) : "0";
        text(310, y + 14, 14, sel ? sp.colours.accent : p.dim, badge.c_str(), 1);
        if (sel && !g_in_rows) focus_ring(44, y, 280, 46, 12, 3);
    }

    float x = 384, w = 852;
    text(x, 96, 26, p.text, fit(26, cur.title, 480).c_str());
    if (in) {
        std::string dir_lbl = shown_path(in->dir + "\\mods", (std::string(cur.title) + "\\mods").c_str());
        text(x + w, 104, 14, p.dim, fit(14, dir_lbl, 360).c_str(), 1);
    }

    if (n == 0) {
        rect(x, 146, w, 112, p.panel, 14);
        icon(IC_MODS, x + 42, 202, 28, cur.colours.accent);
        text(x + 76, 174, 18, p.text, "No mods in this port's mods folder yet");
        text(x + 76, 206, 15, p.soft, "Press X to open the mods folder and drop mod folders inside.");
    } else {
        for (int i = g_top; i < n && i < g_top + kVisible; ++i) {
            const ModEntry &m = g_mods[i];
            float y = 146 + (i - g_top) * 56;
            bool f = g_in_rows && i == g_row;
            if (hover(x, y, w, 48)) {
                if (a.mouse_moved && !a.clicked && (!g_in_rows || g_row != i)) { g_in_rows = true, g_row = i; sound_play(SFX_MOVE); }
                if (a.clicked) { g_in_rows = true, g_row = i; toggle_mod(pid, i); break; }
            }
            rect(x, y, w, 48, p.panel, 12);
            text(x + 20, y + 13, 17, m.enabled ? p.text : p.soft, fit(17, m.name, 620).c_str());
            text(x + w - 94, y + 15, 14, m.enabled ? p.ok : p.dim, m.enabled ? "Enabled" : "Disabled", 1);
            float tx = x + w - 76;
            rect(tx, y + 10, 56, 28, m.enabled ? cur.colours.accent : p.line, 14);
            circle(m.enabled ? tx + 42 : tx + 14, y + 24, 11, IM_COL32_WHITE);
            if (f) focus_ring(x, y, w, 48, 12, 3);
        }
        if (n > kVisible) {
            float h = 56 * kVisible - 8, bh = h * kVisible / n;
            rect(x + w + 12, 146, 4, h, p.line, 2);
            rect(x + w + 12, 146 + (h - bh) * g_top / std::max(1, n - kVisible), 4, bh, p.soft, 2);
        }
    }
    if (running_port() == pid) text(x, 614, 14, p.warn, "The game is running: close it to enable or disable mods.");

    if (g_in_rows) hint_bar({{NAV_A, "Toggle"}, {NAV_X, "Open folder"}, {NAV_Y, "Refresh"}, {NAV_B, "Back"}});
    else hint_bar({{NAV_A, n ? "Open" : "Open folder"}, {NAV_X, "Open folder"}, {NAV_Y, "Refresh"}, {NAV_B, "Back"}});
}

}  // namespace sanctum
