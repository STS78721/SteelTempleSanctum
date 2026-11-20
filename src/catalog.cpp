#include "catalog.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include "config.h"
#include "install.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace sanctum {

static std::vector<Port> g_ports = {
    {"mkw-thwomp", "Mario Kart World", "MKW-THWOMP", "STS78721/MKW-THWOMP", "0.2.2c", "204 MB", "mariokartworld", "MKW",
     "mkw.exe", "mkwgame.id", nullptr, "MKW-THWOMP", {IM_COL32(232, 36, 48, 255), IM_COL32(90, 12, 18, 255)}},
    {"mastersword", "The Legend of Zelda: Ocarina of Time", "MasterSword", "STS78721/OOT-MASTERSWORD", "0.2.2", "176 MB",
     "ocarinaoftime", "OoT", "mastersword.exe", nullptr, "MasterSwordLauncher.exe", nullptr,
     {IM_COL32(47, 158, 90, 255), IM_COL32(122, 90, 18, 255)}},
    {"totk", "The Legend of Zelda: Tears of the Kingdom", "TotK SteelTempleBuild", "STS78721/totk-pc-recomp-SteelTempleBuild",
     "Beta-6.3", "97 MB", "tearsofthekingdom", "TotK", "totk.exe", nullptr, "TotKLauncher.exe", nullptr,
     {IM_COL32(31, 143, 134, 255), IM_COL32(59, 42, 16, 255)}},
    {"slowseparation", "Fast Fusion", "SlowSeparation", "STS78721/SlowSeparation", "Alpha-0.1.1", "158 MB", "fastfusion",
     "FF", "ff.exe", nullptr, "FFLauncher.exe", nullptr, {IM_COL32(255, 46, 136, 255), IM_COL32(15, 13, 34, 255)}},
    {"shadowracket", "Mario Tennis Fever", "ShadowRacket", "STS78721/MTF-ShadowRacket", "Alpha-0.1.3b", "156 MB",
     "mariotennisfever", "MTF", "mtf.exe", nullptr, "MTFLauncher.exe", nullptr,
     {IM_COL32(204, 242, 42, 255), IM_COL32(18, 36, 24, 255)}},
    {"dkb-bananas", "Donkey Kong Bananza", "dkb-BANANAS", "STS78721/dkb-BANANAS", "Alpha-1g", "321 MB",
     "donkeykongbananza", "DKB", "dkb.exe", nullptr, "DKBLauncher.exe", nullptr,
     {IM_COL32(242, 181, 28, 255), IM_COL32(90, 50, 16, 255)}},
};

std::vector<Port> &ports() { return g_ports; }

static void library_save();
static bool exists(const std::string &p);
static void refilter();

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
    if (b >= 1000LL * 1024 * 1024) SDL_snprintf(s, sizeof s, "%.1f GB", b / (1024.0 * 1024 * 1024));
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
    if (b->version.empty()) return PORT_INSTALLED;
    std::string have = tag_key(b->version);
    if (have == tag_key(latest_tag())) return PORT_INSTALLED;
    // only when the installed version is an older release of this port: a newer local build is no "update"
    for (const Release &r : releases)
        if (tag_key(r.tag) == have) return PORT_UPDATE;
    return releases.empty() && have != tag_key(known_tag) ? PORT_UPDATE : PORT_INSTALLED;
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

// installs = the copies a player should see: every copy when build folders are shown, else the others
static void refilter() {
    for (Port &p : g_ports) {
        p.installs.clear();
        p.hidden = 0;
        for (const Install &in : p.all)
            if (cfg().show_dev || !in.dev) p.installs.push_back(in);
            else p.hidden++;
    }
}

void catalog_refilter() { refilter(); }

// ---- small file helpers
static bool exists(const std::string &p) { return SDL_GetPathInfo(p.c_str(), nullptr); }

static std::string read_version(const std::string &dir) {
    for (const char *f : {"VERSION", "launcher\\version.txt", "version.txt"}) {
        size_t n = 0;
        char *d = (char *)SDL_LoadFile((dir + "\\" + f).c_str(), &n);
        if (!d) continue;
        std::string s(d, std::min<size_t>(n, 64));
        SDL_free(d);
        s = s.substr(0, s.find_first_of("\r\n"));
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
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
    for (size_t i = 0; i < g_ports.size(); ++i) {
        const Port &p = g_ports[i];
        // the game program (built or shipped), or the port's own launcher alone: a fresh install of a port that
        // builds its game on this PC at the first start has only the launcher
        bool game = has(p.exe) && (!p.also || has(p.also));
        if (!game && !(p.launcher && has(p.launcher))) continue;
        Install in;
        in.dir = dir;
        in.version = read_version(dir);
        in.launcher = p.launcher && has(p.launcher);
        add_found(out, i, std::move(in));
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
    std::vector<std::vector<Install>> out(g_ports.size());
#ifdef _WIN32
    // 1. the Setup Wizards' entries in Windows' apps list
    for (size_t i = 0; i < g_ports.size(); ++i) {
        if (!g_ports[i].uninstall_key) continue;
        std::wstring sub = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";
        for (const char *c = g_ports[i].uninstall_key; *c; ++c) sub += (wchar_t)*c;
        std::string dir = reg_string(HKEY_CURRENT_USER, sub, L"InstallLocation");
        while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
        if (dir.empty() || !port_files(g_ports[i], dir)) continue;
        Install in;
        in.dir = dir;
        in.version = reg_string(HKEY_CURRENT_USER, sub, L"DisplayVersion");
        if (in.version.empty()) in.version = read_version(dir);
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
    // best first: a registered install, a player's copy before a build folder, one with its launcher, the newest
    // version string, then the shortest path
    for (auto &v : out) {
        for (Install &in : v) in.dev = !in.registered && is_dev_path(in.dir);
        std::sort(v.begin(), v.end(), [](const Install &a, const Install &b) {
            if (a.registered != b.registered) return a.registered;
            if (a.dev != b.dev) return !a.dev;
            if (a.launcher != b.launcher) return a.launcher;
            if (a.version != b.version) return a.version > b.version;
            return a.dir.size() < b.dir.size();
        });
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
    std::string s = "# SteelTemple Sanctum: ports found on this PC (rebuilt by every search)\n";
    for (const Port &p : g_ports)
        for (const Install &in : p.all)
            s += std::string("port=") + p.id + "|" + in.dir + "|" + in.version + "|" + (in.launcher ? "1" : "0") + "|" +
                 (in.registered ? "1" : "0") + "\n";
    std::string tmp = library_path() + ".tmp";
    if (SDL_SaveFile(tmp.c_str(), s.data(), s.size())) SDL_RenamePath(tmp.c_str(), library_path().c_str());
}

void library_load() {
    size_t n = 0;
    char *d = (char *)SDL_LoadFile(library_path().c_str(), &n);
    if (!d) return;
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
        for (Port &p : g_ports)
            if (f[0] == p.id && port_files(p, f[1])) {
                Install in{f[1], f[2], f[3] == "1", f[4] == "1"};
                in.dev = !in.registered && is_dev_path(in.dir);
                p.all.push_back(in);
            }
    }
    refilter();
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
        bool same = now.size() == got.size();
        for (size_t k = 0; same && k < now.size(); ++k) same = now[k].dir == got[k].dir && now[k].version == got[k].version;
        if (!same) changed = true;
        now = got;
    }
    refilter();
    library_save();
    return changed;
}

}  // namespace sanctum
