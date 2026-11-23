#include "perf.h"

#include <SDL3/SDL.h>

#include "app.h"
#include "config.h"

#ifdef _WIN32
#include <mmsystem.h>
#endif

namespace sanctum {

#ifdef _WIN32

struct ProcPowerThrottling {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
};

using SetProcInfoFn = BOOL (WINAPI *)(HANDLE, int, LPVOID, DWORD);
static SetProcInfoFn set_proc_info_fn() {
    static SetProcInfoFn fn = (SetProcInfoFn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation");
    return fn;
}

static void set_eco_qos(HANDLE proc, bool enable_eco) {
    auto fn = set_proc_info_fn();
    if (!fn || !proc) return;
    // ProcessPowerThrottling = 4; Version = 1; EXECUTION_SPEED = 0x1; IGNORE_TIMER_RESOLUTION = 0x4
    ProcPowerThrottling st1{1, 0x1u, enable_eco ? 0x1u : 0u};
    fn(proc, 4, &st1, sizeof st1);
    if (!enable_eco) {
        ProcPowerThrottling st2{1, 0x4u, 0u};   // Windows 11 22H2+: honor game's timer resolution
        fn(proc, 4, &st2, sizeof st2);
    }
}

// Dynamic powrprof.dll loaders (zero admin required on Windows 10/11)
using PowerGetActiveSchemeFn = DWORD (WINAPI *)(HKEY, GUID **);
using PowerSetActiveSchemeFn = DWORD (WINAPI *)(HKEY, const GUID *);
using PowerGetOverlayFn      = DWORD (WINAPI *)(GUID *);
using PowerSetOverlayFn      = DWORD (WINAPI *)(const GUID *);

struct PowerApi {
    HMODULE mod = nullptr;
    PowerGetActiveSchemeFn get_scheme = nullptr;
    PowerSetActiveSchemeFn set_scheme = nullptr;
    PowerGetOverlayFn get_overlay = nullptr;
    PowerSetOverlayFn set_overlay = nullptr;
};

static const PowerApi &power_api() {
    static PowerApi api = []() {
        PowerApi a;
        a.mod = LoadLibraryW(L"powrprof.dll");
        if (a.mod) {
            a.get_scheme  = (PowerGetActiveSchemeFn)(void *)GetProcAddress(a.mod, "PowerGetActiveScheme");
            a.set_scheme  = (PowerSetActiveSchemeFn)(void *)GetProcAddress(a.mod, "PowerSetActiveScheme");
            a.get_overlay = (PowerGetOverlayFn)(void *)GetProcAddress(a.mod, "PowerGetEffectiveOverlayScheme");
            a.set_overlay = (PowerSetOverlayFn)(void *)GetProcAddress(a.mod, "PowerSetActiveOverlayScheme");
        }
        return a;
    }();
    return api;
}

static const GUID kGuidHighPerfPlan    = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
static const GUID kGuidHighPerfOverlay = {0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}};

static bool g_game_boost = false;
static bool g_timer_active = false;
static bool g_exec_state_active = false;
static bool g_plan_swapped = false;
static GUID g_orig_plan{};
static bool g_overlay_swapped = false;
static GUID g_orig_overlay{};
static bool g_sanctum_throttled = false;

static bool is_on_ac_power() {
    SYSTEM_POWER_STATUS sps{};
    if (!GetSystemPowerStatus(&sps)) return true;
    // Desktop (no battery: bit 128) or plugged into AC (ACLineStatus == 1)
    return (sps.BatteryFlag & 128) != 0 || sps.ACLineStatus == 1;
}

static void power_boost_set(bool on) {
    if (demo_mode() || getenv("SANCTUM_SHOT")) return;
    const PowerApi &api = power_api();
    if (on) {
        if (!cfg().perf_power_plan || !is_on_ac_power()) return;
        if (!g_plan_swapped && api.get_scheme && api.set_scheme) {
            GUID *cur = nullptr;
            if (api.get_scheme(nullptr, &cur) == 0 && cur) {
                g_orig_plan = *cur;
                LocalFree(cur);
                if (memcmp(&g_orig_plan, &kGuidHighPerfPlan, sizeof(GUID)) != 0) {
                    if (api.set_scheme(nullptr, &kGuidHighPerfPlan) == 0) {
                        g_plan_swapped = true;
                        sanctum_log("perf: switched Windows power plan to High Performance");
                    }
                }
            }
        }
        if (!g_overlay_swapped && api.get_overlay && api.set_overlay) {
            GUID cur{};
            if (api.get_overlay(&cur) == 0) {
                g_orig_overlay = cur;
                if (memcmp(&g_orig_overlay, &kGuidHighPerfOverlay, sizeof(GUID)) != 0) {
                    if (api.set_overlay(&kGuidHighPerfOverlay) == 0) {
                        g_overlay_swapped = true;
                    }
                }
            }
        }
    } else {
        if (g_overlay_swapped && api.set_overlay) {
            api.set_overlay(&g_orig_overlay);
            g_overlay_swapped = false;
        }
        if (g_plan_swapped && api.set_scheme) {
            api.set_scheme(nullptr, &g_orig_plan);
            g_plan_swapped = false;
            sanctum_log("perf: restored original Windows power plan");
        }
    }
}

void perf_sanctum_foreground(bool foreground) {
    if (foreground) {
        if (g_sanctum_throttled) {
            SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
            set_eco_qos(GetCurrentProcess(), false);
            g_sanctum_throttled = false;
        }
    } else if (cfg().perf_mode > 0 && cfg().perf_trim_ram && !demo_mode()) {
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
        set_eco_qos(GetCurrentProcess(), true);
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
        g_sanctum_throttled = true;
    }
}

void perf_sync_idle() {
    if (g_game_boost) return;   // active gameplay controls boost state
    power_boost_set(cfg().perf_mode == 2);
}

void perf_boost_game_start(HANDLE game_proc, DWORD game_pid) {
    (void)game_pid;
    if (cfg().perf_mode == 0 || demo_mode()) {
        g_game_boost = false;
        return;
    }
    g_game_boost = true;
    if (game_proc) {
        SetPriorityClass(game_proc, ABOVE_NORMAL_PRIORITY_CLASS);
        set_eco_qos(game_proc, false);
    }
    if (!g_timer_active) {
        if (timeBeginPeriod(1) == TIMERR_NOERROR) g_timer_active = true;
    }
    if (!g_exec_state_active) {
        SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
        g_exec_state_active = true;
    }
    power_boost_set(true);
    sanctum_log("perf: game boost active (AboveNormal + EcoQoS off + 1ms timer%s)",
                (g_plan_swapped || g_overlay_swapped) ? " + HighPerf power" : "");
}

void perf_boost_game_stop() {
    g_game_boost = false;
    perf_sanctum_foreground(true);
    if (g_timer_active) {
        timeEndPeriod(1);
        g_timer_active = false;
    }
    if (g_exec_state_active) {
        SetThreadExecutionState(ES_CONTINUOUS);
        g_exec_state_active = false;
    }
    if (cfg().perf_mode != 2) power_boost_set(false);
}

void perf_on_shutdown() {
    g_game_boost = false;
    perf_sanctum_foreground(true);
    if (g_timer_active) { timeEndPeriod(1); g_timer_active = false; }
    if (g_exec_state_active) { SetThreadExecutionState(ES_CONTINUOUS); g_exec_state_active = false; }
    power_boost_set(false);
}

bool perf_game_boost_active() { return g_game_boost; }
bool perf_power_boost_active() { return g_plan_swapped || g_overlay_swapped; }

std::string perf_status_short() {
    if (g_game_boost) return "Boost active";
    if (cfg().perf_mode == 2) return perf_power_boost_active() ? "Always on (High Perf)" : "Always on";
    if (cfg().perf_mode == 1) return "Auto (while gaming)";
    return "Off";
}

#else
void perf_sync_idle() {}
void perf_boost_game_stop() {}
void perf_sanctum_foreground(bool) {}
void perf_on_shutdown() {}
bool perf_game_boost_active() { return false; }
bool perf_power_boost_active() { return false; }
std::string perf_status_short() { return "Off"; }
#endif

}  // namespace sanctum
