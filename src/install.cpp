#include "install.h"

#include <atomic>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>

#include "app.h"
#include "catalog.h"
#include "config.h"
#include "github.h"
#include "net.h"
#include "sound.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace sanctum {

struct Job {
    JobView v;
    Release rel;
    Asset asset;
    std::atomic<bool> cancel{false};
};

static std::mutex g_lock;
static std::vector<std::shared_ptr<Job>> g_jobs;   // every job of this session, in order
static std::atomic<bool> g_worker{false};
static int g_next_id = 1;
struct Finished {
    int port, kind;
    std::string dir, version;
};
static std::vector<Finished> g_finished;
static std::atomic<bool> g_wizard_open{false};
static bool g_stepped_aside = false;

std::string library_dir() {
    if (const char *t = getenv("SANCTUM_LIBRARY")) return t;   // tests: a throwaway folder
    if (!cfg().library_dir.empty()) return cfg().library_dir;
    const char *home = SDL_GetUserFolder(SDL_FOLDER_HOME);
    return std::string(home ? home : "C:\\") + "Games\\SteelTemple";
}

// ---- file helpers (UTF-8 paths)
static std::wstring W(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static bool is_dir(const std::string &p) {
    SDL_PathInfo i;
    return SDL_GetPathInfo(p.c_str(), &i) && i.type == SDL_PATHTYPE_DIRECTORY;
}

static std::vector<std::string> list(const std::string &dir) {
    std::vector<std::string> out;
    int n = 0;
    char **f = SDL_GlobDirectory(dir.c_str(), "*", 0, &n);
    for (int i = 0; f && i < n; ++i) out.push_back(f[i]);
    SDL_free(f);
    return out;
}

void remove_tree(const std::string &p) {
    if (is_dir(p))
        for (auto &n : list(p)) remove_tree(p + "\\" + n);
    SetFileAttributesW(W(p).c_str(), FILE_ATTRIBUTE_NORMAL);
    SDL_RemovePath(p.c_str());
}

// the folder under root holding the port (the zips keep it one folder down, e.g. MKW-THWOMP/)
static std::string find_root(const std::string &root, const Port &p, int depth) {
    if (port_files(p, root)) return root;
    if (depth <= 0) return {};
    for (auto &n : list(root))
        if (is_dir(root + "\\" + n)) {
            std::string r = find_root(root + "\\" + n, p, depth - 1);
            if (!r.empty()) return r;
        }
    return {};
}

// a zipped Setup Wizard (WonderBloom's "...-Setup-Wizard.zip"): the setup program inside it
static std::string find_setup_exe(const std::string &root, int depth) {
    for (auto &n : list(root)) {
        std::string l;
        for (char c : n) l += (char)SDL_tolower((unsigned char)c);
        if (l.size() > 4 && !l.compare(l.size() - 4, 4, ".exe") &&
            (l.find("setup") != std::string::npos || l.find("installer") != std::string::npos))
            return root + "\\" + n;
    }
    if (depth > 0)
        for (auto &n : list(root))
            if (is_dir(root + "\\" + n)) {
                std::string r = find_setup_exe(root + "\\" + n, depth - 1);
                if (!r.empty()) return r;
            }
    return {};
}

static long long free_bytes(const std::string &dir) {
    std::string d = dir;
    while (!d.empty() && !is_dir(d)) d = d.substr(0, d.find_last_of('\\') == std::string::npos ? 0 : d.find_last_of('\\'));
    ULARGE_INTEGER avail;
    if (d.empty() || !GetDiskFreeSpaceExW(W(d).c_str(), &avail, nullptr, nullptr)) return -1;
    return (long long)avail.QuadPart;
}

static void make_dirs(const std::string &p) {
    for (size_t i = 3; i <= p.size(); ++i)
        if (i == p.size() || p[i] == '\\') SDL_CreateDirectory(p.substr(0, i).c_str());
}

// Windows' own tar (bsdtar) unpacks zips; returns "" or why
std::string unzip(const std::string &zip, const std::string &to, const std::atomic<bool> &cancel) {
    const char *sys = getenv("SystemRoot");
    std::string tar = std::string(sys ? sys : "C:\\Windows") + "\\System32\\tar.exe";
    const char *args[] = {tar.c_str(), "-xf", zip.c_str(), "-C", to.c_str(), nullptr};
    SDL_PropertiesID pr = SDL_CreateProperties();
    SDL_SetPointerProperty(pr, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(pr, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_Process *p = SDL_CreateProcessWithProperties(pr);
    SDL_DestroyProperties(pr);
    if (!p) return std::string("cannot run tar: ") + SDL_GetError();
    int code = 0;
    while (!SDL_WaitProcess(p, false, &code)) {
        if (cancel) { SDL_KillProcess(p, true); SDL_WaitProcess(p, true, &code); SDL_DestroyProcess(p); return "cancelled"; }
        SDL_Delay(50);
    }
    SDL_DestroyProcess(p);
    return code == 0 ? std::string() : "the package could not be unpacked (tar " + std::to_string(code) + ")";
}

// Lays the files of src over dst. Each replaced file is first renamed <name>.old; on any failure everything done is
// put back. On success the .old files are removed.
std::string overlay(const std::string &src, const std::string &dst) {
    struct Step { std::string to; bool had_old; };
    std::vector<Step> done;
    std::string why;
    std::vector<std::pair<std::string, std::string>> files;   // (from, to)
    std::vector<std::string> stack{""};
    while (!stack.empty()) {
        std::string rel = stack.back();
        stack.pop_back();
        std::string from = rel.empty() ? src : src + "\\" + rel;
        for (auto &n : list(from)) {
            std::string r = rel.empty() ? n : rel + "\\" + n;
            if (is_dir(src + "\\" + r)) stack.push_back(r);
            else files.push_back({src + "\\" + r, dst + "\\" + r});
        }
    }
    for (auto &f : files) {
        std::string dir = f.second.substr(0, f.second.find_last_of('\\'));
        make_dirs(dir);
        bool had = SDL_GetPathInfo(f.second.c_str(), nullptr);
        if (had && !MoveFileExW(W(f.second).c_str(), W(f.second + ".old").c_str(), MOVEFILE_REPLACE_EXISTING)) {
            why = "a file is in use: " + f.second.substr(dst.size() + 1);
            break;
        }
        if (!MoveFileExW(W(f.first).c_str(), W(f.second).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            if (had) MoveFileExW(W(f.second + ".old").c_str(), W(f.second).c_str(), MOVEFILE_REPLACE_EXISTING);
            why = "cannot write " + f.second.substr(dst.size() + 1);
            break;
        }
        done.push_back({f.second, had});
    }
    if (!why.empty()) {   // put everything back as it was
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            if (it->had_old) MoveFileExW(W(it->to + ".old").c_str(), W(it->to).c_str(), MOVEFILE_REPLACE_EXISTING);
            else DeleteFileW(W(it->to).c_str());
        }
        return why;
    }
    for (auto &d : done)
        if (d.had_old) DeleteFileW(W(d.to + ".old").c_str());
    return {};
}

static void set_phase(Job &j, int ph, const std::string &err = {}) {
    std::lock_guard<std::mutex> l(g_lock);
    j.v.phase = ph;
    if (ph < PH_DONE) j.v.reached = ph;
    if (!err.empty()) j.v.error = err;
}

static void run(Job &j) {
    const Port &port = ports()[j.v.port];
    std::string dl_dir = local_dir() + "downloads\\";
    SDL_CreateDirectory(dl_dir.c_str());
    std::string file = dl_dir + j.asset.name;

    // ---- space: the package, and (for zips) about as much again unpacked
    long long need = j.asset.size * (j.v.kind == JOB_WIZARD ? 1 : 3);
    long long have = free_bytes(j.v.kind == JOB_WIZARD ? dl_dir : library_dir());
    if (have >= 0 && have < need) { set_phase(j, PH_FAILED, "not enough free space (" + mb(need) + " needed)"); return; }

    // ---- download
    set_phase(j, PH_DOWNLOAD);
    Uint64 t0 = SDL_GetTicksNS();
    long long start_bytes = -1;
    std::string err = http_download(j.asset.url, file, j.asset.size, [&](long long d, long long t) {
        std::lock_guard<std::mutex> l(g_lock);
        if (start_bytes < 0) start_bytes = d;
        j.v.done = d, j.v.total = t ? t : j.asset.size;
        double secs = (SDL_GetTicksNS() - t0) / 1e9;
        if (secs > 0.5) j.v.speed = (d - start_bytes) / secs;
    }, j.cancel);
    if (j.cancel) { set_phase(j, PH_CANCELLED); return; }   // the partial file stays: a new try resumes it
    if (!err.empty()) { set_phase(j, PH_FAILED, err); return; }

    // ---- verify
    set_phase(j, PH_VERIFY);
    if (!j.asset.digest.empty()) {
        std::string got = sha256_file(file, j.cancel);
        if (j.cancel) { set_phase(j, PH_CANCELLED); return; }
        if (got != j.asset.digest) {
            SDL_RemovePath(file.c_str());   // damaged: start over next time
            set_phase(j, PH_FAILED, "the download did not match GitHub's checksum; try again");
            return;
        }
    }

    // ---- Setup Wizard: the port's own setup does the rest
    if (j.v.kind == JOB_WIZARD) {
        std::string setup = file, unpacked;
        if (setup.size() > 4 && !SDL_strcasecmp(setup.c_str() + setup.size() - 4, ".zip")) {   // a zipped setup
            unpacked = dl_dir + "setup-" + port.id + "-" + std::to_string(j.v.id);
            remove_tree(unpacked);
            SDL_CreateDirectory(unpacked.c_str());
            err = unzip(file, unpacked, j.cancel);
            setup = err.empty() ? find_setup_exe(unpacked, 2) : "";
            if (setup.empty()) {
                remove_tree(unpacked);
                set_phase(j, j.cancel ? PH_CANCELLED : PH_FAILED, err.empty() ? "no setup program in the download" : err);
                return;
            }
        }
        set_phase(j, PH_WIZARD);
        g_wizard_open = true;
        const char *args[] = {setup.c_str(), nullptr};
        SDL_Process *p = SDL_CreateProcess(args, false);
        if (!p) {
            g_wizard_open = false;
            if (!unpacked.empty()) remove_tree(unpacked);
            set_phase(j, PH_FAILED, std::string("cannot start the setup: ") + SDL_GetError());
            return;
        }
        int code = 0;
        SDL_WaitProcess(p, true, &code);
        SDL_DestroyProcess(p);
        g_wizard_open = false;
        if (!unpacked.empty()) remove_tree(unpacked);
        std::lock_guard<std::mutex> l(g_lock);
        g_finished.push_back({j.v.port, JOB_WIZARD, {}, j.rel.tag});
        j.v.phase = PH_DONE;
        return;
    }

    // ---- unpack beside the library (same drive: the final move is instant)
    set_phase(j, PH_INSTALL);
    std::string lib = library_dir();
    make_dirs(lib);
    std::string stage = lib + "\\.sanctum-" + port.id + "-" + std::to_string(j.v.id);
    remove_tree(stage);
    SDL_CreateDirectory(stage.c_str());
    err = unzip(file, stage, j.cancel);
    if (!err.empty()) { remove_tree(stage); set_phase(j, j.cancel ? PH_CANCELLED : PH_FAILED, err); return; }
    std::string root = find_root(stage, port, 3);
    if (root.empty()) root = stage;   // an Update.zip may carry only what changed, without the program

    std::string dest;
    if (j.v.kind == JOB_UPDATE) {
        dest = j.v.dest;
        err = overlay(root, dest);
    } else {
        dest = lib + "\\" + port.name;
        for (int n = 2; SDL_GetPathInfo(dest.c_str(), nullptr); ++n) dest = lib + "\\" + port.name + " (" + std::to_string(n) + ")";
        if (find_root(stage, port, 3).empty()) err = "this package does not look like " + std::string(port.name);
        else if (!MoveFileExW(W(root).c_str(), W(dest).c_str(), 0)) err = overlay(root, dest);   // a move across drives: copy
    }
    remove_tree(stage);
    if (!err.empty()) { set_phase(j, PH_FAILED, err); return; }
    SDL_RemovePath(file.c_str());   // installed: the package is not needed any more

    std::lock_guard<std::mutex> l(g_lock);
    g_finished.push_back({j.v.port, j.v.kind, dest, j.rel.tag});
    j.v.dest = dest;
    j.v.phase = PH_DONE;
}

static int worker(void *) {
    for (;;) {
        std::shared_ptr<Job> next;
        {
            std::lock_guard<std::mutex> l(g_lock);
            for (auto &j : g_jobs)
                if (j->v.phase == PH_QUEUED) { next = j; break; }
            if (!next) { g_worker = false; return 0; }
        }
        run(*next);
    }
}

int install_enqueue(int port, const Release &r, const Asset &a, JobKind kind, const std::string &dest) {
    auto j = std::make_shared<Job>();
    j->rel = r, j->asset = a;
    {
        std::lock_guard<std::mutex> l(g_lock);
        j->v.id = g_next_id++;
        j->v.port = port, j->v.kind = kind, j->v.phase = PH_QUEUED;
        j->v.tag = r.tag, j->v.file = a.name, j->v.dest = dest, j->v.total = a.size;
        g_jobs.push_back(j);
    }
    if (!g_worker.exchange(true)) {
        SDL_Thread *t = SDL_CreateThread(worker, "install", nullptr);
        if (t) SDL_DetachThread(t);
        else g_worker = false;
    }
    return j->v.id;
}

std::vector<JobView> install_jobs() {
    std::lock_guard<std::mutex> l(g_lock);
    std::vector<JobView> v;
    for (auto &j : g_jobs) v.push_back(j->v);
    return v;
}

void install_cancel(int id) {
    std::lock_guard<std::mutex> l(g_lock);
    for (auto &j : g_jobs)
        if (j->v.id == id) {
            if (j->v.phase == PH_QUEUED) j->v.phase = PH_CANCELLED;
            else if (j->v.phase < PH_WIZARD) j->cancel = true;
        }
}

void install_clear_finished() {
    std::lock_guard<std::mutex> l(g_lock);
    std::vector<std::shared_ptr<Job>> keep;
    for (auto &j : g_jobs)
        if (j->v.phase < PH_DONE) keep.push_back(j);
    g_jobs.swap(keep);
}

bool install_busy() {
    std::lock_guard<std::mutex> l(g_lock);
    for (auto &j : g_jobs)
        if (j->v.phase < PH_DONE) return true;
    return false;
}

bool install_queued(int port) {
    std::lock_guard<std::mutex> l(g_lock);
    for (auto &j : g_jobs)
        if (j->v.port == port && j->v.phase < PH_DONE) return true;
    return false;
}

static void registry_version(const char *key, const std::string &tag) {
    if (!key) return;
    std::wstring sub = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + W(key);
    std::wstring v = W(tag);
    RegSetKeyValueW(HKEY_CURRENT_USER, sub.c_str(), L"DisplayVersion", REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

void install_poll(SDL_Window *w) {
    // a Setup Wizard is open: Sanctum steps aside so the setup is in front, and comes back after
    if (g_wizard_open && !g_stepped_aside) { SDL_MinimizeWindow(w); g_stepped_aside = true; }
    if (!g_wizard_open && g_stepped_aside) { SDL_RestoreWindow(w); SDL_RaiseWindow(w); g_stepped_aside = false; }
    std::vector<Finished> done;
    {
        std::lock_guard<std::mutex> l(g_lock);
        done.swap(g_finished);
    }
    if (!done.empty()) sound_play(SFX_NOTIFY);
    for (auto &f : done) {
        Port &p = ports()[f.port];
        if (f.kind == JOB_WIZARD) {
            scan_start();   // the setup chose the folder: find it
            toast(std::string(p.name) + ": setup finished, looking for it on this PC", 3.5);
            continue;
        }
        if (f.kind == JOB_UPDATE)
            for (auto &in : p.all)
                if (in.dir == f.dir && in.registered) registry_version(p.uninstall_key, f.version);
        catalog_installed(f.port, f.dir, f.version);
        bool needs_relink = false;
        if (f.kind == JOB_UPDATE && p.launcher && p.exe) {
            std::string stem = p.exe;
            if (size_t dot = stem.rfind('.'); dot != std::string::npos) stem.resize(dot);
            SDL_PathInfo rt{}, ex{};
            if (SDL_GetPathInfo((f.dir + "\\runtime\\lib" + stem + "rt.a").c_str(), &rt) &&
                SDL_GetPathInfo((f.dir + "\\" + p.exe).c_str(), &ex) && rt.modify_time > ex.modify_time)
                needs_relink = true;
        }
        if (needs_relink)
            toast(std::string(p.name) + " updated to " + f.version + " — open Port's own launcher once to relink " + p.exe, 5.0);
        else
            toast(std::string(p.name) + (f.kind == JOB_UPDATE ? " updated to " : " installed: ") + f.version, 3.5);
    }
}

}  // namespace sanctum
