// Sanctum Performance Mode: zero-admin, 100% reversible Windows gaming boost & launcher resource trimming.
//
// When active during gameplay:
//   - Boosts the launched game process to Above Normal priority and disables Windows 11 EcoQoS / E-core throttling
//   - Tightens Windows system timer resolution to 1 ms (timeBeginPeriod(1)) for steadier frame pacing & 1% lows
//   - Prevents display/system sleep during controller-only sessions (SetThreadExecutionState)
//   - Switches Windows Power Plan / Power Overlay to High Performance on AC power (auto-restored on suspend/exit)
//   - Drops Sanctum itself into EcoQoS + Below Normal priority and trims its working-set RAM while minimized
#pragma once
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace sanctum {

// Called when Sanctum starts or when cfg().perf_mode / perf_power_plan changes
void perf_sync_idle();

// Called when a game starts or resumes from Quick Suspend
#ifdef _WIN32
void perf_boost_game_start(HANDLE game_proc, DWORD game_pid);
#endif

// Called when a game is Quick-Suspended or exits (restores power plan, timer, sleep state, and Sanctum priority)
void perf_boost_game_stop();

// Called when Sanctum wakes to foreground (e.g. Quick Overlay) while a game stays running
void perf_sanctum_foreground(bool foreground);

// Called on clean exit & atexit: unconditionally restores original Windows power plan, overlay, and timer
void perf_on_shutdown();

bool perf_game_boost_active();
bool perf_power_boost_active();
std::string perf_status_short();   // e.g. "Boost active" / "Auto (on launch)" / "Off"

}  // namespace sanctum
