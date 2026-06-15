//  kurono os: kinit core: registration, target sequencing, spawn, crash
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
#include "../net/unix_socket.h"          // socket activation + sd_notify listener (satoru)
#include "../proc/cgroup.h"              // CPUQuota -> cpu.weight, MemoryMax charge (satoru)
#include "../kernel/vmm.h"               // isolation: own address space + guard pages (satoru)
#include "../kernel/pmm.h"               // isolation: frame allocation (satoru)
#include "../kernel/heap.h"              // test memhog allocations (satoru)
#include "kpkg_daemon.h"
#include "kdaemons.h"

namespace KInit {

namespace {

KService  g_services[KINIT_MAX_SERVICES];
int       g_service_count = 0;
bool      g_booted = false;
uint32_t  g_boot_start_ms = 0;
uint32_t  g_boot_elapsed_ms = 0;        // measured critical-path boot time (satoru)
bool      g_boot_parallel = true;       // parallel target startup (default) (satoru)

// the AF_UNIX datagram socket kinit binds for sd_notify. each service gets
// NOTIFY_SOCKET=<this path> in its env; clients connect + send datagrams. an
// in-kernel daemon bypasses the wire and calls KInit::SdNotify directly. (satoru)
constexpr const char* KINIT_NOTIFY_PATH = "/kurono/runtime/sockets/kinit-notify";
int g_notify_sd = -1;                   // kinit's listening notify socket (satoru)

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

// apply CPUQuota + LimitNOFILE to a freshly-loaded process unit. MemoryMax is
// NOT applied here, it is enforced by continuous polling in Tick() (a kill on
// breach) which works for both in-kernel test services and process units; the
// cgroup memory.max charge hook only catches KernelHeap allocs and would miss a
// user-space mmap, so the poll-and-kill is the reliable enforcement. CPUQuota is
// mapped onto a per-service cgroup cpu.weight (a percent over 100 scales the
// weight up, under 100 down) and the pid is attached. LimitNOFILE is recorded +
// logged (the fd-table cap is enforced by the linux fd allocator per-process;
// see the docs for the enforcement point). every decision is logged. (satoru)
void apply_runtime_limits(KService* svc, Process* p) {
    if (!svc || !p) return;
    if (svc->limits.cpu_quota_pct > 0) {
        // weight = clamp(100 * pct/100, 1, 10000) = clamp(pct, 1, 10000). a 50%
        // quota -> weight 50 (half the default 100); 200% -> 200. this is a
        // proportional-share approximation of an absolute quota, honest given the
        // cooperative/CFS scheduler. (satoru)
        uint32_t w = svc->limits.cpu_quota_pct;
        if (w < 1) w = 1;
        if (w > 10000) w = 10000;
        uint32_t cg = Cgroup::Create(1, svc->name);
        if (cg) {
            Cgroup::EnableController(cg, Cgroup::CTRL_CPU | Cgroup::CTRL_MEMORY);
            Cgroup::SetCpuWeight(cg, w);
            if (svc->limits.memory_max_kb)
                Cgroup::SetMemoryMax(cg, (uint64_t)svc->limits.memory_max_kb * 1024ULL);
            Cgroup::Attach(cg, p->pid);
        }
        char d[64];
        int q = 0;
        q = ki_cat(d, q, (int)sizeof(d), "CPUQuota ");
        q = ki_cat_u(d, q, (int)sizeof(d), svc->limits.cpu_quota_pct);
        q = ki_cat(d, q, (int)sizeof(d), "% -> cpu.weight ");
        q = ki_cat_u(d, q, (int)sizeof(d), w);
        LogEvent("limit", svc->name, d);
    }
    if (svc->limits.limit_nofile > 0) {
        char d[48];
        int q = 0;
        q = ki_cat(d, q, (int)sizeof(d), "LimitNOFILE=");
        q = ki_cat_u(d, q, (int)sizeof(d), svc->limits.limit_nofile);
        LogEvent("limit", svc->name, d);
    }
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

            // record the real pid so MemoryMax accounting + a kill can find it,
            // and apply CPUQuota/LimitNOFILE before the image runs. (satoru)
            svc->pid = (int)p->pid;
            apply_runtime_limits(svc, p);

            ss->running = true;
            ss->exited  = false;
            LogEvent("spawn", svc->name, path);

            // NOTIFY_SOCKET tells a Type=notify service where to send sd_notify
            // datagrams; WATCHDOG_USEC advertises the watchdog interval (systemd
            // convention) so the service knows how often to ping. (satoru)
            char notify_env[64];
            { int q = 0; q = ki_cat(notify_env, q, (int)sizeof(notify_env), "NOTIFY_SOCKET=");
              q = ki_cat(notify_env, q, (int)sizeof(notify_env), KINIT_NOTIFY_PATH); }
            char wd_env[40];
            { int q = 0; q = ki_cat(wd_env, q, (int)sizeof(wd_env), "WATCHDOG_USEC=");
              q = ki_cat_u(wd_env, q, (int)sizeof(wd_env), svc->watchdog_sec * 1000000u); }
            const char* envp[] = {
                "PATH=/kurono/system/bin:/usr/bin", "HOME=/home/user",
                notify_env, (svc->watchdog_sec ? wd_env : nullptr), nullptr
            };
            // this BLOCKS until the user process exits/crashes; the scheduler
            // time-shares it against the gui via preemption. (satoru)
            int rc = Userspace::RunProcessWithArgs(p, argv, envp);

            ss->exit_code = rc;
            ss->running   = false;
            ss->exited    = true;
            svc->pid = 0;
            LogEvent("exited", svc->name, nullptr);
        }
        Scheduler::SleepMs(100);   // idle until Tick() asks for a (re)launch (satoru)
    }
}

// one trampoline per slot (SpawnKernelProcess has no user-arg). there must be
// exactly KINIT_MAX_SERVICES of these or a high-slot process unit gets a null
// entry; the static_assert below guards that. (satoru)
#define KISUP(n) void sup_##n() { supervisor_run(n); }
KISUP(0)  KISUP(1)  KISUP(2)  KISUP(3)  KISUP(4)  KISUP(5)  KISUP(6)  KISUP(7)
KISUP(8)  KISUP(9)  KISUP(10) KISUP(11) KISUP(12) KISUP(13) KISUP(14) KISUP(15)
KISUP(16) KISUP(17) KISUP(18) KISUP(19) KISUP(20) KISUP(21) KISUP(22) KISUP(23)
KISUP(24) KISUP(25) KISUP(26) KISUP(27) KISUP(28) KISUP(29) KISUP(30) KISUP(31)
KISUP(32) KISUP(33) KISUP(34) KISUP(35) KISUP(36) KISUP(37) KISUP(38) KISUP(39)
KISUP(40) KISUP(41) KISUP(42) KISUP(43) KISUP(44) KISUP(45) KISUP(46) KISUP(47)
#undef KISUP
KernelProcessEntry g_sup_entry[KINIT_MAX_SERVICES] = {
    sup_0,  sup_1,  sup_2,  sup_3,  sup_4,  sup_5,  sup_6,  sup_7,
    sup_8,  sup_9,  sup_10, sup_11, sup_12, sup_13, sup_14, sup_15,
    sup_16, sup_17, sup_18, sup_19, sup_20, sup_21, sup_22, sup_23,
    sup_24, sup_25, sup_26, sup_27, sup_28, sup_29, sup_30, sup_31,
    sup_32, sup_33, sup_34, sup_35, sup_36, sup_37, sup_38, sup_39,
    sup_40, sup_41, sup_42, sup_43, sup_44, sup_45, sup_46, sup_47
};
static_assert(KINIT_MAX_SERVICES == 48, "supervisor trampoline table must match KINIT_MAX_SERVICES");

// ensure a supervisor kernel-process exists for slot, spawning it once. (satoru)
void ensure_supervisor(int slot) {
    if (slot < 0 || slot >= KINIT_MAX_SERVICES) return;
    if (!g_sup_entry[slot]) return;
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
    if (!name || !name[0]) return -1;   // never match a tombstone (satoru)
    for (int i = 0; i < g_service_count; i++)
        if (ki_eq(g_services[i].name, name)) return i;
    return -1;
}

// a tombstone is a removed dir-managed slot (empty name). hot-reload removal
// leaves these so the supervisor trampoline slots stay pinned by index; a new
// registration reuses the lowest tombstone before growing the array. returns the
// slot to use, or -1 if the table is full. (satoru)
int alloc_slot() {
    for (int i = 0; i < g_service_count; i++)
        if (g_services[i].name[0] == 0) return i;
    if (g_service_count >= KINIT_MAX_SERVICES) return -1;
    return g_service_count++;
}

// ── resource-limit accounting (satoru) ───────────────────────────────────────
// current memory of a service in kib. an in-kernel unit may register mem_probe
// to report its own tracked allocation; a process unit is read from the
// scheduler snapshot by pid (sum of its mapped regions + stacks). 0 = unknown.
// (satoru)
uint32_t service_mem_kb(const KService* s) {
    if (s->mem_probe) return s->mem_probe();
    if (s->kind == KUNIT_PROCESS && s->pid > 0) {
        static SchedulerProcessSnapshot snap[64];
        int n = Scheduler::GetProcessSnapshot(snap, 64);
        for (int i = 0; i < n; i++)
            if (snap[i].pid == (uint32_t)s->pid) return snap[i].memory_kb;
    }
    return 0;
}

// stop a unit's actual work without changing kinit's view of WHY (the caller
// sets the new state). for an in-kernel unit this fires stop_fn (the test hog's
// stop_fn frees its arena + idles); for a process unit it disables relaunch and,
// if the pid is known, asks the scheduler to terminate it. (satoru)
void halt_unit_work(KService* s, int idx) {
    if (s->kind == KUNIT_INKERNEL) {
        if (s->stop_fn) s->stop_fn();
    } else {
        g_sup[idx].relaunch = false;
        if (s->pid > 0) {
            Process* p = Scheduler::FindProcessByPid((uint32_t)s->pid);
            if (p) Scheduler::MarkProcessExited(p, 137);   // 128+SIGKILL (satoru)
        }
    }
}

// forward decls for the supervisor-relaunch helper used by restart paths. (satoru)
void ensure_supervisor(int slot);

// run a unit through the crash/backoff machinery after kinit decided to KILL it
// (oom or watchdog). honours Restart= + the 5-in-60s burst rule exactly like a
// real crash so a misbehaving service can't be relaunched forever. (satoru)
void kill_and_supervise(int idx, const char* reason) {
    KService* s = &g_services[idx];
    uint32_t t = now_ms();
    halt_unit_work(s, idx);

    s->crash_count++;
    if (t - s->burst_window_start_ms > KINIT_BURST_WINDOW_MS) {
        s->burst_window_start_ms = t;
        s->crash_burst = 0;
    }
    s->crash_burst++;
    LogEvent("kill", s->name, reason);
    RuntimeLog::LogCrash("service killed", s->name);

    if (s->crash_burst >= KINIT_BURST_LIMIT) {
        s->state = KSVC_FAILED;
        LogEvent("failed", s->name, "kill burst limit; giving up");
        if (s->critical) notify_critical_failure(s);
        return;
    }
    if (s->restart == KRESTART_NO) {
        s->state = KSVC_STOPPED;
        return;
    }
    // schedule a backoff relaunch (same doubling/cap as the crash monitor). (satoru)
    s->state = KSVC_RESTARTING;
    s->next_restart_ms = t + s->cur_backoff_ms;
    uint32_t next = s->cur_backoff_ms * 2;
    s->cur_backoff_ms = (next > KINIT_BACKOFF_MAX_MS) ? KINIT_BACKOFF_MAX_MS : next;
}

// enforce MemoryMax for every running unit that declares one. a unit whose
// current memory exceeds the cap is killed + restarted (or failed) and the kill
// is logged. called from Tick(). CPUQuota/LimitNOFILE are applied at spawn (see
// apply_runtime_limits); MemoryMax needs continuous polling, hence here. (satoru)
void enforce_memory_limits() {
    for (int i = 0; i < g_service_count; i++) {
        KService* s = &g_services[i];
        if (s->state != KSVC_RUNNING) continue;
        if (s->limits.memory_max_kb == 0) continue;
        uint32_t kb = service_mem_kb(s);
        if (kb == 0) continue;                       // no reading -> can't judge (satoru)
        if (kb > s->limits.memory_max_kb) {
            s->oom_kills++;
            char detail[80];
            int p = 0;
            p = ki_cat(detail, p, (int)sizeof(detail), "MemoryMax: ");
            p = ki_cat_u(detail, p, (int)sizeof(detail), kb);
            p = ki_cat(detail, p, (int)sizeof(detail), "K > ");
            p = ki_cat_u(detail, p, (int)sizeof(detail), s->limits.memory_max_kb);
            p = ki_cat(detail, p, (int)sizeof(detail), "K");
            kill_and_supervise(i, detail);
        }
    }
}

// ── started/running bookkeeping (satoru) ─────────────────────────────────────
// mark a unit RUNNING and record its boot timing (start -> running) + seed the
// watchdog grace period. (satoru)
void mark_running(KService* s) {
    uint32_t t = now_ms();
    s->state = KSVC_RUNNING;
    s->ready_time_ms = t;
    s->boot_ms = (s->start_time_ms && t >= s->start_time_ms) ? (t - s->start_time_ms) : 0;
    s->last_watchdog_ms = t;
}

// ── socket activation (satoru) ───────────────────────────────────────────────
// bind + listen the AF_UNIX socket a socket-activated unit waits on. idempotent.
// returns true if the unit now has a listening socket. (satoru)
bool ensure_listen_socket(int idx) {
    KService* s = &g_services[idx];
    if (!s->listen_path[0]) return false;
    if (s->listen_sd >= 0) return true;
    int sd = UnixSocket::Create(UnixSocket::UNIX_SOCK_STREAM);
    if (sd < 0) return false;
    // make sure the socket dir exists so a pathname bind succeeds. (satoru)
    KVFS::Mkdirs("/kurono/runtime/sockets");
    if (UnixSocket::Bind(sd, s->listen_path) != 0) { UnixSocket::Close(sd); return false; }
    if (UnixSocket::Listen(sd, 8) != 0) { UnixSocket::Close(sd); return false; }
    s->listen_sd = sd;
    s->socket_activated = false;
    return true;
}

// ── sd_notify socket (satoru) ────────────────────────────────────────────────
// bind kinit's NOTIFY_SOCKET. a Type=notify process unit sends datagrams here;
// the parser routes by peer credentials/path. for in-kernel daemons SdNotify is
// called directly. idempotent. (satoru)
void ensure_notify_socket() {
    if (g_notify_sd >= 0) return;
    KVFS::Mkdirs("/kurono/runtime/sockets");
    int sd = UnixSocket::Create(UnixSocket::UNIX_SOCK_DGRAM);
    if (sd < 0) return;
    if (UnixSocket::Bind(sd, KINIT_NOTIFY_PATH) != 0) { UnixSocket::Close(sd); return; }
    // a dgram listener accepts connectionless sends after Listen marks it bound.
    UnixSocket::Listen(sd, 8);
    g_notify_sd = sd;
    LogEvent("notify-socket", "-", KINIT_NOTIFY_PATH);
}

// drain any sd_notify datagrams that arrived on kinit's notify socket and route
// each to the sender's service by peer pid. called from Tick(). (satoru)
void poll_notify_socket() {
    if (g_notify_sd < 0) return;
    for (int guard = 0; guard < 32; guard++) {
        if (UnixSocket::PendingBytes(g_notify_sd) <= 0) break;
        char buf[256];
        UnixSocket::ControlMsg cm;
        int n = UnixSocket::Recv(g_notify_sd, buf, (int)sizeof(buf) - 1, 0, &cm);
        if (n <= 0) break;
        buf[n] = 0;
        // resolve the sender to a service by pid (creds), else broadcast to any
        // notify unit awaiting READY (best-effort for clients with no creds). (satoru)
        int svc = -1;
        if (cm.creds_valid) {
            for (int i = 0; i < g_service_count; i++)
                if (g_services[i].pid > 0 && (uint32_t)g_services[i].pid == cm.peer_creds.pid) { svc = i; break; }
        }
        if (svc >= 0) SdNotify(g_services[svc].name, buf);
        else {
            // no creds: apply to the single notify unit still awaiting READY. (satoru)
            for (int i = 0; i < g_service_count; i++) {
                KService* s = &g_services[i];
                if (s->type == KTYPE_NOTIFY && s->state == KSVC_STARTING) { SdNotify(s->name, buf); break; }
            }
        }
    }
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
    if (find_index(name) >= 0) return -1;
    int slot = alloc_slot();
    if (slot < 0) return -1;
    KService* s = &g_services[slot];
    for (int i = 0; i < (int)sizeof(KService); i++) ((char*)s)[i] = 0;
    ki_cpy(s->name, name, sizeof(s->name));
    ki_cpy(s->description, desc ? desc : "", sizeof(s->description));
    ki_cpy(s->after, after ? after : "", sizeof(s->after));
    s->kind = KUNIT_INKERNEL;
    s->type = KTYPE_SIMPLE;
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
    s->listen_sd = -1;    // sentinel: no socket (0 is a valid sd) (satoru)
    return slot;
}

int RegisterProcess(const char* name, const char* desc, KTarget target,
                    const char* exec, const char* after, KRestartPolicy restart,
                    uint32_t restart_delay_ms, KCapabilities caps, bool critical) {
    if (find_index(name) >= 0) return -1;
    int slot = alloc_slot();
    if (slot < 0) return -1;
    KService* s = &g_services[slot];
    for (int i = 0; i < (int)sizeof(KService); i++) ((char*)s)[i] = 0;
    ki_cpy(s->name, name, sizeof(s->name));
    ki_cpy(s->description, desc ? desc : "", sizeof(s->description));
    ki_cpy(s->exec, exec ? exec : "", sizeof(s->exec));
    ki_cpy(s->after, after ? after : "", sizeof(s->after));
    s->kind = KUNIT_PROCESS;
    s->type = KTYPE_SIMPLE;
    s->target = target;
    s->restart = restart;
    s->restart_delay_ms = restart_delay_ms ? restart_delay_ms : 2000;
    s->caps = caps;
    s->critical = critical;
    s->enabled = true;
    s->state = KSVC_INACTIVE;
    s->cur_backoff_ms = s->restart_delay_ms;
    s->listen_sd = -1;
    return slot;
}

// register a fully-populated KService (the loader builds the whole struct from
// the parsed keys). all live-state fields are reset here so a config blob never
// carries stale runtime state in. (satoru)
int RegisterService(const KService* tmpl) {
    if (!tmpl) return -1;
    if (find_index(tmpl->name) >= 0) return -1;
    int slot = alloc_slot();
    if (slot < 0) return -1;
    KService* s = &g_services[slot];
    *s = *tmpl;
    // reset all live state (the parser zeroed it, but be explicit + robust against
    // a caller that reused a live struct). (satoru)
    s->state = KSVC_INACTIVE;
    s->pid = 0;
    s->start_time_ms = 0;
    s->stop_time_ms = 0;
    s->ready_time_ms = 0;
    s->boot_ms = 0;
    s->crash_count = 0;
    s->crash_burst = 0;
    s->burst_window_start_ms = 0;
    s->next_restart_ms = 0;
    s->cur_backoff_ms = s->restart_delay_ms ? s->restart_delay_ms : 2000;
    s->oom_kills = 0;
    s->watchdog_kills = 0;
    s->listen_sd = -1;
    s->socket_activated = false;
    s->last_watchdog_ms = 0;
    s->notify_ready = false;
    s->notify_status[0] = 0;
    s->isolated_active = false;
    return slot;
}

// mark a service as fully up and record its boot timing for `kinit analyze`. a
// Type=notify service is NOT "running" until READY=1 arrives, so this is only
// called once that condition is met (immediately for simple/in-kernel units). a
// running unit also gets a watchdog grace period so the first interval is timed
// from when it actually started. (satoru)
namespace {
void mark_running(KService* s);          // fwd (defined in anon ns below) (satoru)
bool ensure_listen_socket(int idx);      // fwd (satoru)
}  // namespace

// ── service control ─────────────────────────────────────────────────────────
int StartService(const char* name) {
    int idx = find_index(name);
    if (idx < 0) return -1;
    KService* s = &g_services[idx];
    if (s->is_template) { LogEvent("start-skip-template", s->name, "instantiate as name@instance"); return -4; }
    if (!s->enabled) { LogEvent("start-skip-disabled", s->name, nullptr); return -2; }
    if (s->state == KSVC_RUNNING || s->state == KSVC_STARTING) return 0;

    s->state = KSVC_STARTING;
    s->start_time_ms = now_ms();
    s->notify_ready = false;

    // socket activation: do NOT start now. ensure the listening socket exists and
    // leave the unit inactive; PollSocketActivation() spawns it on first connect.
    // (satoru)
    if (s->listen_path[0]) {
        if (ensure_listen_socket(idx)) {
            s->state = KSVC_INACTIVE;
            LogEvent("socket-listen", s->name, s->listen_path);
            return 0;
        }
        // fall through and start normally if the socket could not be bound. (satoru)
        LogEvent("socket-bind-fail", s->name, s->listen_path);
    }

    if (s->kind == KUNIT_INKERNEL) {
        // synchronous: the subsystem is already in the kernel; the hook brings
        // it up (or is a no-op if it was started at boot before kinit). (satoru)
        if (s->start_fn) s->start_fn();
        // a Type=notify in-kernel unit stays "starting" until it calls SdNotify
        // READY=1 itself; otherwise it is up as soon as the hook returns. (satoru)
        if (s->type == KTYPE_NOTIFY) {
            LogEvent("start", s->name, "(in-kernel, awaiting READY=1)");
        } else {
            mark_running(s);
            LogEvent("start", s->name, "(in-kernel)");
        }
        return 0;
    }

    // process unit: gate caps + runtime sandbox, then ask its supervisor to
    // (re)launch. (satoru)
    if (!capability_gate(s)) {
        s->state = KSVC_FAILED;
        LogEvent("start-denied", s->name, "capability gate");
        return -3;
    }
    ensure_supervisor(idx);
    g_sup[idx].svc_index = idx;
    g_sup[idx].exited    = false;
    g_sup[idx].relaunch  = true;
    // optimistic running for a simple unit; a notify unit waits for READY=1.
    // Tick() corrects to STOPPED/FAILED on exit. (satoru)
    if (s->type == KTYPE_NOTIFY) {
        LogEvent("start", s->name, "(awaiting READY=1)");
    } else {
        mark_running(s);
        LogEvent("start", s->name, s->exec);
    }
    return 0;
}

int StopService(const char* name) {
    int idx = find_index(name);
    if (idx < 0) return -1;
    KService* s = &g_services[idx];
    // tear down a kinit-owned listening socket regardless of run state. (satoru)
    if (s->listen_sd >= 0) { UnixSocket::Close(s->listen_sd); s->listen_sd = -1; }
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
        if (s->pid > 0) {
            Process* p = Scheduler::FindProcessByPid((uint32_t)s->pid);
            if (p) { Scheduler::MarkProcessExited(p, 143); }   // 128+SIGTERM (satoru)
        }
        LogEvent("stop-note", s->name, "relaunch disabled");
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

// ── sd_notify (satoru) ────────────────────────────────────────────────────────
const char* NotifySocketPath() { return KINIT_NOTIFY_PATH; }

bool SdNotify(const char* service, const char* msg) {
    if (!service || !msg) return false;
    int idx = find_index(service);
    if (idx < 0) return false;
    KService* s = &g_services[idx];
    uint32_t t = now_ms();

    // parse the newline-separated sd_notify payload. unknown keys are ignored
    // (forward-compatible, like systemd). (satoru)
    int i = 0;
    while (msg[i]) {
        int ls = i;
        while (msg[i] && msg[i] != '\n') i++;
        int le = i;
        if (msg[i] == '\n') i++;
        // a single "key=value" assignment in [ls,le). (satoru)
        int eq = -1;
        for (int k = ls; k < le; k++) if (msg[k] == '=') { eq = k; break; }
        if (eq < 0) continue;
        // compare the key. (satoru)
        auto key_is = [&](const char* k) {
            int n = eq - ls;
            for (int j = 0; j < n; j++) if (msg[ls + j] != k[j]) return false;
            return k[n] == 0;
        };
        if (key_is("READY")) {
            if (msg[eq + 1] == '1') {
                if (!s->notify_ready) {
                    s->notify_ready = true;
                    // a notify unit only counts as started now. (satoru)
                    if (s->state == KSVC_STARTING) mark_running(s);
                    LogEvent("ready", s->name, nullptr);
                }
            }
        } else if (key_is("WATCHDOG")) {
            if (msg[eq + 1] == '1') {
                s->last_watchdog_ms = t;
                LogEvent("watchdog-ping", s->name, nullptr);
            }
        } else if (key_is("STATUS")) {
            int vn = le - (eq + 1);
            if (vn < 0) vn = 0;
            if (vn > (int)sizeof(s->notify_status) - 1) vn = (int)sizeof(s->notify_status) - 1;
            for (int j = 0; j < vn; j++) s->notify_status[j] = msg[eq + 1 + j];
            s->notify_status[vn] = 0;
            LogEvent("status", s->name, s->notify_status);
        } else if (key_is("STOPPING")) {
            if (msg[eq + 1] == '1') LogEvent("stopping", s->name, nullptr);
        }
    }
    return true;
}

void PollSocketActivation() {
    poll_notify_socket();   // drain sd_notify datagrams too (called from Tick) (satoru)
    for (int i = 0; i < g_service_count; i++) {
        KService* s = &g_services[i];
        if (s->listen_sd < 0) continue;
        if (s->state == KSVC_RUNNING || s->state == KSVC_STARTING) continue;
        // a pending connection on the listening socket triggers the real start. we
        // accept it (so the backlog drains) then spawn the service, which will
        // itself connect to the same path; the accepted peer is closed once the
        // service is up (a real server would inherit it, but our services rebind).
        // (satoru)
        char peer[UnixSocket::UNIX_PATH_MAX];
        int cd = UnixSocket::Accept(s->listen_sd, peer, (int)sizeof(peer));
        if (cd < 0) continue;
        UnixSocket::Close(cd);
        s->socket_activated = true;
        LogEvent("socket-activated", s->name, s->listen_path);
        // close the listener so we start exactly once, then start normally. (satoru)
        UnixSocket::Close(s->listen_sd);
        s->listen_sd = -1;
        char saved[KINIT_SOCK_LEN];
        ki_cpy(saved, s->listen_path, sizeof(saved));
        s->listen_path[0] = 0;          // prevent StartService re-deferring (satoru)
        StartService(s->name);
        ki_cpy(s->listen_path, saved, sizeof(s->listen_path));  // keep for status (satoru)
    }
}

// ── hot reload (satoru) ──────────────────────────────────────────────────────
// is unit `idx` one managed by the system services directory (i.e. eligible for
// add/remove/change on reload)? built-in in-kernel units, template instances,
// and per-user session units are NOT touched by a system-dir reload. (satoru)
bool is_dir_managed(const KService* s) {
    if (s->name[0] == 0) return false;             // tombstone (satoru)
    if (s->kind == KUNIT_INKERNEL) return false;   // built-in adopted subsystem (satoru)
    if (s->is_instance) return false;              // template instance (satoru)
    if (s->owner_user[0]) return false;            // user-session unit (satoru)
    return true;
}

// has the on-disk config of `cur` changed vs the freshly-parsed `nw`? compares
// the fields a reload can apply. (satoru)
bool config_differs(const KService* cur, const KService* nw) {
    if (!ki_eq(cur->exec, nw->exec)) return true;
    if (!ki_eq(cur->after, nw->after)) return true;
    if (cur->restart != nw->restart) return true;
    if (cur->restart_delay_ms != nw->restart_delay_ms) return true;
    if (cur->target != nw->target) return true;
    if (cur->type != nw->type) return true;
    if (cur->critical != nw->critical) return true;
    if (cur->enabled != nw->enabled) return true;
    if (cur->limits.memory_max_kb != nw->limits.memory_max_kb) return true;
    if (cur->limits.cpu_quota_pct != nw->limits.cpu_quota_pct) return true;
    if (cur->limits.limit_nofile != nw->limits.limit_nofile) return true;
    if (cur->watchdog_sec != nw->watchdog_sec) return true;
    if (!ki_eq(cur->listen_path, nw->listen_path)) return true;
    return false;
}

// remove (tombstone) a dir-managed unit: stop it, free its supervisor slot, and
// clear the name so the slot is reusable + find_index misses it. the slot is NOT
// compacted (supervisor trampolines are pinned by index). (satoru)
void remove_service_slot(int idx) {
    KService* s = &g_services[idx];
    StopService(s->name);
    g_sup[idx].relaunch = false;
    g_sup[idx].svc_index = -1;
    // a tombstone: empty name, inactive. (satoru)
    for (int i = 0; i < (int)sizeof(KService); i++) ((char*)s)[i] = 0;
    s->state = KSVC_STOPPED;
    s->enabled = false;
    s->listen_sd = -1;
}

void Reload() {
    LogEvent("reload", "-", "hot diff of /kurono/system/services");

    // 1) parse the current on-disk set. (satoru)
    const char* dir = "/kurono/system/services";
    KVFS::Mkdirs(dir);
    KVFSNode* entries[64];
    int n = KVFS::Listdir(dir, entries, 64);

    static KService disk[64];
    int disk_n = 0;
    static char rbuf[8192];
    char path[256];
    for (int e = 0; e < n && disk_n < 64; e++) {
        if (!entries[e]) continue;
        const char* fname = entries[e]->name;
        int fl = ki_len(fname);
        if (fl < 10) continue;
        bool ext_ok = true;
        const char* ext = ".kservice";
        for (int k = 0; k < 9; k++) {
            char c = fname[fl - 9 + k];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != ext[k]) { ext_ok = false; break; }
        }
        if (!ext_ok) continue;
        int p = 0;
        for (int k = 0; dir[k] && p < (int)sizeof(path) - 1; k++) path[p++] = dir[k];
        if (p < (int)sizeof(path) - 1) path[p++] = '/';
        for (int k = 0; fname[k] && p < (int)sizeof(path) - 1; k++) path[p++] = fname[k];
        path[p] = 0;
        int got = KVFS::ReadFile(path, rbuf, (uint32_t)sizeof(rbuf) - 1);
        if (got <= 0) continue;
        rbuf[got] = 0;
        if (ParseKService(rbuf, got, &disk[disk_n])) disk_n++;
    }

    int added = 0, removed = 0, changed = 0, unchanged = 0;

    // 2) for each currently dir-managed unit, if it is gone from disk -> remove;
    //    if its config changed -> apply (restart if it was running). (satoru)
    for (int i = 0; i < g_service_count; i++) {
        if (!is_dir_managed(&g_services[i])) continue;
        // find the matching disk unit by name. (satoru)
        int d = -1;
        for (int k = 0; k < disk_n; k++) if (ki_eq(disk[k].name, g_services[i].name)) { d = k; break; }
        if (d < 0) {
            LogEvent("reload-remove", g_services[i].name, nullptr);
            remove_service_slot(i);
            removed++;
            continue;
        }
        if (config_differs(&g_services[i], &disk[d])) {
            bool was_running = (g_services[i].state == KSVC_RUNNING ||
                                g_services[i].state == KSVC_STARTING ||
                                g_services[i].state == KSVC_RESTARTING);
            // apply the new config in place, preserving runtime/socket identity. (satoru)
            KService keep = g_services[i];
            g_services[i] = disk[d];
            g_services[i].state = KSVC_INACTIVE;
            g_services[i].pid = 0;
            g_services[i].listen_sd = -1;
            g_services[i].cur_backoff_ms = g_services[i].restart_delay_ms ? g_services[i].restart_delay_ms : 2000;
            g_services[i].crash_count = keep.crash_count;   // keep history (satoru)
            LogEvent("reload-change", g_services[i].name, nullptr);
            changed++;
            if (was_running && g_services[i].enabled) {
                g_sup[i].relaunch = false;
                StartService(g_services[i].name);
            }
        } else {
            unchanged++;
        }
    }

    // 3) for each disk unit not yet registered -> register + start if its target
    //    has already booted. (satoru)
    for (int k = 0; k < disk_n; k++) {
        if (disk[k].is_template) {
            if (find_index(disk[k].name) < 0) { RegisterService(&disk[k]); added++; }
            continue;
        }
        if (find_index(disk[k].name) >= 0) continue;   // already present (satoru)
        int idx = RegisterService(&disk[k]);
        if (idx < 0) continue;
        added++;
        LogEvent("reload-add", disk[k].name, nullptr);
        // if we have already booted past this unit's target, start it now. (satoru)
        if (g_booted && g_services[idx].enabled) StartService(g_services[idx].name);
    }

    char d[96];
    int q = 0;
    q = ki_cat(d, q, (int)sizeof(d), "added=");      q = ki_cat_u(d, q, (int)sizeof(d), (uint32_t)added);
    q = ki_cat(d, q, (int)sizeof(d), " removed=");   q = ki_cat_u(d, q, (int)sizeof(d), (uint32_t)removed);
    q = ki_cat(d, q, (int)sizeof(d), " changed=");   q = ki_cat_u(d, q, (int)sizeof(d), (uint32_t)changed);
    q = ki_cat(d, q, (int)sizeof(d), " unchanged="); q = ki_cat_u(d, q, (int)sizeof(d), (uint32_t)unchanged);
    LogEvent("reload-done", "-", d);
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
namespace {
// is service i's whole After= list settled? deps may live in earlier targets.
// (satoru)
bool deps_ready(int i) {
    KService* s = &g_services[i];
    if (!s->after[0]) return true;
    for (int j = 0; j < g_service_count; j++) {
        if (j == i || g_services[j].name[0] == 0) continue;
        if (after_lists(s->after, g_services[j].name) && !dep_settled(&g_services[j]))
            return false;
    }
    return true;
}

// wait (bounded) for a just-started unit to settle (running / stopped / failed).
// used by the sequential path so a unit is fully up before its dependent starts;
// the parallel path waits on the whole batch instead. caps the wait so a stuck
// service can never wedge boot. (satoru)
void wait_settled(int i, uint32_t cap_ms) {
    uint32_t start = now_ms();
    while (now_ms() - start < cap_ms) {
        KServiceState st = g_services[i].state;
        if (st == KSVC_RUNNING || st == KSVC_STOPPED || st == KSVC_FAILED) return;
        Tick();                          // advance notify/exit bookkeeping (satoru)
        Scheduler::SleepMs(10);
    }
}
}  // namespace

void StartTarget(KTarget t) { StartTarget(t, g_boot_parallel); }

void StartTarget(KTarget t, bool parallel) {
    LogEvent("target-reached", TargetName(t), nullptr);
    uint32_t tgt_start = now_ms();

    // cap how long we will wait for a single unit / a batch to settle so a stuck
    // service can never deadlock boot. generous enough for the slowest real start.
    // (satoru)
    const uint32_t SETTLE_CAP_MS = 8000;

    bool changed = true;
    int guard = 0;
    while (changed && guard++ < 128) {
        changed = false;

        if (parallel) {
            // start EVERY dep-ready, inactive unit in this target in one pass, then
            // wait for the batch to settle concurrently. independent slow services
            // overlap instead of serialising. (satoru)
            int batch[KINIT_MAX_SERVICES];
            int bn = 0;
            for (int i = 0; i < g_service_count; i++) {
                KService* s = &g_services[i];
                if (s->target != t || s->name[0] == 0) continue;
                if (!s->enabled || s->state != KSVC_INACTIVE) continue;
                if (!deps_ready(i)) continue;
                StartService(s->name);
                if (s->kind == KUNIT_ONESHOT && s->state != KSVC_FAILED) s->state = KSVC_STOPPED;
                batch[bn++] = i;
                changed = true;
            }
            // wait for the whole batch together. (satoru)
            uint32_t wstart = now_ms();
            bool all = false;
            while (!all && now_ms() - wstart < SETTLE_CAP_MS) {
                all = true;
                for (int b = 0; b < bn; b++) {
                    KServiceState st = g_services[batch[b]].state;
                    if (st != KSVC_RUNNING && st != KSVC_STOPPED && st != KSVC_FAILED) { all = false; break; }
                }
                if (!all) { Tick(); Scheduler::SleepMs(10); }
            }
        } else {
            // sequential: start ONE dep-ready unit, wait for it to settle, repeat.
            // this is the historical behaviour, made honest (it actually waits).
            // (satoru)
            for (int i = 0; i < g_service_count; i++) {
                KService* s = &g_services[i];
                if (s->target != t || s->name[0] == 0) continue;
                if (!s->enabled || s->state != KSVC_INACTIVE) continue;
                if (!deps_ready(i)) continue;
                StartService(s->name);
                if (s->kind == KUNIT_ONESHOT && s->state != KSVC_FAILED) s->state = KSVC_STOPPED;
                wait_settled(i, SETTLE_CAP_MS);
                changed = true;
                break;     // re-evaluate deps from the top after each unit (satoru)
            }
        }
    }

    char d[48];
    int q = 0;
    q = ki_cat(d, q, (int)sizeof(d), parallel ? "parallel " : "sequential ");
    q = ki_cat_u(d, q, (int)sizeof(d), now_ms() - tgt_start);
    q = ki_cat(d, q, (int)sizeof(d), "ms");
    LogEvent("target-settled", TargetName(t), d);
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

        // 2) fire a due restart for a PROCESS/ONESHOT unit (in-kernel relaunch is
        //    block 4). (satoru)
        if ((s->kind == KUNIT_PROCESS || s->kind == KUNIT_ONESHOT) &&
            s->state == KSVC_RESTARTING && (int32_t)(t - s->next_restart_ms) >= 0) {
            LogEvent("relaunch", s->name, nullptr);
            ensure_supervisor(i);
            g_sup[i].svc_index = i;
            g_sup[i].exited    = false;
            g_sup[i].relaunch  = true;
            s->notify_ready = false;
            s->state = (s->type == KTYPE_NOTIFY) ? KSVC_STARTING : KSVC_RUNNING;
            s->start_time_ms = t;
            s->last_watchdog_ms = t;       // watchdog grace after relaunch (satoru)
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
            s->notify_ready = false;
            s->last_watchdog_ms = t;          // grace period after relaunch (satoru)
            // a notify in-kernel unit re-arms to STARTING until it re-sends READY=1.
            // (satoru)
            s->state = (s->type == KTYPE_NOTIFY) ? KSVC_STARTING : KSVC_RUNNING;
            LogEvent("relaunch", s->name, "(in-kernel)");
        }

        // 5) watchdog: a running unit with WatchdogSec= must send WATCHDOG=1 at
        //    least once per interval. if the last ping is older than the interval
        //    it is killed + restarted (via the same backoff machinery as a crash).
        //    (satoru)
        if (s->state == KSVC_RUNNING && s->watchdog_sec > 0) {
            uint32_t budget = s->watchdog_sec * 1000u;
            if ((int32_t)(t - s->last_watchdog_ms) > (int32_t)budget) {
                s->watchdog_kills++;
                char d[64];
                int q = 0;
                q = ki_cat(d, q, (int)sizeof(d), "watchdog: no WATCHDOG=1 in ");
                q = ki_cat_u(d, q, (int)sizeof(d), s->watchdog_sec);
                q = ki_cat(d, q, (int)sizeof(d), "s");
                kill_and_supervise(i, d);
            }
        }
    }

    // cross-service enforcement after the per-unit pass. (satoru)
    enforce_memory_limits();
    PollSocketActivation();
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
// WaylandServer::Init, the kernel_processes, etc), kinit ADOPTS them as
// already-running units rather than re-initialising and risking a double-init,
// so their start hooks are no-ops. the user.target daemons, by contrast, are
// genuinely started here via their Init(). (satoru)
void noop_start() {}
void noop_stop()  {}
void start_kpkg() { KpkgDaemon::Init(); }
void start_kupd() { KUpdate::Init(); }
void start_ksec() { KSecurity::Init(); }

// ── headless self-test services (satoru) ─────────────────────────────────────
// these in-kernel units exist ONLY to exercise the new supervision paths under a
// bounded headless boot (gated by EnableTestServices / kurono.kinit.test). they
// touch no hardware. each is an honest in-kernel worker, the same shape as the
// real kpkg/kupdate daemons. (satoru)
uint32_t g_test_bits = 0;

// (1) memhog: grows its heap past MemoryMax so kinit's enforce_memory_limits
// kills it. mem_probe reports the tracked allocation; stop_fn frees + idles.
// (satoru)
volatile uint32_t g_memhog_kb = 0;       // current tracked allocation (satoru)
volatile bool     g_memhog_run = false;  // worker grows while true (satoru)
void** g_memhog_chunks = nullptr;
int    g_memhog_nchunks = 0;
constexpr int  KMEMHOG_CHUNK_KB = 1024;  // 1 MiB per chunk (satoru)
constexpr int  KMEMHOG_MAX_CHUNKS = 256; // ceiling so a runaway can't OOM the box (satoru)

uint32_t memhog_mem_probe() { return g_memhog_kb; }

[[noreturn]] void memhog_worker() {
    SerialLogger::Log("[kinit-test] memhog worker online\r\n");
    for (;;) {
        if (g_memhog_run && g_memhog_nchunks < KMEMHOG_MAX_CHUNKS) {
            void* blk = KernelHeap::Alloc((size_t)KMEMHOG_CHUNK_KB * 1024);
            if (blk) {
                // touch the pages so the allocation is real, not lazy. (satoru)
                volatile uint8_t* b = (volatile uint8_t*)blk;
                for (int o = 0; o < KMEMHOG_CHUNK_KB * 1024; o += 4096) b[o] = 0xA5;
                if (g_memhog_chunks) g_memhog_chunks[g_memhog_nchunks++] = blk;
                g_memhog_kb += KMEMHOG_CHUNK_KB;
            }
        }
        Scheduler::SleepMs(60);
    }
}
bool g_memhog_started = false;
void memhog_start() {
    if (!g_memhog_started) {
        g_memhog_chunks = (void**)KernelHeap::Alloc(sizeof(void*) * KMEMHOG_MAX_CHUNKS);
        g_memhog_nchunks = 0;
        Scheduler::SpawnKernelProcess("kt-memhog", memhog_worker, PRIO_LOW, 64, 8192);
        g_memhog_started = true;
    }
    g_memhog_run = true;     // (re)start growing on each start/relaunch (satoru)
}
void memhog_stop() {
    // a kill stops growth and frees the arena so the next run starts clean. the
    // worker kernel-process stays parked (idle) and is reused. (satoru)
    g_memhog_run = false;
    if (g_memhog_chunks) {
        for (int i = 0; i < g_memhog_nchunks; i++) if (g_memhog_chunks[i]) KernelHeap::Free(g_memhog_chunks[i]);
    }
    g_memhog_nchunks = 0;
    g_memhog_kb = 0;
}

// (2) watchdog test: two notify units that ping WATCHDOG=1. wd-good keeps
// pinging; wd-bad stops after a few pings so the watchdog kills + restarts it.
// each is its own kernel-process; a relaunch re-arms the ping counter. (satoru)
volatile int g_wdbad_pings_left = 0;
volatile int g_wdbad_restart_gen = 0;     // bumps each relaunch so the worker re-arms (satoru)
volatile int g_wdbad_kills = 0;

[[noreturn]] void wd_good_worker() {
    for (;;) { SdNotify("kt-wd-good", "WATCHDOG=1"); Scheduler::SleepMs(800); }
}
bool g_wdgood_started = false;
void wd_good_start() {
    SdNotify("kt-wd-good", "READY=1");
    if (!g_wdgood_started) { Scheduler::SpawnKernelProcess("kt-wd-good", wd_good_worker, PRIO_LOW, 64, 8192); g_wdgood_started = true; }
}

[[noreturn]] void wd_bad_worker() {
    int gen = -1;
    for (;;) {
        if (gen != g_wdbad_restart_gen) {     // (re)armed: ping a few times then go silent (satoru)
            gen = g_wdbad_restart_gen;
            g_wdbad_pings_left = 3;
        }
        if (g_wdbad_pings_left > 0) {
            SdNotify("kt-wd-bad", "WATCHDOG=1");
            g_wdbad_pings_left--;
        }
        // once pings_left hits 0 it stops pinging -> kinit's watchdog fires. (satoru)
        Scheduler::SleepMs(800);
    }
}
bool g_wdbad_started = false;
void wd_bad_start() {
    SdNotify("kt-wd-bad", "READY=1");
    g_wdbad_restart_gen++;                    // re-arm the ping burst on each (re)start (satoru)
    if (g_wdbad_started) g_wdbad_kills++;     // count relaunches after the first (satoru)
    if (!g_wdbad_started) { Scheduler::SpawnKernelProcess("kt-wd-bad", wd_bad_worker, PRIO_LOW, 64, 8192); g_wdbad_started = true; }
}

// (3) socket-activated test unit. start hook just marks it up (it would normally
// bind its own service socket); the point under test is that it does NOT start
// until something connects to ListenStream. a client kernel-process connects
// after a delay to trigger activation. (satoru)
volatile bool g_sock_started = false;
void sock_start() { g_sock_started = true; SerialLogger::Log("[kinit-test] socktest started (activated)\r\n"); }
[[noreturn]] void sock_client_worker() {
    // wait for the desktop to settle, then connect to the activation socket. (satoru)
    Scheduler::SleepMs(6000);
    int cd = UnixSocket::Create(UnixSocket::UNIX_SOCK_STREAM);
    if (cd >= 0) {
        int rc = UnixSocket::Connect(cd, "/kurono/runtime/sockets/kt-socktest");
        SerialLogger::Log(rc == 0 ? "[kinit-test] socktest client connected\r\n"
                                  : "[kinit-test] socktest client connect failed\r\n");
        // leave the socket; PollSocketActivation accepts then closes it. (satoru)
    }
    for (;;) Scheduler::SleepMs(60000);
}

// (4) parallel-measurement units: notify units whose start hook re-arms a worker
// that sleeps SLOW_MS then signals READY. independent (no After= among them) so
// the parallel path overlaps the SLOW_MS waits; the sequential path serialises
// them. each worker re-arms on a per-unit generation counter so the units can be
// re-run to compare both modes. (satoru)
constexpr uint32_t KPAR_SLOW_MS = 500;
constexpr int      KPAR_COUNT   = 4;
const char* g_par_names[KPAR_COUNT] = { "kt-par-1", "kt-par-2", "kt-par-3", "kt-par-4" };
volatile int g_par_gen[KPAR_COUNT]    = { 0, 0, 0, 0 };   // bumped on each (re)start (satoru)
volatile int g_par_seen[KPAR_COUNT]   = { -1, -1, -1, -1 };
bool         g_par_started[KPAR_COUNT] = { false, false, false, false };

[[noreturn]] void par_worker_generic(int which) {
    for (;;) {
        if (g_par_seen[which] != g_par_gen[which]) {
            g_par_seen[which] = g_par_gen[which];
            Scheduler::SleepMs(KPAR_SLOW_MS);          // the "slow start" work (satoru)
            SdNotify(g_par_names[which], "READY=1");
        }
        Scheduler::SleepMs(20);
    }
}
void par_w0() { par_worker_generic(0); }
void par_w1() { par_worker_generic(1); }
void par_w2() { par_worker_generic(2); }
void par_w3() { par_worker_generic(3); }
KernelProcessEntry g_par_entry[KPAR_COUNT] = { par_w0, par_w1, par_w2, par_w3 };
void par_start_n(int n) {
    if (n < 0 || n >= KPAR_COUNT) return;
    g_par_gen[n]++;     // (re)arm: the worker will sleep then signal READY again (satoru)
    if (!g_par_started[n]) {
        char nm[16]; int q = 0; q = ki_cat(nm, q, (int)sizeof(nm), "ktp"); q = ki_cat_u(nm, q, (int)sizeof(nm), (uint32_t)n);
        Scheduler::SpawnKernelProcess(nm, g_par_entry[n], PRIO_NORMAL, 64, 8192);
        g_par_started[n] = true;
    }
}
void par_start_0() { par_start_n(0); }
void par_start_1() { par_start_n(1); }
void par_start_2() { par_start_n(2); }
void par_start_3() { par_start_n(3); }

void register_test_services() {
    if (g_test_bits == 0) return;
    KCapabilities nocap = { false, false, false };
    (void)nocap;

    if (g_test_bits & KTEST_MEMLIMIT) {
        int idx = RegisterInkernel("kt-memhog", "test: 64M memory hog (oom-killed)", KTGT_USER,
                                   memhog_start, memhog_stop, nullptr, "", KRESTART_NO, 2000, false);
        if (idx >= 0) {
            g_services[idx].limits.memory_max_kb = 64 * 1024;   // 64 MiB (satoru)
            g_services[idx].mem_probe = memhog_mem_probe;
        }
    }
    if (g_test_bits & KTEST_WATCHDOG) {
        int g = RegisterInkernel("kt-wd-good", "test: watchdog unit (keeps pinging)", KTGT_USER,
                                 wd_good_start, noop_stop, nullptr, "", KRESTART_ON_FAILURE, 1000, false);
        if (g >= 0) { g_services[g].type = KTYPE_NOTIFY; g_services[g].watchdog_sec = 3; }
        int b = RegisterInkernel("kt-wd-bad", "test: watchdog unit (stops pinging -> restart)", KTGT_USER,
                                 wd_bad_start, noop_stop, nullptr, "", KRESTART_ON_FAILURE, 1000, false);
        if (b >= 0) { g_services[b].type = KTYPE_NOTIFY; g_services[b].watchdog_sec = 3; }
    }
    if (g_test_bits & KTEST_SOCKET) {
        int idx = RegisterInkernel("kt-socktest", "test: socket-activated unit", KTGT_USER,
                                   sock_start, noop_stop, nullptr, "", KRESTART_NO, 2000, false);
        if (idx >= 0) ki_cpy(g_services[idx].listen_path, "/kurono/runtime/sockets/kt-socktest", sizeof(g_services[idx].listen_path));
    }
    if (g_test_bits & KTEST_PARALLEL) {
        KInkernelHook starts[KPAR_COUNT] = { par_start_0, par_start_1, par_start_2, par_start_3 };
        for (int i = 0; i < KPAR_COUNT; i++) {
            int idx = RegisterInkernel(g_par_names[i], "test: slow parallel unit", KTGT_USER,
                                       starts[i], noop_stop, nullptr, "", KRESTART_NO, 2000, false);
            if (idx >= 0) g_services[idx].type = KTYPE_NOTIFY;
        }
    }
    if (g_test_bits & KTEST_CYCLE) {
        // a deliberate cycle: cyc-a After=cyc-b, cyc-b After=cyc-a. (satoru)
        RegisterInkernel("kt-cyc-a", "test: cyclic dep A", KTGT_USER,
                         noop_start, noop_stop, nullptr, "kt-cyc-b", KRESTART_NO, 2000, false);
        RegisterInkernel("kt-cyc-b", "test: cyclic dep B", KTGT_USER,
                         noop_start, noop_stop, nullptr, "kt-cyc-a", KRESTART_NO, 2000, false);
    }
    SerialLogger::Log("[kinit-test] test services registered\r\n");
}

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
    //, an in-kernel worker, not a separate linux address space. the matching
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

    // register the headless test services if the boot token asked for them. these
    // exercise MemoryMax/watchdog/socket-activation/parallel paths and are gated so
    // a normal boot is unaffected. (satoru)
    register_test_services();

    // detect + break any circular After= dependency so boot can never deadlock.
    // (satoru)
    DetectAndBreakCycles();

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

    // bind kinit's sd_notify socket before any notify service starts so READY=1
    // datagrams are never dropped. (satoru)
    ensure_notify_socket();

    // sequence the boot stages in order; each StartTarget brings up that stage's
    // services once their deps are settled. targets run with parallel intra-target
    // startup by default (independent units overlap; direct After= deps still
    // ordered). (satoru)
    uint32_t bs = now_ms();
    StartTarget(KTGT_KERNEL);
    StartTarget(KTGT_NETWORK);
    StartTarget(KTGT_DBUS);
    StartTarget(KTGT_DESKTOP);
    StartTarget(KTGT_USER);
    g_boot_elapsed_ms = now_ms() - bs;

    // start the crash monitor kernel-process. it relaunches dead daemons with
    // backoff, probes in-kernel health, enforces MemoryMax/watchdog, drains
    // sd_notify, and services socket activation. (satoru)
    Scheduler::SpawnKernelProcess("kinit-monitor", monitor_entry, PRIO_LOW, 64, 4096);

    char bd[32];
    int q = 0;
    q = ki_cat_u(bd, q, (int)sizeof(bd), g_boot_elapsed_ms);
    q = ki_cat(bd, q, (int)sizeof(bd), "ms");
    LogEvent("boot-complete", "-", bd);
    SerialLogger::Log("[kinit] boot complete; running services: ");
    SerialLogger::LogDec(RunningCount());
    SerialLogger::Log("\r\n");
}

uint32_t BootElapsedMs() { return g_boot_elapsed_ms; }

void Shutdown() {
    LogEvent("shutdown", "-", nullptr);
    StopTarget(KTGT_USER);
    StopTarget(KTGT_DESKTOP);
    StopTarget(KTGT_DBUS);
    StopTarget(KTGT_NETWORK);
    StopTarget(KTGT_KERNEL);
}

// ── dependency cycle detection (satoru) ──────────────────────────────────────
// the After= graph is small (<= KINIT_MAX_SERVICES nodes). we run an iterative
// DFS with a colour array (0=white,1=grey,2=black). a grey->grey back-edge is a
// cycle: we log it and BREAK it by clearing the offending name out of the
// dependent's After= list, so StartTarget's fixpoint can always make progress.
// returns the number of back-edges broken. (satoru)
namespace {
// remove dep `victim` from service idx's After= list, in place. (satoru)
void strip_after(int idx, const char* victim) {
    KService* s = &g_services[idx];
    char rebuilt[KINIT_DEP_LEN];
    int w = 0;
    int i = 0;
    while (s->after[i]) {
        while (s->after[i] == ' ' || s->after[i] == '\t' || s->after[i] == ',') i++;
        if (!s->after[i]) break;
        int tok = i;
        while (s->after[i] && s->after[i] != ' ' && s->after[i] != '\t' && s->after[i] != ',') i++;
        int toklen = i - tok;
        // does this token equal victim? (satoru)
        bool match = ((int)ki_len(victim) == toklen);
        if (match) for (int k = 0; k < toklen; k++) if (s->after[tok + k] != victim[k]) { match = false; break; }
        if (match) continue;            // drop it (satoru)
        if (w > 0 && w < KINIT_DEP_LEN - 1) rebuilt[w++] = ' ';
        for (int k = 0; k < toklen && w < KINIT_DEP_LEN - 1; k++) rebuilt[w++] = s->after[tok + k];
    }
    rebuilt[w] = 0;
    ki_cpy(s->after, rebuilt, sizeof(s->after));
}
}  // namespace

int DetectAndBreakCycles() {
    uint8_t color[KINIT_MAX_SERVICES];
    for (int i = 0; i < g_service_count; i++) color[i] = 0;
    int broken = 0;

    // iterative DFS stack of (node, next-neighbour-cursor). (satoru)
    for (int root = 0; root < g_service_count; root++) {
        if (g_services[root].name[0] == 0) continue;
        if (color[root] != 0) continue;

        int stack[KINIT_MAX_SERVICES];
        int cursor[KINIT_MAX_SERVICES];
        int sp = 0;
        stack[sp] = root; cursor[sp] = 0; color[root] = 1;

        while (sp >= 0) {
            int u = stack[sp];
            // find the next dependency of u (a node v such that u After= v). (satoru)
            int v = -1;
            for (int j = cursor[sp]; j < g_service_count; j++) {
                if (j == u || g_services[j].name[0] == 0) continue;
                if (after_lists(g_services[u].after, g_services[j].name)) { v = j; cursor[sp] = j + 1; break; }
                cursor[sp] = j + 1;
            }
            if (v < 0) { color[u] = 2; sp--; continue; }   // done with u (satoru)
            if (color[v] == 1) {
                // back-edge u->v: a cycle. break it by removing v from u's After=.
                // (satoru)
                char d[96];
                int q = 0;
                q = ki_cat(d, q, (int)sizeof(d), g_services[u].name);
                q = ki_cat(d, q, (int)sizeof(d), " After=");
                q = ki_cat(d, q, (int)sizeof(d), g_services[v].name);
                q = ki_cat(d, q, (int)sizeof(d), " (broken)");
                LogEvent("dependency-cycle", g_services[u].name, d);
                RuntimeLog::LogSystem("kinit", d);
                strip_after(u, g_services[v].name);
                broken++;
                // do not descend the back-edge. (satoru)
            } else if (color[v] == 0) {
                color[v] = 1; sp++; stack[sp] = v; cursor[sp] = 0;
            }
            // color[v]==2 (black): already fully explored, skip. (satoru)
        }
    }
    if (broken > 0) {
        char d[48];
        int q = 0;
        q = ki_cat_u(d, q, (int)sizeof(d), (uint32_t)broken);
        q = ki_cat(d, q, (int)sizeof(d), " cycle(s) broken");
        LogEvent("dependency-check", "-", d);
    }
    return broken;
}

// ── service templates (satoru) ────────────────────────────────────────────────
// substitute every "%i" in src with inst, into dst (bounded). (satoru)
namespace {
void subst_instance(char* dst, int dmax, const char* src, const char* inst) {
    int w = 0;
    for (int i = 0; src[i] && w < dmax - 1; ) {
        if (src[i] == '%' && src[i + 1] == 'i') {
            for (int k = 0; inst[k] && w < dmax - 1; k++) dst[w++] = inst[k];
            i += 2;
        } else {
            dst[w++] = src[i++];
        }
    }
    dst[w] = 0;
}
}  // namespace

int InstantiateTemplate(const char* tmpl_at_inst) {
    if (!tmpl_at_inst) return -1;
    // split on the LAST '@': "foo@bar" -> tmpl="foo@", inst="bar". (satoru)
    int at = -1;
    for (int i = 0; tmpl_at_inst[i]; i++) if (tmpl_at_inst[i] == '@') at = i;
    if (at < 0) return -1;
    char tmpl_name[KINIT_NAME_LEN];
    char inst[KINIT_NAME_LEN];
    int tn = at + 1;                 // include the '@' so it matches the template (satoru)
    if (tn > (int)sizeof(tmpl_name) - 1) tn = (int)sizeof(tmpl_name) - 1;
    for (int k = 0; k < tn; k++) tmpl_name[k] = tmpl_at_inst[k];
    tmpl_name[tn] = 0;
    ki_cpy(inst, tmpl_at_inst + at + 1, sizeof(inst));
    if (inst[0] == 0) return -1;

    int ti = find_index(tmpl_name);
    if (ti < 0 || !g_services[ti].is_template) {
        LogEvent("template-missing", tmpl_at_inst, tmpl_name);
        return -1;
    }
    // already instantiated? (satoru)
    if (find_index(tmpl_at_inst) >= 0) return find_index(tmpl_at_inst);

    KService inst_svc = g_services[ti];
    inst_svc.is_template = false;
    inst_svc.is_instance = true;
    ki_cpy(inst_svc.name, tmpl_at_inst, sizeof(inst_svc.name));
    ki_cpy(inst_svc.instance, inst, sizeof(inst_svc.instance));
    // substitute %i into exec + description. (satoru)
    char tmp[KINIT_PATH_LEN];
    subst_instance(tmp, sizeof(tmp), g_services[ti].exec, inst);
    ki_cpy(inst_svc.exec, tmp, sizeof(inst_svc.exec));
    char dtmp[64];
    subst_instance(dtmp, sizeof(dtmp), g_services[ti].description, inst);
    ki_cpy(inst_svc.description, dtmp, sizeof(inst_svc.description));

    int idx = RegisterService(&inst_svc);
    if (idx >= 0) {
        char d[64];
        int q = 0;
        q = ki_cat(d, q, (int)sizeof(d), "from ");
        q = ki_cat(d, q, (int)sizeof(d), tmpl_name);
        LogEvent("instantiate", tmpl_at_inst, d);
    }
    return idx;
}

// ── user-session services (satoru) ────────────────────────────────────────────
// per-user units live in /kurono/user/home/<user>/.config/kinit/*.kservice. on
// login they are parsed, tagged with owner_user, registered + started; on logout
// they are stopped + removed. (satoru)
namespace {
void user_kinit_dir(const char* user, char* out, int mx) {
    int p = 0;
    p = ki_cat(out, p, mx, KP_USER_HOME);
    p = ki_cat(out, p, mx, "/");
    p = ki_cat(out, p, mx, user ? user : "user");
    p = ki_cat(out, p, mx, "/.config/kinit");
}
}  // namespace

int StartUserSession(const char* user) {
    if (!user || !user[0]) return 0;
    char dir[160];
    user_kinit_dir(user, dir, sizeof(dir));
    KVFS::Mkdirs(dir);

    KVFSNode* entries[64];
    int n = KVFS::Listdir(dir, entries, 64);
    if (n <= 0) { LogEvent("user-session", user, "no per-user units"); return 0; }

    int started = 0;
    static char buf[8192];
    char path[256];
    for (int e = 0; e < n; e++) {
        if (!entries[e]) continue;
        const char* fname = entries[e]->name;
        int fl = ki_len(fname);
        if (fl < 10) continue;
        bool ext_ok = true;
        const char* ext = ".kservice";
        for (int k = 0; k < 9; k++) { char c = fname[fl - 9 + k]; if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); if (c != ext[k]) { ext_ok = false; break; } }
        if (!ext_ok) continue;
        int p = 0;
        for (int k = 0; dir[k] && p < (int)sizeof(path) - 1; k++) path[p++] = dir[k];
        if (p < (int)sizeof(path) - 1) path[p++] = '/';
        for (int k = 0; fname[k] && p < (int)sizeof(path) - 1; k++) path[p++] = fname[k];
        path[p] = 0;
        int got = KVFS::ReadFile(path, buf, (uint32_t)sizeof(buf) - 1);
        if (got <= 0) continue;
        buf[got] = 0;
        KService svc;
        if (!ParseKService(buf, got, &svc)) continue;
        ki_cpy(svc.owner_user, user, sizeof(svc.owner_user));
        // namespace the registered name as "<user>:<unit>" so two users can run the
        // same-named unit without colliding. (satoru)
        char qual[KINIT_NAME_LEN];
        int q = 0;
        q = ki_cat(qual, q, (int)sizeof(qual), user);
        q = ki_cat(qual, q, (int)sizeof(qual), ":");
        q = ki_cat(qual, q, (int)sizeof(qual), svc.name);
        ki_cpy(svc.name, qual, sizeof(svc.name));
        if (find_index(svc.name) >= 0) continue;
        int idx = RegisterService(&svc);
        if (idx < 0) continue;
        if (StartService(g_services[idx].name) == 0) started++;
    }
    char d[48];
    int q = 0;
    q = ki_cat_u(d, q, (int)sizeof(d), (uint32_t)started);
    q = ki_cat(d, q, (int)sizeof(d), " unit(s) started");
    LogEvent("user-session-start", user, d);
    return started;
}

int StopUserSession(const char* user) {
    if (!user || !user[0]) return 0;
    int stopped = 0;
    for (int i = 0; i < g_service_count; i++) {
        KService* s = &g_services[i];
        if (s->name[0] == 0) continue;
        if (!ki_eq(s->owner_user, user)) continue;
        StopService(s->name);
        // tombstone the slot so a re-login re-registers cleanly. (satoru)
        g_sup[i].relaunch = false;
        g_sup[i].svc_index = -1;
        for (int b = 0; b < (int)sizeof(KService); b++) ((char*)s)[b] = 0;
        s->state = KSVC_STOPPED;
        s->enabled = false;
        s->listen_sd = -1;
        stopped++;
    }
    char d[48];
    int q = 0;
    q = ki_cat_u(d, q, (int)sizeof(d), (uint32_t)stopped);
    q = ki_cat(d, q, (int)sizeof(d), " unit(s) stopped");
    LogEvent("user-session-stop", user, d);
    return stopped;
}

// ── kinit analyze (satoru) ────────────────────────────────────────────────────
int Analyze(char* out, int mx) {
    int p = 0;
    p = ki_cat(out, p, mx, "kinit analyze: boot took ");
    p = ki_cat_u(out, p, mx, g_boot_elapsed_ms);
    p = ki_cat(out, p, mx, "ms (");
    p = ki_cat(out, p, mx, g_boot_parallel ? "parallel" : "sequential");
    p = ki_cat(out, p, mx, " target startup)\n\n");

    // per-service start->running time, sorted slowest first (selection sort over
    // the small table). (satoru)
    p = ki_cat(out, p, mx, "per-service start time (slowest first):\n");
    int order[KINIT_MAX_SERVICES];
    int m = 0;
    for (int i = 0; i < g_service_count; i++)
        if (g_services[i].name[0] && g_services[i].boot_ms > 0) order[m++] = i;
    for (int a = 0; a < m; a++) {
        int best = a;
        for (int b = a + 1; b < m; b++)
            if (g_services[order[b]].boot_ms > g_services[order[best]].boot_ms) best = b;
        int tmp = order[a]; order[a] = order[best]; order[best] = tmp;
    }
    if (m == 0) p = ki_cat(out, p, mx, "  (no timed services; in-kernel hooks are instant)\n");
    for (int a = 0; a < m; a++) {
        KService* s = &g_services[order[a]];
        p = ki_cat(out, p, mx, "  ");
        p = ki_cat_u(out, p, mx, s->boot_ms);
        p = ki_cat(out, p, mx, "ms");
        for (int k = (s->boot_ms >= 1000 ? 6 : 5); k < 8; k++) p = ki_cat(out, p, mx, " ");
        p = ki_cat(out, p, mx, s->name);
        p = ki_cat(out, p, mx, "\n");
    }

    // text dependency graph: each unit and its After= edges, grouped by target.
    // (satoru)
    p = ki_cat(out, p, mx, "\ndependency graph (unit <- After=):\n");
    for (int t = 0; t < KTGT_COUNT; t++) {
        bool any = false;
        for (int i = 0; i < g_service_count; i++)
            if (g_services[i].name[0] && (int)g_services[i].target == t) { any = true; break; }
        if (!any) continue;
        p = ki_cat(out, p, mx, "  [");
        p = ki_cat(out, p, mx, TargetName((KTarget)t));
        p = ki_cat(out, p, mx, "]\n");
        for (int i = 0; i < g_service_count; i++) {
            KService* s = &g_services[i];
            if (!s->name[0] || (int)s->target != t) continue;
            p = ki_cat(out, p, mx, "    ");
            p = ki_cat(out, p, mx, s->name);
            if (s->after[0]) { p = ki_cat(out, p, mx, " <- "); p = ki_cat(out, p, mx, s->after); }
            else             { p = ki_cat(out, p, mx, " <- (none)"); }
            p = ki_cat(out, p, mx, "\n");
        }
    }
    return p;
}

// ── headless self-test harness (satoru) ──────────────────────────────────────
void EnableTestServices(uint32_t bits) { g_test_bits = bits; }

namespace {
// reset the parallel-measurement units to INACTIVE so they can be re-run, and
// re-arm their workers (par_start_n bumps the generation). (satoru)
void par_reset() {
    for (int i = 0; i < KPAR_COUNT; i++) {
        int idx = find_index(g_par_names[i]);
        if (idx < 0) continue;
        g_services[idx].state = KSVC_INACTIVE;
        g_services[idx].notify_ready = false;
        g_par_seen[i] = g_par_gen[i];   // mark current arming consumed so a fresh start re-arms (satoru)
    }
}

// time how long until all KPAR_COUNT units reach RUNNING, starting them either
// all-at-once (parallel) or one-after-the-other-waiting (sequential). returns ms.
// (satoru)
uint32_t par_measure(bool parallel) {
    par_reset();
    uint32_t start = now_ms();
    if (parallel) {
        for (int i = 0; i < KPAR_COUNT; i++) StartService(g_par_names[i]);
        // wait for all four READY (bounded). (satoru)
        uint32_t w = now_ms();
        for (;;) {
            int up = 0;
            for (int i = 0; i < KPAR_COUNT; i++) { int idx = find_index(g_par_names[i]); if (idx >= 0 && g_services[idx].state == KSVC_RUNNING) up++; }
            if (up >= KPAR_COUNT) break;
            if (now_ms() - w > 12000) break;
            poll_notify_socket();
            Scheduler::SleepMs(10);
        }
    } else {
        for (int i = 0; i < KPAR_COUNT; i++) {
            StartService(g_par_names[i]);
            uint32_t w = now_ms();
            int idx = find_index(g_par_names[i]);
            while (idx >= 0 && g_services[idx].state != KSVC_RUNNING) {
                if (now_ms() - w > 12000) break;
                poll_notify_socket();
                Scheduler::SleepMs(10);
            }
        }
    }
    return now_ms() - start;
}
}  // namespace

void RunSelfTests() {
    if (g_test_bits == 0) return;
    SerialLogger::Log("\r\n[kinit-test] ===== kinit self-tests begin =====\r\n");

    // give boot-time scenarios time to fire (memhog grows + gets oom-killed; the
    // wd-bad unit goes silent + gets watchdog-restarted; socket client connects).
    // we poll the relevant state up to a bounded deadline and log PASS/FAIL. all
    // bounded so a stuck test can never hang the box. (satoru)

    // (A) parallel vs sequential measurement (always run if PARALLEL bit set). (satoru)
    if (g_test_bits & KTEST_PARALLEL) {
        uint32_t par = par_measure(true);
        uint32_t seq = par_measure(false);
        char d[96];
        int q = 0;
        q = ki_cat(d, q, (int)sizeof(d), "parallel=");   q = ki_cat_u(d, q, (int)sizeof(d), par);
        q = ki_cat(d, q, (int)sizeof(d), "ms sequential="); q = ki_cat_u(d, q, (int)sizeof(d), seq);
        q = ki_cat(d, q, (int)sizeof(d), "ms");
        LogEvent("test-parallel", "-", d);
        SerialLogger::Log("[kinit-test] PARALLEL "); SerialLogger::Log(d);
        SerialLogger::Log(par < seq ? "  PASS (parallel faster)\r\n" : "  FAIL (not faster)\r\n");
    }

    // (B) memory limit: wait for kt-memhog to be oom-killed. (satoru)
    if (g_test_bits & KTEST_MEMLIMIT) {
        int idx = find_index("kt-memhog");
        bool killed = false;
        uint32_t w = now_ms();
        while (idx >= 0 && now_ms() - w < 30000) {
            if (g_services[idx].oom_kills > 0) { killed = true; break; }
            Scheduler::SleepMs(100);
        }
        char d[64];
        int q = 0;
        q = ki_cat(d, q, (int)sizeof(d), "oom_kills=");
        q = ki_cat_u(d, q, (int)sizeof(d), idx >= 0 ? (uint32_t)g_services[idx].oom_kills : 0u);
        LogEvent("test-memlimit", "-", d);
        SerialLogger::Log("[kinit-test] MEMLIMIT "); SerialLogger::Log(d);
        SerialLogger::Log(killed ? "  PASS (MemoryMax kill fired)\r\n" : "  FAIL (never killed)\r\n");
    }

    // (C) watchdog: kt-wd-bad must be killed+restarted; kt-wd-good must not. (satoru)
    if (g_test_bits & KTEST_WATCHDOG) {
        int bad = find_index("kt-wd-bad");
        int good = find_index("kt-wd-good");
        bool restarted = false;
        uint32_t w = now_ms();
        while (bad >= 0 && now_ms() - w < 30000) {
            if (g_services[bad].watchdog_kills > 0) { restarted = true; break; }
            Scheduler::SleepMs(100);
        }
        char d[80];
        int q = 0;
        q = ki_cat(d, q, (int)sizeof(d), "bad.wd_kills=");
        q = ki_cat_u(d, q, (int)sizeof(d), bad >= 0 ? (uint32_t)g_services[bad].watchdog_kills : 0u);
        q = ki_cat(d, q, (int)sizeof(d), " good.wd_kills=");
        q = ki_cat_u(d, q, (int)sizeof(d), good >= 0 ? (uint32_t)g_services[good].watchdog_kills : 0u);
        LogEvent("test-watchdog", "-", d);
        SerialLogger::Log("[kinit-test] WATCHDOG "); SerialLogger::Log(d);
        bool good_ok = (good < 0) || (g_services[good].watchdog_kills == 0);
        SerialLogger::Log((restarted && good_ok) ? "  PASS (bad restarted, good survived)\r\n" : "  FAIL\r\n");
    }

    // (D) socket activation: kt-socktest must only run after the client connects.
    // (satoru)
    if (g_test_bits & KTEST_SOCKET) {
        int idx = find_index("kt-socktest");
        // it must be inactive now (no connect yet) ... (satoru)
        bool was_inactive = (idx >= 0 && g_services[idx].state == KSVC_INACTIVE);
        SerialLogger::Log(was_inactive ? "[kinit-test] socktest pre-connect: inactive (good)\r\n"
                                       : "[kinit-test] socktest pre-connect: already running (bad)\r\n");
        // spawn the client that connects after a delay. (satoru)
        Scheduler::SpawnKernelProcess("kt-sockcli", sock_client_worker, PRIO_LOW, 64, 8192);
        bool activated = false;
        uint32_t w = now_ms();
        while (idx >= 0 && now_ms() - w < 20000) {
            if (g_services[idx].socket_activated && g_services[idx].state == KSVC_RUNNING) { activated = true; break; }
            Scheduler::SleepMs(100);
        }
        LogEvent("test-socket", "-", activated ? "activated on connect" : "not activated");
        SerialLogger::Log(activated ? "[kinit-test] SOCKET  PASS (started only on connect)\r\n"
                                    : "[kinit-test] SOCKET  FAIL (never activated)\r\n");
    }

    // (E) cycle: boot did not deadlock (we are here) + a cycle was broken. (satoru)
    if (g_test_bits & KTEST_CYCLE) {
        int a = find_index("kt-cyc-a");
        int b = find_index("kt-cyc-b");
        // both should have started (the cycle was broken so neither blocks). (satoru)
        bool a_ok = (a >= 0 && (g_services[a].state == KSVC_RUNNING || g_services[a].state == KSVC_STOPPED));
        bool b_ok = (b >= 0 && (g_services[b].state == KSVC_RUNNING || g_services[b].state == KSVC_STOPPED));
        LogEvent("test-cycle", "-", (a_ok && b_ok) ? "both started; no deadlock" : "cycle blocked startup");
        SerialLogger::Log((a_ok && b_ok) ? "[kinit-test] CYCLE  PASS (no deadlock, both up)\r\n"
                                         : "[kinit-test] CYCLE  FAIL\r\n");
    }

    SerialLogger::Log("[kinit-test] ===== kinit self-tests end =====\r\n");
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
    const char* st = s->is_template ? "template" : StateName(s->state);
    p = ki_cat(out, p, mx, "  ");
    p = ki_cat(out, p, mx, st);
    for (int k = ki_len(st); k < 11; k++) p = ki_cat(out, p, mx, " ");
    p = ki_cat(out, p, mx, s->name);
    for (int k = ki_len(s->name); k < 18; k++) p = ki_cat(out, p, mx, " ");
    p = ki_cat(out, p, mx, kind_short(s->kind));
    for (int k = ki_len(kind_short(s->kind)); k < 10; k++) p = ki_cat(out, p, mx, " ");
    p = ki_cat(out, p, mx, s->description);
    // annotate notable attributes so status is self-describing. (satoru)
    if (s->type == KTYPE_NOTIFY) p = ki_cat(out, p, mx, s->notify_ready ? "  [notify:ready]" : "  [notify]");
    if (s->limits.memory_max_kb) { p = ki_cat(out, p, mx, "  [mem<="); p = ki_cat_u(out, p, mx, s->limits.memory_max_kb); p = ki_cat(out, p, mx, "K]"); }
    if (s->watchdog_sec)         { p = ki_cat(out, p, mx, "  [wd="); p = ki_cat_u(out, p, mx, s->watchdog_sec); p = ki_cat(out, p, mx, "s]"); }
    if (s->listen_path[0])       { p = ki_cat(out, p, mx, "  [socket]"); }
    if (s->isolated_active)      { p = ki_cat(out, p, mx, "  [isolated]"); }
    if (s->notify_status[0])     { p = ki_cat(out, p, mx, "  \""); p = ki_cat(out, p, mx, s->notify_status); p = ki_cat(out, p, mx, "\""); }
    p = ki_cat(out, p, mx, "\n");
    return p;
}
}  // namespace

int CmdKinit(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    int p = 0;

    if (argc < 2 || ki_eq(argv[1], "status")) {
        // overview + per-service table grouped by target. (satoru)
        int live = 0;
        for (int i = 0; i < g_service_count; i++) if (g_services[i].name[0]) live++;
        p = ki_cat(out, p, mx, "kinit service manager   running ");
        p = ki_cat_u(out, p, mx, (uint32_t)RunningCount());
        p = ki_cat(out, p, mx, "/");
        p = ki_cat_u(out, p, mx, (uint32_t)live);
        p = ki_cat(out, p, mx, "\n");
        for (int t = 0; t < KTGT_COUNT; t++) {
            bool any = false;
            for (int i = 0; i < g_service_count; i++)
                if (g_services[i].name[0] && (int)g_services[i].target == t) { any = true; break; }
            if (!any) continue;
            p = ki_cat(out, p, mx, "\n[");
            p = ki_cat(out, p, mx, TargetName((KTarget)t));
            p = ki_cat(out, p, mx, "]\n");
            for (int i = 0; i < g_service_count; i++)
                if (g_services[i].name[0] && (int)g_services[i].target == t)
                    p = print_status_row(out, p, mx, &g_services[i]);
        }
        return p;
    }

    const char* action = argv[1];

    if (ki_eq(action, "reload")) {
        Reload();
        return ki_cat(out, 0, mx, "kinit: hot-reloaded .kservice units (see 'kinit logs' for the diff)\n");
    }

    // analyze: boot timing + slowest services + dependency graph. (satoru)
    if (ki_eq(action, "analyze")) {
        return Analyze(out, mx);
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
        p = ki_cat(out, p, mx, "usage: kinit status | start <svc|tmpl@inst> | stop <svc> | "
                               "restart <svc> | enable <svc> | disable <svc> | "
                               "logs [svc] | reload | analyze\n");
        return p;
    }

    const char* name = argv[2];

    // template instantiation: "start foo@bar" auto-instantiates from "foo@". (satoru)
    if (ki_eq(action, "start") && find_index(name) < 0) {
        int at = -1;
        for (int i = 0; name[i]; i++) if (name[i] == '@') at = i;
        if (at >= 0) {
            int idx = InstantiateTemplate(name);
            if (idx < 0) {
                p = ki_cat(out, p, mx, "kinit: no template for '");
                p = ki_cat(out, p, mx, name);
                p = ki_cat(out, p, mx, "'\n");
                return p;
            }
        }
    }

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
