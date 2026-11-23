#include "hud.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "draw.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#endif

namespace sanctum {

static HudMetrics g_hud{};
static double g_last_sample = -10.0;

#ifdef _WIN32
static ULONGLONG g_prev_idle = 0, g_prev_kern = 0, g_prev_user = 0;
static bool g_cpu_seeded = false;

// DWM timing struct (subset matching DWM_TIMING_INFO from dwmapi.h without linking dwmapi)
#pragma pack(push, 1)
struct DwmTimingMin {
    UINT32 cbSize;
    UINT32 rateRefreshNum, rateRefreshDen;
    UINT64 qpcRefreshPeriod;
    UINT32 rateComposeNum, rateComposeDen;
    UINT64 qpcVBlank;
    UINT64 cRefresh;
    UINT32 cDXRefresh;
    UINT64 qpcCompose;
    UINT64 cFrame;
    UINT32 cDXPresent;
    UINT64 cRefreshFrame;
    UINT64 cFrameSubmitted;
    UINT32 cDXPresentSubmitted;
    UINT64 cFrameConfirmed;
    UINT32 cDXPresentConfirmed;
    UINT64 cRefreshConfirmed;
    UINT32 cDXRefreshConfirmed;
    UINT64 cFramesLate;
    UINT32 cFramesOutstanding;
    UINT64 cFrameDisplayed;
    UINT64 qpcFrameDisplayed;
    UINT64 cRefreshFrameDisplayed;
    UINT64 cFrameComplete;
    UINT64 qpcFrameComplete;
    UINT64 cFramePending;
    UINT64 qpcFramePending;
    UINT64 cFramesDisplayed;
    UINT64 cFramesComplete;
    UINT64 cFramesPending;
    UINT64 cFramesAvailable;
    UINT64 cFramesDropped;
    UINT64 cFramesMissed;
    UINT64 cRefreshNextDisplayed;
    UINT64 cRefreshNextPresented;
    UINT64 cRefreshesDisplayed;
    UINT64 cRefreshesPresented;
    UINT64 cRefreshStarted;
    UINT64 cPixelsReceived;
    UINT64 cPixelsDrawn;
    UINT64 cBuffersEmpty;
};
#pragma pack(pop)

static UINT64 g_prev_dwm_frames = 0;
static double g_prev_dwm_t = 0.0;

// Dynamic PDH loader for zero-admin GPU 3D engine utilization (works across NVIDIA, AMD, and Intel on Win10/11)
struct PdhState {
    bool tried = false;
    bool ok = false;
    HMODULE mod = nullptr;
    HANDLE query = nullptr;
    HANDLE counter = nullptr;
    using OpenFn = LONG (WINAPI *)(LPCWSTR, DWORD_PTR, HANDLE *);
    using AddFn = LONG (WINAPI *)(HANDLE, LPCWSTR, DWORD_PTR, HANDLE *);
    using CollectFn = LONG (WINAPI *)(HANDLE);
    using GetArrFn = LONG (WINAPI *)(HANDLE, DWORD, LPDWORD, LPDWORD, void *);
    using CloseFn = LONG (WINAPI *)(HANDLE);
    OpenFn open_fn = nullptr;
    AddFn add_fn = nullptr;
    CollectFn collect_fn = nullptr;
    GetArrFn get_arr_fn = nullptr;
    CloseFn close_fn = nullptr;
};
static PdhState g_pdh{};

struct PdhFmtCounterVal {
    DWORD status;
    union {
        LONG lval;
        double dval;
        LONGLONG llval;
    };
};

struct PdhFmtCounterItemW {
    LPWSTR name;
    PdhFmtCounterVal value;
};

static int sample_gpu_pdh() {
    if (!g_pdh.tried) {
        g_pdh.tried = true;
        g_pdh.mod = LoadLibraryW(L"pdh.dll");
        if (g_pdh.mod) {
            g_pdh.open_fn = (PdhState::OpenFn)(void *)GetProcAddress(g_pdh.mod, "PdhOpenQueryW");
            g_pdh.add_fn = (PdhState::AddFn)(void *)GetProcAddress(g_pdh.mod, "PdhAddEnglishCounterW");
            g_pdh.collect_fn = (PdhState::CollectFn)(void *)GetProcAddress(g_pdh.mod, "PdhCollectQueryData");
            g_pdh.get_arr_fn = (PdhState::GetArrFn)(void *)GetProcAddress(g_pdh.mod, "PdhGetFormattedCounterArrayW");
            g_pdh.close_fn = (PdhState::CloseFn)(void *)GetProcAddress(g_pdh.mod, "PdhCloseQuery");
            if (g_pdh.open_fn && g_pdh.add_fn && g_pdh.collect_fn && g_pdh.get_arr_fn &&
                g_pdh.open_fn(nullptr, 0, &g_pdh.query) == 0 &&
                g_pdh.add_fn(g_pdh.query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &g_pdh.counter) == 0) {
                g_pdh.collect_fn(g_pdh.query);
                g_pdh.ok = true;
            }
        }
    }
    if (!g_pdh.ok) return -1;
    if (g_pdh.collect_fn(g_pdh.query) != 0) return -1;
    // PDH_FMT_DOUBLE == 0x00000200; sum all 3D engine instances across processes (clamped 0..100)
    DWORD buf_sz = 0, cnt = 0;
    g_pdh.get_arr_fn(g_pdh.counter, 0x00000200u, &buf_sz, &cnt, nullptr);
    if (buf_sz == 0 || cnt == 0) return -1;
    std::vector<unsigned char> buf(buf_sz);
    if (g_pdh.get_arr_fn(g_pdh.counter, 0x00000200u, &buf_sz, &cnt, buf.data()) != 0) return -1;
    const auto *items = (const PdhFmtCounterItemW *)buf.data();
    double sum = 0.0;
    int valid = 0;
    for (DWORD i = 0; i < cnt; ++i) {
        if (items[i].value.status == 0 && items[i].value.dval >= 0.0 && items[i].value.dval <= 100.0) {
            sum += items[i].value.dval;
            ++valid;
        }
    }
    if (valid == 0) return -1;
    return SDL_clamp((int)(sum + 0.5), 0, 100);
}

static bool parse_fps_from_text(const std::string &s, float &fps_out) {
    // Matches "59.8 fps", "60 FPS", "presents, 54.9 fps", "mean 59.4 fps"
    std::string low = s;
    for (char &c : low) c = (char)SDL_tolower((unsigned char)c);
    size_t at = low.rfind(" fps");
    while (at != std::string::npos) {
        size_t p = at;
        while (p > 0 && low[p - 1] == ' ') --p;
        size_t end = p;
        while (p > 0 && (SDL_isdigit((unsigned char)low[p - 1]) || low[p - 1] == '.')) --p;
        if (end > p) {
            float v = (float)atof(low.substr(p, end - p).c_str());
            if (v >= 1.0f && v <= 500.0f) {
                fps_out = v;
                return true;
            }
        }
        at = (p > 0) ? low.rfind(" fps", p - 1) : std::string::npos;
    }
    return false;
}

static bool sample_log_fps(const std::string &log_path, float &fps_out) {
    if (log_path.empty()) return false;
    SDL_PathInfo pi{};
    if (!SDL_GetPathInfo(log_path.c_str(), &pi) || pi.type != SDL_PATHTYPE_FILE || pi.size == 0) return false;
    // Only trust live log FPS if modified within the last 12 seconds
    SDL_Time cur_ns = 0;
    if (SDL_GetCurrentTime(&cur_ns) && cur_ns > pi.modify_time && (cur_ns - pi.modify_time) > 12000000000LL)
        return false;

    int wn = MultiByteToWideChar(CP_UTF8, 0, log_path.c_str(), -1, nullptr, 0);
    if (wn <= 0) return false;
    std::wstring w(wn - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, log_path.c_str(), -1, w.data(), wn);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    std::string chunk;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0) {
        LONGLONG take = sz.QuadPart > 4096 ? 4096 : sz.QuadPart;
        LARGE_INTEGER off{};
        off.QuadPart = sz.QuadPart - take;
        if (SetFilePointerEx(h, off, nullptr, FILE_BEGIN)) {
            chunk.resize((size_t)take);
            DWORD got = 0;
            if (ReadFile(h, chunk.data(), (DWORD)take, &got, nullptr)) chunk.resize(got);
            else chunk.clear();
        }
    }
    CloseHandle(h);
    return !chunk.empty() && parse_fps_from_text(chunk, fps_out);
}
#endif

void hud_reset() {
    g_hud = {};
    g_last_sample = -10.0;
#ifdef _WIN32
    g_cpu_seeded = false;
    g_prev_dwm_frames = 0;
    g_prev_dwm_t = 0.0;
#endif
}

void hud_sample(void *game_proc, unsigned long game_pid, void *game_hwnd, const std::string &log_path,
                double now, bool detailed) {
    (void)game_pid;
    if (now - g_last_sample < 0.75) return;
    g_last_sample = now;

#ifdef _WIN32
    if (detailed) {
        // 1. Overall CPU %
        FILETIME ft_i{}, ft_k{}, ft_u{};
        if (GetSystemTimes(&ft_i, &ft_k, &ft_u)) {
            auto u64 = [](const FILETIME &f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; };
            ULONGLONG ci = u64(ft_i), ck = u64(ft_k), cu = u64(ft_u);
            if (g_cpu_seeded) {
                ULONGLONG di = ci - g_prev_idle;
                ULONGLONG dt = (ck - g_prev_kern) + (cu - g_prev_user);
                if (dt > 0) {
                    int pct = (int)((dt > di ? (dt - di) * 100ULL : 0ULL) / dt);
                    g_hud.cpu_pct = SDL_clamp(pct, 0, 100);
                }
            }
            g_prev_idle = ci, g_prev_kern = ck, g_prev_user = cu;
            g_cpu_seeded = true;
        }

        // 2. Game process Working Set RAM (GB)
        if (game_proc) {
            PROCESS_MEMORY_COUNTERS pmc{};
            pmc.cb = sizeof pmc;
            if (GetProcessMemoryInfo((HANDLE)game_proc, &pmc, sizeof pmc))
                g_hud.ram_gb = (float)((double)pmc.WorkingSetSize / (1024.0 * 1024.0 * 1024.0));
        }

        // 3. GPU 3D engine % (zero-admin PDH multi-instance sum)
        int gp = sample_gpu_pdh();
        if (gp >= 0) g_hud.gpu_pct = gp;
    }

    // 4. Zero-inject FPS: Tier 1 window title -> Tier 2 live port log -> Tier 3 DWM composition rate
    float fps = 0.0f;
    bool got_fps = false;
    if (game_hwnd) {
        wchar_t title[256]{};
        if (GetWindowTextW((HWND)game_hwnd, title, 256) > 0) {
            int u = WideCharToMultiByte(CP_UTF8, 0, title, -1, nullptr, 0, nullptr, nullptr);
            if (u > 1) {
                std::string ts((size_t)(u - 1), '\0');
                WideCharToMultiByte(CP_UTF8, 0, title, -1, ts.data(), u, nullptr, nullptr);
                got_fps = parse_fps_from_text(ts, fps);
            }
        }
    }
    if (!got_fps) got_fps = sample_log_fps(log_path, fps);
    if (!got_fps) {
        using DwmTimingFn = HRESULT (WINAPI *)(HWND, DwmTimingMin *);
        static DwmTimingFn dwm_timing = []() -> DwmTimingFn {
            HMODULE m = LoadLibraryW(L"dwmapi.dll");
            return m ? (DwmTimingFn)(void *)GetProcAddress(m, "DwmGetCompositionTimingInfo") : nullptr;
        }();
        DwmTimingMin dti{};
        dti.cbSize = sizeof dti;
        if (dwm_timing && SUCCEEDED(dwm_timing(nullptr, &dti))) {
            // Windows 10/11 increments cFrame (cFramesDisplayed is legacy and often 0)
            UINT64 cur_f = dti.cFrame ? dti.cFrame : dti.cFramesDisplayed;
            if (g_prev_dwm_t > 0.0 && now > g_prev_dwm_t && cur_f >= g_prev_dwm_frames) {
                double dt = now - g_prev_dwm_t;
                float est = (float)((double)(cur_f - g_prev_dwm_frames) / dt);
                float max_hz = dti.rateRefreshDen > 0 ? (float)dti.rateRefreshNum / (float)dti.rateRefreshDen : 240.0f;
                if (est >= 1.0f && est <= max_hz + 5.0f) {
                    fps = std::min(est, max_hz);
                    got_fps = true;
                }
            }
            g_prev_dwm_frames = cur_f;
            g_prev_dwm_t = now;
        }
    }
    if (got_fps) {
        g_hud.fps = g_hud.has_fps ? (g_hud.fps * 0.45f + fps * 0.55f) : fps;
        g_hud.has_fps = true;
    }

    // 5. Handheld / laptop battery
    int bp = -1;
    bool bc = false;
    if (system_battery_info(&bp, &bc) && bp >= 0) {
        g_hud.bat_pct = bp;
        g_hud.bat_chg = bc;
    } else {
        g_hud.bat_pct = -1;
    }
    g_hud.valid = true;
#else
    (void)game_proc; (void)game_hwnd; (void)log_path;
#endif
}

const HudMetrics &hud_metrics() { return g_hud; }

}  // namespace sanctum
