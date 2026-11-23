#include "input.h"

#include <cmath>

#include "app.h"
#include "config.h"

namespace sanctum {

static Nav g_pressed = NAV_NONE;     // a button this frame
static Nav g_held = NAV_NONE;        // the direction held (pad, stick or arrow key)
static double g_held_since = 0, g_next_repeat = 0;
static float g_axis_x = 0, g_axis_y = 0;
static Nav g_axis_dir = NAV_NONE;
static double g_last_activity = 0;
static double g_chord_since = -1;
static bool g_chord_fired = false;

float input_deadzone_threshold() {
    static const float kDz[] = {0.35f, 0.55f, 0.72f};
    return kDz[SDL_clamp(cfg().stick_deadzone, 0, 2)];
}

double input_last_activity() { return g_last_activity; }
void input_note_activity() { g_last_activity = app().now; }

static PadStyle style_of(SDL_Gamepad *pad) {
    switch (SDL_GetGamepadType(pad)) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE: return PAD_XBOX;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return PAD_PLAYSTATION;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return PAD_NINTENDO;
    default: return PAD_GENERIC;
    }
}

static void press(Nav n) {
    if (g_pressed == NAV_NONE) g_pressed = n;
}

static void hold(Nav n, bool down) {
    if (down) {
        press(n);
        g_held = n;
        g_held_since = app().now;
        g_next_repeat = app().now + 0.38;
    } else if (g_held == n) {
        g_held = NAV_NONE;
    }
}

static PadStyle g_pad_native = PAD_GENERIC;   // the pad in use, before the Button icons setting
static SDL_JoystickID g_pad_id = 0;           // the last pad used, for subtle haptics
static Sint64 g_last_rumble_ms = -1000;

static void use_pad(SDL_JoystickID id) {
    SDL_Gamepad *pad = SDL_GetGamepadFromID(id);
    if (!pad) return;
    if (cfg().hide_cursor && SDL_CursorVisible()) SDL_HideCursor();
    g_pad_id = id;
    g_pad_native = style_of(pad);
    static const PadStyle icons[] = {PAD_GENERIC, PAD_NINTENDO, PAD_XBOX, PAD_PLAYSTATION};
    app().pad = cfg().button_icons ? icons[cfg().button_icons % 4] : g_pad_native;   // Settings > Controllers
    const char *n = SDL_GetGamepadName(pad);
    app().pad_name = n ? n : "Controller";
}

bool input_pad_battery(int *pct_out, bool *charging_out) {
    SDL_Gamepad *pad = g_pad_id ? SDL_GetGamepadFromID(g_pad_id) : nullptr;
    if (!pad) {
        int cnt = 0;
        if (SDL_JoystickID *ids = SDL_GetGamepads(&cnt)) {
            for (int i = 0; i < cnt && !pad; ++i) pad = SDL_GetGamepadFromID(ids[i]);
            SDL_free(ids);
        }
    }
    if (!pad) return false;
    int pct = -1;
    SDL_PowerState ps = SDL_GetGamepadPowerInfo(pad, &pct);
    if (ps != SDL_POWERSTATE_ON_BATTERY && ps != SDL_POWERSTATE_CHARGING && ps != SDL_POWERSTATE_CHARGED)
        return false;
    if (pct_out) *pct_out = pct;
    if (charging_out) *charging_out = (ps == SDL_POWERSTATE_CHARGING || ps == SDL_POWERSTATE_CHARGED);
    return true;
}

bool input_poll_force_close(double now) {
    if (!cfg().force_close_chord) {
        g_chord_since = -1;
        g_chord_fired = false;
        return false;
    }
    SDL_UpdateGamepads();
    int cnt = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&cnt);
    bool held = false;
    for (int i = 0; ids && i < cnt; ++i) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
        if (!g) continue;
        if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_BACK) &&
            SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START) &&
            SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) &&
            SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) {
            held = true;
            break;
        }
    }
    SDL_free(ids);
    if (!held) {
        g_chord_since = -1;
        g_chord_fired = false;
        return false;
    }
    if (g_chord_since < 0) g_chord_since = now;
    if (!g_chord_fired && now - g_chord_since >= 2.0) {
        g_chord_fired = true;
        input_rumble(HAP_LAUNCH, true);
        return true;
    }
    return false;
}

void input_rumble(Haptic h, bool force) {
    if (!cfg().rumble || demo_mode()) return;
    if (!force && app().pad == PAD_KEYBOARD) return;
    SDL_Gamepad *pad = g_pad_id ? SDL_GetGamepadFromID(g_pad_id) : nullptr;
    if (!pad && force) {
        int cnt = 0;
        if (SDL_JoystickID *ids = SDL_GetGamepads(&cnt)) {
            for (int i = 0; i < cnt && !pad; ++i) pad = SDL_GetGamepadFromID(ids[i]);
            SDL_free(ids);
        }
    }
    if (!pad) return;
    Sint64 now = (Sint64)SDL_GetTicks();
    if (!force && now - g_last_rumble_ms < 25) return;
    g_last_rumble_ms = now;
    Uint16 lo = 0, hi = 0;
    Uint32 ms = 16;
    switch (h) {
    case HAP_TICK:   lo = 0x0000; hi = 0x1E00; ms = 16; break;
    case HAP_OK:     lo = 0x1800; hi = 0x3800; ms = 24; break;
    case HAP_BACK:   lo = 0x2400; hi = 0x1000; ms = 20; break;
    case HAP_LAUNCH: lo = 0x4000; hi = 0x5000; ms = 55; break;
    case HAP_ERROR:  lo = 0x3000; hi = 0x1800; ms = 28; break;
    }
    static const float kScale[] = {0.55f, 1.0f, 1.85f};
    int st = SDL_clamp(cfg().rumble_strength, 0, 2);
    float sc = kScale[st];
    lo = (Uint16)SDL_clamp((int)(lo * sc), 0, 0xFFFF);
    hi = (Uint16)SDL_clamp((int)(hi * sc), 0, 0xFFFF);
    if (st == 2) ms += 6;
    SDL_RumbleGamepad(pad, lo, hi, ms);
}

void input_refresh_style() {   // after Button icons changes
    if (app().pad == PAD_KEYBOARD) return;
    static const PadStyle icons[] = {PAD_GENERIC, PAD_NINTENDO, PAD_XBOX, PAD_PLAYSTATION};
    app().pad = cfg().button_icons ? icons[cfg().button_icons % 4] : g_pad_native;
}

// Where an action's button sits on the pad: with Button layout Automatic, where the pad's own printed label puts it
// (A right on a Nintendo pad, at the bottom on others); Nintendo layout always A right, Xbox layout always A bottom.
FacePos nav_position(Nav n) {
    int layout = cfg().pad_layout;
    bool a_right = layout == 1 || (layout == 0 && g_pad_native == PAD_NINTENDO);
    switch (n) {
    case NAV_A: return a_right ? FACE_RIGHT : FACE_BOTTOM;
    case NAV_B: return a_right ? FACE_BOTTOM : FACE_RIGHT;
    case NAV_X: return a_right ? FACE_TOP : FACE_LEFT;
    case NAV_Y: return a_right ? FACE_LEFT : FACE_TOP;
    default: return FACE_BOTTOM;
    }
}

static Nav face(SDL_Gamepad *pad, SDL_GamepadButton b) {
    if (cfg().pad_layout) {   // a fixed layout: by position on the pad, whatever is printed on the buttons
        bool a_right = cfg().pad_layout == 1;
        switch (b) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return a_right ? NAV_B : NAV_A;
        case SDL_GAMEPAD_BUTTON_EAST: return a_right ? NAV_A : NAV_B;
        case SDL_GAMEPAD_BUTTON_WEST: return a_right ? NAV_Y : NAV_X;
        case SDL_GAMEPAD_BUTTON_NORTH: return a_right ? NAV_X : NAV_Y;
        default: return NAV_NONE;
        }
    }
    switch (SDL_GetGamepadButtonLabel(pad, b)) {   // the label printed on it, not its position
    case SDL_GAMEPAD_BUTTON_LABEL_A:
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return NAV_A;
    case SDL_GAMEPAD_BUTTON_LABEL_B:
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return NAV_B;
    case SDL_GAMEPAD_BUTTON_LABEL_X:
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return NAV_X;
    case SDL_GAMEPAD_BUTTON_LABEL_Y:
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return NAV_Y;
    default: break;
    }
    // no label (a generic pad): south confirms, east goes back
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return NAV_A;
    case SDL_GAMEPAD_BUTTON_EAST: return NAV_B;
    case SDL_GAMEPAD_BUTTON_WEST: return NAV_X;
    case SDL_GAMEPAD_BUTTON_NORTH: return NAV_Y;
    default: return NAV_NONE;
    }
}

void input_event(const SDL_Event &e) {
    App &a = app();
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        if (SDL_OpenGamepad(e.gdevice.which)) use_pad(e.gdevice.which);
        g_last_activity = a.now;
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (SDL_Gamepad *g = SDL_GetGamepadFromID(e.gdevice.which)) SDL_CloseGamepad(g);
        if (g_pad_id == e.gdevice.which) g_pad_id = 0;
        a.pad = PAD_KEYBOARD;
        a.pad_name.clear();
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        if (down) { use_pad(e.gbutton.which); g_last_activity = a.now; }
        SDL_GamepadButton b = (SDL_GamepadButton)e.gbutton.button;
        Nav n = NAV_NONE;
        switch (b) {
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: n = NAV_LEFT; break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: n = NAV_RIGHT; break;
        case SDL_GAMEPAD_BUTTON_DPAD_UP: n = NAV_UP; break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: n = NAV_DOWN; break;
        default: break;
        }
        if (n) { hold(n, down); break; }
        if (!down) break;
        switch (b) {
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: press(NAV_L); break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: press(NAV_R); break;
        case SDL_GAMEPAD_BUTTON_START: press(NAV_PLUS); break;
        case SDL_GAMEPAD_BUTTON_BACK: press(NAV_MINUS); break;
        case SDL_GAMEPAD_BUTTON_GUIDE: press(NAV_HOME); break;
        default: press(face(SDL_GetGamepadFromID(e.gbutton.which), b));
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) g_axis_x = e.gaxis.value / 32767.0f;
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) g_axis_y = e.gaxis.value / 32767.0f;
        if (std::fabs(e.gaxis.value) > 32767.0f * input_deadzone_threshold()) {
            use_pad(e.gaxis.which);
            g_last_activity = a.now;
        }
        break;
    case SDL_EVENT_TEXT_INPUT:
        if (a.text_entry) { a.typed += e.text.text; g_last_activity = a.now; }
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        bool down = e.type == SDL_EVENT_KEY_DOWN;
        if (down) g_last_activity = a.now;
        if (a.text_entry) {   // typing: only the arrows move; Backspace, Enter and Esc edit
            if (down) a.pad = PAD_KEYBOARD;
            Nav dir = e.key.key == SDLK_LEFT ? NAV_LEFT : e.key.key == SDLK_RIGHT ? NAV_RIGHT
                    : e.key.key == SDLK_UP ? NAV_UP : e.key.key == SDLK_DOWN ? NAV_DOWN : NAV_NONE;
            if (dir) { if (!e.key.repeat) hold(dir, down); break; }
            if (!down) break;
            if (e.key.key == SDLK_BACKSPACE) a.key_back = true;
            if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) a.key_enter = true;
            if (e.key.key == SDLK_ESCAPE) a.key_escape = true;
            break;
        }
        if (down && !e.key.repeat) a.pad = PAD_KEYBOARD;
        Nav dir = NAV_NONE;
        switch (e.key.key) {
        case SDLK_LEFT: case SDLK_A: dir = NAV_LEFT; break;
        case SDLK_RIGHT: case SDLK_D: dir = NAV_RIGHT; break;
        case SDLK_UP: case SDLK_W: dir = NAV_UP; break;
        case SDLK_DOWN: case SDLK_S: dir = NAV_DOWN; break;
        default: break;
        }
        if (dir) { if (!e.key.repeat) hold(dir, down); break; }
        if (!down || e.key.repeat) break;
        switch (e.key.key) {
        case SDLK_RETURN: case SDLK_SPACE: case SDLK_KP_ENTER: press(NAV_A); break;
        case SDLK_ESCAPE: case SDLK_BACKSPACE: press(NAV_B); break;
        case SDLK_X: press(NAV_X); break;
        case SDLK_Y: case SDLK_TAB: press(NAV_Y); break;
        case SDLK_Q: case SDLK_PAGEUP: press(NAV_L); break;
        case SDLK_E: case SDLK_PAGEDOWN: press(NAV_R); break;
        case SDLK_F5: case SDLK_EQUALS: press(NAV_PLUS); break;
        case SDLK_MINUS: case SDLK_F1: press(NAV_MINUS); break;
        case SDLK_HOME: press(NAV_HOME); break;
        default: break;
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        a.mouse = ImVec2((e.motion.x - a.ox) / a.scale, (e.motion.y - a.oy) / a.scale);
        a.mouse_moved = true;
        g_last_activity = a.now;
        if (!SDL_CursorVisible()) SDL_ShowCursor();
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        a.mouse = ImVec2((e.button.x - a.ox) / a.scale, (e.button.y - a.oy) / a.scale);
        g_last_activity = a.now;
        if (e.button.button == SDL_BUTTON_LEFT) a.clicked = true;
        if (e.button.button == SDL_BUTTON_RIGHT || e.button.button == SDL_BUTTON_X1) press(NAV_B);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        g_last_activity = a.now;
        press(e.wheel.y > 0 ? NAV_LEFT : e.wheel.y < 0 ? NAV_RIGHT : NAV_NONE);
        break;
    default: break;
    }
}

void input_frame() {
    App &a = app();
    // the left stick as a d-pad, with the same repeat
    float dz = input_deadzone_threshold();
    Nav stick = NAV_NONE;
    if (std::fabs(g_axis_x) > dz || std::fabs(g_axis_y) > dz)
        stick = std::fabs(g_axis_x) > std::fabs(g_axis_y) ? (g_axis_x < 0 ? NAV_LEFT : NAV_RIGHT)
                                                          : (g_axis_y < 0 ? NAV_UP : NAV_DOWN);
    if (stick != g_axis_dir) {
        if (g_axis_dir) hold(g_axis_dir, false);
        if (stick) hold(stick, true);
        g_axis_dir = stick;
    }
    a.nav = g_pressed;
    g_pressed = NAV_NONE;
    if (a.nav == NAV_NONE && g_held && a.now >= g_next_repeat) {
        a.nav = g_held;
        // speeds up the longer it is held, like scrolling a long list on a console
        g_next_repeat = a.now + (a.now - g_held_since > 1.6 ? 0.05 : 0.09);
    }
}

}  // namespace sanctum
