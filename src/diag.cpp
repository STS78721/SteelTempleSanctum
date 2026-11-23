#include "diag.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <string>
#include <vector>

#include "app.h"
#include "config.h"
#include "github.h"
#include "install.h"
#include "platform.h"
#include "port_settings.h"
#include "selfupdate.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace sanctum {

static std::string replace_ci(std::string s, const std::string &needle, const std::string &rep) {
    if (needle.empty() || s.empty()) return s;
    std::string low_s = s, low_n = needle;
    for (char &c : low_s) c = (char)SDL_tolower((unsigned char)c);
    for (char &c : low_n) c = (char)SDL_tolower((unsigned char)c);
    size_t pos = 0;
    while ((pos = low_s.find(low_n, pos)) != std::string::npos) {
        s.replace(pos, needle.size(), rep);
        low_s.replace(pos, needle.size(), rep);
        pos += rep.size();
    }
    return s;
}

static std::string scrub_users_segments(std::string s) {
    // Scrub any C:\Users\<username>\ or C:/Users/<username>/ path pattern regardless of active account
    std::string low = s;
    for (char &c : low) c = (char)SDL_tolower((unsigned char)c);
    size_t pos = 0;
    while (pos < low.size()) {
        size_t hit = low.find(":\\users\\", pos);
        char sep = '\\';
        size_t hit2 = low.find(":/users/", pos);
        if (hit2 != std::string::npos && (hit == std::string::npos || hit2 < hit)) { hit = hit2; sep = '/'; }
        if (hit == std::string::npos || hit == 0) break;
        size_t ustart = hit + 8;
        size_t uend = ustart;
        while (uend < s.size() && s[uend] != '\\' && s[uend] != '/' && s[uend] != ' ' &&
               s[uend] != '\r' && s[uend] != '\n' && s[uend] != '"' && s[uend] != '\'')
            ++uend;
        if (uend > ustart) {
            std::string rep = std::string("<USER>") + (uend < s.size() && (s[uend] == '\\' || s[uend] == '/') ? "" : "");
            size_t span_start = hit - 1;
            size_t span_len = uend - span_start;
            s.replace(span_start, span_len, "<USER>");
            low.replace(span_start, span_len, "<user>");
            pos = span_start + 6;
            (void)sep;
        } else {
            pos = ustart;
        }
    }
    return s;
}

static std::string sanitize_text(std::string s, const std::string &port_dir) {
    if (!port_dir.empty()) {
        s = replace_ci(s, port_dir, "<PORT_DIR>");
        std::string fwd = port_dir;
        for (char &c : fwd) if (c == '\\') c = '/';
        s = replace_ci(s, fwd, "<PORT_DIR>");
    }
    for (const char *ev : {"USERPROFILE", "LOCALAPPDATA", "APPDATA", "HOME"}) {
        if (const char *v = getenv(ev); v && v[0]) {
            s = replace_ci(s, v, std::string("<") + ev + ">");
        }
    }
    if (const char *u = getenv("USERNAME"); u && strlen(u) >= 3) {
        s = replace_ci(s, std::string("\\") + u + "\\", "\\<USER>\\");
        s = replace_ci(s, std::string("/") + u + "/", "/<USER>/");
    }
    s = scrub_users_segments(std::move(s));
    return s;
}

static std::string read_tail_bytes(const std::string &path, size_t max_bytes) {
#ifdef _WIN32
    int wn = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wn <= 0) return {};
    std::wstring w(wn - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), wn);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    std::string out;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0) {
        LONGLONG take = sz.QuadPart > (LONGLONG)max_bytes ? (LONGLONG)max_bytes : sz.QuadPart;
        LARGE_INTEGER off{};
        off.QuadPart = sz.QuadPart - take;
        if (SetFilePointerEx(h, off, nullptr, FILE_BEGIN)) {
            out.resize((size_t)take);
            DWORD got = 0;
            if (ReadFile(h, out.data(), (DWORD)take, &got, nullptr)) out.resize(got);
            else out.clear();
        }
    }
    CloseHandle(h);
    return out;
#else
    (void)path; (void)max_bytes;
    return {};
#endif
}

static std::string sanitize_env_spec(const std::string &spec) {
    if (spec.empty()) return {};
    std::string out;
    size_t at = 0;
    while (at < spec.size()) {
        size_t end = spec.find_first_of(";\n", at);
        if (end == std::string::npos) end = spec.size();
        std::string tok = spec.substr(at, end - at);
        at = end + 1;
        size_t eq = tok.find('=');
        if (eq != std::string::npos) {
            std::string lk = tok.substr(0, eq);
            for (char &c : lk) c = (char)SDL_tolower((unsigned char)c);
            if (lk.find("token") != std::string::npos || lk.find("secret") != std::string::npos ||
                lk.find("key") != std::string::npos || lk.find("pass") != std::string::npos)
                tok = tok.substr(0, eq + 1) + "<REDACTED>";
        }
        if (!out.empty()) out += "; ";
        out += tok;
    }
    return out;
}

static std::string build_settings_diff(int port_idx, const std::string &port_dir) {
    const Port &p = ports()[port_idx];
    std::string ini_path = port_settings_ini_path(port_idx);
    if (ini_path.empty()) return "# No port INI settings file found.\n";
    Ini ini;
    if (!ini.load(ini_path)) return "# Port INI settings file not present yet (using defaults).\n";

    std::string out = "# Non-default settings for " + std::string(p.title) + " (" + p.name + ")\n";
    int diffs = 0;
    if (const PortSettings *ps = port_settings(p.id)) {
        for (const SetPage &pg : ps->pages) {
            for (const SetRow &row : pg.rows) {
                if (row.type == "group" || row.ini.empty()) continue;
                std::string cur;
                if (!ini.get(row.ini, cur)) continue;
                if (cur != row.def) {
                    out += row.ini + " = " + sanitize_text(cur, port_dir) + "   # default: " + row.def + "\n";
                    ++diffs;
                }
            }
        }
    }
    if (diffs == 0) {
        // If no schema matched (e.g. custom port or all schema keys default), include non-comment key=value lines
        int raw_cnt = 0;
        for (const std::string &l : ini.lines) {
            size_t a = l.find_first_not_of(" \t");
            if (a == std::string::npos || l[a] == '#' || l[a] == ';' || l[a] == '[') continue;
            if (l.find('=') != std::string::npos) {
                out += sanitize_text(l, port_dir) + "\n";
                if (++raw_cnt >= 120) break;
            }
        }
        if (raw_cnt == 0) out += "(All settings at defaults)\n";
    }
    return out;
}

std::string export_crash_diagnostics(int port_idx, const Install *in, const LastRun *lr) {
    if (demo_mode() || port_idx < 0 || port_idx >= (int)ports().size()) return {};
    const Port &p = ports()[port_idx];
    std::string pdir = in ? in->dir : "";

    std::string stage = local_dir() + "diag-stage";
    remove_tree(stage);
    SDL_CreateDirectory(stage.c_str());

    // 1. system.txt
    const SystemDiag &sd = system_diagnostics();
    std::string sys = "SteelTemple Sanctum Diagnostics Bundle (Sanitized)\n";
    sys += "==================================================\n";
    sys += "Sanctum Version: " + std::string(sanctum_version()) + "\n";
    sys += "Port:            " + std::string(p.title) + " (" + p.name + ", id=" + p.id + ")\n";
    sys += "Port Version:    " + (in && !in->version.empty() ? in->version : std::string("unknown")) + "\n";
    sys += "OS:              " + sd.os + "\n";
    sys += "CPU:             " + sd.cpu + "\n";
    sys += "RAM:             " + std::to_string(sd.ram_gb) + " GB\n";
    sys += "GPU:             " + sd.gpu + "\n";
    if (!sd.gpu_driver.empty()) sys += "GPU Driver:      " + sd.gpu_driver + "\n";
    int hz = display_current_refresh_rate(nullptr);
    if (hz > 0) sys += "Display Refresh: " + std::to_string(hz) + " Hz\n";
    auto hz_it = cfg().refresh_hz.find(p.id);
    if (hz_it != cfg().refresh_hz.end() && hz_it->second > 0)
        sys += "Port Target Hz:  " + std::to_string(hz_it->second) + " Hz\n";
    sys += "Perf Mode:       " + std::string(cfg().perf_mode == 2 ? "always" : cfg().perf_mode == 1 ? "auto" : "off") + "\n";

    int cnt = 0;
    if (SDL_JoystickID *ids = SDL_GetGamepads(&cnt)) {
        sys += "Controllers:     " + std::to_string(cnt);
        for (int i = 0; i < cnt; ++i)
            if (SDL_Gamepad *g = SDL_GetGamepadFromID(ids[i])) {
                const char *nm = SDL_GetGamepadName(g);
                if (nm) sys += std::string(i == 0 ? " (" : ", ") + nm;
            }
        if (cnt > 0) sys += ")";
        sys += "\n";
        SDL_free(ids);
    }
    auto ait = cfg().launch_args.find(p.id);
    if (ait != cfg().launch_args.end() && !ait->second.empty())
        sys += "Launch Args:     " + sanitize_text(ait->second, pdir) + "\n";
    auto eit = cfg().launch_env.find(p.id);
    if (eit != cfg().launch_env.end() && !eit->second.empty())
        sys += "Launch Env:      " + sanitize_text(sanitize_env_spec(eit->second), pdir) + "\n";

    if (lr && lr->verdict != RUN_NONE) {
        static const char *kV[] = {"none", "clean", "crash", "known", "fixed", "no_start"};
        sys += "\nLast Run Summary\n----------------\n";
        sys += "Verdict:         " + std::string(kV[SDL_clamp(lr->verdict, 0, 5)]) + "\n";
        sys += "Duration:        " + std::to_string(lr->seconds) + " s\n";
        if (!lr->issue.empty()) sys += "Issue:           " + lr->issue + "\n";
        if (!lr->fixed_in.empty()) sys += "Fixed In:        " + lr->fixed_in + "\n";
        if (!lr->detail.empty()) sys += "Detail:          " + sanitize_text(lr->detail, pdir) + "\n";
    }
    if (in) {
        PerfSummary ps = port_last_perf(p, *in);
        if (ps.valid) {
            sys += "Last Perf:       " + ps.text + " (" + std::to_string(ps.frames) + " frames)\n";
        }
    }
    SDL_SaveFile((stage + "\\system.txt").c_str(), sys.data(), sys.size());

    // 2. settings-diff.txt
    std::string diff = build_settings_diff(port_idx, pdir);
    SDL_SaveFile((stage + "\\settings-diff.txt").c_str(), diff.data(), diff.size());

    // 3. log-tail.txt (last 64 KB, sanitized)
    std::string log_path = (lr && !lr->log.empty()) ? lr->log : "";
    if (log_path.empty() && in) {
        for (const char *cand : {"mastersword_log.txt", "logs\\mkw.log", "logs\\totk.log", "logs\\ff.log",
                                 "logs\\mtf.log", "logs\\dkb.log", "logs\\smbw.log", "logs\\yatmb.log"}) {
            std::string full = in->dir + "\\" + cand;
            if (SDL_GetPathInfo(full.c_str(), nullptr)) { log_path = full; break; }
        }
    }
    std::string ltail = log_path.empty() ? std::string("# No log file found for this port.\n")
                                         : read_tail_bytes(log_path, 65536);
    if (ltail.empty()) ltail = "# Log file was empty or unreadable.\n";
    ltail = sanitize_text(std::move(ltail), pdir);
    SDL_SaveFile((stage + "\\log-tail.txt").c_str(), ltail.data(), ltail.size());

    // Package into %LOCALAPPDATA%\SteelTemple\Sanctum\diagnostics\<port>-diag-YYYYMMDD-HHMMSS.zip
    std::string out_dir = local_dir() + "diagnostics";
    SDL_CreateDirectory(out_dir.c_str());
    time_t now = time(nullptr);
    struct tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    localtime_r(&lt, &now);
#endif
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &lt);
    std::string dst = out_dir + "\\" + std::string(p.id) + "-diag-" + stamp + ".zip";

    int files = 0;
    bool ok = zip_folder(stage, dst, &files);
    remove_tree(stage);
    if (!ok || files <= 0) return {};

    SDL_SetClipboardText(dst.c_str());
    sanctum_log("diag: exported sanitized diagnostics bundle %s (%d files)", dst.c_str(), files);
    return dst;
}

}  // namespace sanctum
