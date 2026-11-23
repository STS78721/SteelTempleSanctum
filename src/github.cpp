#include "github.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <vector>

#include "catalog.h"
#include "covers.h"
#include "json.h"
#include "net.h"

namespace sanctum {

static const char *kCatalogUrl = "https://raw.githubusercontent.com/STS78721/SteelTempleSanctum/main/catalog.json";

std::string local_dir() {
    const char *l = getenv("LOCALAPPDATA");
    std::string d = std::string(l ? l : ".") + "\\SteelTemple\\Sanctum\\";
    SDL_CreateDirectory(d.c_str());
    return d;
}

struct FetchPort {
    std::string id, repo, settings_url;
};

static std::mutex g_lock;
static std::vector<std::pair<std::string, std::vector<Release>>> g_ready;
static std::string g_catalog_body;
static bool g_catalog_ready = false;
static std::string g_status;
static std::atomic<bool> g_busy{false}, g_force{false}, g_recheck_queued{false}, g_recheck_force{false};

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

static int worker(void *arg) {
    auto *fp = (std::vector<FetchPort> *)arg;
    std::string dir = local_dir() + "releases\\";
    std::string sdir = local_dir() + "settings\\";
    SDL_CreateDirectory(dir.c_str());
    SDL_CreateDirectory(sdir.c_str());
    std::string status;
    // GitHub allows 60 questions an hour without an account, and an "unchanged" answer counts too: asked at most
    // every 15 minutes (the + button asks at once)
    long long last = atoll(load(dir + "last-check").c_str()), now = (long long)time(nullptr);
    bool forced = g_force.exchange(false);
    // a port new to this Sanctum (none of its releases saved yet) is asked for at once, whatever the wait
    bool recent = !forced && now - last < 15 * 60;
    if (recent) {
        bool fresh_port = !SDL_GetPathInfo((local_dir() + "catalog.json").c_str(), nullptr);
        for (const auto &p : *fp) fresh_port |= !SDL_GetPathInfo((dir + p.id + ".json").c_str(), nullptr);
        if (!fresh_port) { delete fp; g_busy = false; return 0; }
    }
    if (!recent) {
        std::string stamp = std::to_string(now);
        SDL_SaveFile((dir + "last-check").c_str(), stamp.data(), stamp.size());
    }
    // 1. Dynamic remote port catalog (raw.githubusercontent.com does not consume the GitHub API rate limit)
    if (!recent || !SDL_GetPathInfo((local_dir() + "catalog.json").c_str(), nullptr)) {
        HttpResult cr = http_get(kCatalogUrl, load(local_dir() + "catalog.etag"));
        if (cr.status == 200 && !cr.body.empty()) {
            SDL_SaveFile((local_dir() + "catalog.json").c_str(), cr.body.data(), cr.body.size());
            if (!cr.etag.empty()) SDL_SaveFile((local_dir() + "catalog.etag").c_str(), cr.etag.data(), cr.etag.size());
            std::lock_guard<std::mutex> l(g_lock);
            g_catalog_body = std::move(cr.body);
            g_catalog_ready = true;
        }
    }
    // 2. Each port's GitHub releases (+ remote settings JSON if not bundled locally)
    for (const auto &p : *fp) {
        if (p.repo.empty()) continue;
        if (!p.settings_url.empty()) {
            std::string local_set = std::string(SDL_GetBasePath()) + "assets\\settings\\" + p.id + ".json";
            std::string cache_set = sdir + p.id + ".json";
            if (!SDL_GetPathInfo(local_set.c_str(), nullptr) && (!SDL_GetPathInfo(cache_set.c_str(), nullptr) || forced)) {
                HttpResult sr = http_get(p.settings_url, load(sdir + p.id + ".etag"));
                if (sr.status == 200 && !sr.body.empty()) {
                    SDL_SaveFile(cache_set.c_str(), sr.body.data(), sr.body.size());
                    if (!sr.etag.empty()) SDL_SaveFile((sdir + p.id + ".etag").c_str(), sr.etag.data(), sr.etag.size());
                }
            }
        }
        std::string base = dir + p.id;
        if (recent && SDL_GetPathInfo((base + ".json").c_str(), nullptr)) continue;   // only the new ones
        HttpResult r = http_get(std::string("https://api.github.com/repos/") + p.repo + "/releases?per_page=12",
                                load(base + ".etag"));
        if (r.status == 304) continue;
        if (r.status != 200) { status = r.error.empty() ? "GitHub could not be reached" : r.error; if (!r.status) break; continue; }
        std::vector<Release> rs;
        if (!parse(r.body, rs)) continue;
        SDL_SaveFile((base + ".json").c_str(), r.body.data(), r.body.size());
        if (!r.etag.empty()) SDL_SaveFile((base + ".etag").c_str(), r.etag.data(), r.etag.size());
        std::lock_guard<std::mutex> l(g_lock);
        g_ready.push_back({p.id, std::move(rs)});
    }
    delete fp;
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
    if (g_busy.exchange(true)) {
        if (force) g_recheck_force = true;
        g_recheck_queued = true;
        return;
    }
    g_recheck_queued = false;
    if (force) g_force = true;
    auto *fp = new std::vector<FetchPort>();
    const auto &ps = ports();
    int bn = builtin_port_count();
    for (int i = 0; i < bn && i < (int)ps.size(); ++i)
        if (!ps[i].custom && ps[i].repo && ps[i].repo[0])
            fp->push_back({ps[i].id, ps[i].repo, ps[i].settings_url});
    SDL_Thread *t = SDL_CreateThread(worker, "github", fp);
    if (t) SDL_DetachThread(t);
    else { delete fp; g_busy = false; }
}

bool github_busy() { return g_busy || g_recheck_queued; }

bool github_poll() {
    std::vector<std::pair<std::string, std::vector<Release>>> ready;
    std::string cat_body;
    bool cat_ready = false;
    {
        std::lock_guard<std::mutex> l(g_lock);
        ready.swap(g_ready);
        if (g_catalog_ready) { cat_body.swap(g_catalog_body); g_catalog_ready = false; cat_ready = true; }
    }
    bool changed = false;
    if (cat_ready && !cat_body.empty()) {
        int added = catalog_merge_json(cat_body);
        if (added > 0) {
            changed = true;
            covers_reload();
            scan_start();
            github_refresh(false);
        }
    }
    for (auto &r : ready) {
        int idx = port_index_by_id(r.first);
        if (idx >= 0) ports()[idx].releases = std::move(r.second), changed = true;
    }
    if (!g_busy && g_recheck_queued.exchange(false)) github_refresh(g_recheck_force.exchange(false));
    return changed;
}

std::string github_status() {
    std::lock_guard<std::mutex> l(g_lock);
    return g_status;
}

}  // namespace sanctum
