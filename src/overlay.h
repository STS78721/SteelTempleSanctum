// Console-style Sanctum Quick Overlay: slides in from the right when pressing the controller Guide / PS / Home button
// (or Home key), both inside Sanctum and directly over a running game in a transparent WS_EX_TOPMOST side-panel
// (zero window-switch flash), plus click-through Switch-style in-game screenshot notifications.
#pragma once
#include <SDL3/SDL.h>

#include <string>

#include "app.h"

namespace sanctum {

bool overlay_is_open();
void overlay_toggle(SDL_Window *main_win);          // Guide / Home pressed while inside Sanctum or transparent overlay
void overlay_open_from_game(SDL_Window *main_win);  // Guide / Home pressed while a game is running (transparent overlay)
void overlay_close();
void overlay_on_game_exit();                        // called by launch_poll() if the running game ends while overlay is up
void overlay_shutdown();                            // tears down transparent overlay window/renderer/ImGui context

// True when the dedicated transparent WS_EX_TOPMOST overlay window is visible (interactive drawer, toast, OR corner HUD pill).
bool overlay_win_visible();
bool overlay_win_is_interactive();
bool overlay_win_has_toast();
void overlay_sync_hud();
SDL_WindowID overlay_win_id();
void overlay_win_event(const SDL_Event &e);
void overlay_win_render(SDL_Window *main_win, Nav nav, bool clicked);

// Instant screenshot trigger (Share / Capture / Mic-Mute button or Quick Overlay "Take screenshot").
void overlay_capture_screenshot(SDL_Window *main_win);

// Draws and handles input for the Quick Overlay inside the main Sanctum window (when main window is active).
void overlay_frame(Nav nav, bool clicked);

}  // namespace sanctum
