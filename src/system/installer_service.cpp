// installer_service.cpp: unix socket service for the graphical installer. it
// validates the request, asks the admin gate, then runs the disk job on a
// launcher kernel process (sched=2). in the old single shell mode, where the
// only detached slot already runs the app itself, the in-kernel app's job runs
// on an installer-job kernel proc and a ring-3 client's job runs inline in its
// request. (satoru)
#include "installer_service.h"
#include "installer_disk.h"
#include "../net/unix_socket.h"
#include "../kernel/heap.h"
#include "../proc/scheduler.h"
#include "../proc/spinlock.h"
#include "../security/admin.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../proc/kernel_processes.h"
#include "../shell/shell.h"

namespace {

bool g_auto = false;
bool g_init = false;

struct Job {
    volatile uint32_t active;
    volatile uint32_t done;
    volatile uint32_t failed;
    volatile uint32_t pct;
    char stage[64];
    char detail[160];
    int disk;
    uint32_t kfs_mb;
    uint32_t convert_ack;
    uint32_t keep_ack;
    uint32_t os_ack;
};

Job g_job;
Spinlock g_job_lock;
Spinlock g_act_lock;

int s_len(const char* s) { int n = 0; if (s) while (s[n]) n++; return n; }
void s_cpy(char* d, const char* s, int mx) {
    if (!d || mx < 1) return;
    int i = 0;
    if (s) while (s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

void s_cat(char* d, const char* s, int mx) {
    if (!d || mx < 1) return;
    int n = s_len(d);
    int i = 0;
    if (s) while (s[i] && n < mx - 1) d[n++] = s[i++];
    d[n] = 0;
}

void set_stage(const char* stage, const char* detail) {
    SpinLockGuard g(g_job_lock);
    if (stage) s_cpy(g_job.stage, stage, sizeof(g_job.stage));
    if (detail) s_cpy(g_job.detail, detail, sizeof(g_job.detail));
}

void job_progress(uint32_t pct, const char* stage, void*) {
    if (pct > 100) pct = 100;
    __atomic_store_n(&g_job.pct, pct, __ATOMIC_RELEASE);
    set_stage(stage, nullptr);
    // let the gui, the log worker and the shell run between disk chunks. (satoru)
    Scheduler::CondResched();
}


// old scheduler mode, in-kernel app: an inline job ran on the gui process and
// froze it for the whole install, so the progress screen never moved. one small
// kernel proc runs the job instead; the gates in front of it are unchanged. it
// is spawned from the gui pass (pump) the first time it is needed. a ring-3
// client keeps the inline job: in this mode no kernel proc gets the bsp while a
// ring-3 app is in front (measured: the worker sat ready for the whole run).
// sched=2 keeps its launcher path. (satoru)
volatile uint32_t g_worker_kick = 0;
volatile uint32_t g_worker_want = 0;
bool g_worker_up = false;

[[noreturn]] void job_worker_entry() {
    for (;;) {
        if (__atomic_exchange_n(&g_worker_kick, 0u, __ATOMIC_ACQ_REL)) {
            SerialLogger::Log("[installer] job worker: running the install\r\n");
            InstallerService::RunJob(nullptr, 0);
        }
        Scheduler::SleepMs(100);
    }
}

bool job_worker_kick() {
    if (Scheduler::SchedV2()) return false;
    __atomic_store_n(&g_worker_kick, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&g_worker_want, 1u, __ATOMIC_RELEASE);
    return true;
}

// gui pass: bring the worker up when a job waits for it (satoru)
void job_worker_pump() {
    if (g_worker_up || !__atomic_load_n(&g_worker_want, __ATOMIC_ACQUIRE)) return;
    g_worker_up = true;
    if (Scheduler::SpawnKernelProcess("installer-job", job_worker_entry, PRIO_NORMAL, 512, 8192)) {
        SerialLogger::Log("[installer] job worker up\r\n");
        return;
    }
    // no worker: run the queued job here, blocking the gui until it is done (satoru)
    SerialLogger::Log("[installer] job worker spawn failed, running inline\r\n");
    if (__atomic_exchange_n(&g_worker_kick, 0u, __ATOMIC_ACQ_REL)) InstallerService::RunJob(nullptr, 0);
}

bool start_job(int disk, uint32_t kfs_mb, uint32_t convert_ack, uint32_t keep_ack,
               uint32_t os_ack, const char* why, bool worker_ok) {
    {
        SpinLockGuard g(g_job_lock);
        g_job.disk = disk;
        g_job.kfs_mb = kfs_mb;
        g_job.convert_ack = convert_ack;
        g_job.keep_ack = keep_ack;
        g_job.os_ack = os_ack;
        g_job.pct = 0;
        g_job.done = 0;
        g_job.failed = 0;
        s_cpy(g_job.stage, "starting the install", sizeof(g_job.stage));
        s_cpy(g_job.detail, why ? why : "", sizeof(g_job.detail));
        __atomic_store_n(&g_job.active, 1u, __ATOMIC_RELEASE);
    }
    // sched=2 has launcher kernel procs; run the job there so the gui keeps
    // painting. in the old single-shell mode the detached slot is already held
    // by the app's own `kapp`: the in-kernel app hands the job to the
    // installer-job worker, a ring-3 client (worker_ok false) runs it inline
    // and blocks until the disk work is done. (satoru)
    if (!Scheduler::DetachedLaunch() || !KernelProcesses::RunShellCommandDetached("installerjob")) {
        if (!worker_ok || !job_worker_kick()) InstallerService::RunJob(nullptr, 0);
    }
    return true;
}

void send_reply(int sd, bool ok, int code, const char* why) {
    KinReply r;
    memset(&r, 0, sizeof(r));
    r.magic = KIN_REPLY_MAGIC;
    r.ok = ok ? 1 : 0;
    r.code = code;
    s_cpy(r.why, why ? why : "", sizeof(r.why));
    UnixSocket::KernelInject(sd, &r, (int)sizeof(r));
}

void handle(int sd, const KinReq& r) {
    if (r.magic != KIN_MAGIC) {
        send_reply(sd, false, 1, "bad request magic");
        return;
    }
    switch (r.kind) {
    case KIN_LIST: {
        KinList* list = (KinList*)KernelHeap::Alloc(sizeof(KinList));
        if (!list) { send_reply(sd, false, 2, "out of memory"); return; }
        int n = InstallerDisk::Scan(list);
        if (n < 0) { KernelHeap::Free(list); send_reply(sd, false, 2, "disk scan failed"); return; }
        send_reply(sd, true, 0, "ok");
        UnixSocket::KernelInject(sd, list, (int)sizeof(KinList));
        KernelHeap::Free(list);
        return;
    }
    case KIN_PLAN: {
        KinPlan* plan = (KinPlan*)KernelHeap::Alloc(sizeof(KinPlan));
        if (!plan) { send_reply(sd, false, 2, "out of memory"); return; }
        InstallerDisk::Plan(r.disk, (uint32_t)(r.kfs_mb > 0 ? r.kfs_mb : 0), plan);
        plan->auto_mode = g_auto ? 1u : 0u;
        send_reply(sd, plan->ok != 0, plan->ok ? 0 : 3, plan->why);
        UnixSocket::KernelInject(sd, plan, (int)sizeof(KinPlan));
        KernelHeap::Free(plan);
        return;
    }
    case KIN_START: {
        if (r.confirm != KIN_CONFIRM) {
            send_reply(sd, false, 4, "the install was not confirmed");
            return;
        }
        if (__atomic_load_n(&g_job.active, __ATOMIC_ACQUIRE)) {
            send_reply(sd, false, 5, "an install is already running");
            return;
        }
        KinPlan plan;
        if (InstallerDisk::Plan(r.disk, (uint32_t)(r.kfs_mb > 0 ? r.kfs_mb : 0), &plan) != 0 ||
            !plan.ok) {
            send_reply(sd, false, 3, plan.why[0] ? plan.why : "no safe install plan");
            return;
        }
        if (plan.mode == KIN_PLAN_EXISTING) {
            send_reply(sd, false, 6, "Kurono is already installed on this disk");
            return;
        }
        if (plan.need_convert && !r.convert_ack) {
            send_reply(sd, false, 7, "the MBR to GPT conversion was not confirmed");
            return;
        }
        if (plan.has_other_os && !r.keep_ack) {
            send_reply(sd, false, 8, "the existing partitions were not confirmed");
            return;
        }
        if (plan.need_os_ack && !r.os_ack) {
            send_reply(sd, false, 10, plan.os_line[0] ? plan.os_line : "the other system on this disk was not confirmed");
            return;
        }
        char detail[160];
        int p = 0;
        const char* a = "create a KURONO partition on ";
        while (*a && p < (int)sizeof(detail) - 1) detail[p++] = *a++;
        for (int i = 0; plan.disk_name[i] && p < (int)sizeof(detail) - 1; i++)
            detail[p++] = plan.disk_name[i];
        if (plan.need_convert && p < (int)sizeof(detail) - 8) {
            const char* b = " (MBR to GPT)";
            while (*b && p < (int)sizeof(detail) - 1) detail[p++] = *b++;
        }
        detail[p] = 0;

        UnixSocket::Credentials cr;
        memset(&cr, 0, sizeof(cr));
        UnixSocket::GetPeerCred(sd, &cr);
        if (!AdminGate::AuthorizeForPid(cr.pid, cr.uid, "install Kurono OS", detail)) {
            send_reply(sd, false, 9, "administrator approval was denied");
            return;
        }
        if (!start_job(r.disk, (uint32_t)(r.kfs_mb > 0 ? r.kfs_mb : 0),
                       r.convert_ack, r.keep_ack, r.os_ack, detail, false)) {
            send_reply(sd, false, 2, "could not start the install worker");
            return;
        }
        send_reply(sd, true, 0, "started");
        return;
    }
    case KIN_STATUS: {
        KinStatus st;
        memset(&st, 0, sizeof(st));
        st.magic = KIN_STATUS_MAGIC;
        st.running = __atomic_load_n(&g_job.active, __ATOMIC_ACQUIRE);
        st.done = __atomic_load_n(&g_job.done, __ATOMIC_ACQUIRE);
        st.failed = __atomic_load_n(&g_job.failed, __ATOMIC_ACQUIRE);
        st.pct = __atomic_load_n(&g_job.pct, __ATOMIC_ACQUIRE);
        {
            SpinLockGuard g(g_job_lock);
            s_cpy(st.stage, g_job.stage, sizeof(st.stage));
            s_cpy(st.detail, g_job.detail, sizeof(st.detail));
        }
        send_reply(sd, true, 0, "ok");
        UnixSocket::KernelInject(sd, &st, (int)sizeof(st));
        return;
    }
    default:
        send_reply(sd, false, 1, "unknown request");
        return;
    }
}

uint8_t g_rx[UnixSocket::UNIX_SVC_SLOTS][sizeof(KinReq)];
int g_rx_len[UnixSocket::UNIX_SVC_SLOTS];

// connections the service accepted, so Pump can close the ones whose client hung up:
// nothing else ever closed them, one leaked socket (and its 64 kb ring) per request
// connection until new sds passed UNIX_SVC_SLOTS and every request was dropped. (satoru)
volatile uint8_t g_live[UnixSocket::UNIX_SVC_SLOTS];

void svc_on_connect(int, int new_sd, void*) {
    if (new_sd >= 0 && new_sd < UnixSocket::UNIX_SVC_SLOTS) { g_rx_len[new_sd] = 0; g_live[new_sd] = 1; }
}

void svc_on_data(int sd, const uint8_t* data, int len, void*) {
    if (sd < 0 || sd >= UnixSocket::UNIX_SVC_SLOTS || !data || len <= 0) return;
    int have = g_rx_len[sd];
    if (have < 0 || have > (int)sizeof(KinReq)) have = 0;
    for (int i = 0; i < len && have < (int)sizeof(KinReq); i++) g_rx[sd][have++] = data[i];
    while (have >= (int)sizeof(KinReq)) {
        KinReq r;
        memcpy(&r, g_rx[sd], sizeof(KinReq));
        handle(sd, r);
        int rem = have - (int)sizeof(KinReq);
        for (int i = 0; i < rem; i++) g_rx[sd][i] = g_rx[sd][i + (int)sizeof(KinReq)];
        have = rem;
    }
    g_rx_len[sd] = have;
}

}  // namespace

namespace InstallerService {
int RunJob(char* out, int max_out);

void Init() {
    if (g_init) return;
    g_init = true;
    KVFS::Mkdirs("/system/run/user/1000");
    int sd = UnixSocket::Create(UnixSocket::UNIX_SOCK_STREAM);
    if (sd < 0) return;
    if (UnixSocket::Bind(sd, KIN_PATH) < 0) { UnixSocket::Close(sd); return; }
    UnixSocket::Listen(sd, 8);
    UnixSocket::RegisterServer(sd, svc_on_connect, svc_on_data, nullptr);
    SerialLogger::Log("[installer] service on " KIN_PATH "\r\n");
}

void Pump() {
    Init();
    for (int sd = 0; sd < UnixSocket::UNIX_SVC_SLOTS; sd++) {
        if (!g_live[sd] || !UnixSocket::PeerClosed(sd)) continue;
        g_live[sd] = 0;
        g_rx_len[sd] = 0;
        UnixSocket::Close(sd);
    }
    job_worker_pump();
}

void SetAuto(bool on) { g_auto = on; }

bool PayloadAvailable() { return InstallerDisk::PayloadAvailable(); }

int List(KinList* out) { return InstallerDisk::Scan(out); }

int Plan(int disk, uint32_t kfs_mb, KinPlan* out) {
    int rc = InstallerDisk::Plan(disk, kfs_mb, out);
    if (out) out->auto_mode = g_auto ? 1u : 0u;
    return rc;
}

int Start(const KinReq& req, uint32_t peer_pid, uint32_t peer_uid, KinReply* rep) {
    if (!rep) return -1;
    memset(rep, 0, sizeof(*rep));
    rep->magic = KIN_REPLY_MAGIC;
    if (req.magic != KIN_MAGIC || req.kind != KIN_START) {
        rep->code = 1;
        s_cpy(rep->why, "bad request", sizeof(rep->why));
        return -1;
    }
    if (req.confirm != KIN_CONFIRM) {
        rep->code = 4;
        s_cpy(rep->why, "the install was not confirmed", sizeof(rep->why));
        return -1;
    }
    if (__atomic_load_n(&g_job.active, __ATOMIC_ACQUIRE)) {
        rep->code = 5;
        s_cpy(rep->why, "an install is already running", sizeof(rep->why));
        return -1;
    }
    KinPlan plan;
    if (InstallerDisk::Plan(req.disk, (uint32_t)(req.kfs_mb > 0 ? req.kfs_mb : 0), &plan) != 0 ||
        !plan.ok) {
        rep->code = 3;
        s_cpy(rep->why, plan.why[0] ? plan.why : "no safe install plan", sizeof(rep->why));
        return -1;
    }
    if (plan.mode == KIN_PLAN_EXISTING) {
        rep->code = 6;
        s_cpy(rep->why, "Kurono is already installed on this disk", sizeof(rep->why));
        return -1;
    }
    if (plan.need_convert && !req.convert_ack) {
        rep->code = 7;
        s_cpy(rep->why, "the MBR to GPT conversion was not confirmed", sizeof(rep->why));
        return -1;
    }
    if (plan.has_other_os && !req.keep_ack) {
        rep->code = 8;
        s_cpy(rep->why, "the existing partitions were not confirmed", sizeof(rep->why));
        return -1;
    }
    if (plan.need_os_ack && !req.os_ack) {
        rep->code = 10;
        s_cpy(rep->why, plan.os_line[0] ? plan.os_line : "the other system on this disk was not confirmed", sizeof(rep->why));
        return -1;
    }
    char detail[160];
    s_cpy(detail, "create a KURONO partition on ", sizeof(detail));
    s_cat(detail, plan.disk_name, sizeof(detail));
    if (!AdminGate::AuthorizeForPid(peer_pid, peer_uid, "install Kurono OS", detail)) {
        rep->code = 9;
        s_cpy(rep->why, "administrator approval was denied", sizeof(rep->why));
        return -1;
    }
    if (!start_job(req.disk, (uint32_t)(req.kfs_mb > 0 ? req.kfs_mb : 0),
                   req.convert_ack, req.keep_ack, req.os_ack, detail, true)) {
        rep->code = 2;
        s_cpy(rep->why, "could not start the install worker", sizeof(rep->why));
        return -1;
    }
    rep->ok = 1;
    rep->code = 0;
    s_cpy(rep->why, "started", sizeof(rep->why));
    return 0;
}

int Status(KinStatus* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->magic = KIN_STATUS_MAGIC;
    out->running = __atomic_load_n(&g_job.active, __ATOMIC_ACQUIRE);
    out->done = __atomic_load_n(&g_job.done, __ATOMIC_ACQUIRE);
    out->failed = __atomic_load_n(&g_job.failed, __ATOMIC_ACQUIRE);
    out->pct = __atomic_load_n(&g_job.pct, __ATOMIC_ACQUIRE);
    {
        SpinLockGuard g(g_job_lock);
        s_cpy(out->stage, g_job.stage, sizeof(out->stage));
        s_cpy(out->detail, g_job.detail, sizeof(out->detail));
    }
    return 0;
}

// the install body, run on a launcher kernel process through the shell. (satoru)
namespace {
int cmd_installer_job(void*, int, const char**, char* out, int mx) {
    return InstallerService::RunJob(out, mx);
}
}

void RegisterShellCommands(void* shell) {
    Shell* sh = (Shell*)shell;
    if (!sh) return;
    sh->RegisterCommand("installerjob", cmd_installer_job, "internal installer worker");
}

int RunJob(char* out, int max_out) {
    int disk;
    uint32_t kfs_mb, conv, keep, osack;
    {
        SpinLockGuard g(g_job_lock);
        disk = g_job.disk;
        kfs_mb = g_job.kfs_mb;
        conv = g_job.convert_ack;
        keep = g_job.keep_ack;
        osack = g_job.os_ack;
    }
    InstallerDisk::Progress p;
    p.fn = job_progress;
    p.ctx = nullptr;
    char why[128];
    why[0] = 0;
    int rc = InstallerDisk::Install(disk, kfs_mb, conv, keep, osack, &p, why, sizeof(why));
    if (rc == 0) {
        set_stage("installation complete", "Kurono OS is ready.");
        __atomic_store_n(&g_job.pct, 100u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_job.done, 1u, __ATOMIC_RELEASE);
    } else {
        set_stage("installation failed", why[0] ? why : "unknown error");
        __atomic_store_n(&g_job.failed, 1u, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_job.active, 0u, __ATOMIC_RELEASE);
    int n = 0;
    const char* s = rc == 0 ? "install complete" : "install failed";
    if (out) {
        while (*s && n < max_out - 1) out[n++] = *s++;
        out[n] = 0;
    }
    return n;
}

}  // namespace InstallerService

// end (satoru)
