// Zero-Inject Corner Performance HUD telemetry sampler:
// Measures live system/process CPU%, game working-set RAM, per-process GPU% (via dynamic PDH when available),
// live zero-inject FPS (window title / fresh port log / DWM compositor timing), and handheld battery — zero admin,
// zero DLL injection into any game.
#pragma once
#include <SDL3/SDL.h>

#include <string>

namespace sanctum {

struct HudMetrics {
    bool valid = false;
    bool has_fps = false;
    float fps = 0.0f;
    int cpu_pct = -1;       // 0..100 overall CPU load
    int gpu_pct = -1;       // 0..100 GPU engine utilization (when PDH counter available)
    float ram_gb = 0.0f;    // game process working set in GB
    int bat_pct = -1;       // handheld/laptop battery % (-1 on desktop)
    bool bat_chg = false;
};

void hud_reset();
void hud_sample(void *game_proc, unsigned long game_pid, void *game_hwnd, const std::string &log_path,
                double now, bool detailed = true);
const HudMetrics &hud_metrics();

}  // namespace sanctum
