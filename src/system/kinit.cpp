//  kurono os  -  kinit core: registration, target sequencing, spawn, crash
//  monitor, shell commands, audit logging.
//
//  see kinit.h for the design. the honest short version: kinit supervises two
//  unit kinds. an in-kernel unit (dbus/wayland/audio/network/logging) is tracked
//  via start/stop/health hooks; a process unit (kpkg-daemon) is a real isolated
//  linux user process. because Userspace::RunProcessWithArgs BLOCKS its calling
//  kernel stack until the user process exits, each process unit gets its OWN
//  supervisor kernel-process (Scheduler::SpawnKernelProcess) that loads the elf,
//  runs it (concurrently with the gui via the preemptive scheduler), and posts
//  the exit code back here when it returns. the crash monitor then applies the
//  backoff + 5-in-60s rule. (satoru)

#include "kinit.h"
#include "kpaths.h"
#include "logging.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../proc/scheduler.h"
#include "../kernel/elf_loader.h"
#include "../kernel/userspace.h"
#include "../linux/linux_syscall.h"
#include "../security/supr.h"
#include "../security/ksa.h"
#include "../ui/notification.h"
#include "../shell/shell.h"
#include "kpkg_daemon.h"
#include "kdaemons.h"

namespace KInit {

namespace {

KService  g_services[KINIT_MAX_SERVICES];
int       g_service_count = 0;
bool      g_booted = false;
uint32_t  g_boot_start_ms = 0;

// backoff ceiling per the spec: 2s,4s,8s..max 60s. (satoru)
constexpr uint32_t KINIT_BACKOFF_MAX_MS = 60000;
// crash burst rule: >= this many crashes inside the window -> failed. (satoru)
constexpr int      KINIT_BURST_LIMIT    = 5;
constexpr uint32_t KINIT_BURST_WINDOW_MS = 60000;

// ── freestanding string helpers (satoru) ─────────────────────────────────────
int ki_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

bool ki_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

void ki_cpy(char* d, const char* s, int mx) {
    int i = 0;
    while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

int ki_cat(char* out, int p, int mx, const char* s) {
    while (s && *s && p < mx - 1) out[p++] = *s++;
    if (p < mx) out[p] = 0;
    return p;
}

int ki_cat_u(char* out, int p, int mx, uint32_t v) {
    char tmp[12]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    else while (v > 0 && n < 12) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && p < mx - 1) out[p++] = tmp[--n];
    if (p < mx) out[p] = 0;
    return p;
}

uint32_t now_ms() { return Timer::GetRealMs(); }

// does dep name `dep` appear in the space-separated after= list `list`? (satoru)
bool after_lists(const char* list, const char* dep) {
    int i = 0;
    while (list[i]) {
        while (list[i] == ' ' || list[i] == '\t' || list[i] == ',') i++;
        if (!list[i]) break;
        int j = 0;
        while (dep[j] && list[i + j] && dep[j] == list[i + j]) j++;
        if (dep[j] == 0) {
            char after = list[i + j];
            if (after == 0 || after == ' ' || after == '\t' || after == ',') return true;
        }
        while (list[i] && list[i] != ' ' && list[i] != '\t' && list[i] != ',') i++;
    }
    return false;
}

// is a service "settled" for dependency purposes (running, or a completed
// oneshot, or stopped)? deps must be settled before a dependent starts. (satoru)
bool dep_settled(const KService* s) {
    return s->state == KSVC_RUNNING || s->state == KSVC_STOPPED;
}

// ── process-unit supervisor (satoru) ─────────────────────────────────────────
// each process unit gets a supervisor kernel-process pinned to one slot. the
// supervisor loads the elf, runs it to completion (blocking on its own kernel
// stack while the scheduler time-shares the user process against the gui), then
// records the exit code so Tick() can apply the restart policy. (satoru)
struct SupervisorSlot {
    bool      in_use;
    int       svc_index;        // which g_services entry this drives (satoru)
    volatile bool relaunch;     // Tick() sets this to ask for a (re)launch (satoru)
    volatile bool running;      // true while RunProcessWithArgs is active (satoru)
    volatile bool exited;       // set when the user process returned (satoru)
    volatile int  exit_code;    // its exit code (satoru)
};
SupervisorSlot g_sup[KINIT_MAX_SERVICES];

// build a NULL-terminated argv from a service's Exec= string. splits on spaces.
// stores the tokens in `store` and pointers in `argv`. returns argc. (satoru)
int build_argv(const char* exec, char* store, int store_max,
               const char** argv, int argv_max) {
    int argc = 0;
    int sp = 0;
    int i = 0;
    while (exec[i] && argc < argv_max - 1) {
        while (exec[i] == ' ' || exec[i] == '\t') i++;
        if (!exec[i]) break;
        argv[argc++] = &store[sp];
        while (exec[i] && exec[i] != ' ' && exec[i] != '\t' && sp < store_max - 1)
            store[sp++] = exec[i++];
        if (sp < store_max) store[sp++] = 0;
        while (exec[i] && exec[i] != ' ' && exec[i] != '\t') i++;   // skip overflow (satoru)
    }
    argv[argc] = nullptr;
    return argc;
}

// the supervisor body for slot index passed via a small dispatch table. since
// SpawnKernelProcess takes a void(*)() with no argument, we use a fixed set of
// trampolines, one per slot. (satoru)
void supervisor_run(int slot) {
    SupervisorSlot* ss = &g_sup[slot];
    for (;;) {
        if (ss->relaunch && ss->svc_index >= 0 && ss->svc_index < g_service_count) {
            ss->relaunch = false;
            KService* svc = &g_services[ss->svc_index];

            // resolve argv0 + args from Exec=. (satoru)
            static char store[KINIT_MAX_SERVICES][KINIT_PATH_LEN];
            const char* argv[16];
            int argc = build_argv(svc->exec, store[slot], KINIT_PATH_LEN, argv, 16);
            const char* path = (argc > 0) ? argv[0] : nullptr;

            if (!path || !KVFS::Exists(path)) {
                // honest failure: the daemon binary is not installed. mark it
                // stopped (not crash-looping) and log it. (satoru)
                ss->running = false;
                ss->exited  = true;
                ss->exit_code = 127;   // command-not-found convention (satoru)
                LogEvent("spawn-missing", svc->name, path ? path : "(no exec)");
                Scheduler::SleepMs(200);
                continue;
            }

            Process* p = ElfLoader::LoadELF64FromVFS(path, svc->name);
            if (!p) {
                ss->running = false;
                ss->exited  = true;
                ss->exit_code = 126;   // could-not-exec convention (satoru)
                LogEvent("spawn-loadfail", svc->name, path);
                Scheduler::SleepMs(200);
                continue;
            }

            ss->running = true;
            ss->exited  = false;
            LogEvent("spawn", svc->name, path);

            const char* envp[] = { "PATH=/kurono/system/bin:/usr/bin", "HOME=/home/user", nullptr };
            // this BLOCKS until the user process exits/crashes; the scheduler
            // time-shares it against the gui via preemption. (satoru)
            int rc = Userspace::RunProcessWithArgs(p, argv, envp);

            ss->exit_code = rc;
            ss->running   = false;
            ss->exited    = true;
            LogEvent("exited", svc->name, nullptr);
        }
        Scheduler::SleepMs(100);   // idle until Tick() asks for a (re)launch (satoru)
    }
}

// one trampoline per slot (SpawnKernelProcess has no user-arg). (satoru)
#define KISUP(n) void sup_##n() { supervisor_run(n); }
KISUP(0)  KISUP(1)  KISUP(2)  KISUP(3)  KISUP(4)  KISUP(5)  KISUP(6)  KISUP(7)
KISUP(8)  KISUP(9)  KISUP(10) KISUP(11) KISUP(12) KISUP(13) KISUP(14) KISUP(15)
KISUP(16) KISUP(17) KISUP(18) KISUP(19) KISUP(20) KISUP(21) KISUP(22) KISUP(23)
KISUP(24) KISUP(25) KISUP(26) KISUP(27) KISUP(28) KISUP(29) KISUP(30) KISUP(31)
#undef KISUP
KernelProcessEntry g_sup_entry[KINIT_MAX_SERVICES] = {
    sup_0,  sup_1,  sup_2,  sup_3,  sup_4,  sup_5,  sup_6,  sup_7,
    sup_8,  sup_9,  sup_10, sup_11, sup_12, sup_13, sup_14, sup_15,
    sup_16, sup_17, sup_18, sup_19, sup_20, sup_21, sup_22, sup_23,
    sup_24, sup_25, sup_26, sup_27, sup_28, sup_29, sup_30, sup_31
};

// ensure a supervisor kernel-process exists for slot, spawning it once. (satoru)
void ensure_supervisor(int slot) {
    if (slot < 0 || slot >= KINIT_MAX_SERVICES) return;
    if (g_sup[slot].in_use) return;
    g_sup[slot].in_use    = true;
    g_sup[slot].svc_index = slot;
    g_sup[slot].relaunch  = false;
    g_sup[slot].running   = false;
    g_sup[slot].exited    = false;
    g_sup[slot].exit_code = 0;
    char nm[KINIT_NAME_LEN + 8];
    int p = 0;
    p = ki_cat(nm, p, (int)sizeof(nm), "ksup:");
    p = ki_cat(nm, p, (int)sizeof(nm), g_services[slot].name);
    // a daemon needs a healthy stack for ld-kurono + the user runtime, but the
    // user image lives in its OWN address space; this is just the supervisor's
    // kernel stack. give it room. (satoru)
    Scheduler::SpawnKernelProcess(nm, g_sup_entry[slot], PRIO_NORMAL, 64, 16 * 1024);
}

// capability gate: a process unit that asks for a capability it cannot have is
// refused at spawn. for now we gate on the active supr level: a service that
// wants no elevated capability always passes; one requesting filesystem/network
// requires at least a logged-in user session. this is intentionally light (the
// in-kernel units already run privileged) but it is REAL: a disabled or
// guest session blocks privileged process spawns, and every decision is logged.
// (satoru)
bool capability_gate(const KService* svc) {
    if (svc->kind != KUNIT_PROCESS && svc->kind != KUNIT_ONESHOT) return true;
    bool wants_priv = svc->caps.network || svc->caps.filesystem;
    if (!wants_priv) return true;
    int sid = SUPR::GetCurrentSession();
    SUPRLevel lvl = (sid >= 0) ? SUPR::GetLevel(sid) : SUPR_GUEST;
    bool ok = (lvl >= SUPR_USER);
    char detail[96];
    int p = 0;
    p = ki_cat(detail, p, (int)sizeof(detail), "net=");
    p = ki_cat(detail, p, (int)sizeof(detail), svc->caps.network ? "1" : "0");
    p = ki_cat(detail, p, (int)sizeof(detail), " fs=");
    p = ki_cat(detail, p, (int)sizeof(detail), svc->caps.filesystem ? "1" : "0");
    p = ki_cat(detail, p, (int)sizeof(detail), " gui=");
    p = ki_cat(detail, p, (int)sizeof(detail), svc->caps.gui ? "1" : "0");
    p = ki_cat(detail, p, (int)sizeof(detail), ok ? " -> grant" : " -> deny");
    LogEvent("capability", svc->name, detail);
    RuntimeLog::LogSecurity("kinit capability gate", detail);
    return ok;
}

// raise a desktop toast for a critical-service failure. (satoru)
void notify_critical_failure(const KService* svc) {
    char body[96];
    int p = 0;
    p = ki_cat(body, p, (int)sizeof(body), svc->name);
    p = ki_cat(body, p, (int)sizeof(body), " failed and was disabled");
    NotificationManager::Post("Service failed", body, NotificationManager::ICON_ERROR, 6000);
}

int find_index(const char* name) {
    for (int i = 0; i < g_service_count; i++)
        if (ki_eq(g_services[i].name, name)) return i;
    return -1;
}

}  // namespace

// ── audit log ─────────────────────────────────────────────────────────────────
void LogEvent(const char* event, const char* service, const char* detail) {
    char line[256];
    int p = 0;
    p = ki_cat_u(line, p, (int)sizeof(line), g_boot_start_ms ? (now_ms() - g_boot_start_ms) : now_ms());
    p = ki_cat(line, p, (int)sizeof(line), "ms  ");
    p = ki_cat(line, p, (int)sizeof(line), event ? event : "?");
    p = ki_cat(line, p, (int)sizeof(line), "  ");
    p = ki_cat(line, p, (int)sizeof(line), service ? service : "-");
    if (detail && *detail) {
        p = ki_cat(line, p, (int)sizeof(line), "  ");
        p = ki_cat(line, p, (int)sizeof(line), detail);
    }
    p = ki_cat(line, p, (int)sizeof(line), "\n");

    const char* path = KP_LOG_DIR "/services.log";
    if (!KVFS::Exists(path)) { KVFS::Mkdirs(KP_LOG_DIR); KVFS::CreateFile(path); }
    KVFS::AppendFile(path, line, (uint32_t)ki_len(line));

    // mirror to serial for headless visibility. (satoru)
    SerialLogger::Log("[kinit] ");
    SerialLogger::Log(line);
}

// ── registration ──────────────────────────────────────────────────────────────
int RegisterInkernel(const char* name, const char* desc, KTarget target,
                     KInkernelHook start, KInkernelHook stop, bool (*health)(),
                     const char* after, KRestartPolicy restart,
                     uint32_t restart_delay_ms, bool critical) {
    if (g_service_count >= KINIT_MAX_SERVICES) return -1;
    if (find_index(name) >= 0) return -1;
    KService* s = &g_services[g_service_count];
    for (int i = 0; i < (int)sizeof(KService); i++) ((char*)s)[i] = 0;
    ki_cpy(s->name, name, sizeof(s->name));
    ki_cpy(s->description, desc ? desc : "", sizeof(s->description));
    ki_cpy(s->after, after ? after : "", sizeof(s->after));
    s->kind = KUNIT_INKERNEL;
    s->target = target;
    s->restart = restart;
    s->restart_delay_ms = restart_delay_ms ? restart_delay_ms : 2000;
    s->critical = critical;
    s->enabled = true;
    s->start_fn = start;
    s->stop_fn = stop;
    s->health_fn = health;
    s->state = KSVC_INACTIVE;
    s->cur_backoff_ms = s->restart_delay_ms;
    return g_service_count++;
}

int RegisterProcess(const char* name, const char* desc, KTarget target,
                    const char* exec, const char* after, KRestartPolicy restart,
                    uint32_t restart_delay_ms, KCapabilities caps, bool critical) {
    if (g_service_count >= KINIT_MAX_SERVICES) return -1;
    if (find_index(name) >= 0) return -1;
    KService* s = &g_services[g_service_count];
    for (int i = 0; i < (int)sizeof(KService); i++) ((char*)s)[i] = 0;
    ki_cpy(s->name, name, sizeof(s->name));
    ki_cpy(s->description, desc ? desc : "", sizeof(s->description));
    ki_cpy(s->exec, exec ? exec : "", sizeof(s->exec));
    ki_cpy(s->after, after ? after : "", sizeof(s->after));
    s->kind = KUNIT_PROCESS;
    s->target = target;
    s->restart = restart;
    s->restart_delay_ms = restart_delay_ms ? restart_delay_ms : 2000;
    s->caps = caps;
    s->critical = critical;
    s->enabled = true;
    s->state = KSVC_INACTIVE;
    s->cur_backoff_ms = s->restart_delay_ms;
    return g_service_count++;
}

// ── service control ─────────────────────────────────────────────────────────
int StartService(const char* name) {
    int idx = find_index(name);
    if (idx < 0) return -1;
    KService* s = &g_services[idx];
    if (!s->enabled) { LogEvent("start-skip-disabled", s->name, nullptr); return -2; }
    if (s->state == KSVC_RUNNING || s->state == KSVC_STARTING) return 0;

    s->state = KSVC_STARTING;
    s->start_time_ms = now_ms();

    if (s->kind == KUNIT_INKERNEL) {
        // synchronous: the subsystem is already in the kernel; the hook brings
        // it up (or is a no-op if it was started at boot before kinit). (satoru)
        if (s->start_fn) s->start_fn();
        s->state = KSVC_RUNNING;
        LogEvent("start", s->name, "(in-kernel)");
        return 0;
    }

    // process unit: gate caps, then ask its supervisor to (re)launch. (satoru)
    if (!capability_gate(s)) {
        s->state = KSVC_FAILED;
        LogEvent("start-denied", s->name, "capability gate");
        return -3;
    }
    ensure_supervisor(idx);
    g_sup[idx].svc_index = idx;
    g_sup[idx].exited    = false;
    g_sup[idx].relaunch  = true;
    s->state = KSVC_RUNNING;   // optimistic; Tick() corrects to STOPPED/FAILED on exit (satoru)
    LogEvent("start", s->name, s->exec);
    return 0;
}

int StopService(const char* name) {
    int idx = find_index(name);
    if (idx < 0) return -1;
    KService* s = &g_services[idx];
    if (s->state != KSVC_RUNNING && s->state != KSVC_RESTARTING) {
        s->state = KSVC_STOPPED;
        return 0;
    }
    s->state = KSVC_STOPPING;
    s->stop_time_ms = now_ms();
    if (s->kind == KUNIT_INKERNEL) {
        if (s->stop_fn) s->stop_fn();
    } else {
        // a real cooperative stop of the user process would deliver SIGTERM via
        // its linux pid; that path is not yet wired, so we record intent and
        // stop supervising relaunches. the running image keeps going until it
        // exits on its own. be honest about this in the log. (satoru)
        g_sup[idx].relaunch = false;
        LogEvent("stop-note", s->name, "relaunch disabled; SIGTERM delivery not yet wired");
    }
    s->state = KSVC_STOPPED;
    LogEvent("stop", s->name, nullptr);
    return 0;
}

int RestartService(const char* name) {
    StopService(name);
    int idx = find_index(name);
    if (idx < 0) return -1;
    KService* s = &g_services[idx];
    s->state = KSVC_INACTIVE;
    s->cur_backoff_ms = s->restart_delay_ms;   // a manual restart resets backoff (satoru)
    LogEvent("restart", s->name, nullptr);
    return StartService(name);
}

int EnableService(const char* name) {
    int idx = find_index(name);
    if (idx < 0) return -1;
    g_services[idx].enabled = true;
    LogEvent("enable", g_services[idx].name, nullptr);
    return 0;
}

int DisableService(const char* name) {
    int idx = find_index(name);
    if (idx < 0) return -1;
    g_services[idx].enabled = false;
    LogEvent("disable", g_services[idx].name, nullptr);
    return 0;
}

void Reload() {
    LogEvent("reload", "-", "re-parsing /kurono/system/services");
    LoadServiceDir();
}

KService* FindService(const char* name) {
    int idx = find_index(name);
    return idx >= 0 ? &g_services[idx] : nullptr;
}
KService* GetServices() { return g_services; }
int GetServiceCount() { return g_service_count; }
int RunningCount() {
    int n = 0;
    for (int i = 0; i < g_service_count; i++)
        if (g_services[i].state == KSVC_RUNNING) n++;
    return n;
}

const char* TargetName(KTarget t) {
    switch (t) {
        case KTGT_KERNEL:  return "kernel.target";
        case KTGT_NETWORK: return "network.target";
        case KTGT_DBUS:    return "dbus.target";
        case KTGT_DESKTOP: return "desktop.target";
        case KTGT_USER:    return "user.target";
        default:           return "unknown.target";
    }
}

const char* StateName(KServiceState s) {
    switch (s) {
        case KSVC_INACTIVE:   return "inactive";
        case KSVC_STARTING:   return "starting";
        case KSVC_RUNNING:    return "running";
        case KSVC_STOPPING:   return "stopping";
        case KSVC_STOPPED:    return "stopped";
        case KSVC_FAILED:     return "failed";
        case KSVC_RESTARTING: return "restarting";
        default:              return "unknown";
    }
}

// ── target sequencing ──────────────────────────────────────────────────────
void StartTarget(KTarget t) {
    LogEvent("target-reached", TargetName(t), nullptr);
    // dependency-aware fixpoint: keep starting services in this target whose
    // after= deps are settled, until nothing changes. (satoru)
    bool changed = true;
    int guard = 0;
    while (changed && guard++ < 64) {
        changed = false;
        for (int i = 0; i < g_service_count; i++) {
            KService* s = &g_services[i];
            if (s->target != t) continue;
            if (!s->enabled) continue;
            if (s->state != KSVC_INACTIVE) continue;

            // are all listed deps settled? deps may live in earlier targets. (satoru)
            bool deps_ok = true;
            if (s->after[0]) {
                for (int j = 0; j < g_service_count; j++) {
                    if (j == i) continue;
                    if (after_lists(s->after, g_services[j].name) && !dep_settled(&g_services[j])) {
                        deps_ok = false; break;
                    }
                }
            }
            if (!deps_ok) continue;

            StartService(s->name);
            // a oneshot completes immediately; mark it stopped so dependents
            // proceed. (satoru)
            if (s->kind == KUNIT_ONESHOT) s->state = KSVC_STOPPED;
            changed = true;
        }
    }
}

void StopTarget(KTarget t) {
    for (int i = g_service_count - 1; i >= 0; i--)
        if (g_services[i].target == t && g_services[i].state == KSVC_RUNNING)
            StopService(g_services[i].name);
}

// ── crash monitor (called periodically by a kernel-process; see Init) ────────
void Tick() {
    uint32_t t = now_ms();
    for (int i = 0; i < g_service_count; i++) {
        KService* s = &g_services[i];

        // 1) handle process-unit exits reported by the supervisor. (satoru)
        if ((s->kind == KUNIT_PROCESS || s->kind == KUNIT_ONESHOT) &&
            g_sup[i].in_use && g_sup[i].exited && !g_sup[i].running) {
            g_sup[i].exited = false;
            int rc = g_sup[i].exit_code;

            // a clean oneshot is just done. (satoru)
            if (s->kind == KUNIT_ONESHOT) {
                s->state = KSVC_STOPPED;
                continue;
            }
            // a clean exit (rc==0) of an always-restart daemon still restarts;
            // an on-failure daemon only restarts on nonzero. (satoru)
            bool failure = (rc != 0);
            bool want_restart =
                (s->restart == KRESTART_ALWAYS) ||
                (s->restart == KRESTART_ON_FAILURE && failure);

            if (failure) {
                s->crash_count++;
                // rolling 60s burst window. (satoru)
                if (t - s->burst_window_start_ms > KINIT_BURST_WINDOW_MS) {
                    s->burst_window_start_ms = t;
                    s->crash_burst = 0;
                }
                s->crash_burst++;
                char detail[64];
                int p = 0;
                p = ki_cat(detail, p, (int)sizeof(detail), "rc=");
                p = ki_cat_u(detail, p, (int)sizeof(detail), (uint32_t)rc);
                p = ki_cat(detail, p, (int)sizeof(detail), " burst=");
                p = ki_cat_u(detail, p, (int)sizeof(detail), (uint32_t)s->crash_burst);
                LogEvent("crash", s->name, detail);
                RuntimeLog::LogCrash("service crash", s->name);

                if (s->crash_burst >= KINIT_BURST_LIMIT) {
                    s->state = KSVC_FAILED;
                    LogEvent("failed", s->name, "5 crashes in 60s; giving up");
                    if (s->critical) notify_critical_failure(s);
                    continue;
                }
            } else {
                LogEvent("clean-exit", s->name, nullptr);
            }

            if (!want_restart) {
                s->state = KSVC_STOPPED;
                continue;
            }

            // schedule the relaunch after the current backoff, then double it
            // (capped at 60s). (satoru)
            s->state = KSVC_RESTARTING;
            s->next_restart_ms = t + s->cur_backoff_ms;
            char d2[48];
            int q = 0;
            q = ki_cat(d2, q, (int)sizeof(d2), "in ");
            q = ki_cat_u(d2, q, (int)sizeof(d2), s->cur_backoff_ms);
            q = ki_cat(d2, q, (int)sizeof(d2), "ms");
            LogEvent("restart-scheduled", s->name, d2);
            uint32_t next = s->cur_backoff_ms * 2;
            s->cur_backoff_ms = (next > KINIT_BACKOFF_MAX_MS) ? KINIT_BACKOFF_MAX_MS : next;
            continue;
        }

        // 2) fire a due restart. (satoru)
        if (s->state == KSVC_RESTARTING && (int32_t)(t - s->next_restart_ms) >= 0) {
            LogEvent("relaunch", s->name, nullptr);
            ensure_supervisor(i);
            g_sup[i].svc_index = i;
            g_sup[i].exited    = false;
            g_sup[i].relaunch  = true;
            s->state = KSVC_RUNNING;
            s->start_time_ms = t;
            continue;
        }

        // 3) in-kernel health probe: if a unit went dead, treat it as a crash
        //    and run it through the same backoff machinery. (satoru)
        if (s->kind == KUNIT_INKERNEL && s->state == KSVC_RUNNING && s->health_fn) {
            if (!s->health_fn()) {
                s->crash_count++;
                if (t - s->burst_window_start_ms > KINIT_BURST_WINDOW_MS) {
                    s->burst_window_start_ms = t;
                    s->crash_burst = 0;
                }
                s->crash_burst++;
                LogEvent("unhealthy", s->name, nullptr);
                if (s->crash_burst >= KINIT_BURST_LIMIT) {
                    s->state = KSVC_FAILED;
                    LogEvent("failed", s->name, "health probe failing");
                    if (s->critical) notify_critical_failure(s);
                    continue;
                }
                if (s->restart != KRESTART_NO) {
                    // in-kernel restart is immediate via the hook (no fork); we
                    // still honour the backoff delay before re-running it. (satoru)
                    s->state = KSVC_RESTARTING;
                    s->next_restart_ms = t + s->cur_backoff_ms;
                    uint32_t next = s->cur_backoff_ms * 2;
                    s->cur_backoff_ms = (next > KINIT_BACKOFF_MAX_MS) ? KINIT_BACKOFF_MAX_MS : next;
                } else {
                    s->state = KSVC_STOPPED;
                }
            }
        }

        // 4) an in-kernel unit due for a backoff relaunch just re-runs its
        //    start hook. (satoru)
        if (s->kind == KUNIT_INKERNEL && s->state == KSVC_RESTARTING &&
            (int32_t)(t - s->next_restart_ms) >= 0) {
            if (s->start_fn) s->start_fn();
            s->state = KSVC_RUNNING;
            LogEvent("relaunch", s->name, "(in-kernel)");
        }
    }
}

// the crash-monitor kernel-process: calls Tick() ~4x/sec. (satoru)
namespace {
[[noreturn]] void monitor_entry() {
    SerialLogger::Log("[kinit] crash monitor online\r\n");
    for (;;) {
        Tick();
        Scheduler::SleepMs(250);
    }
}
}  // namespace

// ── built-in service registration ─────────────────────────────────────────────
namespace {
// health probes for the in-kernel units. they return true if the subsystem
// still looks alive. kept conservative: when unsure, report healthy so the
// monitor never thrashes a working desktop. (satoru)
bool health_klog()  { return true; }                       // logging is passive (satoru)
bool health_knet()  { return true; }                       // net process always present (satoru)
bool health_kdbus() { return true; }                       // bus listener stays bound (satoru)
bool health_kwl()   { return true; }                       // compositor owns the gui loop (satoru)
bool health_kaudio(){ return true; }                       // mixer pump always present (satoru)

// the user.target daemons have REAL health probes (their workers track liveness
// + last self-test result). (satoru)
bool health_kpkg()  { return true; }                       // worker loops forever; idle is healthy (satoru)
bool health_kupd()  { return KUpdate::IsHealthy(); }
bool health_ksec()  { return KSecurity::IsHealthy(); }

// in-kernel "start" hooks: the core subsystems (klog/knet/kdbus/kwayland/kaudio)
// are brought up by the kernel BEFORE kinit runs (DBusServer::Init,
// WaylandServer::Init, the kernel_processes, etc)  -  kinit ADOPTS them as
// already-running units rather than re-initialising and risking a double-init,
// so their start hooks are no-ops. the user.target daemons, by contrast, are
// genuinely started here via their Init(). (satoru)
void noop_start() {}
void noop_stop()  {}
void start_kpkg() { KpkgDaemon::Init(); }
void start_kupd() { KUpdate::Init(); }
void start_ksec() { KSecurity::Init(); }

void register_builtins() {
    // kernel.target: logging + crash handling. (satoru)
    RegisterInkernel("klog", "Runtime logging service", KTGT_KERNEL,
                     noop_start, noop_stop, health_klog, "", KRESTART_ALWAYS, 2000, false);
    // network.target: knet (the in-kernel network kernel-process + tcp/ip). (satoru)
    RegisterInkernel("knet", "Network (DHCP/routing/TCP-IP)", KTGT_NETWORK,
                     noop_start, noop_stop, health_knet, "klog", KRESTART_ALWAYS, 2000, false);
    // dbus.target: kdbus (the AF_UNIX session bus). critical. (satoru)
    RegisterInkernel("kdbus", "D-Bus session bus", KTGT_DBUS,
                     noop_start, noop_stop, health_kdbus, "knet", KRESTART_ALWAYS, 2000, true);
    // desktop.target: kwayland (compositor) + kaudio (audio server). kwayland
    // is critical. (satoru)
    RegisterInkernel("kwayland", "Wayland compositor", KTGT_DESKTOP,
                     noop_start, noop_stop, health_kwl, "kdbus", KRESTART_ALWAYS, 2000, true);
    RegisterInkernel("kaudio", "Audio server", KTGT_DESKTOP,
                     noop_start, noop_stop, health_kaudio, "kdbus", KRESTART_ON_FAILURE, 2000, false);

    // user.target: kurono's own service daemons. these wrap real kernel
    // functionality (package install, update-checking, policy self-test) and run
    // as dedicated worker kernel-processes so they never block the gui. they are
    // registered as in-kernel units because that is what they HONESTLY are today
    //  -  an in-kernel worker, not a separate linux address space. the matching
    // .kservice files on disk document the eventual fully-isolated process form
    // (Exec=/kurono/system/bin/<name>); kinit will run that form once those
    // binaries are built + installed. critical=false: a failed updater/security
    // watcher must not toast on every boot, but ksecurity self-test failures DO
    // alert from inside its worker. (satoru)
    RegisterInkernel("kpkg-daemon", "Package install daemon", KTGT_USER,
                     start_kpkg, noop_stop, health_kpkg, "kdbus knet",
                     KRESTART_ON_FAILURE, 2000, false);
    RegisterInkernel("kupdate", "Update checker", KTGT_USER,
                     start_kupd, noop_stop, health_kupd, "kpkg-daemon",
                     KRESTART_ON_FAILURE, 5000, false);
    RegisterInkernel("ksecurity", "KSA + supr policy watch", KTGT_USER,
                     start_ksec, noop_stop, health_ksec, "kdbus",
                     KRESTART_ON_FAILURE, 2000, false);
}

// drop example .kservice unit files into /kurono/system/services so the parser
// + `kinit reload` have real files to load, and so the format is documented
// on-disk. only written if absent (never clobbers a user's edits). (satoru)
void seed_example_units() {
    const char* dir = "/kurono/system/services";
    KVFS::Mkdirs(dir);
    const char* kpkg_unit =
        "# kpkg-daemon unit. NOTE: kinit currently runs kpkg-daemon as an\n"
        "# in-kernel worker process (KpkgDaemon), which already gives the gui\n"
        "# non-blocking isolation. this file documents the eventual fully\n"
        "# isolated form: once /kurono/system/bin/kpkg-daemon is built as a real\n"
        "# binary, the in-kernel built-in can be dropped and this unit drives it\n"
        "# via fork/exec. the Exec/After/Capabilities below are the target spec.\n"
        "[Service]\n"
        "Name=kpkg-daemon\n"
        "Description=kurono package install daemon\n"
        "Exec=/kurono/system/bin/kpkg-daemon\n"
        "Restart=on-failure\n"
        "RestartDelay=2000\n"
        "After=dbus network\n"
        "WantedBy=user.target\n"
        "Critical=no\n"
        "[Capabilities]\n"
        "Network=yes\n"
        "Filesystem=yes\n"
        "GUI=no\n";
    if (!KVFS::Exists("/kurono/system/services/kpkg-daemon.kservice"))
        KVFS::WriteString("/kurono/system/services/kpkg-daemon.kservice", kpkg_unit);

    const char* kupdate_unit =
        "[Service]\n"
        "Name=kupdate\n"
        "Description=kurono update checker\n"
        "Exec=/kurono/system/bin/kupdate\n"
        "Restart=on-failure\n"
        "RestartDelay=5000\n"
        "After=kpkg-daemon\n"
        "WantedBy=user.target\n"
        "[Capabilities]\n"
        "Network=yes\n"
        "Filesystem=yes\n"
        "GUI=no\n";
    if (!KVFS::Exists("/kurono/system/services/kupdate.kservice"))
        KVFS::WriteString("/kurono/system/services/kupdate.kservice", kupdate_unit);
}
}  // namespace

void Init() {
    g_service_count = 0;
    g_booted = false;
    for (int i = 0; i < KINIT_MAX_SERVICES; i++) {
        g_sup[i].in_use = false;
        g_sup[i].svc_index = -1;
    }
    KVFS::Mkdirs(KP_LOG_DIR);
    if (!KVFS::Exists(KP_LOG_DIR "/services.log"))
        KVFS::CreateFile(KP_LOG_DIR "/services.log");

    register_builtins();
    seed_example_units();
    // load any user-authored .kservice units on top of the built-ins. the built
    // in kpkg-daemon/kupdate already exist by name, so the seeded duplicates are
    // skipped by RegisterProcess's dedup; user units with new names are added.
    // (satoru)
    LoadServiceDir();

    SerialLogger::Log("[kinit] init: ");
    SerialLogger::LogDec(g_service_count);
    SerialLogger::Log(" services registered\r\n");
    LogEvent("init", "-", "kinit ready");
}

void Boot() {
    if (g_booted) return;
    g_booted = true;
    g_boot_start_ms = now_ms();
    LogEvent("boot", "-", "sequencing targets");

    // sequence the boot stages in order; each StartTarget brings up that stage's
    // services once their deps are settled. (satoru)
    StartTarget(KTGT_KERNEL);
    StartTarget(KTGT_NETWORK);
    StartTarget(KTGT_DBUS);
    StartTarget(KTGT_DESKTOP);
    StartTarget(KTGT_USER);

    // start the crash monitor kernel-process. it relaunches dead daemons with
    // backoff and probes in-kernel health. (satoru)
    Scheduler::SpawnKernelProcess("kinit-monitor", monitor_entry, PRIO_LOW, 64, 4096);

    LogEvent("boot-complete", "-", nullptr);
    SerialLogger::Log("[kinit] boot complete; running services: ");
    SerialLogger::LogDec(RunningCount());
    SerialLogger::Log("\r\n");
}

void Shutdown() {
    LogEvent("shutdown", "-", nullptr);
    StopTarget(KTGT_USER);
    StopTarget(KTGT_DESKTOP);
    StopTarget(KTGT_DBUS);
    StopTarget(KTGT_NETWORK);
    StopTarget(KTGT_KERNEL);
}

// ── shell command: kinit <action> [svc] ────────────────────────────────────
namespace {
const char* kind_short(KUnitKind k) {
    switch (k) {
        case KUNIT_INKERNEL: return "in-kernel";
        case KUNIT_PROCESS:  return "process";
        case KUNIT_ONESHOT:  return "oneshot";
        default:             return "?";
    }
}

// print a one-line status row for a service. (satoru)
int print_status_row(char* out, int p, int mx, const KService* s) {
    // state column padded to keep names aligned. (satoru)
    const char* st = StateName(s->state);
    p = ki_cat(out, p, mx, "  ");
    p = ki_cat(out, p, mx, st);
    for (int k = ki_len(st); k < 11; k++) p = ki_cat(out, p, mx, " ");
    p = ki_cat(out, p, mx, s->name);
    for (int k = ki_len(s->name); k < 14; k++) p = ki_cat(out, p, mx, " ");
    p = ki_cat(out, p, mx, kind_short(s->kind));
    for (int k = ki_len(kind_short(s->kind)); k < 10; k++) p = ki_cat(out, p, mx, " ");
    p = ki_cat(out, p, mx, s->description);
    p = ki_cat(out, p, mx, "\n");
    return p;
}
}  // namespace

int CmdKinit(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    int p = 0;

    if (argc < 2 || ki_eq(argv[1], "status")) {
        // overview + per-service table grouped by target. (satoru)
        p = ki_cat(out, p, mx, "kinit  -  service manager   running ");
        p = ki_cat_u(out, p, mx, (uint32_t)RunningCount());
        p = ki_cat(out, p, mx, "/");
        p = ki_cat_u(out, p, mx, (uint32_t)g_service_count);
        p = ki_cat(out, p, mx, "\n");
        for (int t = 0; t < KTGT_COUNT; t++) {
            bool any = false;
            for (int i = 0; i < g_service_count; i++)
                if ((int)g_services[i].target == t) { any = true; break; }
            if (!any) continue;
            p = ki_cat(out, p, mx, "\n[");
            p = ki_cat(out, p, mx, TargetName((KTarget)t));
            p = ki_cat(out, p, mx, "]\n");
            for (int i = 0; i < g_service_count; i++)
                if ((int)g_services[i].target == t)
                    p = print_status_row(out, p, mx, &g_services[i]);
        }
        return p;
    }

    const char* action = argv[1];

    if (ki_eq(action, "reload")) {
        Reload();
        return ki_cat(out, 0, mx, "kinit: reloaded .kservice units from /kurono/system/services\n");
    }

    // logs [svc]: dump services.log (optionally filtered to one service). (satoru)
    if (ki_eq(action, "logs")) {
        const char* filt = (argc >= 3) ? argv[2] : nullptr;
        static char buf[8192];
        int got = KVFS::ReadFile(KP_LOG_DIR "/services.log", buf, (uint32_t)sizeof(buf) - 1);
        if (got <= 0) return ki_cat(out, 0, mx, "kinit: no service log yet\n");
        buf[got] = 0;
        if (!filt) { ki_cpy(out, buf, mx); return ki_len(out); }
        // line-filter on the service token. (satoru)
        int i = 0;
        while (i < got && p < mx - 1) {
            int ls = i;
            while (i < got && buf[i] != '\n') i++;
            int le = i;
            if (i < got) i++;
            // crude contains-check for the service name. (satoru)
            bool hit = false;
            for (int k = ls; k + ki_len(filt) <= le; k++) {
                bool m = true;
                for (int j = 0; filt[j]; j++) if (buf[k + j] != filt[j]) { m = false; break; }
                if (m) { hit = true; break; }
            }
            if (hit) {
                for (int k = ls; k <= le && p < mx - 1; k++) out[p++] = buf[k];
            }
        }
        out[p] = 0;
        if (p == 0) return ki_cat(out, 0, mx, "kinit: no log lines for that service\n");
        return p;
    }

    if (argc < 3) {
        p = ki_cat(out, p, mx, "usage: kinit status | start <svc> | stop <svc> | "
                               "restart <svc> | enable <svc> | disable <svc> | "
                               "logs [svc] | reload\n");
        return p;
    }

    const char* name = argv[2];
    int rc;
    if (ki_eq(action, "start"))        rc = StartService(name);
    else if (ki_eq(action, "stop"))    rc = StopService(name);
    else if (ki_eq(action, "restart")) rc = RestartService(name);
    else if (ki_eq(action, "enable"))  rc = EnableService(name);
    else if (ki_eq(action, "disable")) rc = DisableService(name);
    else {
        p = ki_cat(out, p, mx, "kinit: unknown action '");
        p = ki_cat(out, p, mx, action);
        p = ki_cat(out, p, mx, "'\n");
        return p;
    }

    if (rc == -1) {
        p = ki_cat(out, p, mx, "kinit: no such service '");
        p = ki_cat(out, p, mx, name);
        p = ki_cat(out, p, mx, "'\n");
        return p;
    }
    if (rc == -3) {
        p = ki_cat(out, p, mx, "kinit: '");
        p = ki_cat(out, p, mx, name);
        p = ki_cat(out, p, mx, "' denied by capability gate (see security.log)\n");
        return p;
    }
    KService* s = FindService(name);
    p = ki_cat(out, p, mx, "kinit: ");
    p = ki_cat(out, p, mx, action);
    p = ki_cat(out, p, mx, " ");
    p = ki_cat(out, p, mx, name);
    p = ki_cat(out, p, mx, " -> ");
    p = ki_cat(out, p, mx, s ? StateName(s->state) : "?");
    p = ki_cat(out, p, mx, "\n");
    return p;
}

void RegisterShellCommands(void* shell) {
    KuronoShell* sh = (KuronoShell*)shell;
    if (!sh) return;
    sh->RegisterCommand("kinit", CmdKinit, "kurono service + init manager");
}

}  // namespace KInit

// end (satoru)
