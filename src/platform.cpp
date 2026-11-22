#include "platform.h"

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
#else
bool shortcut_create_desktop() { return false; }
bool shortcut_create_startmenu() { return false; }
bool startup_is_enabled() { return false; }
bool startup_set_enabled(bool) { return false; }
bool recycle_path(const std::string &) { return false; }
long long folder_size_bytes(const std::string &, int *file_count) { if (file_count) *file_count = 0; return 0; }
#endif

}  // namespace sanctum
