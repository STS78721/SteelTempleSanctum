// Every device becomes one Nav per frame: pads (SDL3 gamepads, by the label printed on the button, so A confirms on
// Xbox, Nintendo and PlayStation pads alike), the keyboard, and the mouse (hover moves focus, click is A, the
// right button is B). Held directions and sticks repeat like a console menu: a pause, then a steady step.
#pragma once
#include <SDL3/SDL.h>

#include "app.h"

namespace sanctum {

void input_event(const SDL_Event &e);
void input_frame();   // after the frame's events: picks this frame's Nav (with repeat)

// Settings > Controllers: Button layout (which face button confirms) and Button icons (what the hints draw)
enum FacePos { FACE_BOTTOM, FACE_RIGHT, FACE_LEFT, FACE_TOP };
FacePos nav_position(Nav n);   // where the button for A / B / X / Y sits on the pad in use
void input_refresh_style();    // after Button icons changes

// Subtle Switch-style controller vibration (no-op when keyboard/mouse is active or rumble is disabled)
enum Haptic { HAP_TICK, HAP_OK, HAP_BACK, HAP_LAUNCH, HAP_ERROR };
void input_rumble(Haptic h, bool force = false);

// Active stick deadzone threshold (0.35 / 0.55 / 0.72 based on cfg().stick_deadzone)
float input_deadzone_threshold();

// Timestamp (app().now) of the most recent controller, keyboard, or mouse input (for OLED idle dim)
double input_last_activity();
void input_note_activity();

// Wireless controller battery info (returns true when a connected gamepad reports battery status)
bool input_pad_battery(int *pct_out, bool *charging_out);

// While a game is running: checks if Back+Start+L+R are held for 2.0 s to force-close a hung game
bool input_poll_force_close(double now);

}  // namespace sanctum
