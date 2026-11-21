// Get ports (choose which ports to download), Downloads (the queue, live) and Releases & news (every port's
// releases, newest first). All three are grids or lists driven by the d-pad; nothing needs a mouse or keyboard.
#include <algorithm>
#include <cmath>

#include "config.h"
#include "covers.h"
#include "draw.h"
#include "github.h"
#include "install.h"
#include "launch.h"

namespace sanctum {

// ======================= Get ports
static bool g_sel[32];
static int g_pkg[32];          // ASSET_PORTABLE or ASSET_WIZARD, per port
static int g_focus = 0;        // a card, or ports().size() for the Download button
static const int kCols = 3;

void get_ports_select(int port) {
    if (port >= 0 && port < 32) g_sel[port] = true, g_focus = port;
    go(SCR_PORTS);
}

static std::vector<AssetKind> packages(const Port &p) {
    std::vector<AssetKind> v;
    if (const Release *r = p.latest()) {
        if (r->find(ASSET_PORTABLE)) v.push_back(ASSET_PORTABLE);
        if (r->find(ASSET_WIZARD)) v.push_back(ASSET_WIZARD);
    }
    return v;
}

static AssetKind package_of(int i) {
    auto v = packages(ports()[i]);
    if (v.empty()) return ASSET_OTHER;
    for (auto k : v) if (k == g_pkg[i]) return k;
    return v[0];   // Standalone when there is one: Sanctum then owns the folder and updates it in place
}

static bool can_get(int i) {
    const Port &p = ports()[i];
    return p.installs.empty() && !install_queued(i) && package_of(i) != ASSET_OTHER;
}

static void start_downloads() {
    int n = 0;
    for (int i = 0; i < (int)ports().size(); ++i) {
        if (!g_sel[i] || !can_get(i)) continue;
        const Release *r = ports()[i].latest();
        AssetKind k = package_of(i);
        install_enqueue(i, *r, *r->find(k), k == ASSET_WIZARD ? JOB_WIZARD : JOB_INSTALL);
        g_sel[i] = false;
        ++n;
    }
    if (n) go(SCR_DOWNLOADS);
    else toast("Tick a port first (A)");
}

void ports_frame() {
    App &a = app();
    const Palette &p = pal();
    auto &ps = ports();
    int n = (int)ps.size();
    if (g_focus > n) g_focus = n;
    switch (a.nav) {
    case NAV_LEFT: if (g_focus < n && g_focus % kCols) g_focus--; break;
    case NAV_RIGHT: if (g_focus < n - 1 && g_focus % kCols != kCols - 1) g_focus++; break;
    case NAV_UP: if (g_focus == n) g_focus = std::max(0, n - 1); else if (g_focus >= kCols) g_focus -= kCols; break;
    case NAV_DOWN: if (g_focus + kCols < n) g_focus += kCols; else g_focus = n; break;
    case NAV_A:
        if (g_focus == n) start_downloads();
        else if (can_get(g_focus)) g_sel[g_focus] = !g_sel[g_focus];
        else if (!ps[g_focus].installs.empty()) { a.game = g_focus; go(SCR_GAME); return; }
        else if (install_queued(g_focus)) go(SCR_DOWNLOADS);
        break;
    case NAV_X:
        if (g_focus < n) {
            auto v = packages(ps[g_focus]);
            if (v.size() > 1) g_pkg[g_focus] = package_of(g_focus) == v[0] ? v[1] : v[0];
        }
        break;
    case NAV_Y: folder_open(library_dir(), FOLDER_LIBRARY); return;
    case NAV_PLUS: start_downloads(); return;
    case NAV_B: go_back(); return;
    default: break;
    }

    background(0);
    status_bar();
    text(96, 88, 30, p.text, "Get ports");
    text(96, 128, 16, p.soft, "Choose the ports to download. Each one comes from its own GitHub release; Sanctum never downloads game files.");
    std::string gs = github_status();
    if (github_busy() && ps[0].releases.empty()) gs = "Asking GitHub for the latest releases...";
    if (!gs.empty()) text(96, 150, 14, p.warn, fit(14, gs, 1088).c_str());

    long long total = 0;
    int count = 0;
    float cw = 352, ch = 116, gx = 18, gy = 16, x0 = 96, y0 = 180;
    for (int i = 0; i < n; ++i) {
        const Port &port = ps[i];
        float x = x0 + (i % kCols) * (cw + gx), y = y0 + (i / kCols) * (ch + gy);
        if (hover(x, y, cw, ch)) {
            if (a.mouse_moved) g_focus = i;
            if (a.clicked) { g_focus = i; if (can_get(i)) g_sel[i] = !g_sel[i]; else if (!port.installs.empty()) { a.game = i; go(SCR_GAME); return; } }
        }
        rect(x, y + 5, cw, ch, with_alpha(IM_COL32_BLACK, p.light ? 18 : 60), 16);
        rect(x, y, cw, ch, p.panel, 16);
        tile(port, x + 12, y + 12, 92, 1, false);
        float tx = x + 118;
        text(tx, y + 14, 17, p.text, fit(17, port.title, cw - 160).c_str());
        text(tx, y + 38, 14, p.dim, fit(14, std::string("SteelTemple ") + port.name + "  ·  " + port.latest_tag(), cw - 130).c_str());
        AssetKind k = package_of(i);
        const Release *r = port.latest();
        const Asset *as = r && k != ASSET_OTHER ? r->find(k) : nullptr;
        std::string line;
        ImU32 lc = p.soft;
        if (!port.installs.empty() && port.state() == PORT_UPDATE) line = "On this PC  ·  update " + port.latest_tag() + " ready", lc = p.accent_text;
        else if (!port.installs.empty()) line = "On this PC", lc = p.ok;
        else if (install_queued(i)) line = "In Downloads", lc = p.accent_text;
        else if (!as) line = github_busy() ? "Checking GitHub..." : "No download in this release";
        else line = std::string(k == ASSET_WIZARD ? "Setup Wizard" : "Standalone") + "  ·  " + mb(as->size) +
                    (packages(port).size() > 1 ? "   (X changes)" : "");
        text(tx, y + 66, 14, lc, fit(14, line, cw - 130).c_str());
        if (!port.installs.empty()) icon(IC_CHECK, x + cw - 28, y + 28, 20, p.ok);
        else if (can_get(i)) {
            float cx = x + cw - 28, cy = y + 28;
            if (g_sel[i]) { circle(cx, cy, 14, p.accent); icon(IC_CHECK, cx, cy, 16, IM_COL32_WHITE); }
            else dl()->AddCircle(P(cx, cy), S(13), p.line, 32, S(3));
        }
        if (g_sel[i] && can_get(i) && as) total += as->size, ++count;
        if (i == g_focus) focus_ring(x, y, cw, ch, 16, 4);
    }

    // the summary and the Download button
    float sy = y0 + ((n + kCols - 1) / kCols) * (ch + gy) + 8;
    rect(96, sy, 1088, 76, p.panel2, 16);
    std::string s1 = count ? std::to_string(count) + (count == 1 ? " port selected" : " ports selected") : "Nothing selected yet";
    text(120, sy + 14, 19, p.text, s1.c_str());
    std::string s2 = (count ? mb(total) + " to download  ·  " : std::string()) + "installs into " + library_dir() + "   (Y changes)";
    text(120, sy + 42, 14, p.soft, fit(14, s2, 760).c_str());
    float bw = 210, bx = 1184 - 16 - bw, by = sy + 12;
    if (hover(bx, by, bw, 52)) { if (a.mouse_moved) g_focus = n; if (a.clicked) start_downloads(); }
    rect(bx, by, bw, 52, count ? p.accent : p.line, 26);
    icon(IC_DOWNLOAD, bx + 34, by + 26, 20, IM_COL32_WHITE);
    text(bx + 56, by + 14, 19, IM_COL32_WHITE, count ? ("Download " + std::to_string(count)).c_str() : "Download");
    if (g_focus == n) focus_ring(bx, by, bw, 52, 26, 4);

    if (g_focus < n && can_get(g_focus)) hint_bar({{NAV_A, g_sel[g_focus] ? "Untick" : "Tick"}, {NAV_X, "Package"}, {NAV_PLUS, "Download"}, {NAV_B, "Back"}});
    else hint_bar({{NAV_A, g_focus == n ? "Download" : "Open"}, {NAV_Y, "Install folder"}, {NAV_B, "Back"}});
}

// ======================= Downloads
static int g_row = 0;
static const char *kSteps[] = {"Download", "Verify", "Install", "Ready"};

static int step_of(const JobView &j) {
    switch (j.phase == PH_FAILED || j.phase == PH_CANCELLED ? j.reached : j.phase) {
    case PH_QUEUED: case PH_DOWNLOAD: return 0;
    case PH_VERIFY: return 1;
    case PH_INSTALL: case PH_WIZARD: return 2;
    default: return 3;
    }
}

void downloads_frame() {
    App &a = app();
    const Palette &p = pal();
    std::vector<JobView> jobs = install_jobs();
    std::reverse(jobs.begin(), jobs.end());   // the newest at the top
    int n = (int)jobs.size();
    if (g_row >= n) g_row = std::max(0, n - 1);
    switch (a.nav) {
    case NAV_UP: if (g_row > 0) g_row--; break;
    case NAV_DOWN: if (g_row < n - 1) g_row++; break;
    case NAV_X: if (n && jobs[g_row].phase < PH_WIZARD) { install_cancel(jobs[g_row].id); toast("Cancelled"); } break;
    case NAV_Y: install_clear_finished(); break;
    case NAV_A:
        if (n && jobs[g_row].phase == PH_DONE) { a.game = jobs[g_row].port; go(SCR_GAME); return; }
        if (n && (jobs[g_row].phase == PH_FAILED || jobs[g_row].phase == PH_CANCELLED)) {   // try again
            const Port &port = ports()[jobs[g_row].port];
            for (const Release &r : port.releases)
                if (r.tag == jobs[g_row].tag)
                    for (const Asset &as : r.assets)
                        if (as.name == jobs[g_row].file)
                            install_enqueue(jobs[g_row].port, r, as, (JobKind)jobs[g_row].kind, jobs[g_row].dest);
        }
        break;
    case NAV_B: go_back(); return;
    default: break;
    }

    background(0);
    status_bar();
    text(96, 88, 30, p.text, "Downloads");
    if (!n) {
        text(96, 140, 18, p.soft, "Nothing downloading. Choose ports in Get ports, or press Update on a port's page.");
        hint_bar({{NAV_B, "Back"}});
        return;
    }
    int top = std::max(0, std::min(g_row - 1, n - 4));
    for (int i = top; i < n && i < top + 4; ++i) {
        const JobView &j = jobs[i];
        const Port &port = ports()[j.port];
        float y = 140 + (i - top) * 122, x = 96, w = 1088;
        if (hover(x, y, w, 110) && a.mouse_moved) g_row = i;
        rect(x, y, w, 110, p.panel, 16);
        tile(port, x + 14, y + 14, 82, 1, false);
        std::string verb = j.kind == JOB_UPDATE ? "Updating " : j.kind == JOB_WIZARD ? "Setting up " : "Installing ";
        if (j.phase == PH_DONE) verb = j.kind == JOB_UPDATE ? "Updated " : "Installed ";
        text(x + 112, y + 14, 19, p.text, fit(19, verb + port.name + " " + j.tag, 520).c_str());
        text(x + 112, y + 40, 13, p.dim, fit(13, j.file, 520).c_str());
        // the steps
        int st = step_of(j);
        for (int s = 0; s < 4; ++s) {
            float sx = x + 680 + s * 98, sy = y + 22;
            bool done = s < st || j.phase == PH_DONE, cur = s == st && j.phase != PH_DONE;
            bool bad = cur && (j.phase == PH_FAILED || j.phase == PH_CANCELLED);
            ImU32 c = bad ? p.warn : done ? p.ok : cur ? port.colours.accent : p.line;
            if (s) rect(sx - 98 + 14, sy - 2, 84, 3, done || cur ? p.ok : p.line);
            circle(sx, sy, 12, c);
            if (done) icon(IC_CHECK, sx, sy, 13, IM_COL32_WHITE);
            if (bad) icon(IC_CLOSE, sx, sy, 11, IM_COL32_WHITE);
            text(sx, sy + 16, 12, cur ? p.text : p.dim, kSteps[s], 0.5f);
        }
        // the bar and its words
        std::string left, right;
        float frac = -1;
        switch (j.phase) {
        case PH_QUEUED: left = "Waiting"; frac = 0; right = mb(j.total); break;
        case PH_DOWNLOAD:
            frac = j.total ? (float)j.done / j.total : -1;
            left = mb(j.done) + " of " + mb(j.total);
            if (j.speed > 1) {
                int secs = (int)((j.total - j.done) / j.speed);
                right = mb((long long)j.speed) + "/s  ·  " + (secs >= 60 ? std::to_string(secs / 60) + " min left" : std::to_string(secs) + " s left");
            }
            break;
        case PH_VERIFY: left = "Checking it against GitHub's checksum"; break;
        case PH_INSTALL: left = j.kind == JOB_UPDATE ? "Putting the new files in place (your saves and settings stay)" : "Unpacking"; break;
        case PH_WIZARD: left = "The port's own setup is open"; break;
        case PH_DONE: frac = 1; left = j.dest.empty() ? "Done" : "In " + j.dest; break;
        case PH_FAILED: frac = 0; left = j.error; break;
        case PH_CANCELLED: frac = 0; left = "Cancelled"; break;
        }
        progress_bar(x + 112, y + 70, 940, 8, frac, j.phase == PH_DONE ? p.ok : port.colours.accent);
        text(x + 112, y + 84, 13, j.phase == PH_FAILED ? p.warn : p.soft, fit(13, left, 640).c_str());
        if (!right.empty()) text(x + 1052, y + 84, 13, p.soft, right.c_str(), 1);
        if (i == g_row) focus_ring(x, y, w, 110, 16, 4);
    }
    const JobView &f = jobs[g_row];
    if (f.phase == PH_DONE) hint_bar({{NAV_A, "Open"}, {NAV_Y, "Clear finished"}, {NAV_B, "Back"}});
    else if (f.phase == PH_FAILED || f.phase == PH_CANCELLED) hint_bar({{NAV_A, "Try again"}, {NAV_Y, "Clear finished"}, {NAV_B, "Back"}});
    else hint_bar({{NAV_X, "Cancel"}, {NAV_B, "Keep it going and go back"}});
}

// ======================= Releases & news
struct NewsItem {
    int port;
    const Release *rel;
};
static int g_news = 0;

void news_frame() {
    App &a = app();
    const Palette &p = pal();
    std::vector<NewsItem> items;
    for (int i = 0; i < (int)ports().size(); ++i)
        for (const Release &r : ports()[i].releases) items.push_back({i, &r});
    std::stable_sort(items.begin(), items.end(), [](const NewsItem &x, const NewsItem &y) { return x.rel->date > y.rel->date; });
    int n = (int)items.size();
    if (g_news >= n) g_news = std::max(0, n - 1);
    switch (a.nav) {
    case NAV_UP: if (g_news > 0) g_news--; break;
    case NAV_DOWN: if (g_news < n - 1) g_news++; break;
    case NAV_L: g_news = std::max(0, g_news - 6); break;
    case NAV_R: g_news = std::min(std::max(0, n - 1), g_news + 6); break;
    case NAV_A: if (n) { game_show_release(items[g_news].port, items[g_news].rel->tag); return; } break;
    case NAV_PLUS: github_refresh(true); toast("Asking GitHub for new releases"); break;
    case NAV_B: go_back(); return;
    default: break;
    }
    background(n ? ports()[items[g_news].port].colours.accent : 0);
    status_bar();
    text(96, 88, 30, p.text, "Releases & news");
    std::string gs = github_status();
    if (!gs.empty()) text(96, 128, 14, p.warn, fit(14, gs, 1088).c_str());
    if (!n) text(96, 150, 18, p.soft, github_busy() ? "Asking GitHub..." : "No releases yet. Press + to ask GitHub again.");
    int top = std::max(0, std::min(g_news - 3, n - 7));
    for (int i = top; i < n && i < top + 7; ++i) {
        const NewsItem &it = items[i];
        const Port &port = ports()[it.port];
        float y = 152 + (i - top) * 70, x = 96;
        if (hover(x, y, 1088, 62)) { if (a.mouse_moved) g_news = i; if (a.clicked) { game_show_release(it.port, it.rel->tag); return; } }
        rect(x, y, 1088, 62, p.panel, 12);
        tile(port, x + 8, y + 8, 46, 1, false);
        text(x + 70, y + 9, 17, p.text, (std::string(port.name) + "  " + it.rel->tag).c_str());
        std::string title = it.rel->title.empty() ? it.rel->tag : it.rel->title;
        text(x + 70, y + 34, 14, p.soft, fit(14, title, 820).c_str());
        bool newest = &port.releases[0] == it.rel;
        bool have = false;
        for (auto &in : port.installs) have |= tag_key(in.version) == tag_key(it.rel->tag);
        text(x + 1072, y + 10, 14, p.dim, it.rel->date.c_str(), 1);
        if (have) text(x + 1072, y + 34, 13, p.ok, "On this PC", 1);
        else if (newest) text(x + 1072, y + 34, 13, port.colours.accent, "Latest", 1);
        if (i == g_news) focus_ring(x, y, 1088, 62, 12, 3);
    }
    hint_bar({{NAV_A, "Read"}, {NAV_PLUS, "Check GitHub"}, {NAV_B, "Back"}});
}

}  // namespace sanctum
