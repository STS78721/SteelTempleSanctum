#include "sync.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <map>
#include <mutex>
#include <string>

#include "app.h"
#include "config.h"
#include "github.h"
#include "install.h"
#include "launch.h"
#include "platform.h"
#include "sound.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace sanctum {

static std::string host_name() {
    static std::string nm;
    if (!nm.empty()) return nm;
#ifdef _WIN32
    wchar_t buf[128]{};
    DWORD n = (DWORD)(sizeof buf / sizeof buf[0]);
    if (GetComputerNameW(buf, &n) && n > 0) {
        int u = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, nullptr, 0, nullptr, nullptr);
        if (u > 0) {
            nm.resize((size_t)u);
            WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, nm.data(), u, nullptr, nullptr);
        }
    }
#endif
    if (nm.empty()) nm = "This-PC";
    return nm;
}

static std::string state_path() { return config_dir() + "sync_state.ini"; }

static std::map<std::string, long long> load_sync_state() {
    std::map<std::string, long long> out;
    size_t n = 0;
    char *d = (char *)SDL_LoadFile(state_path().c_str(), &n);
    if (!d) return out;
    std::string all(d, n);
    SDL_free(d);
    size_t at = 0;
    while (at < all.size()) {
        size_t end = all.find('\n', at);
        if (end == std::string::npos) end = all.size();
        std::string l = all.substr(at, end - at);
        at = end + 1;
        if (!l.empty() && l.back() == '\r') l.pop_back();
        size_t eq = l.find('=');
        if (eq == std::string::npos || l[0] == '#') continue;
        out[l.substr(0, eq)] = atoll(l.substr(eq + 1).c_str());
    }
    return out;
}

static void save_sync_mark(const std::string &port_id, long long ts) {
    auto mp = load_sync_state();
    mp[port_id] = ts;
    std::string s = "# SteelTemple Sanctum: last cloud save sync timestamp per port\n";
    for (const auto &e : mp) s += e.first + "=" + std::to_string(e.second) + "\n";
    std::string tmp = state_path() + ".tmp";
    if (SDL_SaveFile(tmp.c_str(), s.data(), s.size())) SDL_RenamePath(tmp.c_str(), state_path().c_str());
}

#ifdef _WIN32
static std::wstring to_w(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static void walk_newest_mtime(const std::wstring &dir, long long &best_unix, int &files, int depth) {
    if (depth <= 0) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            walk_newest_mtime(dir + L"\\" + fd.cFileName, best_unix, files, depth - 1);
        } else {
            ULARGE_INTEGER ui{};
            ui.LowPart = fd.ftLastWriteTime.dwLowDateTime;
            ui.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            // FILETIME (100ns since 1601) -> Unix seconds
            long long unix_s = (long long)(ui.QuadPart / 10000000ULL) - 11644473600LL;
            if (unix_s > best_unix) best_unix = unix_s;
            ++files;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
#endif

static long long folder_newest_mtime(const std::string &dir, int *files_out = nullptr) {
    long long best = 0;
    int files = 0;
#ifdef _WIN32
    if (!dir.empty()) {
        std::wstring w = to_w(dir);
        while (w.size() > 3 && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
        walk_newest_mtime(w, best, files, 16);
    }
#endif
    if (files_out) *files_out = files;
    return best;
}

static std::string cloud_dir_for(const std::string &port_id) {
    std::string base = cfg().save_sync_dir;
    while (!base.empty() && (base.back() == '\\' || base.back() == '/')) base.pop_back();
    return base.empty() ? std::string() : (base + "\\" + port_id);
}

struct CloudMeta {
    bool valid = false;
    long long ts = 0;
    std::string host;
    int files = 0;
};

static CloudMeta read_cloud_meta(const std::string &port_id) {
    CloudMeta m;
    std::string dir = cloud_dir_for(port_id);
    if (dir.empty()) return m;
    std::string zp = dir + "\\latest.zip";
    SDL_PathInfo zpi{};
    if (!SDL_GetPathInfo(zp.c_str(), &zpi) || zpi.type != SDL_PATHTYPE_FILE || zpi.size == 0) return m;
    m.valid = true;
    m.ts = (long long)(zpi.modify_time / 1000000000LL);

    size_t n = 0;
    if (char *d = (char *)SDL_LoadFile((dir + "\\meta.ini").c_str(), &n)) {
        std::string all(d, n);
        SDL_free(d);
        size_t at = 0;
        while (at < all.size()) {
            size_t end = all.find('\n', at);
            if (end == std::string::npos) end = all.size();
            std::string l = all.substr(at, end - at);
            at = end + 1;
            if (!l.empty() && l.back() == '\r') l.pop_back();
            size_t eq = l.find('=');
            if (eq == std::string::npos) continue;
            std::string k = l.substr(0, eq), v = l.substr(eq + 1);
            if (k == "timestamp") { long long t = atoll(v.c_str()); if (t > 0) m.ts = t; }
            else if (k == "host") m.host = v;
            else if (k == "files") m.files = atoi(v.c_str());
        }
    }
    return m;
}

static std::string age_short(long long unix_ts) {
    if (unix_ts <= 0) return "";
    long long d = (long long)time(nullptr) - unix_ts;
    if (d < 90) return "just now";
    if (d < 3600) return std::to_string(d / 60) + " min ago";
    if (d < 86400) return std::to_string(d / 3600) + " h ago";
    return std::to_string(d / 86400) + " d ago";
}

struct SyncCacheEntry {
    double checked = -100.0;
    SyncInfo info;
};
static std::mutex g_sync_lock;
static std::map<std::string, SyncCacheEntry> g_sync_cache;
static std::string g_sync_toast;
static bool g_sync_toast_ok = true;

void sync_invalidate(const char *port_id) {
    std::lock_guard<std::mutex> l(g_sync_lock);
    if (!port_id) g_sync_cache.clear();
    else g_sync_cache.erase(port_id);
}

SyncInfo sync_inspect(const Port &p, const Install &in, bool force) {
    if (demo_mode() || cfg().save_sync_dir.empty()) {
        return {SYNC_OFF, 0, 0, "", "Off (set folder in Storage)", 0};
    }
    double now = app().now;
    {
        std::lock_guard<std::mutex> l(g_sync_lock);
        auto it = g_sync_cache.find(p.id);
        if (!force && it != g_sync_cache.end() && now - it->second.checked < 5.0)
            return it->second.info;
    }

    SyncInfo si;
    std::string sdir = port_save_dir(p, in);
    int lfiles = 0;
    si.local_mtime = sdir.empty() ? 0 : folder_newest_mtime(sdir, &lfiles);
    CloudMeta cm = read_cloud_meta(p.id);
    si.cloud_mtime = cm.valid ? cm.ts : 0;
    si.cloud_host = cm.host;
    si.cloud_files = cm.files;

    auto marks = load_sync_state();
    long long last_sync = marks.count(p.id) ? marks[p.id] : 0;
    std::string me = host_name();

    if (lfiles == 0 && !cm.valid) {
        si.kind = SYNC_NO_SAVES;
        si.summary = "No save files yet";
    } else if (!cm.valid) {
        si.kind = SYNC_LOCAL_NEWER;
        si.summary = "Not uploaded yet  ·  Push";
    } else if (lfiles == 0) {
        si.kind = SYNC_CLOUD_NEWER;
        si.summary = "Cloud save (" + (cm.host.empty() ? "cloud" : cm.host) + ")  ·  Pull";
    } else {
        long long diff = si.cloud_mtime - si.local_mtime;
        bool other_host = !cm.host.empty() && cm.host != me;
        bool both_changed = last_sync > 0 && other_host && (si.local_mtime > last_sync + 15) && (si.cloud_mtime > last_sync + 15);
        if (both_changed) {
            si.kind = SYNC_CONFLICT;
            si.summary = "Conflict (" + cm.host + " " + age_short(cm.ts) + ")  ·  Pull safely";
        } else if (diff > 15) {
            si.kind = SYNC_CLOUD_NEWER;
            si.summary = "Cloud newer (" + (cm.host.empty() ? age_short(cm.ts) : cm.host) + ")  ·  Pull";
        } else if (diff < -15) {
            si.kind = SYNC_LOCAL_NEWER;
            si.summary = "Local newer  ·  Push to cloud";
        } else {
            si.kind = SYNC_UP_TO_DATE;
            si.summary = "Up to date" + (cm.ts > 0 ? ("  ·  " + age_short(cm.ts)) : "");
        }
    }

    {
        std::lock_guard<std::mutex> l(g_sync_lock);
        g_sync_cache[p.id] = {now, si};
    }
    return si;
}

bool sync_push_now(const Port &p, const Install &in, std::string &err_out) {
    err_out.clear();
    if (cfg().save_sync_dir.empty()) { err_out = "No cloud sync folder configured"; return false; }
    std::string sdir = port_save_dir(p, in);
    if (sdir.empty()) { err_out = "No local save folder found"; return false; }
    int lfiles = 0;
    long long mtime = folder_newest_mtime(sdir, &lfiles);
    if (lfiles <= 0) { err_out = "Local save folder is empty"; return false; }

    std::string root = cfg().save_sync_dir;
    while (!root.empty() && (root.back() == '\\' || root.back() == '/')) root.pop_back();
    std::string cdir = root + "\\" + p.id;
    SDL_CreateDirectory(root.c_str());
    SDL_CreateDirectory(cdir.c_str());

    std::string dst_zip = cdir + "\\latest.zip";
    int written = 0;
    if (!zip_folder(sdir, dst_zip, &written) || written <= 0) {
        err_out = "Could not write cloud save snapshot";
        return false;
    }
    SDL_PathInfo zpi{};
    SDL_GetPathInfo(dst_zip.c_str(), &zpi);
    long long ts = std::max(mtime, (long long)time(nullptr));
    std::string meta = "# SteelTemple Sanctum cloud save snapshot\n";
    meta += "port=" + std::string(p.id) + "\n";
    meta += "timestamp=" + std::to_string(ts) + "\n";
    meta += "host=" + host_name() + "\n";
    meta += "files=" + std::to_string(written) + "\n";
    meta += "bytes=" + std::to_string((long long)zpi.size) + "\n";
    std::string mtmp = cdir + "\\meta.ini.tmp";
    if (SDL_SaveFile(mtmp.c_str(), meta.data(), meta.size()))
        SDL_RenamePath(mtmp.c_str(), (cdir + "\\meta.ini").c_str());

    save_sync_mark(p.id, ts);
    sync_invalidate(p.id);
    sanctum_log("sync: pushed %s (%d files, %lld bytes) -> %s", p.id, written, (long long)zpi.size, dst_zip.c_str());
    return true;
}

bool sync_pull_now(const Port &p, const Install &in, std::string &err_out) {
    err_out.clear();
    if (cfg().save_sync_dir.empty()) { err_out = "No cloud sync folder configured"; return false; }
    std::string cdir = cloud_dir_for(p.id);
    std::string src_zip = cdir + "\\latest.zip";
    if (!SDL_GetPathInfo(src_zip.c_str(), nullptr)) { err_out = "No cloud snapshot found"; return false; }

    std::string sdir = port_save_dir(p, in);
    if (sdir.empty()) {
        // Default save dir creation when pulling onto a fresh handheld install
        sdir = (!strcmp(p.id, "mastersword")) ? (in.dir + "\\user\\save") : (in.dir + "\\saves");
        SDL_CreateDirectory(sdir.c_str());
    }

    // Conflict-safe: ALWAYS archive existing local saves before overwriting
    int lfiles = 0;
    if (folder_newest_mtime(sdir, &lfiles) > 0 && lfiles > 0) {
        std::string bdir = config_dir() + "backups\\" + p.id;
        SDL_CreateDirectory((config_dir() + "backups").c_str());
        SDL_CreateDirectory(bdir.c_str());
        time_t now = time(nullptr);
        struct tm lt{};
#ifdef _WIN32
        localtime_s(&lt, &now);
#else
        localtime_r(&lt, &now);
#endif
        char stamp[32];
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &lt);
        zip_folder(sdir, bdir + "\\pre-sync-" + stamp + ".zip");
    }

    std::string stage = local_dir() + "sync-stage";
    remove_tree(stage);
    SDL_CreateDirectory(stage.c_str());
    std::atomic<bool> cancel{false};
    std::string err = unzip(src_zip, stage, cancel);
    if (err.empty()) err = overlay(stage, sdir);
    remove_tree(stage);
    if (!err.empty()) {
        err_out = err;
        return false;
    }

    CloudMeta cm = read_cloud_meta(p.id);
    long long mark = std::max(cm.ts, folder_newest_mtime(sdir));
    save_sync_mark(p.id, mark > 0 ? mark : (long long)time(nullptr));
    sync_invalidate(p.id);
    sanctum_log("sync: pulled %s from %s -> %s", p.id, src_zip.c_str(), sdir.c_str());
    return true;
}

int sync_push_all(int *skipped_cloud_newer) {
    int pushed = 0, skipped = 0;
    for (const Port &p : ports()) {
        const Install *in = p.best();
        if (!in) continue;
        SyncInfo si = sync_inspect(p, *in, true);
        if (si.kind == SYNC_CLOUD_NEWER || si.kind == SYNC_CONFLICT) {
            ++skipped;
            continue;
        }
        if (si.kind == SYNC_LOCAL_NEWER) {
            std::string err;
            if (sync_push_now(p, *in, err)) ++pushed;
        }
    }
    if (skipped_cloud_newer) *skipped_cloud_newer = skipped;
    return pushed;
}

struct SyncJob {
    int port_idx;
    Install in;
};

static int sync_worker(void *vp) {
    auto *j = (SyncJob *)vp;
    if (j->port_idx >= 0 && j->port_idx < (int)ports().size()) {
        const Port &p = ports()[j->port_idx];
        std::string err;
        if (sync_push_now(p, j->in, err)) {
            std::lock_guard<std::mutex> l(g_sync_lock);
            g_sync_toast = "Synced " + std::string(p.name) + " saves to cloud folder";
            g_sync_toast_ok = true;
        }
    }
    delete j;
    return 0;
}

void sync_push_async(int port_idx, const Install &in) {
    if (demo_mode() || cfg().save_sync_dir.empty() || !cfg().save_sync_auto) return;
    if (port_idx < 0 || port_idx >= (int)ports().size()) return;
    auto *j = new SyncJob{port_idx, in};
    SDL_Thread *t = SDL_CreateThread(sync_worker, "savesync", j);
    if (t) SDL_DetachThread(t);
    else delete j;
}

void sync_poll() {
    std::string msg;
    bool ok = true;
    {
        std::lock_guard<std::mutex> l(g_sync_lock);
        if (!g_sync_toast.empty()) { msg = std::move(g_sync_toast); g_sync_toast.clear(); ok = g_sync_toast_ok; }
    }
    if (!msg.empty()) {
        sound_play(ok ? SFX_NOTIFY : SFX_ERROR);
        toast(msg, 3.0);
    }
}

}  // namespace sanctum
