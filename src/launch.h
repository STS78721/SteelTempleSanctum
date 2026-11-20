// Starting a port from Sanctum, the way its own launcher does, and hearing how it went.
//
// Each port has a start profile (launch.cpp, kProfiles): the program, its arguments and environment (the same ones
// its launcher passes), and the files that must exist before the game can start (its built program, the player's
// own game files). When they are all there, Play starts the game directly; otherwise it opens the port's own
// launcher, which does the first setup.
//
// While the game runs Sanctum minimises and sleeps. When it ends: the time played is added up (playtime.ini), and
// the run is judged: THWOMP's own checker (setup\src\tools\prepare\lastrun.py, its known-error list) when present,
// else the exit code and the end of the game's log. The game page then shows how the last run ended.
#pragma once
#include <SDL3/SDL.h>

#include <string>

#include "catalog.h"

namespace sanctum {

enum Verdict { RUN_NONE, RUN_CLEAN, RUN_CRASH, RUN_KNOWN, RUN_FIXED, RUN_NO_START };

struct LastRun {
    int verdict = RUN_NONE;
    std::string issue;      // RUN_KNOWN / RUN_FIXED: the issue that follows it
    std::string fixed_in;   // RUN_FIXED: the release that fixes it
    std::string detail;     // a line for the player (exit code, the log's last error)
    std::string log;        // the game's log, to open
    int seconds = 0;        // how long it ran
};

// what Play will do for this copy: start the game, or open the port's launcher for the first setup
bool can_play(const Port &p, const Install &in);
std::string missing_for_play(const Port &p, const Install &in);   // "" when ready, else what is missing

bool launch(SDL_Window *w, int port, const Install &in, bool own_launcher);
bool game_running();
int running_port();                 // -1 when none
double running_seconds();
void close_game();                  // ends the running game (the player asked, from Sanctum)
void launch_poll(SDL_Window *w);    // each frame (and while waiting): the game's end, and the last-run check

const LastRun *last_run(int port);  // this session's last run of the port, or nullptr
void last_run_clear(int port);

// play time, kept in %APPDATA%\SteelTemple\Sanctum\playtime.ini
void playtime_load();
long long played_seconds(int port);
long long last_played(int port);    // unix time, 0 if never from Sanctum
std::string played_text(int port);  // "Played 3 h 20 min  ·  last today"

}  // namespace sanctum
