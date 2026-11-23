#include "input.h"

#include <cmath>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <devpropdef.h>
#include <setupapi.h>
#include <xinput.h>
#endif

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
static bool g_guide_down = false;
static bool g_guide_pending = false;
static SDL_JoystickID g_guide_hit_id = 0;
static double g_guide_cooldown_until = 0;
static bool g_capture_down = false;
static bool g_capture_pending = false;
static double g_capture_cooldown_until = 0;
static double g_last_mouse_input = -10.0;
static float g_last_mx = -1e9f, g_last_my = -1e9f;

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
    if (cfg().hide_cursor) {
        g_last_mouse_input = -10.0;
        if (SDL_CursorVisible()) SDL_HideCursor();
    }
    g_pad_id = id;
    g_pad_native = style_of(pad);
    static const PadStyle icons[] = {PAD_GENERIC, PAD_NINTENDO, PAD_XBOX, PAD_PLAYSTATION};
    app().pad = cfg().button_icons ? icons[cfg().button_icons % 4] : g_pad_native;   // Settings > Controllers
    const char *n = SDL_GetGamepadName(pad);
    app().pad_name = n ? n : "Controller";
}

#ifdef _WIN32
struct XInputBatCache {
    Uint64 at_ms = 0;
    bool valid = false;
    BYTE type = BATTERY_TYPE_DISCONNECTED;
    BYTE level = BATTERY_LEVEL_EMPTY;
};
static XInputBatCache g_xi_bat[4];

struct BtBatEntry {
    std::string id_upper;   // uppercase PnP instance ID + hardware IDs for VID/PID/MAC matching
    int pct = -1;
};
static Uint64 g_bt_bat_at_ms = 0;
static bool g_bt_bat_valid = false;
static std::vector<BtBatEntry> g_bt_bat_entries;

static void reset_xinput_battery_cache() {
    for (auto &c : g_xi_bat) c = {};
    g_bt_bat_valid = false;
}

static bool query_bt_battery(SDL_Gamepad *pad, int *pct_out) {
    Uint16 vid = SDL_GetGamepadVendor(pad);
    Uint16 pid = SDL_GetGamepadProduct(pad);
    const char *path = SDL_GetGamepadPath(pad);
    const char *serial = SDL_GetGamepadSerial(pad);
    bool looks_bt = false;
    if (path) {
        for (const char *p = path; *p; ++p) {
            if ((p[0] == 'B' || p[0] == 'b') && (p[1] == 'T' || p[1] == 't') && (p[2] == 'H' || p[2] == 'h')) {
                looks_bt = true;
                break;
            }
        }
    }
    if (!looks_bt && SDL_GetGamepadConnectionState(pad) == SDL_JOYSTICK_CONNECTION_WIRELESS) looks_bt = true;
    // Official Xbox One / Series X|S BLE controllers may report ConnState=UNKNOWN over WGI/RawInput
    if (!looks_bt && vid == 0x045e && pid != 0x028e && pid != 0) looks_bt = true;
    if (!looks_bt) return false;

    Uint64 now = SDL_GetTicks();
    if (!g_bt_bat_valid || now - g_bt_bat_at_ms >= 5000) {
        g_bt_bat_entries.clear();
        g_bt_bat_at_ms = now;
        g_bt_bat_valid = true;
        static const DEVPROPKEY kBtBatKey = {
            {0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}}, 2
        };
        static const wchar_t *const kEnums[] = {L"BTHLEDevice", L"BTHLE", L"BTHENUM"};
        for (const wchar_t *en : kEnums) {
            HDEVINFO hdi = SetupDiGetClassDevsW(nullptr, en, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
            if (hdi == INVALID_HANDLE_VALUE) continue;
            SP_DEVINFO_DATA did{};
            did.cbSize = sizeof did;
            for (DWORD i = 0; SetupDiEnumDeviceInfo(hdi, i, &did); ++i) {
                DEVPROPTYPE ptype = 0;
                BYTE bval = 0;
                DWORD req = 0;
                if (!SetupDiGetDevicePropertyW(hdi, &did, &kBtBatKey, &ptype, &bval, sizeof bval, &req, 0)) continue;
                if (ptype != DEVPROP_TYPE_BYTE || bval > 100) continue;
                wchar_t wbuf[512] = {};
                std::string combined;
                if (SetupDiGetDeviceInstanceIdW(hdi, &did, wbuf, 512, nullptr)) {
                    for (wchar_t *w = wbuf; *w; ++w)
                        combined.push_back((char)((*w >= L'a' && *w <= L'z') ? (*w - L'a' + L'A') : (*w < 128 ? *w : '?')));
                }
                wchar_t hwbuf[512] = {};
                if (SetupDiGetDeviceRegistryPropertyW(hdi, &did, SPDRP_HARDWAREID, nullptr, (PBYTE)hwbuf, sizeof hwbuf - 2, nullptr)) {
                    combined.push_back(' ');
                    for (int k = 0; k < 511 && !(hwbuf[k] == 0 && hwbuf[k + 1] == 0); ++k) {
                        wchar_t ch = hwbuf[k];
                        combined.push_back(ch ? (char)((ch >= L'a' && ch <= L'z') ? (ch - L'a' + L'A') : (ch < 128 ? ch : '?')) : ' ');
                    }
                }
                if (!combined.empty()) g_bt_bat_entries.push_back({std::move(combined), (int)bval});
            }
            SetupDiDestroyDeviceInfoList(hdi);
        }
    }

    // Build normalized MAC/serial needle (strip ':' and '-') and VID/PID hex needles
    std::string mac;
    if (serial && *serial) {
        for (const char *s = serial; *s; ++s) {
            char c = *s;
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) mac.push_back(c);
            else if (c >= 'a' && c <= 'f') mac.push_back((char)(c - 'a' + 'A'));
        }
    }
    char v1[16], v2[16], p1[16], p2[16];
    SDL_snprintf(v1, sizeof v1, "VID_%04X", vid);
    SDL_snprintf(v2, sizeof v2, "VID&0002%04X", vid);
    SDL_snprintf(p1, sizeof p1, "PID_%04X", pid);
    SDL_snprintf(p2, sizeof p2, "PID&%04X", pid);

    if (mac.size() >= 12) {
        for (const auto &e : g_bt_bat_entries) {
            if (e.id_upper.find(mac) != std::string::npos) {
                if (pct_out) *pct_out = e.pct;
                return true;
            }
        }
    }
    if (vid && pid) {
        for (const auto &e : g_bt_bat_entries) {
            bool vm = e.id_upper.find(v1) != std::string::npos || e.id_upper.find(v2) != std::string::npos;
            bool pm = e.id_upper.find(p1) != std::string::npos || e.id_upper.find(p2) != std::string::npos;
            if (vm && pm) {
                if (pct_out) *pct_out = e.pct;
                return true;
            }
        }
    }
    return false;
}

static bool query_xinput_battery(int slot, BYTE *type_out, BYTE *level_out) {
    if (slot < 0 || slot >= 4) return false;
    Uint64 now = SDL_GetTicks();
    XInputBatCache &c = g_xi_bat[slot];
    if (c.valid && now - c.at_ms < 2000) {
        *type_out = c.type;
        *level_out = c.level;
        return true;
    }
    typedef DWORD (WINAPI *PFN_XInputGetBatteryInformation)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION *);
    static bool loaded = false;
    static PFN_XInputGetBatteryInformation fn = nullptr;
    if (!loaded) {
        loaded = true;
        HMODULE m = LoadLibraryA("xinput1_4.dll");
        if (!m) m = LoadLibraryA("xinput1_3.dll");
        if (m) fn = (PFN_XInputGetBatteryInformation)GetProcAddress(m, "XInputGetBatteryInformation");
    }
    if (!fn) return false;
    XINPUT_BATTERY_INFORMATION bi{};
    if (fn((DWORD)slot, BATTERY_DEVTYPE_GAMEPAD, &bi) != ERROR_SUCCESS) {
        c.valid = false;
        return false;
    }
    c.at_ms = now;
    c.valid = true;
    c.type = bi.BatteryType;
    c.level = bi.BatteryLevel;
    *type_out = bi.BatteryType;
    *level_out = bi.BatteryLevel;
    return true;
}
#endif

PadPower input_pad_power(SDL_Gamepad *pad, int *pct_out) {
    if (pct_out) *pct_out = -1;
    if (!pad) return PAD_PWR_NONE;

#ifdef _WIN32
    // SDL3 maps XInput's BATTERY_TYPE_WIRED ("wired device, no battery") to SDL_POWERSTATE_CHARGING (100%),
    // causing wired Xbox 360 pads and 2.4 GHz USB dongles/docks to falsely report "100% (Charging)".
    // Query XInput directly for XInput-backed pads so only genuine wireless batteries report charge.
    const char *path = SDL_GetGamepadPath(pad);
    if (path && SDL_strncmp(path, "XInput#", 7) == 0 && path[7] >= '0' && path[7] <= '3') {
        int slot = path[7] - '0';
        BYTE btype = BATTERY_TYPE_UNKNOWN, blevel = BATTERY_LEVEL_EMPTY;
        if (query_xinput_battery(slot, &btype, &blevel)) {
            if (btype == BATTERY_TYPE_ALKALINE || btype == BATTERY_TYPE_NIMH) {
                static const int kPct[] = {5, 25, 60, 100};
                if (pct_out) *pct_out = kPct[SDL_clamp((int)blevel, 0, 3)];
                return PAD_PWR_BATTERY;
            }
            if (btype == BATTERY_TYPE_UNKNOWN && query_bt_battery(pad, pct_out)) return PAD_PWR_BATTERY;
            return PAD_PWR_NONE;
        }
        int pct = -1;
        SDL_PowerState ps = SDL_GetGamepadPowerInfo(pad, &pct);
        if (ps == SDL_POWERSTATE_ON_BATTERY) {
            if (pct_out) *pct_out = pct;
            return PAD_PWR_BATTERY;
        }
        return PAD_PWR_NONE;
    }
#endif

    int pct = -1;
    SDL_PowerState ps = SDL_GetGamepadPowerInfo(pad, &pct);
    // Wired Xbox 360 USB descriptor (045E:028E) carries no battery telemetry even over RawInput/WGI
    if (SDL_GetGamepadVendor(pad) == 0x045e && SDL_GetGamepadProduct(pad) == 0x028e &&
        (ps == SDL_POWERSTATE_CHARGING || ps == SDL_POWERSTATE_CHARGED)) {
        return PAD_PWR_NONE;
    }
    switch (ps) {
    case SDL_POWERSTATE_ON_BATTERY:
        if (pct_out) *pct_out = pct;
        return PAD_PWR_BATTERY;
    case SDL_POWERSTATE_CHARGING:
        if (pct_out) *pct_out = pct;
        return PAD_PWR_CHARGING;
    case SDL_POWERSTATE_CHARGED:
        if (pct_out) *pct_out = pct;
        return PAD_PWR_CHARGED;
    default:
#ifdef _WIN32
        if (query_bt_battery(pad, pct_out)) return PAD_PWR_BATTERY;
#endif
        return PAD_PWR_NONE;
    }
}

SDL_JoystickID input_active_pad_id() { return g_pad_id; }

bool input_pad_battery(int *pct_out, bool *charging_out) {
    SDL_Gamepad *pad = g_pad_id ? SDL_GetGamepadFromID(g_pad_id) : nullptr;
    if (!pad) {
        int cnt = 0;
        if (SDL_JoystickID *ids = SDL_GetGamepads(&cnt)) {
            for (int i = 0; i < cnt && !pad; ++i) pad = SDL_GetGamepadFromID(ids[i]);
            SDL_free(ids);
        }
    }
    int pct = -1;
    PadPower pw = input_pad_power(pad, &pct);
    if (pw == PAD_PWR_NONE) return false;
    if (pct_out) *pct_out = pct;
    if (charging_out) *charging_out = (pw == PAD_PWR_CHARGING);
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

void input_suppress_guide(double now) {
    g_guide_down = true;
    g_guide_pending = false;
    g_guide_hit_id = 0;
    g_guide_cooldown_until = now + 0.65;
    if (g_pressed == NAV_HOME) g_pressed = NAV_NONE;
}

void input_bg_event(const SDL_Event &e, double now) {
    if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) {
        input_event(e);
        return;
    }
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_MISC1) {
        if (now >= g_capture_cooldown_until && !g_capture_pending) {
            g_capture_pending = true;
            g_capture_down = true;
        }
    } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP && e.gbutton.button == SDL_GAMEPAD_BUTTON_MISC1) {
        g_capture_down = false;
    }
    if (!cfg().guide_overlay) return;
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
        if (now >= g_guide_cooldown_until && !g_guide_pending) {
            g_guide_pending = true;
            g_guide_hit_id = e.gbutton.which;
        }
    } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_UP && e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
        g_guide_down = false;
    }
}

bool input_poll_capture_press(double now) {
    if (g_capture_pending) {
        g_capture_pending = false;
        if (now >= g_capture_cooldown_until) {
            g_capture_cooldown_until = now + 0.90;
            sanctum_log("input: Capture/Share button latched (via=event)");
            return true;
        }
    }
    SDL_UpdateGamepads();
    int cnt = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&cnt);
    bool down = false;
    for (int i = 0; ids && i < cnt; ++i) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
        if (g && SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_MISC1)) { down = true; break; }
    }
    SDL_free(ids);
    if (!down) { g_capture_down = false; return false; }
    if (g_capture_down || now < g_capture_cooldown_until) return false;
    g_capture_down = true;
    g_capture_cooldown_until = now + 0.90;
    sanctum_log("input: Capture/Share button detected (via=poll)");
    return true;
}

bool input_poll_guide_press(double now) {
    if (!cfg().guide_overlay) {
        g_guide_down = false;
        g_guide_pending = false;
        return false;
    }
    if (g_guide_pending) {
        g_guide_pending = false;
        SDL_JoystickID hit = g_guide_hit_id;
        g_guide_hit_id = 0;
        if (now >= g_guide_cooldown_until) {
            g_guide_down = true;
            g_guide_cooldown_until = now + 0.65;
            if (hit) use_pad(hit);
            if (g_pressed == NAV_HOME) g_pressed = NAV_NONE;
            sanctum_log("input: Guide press latched in background (pad=%u, via=event)", (unsigned)hit);
            input_rumble(HAP_OK, true);
            return true;
        }
    }
    SDL_UpdateGamepads();
    int cnt = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&cnt);
    bool down = false;
    SDL_JoystickID hit_id = 0;
    for (int i = 0; ids && i < cnt; ++i) {
        SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
        if (g && SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_GUIDE)) {
            down = true;
            hit_id = ids[i];
            break;
        }
    }
    SDL_free(ids);
    if (!down) { g_guide_down = false; return false; }
    if (g_guide_down || now < g_guide_cooldown_until) return false;
    g_guide_down = true;
    g_guide_cooldown_until = now + 0.65;
    if (hit_id) use_pad(hit_id);
    if (g_pressed == NAV_HOME) g_pressed = NAV_NONE;
    sanctum_log("input: Guide press detected in background (pad=%u, via=poll)", (unsigned)hit_id);
    input_rumble(HAP_OK, true);
    return true;
}

static bool any_input_held() {
    SDL_PumpEvents();
    SDL_UpdateGamepads();
    int cnt = 0;
    bool held = false;
    if (SDL_JoystickID *ids = SDL_GetGamepads(&cnt)) {
        for (int i = 0; i < cnt && !held; ++i) {
            SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i]);
            if (!g) continue;
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
                if (SDL_GetGamepadButton(g, (SDL_GamepadButton)b)) { held = true; break; }
            }
            if (!held && (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000 ||
                          SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000)) {
                held = true;
            }
        }
        SDL_free(ids);
    }
    if (!held) {
        int nkeys = 0;
        if (const bool *ks = SDL_GetKeyboardState(&nkeys)) {
            static const SDL_Scancode kCheck[] = {
                SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_SPACE,
                SDL_SCANCODE_ESCAPE, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_HOME,
            };
            for (SDL_Scancode sc : kCheck)
                if ((int)sc < nkeys && ks[sc]) { held = true; break; }
        }
    }
    return held;
}

void input_wait_buttons_released(int max_ms) {
    if (!any_input_held()) return;   // 0 ms overhead when resumed via mouse click or automated test
    Uint64 start = SDL_GetTicks();
    while (any_input_held() && (int)(SDL_GetTicks() - start) < max_ms) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED) input_event(ev);
        }
        SDL_Delay(8);
    }
    // Brief settle window so the controller's HID release report reaches all open handles before focus transfers
    SDL_Delay(35);
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_GAMEPAD_REMOVED) input_event(ev);
    }
    g_pressed = NAV_NONE;
    g_held = NAV_NONE;
    g_axis_dir = NAV_NONE;
    g_guide_pending = false;
    g_guide_down = false;
    sanctum_log("input: waited %llu ms for button release before game handoff",
                (unsigned long long)(SDL_GetTicks() - start));
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
#ifdef _WIN32
        reset_xinput_battery_cache();
#endif
        if (SDL_OpenGamepad(e.gdevice.which)) use_pad(e.gdevice.which);
        g_last_activity = a.now;
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
#ifdef _WIN32
        reset_xinput_battery_cache();
#endif
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
        case SDL_GAMEPAD_BUTTON_GUIDE:
            if (a.now >= g_guide_cooldown_until) {
                g_guide_cooldown_until = a.now + 0.35;
                press(NAV_HOME);
            }
            break;
        case SDL_GAMEPAD_BUTTON_MISC1:
            if (a.now >= g_capture_cooldown_until) g_capture_pending = true;
            break;
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
        if (down && !e.key.repeat) {
            a.pad = PAD_KEYBOARD;
            if (cfg().hide_cursor) {
                g_last_mouse_input = -10.0;
                if (SDL_CursorVisible()) SDL_HideCursor();
            }
        }
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
        case SDLK_F12: case SDLK_PRINTSCREEN:
            if (a.now >= g_capture_cooldown_until) g_capture_pending = true;
            break;
        default: break;
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        a.mouse = ImVec2((e.motion.x - a.ox) / a.scale, (e.motion.y - a.oy) / a.scale);
        if (g_last_mx < -1e8f) {
            // Seed initial mouse coordinates on window open/focus without waking the hidden cursor
            g_last_mx = e.motion.x, g_last_my = e.motion.y;
            break;
        }
        float dx = e.motion.x - g_last_mx, dy = e.motion.y - g_last_my;
        if (dx * dx + dy * dy >= 16.0f) {   // ignore 1-3 px desk/sensor jitter
            g_last_mx = e.motion.x, g_last_my = e.motion.y;
            a.mouse_moved = true;
            g_last_activity = a.now;
            g_last_mouse_input = a.now;
            if (!SDL_CursorVisible()) SDL_ShowCursor();
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        a.mouse = ImVec2((e.button.x - a.ox) / a.scale, (e.button.y - a.oy) / a.scale);
        g_last_mx = e.button.x, g_last_my = e.button.y;
        g_last_activity = a.now;
        g_last_mouse_input = a.now;
        if (!SDL_CursorVisible()) SDL_ShowCursor();
        if (e.button.button == SDL_BUTTON_LEFT) a.clicked = true;
        if (e.button.button == SDL_BUTTON_RIGHT || e.button.button == SDL_BUTTON_X1) press(NAV_B);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        g_last_activity = a.now;
        g_last_mouse_input = a.now;
        if (!SDL_CursorVisible()) SDL_ShowCursor();
        press(e.wheel.y > 0 ? NAV_LEFT : e.wheel.y < 0 ? NAV_RIGHT : NAV_NONE);
        break;
    default: break;
    }
}

void input_frame() {
    App &a = app();
    if (cfg().hide_cursor) {
        if (SDL_CursorVisible() && (a.now - g_last_mouse_input >= 2.5)) SDL_HideCursor();
    } else if (!SDL_CursorVisible()) {
        SDL_ShowCursor();
    }
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
