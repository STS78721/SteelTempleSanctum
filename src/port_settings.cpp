#include "port_settings.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>

#include "app.h"
#include "catalog.h"
#include "config.h"
#include "draw.h"
#include "json.h"
#include "launch.h"

namespace sanctum {

// ======================= the ini
static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
static std::string lower(std::string s) {
    for (char &c : s) c = (char)SDL_tolower((unsigned char)c);
    return s;
}
// the key of a "key = value" line, lower case; "" for comments, sections and blank lines
static std::string line_key(const std::string &l) {
    std::string t = trim(l);
    if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') return {};
    size_t eq = t.find('=');
    return eq == std::string::npos ? std::string() : lower(trim(t.substr(0, eq)));
}

bool Ini::load(const std::string &p) {
    path = p, lines.clear();
    size_t n = 0;
    char *d = (char *)SDL_LoadFile(p.c_str(), &n);
    if (!d) return false;
    std::string all(d, n);
    SDL_free(d);
    crlf = all.find("\r\n") != std::string::npos;   // written back with the file's own line endings
    size_t at = 0;
    while (at < all.size()) {
        size_t end = all.find('\n', at);
        if (end == std::string::npos) end = all.size();
        std::string l = all.substr(at, end - at);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        at = end + 1;
    }
    return true;
}

bool Ini::get(const std::string &key, std::string &out) const {
    std::string k = lower(key);
    bool found = false;
    for (const std::string &l : lines)
        if (line_key(l) == k) {
            std::string v = trim(l.substr(l.find('=') + 1));
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
            out = v, found = true;   // a repeated key: the later line wins, as in cfg.c
        }
    return found;
}

void Ini::set(const std::string &key, const std::string &value, const std::string &section) {
    std::string k = lower(key);
    for (int i = (int)lines.size() - 1; i >= 0; --i)
        if (line_key(lines[i]) == k) {   // the line in effect, rewritten in its own style ("key = value")
            std::string l = lines[i];
            size_t eq = l.find('=');
            lines[i] = l.substr(0, eq + 1) + (eq + 1 < l.size() && l[eq + 1] == ' ' ? " " : "") + value;
            return;
        }
    // not in the file yet: at the end of its section, or a new section at the end
    std::string sec = section.empty() ? "settings" : section;
    int in_sec = -1, last = -1;
    for (int i = 0; i < (int)lines.size(); ++i) {
        std::string t = trim(lines[i]);
        if (!t.empty() && t[0] == '[') in_sec = lower(t) == "[" + lower(sec) + "]" ? i : -1;
        else if (in_sec >= 0 && !t.empty()) last = i;
        if (in_sec >= 0 && last < in_sec) last = in_sec;
    }
    std::string line = k + " = " + value;
    if (last >= 0) lines.insert(lines.begin() + last + 1, line);
    else {
        if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
        lines.push_back("[" + sec + "]");
        lines.push_back(line);
    }
}

void Ini::remove(const std::string &key) {
    std::string k = lower(key);
    lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const std::string &l) { return line_key(l) == k; }), lines.end());
}

bool Ini::save() {
    std::string s;
    for (size_t i = 0; i < lines.size(); ++i) s += lines[i] + (crlf ? "\r\n" : "\n");
    std::string tmp = path + ".sanctum-tmp";
    if (!SDL_SaveFile(tmp.c_str(), s.data(), s.size())) return false;
    return SDL_RenamePath(tmp.c_str(), path.c_str());   // replaces it in one step
}

// ======================= the lists
static std::map<std::string, PortSettings> g_lists;

const PortSettings *port_settings(const char *id) {
    auto it = g_lists.find(id);
    if (it != g_lists.end()) return it->second.loaded ? &it->second : nullptr;
    PortSettings &ps = g_lists[id];
    std::string path = std::string(SDL_GetBasePath()) + "assets\\settings\\" + id + ".json";
    size_t n = 0;
    char *d = (char *)SDL_LoadFile(path.c_str(), &n);
    if (!d) return nullptr;
    Json j;
    bool ok = json_parse(std::string(d, n), j);
    SDL_free(d);
    if (!ok) return nullptr;
    ps.ini = j["ini"].str(), ps.prefix = j["prefix"].str(), ps.read_from = j["read_from"].str();
    for (const Json &pg : j["pages"].arr) {
        SetPage page;
        page.title = pg["title"].str();
        for (const Json &r : pg["rows"].arr) {
            SetRow row;
            row.type = r["type"].str(), row.key = r["key"].str(), row.ini = r["ini"].str(), row.section = r["section"].str();
            row.label = r["label"].str(), row.desc = r["desc"].str(), row.def = r["default"].str(), row.fmt = r["fmt"].str();
            for (const Json &v : r["values"].arr) row.values.push_back(v.str());
            for (const Json &v : r["names"].arr) row.names.push_back(v.str());
            row.lo = (float)r["lo"].n, row.hi = (float)r["hi"].n, row.step = r["step"].type == Json::NUM ? (float)r["step"].n : 1;
            row.live = r["live"].b, row.inverted = r["inverted"].b;
            if (row.type != "group") ps.count++;
            page.rows.push_back(row);
        }
        ps.pages.push_back(page);
    }
    ps.loaded = !ps.pages.empty();
    return ps.loaded ? &ps : nullptr;
}

static std::string ini_path(int port) {
    const Port &p = ports()[port];
    const Install *in = p.best();
    const PortSettings *s = port_settings(p.id);
    return in && s ? in->dir + "\\" + s->ini : std::string();
}

int port_settings_changed(int port) {
    const PortSettings *s = port_settings(ports()[port].id);
    Ini ini;
    if (!s || !ini.load(ini_path(port))) return 0;
    int n = 0;
    std::string v;
    for (auto &pg : s->pages)
        for (auto &r : pg.rows)
            if (r.type != "group" && ini.get(r.ini, v)) ++n;
    return n;
}

// ======================= the page
static int g_port = -1, g_page = 0, g_row = 0;
static bool g_in_rows = false, g_backed_up = false;
static float g_scroll = 0;
static Ini g_ini;
static bool g_has_ini = false;

void port_settings_open(int port) {
    g_port = port, g_page = 0, g_row = 0, g_in_rows = false, g_scroll = 0;
    g_has_ini = g_ini.load(ini_path(port));
    if (!g_has_ini) g_ini.path = ini_path(port), g_ini.lines.clear();
    g_backed_up = false;
    go(SCR_PORT_SETTINGS);
}

static std::string current(const SetRow &r, bool *set_by_player = nullptr) {
    std::string v;
    bool has = g_ini.get(r.ini, v);
    if (set_by_player) *set_by_player = has;
    return has ? v : r.def;
}

static bool env_fixed(const PortSettings &s, const SetRow &r) {   // the environment wins over the ini
    return SDL_getenv((s.prefix + r.key).c_str()) != nullptr;
}

static int choice_index(const SetRow &r, const std::string &v) {
    for (int i = 0; i < (int)r.values.size(); ++i)
        if (lower(r.values[i]) == lower(v)) return i;
    if (r.type == "toggle") return v == "0" || lower(v) == "false" || lower(v) == "off" ? 0 : 1;
    return -1;
}

static std::string shown(const SetRow &r, const std::string &v) {
    if (r.type == "toggle") return "";
    if (r.type == "slider") {
        char b[64];
        std::string f = r.fmt;
        // a plain printf number format only ("%.0f %%", "%.2f"): anything else is shown as a bare number
        bool safe = f.find('%') != std::string::npos;
        for (size_t i = 0; safe && i < f.size(); ++i)
            if (f[i] == '%') {
                if (i + 1 < f.size() && f[i + 1] == '%') { ++i; continue; }
                size_t j = i + 1;
                while (j < f.size() && (f[j] == '+' || f[j] == '-' || f[j] == ' ' || f[j] == '#')) ++j;   // flags
                while (j < f.size() && (SDL_isdigit((unsigned char)f[j]) || f[j] == '.')) ++j;
                if (j >= f.size() || f[j] != 'f') safe = false;
                i = j;
            }
        SDL_snprintf(b, sizeof b, safe ? f.c_str() : "%.2f", (float)atof(v.c_str()));
        return b;
    }
    int i = choice_index(r, v);
    if (i >= 0 && i < (int)r.names.size()) return r.names[i];
    return v.empty() ? std::string("Automatic") : v + " (set by hand)";
}

static std::string number(float v, float step) {
    char b[32];
    if (std::fabs(step - std::round(step)) < 1e-4f) SDL_snprintf(b, sizeof b, "%d", (int)std::lround(v));
    else {
        SDL_snprintf(b, sizeof b, "%.3f", v);
        std::string s = b;
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
        return s;
    }
    return b;
}

// writes v (empty: back to the game's default, the key removed); returns false if the file could not be written
static bool write_value(const SetRow &r, const std::string &v, bool reset) {
    if (!g_backed_up && g_has_ini) {   // the player's file as it was, once per visit
        std::string bak = g_ini.path + ".sanctum-bak";
        size_t n = 0;
        if (void *d = SDL_LoadFile(g_ini.path.c_str(), &n)) { SDL_SaveFile(bak.c_str(), d, n); SDL_free(d); }
        g_backed_up = true;
    }
    if (reset) g_ini.remove(r.ini);
    else g_ini.set(r.ini, v, r.section);
    if (!g_ini.save()) { toast("Could not write " + g_ini.path, 4); g_ini.load(g_ini.path); return false; }
    g_has_ini = true;
    return true;
}

static void step(const SetRow &r, int dir) {
    std::string v = current(r);
    if (r.type == "toggle") {
        int on = choice_index(r, v);
        write_value(r, on ? "0" : "1", false);
    } else if (r.type == "choice") {
        int n = (int)r.values.size(), i = choice_index(r, v);
        i = i < 0 ? 0 : (i + dir + n) % n;
        if (r.values[i].empty()) write_value(r, "", true);   // the automatic choice: the key unset
        else write_value(r, r.values[i], false);
    } else if (r.type == "slider") {
        float x = (float)atof(v.c_str()) + dir * r.step;
        x = std::min(r.hi, std::max(r.lo, x));
        write_value(r, number(x, r.step), false);
    }
}

void port_settings_frame() {
    App &a = app();
    const Palette &p = pal();
    if (g_port < 0) { go_back(); return; }
    const Port &port = ports()[g_port];
    const PortSettings *s = port_settings(port.id);
    if (!s) { go_back(); return; }
    bool locked = running_port() == g_port;   // the game reads its ini at start and writes it from its own menu

    const SetPage &page = s->pages[std::min(g_page, (int)s->pages.size() - 1)];
    auto focusable = [&](int i) { return page.rows[i].type != "group"; };
    auto first = [&]() { for (int i = 0; i < (int)page.rows.size(); ++i) if (focusable(i)) return i; return 0; };
    if (g_row >= (int)page.rows.size() || !focusable(g_row)) g_row = first();

    switch (a.nav) {
    case NAV_UP:
        if (!g_in_rows) g_page = std::max(0, g_page - 1), g_row = 0, g_scroll = 0;
        else for (int i = g_row - 1; i >= 0; --i) if (focusable(i)) { g_row = i; break; }
        break;
    case NAV_DOWN:
        if (!g_in_rows) g_page = std::min((int)s->pages.size() - 1, g_page + 1), g_row = 0, g_scroll = 0;
        else for (int i = g_row + 1; i < (int)page.rows.size(); ++i) if (focusable(i)) { g_row = i; break; }
        break;
    case NAV_L: g_page = (g_page + (int)s->pages.size() - 1) % (int)s->pages.size(), g_row = 0, g_scroll = 0; break;
    case NAV_R: g_page = (g_page + 1) % (int)s->pages.size(), g_row = 0, g_scroll = 0; break;
    case NAV_RIGHT:
    case NAV_LEFT:
        if (!g_in_rows) { if (a.nav == NAV_RIGHT) g_in_rows = true; break; }
        if (locked) { toast("Close the game to change its settings"); break; }
        if (!env_fixed(*s, page.rows[g_row])) step(page.rows[g_row], a.nav == NAV_RIGHT ? 1 : -1);
        break;
    case NAV_A:
        if (!g_in_rows) { g_in_rows = true; break; }
        if (locked) { toast("Close the game to change its settings"); break; }
        if (!env_fixed(*s, page.rows[g_row])) step(page.rows[g_row], 1);
        break;
    case NAV_Y:
        if (g_in_rows && !locked) {
            write_value(page.rows[g_row], "", true);
            toast("Back to the game's default");
        }
        break;
    case NAV_B:
        if (g_in_rows) g_in_rows = false;
        else { go_back(); return; }
        break;
    default: break;
    }

    // ---- the page, in the port's colours
    ImU32 acc = port.colours.accent, deep = port.colours.deep;
    ImU32 bg = mix(p.bg, deep, p.light ? 0.10f : 0.55f), panel = mix(p.panel, deep, p.light ? 0.06f : 0.35f);
    dl()->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, bg);
    dl()->AddRectFilledMultiColor(P(0, 0), P(1280, 300), with_alpha(acc, 40), with_alpha(acc, 10), with_alpha(acc, 0), with_alpha(acc, 0));
    status_bar();
    tile(port, 58, 84, 52, 1, false);
    text(124, 86, 14, acc, (std::string("STEELTEMPLE  ") + port.name).c_str());
    text(124, 104, 24, p.text, "Game settings");
    std::string where = g_has_ini ? g_ini.path : "No " + s->ini + " yet: the first change creates it";
    text(1236, 108, 13, p.dim, fit(13, where, 640).c_str(), 1);

    // the pages
    rect(340, 150, 1, 500, with_alpha(p.line, 160));
    for (int i = 0; i < (int)s->pages.size() && i < 10; ++i) {
        float y = 150 + i * 48;
        if (hover(44, y, 280, 42) && (a.mouse_moved || a.clicked)) g_page = i, g_in_rows = false, g_scroll = 0;
        bool sel = i == g_page;
        if (sel) { rect(44, y, 280, 42, panel, 12); rect(44, y + 8, 4, 26, acc, 2); }
        text(68, y + 11, 17, sel ? p.text : p.soft, fit(17, s->pages[i].title, 240).c_str());
        if (sel && !g_in_rows) focus_ring(44, y, 280, 42, 12, 3);
    }

    // the rows, scrolled to keep the focused one in view
    float x = 372, w = 864, top = 150, bottom = 532;
    float yy = 0, focus_y = 0;
    std::vector<float> ys;
    for (int i = 0; i < (int)page.rows.size(); ++i) {
        ys.push_back(yy);
        if (i == g_row) focus_y = yy;
        yy += page.rows[i].type == "group" ? 40 : 52;
    }
    float want = std::max(0.0f, std::min(focus_y - 120, yy - (bottom - top)));
    g_scroll = ease(g_scroll, want, 16);
    dl()->PushClipRect(P(x - 10, top - 6), P(x + w + 10, bottom + 6), true);
    for (int i = 0; i < (int)page.rows.size(); ++i) {
        const SetRow &r = page.rows[i];
        float y = top + ys[i] - g_scroll;
        if (y < top - 60 || y > bottom) continue;
        if (r.type == "group") { text(x + 4, y + 14, 15, acc, r.label.c_str()); continue; }
        bool f = g_in_rows && i == g_row, mine = false;
        std::string v = current(r, &mine);
        bool fixed = env_fixed(*s, r);
        if (hover(x, y, w, 44) && !locked) {
            if (a.mouse_moved) g_in_rows = true, g_row = i;
            if (a.clicked && !fixed) { g_in_rows = true, g_row = i; step(r, 1); v = current(r, &mine); }
        }
        rect(x, y, w, 44, panel, 10);
        ImU32 lc = fixed || locked ? p.dim : p.text;
        text(x + 18, y + 12, 16, lc, fit(16, r.label, 420).c_str());
        if (mine) circle(x + 8, y + 22, 3, acc);   // set by the player (not the default)
        float rx = x + w - 18;
        if (r.type == "toggle") {
            bool on = choice_index(r, v) == 1;
            if (r.inverted) on = !on;
            rect(rx - 52, y + 9, 52, 26, on ? acc : p.line, 13);
            circle(on ? rx - 13 : rx - 39, y + 22, 10, IM_COL32_WHITE);
        } else if (r.type == "slider") {
            float t = r.hi > r.lo ? ((float)atof(v.c_str()) - r.lo) / (r.hi - r.lo) : 0;
            t = std::min(1.0f, std::max(0.0f, t));
            float bx = rx - 300;
            rect(bx, y + 20, 200, 4, p.line, 2);
            rect(bx, y + 20, 200 * t, 4, acc, 2);
            circle(bx + 200 * t, y + 22, 8, f ? acc : p.soft);
            text(rx, y + 13, 15, p.soft, shown(r, v).c_str(), 1);
        } else {
            std::string sv = shown(r, v);
            float tw = text(rx - (f ? 18 : 0), y + 13, 15, mine ? acc : p.soft, fit(15, sv, 360).c_str(), 1);
            if (f) {   // the arrows a console menu shows on the value being changed
                text(rx, y + 12, 16, p.soft, ">", 1);
                text(rx - 18 - tw - 10, y + 12, 16, p.soft, "<", 1);
            }
        }
        if (f) focus_ring(x, y, w, 44, 10, 3);
    }
    dl()->PopClipRect();

    // what the focused row does
    const SetRow &fr = page.rows[g_row];
    rect(x, 548, w, 92, panel, 12);
    if (g_in_rows) {
        float h = text_wrap(x + 18, 560, 14, p.soft, fr.desc.c_str(), w - 36);
        std::string foot = "Default: " + (fr.type == "toggle" ? std::string(fr.def == "1" ? "On" : "Off") : shown(fr, fr.def));
        if (!fr.live) foot += "  ·  applies at the next start";
        if (env_fixed(*s, fr)) foot += "  ·  fixed by " + s->prefix + fr.key + " in the environment";
        text(x + 18, std::min(560 + h + 6, 616.0f), 13, p.dim, foot.c_str());
    } else {
        text(x + 18, 560, 14, p.soft, (std::to_string(s->count) + " settings, read from the port's own menu. Changes are saved to " +
                                       s->ini + " at once; Y puts a setting back to the game's default.").c_str());
        text(x + 18, 584, 13, p.dim, fit(13, "List from " + s->read_from, w - 36).c_str());
    }
    if (locked) text(x + 18, 616, 13, p.warn, "The game is running: close it to change its settings.");

    if (g_in_rows) hint_bar({{NAV_A, "Change"}, {NAV_Y, "Default"}, {NAV_L, "Pages"}, {NAV_B, "Back"}});
    else hint_bar({{NAV_A, "Open"}, {NAV_L, "Pages"}, {NAV_B, "Back"}});
}

}  // namespace sanctum
