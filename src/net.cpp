#include "net.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

namespace sanctum {

static const wchar_t *kAgent = L"SteelTemple-Sanctum/0.2";

static std::wstring wide(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static std::string narrow(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::string last_error(const char *what) {
    char b[128];
    SDL_snprintf(b, sizeof b, "%s failed (error %lu)", what, GetLastError());
    return b;
}

// one request: session, connection and request handles, closed together
struct Req {
    HINTERNET ses = nullptr, con = nullptr, req = nullptr;
    ~Req() {
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
    }
    // opens url with GET and the extra headers; returns "" or why
    std::string open(const std::string &url, const std::wstring &headers) {
        std::wstring w = wide(url);
        URL_COMPONENTS u{};
        u.dwStructSize = sizeof u;
        wchar_t host[256], path[2048];
        u.lpszHostName = host, u.dwHostNameLength = 256;
        u.lpszUrlPath = path, u.dwUrlPathLength = 2048;
        if (!WinHttpCrackUrl(w.c_str(), 0, 0, &u)) return last_error("reading the address");
        ses = WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses) return last_error("WinHttpOpen");
        DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        WinHttpSetOption(ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof protos);
        WinHttpSetTimeouts(ses, 10000, 15000, 30000, 60000);
        con = WinHttpConnect(ses, host, u.nPort, 0);
        if (!con) return last_error("connecting");
        req = WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!req) return last_error("opening the request");
        if (!WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                headers.empty() ? 0 : (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, nullptr))
            return "no connection to GitHub";
        return {};
    }
    int status() {
        DWORD s = 0, n = sizeof s;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &s, &n,
                            WINHTTP_NO_HEADER_INDEX);
        return (int)s;
    }
    std::string header(const wchar_t *name) {
        wchar_t buf[512];
        DWORD n = sizeof buf;
        if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, buf, &n, WINHTTP_NO_HEADER_INDEX)) return {};
        return narrow(buf);
    }
};

HttpResult http_get(const std::string &url, const std::string &etag) {
    HttpResult r;
    Req q;
    std::wstring h = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!etag.empty()) h += L"If-None-Match: " + wide(etag) + L"\r\n";
    r.error = q.open(url, h);
    if (!r.error.empty()) return r;
    r.status = q.status();
    r.etag = q.header(L"ETag");
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(q.req, &avail) || !avail) break;
        size_t at = r.body.size();
        r.body.resize(at + avail);
        DWORD got = 0;
        if (!WinHttpReadData(q.req, r.body.data() + at, avail, &got)) break;
        r.body.resize(at + got);
    }
    if (r.status == 403 || r.status == 429) r.error = "GitHub's hourly limit reached; trying again later";
    else if (r.status != 200 && r.status != 304) r.error = "GitHub answered " + std::to_string(r.status);
    return r;
}

std::string http_download(const std::string &url, const std::string &path, long long expected,
                          const std::function<void(long long, long long)> &progress, const std::atomic<bool> &cancel) {
    long long have = 0;
    SDL_PathInfo info;
    if (SDL_GetPathInfo(path.c_str(), &info)) have = (long long)info.size;
    if (expected > 0 && have == expected) { progress(have, expected); return {}; }   // already complete
    if (expected > 0 && have > expected) { SDL_RemovePath(path.c_str()); have = 0; }

    Req q;
    std::wstring h;
    if (have > 0) h = L"Range: bytes=" + std::to_wstring(have) + L"-\r\n";
    std::string err = q.open(url, h);
    if (!err.empty()) return err;
    int st = q.status();
    if (st == 200) have = 0;                     // the server sent it all again
    else if (st != 206) return "the download answered " + std::to_string(st);

    FILE *f = _wfopen(wide(path).c_str(), have ? L"ab" : L"wb");
    if (!f) return "cannot write " + path;
    long long total = expected > 0 ? expected : 0, done = have;
    std::vector<char> buf(1 << 20);
    Uint64 last = 0;
    std::string why;
    for (;;) {
        if (cancel) { why = "cancelled"; break; }
        DWORD got = 0;
        if (!WinHttpReadData(q.req, buf.data(), (DWORD)buf.size(), &got)) { why = "the connection dropped"; break; }
        if (!got) break;
        if (fwrite(buf.data(), 1, got, f) != got) { why = "the disk is full"; break; }
        done += got;
        Uint64 now = SDL_GetTicks();
        if (now - last > 100) { progress(done, total); last = now; }
    }
    fclose(f);
    progress(done, total);
    if (why.empty() && expected > 0 && done != expected) why = "the download ended early";
    return why;
}

std::string sha256_file(const std::string &path, const std::atomic<bool> &cancel) {
    FILE *f = _wfopen(wide(path).c_str(), L"rb");
    if (!f) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buf(1 << 20);
        size_t n;
        bool ok = true;
        while ((n = fread(buf.data(), 1, buf.size(), f)) > 0)
            if (cancel || BCryptHashData(hash, buf.data(), (ULONG)n, 0)) { ok = false; break; }
        unsigned char d[32];
        if (ok && BCryptFinishHash(hash, d, 32, 0) == 0) {
            out = "sha256:";
            for (unsigned char c : d) {
                char x[3];
                SDL_snprintf(x, 3, "%02x", c);
                out += x;
            }
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    fclose(f);
    return out;
}

}  // namespace sanctum
