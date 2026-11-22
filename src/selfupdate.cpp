#include "selfupdate.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <vector>

#include "app.h"
#include "config.h"
#include "github.h"
#include "install.h"
#include "json.h"
#include "net.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace sanctum {

static const char *kRepo = "STS78721/SteelTempleSanctum";
static const char *kExe = "SteelTemple-Sanctum.exe";

static std::mutex g_lock;
static std::atomic<int> g_state{UPD_IDLE};
static std::atomic<bool> g_busy{false}, g_announce{false}, g_cancel{false};
static std::atomic<long long> g_done{0}, g_total{0};
static std::string g_tag, g_error, g_staged;   // g_staged: the unpacked folder holding the new exe
static std::string g_url, g_digest;
static long long g_size = 0;

const char *sanctum_version() {
    static const char *v = getenv("SANCTUM_SELF_VERSION") ? getenv("SANCTUM_SELF_VERSION") : SANCTUM_VERSION;
    return v;
}

// "v1.2.3" / "1.2.3" / "1.0.4b" / "Sanctum 1.2" -> ({1, 2, 3}, "") or ({1, 0, 4}, "b")
struct ParsedVer {
    std::vector<int> nums;
    std::string suffix;   // trailing hotfix letters after the final digit (e.g. "b" in "1.0.4b")
};
static ParsedVer parse_ver(const std::string &s) {
    ParsedVer out;
    size_t last_digit = std::string::npos;
    for (size_t i = 0; i < s.size();) {
        if (isdigit((unsigned char)s[i])) {
            int n = 0;
            while (i < s.size() && isdigit((unsigned char)s[i])) n = n * 10 + (s[i++] - '0');
            out.nums.push_back(n);
            last_digit = i;
        } else ++i;
    }
    if (last_digit != std::string::npos) {
        for (size_t i = last_digit; i < s.size() && isalpha((unsigned char)s[i]); ++i)
            out.suffix += (char)SDL_tolower((unsigned char)s[i]);
    }
    return out;
}
static bool newer(const std::string &a, const std::string &b) {   // a is a later version than b
    auto x = parse_ver(a), y = parse_ver(b);
    for (size_t i = 0; i < std::max(x.nums.size(), y.nums.size()); ++i) {
        int p = i < x.nums.size() ? x.nums[i] : 0, q = i < y.nums.size() ? y.nums[i] : 0;
        if (p != q) return p > q;
    }
    return x.suffix > y.suffix;
}

static std::string load(const std::string &path) {
    size_t n = 0;
    char *d = (char *)SDL_LoadFile(path.c_str(), &n);
    std::string s = d ? std::string(d, n) : std::string();
    SDL_free(d);
    return s;
}

static void fail(const std::string &why) {
    std::lock_guard<std::mutex> l(g_lock);
    g_error = why;
    g_state = UPD_FAILED;
}

static std::string find_exe(const std::string &dir, int depth) {
    if (SDL_GetPathInfo((dir + "\\" + kExe).c_str(), nullptr)) return dir;
    if (depth <= 0) return {};
    int n = 0;
    char **f = SDL_GlobDirectory(dir.c_str(), "*", 0, &n);
    std::string found;
    for (int i = 0; f && i < n && found.empty(); ++i) {
        SDL_PathInfo pi;
        std::string sub = dir + "\\" + f[i];
        if (SDL_GetPathInfo(sub.c_str(), &pi) && pi.type == SDL_PATHTYPE_DIRECTORY) found = find_exe(sub, depth - 1);
    }
    SDL_free(f);
    return found;
}

static void download() {
    g_state = UPD_DOWNLOADING;
    std::string tag, url, digest;
    long long size;
    {
        std::lock_guard<std::mutex> l(g_lock);
        tag = g_tag, url = g_url, digest = g_digest, size = g_size;
    }
    std::string dl = local_dir() + "downloads\\";
    SDL_CreateDirectory(dl.c_str());
    std::string zip = dl + "SteelTemple-Sanctum-" + tag + ".zip";
    g_done = 0, g_total = size;
    std::string err = http_download(url, zip, size, [](long long d, long long t) { g_done = d, g_total = t; }, g_cancel);
    if (!err.empty()) { fail(err); return; }
    if (!digest.empty() && sha256_file(zip, g_cancel) != digest) {
        SDL_RemovePath(zip.c_str());
        fail("the download did not match GitHub's checksum");
        return;
    }
    std::string stage = local_dir() + "sanctum-update";
    remove_tree(stage);
    SDL_CreateDirectory(stage.c_str());
    err = unzip(zip, stage, g_cancel);
    if (!err.empty()) { fail(err); return; }
    std::string root = find_exe(stage, 2);
    if (root.empty()) { fail("the release has no " + std::string(kExe)); return; }
    SDL_RemovePath(zip.c_str());
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_staged = root;
    }
    g_state = UPD_READY;
    g_announce = true;
}

static int worker(void *p) {
    bool force = p != nullptr, only_download = (intptr_t)p == 2;
    if (only_download) { download(); g_busy = false; return 0; }
    g_state = UPD_CHECKING;
    std::string dir = local_dir() + "releases\\";
    SDL_CreateDirectory(dir.c_str());
    std::string base = dir + "_sanctum";
    long long last = atoll(load(base + ".last").c_str()), now = (long long)time(nullptr);
    std::string body;
    bool saved = SDL_GetPathInfo((base + ".json").c_str(), nullptr);
    if (!force && saved && now - last < 15 * 60) {
        body = load(base + ".json");   // asked a moment ago: the saved answer
    } else {
        HttpResult r = http_get(std::string("https://api.github.com/repos/") + kRepo + "/releases?per_page=10",
                                saved ? load(base + ".etag") : std::string());
        std::string stamp = std::to_string(now);
        SDL_SaveFile((base + ".last").c_str(), stamp.data(), stamp.size());
        if (r.status == 304) body = load(base + ".json");
        else if (r.status == 200) {
            body = r.body;
            SDL_SaveFile((base + ".json").c_str(), body.data(), body.size());
            if (!r.etag.empty()) SDL_SaveFile((base + ".etag").c_str(), r.etag.data(), r.etag.size());
        } else {
            fail(r.error.empty() ? "GitHub could not be reached" : r.error);
            g_busy = false;
            return 0;
        }
    }
    // the newest full release (not a draft or pre-release) with a Sanctum zip
    Json j;
    std::string best_tag, url, digest;
    long long size = 0;
    if (json_parse(body, j) && j.type == Json::ARR)
        for (const Json &rel : j.arr) {
            if (rel["draft"].b || rel["prerelease"].b) continue;
            std::string tag = rel["tag_name"].str();
            if (!best_tag.empty() && !newer(tag, best_tag)) continue;
            for (const Json &a : rel["assets"].arr) {
                std::string n = a["name"].str();
                std::string l;
                for (char c : n) l += (char)SDL_tolower((unsigned char)c);
                if (l.size() > 4 && l.compare(l.size() - 4, 4, ".zip") == 0 && l.find("sanctum") != std::string::npos) {
                    best_tag = tag, url = a["browser_download_url"].str(), digest = a["digest"].str();
                    size = (long long)a["size"].num();
                    break;
                }
            }
        }
    if (best_tag.empty() || !newer(best_tag, sanctum_version())) {
        g_state = UPD_CURRENT;
        g_busy = false;
        return 0;
    }
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_tag = best_tag, g_url = url, g_digest = digest, g_size = size, g_error.clear();
    }
    sanctum_log("self-update: %s is newer than %s", best_tag.c_str(), sanctum_version());
    if (cfg().auto_update) download();
    else g_state = UPD_AVAILABLE;
    g_busy = false;
    return 0;
}

static void run(intptr_t how) {
    if (demo_mode() || getenv("SANCTUM_NO_UPDATE")) return;
    if (g_busy.exchange(true)) return;
    SDL_Thread *t = SDL_CreateThread(worker, "sanctum-update", (void *)how);
    if (t) SDL_DetachThread(t);
    else g_busy = false;
}

void self_update_start(bool force) {
    // a finished update from last time left the old program renamed: it is not running any more
    std::string old = std::string(SDL_GetBasePath()) + kExe + ".old";
    SDL_RemovePath(old.c_str());
    if (g_state == UPD_READY || g_state == UPD_DOWNLOADING) return;
    run(force ? 1 : 0);
}

void self_update_download() {
    if (g_state == UPD_AVAILABLE || g_state == UPD_FAILED) run(2);
}

UpdState self_update_state() { return (UpdState)g_state.load(); }
std::string self_update_tag() { std::lock_guard<std::mutex> l(g_lock); return g_tag; }
float self_update_progress() { return g_total > 0 ? (float)g_done / (float)g_total : 0; }
std::string self_update_error() { std::lock_guard<std::mutex> l(g_lock); return g_error; }
bool self_update_poll() { return g_announce.exchange(false); }

bool self_update_apply(bool restart) {
    if (g_state != UPD_READY) return false;
    std::string staged;
    {
        std::lock_guard<std::mutex> l(g_lock);
        staged = g_staged;
    }
    std::string here = SDL_GetBasePath();
    if (!here.empty() && (here.back() == '\\' || here.back() == '/')) here.pop_back();
    // Windows lets a running program be renamed, so the overlay can replace Sanctum's own exe
    std::string err = overlay(staged, here);
    if (!err.empty()) {
        fail("Sanctum could not update its folder (" + err + "). Download it from GitHub instead.");
        return false;
    }
    remove_tree(local_dir() + "sanctum-update");
    g_state = UPD_CURRENT;
    sanctum_log("self-update: installed %s", self_update_tag().c_str());
    if (restart) {
        config_save();
        std::string exe = here + "\\" + kExe;
        const char *args[] = {exe.c_str(), nullptr};
        if (SDL_Process *p = SDL_CreateProcess(args, false)) SDL_DestroyProcess(p);   // goes on after Sanctum quits
        app().quit = true;
    }
    return true;
}

}  // namespace sanctum
