// Each port's own in-game settings, inside Sanctum.
//
// The lists come from the ports' own menus (tools/extract_settings.py writes assets/settings/<port>.json: its pages,
// rows, words, values and defaults). Sanctum shows them in the port's colours and writes the port's own ini beside
// its game, the way the in-game menu does (runtime/cfg.c): [sections] only group, a key is found in any section and
// the last line of it wins; a key not yet in the file goes under its section. Only the lines Sanctum changes are
// touched (comments, order and every other key stay); the first change of a session saves <ini>.sanctum-bak.
// Reset (Y) removes the key, so the game's own default applies again.
#pragma once
#include <string>
#include <vector>

namespace sanctum {

struct SetRow {
    std::string type;               // "group", "toggle", "choice", "slider"
    std::string key, ini, section, label, desc, def, fmt;
    std::vector<std::string> values, names;
    float lo = 0, hi = 0, step = 1;
    bool live = false, inverted = false;
};
struct SetPage {
    std::string title;
    std::vector<SetRow> rows;
};
struct PortSettings {
    bool loaded = false;
    std::string ini, prefix, read_from;
    std::vector<SetPage> pages;
    int count = 0;   // settings (not headings)
};

const PortSettings *port_settings(const char *port_id);   // nullptr when Sanctum has no list for the port
void port_settings_open(int port);                         // the full-screen page (SCR_PORT_SETTINGS)
int port_settings_changed(int port);                       // how many of its settings the player has set
std::string port_settings_ini_path(int port);              // path to the port's active ini file, or ""

// a small ini editor that keeps the file as it is
struct Ini {
    std::string path;
    std::vector<std::string> lines;
    bool crlf = true;   // the file's line endings (a new file: Windows')
    bool load(const std::string &p);
    bool get(const std::string &key, std::string &out) const;   // the last line of key, any section
    void set(const std::string &key, const std::string &value, const std::string &section);
    void remove(const std::string &key);
    bool save();
};

}  // namespace sanctum
