// SteelTemple Sanctum: the shared state every screen draws from and acts on.
//
// Layout: the whole UI is laid out on a 1280x720 canvas (the Switch 2 home menu's proportions) and drawn scaled and
// centred into the window, so it looks the same in a window, maximised or fullscreen. Every screen is drawn with the
// ImGui background draw list (no ImGui windows): focus, input and hit-testing are Sanctum's own, so the controller,
// keyboard and mouse all go through one path (input.h).
#pragma once
#include <string>
#include <vector>

#include "imgui.h"

namespace sanctum {

// ---- input: every device becomes these
enum Nav { NAV_NONE, NAV_LEFT, NAV_RIGHT, NAV_UP, NAV_DOWN, NAV_A, NAV_B, NAV_X, NAV_Y, NAV_L, NAV_R, NAV_PLUS, NAV_MINUS, NAV_HOME };

// What the connected pad's face buttons look like, for the button hints.
enum PadStyle { PAD_KEYBOARD, PAD_XBOX, PAD_NINTENDO, PAD_PLAYSTATION, PAD_GENERIC };

enum Screen { SCR_HOME, SCR_GAME, SCR_SETTINGS, SCR_FOLDER, SCR_CONTROLLERS, SCR_SOON, SCR_PORTS, SCR_DOWNLOADS, SCR_NEWS, SCR_PORT_SETTINGS, SCR_PROFILE };
enum FolderFor { FOLDER_COVERS, FOLDER_LIBRARY };

struct Toast {
    std::string text;
    double until = 0, start = 0;
};

struct App {
    Screen screen = SCR_HOME;
    std::vector<Screen> back;   // B returns along this
    double now = 0;              // seconds since start
    float dt = 0;
    bool quit = false;

    // the canvas: canvas point (x, y) is drawn at origin + (x, y) * scale
    float scale = 1, ox = 0, oy = 0;

    // input this frame
    Nav nav = NAV_NONE;          // one action per frame (repeat for held directions is in input.cpp)
    bool mouse_moved = false, clicked = false;
    ImVec2 mouse;                // in canvas points
    PadStyle pad = PAD_KEYBOARD;
    std::string pad_name;        // the last pad used, for the hint bar

    // selection kept across screens
    int game = 0;                // the port the game page shows
    std::string soon_title;      // SCR_SOON's heading
    Toast toast;

    ImFont *font = nullptr;      // one face, loaded large; drawn at any size
    struct SDL_Window *window = nullptr;
};

App &app();
void go(Screen s);               // open a screen (remembers the way back)
void go_back();
void toast(const std::string &text, double seconds = 2.6);
void set_fullscreen(bool on);    // applies and remembers it (Settings > Display, F11, Alt+Enter)
// a line in the file SANCTUM_LOG names (debugging; nothing is written without it). Safe from any thread.
void sanctum_log(const char *fmt, ...);
// SANCTUM_DEMO=1: screenshots for the README. No folders of this PC on screen, no cover art, nothing saved,
// no game started.
bool demo_mode();
std::string shown_path(const std::string &real, const char *port_title = nullptr);

// canvas -> window
inline ImVec2 P(float x, float y) { App &a = app(); return ImVec2(a.ox + x * a.scale, a.oy + y * a.scale); }
inline float S(float v) { return v * app().scale; }
// the mouse is over the canvas rectangle (x, y, w, h)
bool hover(float x, float y, float w, float h);

// screens: each handles app().nav / mouse for itself, then draws
void home_frame();
void game_frame();
void settings_frame();
void folder_frame();
void controllers_frame();
void soon_frame();
void ports_frame();       // Get ports
void downloads_frame();   // the downloads queue
void news_frame();        // Releases & news
void port_settings_frame();   // a port's own settings (port_settings.cpp)
void profile_open();          // the player's profile: icon and colour (profile.cpp)
void profile_frame();

// the controller folder picker; X sets the cover-art folder or the folder new ports install into
void folder_open(const std::string &start, FolderFor what = FOLDER_COVERS);
void game_show_release(int port, const std::string &tag);   // a port's page, on its release notes
void game_show_overview(int port);                          // a port's page, at the top (back from playing)
void get_ports_select(int port);                             // Get ports with this port ticked

}  // namespace sanctum
