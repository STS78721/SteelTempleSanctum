#include "launch.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "app.h"
#include "config.h"
#include "discord_rpc.h"
#include "input.h"
#include "overlay.h"
#include "perf.h"
#include "port_settings.h"
#include "sound.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

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
    {"yatmb", {}, {"!NS2_SHOW_LAUNCHER=0", "!YATMB_SHOW_LAUNCHER=0"}, {"yatmb.exe"}, {}, "logs\\yatmb.log", nullptr},
};

static const Profile *profile_of(const Port &p) {
    for (const Profile &pr : kProfiles)
        if (!strcmp(pr.id, p.id)) return &pr;
    return nullptr;
}

static std::string port_log_rel(const Port &p) {
    if (!p.log_rel.empty()) return p.log_rel;
    if (const Profile *pr = profile_of(p); pr && pr->log) return pr->log;
    if (!p.custom && p.exe && p.exe[0]) {
        std::string stem = p.exe;
        if (stem.size() > 4 && SDL_strcasecmp(stem.c_str() + stem.size() - 4, ".exe") == 0)
            stem.resize(stem.size() - 4);
        if (!stem.empty()) return "logs\\" + stem + ".log";
    }
    return {};
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
static HANDLE g_proc = nullptr;
static int g_port = -1;
static Install g_in;
static Uint64 g_started;
static DWORD g_pid;
static bool g_starting;   // started, its window not up yet: Sanctum shows Starting and keeps the foreground
static bool g_suspended = false;
static bool g_overlay_paused = false;   // soft-frozen while Quick Overlay / Sanctum is in front (blocks background pad bleed)
static bool g_tree_frozen = false;      // exact 1-to-1 balance guard for NtSuspendProcess / NtResumeProcess
static Uint64 g_overlay_pause_at = 0;
static Uint64 g_suspend_at = 0;
static Uint64 g_suspended_ms = 0;
static std::map<int, LastRun> g_last;

using NtProcFn = LONG (NTAPI *)(HANDLE);
static NtProcFn nt_suspend_fn() {
    static NtProcFn fn = (NtProcFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSuspendProcess");
    return fn;
}
static NtProcFn nt_resume_fn() {
    static NtProcFn fn = (NtProcFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtResumeProcess");
    return fn;
}

static std::wstring utf8_w(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static std::wstring quote_arg_w(const std::wstring &s) {
    if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring out = L"\"";
    for (size_t i = 0; i < s.size(); ++i) {
        size_t bs = 0;
        while (i < s.size() && s[i] == L'\\') ++bs, ++i;
        if (i == s.size()) { out.append(bs * 2, L'\\'); break; }
        if (s[i] == L'"') { out.append(bs * 2 + 1, L'\\'); out += L'"'; }
        else { out.append(bs, L'\\'); out += s[i]; }
    }
    out += L'"';
    return out;
}

// the last-run check runs on a worker (THWOMP's checker is a Python start, about a second)
static std::mutex g_lock;
static std::vector<std::pair<int, LastRun>> g_judged;

static std::vector<std::string> split_args(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    bool in_q = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') { in_q = !in_q; continue; }
        if (!in_q && (c == ' ' || c == '\t')) {
            if (!cur.empty()) out.push_back(cur), cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static void apply_custom_env(SDL_Environment *env, const std::string &spec) {
    if (spec.empty()) return;
    size_t at = 0;
    while (at < spec.size()) {
        size_t end = spec.find_first_of(";\n", at);
        if (end == std::string::npos) end = spec.size();
        std::string tok = spec.substr(at, end - at);
        at = end + 1;
        size_t a = tok.find_first_not_of(" \t\r"), b = tok.find_last_not_of(" \t\r");
        if (a == std::string::npos) continue;
        tok = tok.substr(a, b - a + 1);
        size_t eq = tok.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
        SDL_SetEnvironmentVariable(env, k.c_str(), v.c_str(), true);
    }
}

std::string port_save_dir(const Port &p, const Install &in) {
    if (!strcmp(p.id, "mastersword")) {
        if (exists(in.dir + "\\user\\save")) return in.dir + "\\user\\save";
        if (exists(in.dir + "\\user")) return in.dir + "\\user";
        return {};
    }
    for (const char *rel : {"profile", "saves", "save", "user\\save"})
        if (exists(in.dir + "\\" + rel)) return in.dir + "\\" + rel;
    return {};
}

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
    if (play) {
        auto it = cfg().launch_args.find(p.id);
        if (it != cfg().launch_args.end() && !it->second.empty())
            for (auto &extra : split_args(it->second)) argv.push_back(std::move(extra));
    }

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
        // MasterSword creates its window before loading mastersword_options.ini: forward fullscreen=1 via HOS_FULLSCREEN
        // so it opens directly into borderless fullscreen instead of flashing a white-titlebar window while shaders load.
        if (!strcmp(p.id, "mastersword")) {
            Ini ms_ini;
            std::string fs;
            if (ms_ini.load(in.dir + "\\mastersword_options.ini") && ms_ini.get("fullscreen", fs) && fs == "1")
                SDL_SetEnvironmentVariable(env, "HOS_FULLSCREEN", "1", false);
        }
    } else if (play && !pr && !p.custom && p.launcher) {
        // Dynamically catalogued Universal-NS2 port: skip its own launcher on direct Play
        SDL_SetEnvironmentVariable(env, "NS2_SHOW_LAUNCHER", "0", true);
    }
    if (play) {
        auto it = cfg().launch_env.find(p.id);
        if (it != cfg().launch_env.end() && !it->second.empty()) apply_custom_env(env, it->second);
    }

    // Build UTF-16 command line & environment block and launch with CREATE_NO_WINDOW so console-subsystem games
    // (totk/ff/mtf/dkb/smbw/yatmb) never pop up a blank black terminal window beside/before the game window.
    std::wstring cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cmd += L' ';
        cmd += quote_arg_w(utf8_w(argv[i]));
    }
    std::vector<std::wstring> env_list;
    if (char **vars = SDL_GetEnvironmentVariables(env)) {
        for (int i = 0; vars[i]; ++i) {
            std::wstring we = utf8_w(vars[i]);
            if (!we.empty()) env_list.push_back(std::move(we));
        }
        SDL_free(vars);
    }
    SDL_DestroyEnvironment(env);
    std::sort(env_list.begin(), env_list.end(), [](const std::wstring &a, const std::wstring &b) {
        size_t ea = a.find(L'='), eb = b.find(L'=');
        std::wstring ka = a.substr(0, ea), kb = b.substr(0, eb);
        return _wcsicmp(ka.c_str(), kb.c_str()) < 0;
    });
    std::vector<wchar_t> env_block;
    for (const auto &we : env_list) {
        env_block.insert(env_block.end(), we.begin(), we.end());
        env_block.push_back(L'\0');
    }
    if (!env_block.empty()) env_block.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = si.hStdOutput = si.hStdError = nul ? nul : INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi{};
    std::wstring exe_w = utf8_w(exe), cwd_w = utf8_w(in.dir);
    DWORD flags = CREATE_NO_WINDOW | (env_block.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT);
    BOOL ok = CreateProcessW(exe_w.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags,
                             env_block.empty() ? nullptr : (LPVOID)env_block.data(), cwd_w.c_str(), &si, &pi);
    if (nul) CloseHandle(nul);
    if (!ok) {
        DWORD err = GetLastError();
        sound_play(SFX_ERROR);
        toast(std::string("Could not start ") + p.name + " (Win32 error " + std::to_string(err) + ")", 4);
        return false;
    }
    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;
    g_pid = pi.dwProcessId;
    sanctum_log("launch: %s pid=%lu exe=%s", p.id, (unsigned long)g_pid, exe.c_str());
    g_port = port_i, g_in = in, g_started = SDL_GetTicks();
    g_suspended = false, g_overlay_paused = false, g_tree_frozen = false, g_overlay_pause_at = 0;
    g_suspend_at = 0, g_suspended_ms = 0;
    g_last.erase(port_i);
    perf_boost_game_start(g_proc, g_pid);
    // Sanctum stays in front (its Starting screen) until the game's window is up, then hands it the foreground.
    if (g_pid) AllowSetForegroundWindow(g_pid);
    g_starting = true;
    sound_play(SFX_LAUNCH);
    discord_rpc_set_activity(std::string("Playing ") + p.title, p.custom ? "via SteelTemple Sanctum" : (std::string("SteelTemple ") + p.name));
    (void)w;
    return true;
}

// ---- the foreground, handed over and taken back
// Collect root_pid plus any descendant PIDs (needed for PyInstaller --onefile launchers and game bootstrappers
// where a child process owns the visible window).
static std::set<DWORD> collect_process_tree(DWORD root_pid) {
    std::set<DWORD> pids;
    if (!root_pid) return pids;
    pids.insert(root_pid);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    std::vector<std::pair<DWORD, DWORD>> edges;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) {
        do { edges.push_back({pe.th32ProcessID, pe.th32ParentProcessID}); } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    bool added = true;
    while (added) {
        added = false;
        for (const auto &[pid, ppid] : edges)
            if (pids.count(ppid) && pids.insert(pid).second) added = true;
    }
    return pids;
}

struct Find {
    const std::set<DWORD> *pids;
    HWND found;
    DWORD found_pid;
};
static BOOL CALLBACK find_window(HWND h, LPARAM lp) {
    Find *f = (Find *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!f->pids || !f->pids->count(pid) || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    wchar_t cls[64];
    if (GetClassNameW(h, cls, 64) &&
        (!wcscmp(cls, L"ConsoleWindowClass") || !wcscmp(cls, L"PseudoConsoleWindow") ||
         !wcscmp(cls, L"CASCADIA_HOSTING_WINDOW_CLASS")))
        return TRUE;   // a console or Windows Terminal beside the game is not the game
    if (!IsIconic(h)) {
        RECT r;
        if (!GetWindowRect(h, &r) || r.right - r.left < 160 || r.bottom - r.top < 120) return TRUE;   // a real window
    }
    f->found = h;
    f->found_pid = pid;
    return FALSE;
}

// Non-blocking foreground handoff to a game window (never calls synchronous BringWindowToTop/SetFocus across
// processes, so a game initializing Vulkan/shaders before pumping messages never freezes Sanctum).
static void handoff_to_game(HWND h, const std::set<DWORD> &pids, bool restoring = false) {
    for (DWORD pid : pids) AllowSetForegroundWindow(pid);
    if (!h) return;
    if (IsIconic(h) || restoring) ShowWindowAsync(h, IsZoomed(h) ? SW_SHOWMAXIMIZED : SW_RESTORE);
    SetForegroundWindow(h);
    SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
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
    if (GetForegroundWindow() != h) {
        // Fallback when Windows' foreground lock resists over a borderless fullscreen Vulkan/DXGI game:
        // briefly pulse TOPMOST -> NOTOPMOST (never cross-process AttachThreadInput, so a hung game can't block us).
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(h);
        BringWindowToTop(h);
    }
    SetFocus(h);
}

static HWND hwnd_of(SDL_Window *w) {
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
}

bool launch_starting() { return g_proc && g_starting; }
std::string launch_starting_title() { return g_port >= 0 ? ports()[g_port].title : ""; }

bool game_running() { return g_proc != nullptr; }
bool game_suspended() { return g_proc != nullptr && g_suspended; }
int running_port() { return g_proc ? g_port : -1; }
double running_seconds() {
    if (!g_proc) return 0;
    Uint64 now = SDL_GetTicks();
    Uint64 paused = g_suspended_ms + (g_suspended && now > g_suspend_at ? (now - g_suspend_at) : 0);
    Uint64 active = now > g_started + paused ? (now - g_started - paused) : 0;
    return active / 1000.0;
}

static BOOL CALLBACK post_close_cb(HWND h, LPARAM lp) {
    const auto *pids = (const std::set<DWORD> *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pids && pids->count(pid) && IsWindowVisible(h) && !GetWindow(h, GW_OWNER))
        PostMessageW(h, WM_CLOSE, 0, 0);
    return TRUE;
}

static BOOL CALLBACK minimize_game_cb(HWND h, LPARAM lp) {
    const auto *pids = (const std::set<DWORD> *)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pids && pids->count(pid) && IsWindowVisible(h) && !GetWindow(h, GW_OWNER))
        ShowWindowAsync(h, SW_MINIMIZE);
    return TRUE;
}

static bool suspend_resume_tree(bool do_suspend) {
    NtProcFn fn = do_suspend ? nt_suspend_fn() : nt_resume_fn();
    if (!fn || !g_proc) return false;
    std::set<DWORD> pids = collect_process_tree(g_pid);
    bool ok = (fn(g_proc) == 0);
    for (DWORD pid : pids) {
        if (pid == g_pid) continue;
        if (HANDLE ch = OpenProcess(PROCESS_SUSPEND_RESUME, FALSE, pid)) {
            fn(ch);
            CloseHandle(ch);
        }
    }
    return ok;
}

static bool set_tree_frozen(bool freeze) {
    if (!g_proc) { g_tree_frozen = false; return false; }
    if (freeze == g_tree_frozen) return true;   // idempotent: never double-suspend or double-resume
    if (!suspend_resume_tree(freeze)) return false;
    g_tree_frozen = freeze;
    return true;
}

void close_game(bool force) {
    if (!g_proc) return;
    std::set<DWORD> pids = collect_process_tree(g_pid);
    if (force || g_starting) {
        g_suspended = false;
        g_overlay_paused = false;
        g_tree_frozen = false;
        for (DWORD pid : pids) {
            if (pid == g_pid) continue;
            if (HANDLE ch = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
                TerminateProcess(ch, 1);
                CloseHandle(ch);
            }
        }
        TerminateProcess(g_proc, 1);
        return;
    }
    // Resume first if paused/suspended so the game can process WM_CLOSE and save cleanly
    if (g_tree_frozen) {
        set_tree_frozen(false);
        g_suspended = false;
        g_overlay_paused = false;
        SDL_Delay(40);
    }
    Find f{&pids, nullptr, 0};
    if (!pids.empty()) EnumWindows(find_window, (LPARAM)&f);
    if (f.found) EnumWindows(post_close_cb, (LPARAM)&pids);
    else TerminateProcess(g_proc, 1);
}

void launch_wake_sanctum(SDL_Window *w) {
    perf_sanctum_foreground(true);
    if (!w) return;
    SDL_RestoreWindow(w);
    if (cfg().fullscreen && !getenv("SANCTUM_WINDOWED")) SDL_SetWindowFullscreen(w, true);
    sound_set_paused(false);
    bring_to_front(hwnd_of(w), true);
    SDL_RaiseWindow(w);
}

void launch_overlay_pause(bool pause) {
    if (!g_proc || g_starting) return;
    if (pause) {
        if (g_suspended || g_overlay_paused) return;
        if (set_tree_frozen(true)) {
            g_overlay_paused = true;
            g_overlay_pause_at = SDL_GetTicks();
            if (g_port >= 0 && g_port < (int)ports().size())
                sanctum_log("launch: paused %s pid=%lu for Quick Overlay", ports()[g_port].id, (unsigned long)g_pid);
        }
    } else if (g_overlay_paused && !g_suspended) {
        input_wait_buttons_released(650);
        set_tree_frozen(false);
        g_overlay_paused = false;
        if (g_port >= 0 && g_port < (int)ports().size())
            sanctum_log("launch: unpaused %s pid=%lu after Quick Overlay", ports()[g_port].id, (unsigned long)g_pid);
    }
}

bool suspend_game(SDL_Window *w) {
    if (!g_proc || g_starting) return false;
    if (g_suspended) { launch_wake_sanctum(w); return true; }
    // Wait until controller/keyboard buttons are released before briefly unfreezing for DWM minimize
    input_wait_buttons_released(650);
    if (g_tree_frozen) set_tree_frozen(false);
    std::set<DWORD> pids = collect_process_tree(g_pid);
    if (!pids.empty()) EnumWindows(minimize_game_cb, (LPARAM)&pids);
    SDL_Delay(50);   // let DWM complete the minimize while the game window can still respond
    if (!set_tree_frozen(true)) return false;
    g_suspended = true;
    g_overlay_paused = false;
    g_suspend_at = SDL_GetTicks();
    perf_boost_game_stop();   // release 1 ms timer & High Performance power plan while suspended
    if (g_port >= 0 && g_port < (int)ports().size()) {
        const Port &p = ports()[g_port];
        discord_rpc_set_activity(std::string("Suspended  ·  ") + p.title, "Quick Resume in Sanctum");
        sanctum_log("launch: suspended %s pid=%lu", p.id, (unsigned long)g_pid);
    }
    launch_wake_sanctum(w);
    return true;
}

bool resume_game(SDL_Window *w) {
    if (!g_proc) return false;
    // Wait until the user lifts off A / B / Guide / Enter before unfreezing & handing focus back to the game,
    // so the button that triggered Quick Resume / Resume never bleeds into the game.
    input_wait_buttons_released(650);
    if (g_tree_frozen) {
        bool was_suspended = g_suspended;
        set_tree_frozen(false);
        if (was_suspended) {
            Uint64 now = SDL_GetTicks();
            if (now > g_suspend_at) g_suspended_ms += now - g_suspend_at;
        }
        g_suspended = false;
        g_overlay_paused = false;
        if (g_port >= 0 && g_port < (int)ports().size()) {
            const Port &p = ports()[g_port];
            if (was_suspended)
                discord_rpc_set_activity(std::string("Playing ") + p.title, p.custom ? "via SteelTemple Sanctum" : (std::string("SteelTemple ") + p.name));
            sanctum_log("launch: resumed %s pid=%lu", p.id, (unsigned long)g_pid);
        }
        SDL_Delay(40);
    }
    perf_boost_game_start(g_proc, g_pid);
    std::set<DWORD> pids = collect_process_tree(g_pid);
    Find f{&pids, nullptr, 0};
    if (!pids.empty()) EnumWindows(find_window, (LPARAM)&f);
    handoff_to_game(f.found, pids, true);
    if (w) {
        SDL_MinimizeWindow(w);
        if (HWND sw = hwnd_of(w); sw && !IsIconic(sw)) ShowWindowAsync(sw, SW_MINIMIZE);
    }
    perf_sanctum_foreground(false);
    sound_set_paused(true);
    return f.found != nullptr;
}

void launch_on_shutdown() {
    if (g_proc && g_tree_frozen) {
        set_tree_frozen(false);
        g_suspended = false;
        g_overlay_paused = false;
    }
}

// ---- play time (per profile: "p<idx>:<port_id>=<seconds>|<last_played>|<sessions>")
struct PlayStat {
    long long seconds = 0;
    long long last = 0;
    int sessions = 0;
};
static std::map<std::string, PlayStat> g_time;
static std::string playtime_path() { return config_dir() + "playtime.ini"; }

static std::string play_key(int port, int profile_idx = -1) {
    int pi = profile_idx < 0 ? cfg().active_profile : profile_idx;
    if (pi < 0) pi = 0;
    return "p" + std::to_string(pi) + ":" + std::string(ports()[port].id);
}

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
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.empty() || l[0] == '#') continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = l.substr(0, eq), rest = l.substr(eq + 1);
        // Backwards compatibility: legacy "<port_id>=<secs>|<last>" maps to Profile 0 ("p0:<port_id>")
        if (k.find(':') == std::string::npos) k = "p0:" + k;
        size_t b1 = rest.find('|');
        if (b1 == std::string::npos) continue;
        size_t b2 = rest.find('|', b1 + 1);
        PlayStat st;
        st.seconds = atoll(rest.substr(0, b1).c_str());
        st.last = atoll((b2 == std::string::npos ? rest.substr(b1 + 1) : rest.substr(b1 + 1, b2 - b1 - 1)).c_str());
        st.sessions = b2 == std::string::npos ? (st.last > 0 ? 1 : 0) : atoi(rest.substr(b2 + 1).c_str());
        if (st.last > 0 && st.sessions <= 0) st.sessions = 1;
        // If both legacy and p0: exist, keep the richer record
        auto &dst = g_time[k];
        if (st.seconds >= dst.seconds) dst = st;
    }
}

static void playtime_add(int port, long long secs) {
    auto &t = g_time[play_key(port)];
    t.seconds += secs;
    t.last = (long long)time(nullptr);
    t.sessions += 1;
    std::string s = "# SteelTemple Sanctum: time played per profile and port (p<profile>:<id>=seconds|last|sessions)\n";
    for (auto &e : g_time)
        s += e.first + "=" + std::to_string(e.second.seconds) + "|" + std::to_string(e.second.last) + "|" +
             std::to_string(e.second.sessions) + "\n";
    std::string tmp = playtime_path() + ".tmp";
    if (SDL_SaveFile(tmp.c_str(), s.data(), s.size())) SDL_RenamePath(tmp.c_str(), playtime_path().c_str());
}

long long played_seconds(int port) {
    if (port < 0 || port >= (int)ports().size()) return 0;
    auto it = g_time.find(play_key(port));
    return it == g_time.end() ? 0 : it->second.seconds;
}
long long last_played(int port) {
    if (port < 0 || port >= (int)ports().size()) return 0;
    auto it = g_time.find(play_key(port));
    return it == g_time.end() ? 0 : it->second.last;
}
int played_sessions(int port) {
    if (port < 0 || port >= (int)ports().size()) return 0;
    auto it = g_time.find(play_key(port));
    return it == g_time.end() ? 0 : it->second.sessions;
}

std::string played_duration_short(int port) {
    long long s = played_seconds(port), when = last_played(port);
    if (!when && s <= 0) return "Not played yet";
    if (s < 60) return "< 1 min";
    if (s < 3600) return std::to_string(s / 60) + " min";
    return std::to_string(s / 3600) + " h " + std::to_string((s % 3600) / 60) + " min";
}

std::string last_played_short(int port) {
    long long when = last_played(port);
    if (!when) return "Never";
    long long days = ((long long)time(nullptr) - when) / 86400;
    if (days <= 0) return "Today";
    if (days == 1) return "Yesterday";
    return std::to_string(days) + " days ago";
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

// ---- judging a run & last-session performance telemetry
static std::string tail(const std::string &path, size_t bytes) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wn <= 0) return {};
    std::wstring w(wn - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), wn);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    std::string out;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0) {
        LONGLONG take = sz.QuadPart > (LONGLONG)bytes ? (LONGLONG)bytes : sz.QuadPart;
        LARGE_INTEGER off{};
        off.QuadPart = sz.QuadPart - take;
        if (SetFilePointerEx(h, off, nullptr, FILE_BEGIN)) {
            out.resize((size_t)take);
            DWORD got = 0;
            if (ReadFile(h, out.data(), (DWORD)take, &got, nullptr)) out.resize(got);
            else out.clear();
        }
    }
    CloseHandle(h);
    return out;
}

struct PerfCache {
    Uint64 size = 0;
    SDL_Time mtime = 0;
    PerfSummary sum;
};
static std::map<std::string, PerfCache> g_perf_cache;

PerfSummary port_last_perf(const Port &p, const Install &in) {
    std::string rel = port_log_rel(p);
    if (rel.empty()) return {};
    std::string path = in.dir + "\\" + rel;
    SDL_PathInfo pi{};
    if (!SDL_GetPathInfo(path.c_str(), &pi) || pi.type != SDL_PATHTYPE_FILE || pi.size == 0) return {};
    auto it = g_perf_cache.find(path);
    if (it != g_perf_cache.end() && it->second.size == pi.size && it->second.mtime == pi.modify_time)
        return it->second.sum;

    PerfSummary ps{};
    std::string t = tail(path, 24576);
    size_t at = 0;
    while (at < t.size()) {
        size_t end = t.find('\n', at);
        if (end == std::string::npos) end = t.size();
        std::string l = t.substr(at, end - at);
        at = end + 1;
        // Parse "[host] frametime: exit: 967 frames in 16.3 s, mean 59.4 fps, 1% low 52.6 fps, ..."
        if (size_t fp = l.find("frametime: exit:"); fp != std::string::npos) {
            const char *s = l.c_str() + fp;
            if (const char *m = strstr(s, "mean ")) {
                float avg = (float)atof(m + 5);
                if (avg > 0.5f && avg < 1000.0f) {
                    ps.avg_fps = avg;
                    ps.valid = true;
                    ps.low1_fps = 0.0f;
                    ps.frames = atoi(s + 16);
                    if (const char *lo = strstr(m, "1% low ")) ps.low1_fps = (float)atof(lo + 7);
                }
            }
        }
        // Optional error counters on boot/summary lines
        if (size_t ve = l.find("validation errors "); ve != std::string::npos)
            ps.val_errs = atoi(l.c_str() + ve + 18);
        if (size_t se = l.find("shader failures "); se != std::string::npos)
            ps.shader_errs = atoi(l.c_str() + se + 16);
    }
    if (ps.valid) {
        char buf[80];
        if (ps.low1_fps > 0.1f)
            SDL_snprintf(buf, sizeof buf, "%.1f fps  ·  1%% low %.1f", ps.avg_fps, ps.low1_fps);
        else
            SDL_snprintf(buf, sizeof buf, "%.1f fps avg", ps.avg_fps);
        ps.text = buf;
    }
    g_perf_cache[path] = {pi.size, pi.modify_time, ps};
    return ps;
}

// ---- async headless shader pre-warm (<exe> --prepare-shaders, zero console window, live pipe progress)
static std::atomic<bool> g_warming{false};
static std::atomic<bool> g_warm_cancel{false};
static std::mutex g_warm_lock;
static WarmState g_warm_state;
static HANDLE g_warm_job = nullptr;
static std::string g_warm_toast;
static bool g_warm_ok = false;

struct WarmJob {
    int port;
    std::string name, exe, dir;
};

static void warm_parse_line(const std::string &line, std::string &summary_out) {
    // Phase 1: "gfx_shader: prepare: 4200 of 18240 pairs, 310 translated, 5012 ms"
    if (size_t at = line.find("gfx_shader: prepare:"); at != std::string::npos) {
        long long d = 0, tot = 0;
        if (sscanf(line.c_str() + at + 20, " %lld of %lld pairs", &d, &tot) == 2 && tot > 0) {
            int pct = (int)std::min(100LL, std::max(0LL, d * 100 / tot));
            char buf[96];
            SDL_snprintf(buf, sizeof buf, "Translating shaders  %d%% (%lld / %lld)", pct, d, tot);
            std::lock_guard<std::mutex> l(g_warm_lock);
            g_warm_state.stage = 1;
            g_warm_state.done = d;
            g_warm_state.total = tot;
            g_warm_state.label = buf;
            return;
        }
    }
    // Transition from translation to driver pipeline build
    if (size_t at = line.find("gfx_shader: prepared "); at != std::string::npos) {
        long long u = atoll(line.c_str() + at + 21);
        std::lock_guard<std::mutex> l(g_warm_lock);
        g_warm_state.stage = 2;
        g_warm_state.done = 0;
        g_warm_state.total = u > 0 ? u : 0;
        g_warm_state.label = "Building driver pipelines...";
        return;
    }
    // Phase 2: "stage library prebuild preparation progress: 8000 of 24000 variants"
    if (size_t at = line.find("stage library prebuild preparation progress:"); at != std::string::npos) {
        long long d = 0, tot = 0;
        if (sscanf(line.c_str() + at + 44, " %lld of %lld variants", &d, &tot) == 2 && tot > 0) {
            int pct = (int)std::min(100LL, std::max(0LL, d * 100 / tot));
            char buf[96];
            SDL_snprintf(buf, sizeof buf, "Building pipelines  %d%% (%lld / %lld)", pct, d, tot);
            std::lock_guard<std::mutex> l(g_warm_lock);
            g_warm_state.stage = 2;
            g_warm_state.done = d;
            g_warm_state.total = tot;
            g_warm_state.label = buf;
            return;
        }
    }
    // Final summary: "prepare-shaders: 255 variants built into the driver cache on 24 threads in 101 ms"
    if (size_t at = line.find("prepare-shaders:"); at != std::string::npos) {
        std::string s = line.substr(at + 16);
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (!s.empty()) summary_out = s;
    }
}

static int warm_worker(void *vp) {
    auto *j = (WarmJob *)vp;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rpipe = nullptr, wpipe = nullptr, nul_in = nullptr;
    if (!CreatePipe(&rpipe, &wpipe, &sa, 0)) {
        std::lock_guard<std::mutex> l(g_warm_lock);
        g_warm_state = {};
        g_warm_ok = false;
        g_warm_toast = j->name + ": could not start shader pre-warm";
        g_warming.store(false);
        delete j;
        return 0;
    }
    SetHandleInformation(rpipe, HANDLE_FLAG_INHERIT, 0);
    nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul_in ? nul_in : INVALID_HANDLE_VALUE;
    si.hStdOutput = wpipe;
    si.hStdError = wpipe;

    std::wstring cmd = quote_arg_w(utf8_w(j->exe)) + L" --prepare-shaders";
    std::wstring cwd = utf8_w(j->dir);
    PROCESS_INFORMATION pi{};
    // CREATE_NO_WINDOW ensures neither the game exe nor any child ns2_xlate processes open a console window
    BOOL ok = CreateProcessW(utf8_w(j->exe).c_str(), cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, cwd.c_str(), &si, &pi);
    CloseHandle(wpipe);
    if (nul_in) CloseHandle(nul_in);

    DWORD code = 1;
    std::string summary;
    if (ok) {
        // Bind process tree to a Job Object so cancellation or Sanctum exit cleanly terminates all translator workers
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jli{};
            jli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jli, sizeof jli);
            AssignProcessToJobObject(job, pi.hProcess);
            std::lock_guard<std::mutex> l(g_warm_lock);
            g_warm_job = job;
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);

        std::string pending;
        char buf[1024];
        DWORD got = 0;
        while (ReadFile(rpipe, buf, sizeof buf, &got, nullptr) && got > 0) {
            pending.append(buf, got);
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                warm_parse_line(pending.substr(0, nl), summary);
                pending.erase(0, nl + 1);
            }
        }
        if (!pending.empty()) warm_parse_line(pending, summary);
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);

        HANDLE close_job = nullptr;
        {
            std::lock_guard<std::mutex> l(g_warm_lock);
            close_job = g_warm_job;
            g_warm_job = nullptr;
        }
        if (close_job) CloseHandle(close_job);
    }
    CloseHandle(rpipe);

    bool cancelled = g_warm_cancel.load();
    {
        std::lock_guard<std::mutex> l(g_warm_lock);
        g_warm_state = {};
        if (cancelled) {
            g_warm_ok = false;
            g_warm_toast = j->name + ": shader pre-warm cancelled";
        } else if (ok && code == 0) {
            g_warm_ok = true;
            g_warm_toast = summary.empty() ? (j->name + ": shaders pre-warmed cleanly")
                                           : (j->name + ": " + summary);
        } else {
            g_warm_ok = false;
            g_warm_toast = j->name + ": shader pre-warm failed (code " + std::to_string(code) + ")";
        }
    }
    g_warming.store(false);
    g_warm_cancel.store(false);
    delete j;
    return 0;
}

bool shaders_warming() { return g_warming.load(); }

WarmState shader_warm_state() {
    std::lock_guard<std::mutex> l(g_warm_lock);
    return g_warm_state;
}

void cancel_prepare_shaders() {
    if (!g_warming.load()) return;
    g_warm_cancel.store(true);
    HANDLE job = nullptr;
    {
        std::lock_guard<std::mutex> l(g_warm_lock);
        job = g_warm_job;
        g_warm_job = nullptr;
        g_warm_state.label = "Cancelling...";
    }
    if (job) {
        TerminateJobObject(job, 1);
        CloseHandle(job);
    }
}

bool launch_prepare_shaders(int port_i, const Install &in) {
    if (port_i < 0 || port_i >= (int)ports().size()) return false;
    if (game_running()) { sound_play(SFX_ERROR); toast("Close the game first"); return false; }
    if (g_warming.exchange(true)) { sound_play(SFX_ERROR); toast("Shader pre-warm is already running"); return false; }
    const Port &p = ports()[port_i];
    std::string exe = in.dir + "\\" + p.exe;
    if (!exists(exe)) { g_warming.store(false); sound_play(SFX_ERROR); toast("Game executable not found"); return false; }
    g_warm_cancel.store(false);
    {
        std::lock_guard<std::mutex> l(g_warm_lock);
        g_warm_state = {true, port_i, 1, 0, 0, "Preparing shaders..."};
    }
    auto *j = new WarmJob{port_i, p.name, exe, in.dir};
    SDL_Thread *t = SDL_CreateThread(warm_worker, "prewarm", j);
    if (!t) {
        delete j;
        { std::lock_guard<std::mutex> l(g_warm_lock); g_warm_state = {}; }
        g_warming.store(false);
        sound_play(SFX_ERROR);
        return false;
    }
    SDL_DetachThread(t);
    sound_play(SFX_OK);
    toast(std::string("Pre-warming shaders for ") + p.name + " in the background...", 3.5);
    return true;
}

// THWOMP's checker: one line, "clean" / "known <issue>" / "fixed <issue> <tag>" / "crash" / nothing
static bool port_checker(const Install &in, LastRun &r) {
    std::string py = in.dir + "\\setup\\python\\python.exe", script = in.dir + "\\setup\\src\\tools\\prepare\\lastrun.py";
    if (!exists(py) || !exists(script)) return false;
    std::string ver = in.version.empty() ? "0" : in.version;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rpipe = nullptr, wpipe = nullptr, nul_h = nullptr;
    if (!CreatePipe(&rpipe, &wpipe, &sa, 0)) return false;
    SetHandleInformation(rpipe, HANDLE_FLAG_INHERIT, 0);
    nul_h = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul_h ? nul_h : INVALID_HANDLE_VALUE;
    si.hStdOutput = wpipe;
    si.hStdError = nul_h ? nul_h : INVALID_HANDLE_VALUE;
    std::wstring cmd = quote_arg_w(utf8_w(py)) + L" -I " + quote_arg_w(utf8_w(script)) +
                       L" --game " + quote_arg_w(utf8_w(in.dir)) + L" --version " + quote_arg_w(utf8_w(ver));
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(utf8_w(py).c_str(), cmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wpipe);
    if (nul_h) CloseHandle(nul_h);
    std::string line;
    if (ok) {
        char buf[512];
        DWORD got = 0;
        while (ReadFile(rpipe, buf, sizeof buf, &got, nullptr) && got > 0) line.append(buf, got);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(rpipe);
    if (!ok) return false;
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
        std::string wt;
        bool wok = false;
        {
            std::lock_guard<std::mutex> l(g_warm_lock);
            if (!g_warm_toast.empty()) { wt = std::move(g_warm_toast); g_warm_toast.clear(); wok = g_warm_ok; }
        }
        if (!wt.empty()) {
            sound_play(wok ? SFX_TOGGLE_ON : SFX_ERROR);
            toast(wt, 4);
        }
    }
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
    DWORD wait_r = WaitForSingleObject(g_proc, 0);
    if (wait_r != WAIT_OBJECT_0) {
        if (g_starting) {
            // the game's window is up (including child processes of PyInstaller launchers / game bootstrappers):
            // hand foreground asynchronously and minimize Sanctum right away.
            std::set<DWORD> pids = collect_process_tree(g_pid);
            Find f{&pids, nullptr, 0};
            if (!pids.empty()) EnumWindows(find_window, (LPARAM)&f);
            if (f.found || SDL_GetTicks() - g_started > 120000) {
                sanctum_log("launch: handoff pid=%lu (win pid=%lu) hwnd=%p after %.2fs",
                            (unsigned long)g_pid, (unsigned long)f.found_pid, (void *)f.found, running_seconds());
                g_starting = false;
                input_wait_buttons_released(300);
                handoff_to_game(f.found, pids, false);
                SDL_MinimizeWindow(w);
                if (HWND sw = hwnd_of(w); sw && !IsIconic(sw)) ShowWindowAsync(sw, SW_MINIMIZE);
                perf_sanctum_foreground(false);
                sound_set_paused(true);
            }
        } else if (g_overlay_paused && !g_suspended && !overlay_is_open() && SDL_GetTicks() - g_overlay_pause_at > 600) {
            // If the player Alt-Tabbed or clicked straight back into the game window while soft-paused, auto-resume cleanly
            if (HWND fg = GetForegroundWindow()) {
                DWORD fg_pid = 0;
                GetWindowThreadProcessId(fg, &fg_pid);
                if (fg_pid && collect_process_tree(g_pid).count(fg_pid)) {
                    launch_overlay_pause(false);
                    perf_sanctum_foreground(false);
                    sound_set_paused(true);
                }
            }
        }
        return;
    }
    DWORD exit_dw = 0;
    GetExitCodeProcess(g_proc, &exit_dw);
    int code = (int)exit_dw;
    sanctum_log("launch: exited pid=%lu code=%d after %.2fs", (unsigned long)g_pid, code, running_seconds());
    int secs = (int)running_seconds();
    CloseHandle(g_proc);
    g_proc = nullptr;
    g_pid = 0;
    g_suspended = false;
    g_overlay_paused = false;
    g_tree_frozen = false;
    perf_boost_game_stop();
    bool was_starting = g_starting;
    g_starting = false;
    if (secs >= 10) playtime_add(g_port, secs);
    // back in front: restored (still fullscreen if it was), and given the foreground the game just let go of
    SDL_RestoreWindow(w);
    if (cfg().fullscreen && !getenv("SANCTUM_WINDOWED")) SDL_SetWindowFullscreen(w, true);
    sound_set_paused(false);
    discord_rpc_set_activity("Browsing library", "SteelTemple Sanctum");
    bring_to_front(hwnd_of(w), !was_starting);
    SDL_RaiseWindow(w);
    // back on the game's page, its last run being judged
    overlay_on_game_exit();
    game_show_overview(g_port);
    std::string lrel = port_log_rel(ports()[g_port]);
    auto *j = new JudgeJob{g_port, code, g_in, {}};
    j->r.seconds = secs;
    if (!lrel.empty()) j->r.log = g_in.dir + "\\" + lrel;
    if (!exists(j->r.log)) j->r.log.clear();
    SDL_Thread *t = SDL_CreateThread(judge_worker, "lastrun", j);
    if (t) SDL_DetachThread(t);
    else delete j;
    g_port = -1;
}

bool last_run(int port, LastRun &out) {
    auto it = g_last.find(port);
    if (it == g_last.end()) return false;
    out = it->second;
    return true;
}
void last_run_clear(int port) { g_last.erase(port); }

}  // namespace sanctum
