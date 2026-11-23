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

// Wireless controller battery info (excludes wired pads & 2.4 GHz XInput dongles with no battery telemetry)
enum PadPower { PAD_PWR_NONE = 0, PAD_PWR_BATTERY, PAD_PWR_CHARGING, PAD_PWR_CHARGED };
PadPower input_pad_power(SDL_Gamepad *pad, int *pct_out);
bool input_pad_battery(int *pct_out, bool *charging_out);
SDL_JoystickID input_active_pad_id();

// While a game is running: checks if Back+Start+L+R are held for 2.0 s to force-close a hung game
bool input_poll_force_close(double now);

// While a game is running (Sanctum minimized or unfocused): event capture & rising-edge detection for Guide / PS / Home
void input_bg_event(const SDL_Event &e, double now);
bool input_poll_guide_press(double now);
void input_suppress_guide(double now);

// Instant screenshot trigger: Share (Xbox) / Capture (Switch) / Mic-Mute (DualSense) = SDL_GAMEPAD_BUTTON_MISC1 (plus F12/PrtSc inside Sanctum)
bool input_poll_capture_press(double now);

// Waits briefly until held controller buttons / triggers / confirm keys are released before handing focus back to a game,
// so the A / B / Guide press that triggered Resume / Quick Resume never bleeds into the game.
void input_wait_buttons_released(int max_ms = 650);

}  // namespace sanctum
