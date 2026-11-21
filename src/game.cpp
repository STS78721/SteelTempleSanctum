// A port's page: its cover, what is installed, one big main action (Play, Download or Update), and tabs (L/R).
// Overview lists every copy found on this PC (up/down picks which one Play starts), so a second install is never
// needed. Release notes: every release from GitHub, the list on the left, the notes on the right; A moves into the
// notes, up/down scroll them, B comes back out.
#include <algorithm>
#include <cmath>

#include "config.h"
#include "covers.h"
#include "draw.h"
#include "github.h"
#include "install.h"
#include "launch.h"
#include "port_settings.h"

namespace sanctum {

static const char *kTabs[] = {"Overview", "Release notes", "Settings", "Mods", "Files & storage"};
static const int kTabN = 5;
static int g_tab = 0, g_action = 0, g_copy = 0, g_last_game = -1, g_rel = 0;
static float g_tab_x = 0, g_scroll = 0, g_scroll_to = 0;
enum { ROW_ACTIONS, ROW_COPIES, ROW_RELEASES, ROW_NOTES, ROW_SETTINGS };
static int g_row = ROW_ACTIONS;

void game_show_overview(int port) {   // back from a game: its page, at the top, the last run first
    app().game = port;
    g_last_game = port, g_tab = 0, g_row = ROW_ACTIONS, g_action = 0;
    app().back.clear();
    app().back.push_back(SCR_HOME);
    app().screen = SCR_GAME;
}

void game_show_release(int port, const std::string &tag) {
    app().game = port;
    g_last_game = port, g_tab = 1, g_row = ROW_RELEASES, g_rel = 0, g_scroll = g_scroll_to = 0, g_action = 0, g_copy = 0;
    const auto &rs = ports()[port].releases;
    for (int i = 0; i < (int)rs.size(); ++i)
        if (rs[i].tag == tag) g_rel = i;
    go(SCR_GAME);
}

struct Action {
    Icon icon;
    std::string label;
    int id;
};
enum { ACT_PLAY, ACT_DOWNLOAD, ACT_UPDATE, ACT_LAUNCHER, ACT_GITHUB, ACT_PROGRESS, ACT_CLOSE, ACT_LOG, ACT_REPORT, ACT_ISSUE };
static double g_close_armed = -10;   // Close game asks for a second press

// GitHub's Markdown, drawn plainly: headings larger, bullets as dots, emphasis and code marks dropped, links as text
struct Line {
    std::string text;
    int kind;   // 0 text, 1 heading, 2 bullet, 3 sub-bullet, 4 blank
};
static std::vector<Line> notes_lines(const std::string &md) {
    std::vector<Line> out;
    size_t at = 0;
    while (at <= md.size()) {
        size_t end = md.find('\n', at);
        if (end == std::string::npos) end = md.size();
        std::string l = md.substr(at, end - at);
        at = end + 1;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        size_t ind = l.find_first_not_of(" \t");
        if (ind == std::string::npos) { if (!out.empty() && out.back().kind != 4) out.push_back({"", 4}); continue; }
        std::string t = l.substr(ind);
        int kind = 0;
        if (t[0] == '#') { kind = 1; t.erase(0, t.find_first_not_of("# ")); }
        else if (t.rfind("\xE2\x96\xA0", 0) == 0) { kind = 1; t.erase(0, 3); }   // "■ Major Updates"
        else if ((t[0] == '-' || t[0] == '*') && t.size() > 1 && t[1] == ' ') { kind = ind >= 2 ? 3 : 2; t.erase(0, 2); }
        else if (t == "---" || t == "***") continue;
        std::string c;
        for (size_t i = 0; i < t.size(); ++i) {
            if (t[i] == '*' || t[i] == '`' || (t[i] == '_' && i + 1 < t.size() && t[i + 1] == '_')) continue;
            if (t[i] == '[') {   // [text](url) -> text
                size_t close = t.find("](", i);
                size_t paren = close == std::string::npos ? close : t.find(')', close);
                if (paren != std::string::npos) { c += t.substr(i + 1, close - i - 1); i = paren; continue; }
            }
            unsigned char u = (unsigned char)t[i];
            if (u >= 0xE2 && u < 0xF8) {   // characters the font lacks (box shapes, emoji): left out
                int len = u >= 0xF0 ? 4 : 3;
                std::string ch = t.substr(i, len);
                bool keep = ch == "\xE2\x80\x94" || ch == "\xE2\x80\x93" || ch == "\xE2\x80\x99" || ch == "\xE2\x80\x9C" ||
                            ch == "\xE2\x80\x9D" || ch == "\xE2\x80\xA6" || ch == "\xE2\x80\xA2";
                if (keep) c += ch;
                i += len - 1;
                continue;
            }
            c += t[i];
        }
        while (!c.empty() && c.back() == ' ') c.pop_back();
        if (!c.empty() || kind == 1) out.push_back({c, kind});
    }
    return out;
}

void game_frame() {
    App &a = app();
    const Palette &p = pal();
    Port &port = ports()[a.game];
    if (g_last_game != a.game) g_tab = 0, g_action = 0, g_copy = 0, g_row = ROW_ACTIONS, g_rel = 0, g_last_game = a.game;
    if (g_copy >= (int)port.installs.size()) g_copy = 0;
    if (g_rel >= (int)port.releases.size()) g_rel = 0;
    const Install *in = port.installs.empty() ? nullptr : &port.installs[g_copy];
    PortState st = port.state();
    const Release *latest = port.latest();

    // the job for this port, if one is queued or running
    JobView job{};
    bool busy = false;
    for (auto &j : install_jobs())
        if (j.port == a.game && j.phase < PH_DONE) job = j, busy = true;

    std::vector<Action> acts;
    bool running = running_port() == a.game;
    const LastRun *lr = last_run(a.game);
    if (running) acts.push_back({IC_CLOSE, "Close game", ACT_CLOSE});
    else if (busy) acts.push_back({IC_DOWNLOAD, "Downloads", ACT_PROGRESS});
    else if (in && !can_play(port, *in) && port.launcher) acts.push_back({IC_GEAR, "Set up", ACT_PLAY});   // first start
    else if (in) acts.push_back({IC_PLAY, "Play", ACT_PLAY});
    else acts.push_back({IC_DOWNLOAD, "Download  " + port.download_size(), ACT_DOWNLOAD});
    if (!busy && st == PORT_UPDATE && latest) acts.push_back({IC_DOWNLOAD, "Update to " + latest->tag, ACT_UPDATE});
    // after a run that went wrong: what to do about it
    if (lr && !running && (lr->verdict == RUN_CRASH || lr->verdict == RUN_NO_START)) {
        if (!lr->log.empty()) acts.push_back({IC_NEWS, "Open the log", ACT_LOG});
        acts.push_back({IC_INFO, "Report it", ACT_REPORT});
    }
    if (lr && !running && (lr->verdict == RUN_KNOWN || lr->verdict == RUN_FIXED) && !lr->issue.empty())
        acts.push_back({IC_INFO, "Issue " + lr->issue, ACT_ISSUE});
    if (!running && in && in->launcher && can_play(port, *in)) acts.push_back({IC_GEAR, "Port's own launcher", ACT_LAUNCHER});
    acts.push_back({IC_INFO, "Open on GitHub", ACT_GITHUB});
    // the buttons changed (an update finished, a download started): focus goes back to the main one
    static size_t last_count = 0;
    if (acts.size() != last_count) g_action = 0, last_count = acts.size();

    auto run = [&](int id) {
        switch (id) {
        case ACT_PLAY: if (in) launch(a.window, a.game, *in, false); break;
        case ACT_LAUNCHER: if (in) launch(a.window, a.game, *in, true); break;
        case ACT_CLOSE:
            if (a.now - g_close_armed < 3) { close_game(); toast("Closing " + std::string(port.name)); g_close_armed = -10; }
            else { g_close_armed = a.now; toast("Press again to close the game (unsaved progress is lost)", 3); }
            break;
        case ACT_LOG: if (lr) SDL_OpenURL(("file:///" + lr->log).c_str()); break;
        case ACT_REPORT: SDL_OpenURL((std::string("https://github.com/") + port.repo + "/issues/new").c_str()); break;
        case ACT_ISSUE: {
            std::string n = lr ? lr->issue : "";
            if (!n.empty() && n[0] == '#') n.erase(0, 1);
            SDL_OpenURL((std::string("https://github.com/") + port.repo + "/issues/" + n).c_str());
            break;
        }
        case ACT_PROGRESS: go(SCR_DOWNLOADS); break;
        case ACT_DOWNLOAD: {
            if (!latest) { toast(github_busy() ? "Still asking GitHub, one moment" : "No release found on GitHub"); break; }
            if (const Asset *as = latest->find(ASSET_PORTABLE)) install_enqueue(a.game, *latest, *as, JOB_INSTALL);
            else if (const Asset *w = latest->find(ASSET_WIZARD)) install_enqueue(a.game, *latest, *w, JOB_WIZARD);
            else { toast("This release has no download Sanctum can install"); break; }
            toast(std::string(port.name) + " added to Downloads");
            break;
        }
        case ACT_UPDATE: {
            if (game_running()) { toast("Close the game first"); break; }
            const Install *b = port.best();
            const Asset *up = latest ? latest->find(ASSET_UPDATE) : nullptr;
            if (!b || !up) { toast("This release has no update package; Download installs it fresh"); break; }
            install_enqueue(a.game, *latest, *up, JOB_UPDATE, b->dir);
            toast(std::string(port.name) + " update added to Downloads");
            break;
        }
        case ACT_GITHUB: SDL_OpenURL((std::string("https://github.com/") + port.repo + "/releases").c_str()); break;
        }
    };

    // ---- input
    bool copies_here = g_tab == 0 && port.installs.size() > 1;
    bool notes_here = g_tab == 1 && !port.releases.empty();
    switch (a.nav) {
    case NAV_LEFT: if (g_row == ROW_ACTIONS && g_action > 0) g_action--; break;
    case NAV_RIGHT: if (g_row == ROW_ACTIONS && g_action < (int)acts.size() - 1) g_action++; break;
    case NAV_DOWN:
        if (g_row == ROW_ACTIONS && copies_here) g_row = ROW_COPIES;
        else if (g_row == ROW_ACTIONS && notes_here) g_row = ROW_RELEASES;
        else if (g_row == ROW_ACTIONS && g_tab == 2 && in) g_row = ROW_SETTINGS;
        else if (g_row == ROW_COPIES && g_copy < (int)port.installs.size() - 1) g_copy++;
        else if (g_row == ROW_RELEASES && g_rel < (int)port.releases.size() - 1) g_rel++, g_scroll_to = 0;
        else if (g_row == ROW_NOTES) g_scroll_to += 90;
        break;
    case NAV_UP:
        if (g_row == ROW_COPIES) { if (g_copy > 0) g_copy--; else g_row = ROW_ACTIONS; }
        else if (g_row == ROW_RELEASES) { if (g_rel > 0) g_rel--, g_scroll_to = 0; else g_row = ROW_ACTIONS; }
        else if (g_row == ROW_NOTES) g_scroll_to = std::max(0.0f, g_scroll_to - 90);
        else if (g_row == ROW_SETTINGS) g_row = ROW_ACTIONS;
        break;
    case NAV_L: g_tab = (g_tab + kTabN - 1) % kTabN; g_row = ROW_ACTIONS; break;
    case NAV_R: g_tab = (g_tab + 1) % kTabN; g_row = ROW_ACTIONS; break;
    case NAV_A:
        if (g_row == ROW_COPIES) {   // remembered: Play starts this copy from now on
            catalog_prefer(a.game, port.installs[g_copy].dir);
            g_copy = 0, g_row = ROW_ACTIONS, g_action = 0;
            toast("Play starts this copy from now on");
        }
        else if (g_row == ROW_RELEASES) g_row = ROW_NOTES;
        else if (g_row == ROW_ACTIONS) run(acts[g_action].id);
        else if (g_row == ROW_SETTINGS) { /* the tab body opens it (below), with a.nav still A */ }
        break;
    case NAV_Y: run(ACT_GITHUB); break;
    case NAV_B:
        if (g_row == ROW_NOTES) { g_row = ROW_RELEASES; break; }
        if (g_row != ROW_ACTIONS) { g_row = ROW_ACTIONS; break; }
        go_back();
        return;
    default: break;
    }

    // ---- draw
    background(cfg().follow ? port.colours.accent : p.accent);
    if (SDL_Texture *t = cover(port.id)) image_fade(t, 420, -280, 1340, 640, p.light ? 70 : 64);
    status_bar();
    tile(port, 76, 104, 330, 1, false);
    if (busy) {   // progress across the bottom of the cover
        float f = job.phase == PH_DOWNLOAD && job.total ? (float)job.done / job.total : -1;
        progress_bar(96, 404, 290, 10, f, port.colours.accent);
    }

    float x = 452;
    text(x, 104, 15, port.colours.accent, (std::string("STEELTEMPLE  ") + port.name).c_str());
    text(x, 128, 38, p.text, fit(38, port.title, 780).c_str());
    text(x, 178, 16, p.soft, (std::string("github.com/") + port.repo).c_str());

    float cx = x;
    auto chip = [&](const std::string &s, ImU32 fg, ImU32 bg) {
        float w = text_w(15, s.c_str()) + 28;
        rect(cx, 212, w, 32, bg, 16);
        text(cx + 14, 219, 15, fg, s.c_str());
        cx += w + 10;
    };
    if (busy) {
        static const char *ph[] = {"Waiting in Downloads", "Downloading", "Checking", "Installing", "Setup open"};
        std::string s = ph[std::min(job.phase, 4)];
        if (job.phase == PH_DOWNLOAD && job.total) s += " " + std::to_string((int)(100.0 * job.done / job.total)) + "%";
        chip(s, p.accent_text, with_alpha(p.accent, 50));
    } else if (in) chip(std::string("Installed ") + (in->version.empty() ? "" : in->version), p.ok, p.panel2);
    else chip("Not on this PC", p.soft, p.panel2);
    if (!busy && st == PORT_UPDATE) chip("Update " + port.latest_tag() + " available", p.accent_text, with_alpha(p.accent, 50));
    else if (!busy && in && latest) chip(in->version.empty() ? "Version unknown" : "Up to date", p.soft, p.panel2);
    if (running) chip("Running  ·  " + std::to_string((int)running_seconds() / 60) + " min", p.ok, with_alpha(p.ok, 40));
    else if (in && !busy && !can_play(port, *in)) chip("First start: set up with your own game files", p.warn, p.panel2);
    if (port.installs.size() > 1) chip(std::to_string(port.installs.size()) + " copies on this PC", p.soft, p.panel2);

    float ax = x;
    for (int i = 0; i < (int)acts.size(); ++i) {
        bool primary = i == 0;
        float w = text_w(19, acts[i].label.c_str()) + (primary ? 84 : 72);
        if (a.mouse_moved && hover(ax, 268, w, 58)) g_row = ROW_ACTIONS, g_action = i;
        if (a.clicked && hover(ax, 268, w, 58)) run(acts[i].id);
        rect(ax, 268 + 4, w, 58, with_alpha(IM_COL32_BLACK, p.light ? 20 : 60), 29);
        rect(ax, 268, w, 58, primary ? port.colours.accent : p.panel, 29);
        ImU32 a_ = port.colours.accent;
        bool bright = ((a_ & 255) * 299 + ((a_ >> 8) & 255) * 587 + ((a_ >> 16) & 255) * 114) / 1000 > 170;
        ImU32 fg = primary ? (bright ? IM_COL32(20, 20, 20, 255) : IM_COL32_WHITE) : p.text;
        icon(acts[i].icon, ax + 32, 297, 20, fg);
        text(ax + 52, 285, 19, fg, acts[i].label.c_str());
        if (g_row == ROW_ACTIONS && i == g_action) focus_ring(ax, 268, w, 58, 29, 5);
        ax += w + 14;
    }

    float tx = x;
    for (int i = 0; i < kTabN; ++i) {
        float w = text_w(17, kTabs[i]) + 32;
        if (a.clicked && hover(tx, 352, w, 40)) g_tab = i, g_row = ROW_ACTIONS;
        text(tx + 16, 360, 17, i == g_tab ? p.text : p.dim, kTabs[i]);
        if (i == g_tab) g_tab_x = ease(g_tab_x, tx, 20);
        tx += w;
    }
    rect(x, 394, 780, 1, p.line);
    rect(g_tab_x + 12, 391, text_w(17, kTabs[g_tab]) + 8, 4, port.colours.accent, 2);

    float by = 412;
    switch (g_tab) {
    case 0:
        // how the last run ended, at the top while it is news
        if (lr && !running && lr->verdict != RUN_NONE) {
            ImU32 c = lr->verdict == RUN_CLEAN ? p.ok : lr->verdict == RUN_FIXED ? p.accent_text : p.warn;
            std::string head;
            int m = lr->seconds / 60;
            std::string dur = m >= 60 ? std::to_string(m / 60) + " h " + std::to_string(m % 60) + " min"
                            : m ? std::to_string(m) + " min" : std::to_string(lr->seconds) + " s";
            switch (lr->verdict) {
            case RUN_CLEAN: head = "Last run ended normally  ·  " + dur; break;
            case RUN_CRASH: head = "The last run stopped with an error after " + dur; break;
            case RUN_NO_START: head = "The game could not start"; break;
            case RUN_KNOWN: head = "The last run hit a known problem (issue " + lr->issue + ")"; break;
            case RUN_FIXED: head = "The last run hit a problem fixed in " + lr->fixed_in + ": update to get the fix"; break;
            }
            int lines = 1 + !lr->detail.empty() + (lr->verdict == RUN_NO_START);
            float ch = 40 + (lines - 1) * 20;
            rect(x, by - 4, 780, ch, with_alpha(c, 34), 10);
            rect(x, by - 4, 4, ch, c, 2);
            icon(lr->verdict == RUN_CLEAN ? IC_CHECK : IC_INFO, x + 24, by + 16, 18, c);
            text(x + 46, by + 5, 16, p.text, fit(16, head, 720).c_str());
            if (!lr->detail.empty()) text(x + 46, by + 30, 13, p.soft, fit(13, lr->detail, 720).c_str());
            if (lr->verdict == RUN_NO_START)
                text(x + 46, by + 30 + (lr->detail.empty() ? 0 : 20), 13, p.dim, "Its own launcher (Set up) can check the game files.");
            by += ch + 10;
        } else if (in && last_played(a.game)) {
            text(x, by, 15, p.soft, played_text(a.game).c_str());
            by += 28;
        }
        if (port.installs.empty()) {
            text(x, by, 17, p.soft, "Not found on this PC. Sanctum looked in the Setup Wizards' entries and on every drive.");
            text(x, by + 28, 17, p.soft, "Download installs the latest release; its first start then asks for your own game files.");
        } else {
            text(x, by, 15, p.dim, port.installs.size() > 1 ? "Copies on this PC (Play starts the selected one)" : "On this PC");
            int rows = std::max(1, (int)((640 - (by + 26)) / 50));
            for (int i = 0; i < (int)port.installs.size() && i < rows; ++i) {
                const Install &c = port.installs[i];
                float y = by + 26 + i * 50;
                rect(x, y, 780, 42, i == g_copy ? p.panel : with_alpha(p.panel, 120), 10);
                if (i == g_copy) icon(IC_CHECK, x + 22, y + 21, 18, p.ok);
                text(x + 44, y + 10, 16, p.text, fit(16, c.dir, 520).c_str());
                std::string tag = (c.version.empty() ? std::string("version unknown") : c.version) +
                                  (c.registered ? "  ·  Setup Wizard" : "") + (c.dev ? "  ·  build folder" : "");
                text(x + 768, y + 11, 14, p.dim, tag.c_str(), 1);
                if (g_row == ROW_COPIES && i == g_copy) focus_ring(x, y, 780, 42, 10, 3);
                if (a.clicked && hover(x, y, 780, 42)) g_copy = i;
            }
        }
        break;
    case 1: {
        if (port.releases.empty()) {
            std::string why = github_status();
            text(x, by, 17, p.soft, github_busy() ? "Asking GitHub..." : why.empty() ? "No releases yet." : why.c_str());
            text(x, by + 28, 14, p.dim, "Press + to ask GitHub again.");
            break;
        }
        // the list
        int top = std::max(0, std::min(g_rel - 2, (int)port.releases.size() - 5));
        for (int i = top; i < (int)port.releases.size() && i < top + 5; ++i) {
            const Release &r = port.releases[i];
            float y = by + (i - top) * 46;
            if (hover(x, y, 190, 40)) { if (a.clicked) g_rel = i, g_row = ROW_NOTES, g_scroll_to = 0; }
            rect(x, y, 190, 40, i == g_rel ? p.panel : with_alpha(p.panel, 110), 10);
            text(x + 12, y + 4, 15, p.text, fit(15, r.tag, 170).c_str());
            bool have = false;
            for (auto &c : port.installs) have |= tag_key(c.version) == tag_key(r.tag);
            text(x + 12, y + 22, 12, have ? p.ok : p.dim, (r.date + (i == 0 ? "  ·  Latest" : "") + (have ? "  ·  On this PC" : "")).c_str());
            if (g_row == ROW_RELEASES && i == g_rel) focus_ring(x, y, 190, 40, 10, 3);
        }
        // the notes
        const Release &r = port.releases[g_rel];
        float nx = x + 206, nw = 574, ny = by, nh = 226;
        rect(nx, ny, nw, nh, p.panel, 12);
        if (g_row == ROW_NOTES) focus_ring(nx, ny, nw, nh, 12, 3);
        dl()->PushClipRect(P(nx, ny + 6), P(nx + nw, ny + nh - 6), true);
        g_scroll = ease(g_scroll, g_scroll_to, 16);
        float y = ny + 14 - g_scroll, tw = nw - 36;
        y += text_wrap(nx + 18, y, 18, p.text, (r.title.empty() ? r.tag : r.title).c_str(), tw) + 8;
        for (const Line &l : notes_lines(r.body)) {
            if (l.kind == 4) { y += 8; continue; }
            if (l.kind == 1) { y += 6; y += text_wrap(nx + 18, y, 16, port.colours.accent, l.text.c_str(), tw) + 4; continue; }
            float ix = l.kind == 2 ? 14 : l.kind == 3 ? 30 : 0;
            if (l.kind >= 2) circle(nx + 18 + ix - 8, y + 9, 2.2f, p.soft);
            y += text_wrap(nx + 18 + ix, y, 14, p.soft, l.text.c_str(), tw - ix) + 4;
        }
        float content = y + g_scroll - (ny + 14);
        g_scroll_to = std::min(g_scroll_to, std::max(0.0f, content - nh + 28));
        dl()->PopClipRect();
        if (content > nh) {   // where in the notes
            float bh = nh * nh / content;
            rect(nx + nw - 8, ny + 6 + (nh - 12 - bh) * (g_scroll / std::max(1.0f, content - nh + 28)), 3, bh - 12, p.dim, 2);
        }
        break;
    }
    case 2: {   // the port's own settings, in its own page
        const PortSettings *ps = port_settings(port.id);
        if (!in) { text(x, by, 17, p.soft, "Install the port to change its settings here."); break; }
        bool has = ps != nullptr;
        std::string head = has ? std::string(port.name) + " settings" : std::string(port.name) + "'s settings are in its own launcher";
        std::string sub = has ? std::to_string(ps->count) + " settings on " + std::to_string(ps->pages.size()) + " pages, as in the game's own menu" +
                                    (port_settings_changed(a.game) ? "  ·  " + std::to_string(port_settings_changed(a.game)) + " set by you" : "")
                              : "Sanctum does not have a list for this port yet; A opens its launcher's settings.";
        if (hover(x, by, 780, 74)) { if (a.mouse_moved) g_row = ROW_SETTINGS; if (a.clicked) g_row = ROW_SETTINGS, a.nav = NAV_A; }
        rect(x, by, 780, 74, p.panel, 14);
        rect(x, by, 6, 74, port.colours.accent, 3);
        icon(IC_GEAR, x + 40, by + 37, 26, port.colours.accent);
        text(x + 72, by + 14, 18, p.text, head.c_str());
        text(x + 72, by + 42, 14, p.soft, fit(14, sub, 680).c_str());
        if (g_row == ROW_SETTINGS) focus_ring(x, by, 780, 74, 14, 4);
        if (g_row == ROW_SETTINGS && a.nav == NAV_A) {
            if (has) port_settings_open(a.game);
            else if (port.launcher) launch(a.window, a.game, *in, true);
        }
        break;
    }
    default:
        text(x, by, 17, p.dim, "Coming in a later build step. Until then, the port's own launcher has these.");
        break;
    }

    if (g_row == ROW_COPIES) hint_bar({{NAV_A, "Use this copy"}, {NAV_L, "Tabs"}, {NAV_B, "Back"}});
    else if (g_row == ROW_RELEASES) hint_bar({{NAV_A, "Read"}, {NAV_L, "Tabs"}, {NAV_B, "Back"}});
    else if (g_row == ROW_SETTINGS) hint_bar({{NAV_A, "Open settings"}, {NAV_L, "Tabs"}, {NAV_B, "Back"}});
    else if (g_row == ROW_NOTES) hint_bar({{NAV_Y, "Open on GitHub"}, {NAV_B, "Back to the list"}});
    else hint_bar({{NAV_A, acts[g_action].label.substr(0, acts[g_action].label.find("  ")).c_str()}, {NAV_Y, "GitHub"}, {NAV_L, "Tabs"}, {NAV_B, "Back"}});
}

}  // namespace sanctum
