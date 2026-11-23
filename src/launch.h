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
bool game_suspended();              // true when the running game is paused via Quick Suspend (0% CPU/GPU)
bool game_frozen();                 // true whenever the running game tree is paused (Quick Suspend OR Quick Overlay)
bool launch_starting();             // started, its window not up yet: Sanctum shows its Starting screen
std::string launch_starting_title();
int running_port();                 // -1 when none
double running_seconds();
void close_game(bool force = false); // ends the running game (force=true terminates hung/frozen games)
void launch_wake_sanctum(SDL_Window *w); // brings Sanctum to front while a game keeps running (in-game Guide overlay)
void launch_wake_hwnd(void *hwnd, bool keep_topmost = false); // brings an arbitrary Sanctum HWND (e.g. transparent overlay) cleanly to foreground
void *launch_game_hwnd();                // current visible HWND of the running game (or nullptr)
void *launch_game_proc();                // Win32 HANDLE of the running game (for zero-inject HUD telemetry)
unsigned long launch_game_pid();         // PID of the running game (or 0)
std::string launch_game_log_path();      // active log path for the running game (or "")
void launch_overlay_pause(bool pause); // soft-pauses running game while Quick Overlay / Sanctum is in front (blocks input bleed)
bool suspend_game(SDL_Window *w);    // minimizes and pauses the running game (Quick Resume)
bool resume_game(SDL_Window *w);     // un-suspends (if paused), hands foreground back to the game, minimizes Sanctum
void launch_on_shutdown();           // ensures no suspended game is left frozen if Sanctum exits
void launch_poll(SDL_Window *w);    // each frame (and while waiting): the game's end, and the last-run check

bool last_run(int port, LastRun &out);  // copies this session's last run of the port (never returns a dangling ptr)
void last_run_clear(int port);

// play time, kept in %APPDATA%\SteelTemple\Sanctum\playtime.ini (tracked per local profile)
void playtime_load();
long long played_seconds(int port);
long long last_played(int port);    // unix time, 0 if never from Sanctum
int played_sessions(int port);      // number of recorded sessions for active profile
std::string played_text(int port);  // "Played 3 h 20 min  ·  last today"
std::string played_duration_short(int port); // "3 h 20 min", "5 min", "< 1 min", or "Not played"
std::string last_played_short(int port);     // "Today", "Yesterday", "4 days ago", or "Never"

// best-effort save directory for a port install (empty if none exists yet)
std::string port_save_dir(const Port &p, const Install &in);

// last-session performance telemetry parsed from the port's log (cached by file size & mtime)
struct PerfSummary {
    bool valid = false;
    float avg_fps = 0.0f;
    float low1_fps = 0.0f;
    int frames = 0;
    int val_errs = 0;
    int shader_errs = 0;
    std::string text;   // e.g. "59.4 fps  ·  1% low 52.6"
};
PerfSummary port_last_perf(const Port &p, const Install &in);

// Persistent multi-session FPS history per port (%APPDATA%\SteelTemple\Sanctum\perf_history.ini)
struct PerfPoint {
    long long ts = 0;
    float avg_fps = 0.0f;
    float low1_fps = 0.0f;
};
void perf_history_load();
std::vector<PerfPoint> port_perf_history(const char *port_id);

// async headless shader pre-warm (<exe> --prepare-shaders with CREATE_NO_WINDOW + live progress)
struct WarmState {
    bool active = false;
    int port = -1;
    int stage = 0;        // 1 = translating shaders, 2 = building driver pipelines
    long long done = 0;
    long long total = 0;
    std::string label;    // human-readable live status
};
bool launch_prepare_shaders(int port, const Install &in);
void cancel_prepare_shaders();
bool shaders_warming();
WarmState shader_warm_state();

}  // namespace sanctum
