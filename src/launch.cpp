#include "launch.h"

#include <atomic>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <vector>

#include "app.h"
#include "config.h"
#include "sound.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace sanctum {

// ---- each port's start profile, as its own launcher starts the game
//   {dir}  the copy's folder        {ver}  its version        {gpu}  NVIDIA / AMD / Intel
struct Profile {
    const char *id;
    std::vector<const char *> args;          // after the program
    std::vector<const char *> env;           // NAME=value, set only if the player has not set it; "!NAME=value"
                                             // always (from Sanctum a game starts directly, never on its start screen)
    std::vector<const char *> ready;         // relative paths that must exist to Play
    std::vector<const char *> make_dirs;     // created before the start
    const char *log;                         // the game's log, relative
    const char *profile_env;                 // the environment variable naming the player profile, or nullptr
};

static const Profile kProfiles[] = {
    // MKW-THWOMP: the start screen is inside mkw.exe; --play goes straight in. mkwgame.dll is the game the first
    // setup builds from the player's files (runtime/core/boot/main.c want_launcher)
    {"mkw-thwomp", {"--play"}, {"!MKW_NO_LAUNCHER=1"}, {"mkw.exe", "mkwgame.dll"}, {}, "logs\\mkw.log", "MKW_PLAYER"},
    // MasterSword: the dump beside the program, passed explicitly (MasterSwordLauncher.py launch_game)
    {"mastersword", {"{dir}\\image.bin", "--romfs", "{dir}\\romfs", "--user", "{dir}\\user"},
     {"MasterSword_GPU_NAME={gpu}"}, {"mastersword.exe", "image.bin", "romfs"}, {"user"}, "mastersword_log.txt", nullptr},
    // TotK: totk.exe from its folder, with exefs and romfs beside it (launcher/totk_launcher.cpp launch_game)
    {"totk", {}, {}, {"totk.exe", "exefs", "romfs"}, {}, "logs\\totk.log", nullptr},
    // the universal-tool ports: the program the first setup built, from its folder (MTFLauncher.py launch_game)
    // NS2_SHOW_LAUNCHER=0: the universal runtime's own start screen (runtime/ui/launcher) is skipped; the environment
    // wins over <game>.ini (runtime/cfg.c)
    {"slowseparation", {}, {"!NS2_SHOW_LAUNCHER=0"}, {"ff.exe"}, {}, "logs\\ff.log", nullptr},
    {"shadowracket", {}, {"!NS2_SHOW_LAUNCHER=0"}, {"mtf.exe"}, {}, "logs\\mtf.log", nullptr},
    // DKB: dkb.exe with the player's exefs and packed romfs (DKBLauncher.py launch_game)
    {"dkb-bananas", {}, {"DKB_GPU_NAME={gpu}", "!NS2_SHOW_LAUNCHER=0"}, {"dkb.exe", "exefs\\main", "romfs\\romfs.bin"}, {}, "logs\\dkb.log", nullptr},
    {"wonderbloom", {}, {"!NS2_SHOW_LAUNCHER=0"}, {"smbw.exe"}, {}, "logs\\smbw.log", nullptr},
};

static const Profile *profile_of(const Port &p) {
    for (const Profile &pr : kProfiles)
        if (!strcmp(pr.id, p.id)) return &pr;
    return nullptr;
}

static bool exists(const std::string &p) { return SDL_GetPathInfo(p.c_str(), nullptr); }

std::string missing_for_play(const Port &p, const Install &in) {
    const Profile *pr = profile_of(p);
    if (!pr) return exists(in.dir + "\\" + p.exe) ? "" : p.exe;
    std::string miss;
    for (const char *f : pr->ready)
        if (!exists(in.dir + "\\" + f)) miss += (miss.empty() ? "" : ", ") + std::string(f);
    return miss;
}

bool can_play(const Port &p, const Install &in) { return missing_for_play(p, in).empty(); }

static std::string gpu_vendor() {
    static std::string v;
    if (!v.empty()) return v;
    DISPLAY_DEVICEW d{};
    d.cb = sizeof d;
    v = "Other";
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &d, 0); ++i) {
        if (!(d.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)) continue;
        std::wstring s = d.DeviceString;
        if (s.find(L"NVIDIA") != std::wstring::npos) v = "NVIDIA";
        else if (s.find(L"AMD") != std::wstring::npos || s.find(L"Radeon") != std::wstring::npos) v = "AMD";
        else if (s.find(L"Intel") != std::wstring::npos) v = "Intel";
        break;
    }
    return v;
}

static std::string expand(const char *t, const Install &in) {
    std::string s = t;
    auto rep = [&](const char *k, const std::string &v) {
        for (size_t at; (at = s.find(k)) != std::string::npos;) s.replace(at, strlen(k), v);
    };
    rep("{dir}", in.dir);
    rep("{ver}", in.version);
    rep("{gpu}", gpu_vendor());
    return s;
}

// ---- the running game
static SDL_Process *g_proc;
static int g_port = -1;
static Install g_in;
static Uint64 g_started;
static DWORD g_pid;
static bool g_starting;   // started, its window not up yet: Sanctum shows Starting and keeps the foreground
static std::map<int, LastRun> g_last;

// the last-run check runs on a worker (THWOMP's checker is a Python start, about a second)
static std::mutex g_lock;
static std::vector<std::pair<int, LastRun>> g_judged;

bool launch(SDL_Window *w, int port_i, const Install &in, bool own_launcher) {
    if (g_proc) { sound_play(SFX_ERROR); toast("A game is already running"); return false; }
    if (demo_mode()) { sound_play(SFX_ERROR); toast("Demo mode: games don't start"); return false; }
    const Port &p = ports()[port_i];
    const Profile *pr = profile_of(p);
    bool play = !own_launcher && can_play(p, in);
    if (!play && !p.launcher) { sound_play(SFX_ERROR); toast(std::string(p.name) + " is missing " + missing_for_play(p, in)); return false; }

    std::string exe = in.dir + "\\" + (play ? p.exe : p.launcher);
    std::vector<std::string> argv{exe};
    if (play && pr)
        for (const char *a : pr->args) argv.push_back(expand(a, in));
    std::vector<const char *> args;
    for (auto &a : argv) args.push_back(a.c_str());
    args.push_back(nullptr);

    // the environment: Sanctum's own, plus what the port's launcher would add (never over the player's own values)
    SDL_Environment *env = SDL_CreateEnvironment(true);
    if (play && pr) {
        for (const char *d : pr->make_dirs) SDL_CreateDirectory((in.dir + "\\" + d).c_str());
        for (const char *e : pr->env) {
            bool force = e[0] == '!';
            std::string kv = expand(force ? e + 1 : e, in);
            size_t eq = kv.find('=');
            if (eq == std::string::npos || kv.substr(eq + 1) == "Other") continue;
            SDL_SetEnvironmentVariable(env, kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str(), force);
        }
        if (pr->profile_env && !cfg().profile.empty())
            SDL_SetEnvironmentVariable(env, pr->profile_env, cfg().profile.c_str(), false);
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args.data());
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
    SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, in.dir.c_str());
    // not "background": that would hide the exit code, which says whether the game started at all. Its standard
    // handles go nowhere instead; the game keeps running even if Sanctum closes.
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    g_proc = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    SDL_DestroyEnvironment(env);
    if (!g_proc) {
        sound_play(SFX_ERROR);
        toast(std::string("Could not start ") + p.name + ": " + SDL_GetError(), 4);
        return false;
    }
    g_port = port_i, g_in = in, g_started = SDL_GetTicks();
    g_last.erase(port_i);
    // Sanctum stays in front (its Starting screen) until the game's window is up, then hands it the foreground.
    // Minimising at once would let Windows give the foreground to whatever was behind (Explorer), and a game that
    // never gets it neither takes the keyboard nor hides and holds the mouse pointer.
    g_pid = (DWORD)SDL_GetNumberProperty(SDL_GetProcessProperties(g_proc), SDL_PROP_PROCESS_PID_NUMBER, 0);
    if (g_pid) AllowSetForegroundWindow(g_pid);
    g_starting = true;
    sound_play(SFX_LAUNCH);
    (void)w;
    return true;
}

// ---- the foreground, handed over and taken back
struct Find {
    DWORD pid;
    HWND found;
};
static BOOL CALLBACK find_window(HWND h, LPARAM lp) {
    Find *f = (Find *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != f->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    wchar_t cls[64];
    if (GetClassNameW(h, cls, 64) && (!wcscmp(cls, L"ConsoleWindowClass") || !wcscmp(cls, L"PseudoConsoleWindow")))
        return TRUE;   // a console beside the game is not the game
    RECT r;
    if (!GetWindowRect(h, &r) || r.right - r.left < 160 || r.bottom - r.top < 120) return TRUE;   // a real window
    f->found = h;
    return FALSE;
}

// Windows lets only the foreground process give the foreground away; when Sanctum is not it (coming back from a
// game), a tap of Alt lifts that lock for the next call, the usual way for a program the player is returning to.
static void bring_to_front(HWND h, bool unlock) {
    if (!h) return;
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    if (unlock) {
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[1].ki.wVk = VK_MENU;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
    }
    SetForegroundWindow(h);
    BringWindowToTop(h);
    SetFocus(h);
}

static HWND hwnd_of(SDL_Window *w) {
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
}

bool launch_starting() { return g_proc && g_starting; }
std::string launch_starting_title() { return g_port >= 0 ? ports()[g_port].title : ""; }

bool game_running() { return g_proc != nullptr; }
int running_port() { return g_proc ? g_port : -1; }
double running_seconds() { return g_proc ? (SDL_GetTicks() - g_started) / 1000.0 : 0; }

void close_game() {
    if (g_proc) SDL_KillProcess(g_proc, false);   // a polite close first (WM_CLOSE / Ctrl-Break)
}

// ---- play time
static std::map<std::string, std::pair<long long, long long>> g_time;   // id -> (seconds, last played)
static std::string playtime_path() { return config_dir() + "playtime.ini"; }

void playtime_load() {
    size_t n = 0;
    char *d = (char *)SDL_LoadFile(playtime_path().c_str(), &n);
    if (!d) return;
    std::string all(d, n);
    SDL_free(d);
    size_t at = 0;
    while (at < all.size()) {
        size_t end = all.find('\n', at);
        if (end == std::string::npos) end = all.size();
        std::string l = all.substr(at, end - at);
        at = end + 1;
        size_t a = l.find('='), b = l.find('|');
        if (a == std::string::npos || b == std::string::npos) continue;
        g_time[l.substr(0, a)] = {atoll(l.substr(a + 1, b - a - 1).c_str()), atoll(l.substr(b + 1).c_str())};
    }
}

static void playtime_add(int port, long long secs) {
    auto &t = g_time[ports()[port].id];
    t.first += secs, t.second = (long long)time(nullptr);
    std::string s = "# SteelTemple Sanctum: time played from Sanctum, per port (seconds|last played)\n";
    for (auto &e : g_time) s += e.first + "=" + std::to_string(e.second.first) + "|" + std::to_string(e.second.second) + "\n";
    std::string tmp = playtime_path() + ".tmp";
    if (SDL_SaveFile(tmp.c_str(), s.data(), s.size())) SDL_RenamePath(tmp.c_str(), playtime_path().c_str());
}

long long played_seconds(int port) {
    auto it = g_time.find(ports()[port].id);
    return it == g_time.end() ? 0 : it->second.first;
}
long long last_played(int port) {
    auto it = g_time.find(ports()[port].id);
    return it == g_time.end() ? 0 : it->second.second;
}

std::string played_text(int port) {
    long long s = played_seconds(port), when = last_played(port);
    if (!when) return {};
    std::string t = "Played ";
    if (s < 60) t += "a moment";
    else if (s < 3600) t += std::to_string(s / 60) + " min";
    else t += std::to_string(s / 3600) + " h " + std::to_string((s % 3600) / 60) + " min";
    long long days = ((long long)time(nullptr) - when) / 86400;
    t += days <= 0 ? "  ·  last today" : days == 1 ? "  ·  last yesterday" : "  ·  last " + std::to_string(days) + " days ago";
    return t;
}

// ---- judging a run
static std::string tail(const std::string &path, size_t bytes) {
    size_t n = 0;
    char *d = (char *)SDL_LoadFile(path.c_str(), &n);
    if (!d) return {};
    std::string s(d + (n > bytes ? n - bytes : 0), d + n);
    SDL_free(d);
    return s;
}

// THWOMP's checker: one line, "clean" / "known <issue>" / "fixed <issue> <tag>" / "crash" / nothing
static bool port_checker(const Install &in, LastRun &r) {
    std::string py = in.dir + "\\setup\\python\\python.exe", script = in.dir + "\\setup\\src\\tools\\prepare\\lastrun.py";
    if (!exists(py) || !exists(script)) return false;
    std::string ver = in.version.empty() ? "0" : in.version;
    const char *args[] = {py.c_str(), "-I", script.c_str(), "--game", in.dir.c_str(), "--version", ver.c_str(), nullptr};
    SDL_PropertiesID pr = SDL_CreateProperties();
    SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_Process *p = SDL_CreateProcessWithProperties(pr);
    SDL_DestroyProperties(pr);
    if (!p) return false;
    size_t n = 0;
    int code = 0;
    char *out = (char *)SDL_ReadProcess(p, &n, &code);   // waits for it (a second or two)
    SDL_DestroyProcess(p);
    std::string line = out ? std::string(out, n) : std::string();
    SDL_free(out);
    line = line.substr(0, line.find_first_of("\r\n"));
    if (line.empty()) return false;
    if (line == "clean") r.verdict = RUN_CLEAN;
    else if (line == "crash") r.verdict = RUN_CRASH;
    else if (line.rfind("known ", 0) == 0) r.verdict = RUN_KNOWN, r.issue = line.substr(6);
    else if (line.rfind("fixed ", 0) == 0) {
        r.verdict = RUN_FIXED;
        std::string rest = line.substr(6);
        size_t sp = rest.find(' ');
        r.issue = rest.substr(0, sp);
        if (sp != std::string::npos) r.fixed_in = rest.substr(sp + 1);
    } else return false;
    return true;
}

// any port: the exit code, and the last error the log names
static void judge_generic(int code, LastRun &r) {
    std::string t = r.log.empty() ? std::string() : tail(r.log, 16384);
    std::string last_err;
    size_t at = 0;
    while (at < t.size()) {
        size_t end = t.find('\n', at);
        if (end == std::string::npos) end = t.size();
        std::string l = t.substr(at, end - at), low;
        at = end + 1;
        for (char c : l) low += (char)SDL_tolower((unsigned char)c);
        for (const char *w : {"fatal", "panic", "unhandled", "exception", "assert", "access violation", "device lost", "crash"})
            if (low.find(w) != std::string::npos) { last_err = l; break; }
    }
    while (!last_err.empty() && (last_err.back() == '\r' || last_err.back() == ' ')) last_err.pop_back();
    if (r.seconds < 6 && code != 0) r.verdict = RUN_NO_START;
    else if (code != 0 || !last_err.empty()) r.verdict = RUN_CRASH;
    else r.verdict = RUN_CLEAN;
    if (code != 0) r.detail = "Exit code " + std::to_string(code);
    if (!last_err.empty()) r.detail += (r.detail.empty() ? "" : "  ·  ") + last_err.substr(0, 160);
}

struct JudgeJob {
    int port, code;
    Install in;
    LastRun r;
};

static int judge_worker(void *p) {
    JudgeJob *j = (JudgeJob *)p;
    if (!port_checker(j->in, j->r)) judge_generic(j->code, j->r);
    else if (j->code != 0 && j->r.verdict == RUN_CLEAN) j->r.detail = "Exit code " + std::to_string(j->code);
    std::lock_guard<std::mutex> l(g_lock);
    g_judged.push_back({j->port, j->r});
    delete j;
    return 0;
}

void launch_poll(SDL_Window *w) {
    {
        std::lock_guard<std::mutex> l(g_lock);
        for (auto &j : g_judged) {
            g_last[j.first] = j.second;
            const char *name = ports()[j.first].name;
            switch (j.second.verdict) {
            case RUN_CRASH: toast(std::string(name) + " stopped with an error: see its page", 4); break;
            case RUN_NO_START: toast(std::string(name) + " could not start: see its page", 4); break;
            case RUN_KNOWN: case RUN_FIXED: toast(std::string(name) + " hit a known problem: see its page", 4); break;
            default: break;
            }
        }
        g_judged.clear();
    }
    if (!g_proc) return;
    int code = 0;
    if (!SDL_WaitProcess(g_proc, false, &code)) {
        if (g_starting) {
            // the game's window is up: it gets the foreground (Sanctum still has it, so this is allowed), then
            // Sanctum steps aside. A game slow to open a window (a first shader build) still gets it within 2 minutes.
            Find f{g_pid, nullptr};
            if (g_pid) EnumWindows(find_window, (LPARAM)&f);
            if (f.found || SDL_GetTicks() - g_started > 120000) {
                g_starting = false;
                bring_to_front(f.found, false);
                SDL_MinimizeWindow(w);
                sound_set_paused(true);
            }
        }
        return;
    }
    SDL_DestroyProcess(g_proc);
    g_proc = nullptr;
    bool was_starting = g_starting;
    g_starting = false;
    int secs = (int)((SDL_GetTicks() - g_started) / 1000);
    if (secs >= 10) playtime_add(g_port, secs);
    // back in front: restored (still fullscreen if it was), and given the foreground the game just let go of
    SDL_RestoreWindow(w);
    sound_set_paused(false);
    bring_to_front(hwnd_of(w), !was_starting);
    SDL_RaiseWindow(w);
    // back on the game's page, its last run being judged
    game_show_overview(g_port);
    const Profile *pr = profile_of(ports()[g_port]);
    auto *j = new JudgeJob{g_port, code, g_in, {}};
    j->r.seconds = secs;
    if (pr && pr->log) j->r.log = g_in.dir + "\\" + pr->log;
    if (!exists(j->r.log)) j->r.log.clear();
    SDL_Thread *t = SDL_CreateThread(judge_worker, "lastrun", j);
    if (t) SDL_DetachThread(t);
    else delete j;
    g_port = -1;
}

const LastRun *last_run(int port) {
    auto it = g_last.find(port);
    return it == g_last.end() ? nullptr : &it->second;
}
void last_run_clear(int port) { g_last.erase(port); }

}  // namespace sanctum
