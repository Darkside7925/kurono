#pragma once
#include "../kernel/types.h"

//  kurono os  -  kpkg-daemon: the package install/download daemon.
//
//  the concrete win kinit buys us: the package download+extract loop is slow
//  (large tars, slirp-bound networking) and it USED to run inline on whatever
//  thread invoked `kpkg install`, so a gui-initiated install blocked the
//  desktop. kpkg-daemon moves that loop into a dedicated worker so the gui never
//  blocks: a caller enqueues an install request and returns immediately; the
//  worker downloads in the background and publishes progress that the gui polls
//  (and emits a D-Bus signal on org.kurono.Pkg for any bus subscriber).
//
//  HONESTY NOTE: the worker is a dedicated in-kernel kernel-process, not a
//  separate linux address space. that already delivers the isolation that
//  matters here (the gui render loop is never blocked by a download, because the
//  preemptive scheduler time-shares the worker against the gui). a fully
//  separate /kurono/system/bin/kpkg-daemon user process is also registered as a
//  kinit process-unit; it launches once that binary is built+installed. the
//  in-kernel worker is what works today and is what the gui talks to. it does
//  NOT change the download throughput (that is slirp + kvfs-write bound, not
//  cpu-bound)  -  its value is purely non-blocking isolation. (satoru)

namespace KpkgDaemon {

enum JobState : uint8_t {
    KPKG_IDLE = 0,      // no job (satoru)
    KPKG_QUEUED,        // request accepted, not yet started (satoru)
    KPKG_DOWNLOADING,   // worker is installing (download+extract) (satoru)
    KPKG_DONE,          // finished ok (satoru)
    KPKG_FAILED         // finished with an error (satoru)
};

constexpr int KPKG_NAME_LEN = 32;

struct JobStatus {
    JobState state;
    char     package[KPKG_NAME_LEN];
    int      percent;          // 0..100 best-effort (satoru)
    char     message[96];      // last status line (satoru)
};

// start the daemon: create the request ring + spawn the worker kernel-process.
// idempotent. (satoru)
void Init();

// enqueue an install request. returns true if accepted (queued), false if the
// daemon is busy with another job or the queue is full. NON-BLOCKING. the gui /
// shell calls this and returns immediately. (satoru)
bool RequestInstall(const char* package);

// current job status snapshot (for the gui progress bar + `kpkg-daemon status`).
// (satoru)
void GetStatus(JobStatus* out);

// true once a job has been requested and the worker hasn't gone idle. (satoru)
bool IsBusy();

// shell command: `kpkg-daemon status|install <pkg>`. also reachable via the
// `kpkg install --daemon <pkg>` route wired in pkgmgr. (satoru)
int  Cmd(void* sh, int argc, const char** argv, char* out, int mx);
void RegisterShellCommands(void* shell);

}  // namespace KpkgDaemon

// end (satoru)
