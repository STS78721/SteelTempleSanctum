// The ports Sanctum knows, and where they are on this PC.
//
// Each port has one entry: its GitHub repo, the files that identify an installed copy (its game program, and its own
// launcher when it has one), its tile colours and the key its cover is matched by. Step 1 carries the entries in code
// with the last release seen when the catalog was written; step 2 replaces `known_tag` with the live GitHub release.
//
// Finding installs (so nothing is installed twice): at start the cached list (library.ini) is shown at once, then a
// background search refreshes it: the Setup Wizards' registry entries, then every fixed drive, a few folders deep,
// for the identifying files. Every copy found is kept; the tile uses the newest, the game page lists the others.
#pragma once
#include <string>
#include <vector>

#include "theme.h"

namespace sanctum {

enum PortState { PORT_ABSENT, PORT_INSTALLED, PORT_UPDATE };

struct Install {
    std::string dir;          // the folder holding the game program
    std::string version;      // VERSION / launcher\version.txt / the Setup Wizard's DisplayVersion; may be empty
    bool launcher = false;    // the port's own launcher is beside it (a player's install rather than a bare build)
    bool registered = false;  // installed by its Setup Wizard (Windows' apps list)
    bool dev = false;         // inside a build, staging or backup folder (hidden unless Settings shows them)
};

// A port's GitHub release and its files
enum AssetKind { ASSET_OTHER, ASSET_PORTABLE, ASSET_UPDATE, ASSET_WIZARD };
struct Asset {
    std::string name, url, digest;   // digest: "sha256:<hex>" when GitHub has one
    long long size = 0;
    AssetKind kind = ASSET_OTHER;
};
struct Release {
    std::string tag, title, body, date;   // date: "2026-11-19"
    bool pre = false;
    std::vector<Asset> assets;
    const Asset *find(AssetKind k) const {
        for (const Asset &a : assets) if (a.kind == k) return &a;
        return nullptr;
    }
};
std::string mb(long long bytes);   // "204 MB"

struct Port {
    const char *id;
    const char *title;        // the game, as on its tile
    const char *name;         // the port's own name
    const char *repo;         // owner/name on GitHub
    const char *known_tag;    // the latest release when this catalog was written
    const char *known_size;   // its Portable (or Setup) download
    const char *cover_key;    // matched against the player's cover file names: letters and digits only, lower case
    const char *mono;         // the tile's monogram when no cover is found
    const char *exe;          // the game program that identifies an install
    const char *also;         // a second file that must sit beside it, or nullptr
    const char *launcher;     // the port's own launcher, or nullptr
    const char *uninstall_key;// HKCU ...\Uninstall\<key> written by its Setup Wizard, or nullptr
    PortColours colours;

    std::vector<Install> all;        // every copy found, best first
    std::vector<Install> installs;   // the ones shown (build folders only when Settings asks), best first
    int hidden = 0;                  // build-folder copies left out
    std::vector<Release> releases;   // newest first, from GitHub (or its saved copy)
    const Release *latest() const { return releases.empty() ? nullptr : &releases[0]; }
    std::string latest_tag() const { return releases.empty() ? known_tag : releases[0].tag; }
    std::string download_size() const;   // the package Get ports would fetch
    PortState state() const;
    const Install *best() const { return installs.empty() ? nullptr : &installs[0]; }
};

std::vector<Port> &ports();

void library_load();      // the cached list, instantly
void scan_start();        // the search, on a worker thread
bool scan_running();
int scan_folders();       // folders looked at so far (for the status line)
bool scan_poll();         // on the UI thread each frame: 1 when a finished search changed the ports
void catalog_refilter();  // after the "show build folders" setting changes
void catalog_prefer(int port, const std::string &dir);   // the copy Play starts, remembered (game page: Use this copy)
// each copy's version read from its game program (it carries its release tag): on a worker, once the releases are
// known; cached by the program's size and date in versions.ini
void version_probe_start();
bool version_probe_poll();   // UI thread: 1 when versions changed
// a copy Sanctum has just installed or updated (no search needed)
void catalog_installed(int port, const std::string &dir, const std::string &version);
std::string catalog_match_folder(const std::string &dir);      // matched port title(s) if dir holds a port, else ""
int catalog_add_folder(const std::string &dir);                // manually register a port folder; returns ports added
bool catalog_remove(int port, const std::string &dir);         // drop a deleted/uninstalled copy from library.ini
std::string tag_key(const std::string &s);   // "Beta 6.3" == "Beta-6.3"
bool port_files(const Port &p, const std::string &dir);   // dir holds this port (its game or its launcher)
// the game program exists (a port that builds its game on this PC has only its launcher until the first setup)
bool port_ready(const Port &p, const Install &in);

}  // namespace sanctum
