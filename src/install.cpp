#include "install.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>

#include "app.h"
#include "catalog.h"
#include "config.h"
#include "github.h"
#include "launch.h"
#include "net.h"
#include "platform.h"
#include "sound.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace sanctum {

struct Job {
    JobView v;
    Release rel;
    Asset asset;
    std::atomic<bool> cancel{false};
};

static std::mutex g_lock;
static std::vector<std::shared_ptr<Job>> g_jobs;   // every job of this session, in order
static std::atomic<bool> g_worker{false};
static int g_next_id = 1;
struct Finished {
    int port, kind;
    std::string dir, version;
    bool relinked = false;
    bool backed_up_saves = false;
};
static std::vector<Finished> g_finished;
static std::atomic<bool> g_wizard_open{false};
static bool g_stepped_aside = false;

std::string library_dir() {
    if (const char *t = getenv("SANCTUM_LIBRARY")) return t;   // tests: a throwaway folder
    if (!cfg().library_dir.empty()) return cfg().library_dir;
    const char *home = SDL_GetUserFolder(SDL_FOLDER_HOME);
    return std::string(home ? home : "C:\\") + "Games\\SteelTemple";
}

// ---- file helpers (UTF-8 paths)
static std::wstring W(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static bool is_dir(const std::string &p) {
    SDL_PathInfo i;
    return SDL_GetPathInfo(p.c_str(), &i) && i.type == SDL_PATHTYPE_DIRECTORY;
}

static std::vector<std::string> list(const std::string &dir) {
    std::vector<std::string> out;
    int n = 0;
    char **f = SDL_GlobDirectory(dir.c_str(), "*", 0, &n);
    for (int i = 0; f && i < n; ++i) out.push_back(f[i]);
    SDL_free(f);
    return out;
}

void remove_tree(const std::string &p) {
    if (is_dir(p))
        for (auto &n : list(p)) remove_tree(p + "\\" + n);
    SetFileAttributesW(W(p).c_str(), FILE_ATTRIBUTE_NORMAL);
    SDL_RemovePath(p.c_str());
}

// the folder under root holding the port (the zips keep it one folder down, e.g. MKW-THWOMP/)
static std::string find_root(const std::string &root, const Port &p, int depth) {
    if (port_files(p, root)) return root;
    if (depth <= 0) return {};
    for (auto &n : list(root))
        if (is_dir(root + "\\" + n)) {
            std::string r = find_root(root + "\\" + n, p, depth - 1);
            if (!r.empty()) return r;
        }
    return {};
}

// a zipped Setup Wizard (WonderBloom's "...-Setup-Wizard.zip"): the setup program inside it
static std::string find_setup_exe(const std::string &root, int depth) {
    for (auto &n : list(root)) {
        std::string l;
        for (char c : n) l += (char)SDL_tolower((unsigned char)c);
        if (l.size() > 4 && !l.compare(l.size() - 4, 4, ".exe") &&
            (l.find("setup") != std::string::npos || l.find("installer") != std::string::npos))
            return root + "\\" + n;
    }
    if (depth > 0)
        for (auto &n : list(root))
            if (is_dir(root + "\\" + n)) {
                std::string r = find_setup_exe(root + "\\" + n, depth - 1);
                if (!r.empty()) return r;
            }
    return {};
}

static long long free_bytes(const std::string &dir) {
    std::string d = dir;
    while (!d.empty() && !is_dir(d)) d = d.substr(0, d.find_last_of('\\') == std::string::npos ? 0 : d.find_last_of('\\'));
    ULARGE_INTEGER avail;
    if (d.empty() || !GetDiskFreeSpaceExW(W(d).c_str(), &avail, nullptr, nullptr)) return -1;
    return (long long)avail.QuadPart;
}

static void make_dirs(const std::string &p) {
    for (size_t i = 3; i <= p.size(); ++i)
        if (i == p.size() || p[i] == '\\') SDL_CreateDirectory(p.substr(0, i).c_str());
}

static std::wstring quote_arg_w(const std::wstring &s) {
    if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring out = L"\"";
    for (size_t i = 0; i < s.size(); ++i) {
        size_t bs = 0;
        while (i < s.size() && s[i] == L'\\') ++bs, ++i;
        if (i == s.size()) { out.append(bs * 2, L'\\'); break; }
        if (s[i] == L'"') { out.append(bs * 2 + 1, L'\\'); out += L'"'; }
        else { out.append(bs, L'\\'); out += s[i]; }
    }
    out += L'"';
    return out;
}

// Runs a helper process completely hidden (CREATE_NO_WINDOW + NUL stdio + Job Object for clean cancel)
static int run_hidden_wait(const std::vector<std::string> &argv, const std::string &cwd,
                           const std::atomic<bool> &cancel, const std::vector<wchar_t> *env_block = nullptr) {
    if (argv.empty()) return -1;
    std::wstring cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cmd += L' ';
        cmd += quote_arg_w(W(argv[i]));
    }
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = si.hStdOutput = si.hStdError = nul ? nul : INVALID_HANDLE_VALUE;
    std::wstring exe_w = W(argv[0]);
    std::wstring cwd_w = cwd.empty() ? std::wstring() : W(cwd);
    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | (env_block ? CREATE_UNICODE_ENVIRONMENT : 0);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe_w.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags,
                             env_block ? (LPVOID)env_block->data() : nullptr,
                             cwd.empty() ? nullptr : cwd_w.c_str(), &si, &pi);
    if (nul) CloseHandle(nul);
    if (!ok) return -1;
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jli{};
        jli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jli, sizeof jli);
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    bool killed = false;
    while (WaitForSingleObject(pi.hProcess, 80) == WAIT_TIMEOUT) {
        if (cancel) {
            killed = true;
            if (job) TerminateJobObject(job, 1);
            else TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            break;
        }
    }
    DWORD code = 1;
    if (!killed) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    return killed ? -2 : (int)code;
}

// Windows' own tar (bsdtar) unpacks zips headlessly; returns "" or why
std::string unzip(const std::string &zip, const std::string &to, const std::atomic<bool> &cancel) {
    const char *sys = getenv("SystemRoot");
    std::string tar = std::string(sys ? sys : "C:\\Windows") + "\\System32\\tar.exe";
    int code = run_hidden_wait({tar, "-xf", zip, "-C", to}, {}, cancel);
    if (code == -2) return "cancelled";
    if (code == -1) return "cannot run tar";
    return code == 0 ? std::string() : "the package could not be unpacked (tar " + std::to_string(code) + ")";
}

// Lays the files of src over dst. Each replaced file is first renamed <name>.old; on any failure everything done is
// put back. On success the .old files are removed.
std::string overlay(const std::string &src, const std::string &dst) {
    struct Step { std::string to; bool had_old; };
    std::vector<Step> done;
    std::string why;
    std::vector<std::pair<std::string, std::string>> files;   // (from, to)
    std::vector<std::string> stack{""};
    while (!stack.empty()) {
        std::string rel = stack.back();
        stack.pop_back();
        std::string from = rel.empty() ? src : src + "\\" + rel;
        for (auto &n : list(from)) {
            std::string r = rel.empty() ? n : rel + "\\" + n;
            if (is_dir(src + "\\" + r)) stack.push_back(r);
            else files.push_back({src + "\\" + r, dst + "\\" + r});
        }
    }
    for (auto &f : files) {
        std::string dir = f.second.substr(0, f.second.find_last_of('\\'));
        make_dirs(dir);
        bool had = SDL_GetPathInfo(f.second.c_str(), nullptr);
        if (had && !MoveFileExW(W(f.second).c_str(), W(f.second + ".old").c_str(), MOVEFILE_REPLACE_EXISTING)) {
            why = "a file is in use: " + f.second.substr(dst.size() + 1);
            break;
        }
        if (!MoveFileExW(W(f.first).c_str(), W(f.second).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            if (had) MoveFileExW(W(f.second + ".old").c_str(), W(f.second).c_str(), MOVEFILE_REPLACE_EXISTING);
            why = "cannot write " + f.second.substr(dst.size() + 1);
            break;
        }
        done.push_back({f.second, had});
    }
    if (!why.empty()) {   // put everything back as it was
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            if (it->had_old) MoveFileExW(W(it->to + ".old").c_str(), W(it->to).c_str(), MOVEFILE_REPLACE_EXISTING);
            else DeleteFileW(W(it->to).c_str());
        }
        return why;
    }
    for (auto &d : done)
        if (d.had_old) DeleteFileW(W(d.to + ".old").c_str());
    return {};
}

static std::string read_text_file(const std::string &path) {
    size_t n = 0;
    char *d = (char *)SDL_LoadFile(path.c_str(), &n);
    std::string s = d ? std::string(d, n) : std::string();
    SDL_free(d);
    return s;
}

static bool copy_file_overwrite(const std::string &src, const std::string &dst) {
    return CopyFileW(W(src).c_str(), W(dst).c_str(), FALSE) != 0;
}

// Rolling automatic save backup right before applying any port Update.zip (keeps last 5 auto snapshots).
static bool auto_backup_saves(const Port &port, const std::string &dest) {
    if (demo_mode() || !port.id) return false;
    Install probe{};
    probe.dir = dest;
    std::string sdir = port_save_dir(port, probe);
    if (sdir.empty()) return false;
    std::string bbase = config_dir() + "backups";
    std::string bdir = bbase + "\\" + port.id;
    SDL_CreateDirectory(bbase.c_str());
    SDL_CreateDirectory(bdir.c_str());
    time_t now = time(nullptr);
    struct tm lt{};
    localtime_s(&lt, &now);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &lt);
    std::string dst = bdir + "\\auto-pre-update-" + stamp + ".zip";
    int files = 0;
    if (!zip_folder(sdir, dst, &files) || files <= 0) {
        SDL_RemovePath(dst.c_str());
        return false;
    }
    // Prune older auto-pre-update-*.zip beyond 5 rolling copies (never touch manual save-*.zip backups)
    std::vector<std::string> autos;
    for (const std::string &n : list(bdir))
        if (n.rfind("auto-pre-update-", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".zip") == 0)
            autos.push_back(n);
    std::sort(autos.begin(), autos.end());
    while (autos.size() > 5) {
        SDL_RemovePath((bdir + "\\" + autos.front()).c_str());
        autos.erase(autos.begin());
    }
    sanctum_log("install: auto-backed up %d save files to %s", files, dst.c_str());
    return true;
}

static std::string extract_c_string(const std::string &src, const char *sym) {
    size_t at = src.find(sym);
    if (at == std::string::npos) return {};
    size_t q1 = src.find('"', at);
    if (q1 == std::string::npos) return {};
    size_t q2 = src.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    std::string v = src.substr(q1 + 1, q2 - q1 - 1);
    for (char &c : v) if (c == '/') c = '\\';
    return v;
}

// Zero-touch background relink for Universal-NS2 ports whose Update.zip updated runtime/lib<stem>rt.a.
static bool try_auto_relink(const Port &port, const std::string &dest, const std::atomic<bool> &cancel) {
    if (!port.launcher || !port.exe) return false;
    std::string stem = port.exe;
    if (size_t dot = stem.rfind('.'); dot != std::string::npos) stem.resize(dot);
    std::string rt_lib = dest + "\\runtime\\lib" + stem + "rt.a";
    std::string exe_dst = dest + "\\" + port.exe;
    std::string py = dest + "\\python\\python.exe";
    std::string bg = dest + "\\tools\\build_game.py";
    std::string gen_root = dest + "\\work\\generated";
    SDL_PathInfo rt_i{}, ex_i{};
    if (!SDL_GetPathInfo(rt_lib.c_str(), &rt_i) || !SDL_GetPathInfo(exe_dst.c_str(), &ex_i)) return false;
    if (!SDL_GetPathInfo(py.c_str(), nullptr) || !SDL_GetPathInfo(bg.c_str(), nullptr) || !is_dir(gen_root)) return false;

    // Discover generated/<ver> containing layout.json
    std::string ver, gen_dir;
    for (const std::string &n : list(gen_root)) {
        std::string cand = gen_root + "\\" + n;
        if (is_dir(cand) && SDL_GetPathInfo((cand + "\\layout.json").c_str(), nullptr)) {
            ver = n, gen_dir = cand;
            break;
        }
    }
    if (ver.empty()) return false;
    std::string build_out = dest + "\\work\\build\\" + ver;
    make_dirs(build_out);

    std::string mods_c = read_text_file(build_out + "\\modules.c");
    std::string exefs = dest + "\\exefs";
    if (!SDL_GetPathInfo((exefs + "\\main").c_str(), nullptr)) exefs = extract_c_string(mods_c, "rt_default_exefs");
    std::string romfs = dest + "\\romfs_packed";
    if (!SDL_GetPathInfo((romfs + "\\romfs.bin").c_str(), nullptr)) romfs = extract_c_string(mods_c, "rt_default_romfs");
    if (exefs.empty() || !SDL_GetPathInfo((exefs + "\\main").c_str(), nullptr)) return false;
    if (romfs.empty() || !SDL_GetPathInfo((romfs + "\\romfs.bin").c_str(), nullptr)) return false;

    std::string built_exe = build_out + "\\" + port.exe;
    SDL_RemovePath(built_exe.c_str());

    std::vector<std::string> argv = {
        py, "-B", bg,
        "--version", ver,
        "--gen", gen_dir,
        "--out", build_out,
        "--exefs", exefs,
        "--romfs", romfs,
        "--profile", dest + "\\profile",
        "--runtime", dest + "\\runtime",
        "--runtime-lib", rt_lib,
    };
    std::string exp = dest + "\\tools\\data\\expected-" + ver + ".json";
    if (SDL_GetPathInfo(exp.c_str(), nullptr)) { argv.push_back("--expected"); argv.push_back(exp); }
    if (is_dir(dest + "\\llvm")) {
        argv.push_back("--toolchain"); argv.push_back(dest + "\\llvm");
        argv.push_back("--deps"); argv.push_back(dest + "\\runtime\\deps");
    }
    std::string ninja = dest + "\\bin\\ninja.exe";
    if (SDL_GetPathInfo(ninja.c_str(), nullptr)) { argv.push_back("--ninja"); argv.push_back(ninja); }

    // Build isolated UTF-16 environment block (strips ambient Python vars, prepends bundled python/llvm/bin to PATH)
    std::vector<wchar_t> env_block;
    if (wchar_t *raw = GetEnvironmentStringsW()) {
        std::wstring path_prefix = W(dest + "\\python;" + dest + "\\llvm\\bin;" + dest + "\\bin;");
        bool saw_path = false;
        for (const wchar_t *p = raw; *p; p += wcslen(p) + 1) {
            std::wstring entry(p);
            size_t eq = entry.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring key = entry.substr(0, eq);
            std::wstring up = key;
            for (wchar_t &c : up) if (c >= L'a' && c <= L'z') c -= L'a' - L'A';
            if (up == L"PYTHONHOME" || up == L"PYTHONPATH" || up == L"PYTHONSTARTUP" ||
                up == L"PYTHONUSERBASE" || up == L"VIRTUAL_ENV") continue;
            if (up == L"PATH") {
                entry = key + L"=" + path_prefix + entry.substr(eq + 1);
                saw_path = true;
            }
            env_block.insert(env_block.end(), entry.begin(), entry.end());
            env_block.push_back(L'\0');
        }
        FreeEnvironmentStringsW(raw);
        if (!saw_path) {
            std::wstring pe = L"PATH=" + path_prefix;
            env_block.insert(env_block.end(), pe.begin(), pe.end());
            env_block.push_back(L'\0');
        }
        env_block.push_back(L'\0');
    }

    int code = run_hidden_wait(argv, dest, cancel, env_block.empty() ? nullptr : &env_block);
    if (code != 0 || !SDL_GetPathInfo(built_exe.c_str(), nullptr)) {
        sanctum_log("relink: %s build_game failed (exit %d)", port.id, code);
        return false;
    }
    // Atomically swap updated game binary & companion runtime files into dest
    std::string tmp_exe = exe_dst + ".sanctum-new";
    if (!copy_file_overwrite(built_exe, tmp_exe)) return false;
    if (!MoveFileExW(W(tmp_exe).c_str(), W(exe_dst).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        SDL_RemovePath(tmp_exe.c_str());
        return false;
    }
    for (const char *f : {"SDL3.dll", "libopus-0.dll", "romfs-index.bin"})
        if (SDL_GetPathInfo((build_out + "\\" + f).c_str(), nullptr))
            copy_file_overwrite(build_out + "\\" + f, dest + "\\" + f);
    for (const char *f : {"ns2_xlate.exe", "ns2_xlate.id"})
        if (SDL_GetPathInfo((dest + "\\bin\\" + f).c_str(), nullptr))
            copy_file_overwrite(dest + "\\bin\\" + f, dest + "\\" + f);

    // Refresh .<stem>_runtime_stamp so the port's own launcher also sees the binary is up to date
    std::string stamp_ver = read_text_file(dest + "\\VERSION");
    stamp_ver = stamp_ver.substr(0, stamp_ver.find_first_of("\r\n"));
    std::atomic<bool> nc{false};
    std::string rt_sha = sha256_file(rt_lib, nc);
    std::string bg_sha = sha256_file(bg, nc);
    auto strip_pref = [](std::string h) { return h.rfind("sha256:", 0) == 0 ? h.substr(7) : h; };
    rt_sha = strip_pref(rt_sha);
    bg_sha = strip_pref(bg_sha);
    std::string xid = read_text_file(dest + "\\bin\\ns2_xlate.id");
    while (!xid.empty() && (xid.back() == '\r' || xid.back() == '\n' || xid.back() == ' ')) xid.pop_back();
    SDL_PathInfo ex_new{};
    if (!stamp_ver.empty() && rt_sha.size() >= 16 && bg_sha.size() >= 12 && SDL_GetPathInfo(exe_dst.c_str(), &ex_new)) {
        std::string st = stamp_ver + "|" + std::to_string(rt_i.size) + "|" + rt_sha.substr(0, 16) + "|" +
                         bg_sha.substr(0, 12) + "|" + xid + "|exe:" + std::to_string(ex_new.size) + "\n";
        std::string sp = build_out + "\\." + stem + "_runtime_stamp";
        SDL_SaveFile(sp.c_str(), st.data(), st.size());
    }
    sanctum_log("relink: %s -> %s succeeded", port.id, exe_dst.c_str());
    return true;
}

static void set_phase(Job &j, int ph, const std::string &err = {}) {
    std::lock_guard<std::mutex> l(g_lock);
    j.v.phase = ph;
    if (ph < PH_DONE) j.v.reached = ph;
    if (!err.empty()) j.v.error = err;
}

static void run(Job &j) {
    const Port &port = ports()[j.v.port];
    std::string dl_dir = local_dir() + "downloads\\";
    SDL_CreateDirectory(dl_dir.c_str());
    std::string file = dl_dir + j.asset.name;

    // ---- space: the package, and (for zips) about as much again unpacked
    long long need = j.asset.size * (j.v.kind == JOB_WIZARD ? 1 : 3);
    long long have = free_bytes(j.v.kind == JOB_WIZARD ? dl_dir : library_dir());
    if (have >= 0 && have < need) { set_phase(j, PH_FAILED, "not enough free space (" + mb(need) + " needed)"); return; }

    // ---- download
    set_phase(j, PH_DOWNLOAD);
    Uint64 t0 = SDL_GetTicksNS();
    long long start_bytes = -1;
    std::string err = http_download(j.asset.url, file, j.asset.size, [&](long long d, long long t) {
        std::lock_guard<std::mutex> l(g_lock);
        if (start_bytes < 0) start_bytes = d;
        j.v.done = d, j.v.total = t ? t : j.asset.size;
        double secs = (SDL_GetTicksNS() - t0) / 1e9;
        if (secs > 0.5) j.v.speed = (d - start_bytes) / secs;
    }, j.cancel);
    if (j.cancel) { set_phase(j, PH_CANCELLED); return; }   // the partial file stays: a new try resumes it
    if (!err.empty()) { set_phase(j, PH_FAILED, err); return; }

    // ---- verify
    set_phase(j, PH_VERIFY);
    if (!j.asset.digest.empty()) {
        std::string got = sha256_file(file, j.cancel);
        if (j.cancel) { set_phase(j, PH_CANCELLED); return; }
        if (got != j.asset.digest) {
            SDL_RemovePath(file.c_str());   // damaged: start over next time
            set_phase(j, PH_FAILED, "the download did not match GitHub's checksum; try again");
            return;
        }
    }

    // ---- Setup Wizard: the port's own setup does the rest
    if (j.v.kind == JOB_WIZARD) {
        std::string setup = file, unpacked;
        if (setup.size() > 4 && !SDL_strcasecmp(setup.c_str() + setup.size() - 4, ".zip")) {   // a zipped setup
            unpacked = dl_dir + "setup-" + port.id + "-" + std::to_string(j.v.id);
            remove_tree(unpacked);
            SDL_CreateDirectory(unpacked.c_str());
            err = unzip(file, unpacked, j.cancel);
            setup = err.empty() ? find_setup_exe(unpacked, 2) : "";
            if (setup.empty()) {
                remove_tree(unpacked);
                set_phase(j, j.cancel ? PH_CANCELLED : PH_FAILED, err.empty() ? "no setup program in the download" : err);
                return;
            }
        }
        set_phase(j, PH_WIZARD);
        g_wizard_open = true;
        const char *args[] = {setup.c_str(), nullptr};
        SDL_Process *p = SDL_CreateProcess(args, false);
        if (!p) {
            g_wizard_open = false;
            if (!unpacked.empty()) remove_tree(unpacked);
            set_phase(j, PH_FAILED, std::string("cannot start the setup: ") + SDL_GetError());
            return;
        }
        int code = 0;
        SDL_WaitProcess(p, true, &code);
        SDL_DestroyProcess(p);
        g_wizard_open = false;
        if (!unpacked.empty()) remove_tree(unpacked);
        std::lock_guard<std::mutex> l(g_lock);
        g_finished.push_back({j.v.port, JOB_WIZARD, {}, j.rel.tag});
        j.v.phase = PH_DONE;
        return;
    }

    // ---- unpack beside the library (same drive: the final move is instant)
    set_phase(j, PH_INSTALL);
    std::string lib = library_dir();
    make_dirs(lib);
    std::string stage = lib + "\\.sanctum-" + port.id + "-" + std::to_string(j.v.id);
    remove_tree(stage);
    SDL_CreateDirectory(stage.c_str());
    err = unzip(file, stage, j.cancel);
    if (!err.empty()) { remove_tree(stage); set_phase(j, j.cancel ? PH_CANCELLED : PH_FAILED, err); return; }
    std::string root = find_root(stage, port, 3);
    if (root.empty()) {
        // Partial Update.zip archives without the main exe may still wrap files in a single top-level folder (e.g. PortName/)
        root = stage;
        for (int depth = 0; depth < 2; ++depth) {
            auto entries = list(root);
            if (entries.size() == 1 && is_dir(root + "\\" + entries[0])) root += "\\" + entries[0];
            else break;
        }
    }

    std::string dest;
    bool backed_up_saves = false;
    bool relinked = false;
    if (j.v.kind == JOB_UPDATE) {
        dest = j.v.dest;
        backed_up_saves = auto_backup_saves(port, dest);
        err = overlay(root, dest);
    } else {
        dest = lib + "\\" + port.name;
        for (int n = 2; SDL_GetPathInfo(dest.c_str(), nullptr); ++n) dest = lib + "\\" + port.name + " (" + std::to_string(n) + ")";
        if (find_root(stage, port, 3).empty()) err = "this package does not look like " + std::string(port.name);
        else if (!MoveFileExW(W(root).c_str(), W(dest).c_str(), 0)) err = overlay(root, dest);   // a move across drives: copy
    }
    remove_tree(stage);
    if (!err.empty()) { set_phase(j, PH_FAILED, err); return; }
    SDL_RemovePath(file.c_str());   // installed: the package is not needed any more

    // Zero-touch relink for Universal-NS2 ports when Update.zip updated runtime/lib<stem>rt.a
    if (j.v.kind == JOB_UPDATE && port.launcher && port.exe) {
        std::string stem = port.exe;
        if (size_t dot = stem.rfind('.'); dot != std::string::npos) stem.resize(dot);
        if (SDL_GetPathInfo((dest + "\\runtime\\lib" + stem + "rt.a").c_str(), nullptr) &&
            SDL_GetPathInfo((dest + "\\" + port.exe).c_str(), nullptr) &&
            is_dir(dest + "\\work\\generated")) {
            set_phase(j, PH_RELINK);
            relinked = try_auto_relink(port, dest, j.cancel);
        }
    }

    // Write authoritative .sanctum-version stamp AFTER relink so hotfix/no-VERSION/un-relinked updates never loop
    if (!j.rel.tag.empty()) {
        std::string sv = j.rel.tag + "\n";
        std::string sp = dest + "\\.sanctum-version";
        std::string stmp = sp + ".tmp";
        if (SDL_SaveFile(stmp.c_str(), sv.data(), sv.size())) SDL_RenamePath(stmp.c_str(), sp.c_str());
    }

    std::lock_guard<std::mutex> l(g_lock);
    g_finished.push_back({j.v.port, j.v.kind, dest, j.rel.tag, relinked, backed_up_saves});
    j.v.dest = dest;
    j.v.relinked = relinked;
    j.v.backed_up_saves = backed_up_saves;
    j.v.phase = PH_DONE;
}

static int worker(void *) {
    for (;;) {
        std::shared_ptr<Job> next;
        {
            std::lock_guard<std::mutex> l(g_lock);
            for (auto &j : g_jobs)
                if (j->v.phase == PH_QUEUED) { next = j; break; }
            if (!next) { g_worker = false; return 0; }
        }
        run(*next);
    }
}

int install_enqueue(int port, const Release &r, const Asset &a, JobKind kind, const std::string &dest) {
    auto j = std::make_shared<Job>();
    j->rel = r, j->asset = a;
    {
        std::lock_guard<std::mutex> l(g_lock);
        j->v.id = g_next_id++;
        j->v.port = port, j->v.kind = kind, j->v.phase = PH_QUEUED;
        j->v.tag = r.tag, j->v.file = a.name, j->v.dest = dest, j->v.total = a.size;
        g_jobs.push_back(j);
    }
    if (!g_worker.exchange(true)) {
        SDL_Thread *t = SDL_CreateThread(worker, "install", nullptr);
        if (t) SDL_DetachThread(t);
        else g_worker = false;
    }
    return j->v.id;
}

std::vector<JobView> install_jobs() {
    std::lock_guard<std::mutex> l(g_lock);
    std::vector<JobView> v;
    for (auto &j : g_jobs) v.push_back(j->v);
    return v;
}

void install_cancel(int id) {
    std::lock_guard<std::mutex> l(g_lock);
    for (auto &j : g_jobs)
        if (j->v.id == id) {
            if (j->v.phase == PH_QUEUED) j->v.phase = PH_CANCELLED;
            else if (j->v.phase < PH_WIZARD) j->cancel = true;
        }
}

void install_clear_finished() {
    std::lock_guard<std::mutex> l(g_lock);
    std::vector<std::shared_ptr<Job>> keep;
    for (auto &j : g_jobs)
        if (j->v.phase < PH_DONE) keep.push_back(j);
    g_jobs.swap(keep);
}

bool install_busy() {
    std::lock_guard<std::mutex> l(g_lock);
    for (auto &j : g_jobs)
        if (j->v.phase < PH_DONE) return true;
    return false;
}

bool install_queued(int port) {
    std::lock_guard<std::mutex> l(g_lock);
    for (auto &j : g_jobs)
        if (j->v.port == port && j->v.phase < PH_DONE) return true;
    return false;
}

static void registry_version_if_matches(const char *key, const std::string &dir, const std::string &tag) {
    if (!key || !*key || tag.empty()) return;
    std::wstring sub = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + W(key);
    wchar_t loc[1024]{};
    DWORD sz = sizeof loc;
    if (RegGetValueW(HKEY_CURRENT_USER, sub.c_str(), L"InstallLocation", RRF_RT_REG_SZ, nullptr, loc, &sz) != ERROR_SUCCESS)
        return;
    std::wstring wl(loc);
    while (!wl.empty() && (wl.back() == L'\\' || wl.back() == L'/')) wl.pop_back();
    std::wstring wd = W(dir);
    while (!wd.empty() && (wd.back() == L'\\' || wd.back() == L'/')) wd.pop_back();
    if (_wcsicmp(wl.c_str(), wd.c_str()) != 0) return;
    std::wstring v = W(tag);
    RegSetKeyValueW(HKEY_CURRENT_USER, sub.c_str(), L"DisplayVersion", REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

void install_poll(SDL_Window *w) {
    // a Setup Wizard is open: Sanctum steps aside so the setup is in front, and comes back after
    if (g_wizard_open && !g_stepped_aside) { SDL_MinimizeWindow(w); g_stepped_aside = true; }
    if (!g_wizard_open && g_stepped_aside) { SDL_RestoreWindow(w); SDL_RaiseWindow(w); g_stepped_aside = false; }
    std::vector<Finished> done;
    {
        std::lock_guard<std::mutex> l(g_lock);
        done.swap(g_finished);
    }
    if (!done.empty()) sound_play(SFX_NOTIFY);
    for (auto &f : done) {
        Port &p = ports()[f.port];
        if (f.kind == JOB_WIZARD) {
            catalog_invalidate_version(f.port);
            scan_start();          // the setup chose the folder: find it
            version_probe_start(); // re-probe updated exe even if installed over an existing folder
            toast(std::string(p.name) + ": setup finished, looking for it on this PC", 3.5);
            continue;
        }
        if (f.kind == JOB_UPDATE)
            registry_version_if_matches(p.uninstall_key, f.dir, f.version);
        catalog_installed(f.port, f.dir, f.version);
        bool needs_relink = false;
        if (f.kind == JOB_UPDATE && !f.relinked && p.launcher && p.exe) {
            std::string stem = p.exe;
            if (size_t dot = stem.rfind('.'); dot != std::string::npos) stem.resize(dot);
            SDL_PathInfo rt{}, ex{};
            if (SDL_GetPathInfo((f.dir + "\\runtime\\lib" + stem + "rt.a").c_str(), &rt) &&
                SDL_GetPathInfo((f.dir + "\\" + p.exe).c_str(), &ex) && rt.modify_time > ex.modify_time)
                needs_relink = true;
        }
        if (f.relinked)
            toast(std::string(p.name) + " updated & relinked to " + f.version + " (saves backed up)", 4.5);
        else if (needs_relink)
            toast(std::string(p.name) + " updated to " + f.version + " — open Port's own launcher once to relink " + p.exe, 5.0);
        else
            toast(std::string(p.name) + (f.kind == JOB_UPDATE ? " updated to " : " installed: ") + f.version, 3.5);
        version_probe_start();
    }
}

}  // namespace sanctum
