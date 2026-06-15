//  kurono os  -  kpkg-daemon implementation.
//
//  see kpkg_daemon.h. a single-job daemon: a request ring of depth 1 (one
//  install at a time, like apt's lock), a worker kernel-process that drains it
//  by calling PackageManager::Install, and a published JobStatus the gui polls.
//  every transition is logged to services.log via kinit and broadcast on
//  org.kurono.Pkg over D-Bus. (satoru)

#include "kpkg_daemon.h"
#include "kinit.h"
#include "dbus_server.h"
#include "logging.h"
#include "../packages/pkgmgr.h"
#include "../drivers/serial.h"
#include "../proc/scheduler.h"
#include "../ui/notification.h"
#include "../shell/shell.h"

namespace KpkgDaemon {

namespace {

// the single in-flight job + a one-slot pending request. volatile because the
// gui/shell thread writes the request and the worker thread reads it. (satoru)
volatile bool g_have_request = false;
char          g_request_pkg[KPKG_NAME_LEN];

JobStatus     g_status;
bool          g_worker_started = false;

void kd_cpy(char* d, const char* s, int mx) {
    int i = 0; while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; } d[i] = 0;
}
bool kd_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

void set_status(JobState st, const char* pkg, int pct, const char* msg) {
    g_status.state = st;
    if (pkg) kd_cpy(g_status.package, pkg, sizeof(g_status.package));
    g_status.percent = pct;
    if (msg) kd_cpy(g_status.message, msg, sizeof(g_status.message));
    // broadcast to any D-Bus subscriber on org.kurono.Pkg. the current bus
    // EmitSignal is a stub (no AddMatch routing yet), so this is best-effort and
    // the gui polls GetStatus() as the reliable channel. logged regardless so
    // the event is never lost. (satoru)
    DBusServer::EmitSignal("/org/kurono/Pkg", "org.kurono.Pkg", "Progress");
}

// the worker: drains the one-slot request ring forever. it runs concurrently
// with the gui via the preemptive scheduler, so the long blocking
// PackageManager::Install (download + tar-extract) never freezes the desktop.
// (satoru)
[[noreturn]] void worker_entry() {
    SerialLogger::Log("[kpkg-daemon] worker online\r\n");
    for (;;) {
        if (g_have_request) {
            char pkg[KPKG_NAME_LEN];
            kd_cpy(pkg, g_request_pkg, sizeof(pkg));
            g_have_request = false;

            set_status(KPKG_DOWNLOADING, pkg, 0, "starting download");
            KInit::LogEvent("kpkg-install-begin", pkg, nullptr);
            NotificationManager::Post("kpkg", "installing in background", NotificationManager::ICON_INFO, 3000);

            // the real work. this blocks the WORKER (not the gui) for the whole
            // download+extract. progress granularity inside Install is coarse
            // (pkgmgr emits its own [kpkg] received N bytes lines), so we report
            // the phase transitions here. (satoru)
            bool ok = PackageManager::Install(pkg);

            if (ok) {
                set_status(KPKG_DONE, pkg, 100, "install complete");
                KInit::LogEvent("kpkg-install-done", pkg, nullptr);
                NotificationManager::Post("kpkg", "install complete", NotificationManager::ICON_SUCCESS, 4000);
            } else {
                set_status(KPKG_FAILED, pkg, 0, PackageManager::GetLastSyncMessage());
                KInit::LogEvent("kpkg-install-failed", pkg, PackageManager::GetLastSyncMessage());
                NotificationManager::Post("kpkg", "install failed", NotificationManager::ICON_ERROR, 5000);
            }
        }
        Scheduler::SleepMs(100);
    }
}

}  // namespace

void Init() {
    if (g_worker_started) return;
    g_status.state = KPKG_IDLE;
    g_status.package[0] = 0;
    g_status.percent = 0;
    g_status.message[0] = 0;
    g_have_request = false;
    Scheduler::SpawnKernelProcess("kpkg-daemon", worker_entry, PRIO_NORMAL, 64, 16 * 1024);
    g_worker_started = true;
    KInit::LogEvent("kpkg-daemon", "kpkg-daemon", "in-kernel worker started");
    SerialLogger::Log("[kpkg-daemon] init\r\n");
}

bool RequestInstall(const char* package) {
    if (!package || !package[0]) return false;
    if (!g_worker_started) Init();
    // single-job: reject a new request while one is running or pending. (satoru)
    if (g_have_request || g_status.state == KPKG_DOWNLOADING) return false;
    kd_cpy(g_request_pkg, package, sizeof(g_request_pkg));
    set_status(KPKG_QUEUED, package, 0, "queued");
    g_have_request = true;
    KInit::LogEvent("kpkg-queued", package, nullptr);
    return true;
}

void GetStatus(JobStatus* out) {
    if (!out) return;
    *out = g_status;
}

bool IsBusy() {
    return g_have_request || g_status.state == KPKG_DOWNLOADING || g_status.state == KPKG_QUEUED;
}

int Cmd(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    int p = 0;
    auto cat = [&](const char* s) { while (*s && p < mx - 1) out[p++] = *s++; out[p] = 0; };

    if (argc < 2 || kd_eq(argv[1], "status")) {
        const char* st = "idle";
        switch (g_status.state) {
            case KPKG_IDLE:        st = "idle"; break;
            case KPKG_QUEUED:      st = "queued"; break;
            case KPKG_DOWNLOADING: st = "downloading"; break;
            case KPKG_DONE:        st = "done"; break;
            case KPKG_FAILED:      st = "failed"; break;
        }
        cat("kpkg-daemon: ");
        cat(st);
        if (g_status.package[0]) { cat("  pkg="); cat(g_status.package); }
        if (g_status.message[0]) { cat("  ("); cat(g_status.message); cat(")"); }
        cat("\n");
        return p;
    }

    if (kd_eq(argv[1], "install")) {
        if (argc < 3) { cat("usage: kpkg-daemon install <package>\n"); return p; }
        if (RequestInstall(argv[2])) {
            cat("kpkg-daemon: queued '");
            cat(argv[2]);
            cat("'  -  installing in background; the desktop stays responsive.\n");
            cat("watch progress: kpkg-daemon status\n");
        } else {
            cat("kpkg-daemon: busy with another install; try again when it finishes.\n");
        }
        return p;
    }

    cat("usage: kpkg-daemon status | install <package>\n");
    return p;
}

void RegisterShellCommands(void* shell) {
    // shell.h provides the (void*,...) RegisterCommand convenience overload via
    // the Shell alias; kinit uses it too. (satoru)
    Shell* sh = (Shell*)shell;
    if (!sh) return;
    sh->RegisterCommand("kpkg-daemon", Cmd, "background package install daemon");
}

}  // namespace KpkgDaemon

// end (satoru)
