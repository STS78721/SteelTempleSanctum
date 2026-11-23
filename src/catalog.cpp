#include "catalog.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>

#include "app.h"
#include "config.h"
#include "covers.h"
#include "install.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace sanctum {

static std::vector<Port> g_ports = {
    {"mkw-thwomp", "Mario Kart World", "MKW-THWOMP", "STS78721/MKW-THWOMP", "0.2.3b", "204 MB", "mariokartworld", "MKW",
     "mkw.exe", "mkwgame.id", nullptr, "MKW-THWOMP", {IM_COL32(232, 36, 48, 255), IM_COL32(90, 12, 18, 255)}},
    {"mastersword", "The Legend of Zelda: Ocarina of Time", "MasterSword", "STS78721/OOT-MASTERSWORD", "0.2.3", "176 MB",
     "ocarinaoftime", "OoT", "mastersword.exe", nullptr, "MasterSwordLauncher.exe", nullptr,
     {IM_COL32(47, 158, 90, 255), IM_COL32(122, 90, 18, 255)}},
    {"totk", "The Legend of Zelda: Tears of the Kingdom", "TotK SteelTempleBuild", "STS78721/totk-pc-recomp-SteelTempleBuild",
     "Beta-6.3", "97 MB", "tearsofthekingdom", "TotK", "totk.exe", nullptr, "TotKLauncher.exe", nullptr,
     {IM_COL32(31, 143, 134, 255), IM_COL32(59, 42, 16, 255)}},
    {"slowseparation", "Fast Fusion", "SlowSeparation", "STS78721/SlowSeparation", "Alpha-0.1.2a", "158 MB", "fastfusion",
     "FF", "ff.exe", nullptr, "FFLauncher.exe", nullptr, {IM_COL32(255, 46, 136, 255), IM_COL32(15, 13, 34, 255)}},
    {"shadowracket", "Mario Tennis Fever", "ShadowRacket", "STS78721/MTF-ShadowRacket", "Alpha-0.1.3b", "156 MB",
     "mariotennisfever", "MTF", "mtf.exe", nullptr, "MTFLauncher.exe", nullptr,
     {IM_COL32(204, 242, 42, 255), IM_COL32(18, 36, 24, 255)}},
    {"dkb-bananas", "Donkey Kong Bananza", "dkb-BANANAS", "STS78721/dkb-BANANAS", "Alpha-1g", "321 MB",
     "donkeykongbananza", "DKB", "dkb.exe", nullptr, "DKBLauncher.exe", nullptr,
     {IM_COL32(242, 181, 28, 255), IM_COL32(90, 50, 16, 255)}},
    // WonderBloom: the start screen is inside smbw.exe (no separate launcher program); its setup is the release zip
    {"wonderbloom", "Super Mario Bros. Wonder", "WonderBloom", "STS78721/SMBWONDERBLOOM", "0.2.1", "143 MB",
     "supermariobroswonder", "SMBW", "smbw.exe", nullptr, nullptr, nullptr,
     {IM_COL32(156, 72, 232, 255), IM_COL32(38, 16, 72, 255)}},
};

static const int kBuiltinCount = (int)g_ports.size();
int builtin_port_count() { return kBuiltinCount; }
std::vector<Port> &ports() { return g_ports; }

// Stable string backing for user-added custom PC games (std::deque never invalidates c_str() on push_back)
struct CustomBacking {
    std::string id, title, name, cover_key, mono, exe, dir;
    PortColours colours;
};
static std::deque<CustomBacking> g_custom_store;
static std::string custom_games_path() { return config_dir() + "custom_games.ini"; }
static void custom_games_load();
static void custom_games_save();

static void library_save();
static bool exists(const std::string &p);
static void refilter();
static void rank(std::vector<Install> &v);
static void apply_known_versions(const Port &p, std::vector<Install> &v);
static bool version_newer(const std::string &a, const std::string &b);

bool port_files(const Port &p, const std::string &dir) {
    return exists(dir + "\\" + p.exe) || (p.launcher && exists(dir + "\\" + p.launcher));
}
bool port_ready(const Port &p, const Install &in) { return exists(in.dir + "\\" + p.exe); }

static std::string lower(std::string s) {
    for (char &c : s) c = (char)SDL_tolower((unsigned char)c);
    return s;
}

std::string mb(long long b) {
    char s[32];
    if (b >= 1000LL * 1024 * 1024 * 1024) SDL_snprintf(s, sizeof s, "%.2f TB", b / (1024.0 * 1024 * 1024 * 1024));
    else if (b >= 1000LL * 1024 * 1024) SDL_snprintf(s, sizeof s, "%.1f GB", b / (1024.0 * 1024 * 1024));
    else SDL_snprintf(s, sizeof s, "%lld MB", (b + 512 * 1024) / (1024 * 1024));
    return s;
}

std::string Port::download_size() const {
    if (const Release *r = latest()) {
        const Asset *a = r->find(ASSET_PORTABLE);
        if (!a) a = r->find(ASSET_WIZARD);
        if (a) return mb(a->size);
    }
    return known_size;
}

// "Beta 6.3", "beta-6.3" and "Beta-6.3" are the same release
std::string tag_key(const std::string &s) {
    std::string k;
    for (unsigned char c : s)
        if (SDL_isalnum(c)) k += (char)SDL_tolower(c);
    if (k.size() > 1 && k[0] == 'v' && SDL_isdigit((unsigned char)k[1])) k.erase(0, 1);
    return k;
}

PortState Port::state() const {
    const Install *b = best();
    if (!b) return PORT_ABSENT;
    if (custom) return PORT_INSTALLED;
    if (b->version.empty()) return PORT_INSTALLED;
    std::string have = tag_key(b->version);
    if (have == tag_key(latest_tag())) return PORT_INSTALLED;
    // only when the installed version is an older release of this port: a newer local build is no "update"
    for (const Release &r : releases)
        if (tag_key(r.tag) == have) return version_newer(latest_tag(), b->version) ? PORT_UPDATE : PORT_INSTALLED;
    return releases.empty() && known_tag && version_newer(known_tag, b->version) ? PORT_UPDATE : PORT_INSTALLED;
}

static std::string read_version(const std::string &dir, const char *exe = nullptr);

static bool folder_matches_port(const Port &p, const std::string &dir) {
    bool game = exists(dir + "\\" + p.exe) && (!p.also || exists(dir + "\\" + p.also));
    return game || (p.launcher && exists(dir + "\\" + p.launcher));
}

std::string catalog_match_folder(const std::string &dir) {
    if (dir.empty()) return {};
    std::string out;
    for (const Port &p : g_ports)
        if (folder_matches_port(p, dir)) out += (out.empty() ? "" : ", ") + std::string(p.name);
    return out;
}

int catalog_add_folder(const std::string &dir) {
    if (dir.empty()) return 0;
    std::string clean = dir;
    while (clean.size() > 3 && (clean.back() == '\\' || clean.back() == '/')) clean.pop_back();
    int added = 0;
    for (size_t i = 0; i < g_ports.size(); ++i) {
        Port &p = g_ports[i];
        if (!folder_matches_port(p, clean)) continue;
        bool had = false;
        for (Install &in : p.all)
            if (lower(in.dir) == lower(clean)) {
                in.dev = false;
                in.launcher = p.launcher && exists(clean + "\\" + p.launcher);
                if (in.version.empty()) in.version = read_version(clean, p.exe);
                had = true;
                break;
            }
        if (!had) {
            Install in;
            in.dir = clean;
            in.version = read_version(clean, p.exe);
            in.launcher = p.launcher && exists(clean + "\\" + p.launcher);
            in.dev = false;
            p.all.push_back(std::move(in));
        }
        apply_known_versions(p, p.all);
        rank(p.all);
        if (p.installs.empty()) cfg().prefer[p.id] = clean;
        ++added;
    }
    if (added) {
        refilter();
        library_save();
        config_save();
        version_probe_start();
    }
    return added;
}

bool catalog_remove(int port, const std::string &dir) {
    if (port < 0 || port >= (int)g_ports.size()) return false;
    Port &p = g_ports[port];
    std::string low = lower(dir);
    size_t before = p.all.size();
    p.all.erase(std::remove_if(p.all.begin(), p.all.end(), [&](const Install &in) { return lower(in.dir) == low; }),
                p.all.end());
    if (p.all.size() == before) return false;
    auto it = cfg().prefer.find(p.id);
    if (it != cfg().prefer.end() && lower(it->second) == low) {
        cfg().prefer.erase(it);
        config_save();
    }
    refilter();
    library_save();
    return true;
}

void catalog_installed(int port, const std::string &dir, const std::string &version) {
    Port &p = g_ports[port];
    for (Install &in : p.all)
        if (lower(in.dir) == lower(dir)) {
            in.version = version;
            refilter();
            library_save();
            return;
        }
    Install in;
    in.dir = dir, in.version = version;
    in.launcher = p.launcher && exists(dir + "\\" + p.launcher);
    p.all.insert(p.all.begin(), in);   // the copy just installed is the one Play starts
    refilter();
    library_save();
}

// A copy inside a build, staging or backup folder: a developer's working tree, not a player's install.
static bool is_dev_path(const std::string &dir) {
    // Sanctum's own install folder holds players' installs, wherever it is
    std::string lib = lower(library_dir());
    if (lower(dir).rfind(lib + "\\", 0) == 0) return false;
    static const char *const seg[] = {"build", "_backups", "_release_prep", "runs", "work", "temp", "tmp", "payload",
                                      "dist", "obj", "out", "staging", "ffmerge-build", "upstream-mkw-recomp"};
    std::string d = lower(dir);
    size_t at = 0;
    while (at < d.size()) {
        size_t end = d.find_first_of("\\/", at);
        if (end == std::string::npos) end = d.size();
        std::string s = d.substr(at, end - at);
        for (const char *x : seg)
            if (s == x) return true;
        if (s.rfind("stage_", 0) == 0 || s.rfind("build-", 0) == 0) return true;
        at = end + 1;
    }
    return false;
}

// "0.2.10" after "0.2.9", "Beta-6.3" after "Beta-6.2b", "0.2.3.b"/"0.2.3b" after "0.2.3": numbers as numbers, rest as text
static bool version_newer(const std::string &a, const std::string &b) {
    size_t i = (a.size() > 1 && (a[0] == 'v' || a[0] == 'V') && SDL_isdigit((unsigned char)a[1])) ? 1 : 0;
    size_t j = (b.size() > 1 && (b[0] == 'v' || b[0] == 'V') && SDL_isdigit((unsigned char)b[1])) ? 1 : 0;
    auto sep_before_alpha = [](const std::string &s, size_t k) {
        return k + 1 < s.size() && (s[k] == '.' || s[k] == '-' || s[k] == '_') && SDL_isalpha((unsigned char)s[k + 1]);
    };
    while (i < a.size() && j < b.size()) {
        if (SDL_isdigit((unsigned char)a[i]) && SDL_isdigit((unsigned char)b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && SDL_isdigit((unsigned char)a[i2])) ++i2;
            while (j2 < b.size() && SDL_isdigit((unsigned char)b[j2])) ++j2;
            long long x = atoll(a.substr(i, i2 - i).c_str()), y = atoll(b.substr(j, j2 - j).c_str());
            if (x != y) return x > y;
            i = i2, j = j2;
            continue;
        }
        if (sep_before_alpha(a, i) && SDL_isalpha((unsigned char)b[j])) { ++i; continue; }
        if (sep_before_alpha(b, j) && SDL_isalpha((unsigned char)a[i])) { ++j; continue; }
        char x = (char)SDL_tolower((unsigned char)a[i]), y = (char)SDL_tolower((unsigned char)b[j]);
        if (x == ' ' || x == '-') x = '-';
        if (y == ' ' || y == '-') y = '-';
        if (x != y) return x > y;
        ++i, ++j;
    }
    return a.size() - i > b.size() - j;   // "0.2.2c" / "0.2.2.b" after "0.2.2"
}

// best first: a Setup Wizard's install, a player's copy before a build folder, the newest version (an unknown one
// last), one with its launcher, then the shortest path
static void rank(std::vector<Install> &v) {
    std::stable_sort(v.begin(), v.end(), [](const Install &a, const Install &b) {
        if (a.registered != b.registered) return a.registered;
        if (a.dev != b.dev) return !a.dev;
        if (a.version.empty() != b.version.empty()) return !a.version.empty();
        if (tag_key(a.version) != tag_key(b.version)) return version_newer(a.version, b.version);
        if (a.launcher != b.launcher) return a.launcher;
        return a.dir.size() < b.dir.size();
    });
}

// installs = the copies a player sees: all of them (build folders too, ranked last) unless Settings hides those;
// the copy the player chose ("Use this copy") first, so Play starts it
static void refilter() {
    for (Port &p : g_ports) {
        p.installs.clear();
        p.hidden = 0;
        for (const Install &in : p.all)
            if (!cfg().hide_dev || !in.dev) p.installs.push_back(in);
            else p.hidden++;
        auto it = cfg().prefer.find(p.id);
        if (it == cfg().prefer.end()) continue;
        for (size_t i = 0; i < p.installs.size(); ++i)
            if (lower(p.installs[i].dir) == lower(it->second)) {
                std::rotate(p.installs.begin(), p.installs.begin() + i, p.installs.begin() + i + 1);
                break;
            }
    }
}

void catalog_prefer(int port, const std::string &dir) {
    cfg().prefer[g_ports[port].id] = dir;
    config_save();
    refilter();
}

void catalog_refilter() { refilter(); }

// ---- small file helpers
static bool exists(const std::string &p) { return SDL_GetPathInfo(p.c_str(), nullptr); }

static std::string exe_stem_of(const char *exe) {
    if (!exe) return {};
    std::string s = lower(exe);
    if (size_t dot = s.rfind('.'); dot != std::string::npos) s.resize(dot);
    return s;
}

static std::string companion_dll(const std::string &dir, const char *id, const char *exe) {
    std::string stem = exe_stem_of(exe);
    if (!stem.empty()) {
        std::string p = dir + "\\" + stem + "_vk.dll";
        if (exists(p)) return p;
    }
    if (id && strcmp(id, "mastersword") == 0) {
        std::string p = dir + "\\hookshot_vk.dll";
        if (exists(p)) return p;
    }
    return {};
}

static std::string read_version(const std::string &dir, const char *exe) {
    std::vector<std::string> files;
    std::string stem = exe_stem_of(exe);
    if (!stem.empty()) files.push_back(stem + "_version.txt");
    files.push_back("VERSION");
    files.push_back("launcher\\version.txt");
    files.push_back("version.txt");
    for (const std::string &f : files) {
        size_t n = 0;
        char *d = (char *)SDL_LoadFile((dir + "\\" + f).c_str(), &n);
        if (!d) continue;
        std::string s(d, std::min<size_t>(n, 64));
        SDL_free(d);
        s = s.substr(0, s.find_first_of("\r\n"));
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
        if (lower(s).rfind("version=", 0) == 0) {
            s.erase(0, 8);
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
        }
        // a version is short and plain (not, say, a resource script that happens to be called version.txt)
        if (!s.empty() && s.size() <= 24 && s.find_first_of("(=#{") == std::string::npos) return s;
    }
    return {};
}

// ---- results: found on the worker, merged on the UI thread
static std::mutex g_lock;
static std::vector<std::vector<Install>> g_found;   // per port, while a search runs
static std::atomic<bool> g_running{false}, g_done{false};
static std::atomic<int> g_folders{0};

static void add_found(std::vector<std::vector<Install>> &out, size_t port, Install in) {
    for (Install &e : out[port])
        if (lower(e.dir) == lower(in.dir)) {   // the same folder found twice (registry and disk)
            e.registered |= in.registered;
            if (e.version.empty()) e.version = in.version;
            return;
        }
    out[port].push_back(std::move(in));
}

static void check_folder(const std::string &dir, const char *const *names, int count,
                         std::vector<std::vector<Install>> &out) {
    // names: the file names of this folder (lower case)
    auto has = [&](const char *f) {
        std::string l = lower(f);
        for (int i = 0; i < count; ++i)
            if (l == names[i]) return true;
        return false;
    };
    for (int i = 0; i < kBuiltinCount; ++i) {
        const Port &p = g_ports[i];
        // the game program (built or shipped), or the port's own launcher alone: a fresh install of a port that
        // builds its game on this PC at the first start has only the launcher
        bool game = has(p.exe) && (!p.also || has(p.also));
        if (!game && !(p.launcher && has(p.launcher))) continue;
        Install in;
        in.dir = dir;
        in.version = read_version(dir, p.exe);
        in.launcher = p.launcher && has(p.launcher);
        add_found(out, (size_t)i, std::move(in));
    }
}

#ifdef _WIN32
static std::string utf8(const wchar_t *w) {
    char out[2048];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, sizeof out, nullptr, nullptr);
    return n > 0 ? std::string(out) : std::string();
}

static std::string reg_string(HKEY key, const std::wstring &sub, const wchar_t *name) {
    wchar_t buf[1024];
    DWORD size = sizeof buf;
    if (RegGetValueW(key, sub.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return utf8(buf);
}

// Folders never worth opening: the system, caches, toolchains and source trees' insides.
static bool skip_dir(const wchar_t *n) {
    static const wchar_t *const skip[] = {L"windows", L"$recycle.bin", L"system volume information", L"programdata",
                                          L"appdata", L"node_modules", L"__pycache__", L"site-packages", L"llvm",
                                          L"third_party", L"msys64", L"windowsapps", L"recovery", L"perflogs",
                                          L"intel", L"amd", L"nvidia", L"drivers", L"winsxs", L"cmake"};
    if (n[0] == L'.' || n[0] == L'$') return true;
    for (const wchar_t *s : skip)
        if (!_wcsicmp(n, s)) return true;
    return false;
}

static void walk(const std::wstring &dir, int depth, std::vector<std::vector<Install>> &out) {
    if (++g_folders > 400000) return;   // a hard stop on enormous disks
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> subdirs;
    std::vector<std::string> files;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   // junctions: no loops, no doubles
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..") && !skip_dir(fd.cFileName))
                subdirs.push_back(fd.cFileName);
        } else {
            size_t len = wcslen(fd.cFileName);
            bool exe = len > 4 && !_wcsicmp(fd.cFileName + len - 4, L".exe");
            bool id = len > 3 && !_wcsicmp(fd.cFileName + len - 3, L".id");
            if (exe || id) files.push_back(lower(utf8(fd.cFileName)));
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!files.empty()) {
        std::vector<const char *> names;
        for (auto &f : files) names.push_back(f.c_str());
        check_folder(utf8(dir.c_str()), names.data(), (int)names.size(), out);
    }
    if (depth <= 0) return;
    for (auto &s : subdirs) walk(dir + L"\\" + s, depth - 1, out);
}
#endif

static int search(void *) {
    std::vector<std::vector<Install>> out((size_t)kBuiltinCount);
#ifdef _WIN32
    // 1. the Setup Wizards' entries in Windows' apps list
    for (int i = 0; i < kBuiltinCount; ++i) {
        if (!g_ports[i].uninstall_key) continue;
        std::wstring sub = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";
        for (const char *c = g_ports[i].uninstall_key; *c; ++c) sub += (wchar_t)*c;
        std::string dir = reg_string(HKEY_CURRENT_USER, sub, L"InstallLocation");
        while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
        if (dir.empty() || !port_files(g_ports[i], dir)) continue;
        Install in;
        in.dir = dir;
        in.version = reg_string(HKEY_CURRENT_USER, sub, L"DisplayVersion");
        if (in.version.empty()) in.version = read_version(dir, g_ports[i].exe);
        in.launcher = g_ports[i].launcher && exists(dir + "\\" + g_ports[i].launcher);
        in.registered = true;
        add_found(out, i, std::move(in));
    }
    // 2. Sanctum's own install folder, wherever it is (it may sit in a folder the drive search skips, e.g. AppData)
    {
        std::string lib = library_dir();
        int n = MultiByteToWideChar(CP_UTF8, 0, lib.c_str(), -1, nullptr, 0);
        std::wstring w(n ? n - 1 : 0, L'\0');
        if (n) MultiByteToWideChar(CP_UTF8, 0, lib.c_str(), -1, w.data(), n);
        if (!w.empty()) walk(w, 3, out);
    }
    // 3. every fixed drive, six folders deep (D:\Games\<port>\GAME\totk.exe is four)
    DWORD drives = GetLogicalDrives();
    for (int d = 0; d < 26; ++d) {
        if (!(drives & (1u << d))) continue;
        wchar_t root[4] = {(wchar_t)(L'A' + d), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        root[2] = 0;
        walk(root, 6, out);
    }
#endif
    for (auto &v : out) {
        for (Install &in : v) in.dev = !in.registered && is_dev_path(in.dir);
        rank(v);
    }
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_found = std::move(out);
    }
    g_done = true;
    g_running = false;
    return 0;
}

void scan_start() {
    if (g_running) return;
    g_running = true;
    g_done = false;
    g_folders = 0;
    SDL_Thread *t = SDL_CreateThread(search, "scan", nullptr);
    if (t) SDL_DetachThread(t);
    else g_running = false;
}

bool scan_running() { return g_running; }
int scan_folders() { return g_folders; }

// ---- library.ini: the last search, shown instantly at the next start
//   port=<id>|<dir>|<version>|<launcher 0/1>|<registered 0/1>
static std::string library_path() { return config_dir() + "library.ini"; }

static void library_save() {
    if (demo_mode()) return;
    std::string s = "# SteelTemple Sanctum: ports found on this PC (rebuilt by every search)\n";
    for (int i = 0; i < kBuiltinCount && i < (int)g_ports.size(); ++i) {
        const Port &p = g_ports[i];
        for (const Install &in : p.all)
            s += std::string("port=") + p.id + "|" + in.dir + "|" + in.version + "|" + (in.launcher ? "1" : "0") + "|" +
                 (in.registered ? "1" : "0") + "\n";
    }
    std::string tmp = library_path() + ".tmp";
    if (SDL_SaveFile(tmp.c_str(), s.data(), s.size())) SDL_RenamePath(tmp.c_str(), library_path().c_str());
}

static const PortColours kCustomPalette[] = {
    {IM_COL32(64, 156, 255, 255), IM_COL32(18, 42, 78, 255)},
    {IM_COL32(255, 128, 64, 255), IM_COL32(82, 34, 16, 255)},
    {IM_COL32(52, 200, 152, 255), IM_COL32(14, 60, 48, 255)},
    {IM_COL32(224, 92, 168, 255), IM_COL32(72, 22, 54, 255)},
    {IM_COL32(138, 114, 255, 255), IM_COL32(36, 28, 78, 255)},
    {IM_COL32(236, 192, 56, 255), IM_COL32(76, 56, 16, 255)},
};

static void append_custom_port(CustomBacking b) {
    g_custom_store.push_back(std::move(b));
    const CustomBacking &cb = g_custom_store.back();
    Port p{};
    p.id = cb.id.c_str();
    p.title = cb.title.c_str();
    p.name = cb.name.c_str();
    p.repo = "";
    p.known_tag = "";
    p.known_size = "";
    p.cover_key = cb.cover_key.c_str();
    p.mono = cb.mono.c_str();
    p.exe = cb.exe.c_str();
    p.also = nullptr;
    p.launcher = nullptr;
    p.uninstall_key = nullptr;
    p.colours = cb.colours;
    p.custom = true;
    Install in;
    in.dir = cb.dir;
    p.all.push_back(in);
    p.installs.push_back(in);
    g_ports.push_back(std::move(p));
}

static void custom_games_save() {
    if (demo_mode()) return;
    std::string s = "# SteelTemple Sanctum: custom PC games added by player (id|title|dir|exe|colour_idx)\n";
    for (int i = kBuiltinCount; i < (int)g_ports.size(); ++i) {
        const Port &p = g_ports[i];
        if (!p.custom || p.all.empty()) continue;
        int cidx = 0;
        for (int k = 0; k < 6; ++k)
            if (kCustomPalette[k].accent == p.colours.accent) { cidx = k; break; }
        s += std::string("game=") + p.id + "|" + p.title + "|" + p.all[0].dir + "|" + p.exe + "|" + std::to_string(cidx) + "\n";
    }
    std::string tmp = custom_games_path() + ".tmp";
    if (SDL_SaveFile(tmp.c_str(), s.data(), s.size())) SDL_RenamePath(tmp.c_str(), custom_games_path().c_str());
}

static std::string make_mono(const std::string &title) {
    std::string m;
    bool take = true;
    for (unsigned char c : title) {
        if (SDL_isalnum(c)) {
            if (take && m.size() < 4) m += (char)SDL_toupper(c);
            take = false;
        } else take = true;
    }
    if (m.size() < 2) {
        m.clear();
        for (unsigned char c : title) if (SDL_isalnum(c) && m.size() < 4) m += (char)SDL_toupper(c);
    }
    return m.empty() ? std::string("PC") : m;
}

static void custom_games_load() {
    if (demo_mode()) return;
    size_t n = 0;
    char *d = (char *)SDL_LoadFile(custom_games_path().c_str(), &n);
    if (!d) return;
    std::string all(d, n);
    SDL_free(d);
    size_t at = 0;
    while (at < all.size()) {
        size_t end = all.find('\n', at);
        if (end == std::string::npos) end = all.size();
        std::string line = all.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("game=", 0) != 0) continue;
        std::vector<std::string> f;
        size_t s = 5;
        for (size_t bar; (bar = line.find('|', s)) != std::string::npos; s = bar + 1) f.push_back(line.substr(s, bar - s));
        f.push_back(line.substr(s));
        if (f.size() < 4 || f[0].empty() || f[1].empty() || f[2].empty() || f[3].empty()) continue;
        if (!exists(f[2] + "\\" + f[3])) continue;
        int cidx = f.size() >= 5 ? atoi(f[4].c_str()) : 0;
        CustomBacking b;
        b.id = f[0];
        b.title = f[1];
        b.name = "PC Game";
        b.cover_key = tag_key(f[1]);
        b.mono = make_mono(f[1]);
        b.dir = f[2];
        b.exe = f[3];
        b.colours = kCustomPalette[(cidx >= 0 ? cidx : 0) % 6];
        append_custom_port(std::move(b));
    }
}

int catalog_add_custom(const std::string &exe_path, const std::string &custom_title) {
    if (exe_path.empty() || !exists(exe_path)) return -1;
    std::string clean = exe_path;
    for (char &c : clean) if (c == '/') c = '\\';
    size_t sl = clean.find_last_of('\\');
    if (sl == std::string::npos || sl + 1 >= clean.size()) return -1;
    std::string dir = clean.substr(0, sl);
    std::string exe = clean.substr(sl + 1);
    // Avoid duplicates of exact same folder + exe
    for (int i = 0; i < (int)g_ports.size(); ++i)
        if (!g_ports[i].all.empty() && lower(g_ports[i].all[0].dir) == lower(dir) && lower(g_ports[i].exe) == lower(exe))
            return i;

    std::string title = custom_title;
    if (title.empty()) {
        // Prefer parent folder name unless generic ("bin", "build", "dist", "game", "games")
        size_t psl = dir.find_last_of('\\');
        std::string folder = psl == std::string::npos ? dir : dir.substr(psl + 1);
        std::string lf = lower(folder);
        bool generic = lf.empty() || lf == "bin" || lf == "build" || lf == "dist" || lf == "game" || lf == "games" ||
                       lf == "release" || lf == "x64" || lf == "win64" || (folder.size() == 2 && folder[1] == ':');
        if (!generic) title = folder;
        else {
            size_t dot = exe.rfind('.');
            title = dot == std::string::npos ? exe : exe.substr(0, dot);
        }
    }
    std::string slug = tag_key(title);
    if (slug.empty()) slug = "game";
    std::string id = "custom-" + slug;
    int suffix = 2;
    for (;;) {
        bool dup = false;
        for (const Port &p : g_ports) if (id == p.id) { dup = true; break; }
        if (!dup) break;
        id = "custom-" + slug + "-" + std::to_string(suffix++);
    }
    CustomBacking b;
    b.id = id;
    b.title = title;
    b.name = "PC Game";
    b.cover_key = tag_key(title);
    b.mono = make_mono(title);
    b.dir = dir;
    b.exe = exe;
    b.colours = kCustomPalette[(int)(g_ports.size() - kBuiltinCount) % 6];
    append_custom_port(std::move(b));
    custom_games_save();
    covers_reload();
    return (int)g_ports.size() - 1;
}

bool catalog_remove_custom(int port) {
    if (port < kBuiltinCount || port >= (int)g_ports.size() || !g_ports[port].custom) return false;
    std::string id = g_ports[port].id;
    g_ports.erase(g_ports.begin() + port);
    cfg().pinned.erase(id);
    cfg().port_covers.erase(id);
    cfg().launch_args.erase(id);
    cfg().launch_env.erase(id);
    config_save();
    custom_games_save();
    covers_reload();
    return true;
}

void library_load() {
    size_t n = 0;
    char *d = (char *)SDL_LoadFile(library_path().c_str(), &n);
    if (d) {
        std::string all(d, n);
        SDL_free(d);
        size_t at = 0;
        while (at < all.size()) {
            size_t end = all.find('\n', at);
            if (end == std::string::npos) end = all.size();
            std::string line = all.substr(at, end - at);
            at = end + 1;
            if (line.rfind("port=", 0)) continue;
            std::vector<std::string> f;
            size_t s = 5;
            for (size_t bar; (bar = line.find('|', s)) != std::string::npos; s = bar + 1) f.push_back(line.substr(s, bar - s));
            f.push_back(line.substr(s));
            if (f.size() < 5) continue;
            for (int i = 0; i < kBuiltinCount; ++i) {
                Port &p = g_ports[i];
                if (f[0] == p.id && port_files(p, f[1])) {
                    Install in{f[1], f[2], f[3] == "1", f[4] == "1"};
                    in.dev = !in.registered && is_dev_path(in.dir);
                    p.all.push_back(in);
                }
            }
        }
        for (int i = 0; i < kBuiltinCount; ++i) {   // the versions read last time, from the first frame on
            apply_known_versions(g_ports[i], g_ports[i].all);
            rank(g_ports[i].all);
        }
    }
    custom_games_load();
    refilter();
}

// ---- versions read from the game programs
// A port's program carries its release tag as text (rt_port_version: "0.2.2c", "Alpha 0.1.3b", "Beta 6.3"), which
// is the truth about a copy: a Setup Wizard's registry entry and a VERSION file both go stale when a copy is updated
// another way. The worker reads each program once (8 MB at a time) for every spelling of the port's release tags,
// a match standing alone (no letter, digit or dot either side); the newest tag found wins.
struct ProbeJob {
    int port;
    std::string dir, exe, dll, id, exe_stem, title, tag_sig;
    std::vector<std::string> tags;   // newest first
};
struct ProbeHit {
    int port;
    std::string dir, version;
};
static std::mutex g_probe_lock;
static std::vector<ProbeHit> g_probe_hits;
static std::atomic<bool> g_probe_running{false}, g_probe_again{false};
static std::map<std::string, std::string> g_probe_cache;   // "<exe>|<size>|<time>[+<dll_size>:<dll_time>]|v3|<tag_sig>" -> version
static bool g_probe_cache_loaded = false;
static std::string probe_cache_path() { return config_dir() + "versions.ini"; }

static std::string tags_signature(const std::vector<std::string> &tags) {
    return (tags.empty() ? std::string("none") : tags[0]) + ":" + std::to_string(tags.size());
}

static std::string port_tags_signature(const Port &p) {
    if (p.releases.empty()) return std::string("known:") + p.known_tag;
    return p.releases[0].tag + ":" + std::to_string(p.releases.size());
}

static void probe_cache_load() {
    g_probe_cache_loaded = true;
    size_t n = 0;
    char *d = (char *)SDL_LoadFile(probe_cache_path().c_str(), &n);
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
        size_t eq = l.rfind('=');
        if (eq == std::string::npos || l.empty() || l[0] == '#') continue;
        std::string k = l.substr(0, eq);
        // Drop legacy v1/v2 entries (without |v3|): v3 also tracks companion renderer DLLs (e.g. mastersword_vk.dll)
        if (k.find("|v3|") == std::string::npos) continue;
        g_probe_cache[k] = l.substr(eq + 1);
    }
}

static void probe_cache_save() {
    if (demo_mode()) return;
    std::string s = "# SteelTemple Sanctum: versions read from game programs (<program>|<size>|<time>|v3|<tags>=<version>)\n";
    for (auto &e : g_probe_cache) s += e.first + "=" + e.second + "\n";
    std::string tmp = probe_cache_path() + ".tmp";
    if (SDL_SaveFile(tmp.c_str(), s.data(), s.size())) SDL_RenamePath(tmp.c_str(), probe_cache_path().c_str());
}

static std::string probe_prefix(const std::string &exe, const std::string &dll = {}) {
    SDL_PathInfo i;
    if (!SDL_GetPathInfo(exe.c_str(), &i)) return {};
    std::string s = exe + "|" + std::to_string(i.size) + "|" + std::to_string(i.modify_time);
    SDL_PathInfo di;
    if (!dll.empty() && SDL_GetPathInfo(dll.c_str(), &di))
        s += "+" + std::to_string(di.size) + ":" + std::to_string(di.modify_time);
    return s + "|v3|";
}

static bool valid_ver(const std::string &s) {
    if (s.size() < 3 || s.size() > 24) return false;
    bool digit = false, dot = false;
    for (unsigned char c : s) {
        if (SDL_isdigit(c)) digit = true;
        else if (c == '.') dot = true;
        else if (!(SDL_isalpha(c) || c == ' ' || c == '-' || c == '_')) return false;
    }
    return digit && dot;
}

static std::string probe_direct_chunk(const char *buf, size_t len, const std::string &id, const std::string &stem,
                                      const std::string &title) {
    auto find_seq = [&](const char *pat, size_t plen, size_t from) -> size_t {
        if (plen == 0 || from + plen > len) return std::string::npos;
        const char *it = std::search(buf + from, buf + len, pat, pat + plen);
        return it == buf + len ? std::string::npos : (size_t)(it - buf);
    };
    // 1. Universal-NS2 embedded runtime table: \0<port_ver>\0<game_ver>\0<stem>\0+<Title>\0
    if (!title.empty() && !stem.empty()) {
        std::string t_nul = title + '\0';
        for (size_t at = 0; (at = find_seq(t_nul.data(), t_nul.size(), at)) != std::string::npos; ++at) {
            size_t p = at;
            int nuls = 0;
            while (p > 0 && buf[p - 1] == '\0' && nuls < 16) --p, ++nuls;
            if (nuls == 0 || p < stem.size() + 2) continue;
            if (memcmp(buf + p - stem.size(), stem.data(), stem.size()) != 0 || buf[p - stem.size() - 1] != '\0')
                continue;
            // walk back across <game_ver> (e.g. "1.0.0", "1.1.1")
            size_t g_end = p - stem.size() - 1;
            size_t g_start = g_end;
            while (g_start > 0 && buf[g_start - 1] != '\0' && g_end - g_start < 16) --g_start;
            if (g_start == 0 || buf[g_start - 1] != '\0' || g_end - g_start < 3) continue;
            // walk back across <port_ver> (e.g. "Alpha 0.1.2", "0.2.1")
            size_t v_end = g_start - 1;
            size_t v_start = v_end;
            while (v_start > 0 && buf[v_start - 1] != '\0' && v_end - v_start < 28) --v_start;
            std::string ver(buf + v_start, v_end - v_start);
            if (valid_ver(ver)) return ver;
        }
    }
    // 2. MasterSword: \0<ver> (source <hex>)\0
    if (id == "mastersword") {
        const char *tag = " (source ";
        size_t tlen = strlen(tag);
        for (size_t at = 0; (at = find_seq(tag, tlen, at)) != std::string::npos; ++at) {
            size_t h = at + tlen, h0 = h;
            while (h < len && SDL_isxdigit((unsigned char)buf[h]) && h - h0 < 16) ++h;
            if (h - h0 < 6 || h + 1 >= len || buf[h] != ')' || buf[h + 1] != '\0') continue;
            size_t v_start = at;
            while (v_start > 0 && buf[v_start - 1] != '\0' && at - v_start < 24) --v_start;
            std::string ver(buf + v_start, at - v_start);
            if (valid_ver(ver)) return ver;
        }
    }
    // 3. MKW-THWOMP: MKW_COLOR_GAMMA\0%+.0f %%\0+<ver>\0
    if (id == "mkw-thwomp") {
        static const char kMkw[] = "MKW_COLOR_GAMMA\0%+.0f %%\0";
        size_t mlen = sizeof(kMkw) - 1;
        size_t at = find_seq(kMkw, mlen, 0);
        if (at != std::string::npos) {
            size_t p = at + mlen;
            int nuls = 0;
            while (p < len && buf[p] == '\0' && nuls < 12) ++p, ++nuls;
            size_t e = p;
            while (e < len && buf[e] != '\0' && e - p < 24) ++e;
            std::string ver(buf + p, e - p);
            if (valid_ver(ver)) return ver;
        }
    }
    return {};
}

static std::string probe_binary(const std::string &path, const ProbeJob &j) {
    // every spelling: as tagged, '-' as ' ' ("Alpha-0.1.1" / "Alpha 0.1.1"), and with a leading v
    std::vector<std::pair<std::string, int>> pats;   // (text, tag index)
    for (int t = 0; t < (int)j.tags.size(); ++t) {
        std::string a = j.tags[t], b = j.tags[t];
        std::replace(b.begin(), b.end(), '-', ' ');
        for (const std::string &s : {a, b, "v" + a})
            if (s.size() >= 3) pats.push_back({s, t});
    }
    SDL_IOStream *io = SDL_IOFromFile(path.c_str(), "rb");
    if (!io) return {};
    const size_t chunk = 8 << 20, keep = 256;
    std::vector<char> buf(chunk + keep);
    size_t have = 0;
    int best = (int)j.tags.size();
    std::string direct;
    auto clean = [](char c) { return !SDL_isalnum((unsigned char)c) && c != '.' && c != '-'; };
    for (;;) {
        size_t got = SDL_ReadIO(io, buf.data() + have, chunk);
        size_t len = have + got;
        if (direct.empty()) direct = probe_direct_chunk(buf.data(), len, j.id, j.exe_stem, j.title);
        if (direct.empty()) {
            for (auto &p : pats) {
                if (p.second >= best) continue;
                const std::string &s = p.first;
                auto it = buf.begin(), end = buf.begin() + len;
                std::boyer_moore_horspool_searcher<std::string::const_iterator> bm(s.begin(), s.end());
                while ((it = std::search(it, end, bm)) != end) {
                    size_t pos = it - buf.begin();
                    bool l = pos == 0 || clean(buf[pos - 1]);
                    bool r = pos + s.size() < len && clean(buf[pos + s.size()]);
                    if (l && r) { best = p.second; break; }
                    ++it;
                }
            }
        }
        if (!direct.empty() || (best == 0 && j.id != "mastersword" && j.id != "mkw-thwomp" && j.id != "slowseparation" &&
                                j.id != "shadowracket" && j.id != "wonderbloom") ||
            got == 0)
            break;
        // keep the end of this chunk, so a tag or runtime header across the boundary is still found
        size_t k = std::min(keep, len);
        memmove(buf.data(), buf.data() + len - k, k);
        have = k;
    }
    SDL_CloseIO(io);
    if (!direct.empty()) {
        // normalize spelling to the matching GitHub tag if it corresponds to a published release
        std::string dk = tag_key(direct);
        for (const std::string &t : j.tags)
            if (tag_key(t) == dk) return t;
        return direct;
    }
    return best < (int)j.tags.size() ? j.tags[best] : std::string();
}

static std::string probe_file(const ProbeJob &j) {
    // Renderer-only updates (e.g. MasterSword 0.2.3 updating mastersword_vk.dll while mastersword.exe stays 0.2.2)
    // embed the newer version in the companion DLL; pick whichever binary carries the newer version.
    std::string v1 = probe_binary(j.exe, j);
    std::string v2 = j.dll.empty() ? std::string() : probe_binary(j.dll, j);
    if (!v2.empty() && (v1.empty() || version_newer(v2, v1))) return v2;
    return v1;
}

static int probe_worker(void *p) {
    auto *jobs = (std::vector<ProbeJob> *)p;
    for (const ProbeJob &j : *jobs) {
        std::string pref = probe_prefix(j.exe, j.dll);
        std::string key = pref.empty() ? std::string() : pref + j.tag_sig;
        std::string v;
        bool cached = false;
        {
            std::lock_guard<std::mutex> l(g_probe_lock);
            auto it = g_probe_cache.find(key);
            cached = !key.empty() && it != g_probe_cache.end();
            if (cached) v = it->second;
        }
        if (!cached && !key.empty()) {
            v = probe_file(j);
            std::lock_guard<std::mutex> l(g_probe_lock);
            // prune older entries for this same exe path
            std::string exe_base = j.exe + "|";
            for (auto it = g_probe_cache.begin(); it != g_probe_cache.end();) {
                if (it->first.rfind(exe_base, 0) == 0) it = g_probe_cache.erase(it);
                else ++it;
            }
            g_probe_cache[key] = v;
            probe_cache_save();   // after every program: a closed Sanctum keeps what it already read
        }
        sanctum_log("version: %s -> %s (%s, %d tags)", j.exe.c_str(), v.empty() ? "none" : v.c_str(),
                    cached ? "saved" : "read", (int)j.tags.size());
        if (!v.empty()) {
            std::lock_guard<std::mutex> l(g_probe_lock);
            g_probe_hits.push_back({j.port, j.dir, v});
        }
    }
    {
        std::lock_guard<std::mutex> l(g_probe_lock);
        probe_cache_save();
    }
    delete jobs;
    g_probe_running = false;
    return 0;
}

static std::string normalize_port_version(const Port &p, const std::string &v) {
    if (v.empty()) return {};
    std::string k = tag_key(v);
    for (const Release &r : p.releases)
        if (tag_key(r.tag) == k) return r.tag;
    if (p.known_tag && *p.known_tag && tag_key(p.known_tag) == k) return p.known_tag;
    return v;
}

static std::string pick_newer_version(const Port &p, const std::string &a, const std::string &b) {
    std::string na = normalize_port_version(p, a);
    std::string nb = normalize_port_version(p, b);
    if (na.empty()) return nb;
    if (nb.empty()) return na;
    if (tag_key(na) == tag_key(nb)) return na;
    return version_newer(nb, na) ? nb : na;
}

// the versions already read (versions.ini) combined with VERSION / <stem>_version.txt on disk:
// whichever is newer wins, so stale VERSION files never downgrade a newer exe, and launcher/runtime-only
// hotfix updates (which update VERSION without rebuilding the game exe) are never overwritten by the older exe
static void apply_known_versions(const Port &p, std::vector<Install> &v) {
    if (!g_probe_cache_loaded) probe_cache_load();
    std::string sig = port_tags_signature(p);
    for (Install &in : v) {
        std::string file_ver = read_version(in.dir, p.exe);
        std::string bin_ver;
        std::string dll = companion_dll(in.dir, p.id, p.exe);
        std::string pref = probe_prefix(in.dir + "\\" + p.exe, dll);
        if (!pref.empty()) {
            std::lock_guard<std::mutex> l(g_probe_lock);
            auto it = g_probe_cache.find(pref + sig);
            if (it != g_probe_cache.end() && !it->second.empty()) {
                bin_ver = it->second;
            } else {
                // before github_load_saved() runs, still reuse any v3 cache entry for this exact exe+dll signature
                for (const auto &e : g_probe_cache)
                    if (e.first.rfind(pref, 0) == 0 && !e.second.empty()) {
                        bin_ver = e.second;
                        break;
                    }
            }
        }
        std::string best = pick_newer_version(p, bin_ver, file_ver);
        if (!best.empty()) in.version = best;
        else if (!in.version.empty()) in.version = normalize_port_version(p, in.version);
    }
}

void version_probe_start() {
    if (g_probe_running) { g_probe_again = true; return; }
    if (!g_probe_cache_loaded) probe_cache_load();
    bool quick_changed = false;
    for (int i = 0; i < kBuiltinCount; ++i) {
        std::vector<std::string> prev;
        for (const Install &in : g_ports[i].all) prev.push_back(in.version);
        apply_known_versions(g_ports[i], g_ports[i].all);
        for (size_t k = 0; k < prev.size(); ++k)
            if (g_ports[i].all[k].version != prev[k]) quick_changed = true;
    }
    if (quick_changed) {
        for (int i = 0; i < kBuiltinCount; ++i) rank(g_ports[i].all);
        refilter();
        library_save();
    }
    // each port's copy in use first (the one its tile and Play use), then the other copies, then build folders:
    // a first read of every program on a developer's PC can take minutes, and the copies that matter come first
    // per port: its copy in use, the player's other copies, then build folders; then the ports taken in turn
    // (each one's 1st copy, each one's 2nd, ...), so one port with forty large copies holds nobody else up
    std::vector<std::vector<ProbeJob>> per((size_t)kBuiltinCount);
    for (int i = 0; i < kBuiltinCount; ++i) {
        const Port &p = g_ports[i];
        std::vector<std::string> tags;
        for (const Release &r : p.releases) tags.push_back(r.tag);
        if (tags.empty() && p.known_tag) tags.push_back(p.known_tag);
        std::string sig = port_tags_signature(p);
        std::string stem = exe_stem_of(p.exe);
        for (int pass = 0; pass < 3; ++pass)
            for (const Install &in : p.all) {
                bool first = !p.installs.empty() && lower(p.installs[0].dir) == lower(in.dir);
                if ((pass == 0) != first || (pass == 1 && in.dev) || (pass == 2 && !in.dev)) continue;
                std::string exe = in.dir + "\\" + p.exe;
                std::string dll = companion_dll(in.dir, p.id, p.exe);
                if (exists(exe)) per[i].push_back({i, in.dir, exe, dll, p.id, stem, p.title, sig, tags});
            }
    }
    auto *jobs = new std::vector<ProbeJob>;
    for (size_t k = 0;; ++k) {
        bool any = false;
        for (auto &v : per)
            if (k < v.size()) jobs->push_back(v[k]), any = true;
        if (!any) break;
    }
    sanctum_log("version: %d programs to read", (int)jobs->size());
    if (jobs->empty()) { delete jobs; return; }
    g_probe_running = true;
    SDL_Thread *t = SDL_CreateThread(probe_worker, "versions", jobs);
    if (t) SDL_DetachThread(t);
    else { delete jobs; g_probe_running = false; }
}

bool version_probe_poll() {
    std::vector<ProbeHit> hits;
    {
        std::lock_guard<std::mutex> l(g_probe_lock);
        hits.swap(g_probe_hits);
    }
    bool changed = false;
    for (const ProbeHit &h : hits) {
        const Port &p = g_ports[h.port];
        for (Install &in : g_ports[h.port].all) {
            if (lower(in.dir) != lower(h.dir)) continue;
            std::string best = pick_newer_version(p, h.version, read_version(in.dir, p.exe));
            if (!best.empty() && in.version != best) {
                sanctum_log("version: %s now %s (was %s)", in.dir.c_str(), best.c_str(), in.version.c_str());
                in.version = best, changed = true;
            }
        }
    }
    if (changed) {   // with their real versions the copies may rank differently (the newest becomes the main one)
        for (Port &p : g_ports) rank(p.all);
        refilter();
        library_save();
    }
    if (!g_probe_running && g_probe_again.exchange(false)) version_probe_start();
    return changed;
}

bool scan_poll() {
    if (!g_done.exchange(false)) return false;
    std::lock_guard<std::mutex> l(g_lock);
    bool changed = false;
    for (size_t i = 0; i < g_ports.size() && i < g_found.size(); ++i) {
        auto &now = g_ports[i].all;
        auto &got = g_found[i];
        // a copy without a VERSION file keeps the version Sanctum installed it as
        for (Install &g : got)
            if (g.version.empty())
                for (const Install &o : now)
                    if (lower(o.dir) == lower(g.dir)) g.version = o.version;
        // a copy known before and still there is kept, even where the search does not look
        for (const Install &o : now) {
            bool found = false;
            for (const Install &g : got) found |= lower(g.dir) == lower(o.dir);
            if (!found && port_files(g_ports[i], o.dir)) got.push_back(o);
        }
        // a version read from the copy's own program beats what the search found (a registry entry, a VERSION file)
        apply_known_versions(g_ports[i], got);
        bool same = now.size() == got.size();
        for (size_t k = 0; same && k < now.size(); ++k) same = now[k].dir == got[k].dir && now[k].version == got[k].version;
        if (!same) changed = true;
        now = got;
        rank(now);
    }
    refilter();
    library_save();
    return changed;
}

}  // namespace sanctum
