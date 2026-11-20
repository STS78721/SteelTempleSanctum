// HTTPS for Sanctum: WinHTTP (part of Windows, no DLL to ship). GitHub's API with ETags (a 304 costs nothing against
// the 60-an-hour limit), and downloads that resume where they stopped and report progress. The User-Agent is the
// program's name only: nothing about the player is ever sent.
#pragma once
#include <atomic>
#include <functional>
#include <string>

namespace sanctum {

struct HttpResult {
    int status = 0;            // 0: no connection
    std::string body, etag, error;
};

// GET url; with etag, a 304 means "unchanged" (body empty)
HttpResult http_get(const std::string &url, const std::string &etag = {});

// url to path; continues a partial file (Range). progress(done, total) from the worker; cancel stops it.
// Returns "" on success, else why.
std::string http_download(const std::string &url, const std::string &path, long long expected,
                          const std::function<void(long long, long long)> &progress, const std::atomic<bool> &cancel);

// "sha256:<hex>" of a file, or "" if it cannot be read
std::string sha256_file(const std::string &path, const std::atomic<bool> &cancel);

}  // namespace sanctum
