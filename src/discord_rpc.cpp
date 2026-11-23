#include "discord_rpc.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>

#include "app.h"
#include "config.h"
#include "selfupdate.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace sanctum {

// SteelTemple Discord application ID (overridable via SANCTUM_DISCORD_APP_ID or settings.ini discord_app_id=)
static const char *kDefaultDiscordAppId = "1569529153349357731";

static std::mutex g_lock;
static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_enabled{false};
static std::atomic<bool> g_dirty{true};
static SDL_Thread *g_thread = nullptr;
static std::string g_details = "Browsing library";
static std::string g_state = "SteelTemple Sanctum";
static long long g_start_ts = 0;

static std::string json_esc(const std::string &s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += c;
        else if ((unsigned char)c >= 0x20) o += c;
    }
    return o;
}

#ifdef _WIN32
static bool write_frame(HANDLE h, uint32_t op, const std::string &payload) {
    uint32_t hdr[2] = {op, (uint32_t)payload.size()};
    DWORD wr = 0;
    if (!WriteFile(h, hdr, sizeof hdr, &wr, nullptr) || wr != sizeof hdr) return false;
    if (!payload.empty() && (!WriteFile(h, payload.data(), (DWORD)payload.size(), &wr, nullptr) || wr != payload.size()))
        return false;
    return true;
}

static bool drain_frame(HANDLE h, uint32_t *op_out = nullptr) {
    DWORD avail = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) || avail < 8) return false;
    uint32_t hdr[2] = {};
    DWORD rd = 0;
    if (!ReadFile(h, hdr, 8, &rd, nullptr) || rd != 8) return false;
    if (op_out) *op_out = hdr[0];
    uint32_t rem = hdr[1];
    char scratch[512];
    while (rem > 0) {
        DWORD want = rem > sizeof scratch ? sizeof scratch : rem;
        if (!ReadFile(h, scratch, want, &rd, nullptr) || rd == 0) break;
        rem -= rd;
    }
    return true;
}

static HANDLE connect_discord(const std::string &app_id) {
    for (int i = 0; i < 10; ++i) {
        char path[64];
        snprintf(path, sizeof path, "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::string hs = "{\"v\":1,\"client_id\":\"" + json_esc(app_id) + "\"}";
        if (!write_frame(h, 0, hs)) { CloseHandle(h); continue; }
        // Wait briefly for READY frame
        for (int k = 0; k < 20; ++k) {
            uint32_t op = 0;
            if (drain_frame(h, &op)) {
                if (op == 1) return h;
                break;
            }
            SDL_Delay(25);
        }
        CloseHandle(h);
    }
    return INVALID_HANDLE_VALUE;
}

static int rpc_worker(void *) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    int nonce = 0;
    Uint64 last_try = 0, last_send = 0;
    while (!g_stop.load()) {
        if (!g_enabled.load() || demo_mode()) {
            if (pipe != INVALID_HANDLE_VALUE) {
                write_frame(pipe, 2, "{}");
                CloseHandle(pipe);
                pipe = INVALID_HANDLE_VALUE;
            }
            SDL_Delay(250);
            continue;
        }
        Uint64 now = SDL_GetTicks();
        if (pipe == INVALID_HANDLE_VALUE) {
            if (now - last_try < 8000 && last_try != 0) { SDL_Delay(250); continue; }
            last_try = now;
            const char *env_id = getenv("SANCTUM_DISCORD_APP_ID");
            std::string id = (env_id && *env_id) ? env_id
                           : (!cfg().discord_app_id.empty() ? cfg().discord_app_id : kDefaultDiscordAppId);
            pipe = connect_discord(id);
            if (pipe != INVALID_HANDLE_VALUE) {
                sanctum_log("discord: connected");
                g_dirty = true;
            } else {
                SDL_Delay(250);
                continue;
            }
        }
        // Drain any inbound replies/pings
        uint32_t op = 0;
        while (drain_frame(pipe, &op)) {
            if (op == 2) { CloseHandle(pipe); pipe = INVALID_HANDLE_VALUE; break; }
            if (op == 3) write_frame(pipe, 4, "{}");
        }
        if (pipe == INVALID_HANDLE_VALUE) continue;

        if (g_dirty.exchange(false) || now - last_send > 60000) {
            std::string det, st;
            long long ts = 0;
            {
                std::lock_guard<std::mutex> l(g_lock);
                det = g_details, st = g_state, ts = g_start_ts;
            }
            char buf[1024];
            snprintf(buf, sizeof buf,
                     "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{"
                     "\"details\":\"%s\",\"state\":\"%s\",\"timestamps\":{\"start\":%lld},"
                     "\"assets\":{\"large_text\":\"SteelTemple Sanctum %s\"}}},\"nonce\":\"st-%d\"}",
                     (unsigned long)GetCurrentProcessId(), json_esc(det).c_str(), json_esc(st).c_str(), ts,
                     sanctum_version(), ++nonce);
            if (!write_frame(pipe, 1, buf)) {
                CloseHandle(pipe);
                pipe = INVALID_HANDLE_VALUE;
            } else {
                last_send = now;
            }
        }
        SDL_Delay(250);
    }
    if (pipe != INVALID_HANDLE_VALUE) {
        write_frame(pipe, 2, "{}");
        CloseHandle(pipe);
    }
    return 0;
}
#endif

void discord_rpc_init() {
    if (demo_mode() || getenv("SANCTUM_SHOT")) return;
    g_start_ts = (long long)time(nullptr);
    g_enabled = cfg().discord_rpc;
    g_stop = false;
#ifdef _WIN32
    if (!g_thread) g_thread = SDL_CreateThread(rpc_worker, "discord-rpc", nullptr);
#endif
}

void discord_rpc_shutdown() {
    g_stop = true;
    if (g_thread) {
        SDL_WaitThread(g_thread, nullptr);
        g_thread = nullptr;
    }
}

void discord_rpc_set_enabled(bool on) {
    g_enabled = on;
    g_dirty = true;
}

void discord_rpc_set_activity(const std::string &details, const std::string &state) {
    {
        std::lock_guard<std::mutex> l(g_lock);
        if (g_details == details && g_state == state) return;
        g_details = details;
        g_state = state;
        g_start_ts = (long long)time(nullptr);
    }
    g_dirty = true;
}

}  // namespace sanctum
