// Cloud-Folder Save Sync Bridge (OneDrive / Google Drive / Dropbox / Syncthing / NAS / USB).
// Mirrors each port's save folder as an atomic snapshot (<sync_dir>\<port_id>\latest.zip + meta.ini) after gameplay
// and detects newer cloud saves from another PC or handheld (ROG Ally, Legion Go, etc.) with conflict-safe pull
// (always saves a local pre-sync safety backup before unpacking a cloud save).
#pragma once
#include <string>

#include "catalog.h"

namespace sanctum {

enum SyncKind {
    SYNC_OFF,          // no save_sync_dir configured
    SYNC_NO_SAVES,     // neither local saves nor cloud snapshot exist yet
    SYNC_UP_TO_DATE,   // local and cloud timestamps match (within tolerance)
    SYNC_LOCAL_NEWER,  // local saves are newer than cloud (or cloud has no snapshot yet)
    SYNC_CLOUD_NEWER,  // cloud snapshot from another device (or newer backup) is ahead of local
    SYNC_CONFLICT,     // both local and cloud changed independently since last sync
};

struct SyncInfo {
    SyncKind kind = SYNC_OFF;
    long long local_mtime = 0;   // unix seconds of newest file in local save folder
    long long cloud_mtime = 0;   // unix seconds recorded in cloud meta.ini
    std::string cloud_host;      // device name that uploaded the cloud snapshot
    std::string summary;         // concise UI string (e.g. "Cloud newer (ROG-ALLY) · Pull")
    int cloud_files = 0;
};

SyncInfo sync_inspect(const Port &p, const Install &in, bool force = false);
void sync_invalidate(const char *port_id = nullptr);

bool sync_push_now(const Port &p, const Install &in, std::string &err_out);
bool sync_pull_now(const Port &p, const Install &in, std::string &err_out);
int sync_push_all(int *skipped_cloud_newer = nullptr);

// Background post-session auto-sync (called by launch_poll when a game session ends)
void sync_push_async(int port_idx, const Install &in);
void sync_poll();

}  // namespace sanctum
