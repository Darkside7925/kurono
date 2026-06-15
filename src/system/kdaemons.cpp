//  kurono os  -  kupdate + ksecurity in-kernel service daemons. see kdaemons.h.

#include "kdaemons.h"
#include "kinit.h"
#include "logging.h"
#include "../packages/pkgmgr.h"
#include "../security/supr.h"
#include "../security/ksa.h"
#include "../ui/notification.h"
#include "../proc/scheduler.h"
#include "../drivers/serial.h"
#include "../shell/shell.h"

// ── kupdate ──────────────────────────────────────────────────────────────────
namespace KUpdate {
namespace {
volatile bool g_started = false;
volatile int  g_pending = 0;
volatile bool g_healthy = true;

bool ku_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

[[noreturn]] void worker_entry() {
    SerialLogger::Log("[kupdate] update checker online\r\n");
    // first check is deferred a little so the network + repo index settle. then
    // re-poll on a long interval (updates are not urgent and a sync touches the
    // network). (satoru)
    uint32_t next_check_ms = 0;
    int last_reported = -1;
    for (;;) {
        uint32_t now = (uint32_t)Scheduler::NowMs();
        if (now >= next_check_ms) {
            // GetPendingUpdateCount is a cheap query against the in-memory pkg db
            // (which SyncRepository populates). it does NOT itself hit the
            // network, so this loop is light; an explicit `kupdate check` forces
            // a sync. (satoru)
            int n = PackageManager::GetPendingUpdateCount();
            g_pending = n;
            if (n > 0 && n != last_reported) {
                char body[64];
                int p = 0;
                auto cat = [&](const char* s){ while (*s && p < (int)sizeof(body)-1) body[p++]=*s++; body[p]=0; };
                char num[12]; int ni = 0; int v = n;
                if (v == 0) num[ni++]='0'; else { char r[12]; int ri=0; while(v){r[ri++]='0'+(v%10);v/=10;} while(ri) num[ni++]=r[--ri]; }
                num[ni]=0;
                cat(num); cat(" update"); if (n != 1) cat("s"); cat(" available");
                NotificationManager::Post("kupdate", body, NotificationManager::ICON_INFO, 5000);
                KInit::LogEvent("updates-available", "kupdate", body);
            }
            last_reported = n;
            next_check_ms = now + 60000;   // re-poll once a minute (satoru)
        }
        Scheduler::SleepMs(500);
    }
}
}  // namespace

void Init() {
    if (g_started) return;
    g_started = true;
    Scheduler::SpawnKernelProcess("kupdate", worker_entry, PRIO_LOW, 64, 8192);
}

bool IsHealthy() { return g_healthy; }
int  PendingCount() { return g_pending; }

int Cmd(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    int p = 0;
    auto cat = [&](const char* s){ while (*s && p < mx-1) out[p++]=*s++; out[p]=0; };
    auto cati = [&](int v){ char r[12]; int ri=0; if(v==0)r[ri++]='0'; else{int x=v<0?-v:v; char t[12]; int ti=0; while(x){t[ti++]='0'+(x%10);x/=10;} if(v<0)r[ri++]='-'; while(ti)r[ri++]=t[--ti];} r[ri]=0; cat(r); };

    if (argc >= 2 && ku_eq(argv[1], "check")) {
        cat("kupdate: syncing repository index...\n");
        bool ok = PackageManager::SyncRepository();
        int n = PackageManager::GetPendingUpdateCount();
        g_pending = n;
        if (!ok) { cat("kupdate: sync failed: "); cat(PackageManager::GetLastSyncMessage()); cat("\n"); return p; }
        cat("kupdate: "); cati(n); cat(" update"); if (n != 1) cat("s"); cat(" available\n");
        return p;
    }
    cat("kupdate: "); cati(g_pending); cat(" update"); if (g_pending != 1) cat("s");
    cat(" pending (run 'kupdate check' to re-scan)\n");
    return p;
}

void RegisterShellCommands(void* shell) {
    Shell* sh = (Shell*)shell;
    if (sh) sh->RegisterCommand("kupdate", Cmd, "system update checker");
}
}  // namespace KUpdate

// ── ksecurity ────────────────────────────────────────────────────────────────
namespace KSecurity {
namespace {
volatile bool g_started = false;
volatile bool g_healthy = true;
volatile bool g_supr_ok = true;
volatile bool g_ksa_ok  = true;

bool ks_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

[[noreturn]] void worker_entry() {
    SerialLogger::Log("[ksecurity] policy watch online\r\n");
    uint32_t next_ms = 0;
    bool first = true;
    for (;;) {
        uint32_t now = (uint32_t)Scheduler::NowMs();
        if (now >= next_ms) {
            bool supr_ok = SUPR::PolicySelfTest();
            // ksa self-test only meaningful where the nested-vm secure path is
            // available; treat an unavailable ksa as healthy (not a failure) so
            // a plain qemu boot doesn't flag a security alert. (satoru)
            bool ksa_ok = KSA::IsAvailable() ? KSA::SelfTest() : true;
            g_supr_ok = supr_ok;
            g_ksa_ok  = ksa_ok;
            bool healthy = supr_ok && ksa_ok;
            if (!healthy && (first || g_healthy)) {
                // transitioned to unhealthy -> alert. (satoru)
                NotificationManager::Post("ksecurity", "policy self-test FAILED",
                                          NotificationManager::ICON_ERROR, 6000);
                KInit::LogEvent("policy-selftest-fail", "ksecurity",
                                supr_ok ? "ksa" : "supr");
                RuntimeLog::LogSecurity("ksecurity policy self-test failed", supr_ok ? "ksa" : "supr");
            } else if (first) {
                KInit::LogEvent("policy-selftest-ok", "ksecurity", nullptr);
            }
            g_healthy = healthy;
            first = false;
            next_ms = now + 30000;   // re-verify every 30s (satoru)
        }
        Scheduler::SleepMs(500);
    }
}
}  // namespace

void Init() {
    if (g_started) return;
    g_started = true;
    Scheduler::SpawnKernelProcess("ksecurity", worker_entry, PRIO_LOW, 64, 8192);
}

bool IsHealthy() { return g_healthy; }

int Cmd(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    auto cat = [&](const char* s){ while (*s && p < mx-1) out[p++]=*s++; out[p]=0; };
    cat("ksecurity policy watch:\n");
    cat("  supr policy self-test: "); cat(g_supr_ok ? "ok\n" : "FAILED\n");
    cat("  ksa  self-test:        ");
    if (!KSA::IsAvailable()) cat("n/a (no nested-vm secure path)\n");
    else cat(g_ksa_ok ? "ok\n" : "FAILED\n");
    return p;
}

void RegisterShellCommands(void* shell) {
    Shell* sh = (Shell*)shell;
    if (sh) sh->RegisterCommand("ksecurity", Cmd, "KSA + supr policy watch");
}
}  // namespace KSecurity

// end (satoru)
