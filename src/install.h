// The downloads queue: one job at a time on a worker, the rest waiting, all of it running while the player browses.
//
//   Download  the release file from GitHub into %LOCALAPPDATA%\SteelTemple\Sanctum\downloads (resumes if stopped)
//   Verify    its size, and its SHA-256 against the digest GitHub publishes for it
//   Install   Standalone: unpacked (Windows' own tar) beside the library folder, then moved in as <library>\<port>
//             Update: unpacked, then laid over the install; every replaced file is first renamed *.old and put back
//                     if anything fails, and the *.old files are removed once all is in place
//             Setup Wizard: the port's own setup runs (Sanctum steps aside while it is open), then this PC is searched
//   Ready     the copy is added to the port, Play starts it; the port's own first start sets up the game files
//
// Sanctum never downloads game files: only the ports' own releases.
#pragma once
#include <atomic>
#include <SDL3/SDL.h>

#include <string>
#include <vector>

#include "catalog.h"

namespace sanctum {

enum JobKind { JOB_INSTALL, JOB_UPDATE, JOB_WIZARD };
enum JobPhase { PH_QUEUED, PH_DOWNLOAD, PH_VERIFY, PH_INSTALL, PH_WIZARD, PH_DONE, PH_FAILED, PH_CANCELLED };

struct JobView {
    int id, port, kind, phase;
    int reached = PH_QUEUED;   // the last working phase (where a failure happened)
    std::string tag, file, dest, error;
    long long done = 0, total = 0;
    double speed = 0;   // bytes a second while downloading
};

// queue a release file for a port; dest: the folder an update goes over (empty for a new install)
int install_enqueue(int port, const Release &r, const Asset &a, JobKind kind, const std::string &dest = {});
std::vector<JobView> install_jobs();   // a snapshot, newest last
void install_cancel(int id);
void install_clear_finished();
bool install_busy();                   // a job is waiting or running
bool install_queued(int port);         // this port has a job waiting or running
void install_poll(SDL_Window *w);      // UI thread: finished jobs into the catalog; steps aside for a Setup Wizard
std::string library_dir();             // where new ports go (System Settings > Ports on this PC)

// package helpers (also Sanctum's own updates): unzip with Windows' tar; lay src's files over dst (each replaced file
// becomes <name>.old first, all put back on failure); remove a folder and everything in it
std::string unzip(const std::string &zip, const std::string &to, const std::atomic<bool> &cancel);
std::string overlay(const std::string &src, const std::string &dst);
void remove_tree(const std::string &p);

}  // namespace sanctum
