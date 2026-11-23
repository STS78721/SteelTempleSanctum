// A port's page: its cover, what is installed, one big main action (Play, Download or Update), and tabs (L/R).
// Overview lists every copy found on this PC (up/down picks which one Play starts), so a second install is never
// needed. Release notes: every release from GitHub, the list on the left, the notes on the right; A moves into the
// notes, up/down scroll them, B comes back out.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>

#include "cache.h"
#include "config.h"
#include "covers.h"
#include "diag.h"
#include "draw.h"
#include "github.h"
#include "install.h"
#include "launch.h"
#include "mods.h"
#include "platform.h"
#include "port_settings.h"
#include "sound.h"
#include "sync.h"

namespace sanctum {

static const char *kTabs[] = {"Overview", "Release notes", "Settings", "Mods", "Files & storage"};
static const int kTabN = 5;
static int g_tab = 0, g_action = 0, g_copy = 0, g_last_game = -1, g_rel = 0, g_file_row = 0;
static float g_tab_x = 0, g_scroll = 0, g_scroll_to = 0, g_notes_max = 0;
static int g_notes_game = -1, g_notes_rel = -1;
static double g_uninstall_armed = -10, g_restore_save_armed = -10, g_clear_shaders_armed = -10, g_sync_pull_armed = -10;
enum { ROW_ACTIONS, ROW_COPIES, ROW_RELEASES, ROW_NOTES, ROW_SETTINGS, ROW_MODS, ROW_FILES };
static int g_row = ROW_ACTIONS;

// ---- lightweight inline text modal for per-game custom launch args & env overrides
static bool g_prompt_open = false;
static int g_prompt_kind = 0;   // 0 = launch_args, 1 = launch_env
static std::string g_prompt_port, g_prompt_buf;

static void prompt_start(int kind, const std::string &port_id, const std::string &initial) {
    g_prompt_open = true;
    g_prompt_kind = kind;
    g_prompt_port = port_id;
    g_prompt_buf = initial;
    app().text_entry = true;
    SDL_StartTextInput(app().window);
    sound_play(SFX_OK);
}

static void prompt_close() {
    g_prompt_open = false;
    app().text_entry = false;
    SDL_StopTextInput(app().window);
}

static std::string latest_save_backup(const std::string &port_id, int *count_out = nullptr) {
    std::string bdir = config_dir() + "backups\\" + port_id;
    int n = 0;
    char **names = SDL_GlobDirectory((bdir + "\\").c_str(), "*.zip", SDL_GLOB_CASEINSENSITIVE, &n);
    struct Entry { std::string path, name; SDL_Time mtime; };
    std::vector<Entry> zips;
    for (int i = 0; names && i < n; ++i) {
        if (!names[i]) continue;
        if (strncmp(names[i], "save-", 5) != 0 && strncmp(names[i], "auto-pre-update-", 16) != 0) continue;
        std::string full = bdir + "\\" + names[i];
        SDL_PathInfo pi{};
        SDL_GetPathInfo(full.c_str(), &pi);
        zips.push_back({full, names[i], pi.modify_time});
    }
    SDL_free(names);
    std::sort(zips.begin(), zips.end(), [](const Entry &a, const Entry &b) {
        if (a.mtime != b.mtime) return a.mtime < b.mtime;
        return a.name < b.name;
    });
    if (count_out) *count_out = (int)zips.size();
    return zips.empty() ? std::string() : zips.back().path;
}

void game_open_tab(int port, int tab) {
    app().game = port;
    g_last_game = port;
    g_tab = SDL_clamp(tab, 0, kTabN - 1);
    g_row = ROW_ACTIONS, g_action = 0, g_file_row = 0;
    go(SCR_GAME);
}

// ---- async disk size measurement for Files & storage
struct SizeInfo {
    bool ready = false, running = false;
    long long bytes = 0;
    int files = 0;
};
static std::mutex g_size_lock;
static std::map<std::string, SizeInfo> g_sizes;

static int size_worker(void *p) {
    auto *dir = (std::string *)p;
    int files = 0;
    long long bytes = folder_size_bytes(*dir, &files);
    cache_size_put(*dir, bytes, files);
    {
        std::lock_guard<std::mutex> l(g_size_lock);
        g_sizes[*dir] = {true, false, bytes, files};
    }
    delete dir;
    return 0;
}

static SizeInfo folder_size_cached(const std::string &dir, bool force = false) {
    if (dir.empty()) return {};
    std::lock_guard<std::mutex> l(g_size_lock);
    SizeInfo &si = g_sizes[dir];
    if (!force && !si.ready) {
        long long cb = 0;
        int cf = 0;
        if (cache_size_get(dir, cb, cf)) {
            si = {true, false, cb, cf};
            return si;
        }
    }
    if (!si.running && (!si.ready || force)) {
        if (force) cache_size_invalidate(dir);
        si.running = true;
        if (force) si.ready = false;
        SDL_Thread *t = SDL_CreateThread(size_worker, "dirsize", new std::string(dir));
        if (t) SDL_DetachThread(t);
        else si.running = false;
    }
    return si;
}

static bool has_installable_asset(const Install *in, const Release &r) {
    if (in && r.find(ASSET_UPDATE)) return true;
    return r.find(ASSET_PORTABLE) != nullptr || r.find(ASSET_WIZARD) != nullptr;
}

static void install_release(int port_i, const Port &port, const Install *in, const Release &r, bool busy) {
    if (game_running()) { sound_play(SFX_ERROR); toast("Close the game first"); return; }
    if (busy) { sound_play(SFX_ERROR); toast("A download for this port is already running"); return; }
    if (in && !in->version.empty() && tag_key(in->version) == tag_key(r.tag)) {
        sound_play(SFX_ERROR);
        toast("Already on " + r.tag);
        return;
    }
    if (in) {
        if (const Asset *up = r.find(ASSET_UPDATE)) {
            install_enqueue(port_i, r, *up, JOB_UPDATE, in->dir);
            sound_play(SFX_OK);
            toast("Queued " + std::string(port.name) + " " + r.tag + " in Downloads");
            return;
        }
    }
    if (const Asset *as = r.find(ASSET_PORTABLE)) {
        install_enqueue(port_i, r, *as, JOB_INSTALL);
        sound_play(SFX_OK);
        toast("Queued " + std::string(port.name) + " " + r.tag + " in Downloads");
        return;
    }
    if (const Asset *w = r.find(ASSET_WIZARD)) {
        install_enqueue(port_i, r, *w, JOB_WIZARD);
        sound_play(SFX_OK);
        toast("Queued " + std::string(port.name) + " " + r.tag + " in Downloads");
        return;
    }
    sound_play(SFX_ERROR);
    toast("This release has no package Sanctum can install");
}

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
enum { ACT_PLAY, ACT_RESUME, ACT_SUSPEND, ACT_DOWNLOAD, ACT_UPDATE, ACT_LAUNCHER, ACT_GITHUB, ACT_PROGRESS, ACT_CLOSE, ACT_LOG, ACT_DIAG, ACT_REPORT, ACT_ISSUE };
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
    LastRun lr_val{};
    bool has_lr = last_run(a.game, lr_val);
    if (running) {
        acts.push_back({IC_PLAY, game_suspended() ? "Quick Resume" : "Resume game", ACT_RESUME});
        if (!game_suspended()) acts.push_back({IC_PAUSE, "Suspend", ACT_SUSPEND});
        acts.push_back({IC_CLOSE, "Close game", ACT_CLOSE});
    } else if (busy) acts.push_back({IC_DOWNLOAD, "Downloads", ACT_PROGRESS});
    else if (in && !can_play(port, *in) && port.launcher) acts.push_back({IC_GEAR, "Set up", ACT_PLAY});   // first start
    else if (in) acts.push_back({IC_PLAY, "Play", ACT_PLAY});
    else acts.push_back({IC_DOWNLOAD, "Download  " + port.download_size(), ACT_DOWNLOAD});
    if (!busy && st == PORT_UPDATE && latest) acts.push_back({IC_DOWNLOAD, "Update to " + latest->tag, ACT_UPDATE});
    // after a run that went wrong: what to do about it
    if (has_lr && !running && (lr_val.verdict == RUN_CRASH || lr_val.verdict == RUN_NO_START)) {
        if (!lr_val.log.empty()) acts.push_back({IC_NEWS, "Open the log", ACT_LOG});
        acts.push_back({IC_DOWNLOAD, "Export diagnostics", ACT_DIAG});
        acts.push_back({IC_INFO, "Report it", ACT_REPORT});
    }
    if (has_lr && !running && (lr_val.verdict == RUN_KNOWN || lr_val.verdict == RUN_FIXED) && !lr_val.issue.empty()) {
        if (lr_val.verdict == RUN_KNOWN) acts.push_back({IC_DOWNLOAD, "Export diagnostics", ACT_DIAG});
        acts.push_back({IC_INFO, "Issue " + lr_val.issue, ACT_ISSUE});
    }
    if (!running && in && in->launcher && can_play(port, *in)) acts.push_back({IC_GEAR, "Port's own launcher", ACT_LAUNCHER});
    if (!port.custom) acts.push_back({IC_INFO, "Open on GitHub", ACT_GITHUB});
    // the buttons changed (an update finished, a download started): focus goes back to the main one
    static size_t last_count = 0;
    if (acts.size() != last_count) g_action = 0, last_count = acts.size();

    // If the inline launch-options text modal is open, hold screen nav so the modal owns input
    Nav modal_nav = NAV_NONE;
    bool modal_click = false;
    if (g_prompt_open) {
        modal_nav = a.nav, modal_click = a.clicked;
        a.nav = NAV_NONE, a.clicked = false;
    }

    auto run = [&](int id) {
        switch (id) {
        case ACT_PLAY:
            if (in && launch(a.window, a.game, *in, false)) {
                running = true;
                has_lr = false;
            }
            break;
        case ACT_RESUME:
            sound_play(SFX_OK);
            resume_game(a.window);
            break;
        case ACT_SUSPEND:
            if (suspend_game(a.window)) {
                sound_play(SFX_TOGGLE_ON);
                toast("Suspended " + std::string(port.title) + " (Quick Resume ready)", 3.5);
            } else {
                sound_play(SFX_ERROR);
                toast("Could not suspend game");
            }
            break;
        case ACT_LAUNCHER:
            if (in && launch(a.window, a.game, *in, true)) {
                running = true;
                has_lr = false;
            }
            break;
        case ACT_CLOSE:
            if (a.now - g_close_armed < 3) { sound_play(SFX_BACK); close_game(); toast("Closing " + std::string(port.name)); g_close_armed = -10; }
            else { sound_play(SFX_ERROR); g_close_armed = a.now; toast("Press again to close the game (unsaved progress is lost)", 3); }
            break;
        case ACT_LOG: if (has_lr) { sound_play(SFX_OK); SDL_OpenURL(("file:///" + lr_val.log).c_str()); } break;
        case ACT_DIAG: {
            if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: diagnostics disabled"); break; }
            std::string zp = export_crash_diagnostics(a.game, in, has_lr ? &lr_val : nullptr);
            if (!zp.empty()) {
                sound_play(SFX_TOGGLE_ON);
                toast("Exported sanitized diagnostics zip (path copied)", 4.0);
                size_t sl = zp.find_last_of("\\/");
                SDL_OpenURL(("file:///" + (sl == std::string::npos ? zp : zp.substr(0, sl))).c_str());
            } else {
                sound_play(SFX_ERROR);
                toast("Could not create diagnostics bundle");
            }
            break;
        }
        case ACT_REPORT: sound_play(SFX_OK); SDL_OpenURL((std::string("https://github.com/") + port.repo + "/issues/new").c_str()); break;
        case ACT_ISSUE: {
            sound_play(SFX_OK);
            std::string n = has_lr ? lr_val.issue : "";
            if (!n.empty() && n[0] == '#') n.erase(0, 1);
            SDL_OpenURL((std::string("https://github.com/") + port.repo + "/issues/" + n).c_str());
            break;
        }
        case ACT_PROGRESS: go(SCR_DOWNLOADS); break;
        case ACT_DOWNLOAD: {
            if (!latest) { sound_play(SFX_ERROR); toast(github_busy() ? "Still asking GitHub, one moment" : "No release found on GitHub"); break; }
            if (const Asset *as = latest->find(ASSET_PORTABLE)) install_enqueue(a.game, *latest, *as, JOB_INSTALL);
            else if (const Asset *w = latest->find(ASSET_WIZARD)) install_enqueue(a.game, *latest, *w, JOB_WIZARD);
            else { sound_play(SFX_ERROR); toast("This release has no download Sanctum can install"); break; }
            sound_play(SFX_OK);
            toast(std::string(port.name) + " added to Downloads");
            break;
        }
        case ACT_UPDATE: {
            if (game_running()) { sound_play(SFX_ERROR); toast("Close the game first"); break; }
            const Install *b = port.best();
            const Asset *up = latest ? latest->find(ASSET_UPDATE) : nullptr;
            if (!b || !up) { sound_play(SFX_ERROR); toast("This release has no update package; Download installs it fresh"); break; }
            install_enqueue(a.game, *latest, *up, JOB_UPDATE, b->dir);
            sound_play(SFX_OK);
            toast(std::string(port.name) + " update added to Downloads");
            break;
        }
        case ACT_GITHUB: sound_play(SFX_OK); SDL_OpenURL((std::string("https://github.com/") + port.repo + "/releases").c_str()); break;
        }
    };

    // ---- input
    bool copies_here = g_tab == 0 && port.installs.size() > 1;
    bool notes_here = g_tab == 1 && !port.releases.empty();
    // files & storage action list (needed for Up/Down/A handling)
    struct FileRow { std::string label, value; int id; };
    enum { F_OPEN_DIR, F_OPEN_LOGS, F_VERIFY, F_CLEAR_SHADERS, F_PREWARM_SHADERS,
           F_CLOUD_SYNC, F_BACKUP_SAVES, F_RESTORE_SAVES, F_BACKUP_INI, F_RESTORE_INI,
           F_REFRESH_HZ, F_EXPORT_DIAG,
           F_CUSTOM_COVER, F_CLEAR_COVER, F_LAUNCH_ARGS, F_LAUNCH_ENV, F_PIN, F_UNINSTALL };
    std::vector<FileRow> frows;
    std::string f_ini, f_bak, f_miss, f_save_dir, f_save_bak;
    std::vector<std::string> f_shader_paths;
    long long f_shader_bytes = 0;
    bool f_shader_sizing = false;
    int f_save_cnt = 0;
    if (g_tab == 4 && in) {
        f_miss = missing_for_play(port, *in);
        f_ini = port_settings_ini_path(a.game);
        f_bak = f_ini.empty() ? std::string() : f_ini + ".sanctum-bak";
        f_save_dir = port_save_dir(port, *in);
        f_save_bak = demo_mode() ? std::string() : latest_save_backup(port.id, &f_save_cnt);
        if (!port.custom && !demo_mode()) {
            for (const char *d : {"shader-cache", "vk_cache"}) {
                std::string dp = in->dir + "\\" + d;
                SDL_PathInfo pi{};
                if (SDL_GetPathInfo(dp.c_str(), &pi) && pi.type == SDL_PATHTYPE_DIRECTORY) {
                    f_shader_paths.push_back(dp);
                    SizeInfo dsi = folder_size_cached(dp);
                    if (dsi.ready) f_shader_bytes += dsi.bytes;
                    else f_shader_sizing = true;
                }
            }
            for (const char *f : {"pipeline-cache.bin", "pipeline-keys.bin", "shader-prepared.bin",
                                  "vk_pipeline_cache.bin", "shader_cache.bin"}) {
                std::string fp = in->dir + "\\" + f;
                SDL_PathInfo pi{};
                if (SDL_GetPathInfo(fp.c_str(), &pi) && pi.type == SDL_PATHTYPE_FILE) {
                    f_shader_paths.push_back(fp);
                    f_shader_bytes += pi.size;
                }
            }
        }
        bool has_xlate = !port.custom && !demo_mode() &&
                         (SDL_GetPathInfo((in->dir + "\\ns2_xlate.exe").c_str(), nullptr) ||
                          SDL_GetPathInfo((in->dir + "\\totk_xlate.exe").c_str(), nullptr));
        bool has_seed = has_xlate &&
                        ((SDL_GetPathInfo((in->dir + "\\seed-answers.bin").c_str(), nullptr) &&
                          SDL_GetPathInfo((in->dir + "\\seed-programs.bin").c_str(), nullptr)) ||
                         (SDL_GetPathInfo((in->dir + "\\shader-answers.bin").c_str(), nullptr) &&
                          SDL_GetPathInfo((in->dir + "\\shader-programs.bin").c_str(), nullptr)));
        bool has_logs = SDL_GetPathInfo((in->dir + "\\logs").c_str(), nullptr);
        frows.push_back({"Open install folder", "Explorer", F_OPEN_DIR});
        if (!port.custom) frows.push_back({"Open logs folder", has_logs ? "logs\\" : "Install folder", F_OPEN_LOGS});
        frows.push_back({"Verify required files", f_miss.empty() ? "All present" : "Missing files", F_VERIFY});
        if (!f_shader_paths.empty()) {
            bool s_armed = a.now - g_clear_shaders_armed < 4.0;
            std::string sz_str = f_shader_sizing && f_shader_bytes == 0 ? "Calculating..." : mb(f_shader_bytes);
            frows.push_back({"Clear shader & pipeline cache",
                             s_armed ? ("Press again to clear " + sz_str) : (sz_str + "  ·  Recycle Bin"),
                             F_CLEAR_SHADERS});
        }
        if (has_seed) {
            WarmState ws = shader_warm_state();
            std::string wval = (ws.active && ws.port == a.game) ? (ws.label + "  ·  Cancel")
                             : ws.active ? "Another port is pre-warming..."
                             : "Build shader & driver cache";
            frows.push_back({"Pre-warm shader cache", wval, F_PREWARM_SHADERS});
        }
        if (!cfg().save_sync_dir.empty() && !demo_mode()) {
            SyncInfo si = sync_inspect(port, *in);
            bool pull_armed = a.now - g_sync_pull_armed < 4.0;
            std::string sval = (pull_armed && (si.kind == SYNC_CLOUD_NEWER || si.kind == SYNC_CONFLICT))
                             ? "Press again to pull cloud save (local backed up first)"
                             : si.summary;
            frows.push_back({"Cloud save sync", sval, F_CLOUD_SYNC});
        }
        if (!f_save_dir.empty()) {
            frows.push_back({"Back up save files", f_save_cnt ? (std::to_string(f_save_cnt) + " saved") : "Create zip backup", F_BACKUP_SAVES});
            if (!f_save_bak.empty()) {
                bool r_armed = a.now - g_restore_save_armed < 4.0;
                bool is_auto = f_save_bak.find("\\auto-pre-update-") != std::string::npos;
                frows.push_back({"Restore latest save backup",
                                 r_armed ? "Press again to restore" : (is_auto ? "Auto pre-update snapshot" : "Safe pre-backup first"),
                                 F_RESTORE_SAVES});
            }
        }
        if (!f_bak.empty() && SDL_GetPathInfo(f_bak.c_str(), nullptr))
            frows.push_back({"Restore settings backup", ".sanctum-bak", F_RESTORE_INI});
        else if (!f_ini.empty() && SDL_GetPathInfo(f_ini.c_str(), nullptr))
            frows.push_back({"Back up settings", "Save .sanctum-bak", F_BACKUP_INI});
        auto hz_it = cfg().refresh_hz.find(port.id);
        int tgt_hz = (hz_it != cfg().refresh_hz.end()) ? hz_it->second : 0;
        frows.push_back({"Display refresh rate", tgt_hz > 0 ? (std::to_string(tgt_hz) + " Hz (restored on exit)") : "System default", F_REFRESH_HZ});
        frows.push_back({"Export diagnostics bundle", "Sanitized .zip for GitHub", F_EXPORT_DIAG});
        auto cov_it = cfg().port_covers.find(port.id);
        bool has_cov = cov_it != cfg().port_covers.end() && !cov_it->second.empty();
        frows.push_back({"Custom cover art", has_cov ? "Change image..." : "Pick image...", F_CUSTOM_COVER});
        if (has_cov) frows.push_back({"Clear custom cover art", "Use folder / default", F_CLEAR_COVER});
        auto arg_it = cfg().launch_args.find(port.id);
        std::string cur_args = arg_it == cfg().launch_args.end() ? "" : arg_it->second;
        frows.push_back({"Custom launch arguments", cur_args.empty() ? "None" : fit(14, cur_args, 240), F_LAUNCH_ARGS});
        auto env_it = cfg().launch_env.find(port.id);
        std::string cur_env = env_it == cfg().launch_env.end() ? "" : env_it->second;
        frows.push_back({"Custom environment variables", cur_env.empty() ? "None" : fit(14, cur_env, 240), F_LAUNCH_ENV});
        bool pinned = cfg().pinned.count(port.id) > 0;
        frows.push_back({"Pin to front on Home", pinned ? "Pinned" : "Off", F_PIN});
        bool armed = a.now - g_uninstall_armed < 4.0;
        if (port.custom)
            frows.push_back({"Remove shortcut from Sanctum", armed ? "Press again to remove" : "Keeps game files on disk", F_UNINSTALL});
        else
            frows.push_back({"Uninstall this copy", armed ? "Press again to move to Recycle Bin" : "Moves to Recycle Bin", F_UNINSTALL});
        if (g_file_row >= (int)frows.size()) g_file_row = std::max(0, (int)frows.size() - 1);
    }

    auto run_file_act = [&](int fid) -> bool {
        if (!in) return false;
        switch (fid) {
        case F_OPEN_DIR:
            if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: folders don't open"); break; }
            sound_play(SFX_OK);
            SDL_OpenURL(("file:///" + in->dir).c_str());
            break;
        case F_OPEN_LOGS: {
            if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: folders don't open"); break; }
            std::string ldir = in->dir + "\\logs";
            if (!SDL_GetPathInfo(ldir.c_str(), nullptr)) ldir = in->dir;
            sound_play(SFX_OK);
            SDL_OpenURL(("file:///" + ldir).c_str());
            break;
        }
        case F_VERIFY: {
            std::string m = missing_for_play(port, *in);
            folder_size_cached(in->dir, true);
            if (m.empty()) { sound_play(SFX_TOGGLE_ON); toast("All required game files are present"); }
            else { sound_play(SFX_ERROR); toast("Missing: " + m, 4); }
            break;
        }
        case F_CLEAR_SHADERS: {
            if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: files unchanged"); break; }
            if (running) { sound_play(SFX_ERROR); toast("Close the game before clearing shaders"); break; }
            if (f_shader_paths.empty()) { sound_play(SFX_ERROR); toast("No shader cache files found"); break; }
            if (a.now - g_clear_shaders_armed < 4.0) {
                g_clear_shaders_armed = -10;
                int removed = 0;
                for (const std::string &sp : f_shader_paths)
                    if (recycle_path(sp)) ++removed;
                folder_size_cached(in->dir, true);
                for (const char *d : {"shader-cache", "vk_cache"}) {
                    std::lock_guard<std::mutex> l(g_size_lock);
                    g_sizes.erase(in->dir + "\\" + d);
                }
                if (removed > 0) {
                    sound_play(SFX_TOGGLE_ON);
                    toast("Cleared shader & pipeline cache (" + mb(f_shader_bytes) + " moved to Recycle Bin)", 3.5);
                } else {
                    sound_play(SFX_ERROR);
                    toast("Could not clear shader cache (is the game open?)", 4);
                }
            } else {
                g_clear_shaders_armed = a.now;
                sound_play(SFX_ERROR);
                toast("Press again to move shader & pipeline cache (" + mb(f_shader_bytes) + ") to Recycle Bin", 4);
            }
            break;
        }
        case F_PREWARM_SHADERS: {
            if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: shaders unchanged"); break; }
            WarmState ws = shader_warm_state();
            if (ws.active && ws.port == a.game) {
                cancel_prepare_shaders();
                sound_play(SFX_BACK);
                toast("Cancelling shader pre-warm...");
            } else {
                launch_prepare_shaders(a.game, *in);
            }
            break;
        }
        case F_CLOUD_SYNC: {
            if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: saves unchanged"); break; }
            if (running) { sound_play(SFX_ERROR); toast("Close the game before syncing saves"); break; }
            SyncInfo si = sync_inspect(port, *in, true);
            if (si.kind == SYNC_CLOUD_NEWER || si.kind == SYNC_CONFLICT) {
                if (a.now - g_sync_pull_armed < 4.0) {
                    g_sync_pull_armed = -10.0;
                    std::string err;
                    if (sync_pull_now(port, *in, err)) {
                        sound_play(SFX_TOGGLE_ON);
                        toast("Pulled cloud save safely (local copy backed up)", 3.5);
                    } else {
                        sound_play(SFX_ERROR);
                        toast("Cloud pull failed: " + err, 4.0);
                    }
                } else {
                    g_sync_pull_armed = a.now;
                    sound_play(SFX_ERROR);
                    toast("Press again to pull cloud save (existing local saves are backed up first)", 4.0);
                }
            } else {
                std::string err;
                if (sync_push_now(port, *in, err)) {
                    sound_play(SFX_TOGGLE_ON);
                    toast("Pushed " + std::string(port.name) + " saves to cloud folder", 3.0);
                } else {
                    sound_play(SFX_ERROR);
                    toast("Cloud push failed: " + err, 4.0);
                }
            }
            break;
        }
        case F_REFRESH_HZ: {
            if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: display unchanged"); break; }
            void *hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(a.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
            std::vector<int> rates = display_supported_refresh_rates(hwnd);
            if (rates.empty()) { sound_play(SFX_ERROR); toast("No alternate refresh rates reported by this display"); break; }
            // Cycle: 0 (System default) -> supported rates -> 0
            int cur = cfg().refresh_hz.count(port.id) ? cfg().refresh_hz[port.id] : 0;
            int next = 0;
            if (cur == 0) {
                next = rates.front();
            } else {
                for (size_t i = 0; i < rates.size(); ++i) {
                    if (rates[i] == cur) { next = (i + 1 < rates.size()) ? rates[i + 1] : 0; break; }
                    if (rates[i] > cur) { next = rates[i]; break; }
                }
            }
            if (next > 0) cfg().refresh_hz[port.id] = next;
            else cfg().refresh_hz.erase(port.id);
            config_save();
            sound_play(next > 0 ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF);
            toast(next > 0 ? ("Refresh rate target: " + std::to_string(next) + " Hz while playing")
                           : "Refresh rate: System default");
            break;
        }
        case F_EXPORT_DIAG: {
            if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: diagnostics disabled"); break; }
            std::string zp = export_crash_diagnostics(a.game, in, has_lr ? &lr_val : nullptr);
            if (!zp.empty()) {
                sound_play(SFX_TOGGLE_ON);
                toast("Exported sanitized diagnostics zip (path copied)", 4.0);
                size_t sl = zp.find_last_of("\\/");
                SDL_OpenURL(("file:///" + (sl == std::string::npos ? zp : zp.substr(0, sl))).c_str());
            } else {
                sound_play(SFX_ERROR);
                toast("Could not export diagnostics bundle");
            }
            break;
        }
        case F_BACKUP_SAVES: {
            if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: saves unchanged"); break; }
            if (running) { sound_play(SFX_ERROR); toast("Close the game before backing up saves"); break; }
            if (f_save_dir.empty()) { sound_play(SFX_ERROR); toast("No save folder found yet"); break; }
            std::string bdir = config_dir() + "backups\\" + port.id;
            SDL_CreateDirectory((config_dir() + "backups").c_str());
            SDL_CreateDirectory(bdir.c_str());
            time_t now = time(nullptr);
            struct tm lt{};
            localtime_s(&lt, &now);
            char stamp[32];
            strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &lt);
            std::string dst = bdir + "\\save-" + stamp + ".zip";
            int files = 0;
            if (zip_folder(f_save_dir, dst, &files)) {
                sound_play(SFX_TOGGLE_ON);
                toast("Backed up " + std::to_string(files) + " save files to backups\\" + std::string(port.id), 3.5);
            } else {
                sound_play(SFX_ERROR);
                toast("No save files found to back up");
            }
            break;
        }
        case F_RESTORE_SAVES: {
            if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: saves unchanged"); break; }
            if (running) { sound_play(SFX_ERROR); toast("Close the game first"); break; }
            if (f_save_bak.empty() || f_save_dir.empty()) { sound_play(SFX_ERROR); toast("No save backup found"); break; }
            if (a.now - g_restore_save_armed < 4.0) {
                g_restore_save_armed = -10;
                // Safety pre-backup before restoring
                std::string bdir = config_dir() + "backups\\" + port.id;
                zip_folder(f_save_dir, bdir + "\\pre-restore.zip");
                std::string stage = local_dir() + "save-restore";
                remove_tree(stage);
                SDL_CreateDirectory(stage.c_str());
                std::atomic<bool> cancel{false};
                std::string err = unzip(f_save_bak, stage, cancel);
                if (err.empty()) err = overlay(stage, f_save_dir);
                remove_tree(stage);
                if (err.empty()) { sound_play(SFX_TOGGLE_ON); toast("Restored saves from latest backup", 3.5); }
                else { sound_play(SFX_ERROR); toast("Restore failed: " + err, 4); }
            } else {
                g_restore_save_armed = a.now;
                sound_play(SFX_ERROR);
                toast("Press again to restore saves (current saves are backed up first)", 4);
            }
            break;
        }
        case F_BACKUP_INI: {
            if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: files unchanged"); break; }
            size_t sz = 0;
            if (void *d = SDL_LoadFile(f_ini.c_str(), &sz)) {
                bool ok = SDL_SaveFile(f_bak.c_str(), d, sz);
                SDL_free(d);
                sound_play(ok ? SFX_TOGGLE_ON : SFX_ERROR);
                toast(ok ? "Settings backed up" : "Could not save settings backup");
            } else {
                sound_play(SFX_ERROR);
                toast("Settings file not found");
            }
            break;
        }
        case F_RESTORE_INI: {
            if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: files unchanged"); break; }
            if (running) { sound_play(SFX_ERROR); toast("Close the game first"); break; }
            size_t sz = 0;
            if (void *d = SDL_LoadFile(f_bak.c_str(), &sz)) {
                bool ok = SDL_SaveFile(f_ini.c_str(), d, sz);
                SDL_free(d);
                sound_play(ok ? SFX_OK : SFX_ERROR);
                toast(ok ? "Settings restored from backup" : "Could not restore settings");
            } else {
                sound_play(SFX_ERROR);
                toast("Backup file not found");
            }
            break;
        }
        case F_CUSTOM_COVER: {
            if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: covers unchanged"); break; }
            std::string img = pick_open_file(a.window, "Choose cover image", "Images", "*.png;*.jpg;*.jpeg;*.bmp");
            if (!img.empty()) {
                cfg().port_covers[port.id] = img;
                config_save();
                covers_reload();
                sound_play(SFX_TOGGLE_ON);
                toast("Custom cover art updated");
            }
            break;
        }
        case F_CLEAR_COVER:
            cfg().port_covers.erase(port.id);
            config_save();
            covers_reload();
            sound_play(SFX_TOGGLE_OFF);
            toast("Cleared custom cover art");
            break;
        case F_LAUNCH_ARGS: {
            auto it = cfg().launch_args.find(port.id);
            prompt_start(0, port.id, it == cfg().launch_args.end() ? "" : it->second);
            break;
        }
        case F_LAUNCH_ENV: {
            auto it = cfg().launch_env.find(port.id);
            prompt_start(1, port.id, it == cfg().launch_env.end() ? "" : it->second);
            break;
        }
        case F_PIN:
            if (cfg().pinned.count(port.id)) {
                cfg().pinned.erase(port.id);
                sound_play(SFX_TOGGLE_OFF);
                toast("Unpinned from Home");
            } else {
                cfg().pinned.insert(port.id);
                sound_play(SFX_TOGGLE_ON);
                toast("Pinned to front on Home");
            }
            config_save();
            break;
        case F_UNINSTALL: {
            if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: nothing deleted"); break; }
            if (running) { sound_play(SFX_ERROR); toast("Close the game first"); break; }
            if (a.now - g_uninstall_armed < 4.0) {
                g_uninstall_armed = -10;
                if (port.custom) {
                    if (catalog_remove_custom(a.game)) {
                        sound_play(SFX_BACK);
                        toast("Removed custom shortcut");
                        if (a.game >= (int)ports().size()) a.game = std::max(0, (int)ports().size() - 1);
                        go_back(true);
                        return true;
                    }
                    break;
                }
                std::string target = in->dir;
                if (recycle_path(target)) {
                    catalog_remove(a.game, target);
                    g_copy = 0, g_row = ROW_ACTIONS, g_file_row = 0;
                    in = port.installs.empty() ? nullptr : &port.installs[0];
                    sound_play(SFX_BACK);
                    toast("Moved copy to Recycle Bin", 3.5);
                } else {
                    sound_play(SFX_ERROR);
                    toast("Could not move folder to Recycle Bin (is a file open?)", 4);
                }
            } else {
                g_uninstall_armed = a.now;
                sound_play(SFX_ERROR);
                toast(port.custom ? "Press again to remove shortcut from Sanctum" : ("Press again to move " + in->dir + " to Recycle Bin"), 4);
            }
            break;
        }
        }
        return false;
    };

    switch (a.nav) {
    case NAV_LEFT:
        if (g_row == ROW_ACTIONS) {
            if (g_action > 0) { g_action--; sound_play(SFX_MOVE); }
            else sound_play(SFX_EDGE);
        }
        break;
    case NAV_RIGHT:
        if (g_row == ROW_ACTIONS) {
            if (g_action < (int)acts.size() - 1) { g_action++; sound_play(SFX_MOVE); }
            else sound_play(SFX_EDGE);
        }
        break;
    case NAV_DOWN:
        if (g_row == ROW_ACTIONS && copies_here) { g_row = ROW_COPIES; sound_play(SFX_MOVE); }
        else if (g_row == ROW_ACTIONS && notes_here) { g_row = ROW_RELEASES; sound_play(SFX_MOVE); }
        else if (g_row == ROW_ACTIONS && g_tab == 2 && in) { g_row = ROW_SETTINGS; sound_play(SFX_MOVE); }
        else if (g_row == ROW_ACTIONS && g_tab == 3 && in) { g_row = ROW_MODS; sound_play(SFX_MOVE); }
        else if (g_row == ROW_ACTIONS && g_tab == 4 && in && !frows.empty()) { g_row = ROW_FILES, g_file_row = 0; sound_play(SFX_MOVE); }
        else if (g_row == ROW_COPIES) {
            if (g_copy < (int)port.installs.size() - 1) { g_copy++; sound_play(SFX_MOVE); }
            else sound_play(SFX_EDGE);
        } else if (g_row == ROW_RELEASES) {
            if (g_rel < (int)port.releases.size() - 1) { g_rel++, g_scroll_to = 0; sound_play(SFX_MOVE); }
            else sound_play(SFX_EDGE);
        } else if (g_row == ROW_NOTES) {
            bool at_end = g_notes_game == a.game && g_notes_rel == g_rel && g_scroll_to >= g_notes_max;
            if (!at_end) { g_scroll_to += 90; sound_play(SFX_KEY); }
            else sound_play(SFX_EDGE);
        } else if (g_row == ROW_FILES) {
            if (g_file_row < (int)frows.size() - 1) { g_file_row++; sound_play(SFX_MOVE); }
            else sound_play(SFX_EDGE);
        } else {
            sound_play(SFX_EDGE);
        }
        break;
    case NAV_UP:
        if (g_row == ROW_COPIES) { if (g_copy > 0) g_copy--; else g_row = ROW_ACTIONS; sound_play(SFX_MOVE); }
        else if (g_row == ROW_RELEASES) { if (g_rel > 0) g_rel--, g_scroll_to = 0; else g_row = ROW_ACTIONS; sound_play(SFX_MOVE); }
        else if (g_row == ROW_NOTES) {
            if (g_scroll_to > 0) { g_scroll_to = std::max(0.0f, g_scroll_to - 90); sound_play(SFX_KEY); }
            else sound_play(SFX_EDGE);
        }
        else if (g_row == ROW_SETTINGS || g_row == ROW_MODS) { g_row = ROW_ACTIONS; sound_play(SFX_MOVE); }
        else if (g_row == ROW_FILES) { if (g_file_row > 0) g_file_row--; else g_row = ROW_ACTIONS; sound_play(SFX_MOVE); }
        else sound_play(SFX_EDGE);
        break;
    case NAV_L: g_tab = (g_tab + kTabN - 1) % kTabN; g_row = ROW_ACTIONS; g_file_row = 0; sound_play(SFX_TAB); break;
    case NAV_R: g_tab = (g_tab + 1) % kTabN; g_row = ROW_ACTIONS; g_file_row = 0; sound_play(SFX_TAB); break;
    case NAV_A:
        if (g_row == ROW_COPIES) {   // remembered: Play starts this copy from now on
            catalog_prefer(a.game, port.installs[g_copy].dir);
            g_copy = 0, g_row = ROW_ACTIONS, g_action = 0;
            in = port.installs.empty() ? nullptr : &port.installs[0];
            sound_play(SFX_TOGGLE_ON);
            toast("Play starts this copy from now on");
        }
        else if (g_row == ROW_RELEASES) { g_row = ROW_NOTES; sound_play(SFX_OK); }
        else if (g_row == ROW_ACTIONS) run(acts[g_action].id);
        else if (g_row == ROW_MODS && in) mods_open(a.game);
        else if (g_row == ROW_FILES && g_file_row < (int)frows.size()) { if (run_file_act(frows[g_file_row].id)) return; }
        else if (g_row == ROW_SETTINGS) { /* the tab body opens it (below), with a.nav still A */ }
        break;
    case NAV_X:
        if (g_tab == 1 && (g_row == ROW_RELEASES || g_row == ROW_NOTES) && !port.releases.empty())
            install_release(a.game, port, in, port.releases[g_rel], busy);
        break;
    case NAV_Y: if (!port.custom) run(ACT_GITHUB); break;
    case NAV_B:
        if (g_row == ROW_NOTES) { g_row = ROW_RELEASES; sound_play(SFX_BACK); break; }
        if (g_row != ROW_ACTIONS) { g_row = ROW_ACTIONS; sound_play(SFX_BACK); break; }
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

    // Per-game Time Played + last-session performance telemetry + multi-session FPS sparkline below cover art
    {
        PerfSummary perf = (in && !demo_mode()) ? port_last_perf(port, *in) : PerfSummary{};
        std::vector<PerfPoint> hist = demo_mode()
            ? std::vector<PerfPoint>{{1, 56.2f, 44.0f}, {2, 58.1f, 48.2f}, {3, 57.4f, 46.8f},
                                     {4, 59.3f, 52.1f}, {5, 59.8f, 54.4f}, {6, 59.9f, 56.3f}}
            : port_perf_history(port.id);
        if (demo_mode()) {
            perf.valid = true;
            perf.avg_fps = 59.8f;
            perf.low1_fps = 56.3f;
            perf.text = "59.8 fps  ·  1% low 56.3";
        }
        bool show_spark = hist.size() >= 2;
        float sy = show_spark ? 442.0f : 444.0f, sw = 330.0f;
        float sh = perf.valid ? (show_spark ? 204.0f : 192.0f) : 164.0f;
        rect(76, sy, sw, sh, p.panel, 16);
        rect(92, sy, sw - 32, 3, port.colours.accent, 1.5f);
        text(96, sy + 12, 12, p.dim, "TIME PLAYED");
        text(76 + sw - 20, sy + 12, 12, p.dim, fit(12, demo_mode() ? "Player 1" : cfg().profile, 140).c_str(), 1);
        std::string dur = demo_mode() ? "3 h 40 min" : played_duration_short(a.game);
        std::string lp  = demo_mode() ? "Today" : last_played_short(a.game);
        int sess = demo_mode() ? 6 : played_sessions(a.game);
        if (show_spark && perf.valid) {
            text(96, sy + 28, 22, p.text, fit(22, dur, sw - 40).c_str());
            rect(96, sy + 58, sw - 40, 1, p.line);
            text(96, sy + 66, 13, p.soft, "Last played");
            text(76 + sw - 20, sy + 66, 13, p.text, lp.c_str(), 1);
            text(96, sy + 88, 13, p.soft, "Sessions");
            text(76 + sw - 20, sy + 88, 13, p.text, sess > 0 ? std::to_string(sess).c_str() : "—", 1);
            text(96, sy + 110, 13, p.soft, "Last run");
            ImU32 fc = perf.avg_fps >= 55.0f ? p.ok : (perf.avg_fps >= 29.0f ? p.text : p.warn);
            text(76 + sw - 20, sy + 110, 13, fc, fit(13, perf.text, 215).c_str(), 1);
        } else {
            text(96, sy + 34, 25, p.text, fit(25, dur, sw - 40).c_str());
            rect(96, sy + 74, sw - 40, 1, p.line);
            text(96, sy + 86, 14, p.soft, "Last played");
            text(76 + sw - 20, sy + 86, 14, p.text, lp.c_str(), 1);
            text(96, sy + 114, 14, p.soft, "Sessions");
            text(76 + sw - 20, sy + 114, 14, p.text, sess > 0 ? std::to_string(sess).c_str() : "—", 1);
            if (perf.valid) {
                text(96, sy + 142, 14, p.soft, "Last run");
                ImU32 fc = perf.avg_fps >= 55.0f ? p.ok : (perf.avg_fps >= 29.0f ? p.text : p.warn);
                text(76 + sw - 20, sy + 143, 13, fc, fit(13, perf.text, 215).c_str(), 1);
            }
        }
        if (perf.valid && show_spark) {
            float gx = 96, gy = sy + 134, gw = sw - 40, gh = 58;
            rect(gx, gy, gw, gh, p.panel2, 8);
            float mn = hist[0].avg_fps, mx = hist[0].avg_fps;
            for (const PerfPoint &pt : hist) { mn = std::min(mn, pt.avg_fps); mx = std::max(mx, pt.avg_fps); }
            float lo = std::max(0.0f, std::floor(mn - 4.0f));
            float hi = std::max(lo + 8.0f, std::ceil(mx + 2.0f));
            std::vector<ImVec2> pts;
            pts.reserve(hist.size());
            for (size_t i = 0; i < hist.size(); ++i) {
                float fx = hist.size() > 1 ? (float)i / (float)(hist.size() - 1) : 1.0f;
                float fy = SDL_clamp((hist[i].avg_fps - lo) / (hi - lo), 0.0f, 1.0f);
                pts.push_back(P(gx + 10.0f + fx * (gw - 20.0f), gy + gh - 8.0f - fy * (gh - 24.0f)));
            }
            for (size_t i = 1; i < pts.size(); ++i) {
                ImVec2 b0(pts[i - 1].x, P(0, gy + gh - 5.0f).y), b1(pts[i].x, P(0, gy + gh - 5.0f).y);
                dl()->AddQuadFilled(pts[i - 1], pts[i], b1, b0, with_alpha(port.colours.accent, 36));
            }
            dl()->AddPolyline(pts.data(), (int)pts.size(), with_alpha(port.colours.accent, 235), ImDrawFlags_None, S(2.0f));
            for (size_t i = 0; i < pts.size(); ++i) {
                bool last_pt = (i + 1 == pts.size());
                dl()->AddCircleFilled(pts[i], S(last_pt ? 3.5f : 2.2f), last_pt ? p.ok : port.colours.accent);
            }
            char hlbl[48];
            SDL_snprintf(hlbl, sizeof hlbl, "FPS trend  ·  %d runs", (int)hist.size());
            text(gx + 8, gy + 4, 11, p.dim, hlbl);
            char rlbl[32];
            SDL_snprintf(rlbl, sizeof rlbl, "%.0f-%.0f fps", mn, mx);
            text(gx + gw - 8, gy + 4, 11, p.dim, rlbl, 1);
        }
    }

    float x = 452;
    text(x, 104, 15, port.colours.accent, port.custom ? "CUSTOM PC GAME" : (std::string("STEELTEMPLE  ") + port.name).c_str());
    text(x, 128, 38, p.text, fit(38, port.title, 780).c_str());
    text(x, 178, 16, p.soft, port.custom ? port.exe : (std::string("github.com/") + port.repo).c_str());

    float cx = x;
    auto chip = [&](const std::string &s, ImU32 fg, ImU32 bg) {
        float w = text_w(15, s.c_str()) + 28;
        rect(cx, 212, w, 32, bg, 16);
        text(cx + 14, 219, 15, fg, s.c_str());
        cx += w + 10;
    };
    if (busy) {
        static const char *ph[] = {"Waiting in Downloads", "Downloading", "Checking", "Installing", "Relinking", "Setup open"};
        std::string s = ph[std::min(job.phase, 5)];
        if (job.phase == PH_DOWNLOAD && job.total) s += " " + std::to_string((int)(100.0 * job.done / job.total)) + "%";
        chip(s, p.accent_text, with_alpha(p.accent, 50));
    } else if (in) chip(std::string("Installed ") + (in->version.empty() ? "" : in->version), p.ok, p.panel2);
    else chip("Not on this PC", p.soft, p.panel2);
    if (!busy && st == PORT_UPDATE) chip("Update " + port.latest_tag() + " available", p.accent_text, with_alpha(p.accent, 50));
    else if (!busy && in && latest) chip(in->version.empty() ? "Version unknown" : "Up to date", p.soft, p.panel2);
    if (running && game_suspended()) chip("Suspended  ·  Quick Resume", p.warn, with_alpha(p.warn, 45));
    else if (running) chip("Running  ·  " + std::to_string((int)running_seconds() / 60) + " min", p.ok, with_alpha(p.ok, 40));
    else if (in && !busy && !can_play(port, *in)) chip("First start: set up with your own game files", p.warn, p.panel2);
    if (in && !demo_mode() && !cfg().save_sync_dir.empty()) {
        SyncInfo si = sync_inspect(port, *in);
        if (si.kind == SYNC_CLOUD_NEWER || si.kind == SYNC_CONFLICT)
            chip("Cloud save newer" + (si.cloud_host.empty() ? std::string() : (" (" + si.cloud_host + ")")),
                 p.warn, with_alpha(p.warn, 42));
    }
    if (port.installs.size() > 1) chip(std::to_string(port.installs.size()) + " copies on this PC", p.soft, p.panel2);

    float ax = x;
    for (int i = 0; i < (int)acts.size(); ++i) {
        bool primary = i == 0;
        float w = text_w(19, acts[i].label.c_str()) + (primary ? 84 : 72);
        if (a.mouse_moved && !a.clicked && hover(ax, 268, w, 58) && (g_row != ROW_ACTIONS || g_action != i)) {
            g_row = ROW_ACTIONS, g_action = i;
            sound_play(SFX_MOVE);
        }
        if (a.clicked && hover(ax, 268, w, 58)) { g_row = ROW_ACTIONS, g_action = i; run(acts[i].id); }
        rect(ax, 268 + 4, w, 58, with_alpha(IM_COL32_BLACK, p.light ? 20 : 60), 29);
        rect(ax, 268, w, 58, primary ? port.colours.accent : p.panel, 29);
        ImU32 a_ = port.colours.accent;
        bool bright = ((a_ & 255) * 299 + ((a_ >> 8) & 255) * 587 + ((a_ >> 16) & 255) * 114) / 1000 > 170;
        ImU32 fg = primary ? (bright ? IM_COL32(20, 20, 20, 255) : IM_COL32_WHITE) : p.text;
        icon(acts[i].icon, ax + 32, 297, 20, fg);
        text(ax + 52, 285, 19, fg, acts[i].label.c_str());
        if (g_row == ROW_ACTIONS && i == g_action) focus_ring(ax, 268, w, 58, 29, 5, port.colours.accent);
        ax += w + 14;
    }

    float tx = x;
    for (int i = 0; i < kTabN; ++i) {
        float w = text_w(17, kTabs[i]) + 32;
        if (a.clicked && hover(tx, 352, w, 40)) { if (g_tab != i) sound_play(SFX_TAB); g_tab = i, g_row = ROW_ACTIONS; }
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
        if (has_lr && !running && lr_val.verdict != RUN_NONE) {
            ImU32 c = lr_val.verdict == RUN_CLEAN ? p.ok : lr_val.verdict == RUN_FIXED ? p.accent_text : p.warn;
            std::string head;
            int m = lr_val.seconds / 60;
            std::string dur = m >= 60 ? std::to_string(m / 60) + " h " + std::to_string(m % 60) + " min"
                            : m ? std::to_string(m) + " min" : std::to_string(lr_val.seconds) + " s";
            switch (lr_val.verdict) {
            case RUN_CLEAN: head = "Last run ended normally  ·  " + dur; break;
            case RUN_CRASH: head = "The last run stopped with an error after " + dur; break;
            case RUN_NO_START: head = "The game could not start"; break;
            case RUN_KNOWN: head = "The last run hit a known problem (issue " + lr_val.issue + ")"; break;
            case RUN_FIXED: head = "The last run hit a problem fixed in " + lr_val.fixed_in + ": update to get the fix"; break;
            }
            int lines = 1 + !lr_val.detail.empty() + (lr_val.verdict == RUN_NO_START);
            float ch = 40 + (lines - 1) * 20;
            rect(x, by - 4, 780, ch, with_alpha(c, 34), 10);
            rect(x, by - 4, 4, ch, c, 2);
            icon(lr_val.verdict == RUN_CLEAN ? IC_CHECK : IC_INFO, x + 24, by + 16, 18, c);
            text(x + 46, by + 5, 16, p.text, fit(16, head, 720).c_str());
            if (!lr_val.detail.empty() && !demo_mode()) text(x + 46, by + 30, 13, p.soft, fit(13, lr_val.detail, 720).c_str());
            if (lr_val.verdict == RUN_NO_START)
                text(x + 46, by + 30 + (lr_val.detail.empty() ? 0 : 20), 13, p.dim, "Its own launcher (Set up) can check the game files.");
            by += ch + 10;
        }
        if (port.installs.empty()) {
            text(x, by, 17, p.soft, "Not found on this PC. Sanctum looked in the Setup Wizards' entries and on every drive.");
            text(x, by + 28, 17, p.soft, "Download installs the latest release; its first start then asks for your own game files.");
        } else {
            text(x, by, 15, p.dim, port.installs.size() > 1 ? "Copies on this PC (Play starts the selected one)" : "On this PC");
            int rows = demo_mode() ? 1 : std::max(1, (int)((640 - (by + 26)) / 50));
            for (int i = 0; i < (int)port.installs.size() && i < rows; ++i) {
                const Install &c = port.installs[i];
                float y = by + 26 + i * 50;
                rect(x, y, 780, 42, i == g_copy ? p.panel : with_alpha(p.panel, 120), 10);
                if (i == g_copy) icon(IC_CHECK, x + 22, y + 21, 18, p.ok);
                text(x + 44, y + 10, 16, p.text, fit(16, shown_path(c.dir, port.title), 520).c_str());
                std::string tag = (c.version.empty() ? std::string("version unknown") : c.version) +
                                  (c.registered ? "  ·  Setup Wizard" : "") + (c.dev ? "  ·  build folder" : "");
                text(x + 768, y + 11, 14, p.dim, tag.c_str(), 1);
                if (g_row == ROW_COPIES && i == g_copy) focus_ring(x, y, 780, 42, 10, 3);
                if (a.clicked && hover(x, y, 780, 42) && g_copy != i) { g_copy = i; sound_play(SFX_MOVE); }
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
            if (hover(x, y, 190, 40)) { if (a.clicked) { g_rel = i, g_row = ROW_NOTES, g_scroll_to = 0; sound_play(SFX_OK); } }
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
        bool can_inst = has_installable_asset(in, r) && !(in && !in->version.empty() && tag_key(in->version) == tag_key(r.tag));
        dl()->PushClipRect(P(nx, ny + 6), P(nx + nw, ny + nh - 6), true);
        g_scroll = ease(g_scroll, g_scroll_to, 16);
        float y = ny + 14 - g_scroll, tw = nw - 36;
        y += text_wrap(nx + 18, y, 18, p.text, (r.title.empty() ? r.tag : r.title).c_str(), can_inst ? tw - 170 : tw) + 8;
        for (const Line &l : notes_lines(r.body)) {
            if (l.kind == 4) { y += 8; continue; }
            if (l.kind == 1) { y += 6; y += text_wrap(nx + 18, y, 16, port.colours.accent, l.text.c_str(), tw) + 4; continue; }
            float ix = l.kind == 2 ? 14 : l.kind == 3 ? 30 : 0;
            if (l.kind >= 2) circle(nx + 18 + ix - 8, y + 9, 2.2f, p.soft);
            y += text_wrap(nx + 18 + ix, y, 14, p.soft, l.text.c_str(), tw - ix) + 4;
        }
        float content = y + g_scroll - (ny + 14);
        g_notes_max = std::max(0.0f, content - nh + 28);
        g_notes_game = a.game, g_notes_rel = g_rel;
        g_scroll_to = std::min(g_scroll_to, g_notes_max);
        dl()->PopClipRect();
        if (can_inst && !busy) {
            const char *bl = in ? "Install version" : "Download";
            float bw = text_w(13, bl) + 28, bx = nx + nw - bw - 16, byb = ny + 10;
            if (a.clicked && hover(bx, byb, bw, 28)) install_release(a.game, port, in, r, busy);
            rect(bx, byb, bw, 28, with_alpha(port.colours.accent, hover(bx, byb, bw, 28) ? 235 : 190), 14);
            ImU32 a_ = port.colours.accent;
            bool bright = ((a_ & 255) * 299 + ((a_ >> 8) & 255) * 587 + ((a_ >> 16) & 255) * 114) / 1000 > 170;
            text(bx + bw / 2, byb + 7, 13, bright ? IM_COL32(20, 20, 20, 255) : IM_COL32_WHITE, bl, 0.5f);
        }
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
        if (hover(x, by, 780, 74)) {
            if (a.mouse_moved && !a.clicked && g_row != ROW_SETTINGS) { g_row = ROW_SETTINGS; sound_play(SFX_MOVE); }
            if (a.clicked) g_row = ROW_SETTINGS, a.nav = NAV_A;
        }
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
    case 3: {   // Mods
        if (!in) { text(x, by, 17, p.soft, "Install the port to manage its mods here."); break; }
        int en = 0, tot = mods_summary(in->dir, &en);
        std::string head = std::string(port.name) + " mods";
        std::string sub = tot ? std::to_string(en) + " of " + std::to_string(tot) + " mods enabled  ·  A opens the mod manager"
                              : "No mods installed yet  ·  A opens the mod manager and mods folder";
        if (hover(x, by, 780, 74)) {
            if (a.mouse_moved && !a.clicked && g_row != ROW_MODS) { g_row = ROW_MODS; sound_play(SFX_MOVE); }
            if (a.clicked) { g_row = ROW_MODS; mods_open(a.game); return; }
        }
        rect(x, by, 780, 74, p.panel, 14);
        rect(x, by, 6, 74, port.colours.accent, 3);
        icon(IC_MODS, x + 40, by + 37, 26, port.colours.accent);
        text(x + 72, by + 14, 18, p.text, head.c_str());
        text(x + 72, by + 42, 14, p.soft, fit(14, sub, 680).c_str());
        if (g_row == ROW_MODS) focus_ring(x, by, 780, 74, 14, 4);
        break;
    }
    case 4: {   // Files & storage
        if (!in) { text(x, by, 17, p.soft, "Install the port to manage its files and storage here."); break; }
        SizeInfo si = demo_mode() ? SizeInfo{true, false, 1420000000LL, 420} : folder_size_cached(in->dir);
        std::string loc = shown_path(in->dir, port.title);
        std::string sz = si.ready ? (mb(si.bytes) + "  ·  " + std::to_string(si.files) + " files") : "Calculating size...";
        rect(x, by, 780, 38, p.panel2, 10);
        text(x + 16, by + 10, 14, p.text, fit(14, loc, 430).c_str());
        text(x + 764, by + 10, 14, f_miss.empty() ? p.ok : p.warn,
             (sz + (f_miss.empty() ? "  ·  Ready" : "  ·  Missing files")).c_str(), 1);

        int n = (int)frows.size();
        const int kVis = 4;
        int top = std::max(0, std::min(g_file_row - 1, n - kVis));
        for (int i = top; i < n && i < top + kVis; ++i) {
            const FileRow &fr = frows[i];
            float y = by + 46 + (i - top) * 46;
            if (hover(x, y, 780, 40)) {
                if (a.mouse_moved && !a.clicked && (g_row != ROW_FILES || g_file_row != i)) {
                    g_row = ROW_FILES, g_file_row = i;
                    sound_play(SFX_MOVE);
                }
                if (a.clicked) { g_row = ROW_FILES, g_file_row = i; if (run_file_act(fr.id)) return; break; }
            }
            rect(x, y, 780, 40, p.panel, 10);
            ImU32 lc = fr.id == F_UNINSTALL ? p.warn : p.text;
            text(x + 18, y + 10, 16, lc, fr.label.c_str());
            text(x + 762, y + 11, 14, fr.id == F_UNINSTALL ? p.warn : p.soft, fr.value.c_str(), 1);
            if (fr.id == F_PREWARM_SHADERS) {
                WarmState ws = shader_warm_state();
                if (ws.active && ws.port == a.game) {
                    float frac = ws.total > 0 ? (float)ws.done / (float)ws.total : -1.0f;
                    progress_bar(x + 14, y + 34, 752, 3, frac, port.colours.accent);
                }
            }
            if (g_row == ROW_FILES && i == g_file_row) focus_ring(x, y, 780, 40, 10, 3, port.colours.accent);
        }
        if (n > kVis) {
            float h = 46 * kVis - 6, bh = h * kVis / n;
            rect(x + 788, by + 46, 3, h, p.line, 2);
            rect(x + 788, by + 46 + (h - bh) * top / std::max(1, n - kVis), 3, bh, p.soft, 2);
        }
        break;
    }
    default:
        break;
    }

    bool rel_installable = g_tab == 1 && !port.releases.empty() && has_installable_asset(in, port.releases[g_rel]) &&
                           !(in && !in->version.empty() && tag_key(in->version) == tag_key(port.releases[g_rel].tag));
    if (g_row == ROW_COPIES) hint_bar({{NAV_A, "Use this copy"}, {NAV_L, "Tabs"}, {NAV_B, "Back"}});
    else if (g_row == ROW_RELEASES && rel_installable) hint_bar({{NAV_A, "Read"}, {NAV_X, "Install version"}, {NAV_L, "Tabs"}, {NAV_B, "Back"}});
    else if (g_row == ROW_RELEASES) hint_bar({{NAV_A, "Read"}, {NAV_L, "Tabs"}, {NAV_B, "Back"}});
    else if (g_row == ROW_SETTINGS) hint_bar({{NAV_A, "Open settings"}, {NAV_L, "Tabs"}, {NAV_B, "Back"}});
    else if (g_row == ROW_MODS) hint_bar({{NAV_A, "Open mods"}, {NAV_L, "Tabs"}, {NAV_B, "Back"}});
    else if (g_row == ROW_FILES) hint_bar({{NAV_A, "OK"}, {NAV_L, "Tabs"}, {NAV_B, "Back"}});
    else if (g_row == ROW_NOTES && rel_installable) hint_bar({{NAV_X, "Install version"}, {NAV_Y, "Open on GitHub"}, {NAV_B, "Back to the list"}});
    else if (g_row == ROW_NOTES) hint_bar({{NAV_Y, "Open on GitHub"}, {NAV_B, "Back to the list"}});
    else if (port.custom) hint_bar({{NAV_A, acts[g_action].label.substr(0, acts[g_action].label.find("  ")).c_str()}, {NAV_L, "Tabs"}, {NAV_B, "Back"}});
    else hint_bar({{NAV_A, acts[g_action].label.substr(0, acts[g_action].label.find("  ")).c_str()}, {NAV_Y, "GitHub"}, {NAV_L, "Tabs"}, {NAV_B, "Back"}});

    // Inline text-input modal for custom launch args / env overrides
    if (g_prompt_open) {
        if (!a.typed.empty()) {
            for (char c : a.typed)
                if ((unsigned char)c >= 0x20 && g_prompt_buf.size() < 240) g_prompt_buf += c;
            sound_play(SFX_KEY);
        }
        if (a.key_back && !g_prompt_buf.empty()) { g_prompt_buf.pop_back(); sound_play(SFX_KEY); }
        bool save = a.key_enter || modal_nav == NAV_A;
        bool cancel = a.key_escape || modal_nav == NAV_B;
        bool clear = modal_nav == NAV_Y;
        float mw = 720, mh = 240, mx = 640 - mw / 2, my = 360 - mh / 2;
        if (modal_click) {
            if (hover(mx + mw - 140, my + mh - 64, 116, 42)) save = true;
            else if (hover(mx + mw - 268, my + mh - 64, 116, 42)) clear = true;
            else if (hover(mx + 24, my + mh - 64, 116, 42) || !hover(mx, my, mw, mh)) cancel = true;
        }
        if (clear) { g_prompt_buf.clear(); sound_play(SFX_BACK); }
        if (save) {
            auto &mp = g_prompt_kind == 0 ? cfg().launch_args : cfg().launch_env;
            if (g_prompt_buf.empty()) mp.erase(g_prompt_port);
            else mp[g_prompt_port] = g_prompt_buf;
            config_save();
            prompt_close();
            sound_play(SFX_TOGGLE_ON);
            toast(g_prompt_kind == 0 ? "Saved custom launch arguments" : "Saved custom environment variables");
        } else if (cancel) {
            prompt_close();
            sound_play(SFX_BACK);
        } else {
            rect(0, 0, 1280, 720, IM_COL32(0, 0, 0, 155));
            rect(mx, my, mw, mh, p.bg2, 20);
            text(mx + 28, my + 22, 22, p.text, g_prompt_kind == 0 ? "Custom launch arguments" : "Custom environment variables");
            text(mx + 28, my + 54, 14, p.soft,
                 g_prompt_kind == 0 ? "Example: --windowed --fps=120  (type on keyboard, Enter saves)"
                                    : "Example: KEY=VALUE;OTHER=1  (separated by semicolons, Enter saves)");
            rect(mx + 28, my + 90, mw - 56, 52, p.panel, 12);
            frame(mx + 28, my + 90, mw - 56, 52, p.focus, 12, 2.5f);
            std::string shown = g_prompt_buf + (std::fmod(a.now, 1.0) < 0.5 ? "|" : "");
            text(mx + 44, my + 106, 17, p.text, fit(17, shown, mw - 88).c_str());
            rect(mx + 24, my + mh - 64, 116, 42, p.panel, 21);
            text(mx + 82, my + mh - 52, 16, p.soft, "Cancel", 0.5f);
            rect(mx + mw - 268, my + mh - 64, 116, 42, p.panel, 21);
            text(mx + mw - 210, my + mh - 52, 16, p.warn, "Clear", 0.5f);
            rect(mx + mw - 140, my + mh - 64, 116, 42, p.accent, 21);
            text(mx + mw - 82, my + mh - 52, 16, IM_COL32_WHITE, "Save", 0.5f);
        }
    }
}

}  // namespace sanctum
