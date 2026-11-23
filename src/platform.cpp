#include "platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

namespace sanctum {

#ifdef _WIN32
static std::wstring to_wide(const std::string &s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

static std::string to_utf8(const std::wstring &w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring self_exe_path() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    return n > 0 ? std::wstring(buf, n) : std::wstring();
}

static bool create_shortcut_in(int csidl) {
    std::wstring exe = self_exe_path();
    if (exe.empty()) return false;
    wchar_t dir[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, dir))) return false;
    std::wstring lnk = std::wstring(dir) + L"\\SteelTemple Sanctum.lnk";
    size_t slash = exe.find_last_of(L"\\/");
    std::wstring workdir = slash == std::wstring::npos ? L"" : exe.substr(0, slash);

    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool uninit = SUCCEEDED(co);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE) return false;

    bool ok = false;
    IShellLinkW *sl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void **)&sl)) && sl) {
        sl->SetPath(exe.c_str());
        if (!workdir.empty()) sl->SetWorkingDirectory(workdir.c_str());
        sl->SetDescription(L"SteelTemple Sanctum");
        sl->SetIconLocation(exe.c_str(), 0);
        IPersistFile *pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void **)&pf)) && pf) {
            ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE));
            pf->Release();
        }
        sl->Release();
    }
    if (uninit) CoUninitialize();
    return ok;
}

bool shortcut_create_desktop() { return create_shortcut_in(CSIDL_DESKTOPDIRECTORY); }
bool shortcut_create_startmenu() { return create_shortcut_in(CSIDL_PROGRAMS); }

static const wchar_t *kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t *kRunName = L"SteelTempleSanctum";

bool startup_is_enabled() {
    wchar_t buf[MAX_PATH * 4];
    DWORD sz = sizeof buf;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS && buf[0] != 0;
}

bool startup_set_enabled(bool on) {
    if (!on) {
        LONG r = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunName);
        return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
    }
    std::wstring exe = self_exe_path();
    if (exe.empty()) return false;
    std::wstring val = L"\"" + exe + L"\"";
    return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunName, REG_SZ, val.c_str(),
                           (DWORD)((val.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool recycle_path(const std::string &path) {
    std::wstring w = to_wide(path);
    for (wchar_t &c : w) if (c == L'/') c = L'\\';
    while (w.size() > 3 && w.back() == L'\\') w.pop_back();
    if (w.size() < 4) return false;   // refuse drive roots like "C:\"
    w.push_back(L'\0');
    w.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = w.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

static void walk_size(const std::wstring &dir, long long &bytes, int &files, int depth) {
    if (depth <= 0) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L".."))
                walk_size(dir + L"\\" + fd.cFileName, bytes, files, depth - 1);
        } else {
            bytes += ((long long)fd.nFileSizeHigh << 32) | (long long)fd.nFileSizeLow;
            ++files;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

long long folder_size_bytes(const std::string &dir, int *file_count) {
    std::wstring w = to_wide(dir);
    for (wchar_t &c : w) if (c == L'/') c = L'\\';
    while (w.size() > 3 && w.back() == L'\\') w.pop_back();
    long long bytes = 0;
    int files = 0;
    if (!w.empty()) walk_size(w, bytes, files, 24);
    if (file_count) *file_count = files;
    return bytes;
}

std::string pick_open_file(SDL_Window *win, const char *title, const char *filter_name, const char *filter_spec) {
    bool was_fs = win && (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN);
    if (was_fs) SDL_SetWindowFullscreen(win, false);
    if (!SDL_CursorVisible()) SDL_ShowCursor();

    HWND owner = win ? (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(win), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr) : nullptr;
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool uninit = SUCCEEDED(co);
    std::string result;
    if (SUCCEEDED(co) || co == RPC_E_CHANGED_MODE) {
        IFileOpenDialog *dlg = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, (void **)&dlg)) && dlg) {
            DWORD opts = 0;
            if (SUCCEEDED(dlg->GetOptions(&opts)))
                dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
            std::wstring wtitle = to_wide(title ? title : "Choose file");
            dlg->SetTitle(wtitle.c_str());
            std::wstring wfname = to_wide(filter_name ? filter_name : "Files");
            std::wstring wfspec = to_wide(filter_spec ? filter_spec : "*.*");
            COMDLG_FILTERSPEC specs[2] = {
                {wfname.c_str(), wfspec.c_str()},
                {L"All files", L"*.*"},
            };
            dlg->SetFileTypes(2, specs);
            dlg->SetFileTypeIndex(1);
            if (SUCCEEDED(dlg->Show(owner))) {
                IShellItem *item = nullptr;
                if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                    PWSTR psz = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz) {
                        result = to_utf8(psz);
                        CoTaskMemFree(psz);
                    }
                    item->Release();
                }
            }
            dlg->Release();
        }
        if (uninit) CoUninitialize();
    }
    if (was_fs && win) SDL_SetWindowFullscreen(win, true);
    if (win) SDL_RaiseWindow(win);
    return result;
}

// ---- pure C++ ZIP archiver (store method with CRC-32 + UTF-8 flag)
static uint32_t crc32_bytes(const uint8_t *data, size_t len) {
    static uint32_t tbl[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tbl[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = tbl[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void put16(std::vector<uint8_t> &b, uint16_t v) { b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF); }
static void put32(std::vector<uint8_t> &b, uint32_t v) {
    b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF); b.push_back((v >> 16) & 0xFF); b.push_back((v >> 24) & 0xFF);
}

static void collect_files_zip(const std::wstring &root, const std::wstring &rel,
                              std::vector<std::pair<std::wstring, std::string>> &out, int depth) {
    if (depth <= 0) return;
    std::wstring dir = rel.empty() ? root : root + L"\\" + rel;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::wstring sub = rel.empty() ? std::wstring(fd.cFileName) : rel + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            collect_files_zip(root, sub, out, depth - 1);
        } else {
            std::string entry = to_utf8(sub);
            for (char &c : entry) if (c == '\\') c = '/';
            out.push_back({root + L"\\" + sub, entry});
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool zip_folder(const std::string &src_dir, const std::string &dst_zip, int *files_written) {
    std::wstring wsrc = to_wide(src_dir);
    for (wchar_t &c : wsrc) if (c == L'/') c = L'\\';
    while (wsrc.size() > 3 && wsrc.back() == L'\\') wsrc.pop_back();
    std::vector<std::pair<std::wstring, std::string>> list;
    collect_files_zip(wsrc, L"", list, 20);
    if (files_written) *files_written = 0;
    if (list.empty()) return false;

    std::string tmp = dst_zip + ".tmp";
    SDL_IOStream *io = SDL_IOFromFile(tmp.c_str(), "wb");
    if (!io) return false;

    struct CdEntry {
        std::string name;
        uint32_t crc, size, offset;
    };
    std::vector<CdEntry> cd;
    uint32_t offset = 0;
    bool ok = true;
    int written = 0;

    for (const auto &it : list) {
        size_t sz = 0;
        std::string utf8_full = to_utf8(it.first);
        void *data = SDL_LoadFile(utf8_full.c_str(), &sz);
        if (!data && sz > 0) continue;
        if (sz > 0x7FFFFFFFu) { SDL_free(data); continue; }
        uint32_t crc = data ? crc32_bytes((const uint8_t *)data, sz) : 0;
        uint32_t u32sz = (uint32_t)sz;

        std::vector<uint8_t> lh;
        lh.reserve(30 + it.second.size());
        put32(lh, 0x04034B50u);  // local file header sig
        put16(lh, 20);           // version needed 2.0
        put16(lh, 0x0800);       // UTF-8 filename flag
        put16(lh, 0);            // method 0 (store)
        put16(lh, 0); put16(lh, 0x5421);  // mod time/date
        put32(lh, crc);
        put32(lh, u32sz);
        put32(lh, u32sz);
        put16(lh, (uint16_t)it.second.size());
        put16(lh, 0);
        lh.insert(lh.end(), it.second.begin(), it.second.end());

        if (SDL_WriteIO(io, lh.data(), lh.size()) != lh.size() ||
            (sz > 0 && SDL_WriteIO(io, data, sz) != sz)) {
            SDL_free(data);
            ok = false;
            break;
        }
        SDL_free(data);
        cd.push_back({it.second, crc, u32sz, offset});
        offset += (uint32_t)lh.size() + u32sz;
        ++written;
    }

    if (ok && written > 0) {
        uint32_t cd_start = offset;
        std::vector<uint8_t> cbuf;
        for (const auto &e : cd) {
            put32(cbuf, 0x02014B50u);  // central dir header sig
            put16(cbuf, 20); put16(cbuf, 20);
            put16(cbuf, 0x0800);       // UTF-8
            put16(cbuf, 0);            // store
            put16(cbuf, 0); put16(cbuf, 0x5421);
            put32(cbuf, e.crc);
            put32(cbuf, e.size);
            put32(cbuf, e.size);
            put16(cbuf, (uint16_t)e.name.size());
            put16(cbuf, 0); put16(cbuf, 0); put16(cbuf, 0); put16(cbuf, 0);
            put32(cbuf, 0);
            put32(cbuf, e.offset);
            cbuf.insert(cbuf.end(), e.name.begin(), e.name.end());
        }
        uint32_t cd_size = (uint32_t)cbuf.size();
        put32(cbuf, 0x06054B50u);      // EOCD sig
        put16(cbuf, 0); put16(cbuf, 0);
        put16(cbuf, (uint16_t)cd.size());
        put16(cbuf, (uint16_t)cd.size());
        put32(cbuf, cd_size);
        put32(cbuf, cd_start);
        put16(cbuf, 0);
        if (SDL_WriteIO(io, cbuf.data(), cbuf.size()) != cbuf.size()) ok = false;
    }
    SDL_CloseIO(io);
    if (!ok || written == 0) {
        SDL_RemovePath(tmp.c_str());
        return false;
    }
    if (!SDL_RenamePath(tmp.c_str(), dst_zip.c_str())) {
        SDL_RemovePath(tmp.c_str());
        return false;
    }
    if (files_written) *files_written = written;
    return true;
}

static std::string reg_sz(HKEY root, const wchar_t *sub, const wchar_t *name) {
    wchar_t buf[512];
    DWORD sz = sizeof buf;
    if (RegGetValueW(root, sub, name, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return {};
    std::string s = to_utf8(buf);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

const SystemDiag &system_diagnostics() {
    static SystemDiag d;
    static bool loaded = false;
    if (loaded) return d;
    loaded = true;

    // CPU
    d.cpu = reg_sz(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    if (d.cpu.empty()) d.cpu = "Unknown CPU";

    // RAM
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms))
        d.ram_gb = (int)((ms.ullTotalPhys + (512ULL << 20)) >> 30);

    // Windows OS
    std::string prod = reg_sz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
    std::string build = reg_sz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber");
    std::string disp = reg_sz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion");
    int bnum = atoi(build.c_str());
    if (bnum >= 22000 && prod.find("Windows 10") != std::string::npos)
        prod.replace(prod.find("Windows 10"), 10, "Windows 11");
    d.os = prod.empty() ? "Windows" : prod;
    if (!disp.empty()) d.os += " " + disp;
    if (!build.empty()) d.os += " (Build " + build + ")";

    // Primary GPU
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof dd;
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
            d.gpu = to_utf8(dd.DeviceString);
            break;
        }
    }
    if (d.gpu.empty()) d.gpu = "Unknown GPU";
    return d;
}

std::vector<DriveInfo> list_drives() {
    std::vector<DriveInfo> out;
    DWORD old_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_mode);
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
        UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        ULARGE_INTEGER avail{}, total{}, free_total{};
        if (!GetDiskFreeSpaceExW(root, &avail, &total, &free_total) || total.QuadPart == 0) continue;
        DriveInfo di;
        di.letter = (char)('A' + i);
        di.total_bytes = (long long)total.QuadPart;
        di.free_bytes = (long long)avail.QuadPart;
        di.removable = (type == DRIVE_REMOVABLE);
        wchar_t vol[128]{}, fs[32]{};
        if (GetVolumeInformationW(root, vol, 128, nullptr, nullptr, nullptr, fs, 32)) {
            di.label = to_utf8(vol);
            di.fs = to_utf8(fs);
        }
        if (di.label.empty()) di.label = di.removable ? "Removable Drive" : "Local Disk";
        out.push_back(std::move(di));
    }
    SetThreadErrorMode(old_mode, nullptr);
    return out;
}

#else
bool shortcut_create_desktop() { return false; }
bool shortcut_create_startmenu() { return false; }
bool startup_is_enabled() { return false; }
bool startup_set_enabled(bool) { return false; }
bool recycle_path(const std::string &) { return false; }
long long folder_size_bytes(const std::string &, int *file_count) { if (file_count) *file_count = 0; return 0; }
std::string pick_open_file(SDL_Window *, const char *, const char *, const char *) { return {}; }
bool zip_folder(const std::string &, const std::string &, int *files_written) { if (files_written) *files_written = 0; return false; }
const SystemDiag &system_diagnostics() { static SystemDiag d{"Unknown", "Unknown", "Unknown", 0}; return d; }
std::vector<DriveInfo> list_drives() { return {}; }
#endif

}  // namespace sanctum
