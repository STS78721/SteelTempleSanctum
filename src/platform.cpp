#include "platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <propsys.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wincodec.h>
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

    // Primary GPU & driver version (from Display class registry)
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof dd;
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
            d.gpu = to_utf8(dd.DeviceString);
            break;
        }
    }
    if (d.gpu.empty()) d.gpu = "Unknown GPU";
    for (int idx = 0; idx < 8; ++idx) {
        wchar_t sub[128];
        swprintf(sub, 128, L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\%04d", idx);
        std::string desc = reg_sz(HKEY_LOCAL_MACHINE, sub, L"DriverDesc");
        std::string ver = reg_sz(HKEY_LOCAL_MACHINE, sub, L"DriverVersion");
        if (!desc.empty() && !ver.empty()) {
            if (desc == d.gpu || d.gpu_driver.empty()) d.gpu_driver = ver;
            if (desc == d.gpu) break;
        }
    }
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

static const GUID kClsidMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID kIidIMMDeviceEnumerator  = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID kIidIAudioEndpointVolume = {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};

template <typename Fn>
static bool with_endpoint_volume(Fn &&fn) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool uninit = SUCCEEDED(co);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE) return false;
    bool ok = false;
    IMMDeviceEnumerator *en = nullptr;
    if (SUCCEEDED(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, kIidIMMDeviceEnumerator, (void **)&en)) && en) {
        IMMDevice *dev = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && dev) {
            IAudioEndpointVolume *vol = nullptr;
            if (SUCCEEDED(dev->Activate(kIidIAudioEndpointVolume, CLSCTX_ALL, nullptr, (void **)&vol)) && vol) {
                ok = fn(vol);
                vol->Release();
            }
            dev->Release();
        }
        en->Release();
    }
    if (uninit) CoUninitialize();
    return ok;
}

bool system_volume_get(int *pct_out, bool *muted_out) {
    return with_endpoint_volume([&](IAudioEndpointVolume *vol) {
        float scalar = 0.0f;
        BOOL muted = FALSE;
        if (FAILED(vol->GetMasterVolumeLevelScalar(&scalar))) return false;
        vol->GetMute(&muted);
        if (pct_out) *pct_out = (int)(scalar * 100.0f + 0.5f);
        if (muted_out) *muted_out = muted != FALSE;
        return true;
    });
}

bool system_volume_set(int pct, bool mute) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return with_endpoint_volume([&](IAudioEndpointVolume *vol) {
        bool ok1 = SUCCEEDED(vol->SetMasterVolumeLevelScalar(pct / 100.0f, nullptr));
        bool ok2 = SUCCEEDED(vol->SetMute(mute ? TRUE : FALSE, nullptr));
        return ok1 && ok2;
    });
}

// ---- Windows audio output enumeration & live default playback endpoint switching (zero admin)
static const PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};
static const GUID kClsidPolicyConfigClient = {0x870AF99C, 0x171D, 0x4F9E, {0xAF, 0x0D, 0xE6, 0x3D, 0xF4, 0x0C, 0x2B, 0xC9}};
static const GUID kIidPolicyConfig         = {0xF8679F50, 0x850A, 0x41CF, {0x9C, 0x72, 0x43, 0x0F, 0x29, 0x02, 0x90, 0xC8}};
static const GUID kIidPolicyConfigVista    = {0x568B9108, 0x44BF, 0x40B4, {0x90, 0x06, 0x86, 0xAF, 0xE5, 0xB5, 0xA6, 0x20}};

struct IPolicyConfigMinimal : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId, ERole eRole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

struct IPolicyConfigVistaMinimal : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId, ERole eRole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

std::vector<AudioOutput> system_audio_outputs() {
    std::vector<AudioOutput> out;
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool uninit = SUCCEEDED(co);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE) return out;

    IMMDeviceEnumerator *en = nullptr;
    if (SUCCEEDED(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, kIidIMMDeviceEnumerator, (void **)&en)) && en) {
        std::wstring def_id;
        IMMDevice *def_dev = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def_dev)) && def_dev) {
            LPWSTR wid = nullptr;
            if (SUCCEEDED(def_dev->GetId(&wid)) && wid) { def_id = wid; CoTaskMemFree(wid); }
            def_dev->Release();
        }
        IMMDeviceCollection *coll = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) && coll) {
            UINT count = 0;
            coll->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice *dev = nullptr;
                if (FAILED(coll->Item(i, &dev)) || !dev) continue;
                LPWSTR wid = nullptr;
                if (SUCCEEDED(dev->GetId(&wid)) && wid) {
                    AudioOutput ao;
                    ao.id = to_utf8(wid);
                    ao.active = (!def_id.empty() && def_id == wid);
                    IPropertyStore *ps = nullptr;
                    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                        PROPVARIANT pv;
                        PropVariantInit(&pv);
                        if (SUCCEEDED(ps->GetValue(kPkeyDeviceFriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
                            ao.name = to_utf8(pv.pwszVal);
                        PropVariantClear(&pv);
                        ps->Release();
                    }
                    if (ao.name.empty()) ao.name = "Audio Device " + std::to_string(i + 1);
                    out.push_back(std::move(ao));
                    CoTaskMemFree(wid);
                }
                dev->Release();
            }
            coll->Release();
        }
        en->Release();
    }
    if (uninit) CoUninitialize();
    return out;
}

bool system_audio_output_set(const std::string &device_id) {
    if (device_id.empty()) return false;
    std::wstring wid = to_wide(device_id);
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool uninit = SUCCEEDED(co);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE) return false;

    bool ok = false;
    IPolicyConfigMinimal *pc = nullptr;
    if (SUCCEEDED(CoCreateInstance(kClsidPolicyConfigClient, nullptr, CLSCTX_ALL, kIidPolicyConfig, (void **)&pc)) && pc) {
        HRESULT hr1 = pc->SetDefaultEndpoint(wid.c_str(), eConsole);
        HRESULT hr2 = pc->SetDefaultEndpoint(wid.c_str(), eMultimedia);
        pc->SetDefaultEndpoint(wid.c_str(), eCommunications);
        ok = SUCCEEDED(hr1) || SUCCEEDED(hr2);
        pc->Release();
    } else {
        IPolicyConfigVistaMinimal *pcv = nullptr;
        if (SUCCEEDED(CoCreateInstance(kClsidPolicyConfigClient, nullptr, CLSCTX_ALL, kIidPolicyConfigVista, (void **)&pcv)) && pcv) {
            HRESULT hr1 = pcv->SetDefaultEndpoint(wid.c_str(), eConsole);
            HRESULT hr2 = pcv->SetDefaultEndpoint(wid.c_str(), eMultimedia);
            pcv->SetDefaultEndpoint(wid.c_str(), eCommunications);
            ok = SUCCEEDED(hr1) || SUCCEEDED(hr2);
            pcv->Release();
        }
    }
    if (uninit) CoUninitialize();
    return ok;
}

// ---- Monitor bounds & WIC PNG screenshot capture (zero extra DLLs or link libraries)
static const GUID kClsidWicImagingFactory = {0xCACAF262, 0x9370, 0x4615, {0xA1, 0x3B, 0x9F, 0x55, 0x39, 0xDA, 0x4C, 0x0A}};
static const GUID kIidIWicImagingFactory  = {0xEC5EC8A9, 0xC395, 0x4314, {0x9C, 0x77, 0x54, 0xD7, 0xA9, 0x35, 0xFF, 0x70}};
static const GUID kWicContainerPng        = {0x1B7CFAF4, 0x713F, 0x473C, {0xBB, 0xCD, 0x61, 0x37, 0x42, 0x5F, 0xAE, 0xAF}};
static const GUID kWicFmt24bppBGR         = {0x6FDDC324, 0x4E03, 0x4BFE, {0xB1, 0x85, 0x3D, 0x77, 0x76, 0x8D, 0xC9, 0x0C}};

bool monitor_rect_for_hwnd(void *target_hwnd, int &x, int &y, int &w, int &h) {
    HWND hwnd = (HWND)target_hwnd;
    HMONITOR mon = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
                        : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (mon && GetMonitorInfoW(mon, &mi)) {
        x = mi.rcMonitor.left;
        y = mi.rcMonitor.top;
        w = mi.rcMonitor.right - mi.rcMonitor.left;
        h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        return w > 0 && h > 0;
    }
    x = 0, y = 0;
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
    return w > 0 && h > 0;
}

// ---- Reversible per-game display refresh rate switching (session-only ChangeDisplaySettingsExW)
static bool g_hz_active = false;
static std::wstring g_hz_dev;
static DEVMODEW g_hz_orig{};

static bool monitor_device_name(void *hwnd_opt, std::wstring &out) {
    HWND hwnd = (HWND)hwnd_opt;
    HMONITOR mon = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)
                        : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    if (mon && GetMonitorInfoW(mon, &mi) && mi.szDevice[0]) {
        out = mi.szDevice;
        return true;
    }
    return false;
}

int display_current_refresh_rate(void *hwnd_opt) {
    std::wstring dev;
    DEVMODEW cur{};
    cur.dmSize = sizeof cur;
    if (EnumDisplaySettingsExW(monitor_device_name(hwnd_opt, dev) ? dev.c_str() : nullptr, ENUM_CURRENT_SETTINGS, &cur, 0))
        return (int)cur.dmDisplayFrequency;
    return 0;
}

std::vector<int> display_supported_refresh_rates(void *hwnd_opt) {
    std::vector<int> rates;
    std::wstring dev;
    const wchar_t *dptr = monitor_device_name(hwnd_opt, dev) ? dev.c_str() : nullptr;
    DEVMODEW cur{};
    cur.dmSize = sizeof cur;
    if (!EnumDisplaySettingsExW(dptr, ENUM_CURRENT_SETTINGS, &cur, 0)) return rates;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsExW(dptr, i, &dm, 0); ++i) {
        if (dm.dmPelsWidth == cur.dmPelsWidth && dm.dmPelsHeight == cur.dmPelsHeight &&
            dm.dmBitsPerPel == cur.dmBitsPerPel && dm.dmDisplayFrequency >= 30 && dm.dmDisplayFrequency <= 500) {
            int hz = (int)dm.dmDisplayFrequency;
            bool dup = false;
            for (int r : rates) if (r == hz) { dup = true; break; }
            if (!dup) rates.push_back(hz);
        }
    }
    // Simple ascending sort, then drop NTSC fractional duplicates (e.g. 59 next to 60, 119 next to 120)
    for (size_t i = 0; i < rates.size(); ++i)
        for (size_t j = i + 1; j < rates.size(); ++j)
            if (rates[j] < rates[i]) std::swap(rates[i], rates[j]);
    std::vector<int> clean;
    for (size_t i = 0; i < rates.size(); ++i) {
        if (i + 1 < rates.size() && rates[i] + 1 == rates[i + 1]) continue;
        clean.push_back(rates[i]);
    }
    return clean;
}

bool display_refresh_override_push(void *hwnd_opt, int target_hz) {
    if (target_hz < 30 || target_hz > 500) return false;
    display_refresh_override_pop();
    std::wstring dev;
    if (!monitor_device_name(hwnd_opt, dev)) return false;
    DEVMODEW cur{};
    cur.dmSize = sizeof cur;
    if (!EnumDisplaySettingsExW(dev.c_str(), ENUM_CURRENT_SETTINGS, &cur, 0)) return false;
    if ((int)cur.dmDisplayFrequency == target_hz) return true;   // already at target Hz

    // Find an exact supported mode at current resolution + target Hz
    DEVMODEW dm{}, match{};
    dm.dmSize = sizeof dm;
    bool found = false;
    for (DWORD i = 0; EnumDisplaySettingsExW(dev.c_str(), i, &dm, 0); ++i) {
        if (dm.dmPelsWidth == cur.dmPelsWidth && dm.dmPelsHeight == cur.dmPelsHeight &&
            dm.dmBitsPerPel == cur.dmBitsPerPel && (int)dm.dmDisplayFrequency == target_hz) {
            match = dm;
            found = true;
            break;
        }
    }
    if (!found) return false;
    match.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    LONG r = ChangeDisplaySettingsExW(dev.c_str(), &match, nullptr, CDS_FULLSCREEN, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL) return false;
    g_hz_dev = dev;
    g_hz_orig = cur;
    g_hz_active = true;
    return true;
}

void display_refresh_override_pop() {
    if (!g_hz_active) return;
    g_hz_orig.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    ChangeDisplaySettingsExW(g_hz_dev.c_str(), &g_hz_orig, nullptr, 0, nullptr);
    g_hz_active = false;
    g_hz_dev.clear();
}

static bool write_bgra_png_wic(const uint8_t *bgra, int w, int h, const std::string &dst_png) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool uninit = SUCCEEDED(co);
    if (FAILED(co) && co != RPC_E_CHANGED_MODE) return false;

    std::string tmp = dst_png + ".tmp";
    std::wstring wtmp = to_wide(tmp);
    bool ok = false;
    IWICImagingFactory *fac = nullptr;
    if (SUCCEEDED(CoCreateInstance(kClsidWicImagingFactory, nullptr, CLSCTX_INPROC_SERVER, kIidIWicImagingFactory, (void **)&fac)) && fac) {
        IWICStream *stm = nullptr;
        if (SUCCEEDED(fac->CreateStream(&stm)) && stm) {
            if (SUCCEEDED(stm->InitializeFromFilename(wtmp.c_str(), GENERIC_WRITE))) {
                IWICBitmapEncoder *enc = nullptr;
                if (SUCCEEDED(fac->CreateEncoder(kWicContainerPng, nullptr, &enc)) && enc) {
                    if (SUCCEEDED(enc->Initialize(stm, WICBitmapEncoderNoCache))) {
                        IWICBitmapFrameEncode *frame = nullptr;
                        if (SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && frame) {
                            if (SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize((UINT)w, (UINT)h))) {
                                WICPixelFormatGUID fmt = kWicFmt24bppBGR;
                                frame->SetPixelFormat(&fmt);
                                // Convert BGRA -> opaque 24-bit BGR (smaller PNGs, zero alpha issues)
                                int stride24 = (w * 3 + 3) & ~3;
                                std::vector<uint8_t> bgr((size_t)stride24 * (size_t)h);
                                for (int row = 0; row < h; ++row) {
                                    const uint8_t *src = bgra + (size_t)row * (size_t)w * 4;
                                    uint8_t *dst = bgr.data() + (size_t)row * (size_t)stride24;
                                    for (int col = 0; col < w; ++col) {
                                        dst[col * 3 + 0] = src[col * 4 + 0];
                                        dst[col * 3 + 1] = src[col * 4 + 1];
                                        dst[col * 3 + 2] = src[col * 4 + 2];
                                    }
                                }
                                if (SUCCEEDED(frame->WritePixels((UINT)h, (UINT)stride24, (UINT)bgr.size(), bgr.data())) &&
                                    SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit())) {
                                    ok = true;
                                }
                            }
                            frame->Release();
                        }
                    }
                    enc->Release();
                }
            }
            stm->Release();
        }
        fac->Release();
    }
    if (uninit) CoUninitialize();
    if (!ok) { SDL_RemovePath(tmp.c_str()); return false; }
    if (!SDL_RenamePath(tmp.c_str(), dst_png.c_str())) { SDL_RemovePath(tmp.c_str()); return false; }
    return true;
}

bool capture_screenshot_png(void *target_hwnd, const std::string &dst_png, int *w_out, int *h_out, bool allow_print_window) {
    RECT rc{};
    HWND hwnd = (HWND)target_hwnd;
    bool got_rc = false;
    if (hwnd && IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
        using DwmGetAttrFn = HRESULT (WINAPI *)(HWND, DWORD, PVOID, DWORD);
        static DwmGetAttrFn dwm_attr = []() -> DwmGetAttrFn {
            HMODULE m = LoadLibraryW(L"dwmapi.dll");
            return m ? (DwmGetAttrFn)(void *)GetProcAddress(m, "DwmGetWindowAttribute") : nullptr;
        }();
        // DWMWA_EXTENDED_FRAME_BOUNDS == 9 (excludes invisible Win10/11 drop shadow)
        if (dwm_attr && SUCCEEDED(dwm_attr(hwnd, 9, &rc, sizeof rc))) got_rc = true;
        else if (GetWindowRect(hwnd, &rc)) got_rc = true;
    }
    if (!got_rc) {
        int mx = 0, my = 0, mw = 0, mh = 0;
        if (!monitor_rect_for_hwnd(hwnd, mx, my, mw, mh)) return false;
        rc = {mx, my, mx + mw, my + mh};
    }
    // Clamp capture rect to the monitor it resides on so off-screen borders are never black
    int mx = 0, my = 0, mw = 0, mh = 0;
    if (monitor_rect_for_hwnd(hwnd, mx, my, mw, mh)) {
        if (rc.left < mx) rc.left = mx;
        if (rc.top < my) rc.top = my;
        if (rc.right > mx + mw) rc.right = mx + mw;
        if (rc.bottom > my + mh) rc.bottom = my + mh;
    }
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < 64 || h < 64) return false;

    HDC screen_dc = GetDC(nullptr);
    if (!screen_dc) return false;
    HDC mem_dc = CreateCompatibleDC(screen_dc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down BGRA
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!mem_dc || !bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        if (mem_dc) DeleteDC(mem_dc);
        ReleaseDC(nullptr, screen_dc);
        return false;
    }
    HGDIOBJ old = SelectObject(mem_dc, bmp);
    BOOL blt_ok = BitBlt(mem_dc, 0, 0, w, h, screen_dc, rc.left, rc.top, SRCCOPY);
    if (!blt_ok && hwnd && allow_print_window) {
        // Fallback when desktop DC BitBlt is unavailable (e.g. locked desktop / restricted compositor):
        // PW_RENDERFULLCONTENT (2) asks DWM for the window's composed surface directly (never call on a suspended process!).
        blt_ok = PrintWindow(hwnd, mem_dc, 2);
    }
    std::vector<uint8_t> pixels;
    if (blt_ok) {
        pixels.resize((size_t)w * (size_t)h * 4);
        memcpy(pixels.data(), bits, pixels.size());
    }
    SelectObject(mem_dc, old);
    DeleteObject(bmp);
    DeleteDC(mem_dc);
    ReleaseDC(nullptr, screen_dc);
    if (!blt_ok || pixels.empty()) return false;

    if (!write_bgra_png_wic(pixels.data(), w, h, dst_png)) return false;
    if (w_out) *w_out = w;
    if (h_out) *h_out = h;
    return true;
}

// Reversible Xbox Game Bar Guide-button override (only active while Sanctum runs; restores original value on exit)
static const wchar_t *kGameBarKey = L"Software\\Microsoft\\GameBar";
static const wchar_t *kGameBarVal = L"UseNexusForGameBarEnabled";
static bool g_gb_active = false;
static bool g_gb_had_orig = false;
static DWORD g_gb_orig = 1;

void gamebar_override_set(bool active) {
    if (active == g_gb_active) return;
    if (active) {
        DWORD v = 0, sz = sizeof v;
        LONG r = RegGetValueW(HKEY_CURRENT_USER, kGameBarKey, kGameBarVal, RRF_RT_REG_DWORD, nullptr, &v, &sz);
        g_gb_had_orig = (r == ERROR_SUCCESS);
        g_gb_orig = g_gb_had_orig ? v : 1;
        DWORD zero = 0;
        if (RegSetKeyValueW(HKEY_CURRENT_USER, kGameBarKey, kGameBarVal, REG_DWORD, &zero, sizeof zero) == ERROR_SUCCESS)
            g_gb_active = true;
    } else {
        if (g_gb_had_orig)
            RegSetKeyValueW(HKEY_CURRENT_USER, kGameBarKey, kGameBarVal, REG_DWORD, &g_gb_orig, sizeof g_gb_orig);
        else
            RegDeleteKeyValueW(HKEY_CURRENT_USER, kGameBarKey, kGameBarVal);
        g_gb_active = false;
    }
}

bool is_steam_running() {
    static Uint64 last_check = 0;
    static bool cached = false;
    Uint64 now = SDL_GetTicks();
    if (last_check && now - last_check < 5000) return cached;
    last_check = now;
    cached = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"steam.exe") == 0) { cached = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return cached;
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
const SystemDiag &system_diagnostics() { static SystemDiag d{"Unknown", "Unknown", "Unknown", "", 0}; return d; }
std::vector<DriveInfo> list_drives() { return {}; }
bool system_volume_get(int *, bool *) { return false; }
bool system_volume_set(int, bool) { return false; }
std::vector<AudioOutput> system_audio_outputs() { return {}; }
bool system_audio_output_set(const std::string &) { return false; }
bool capture_screenshot_png(void *, const std::string &, int *, int *, bool) { return false; }
bool monitor_rect_for_hwnd(void *, int &, int &, int &, int &) { return false; }
std::vector<int> display_supported_refresh_rates(void *) { return {}; }
int display_current_refresh_rate(void *) { return 0; }
bool display_refresh_override_push(void *, int) { return false; }
void display_refresh_override_pop() {}
void gamebar_override_set(bool) {}
bool is_steam_running() { return false; }
#endif

}  // namespace sanctum
