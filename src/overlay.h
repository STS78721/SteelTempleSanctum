// Console-style Sanctum Quick Overlay: slides in from the right when pressing the controller Guide / PS / Home button
// (or Home key), both inside Sanctum and while a game launched from Sanctum is running.
#pragma once
#include <SDL3/SDL.h>

#include "app.h"

namespace sanctum {

bool overlay_is_open();
void overlay_toggle(SDL_Window *w);          // Guide / Home pressed while inside Sanctum
void overlay_open_from_game(SDL_Window *w);  // Guide / Home pressed while a game is running (wakes Sanctum)
void overlay_close();
void overlay_on_game_exit();                 // called by launch_poll() if the running game ends while overlay is up

// Draws and handles input for the Quick Overlay (called each frame after the active screen, before toast_draw).
void overlay_frame(Nav nav, bool clicked);

}  // namespace sanctum
