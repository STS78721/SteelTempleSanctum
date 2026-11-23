#include "github.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <vector>

#include "catalog.h"
#include "json.h"
#include "net.h"

namespace sanctum {

std::string local_dir() {
    const char *l = getenv("LOCALAPPDATA");
    std::string d = std::string(l ? l : ".") + "\\SteelTemple\\Sanctum\\";
    SDL_CreateDirectory(d.c_str());
    return d;
}

static std::mutex g_lock;
static std::vector<std::pair<int, std::vector<Release>>> g_ready;
static std::string g_status;
static std::atomic<bool> g_busy{false}, g_force{false};

static AssetKind kind_of(const std::string &name) {
    std::string n;
    for (char c : name) n += (char)SDL_tolower((unsigned char)c);
    auto has = [&](const char *s) { return n.find(s) != std::string::npos; };
    bool zip = n.size() > 4 && n.compare(n.size() - 4, 4, ".zip") == 0;
    bool exe = n.size() > 4 && n.compare(n.size() - 4, 4, ".exe") == 0;
    if (zip && (has("portable") || has("standalone"))) return ASSET_PORTABLE;
    if (zip && has("update")) return ASSET_UPDATE;
    if ((exe || zip) && (has("setup") || has("installer"))) return ASSET_WIZARD;   // WonderBloom zips its setup
    return ASSET_OTHER;
}

static bool parse(const std::string &body, std::vector<Release> &out) {
    Json j;
    if (!json_parse(body, j) || j.type != Json::ARR) return false;
    for (const Json &r : j.arr) {
        if (r["draft"].b) continue;
        Release rel;
        rel.tag = r["tag_name"].str();
        rel.title = r["name"].str();
        rel.body = r["body"].str();
        rel.date = r["published_at"].str().substr(0, 10);
        rel.pre = r["prerelease"].b;
        for (const Json &a : r["assets"].arr) {
            Asset as;
            as.name = a["name"].str();
            as.url = a["browser_download_url"].str();
            as.digest = a["digest"].str();
            as.size = a["size"].num();
            as.kind = kind_of(as.name);
            rel.assets.push_back(as);
        }
        if (!rel.tag.empty()) out.push_back(std::move(rel));
    }
    return true;
}

static std::string load(const std::string &path) {
    size_t n = 0;
    char *d = (char *)SDL_LoadFile(path.c_str(), &n);
    std::string s = d ? std::string(d, n) : std::string();
    SDL_free(d);
    return s;
}

static int worker(void *) {
    std::string dir = local_dir() + "releases\\";
    SDL_CreateDirectory(dir.c_str());
    std::string status;
    const auto &ps = ports();
    int bn = builtin_port_count();
    // GitHub allows 60 questions an hour without an account, and an "unchanged" answer counts too: asked at most
    // every 15 minutes (the + button asks at once)
    long long last = atoll(load(dir + "last-check").c_str()), now = (long long)time(nullptr);
    // a port new to this Sanctum (none of its releases saved yet) is asked for at once, whatever the wait
    bool recent = !g_force && now - last < 15 * 60;
    if (recent) {
        bool fresh_port = false;
        for (int i = 0; i < bn; ++i) fresh_port |= !SDL_GetPathInfo((dir + ps[i].id + ".json").c_str(), nullptr);
        if (!fresh_port) { g_busy = false; return 0; }
    }
    g_force = false;
    if (!recent) {
        std::string stamp = std::to_string(now);
        SDL_SaveFile((dir + "last-check").c_str(), stamp.data(), stamp.size());
    }
    for (int i = 0; i < bn; ++i) {
        if (ps[i].custom || !ps[i].repo || !ps[i].repo[0]) continue;
        std::string base = dir + ps[i].id;
        if (recent && SDL_GetPathInfo((base + ".json").c_str(), nullptr)) continue;   // only the new ones
        HttpResult r = http_get(std::string("https://api.github.com/repos/") + ps[i].repo + "/releases?per_page=12",
                                load(base + ".etag"));
        if (r.status == 304) continue;
        if (r.status != 200) { status = r.error.empty() ? "GitHub could not be reached" : r.error; if (!r.status) break; continue; }
        std::vector<Release> rs;
        if (!parse(r.body, rs)) continue;
        SDL_SaveFile((base + ".json").c_str(), r.body.data(), r.body.size());
        if (!r.etag.empty()) SDL_SaveFile((base + ".etag").c_str(), r.etag.data(), r.etag.size());
        std::lock_guard<std::mutex> l(g_lock);
        g_ready.push_back({i, std::move(rs)});
    }
    {
        std::lock_guard<std::mutex> l(g_lock);
        g_status = status.empty() ? std::string() : status + " (showing the releases saved last time)";
    }
    g_busy = false;
    return 0;
}

// the saved lists, on the UI thread before the first frame: a port's page never waits for GitHub, even offline
void github_load_saved() {
    std::string dir = local_dir() + "releases\\";
    auto &ps = ports();
    for (auto &p : ps) {
        if (p.custom) continue;
        std::vector<Release> rs;
        if (parse(load(dir + p.id + ".json"), rs) && !rs.empty()) p.releases = std::move(rs);
    }
}

void github_refresh(bool force) {
    if (force) g_force = true;
    if (g_busy.exchange(true)) return;
    SDL_Thread *t = SDL_CreateThread(worker, "github", nullptr);
    if (t) SDL_DetachThread(t);
    else g_busy = false;
}

bool github_busy() { return g_busy; }

bool github_poll() {
    std::lock_guard<std::mutex> l(g_lock);
    if (g_ready.empty()) return false;
    for (auto &r : g_ready) ports()[r.first].releases = std::move(r.second);
    g_ready.clear();
    return true;
}

std::string github_status() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_status;
}

}  // namespace sanctum
