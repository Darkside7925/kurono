// kld supervisor: restart policy for kld containers (kurono.kld.sup=1). a
// container that exits rc != 0, dies on a fault, sends no first heartbeat
// within 60 s or then goes quiet for 30 s (kurono.kld.sup.hang=<s>) is
// released and started again (1 s, then 4 s); after two failed restarts the
// family is given up. rc == 0 is a clean exit. (satoru)

#include "kld_supervisor.h"
#include "kld_broker.h"
#include "elf_loader.h"
#include "userspace.h"
#include "../proc/scheduler.h"
#include "../proc/spinlock.h"   // kld reload slots (satoru)
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "kcrash.h"
#include "../fs/kvfs.h"   // reload log: image size (satoru)
#include "../linux/linux_syscall.h"   // exit code from the linux record (satoru)

namespace KldSupervisor {
namespace {

constexpr int      MAX_RESTARTS   = 2;
constexpr uint64_t FIRST_BEAT_MS  = 60000;
constexpr int      RC_UNOBSERVED  = -3;   // the leader vanished before its exit was seen (satoru)
uint64_t g_hang_ms = 30000;
constexpr uint64_t RECOVERED_MS   = 30000;
constexpr int      HB_SLOTS       = 16;

bool g_enabled = false;
bool g_kcrash_disk = false;          // kurono.kcrash=1: records + policy on disk (satoru)
volatile uint32_t g_kcrash_state = 0;   // 0 not started, 1 running, 2 done (satoru)

// the first family's runner opens the crash store; the others wait for it. (satoru)
void kcrash_once()
{
    uint32_t z = 0;
    if (__atomic_compare_exchange_n(&g_kcrash_state, &z, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        KCrash::BootInit(g_kcrash_disk);
        __atomic_store_n(&g_kcrash_state, 2u, __ATOMIC_RELEASE);
        return;
    }
    while (__atomic_load_n(&g_kcrash_state, __ATOMIC_ACQUIRE) != 2) Scheduler::SleepMs(20);
}

void fill_record(KCrash::Record& r, const char* fam, uint32_t bdf, uint16_t ven, uint16_t dev,
                 int pid, int rc, bool hung, int attempt, bool gave_up, const char* action)
{
    uint8_t* b = (uint8_t*)&r;
    for (uint32_t i = 0; i < sizeof(r); i++) b[i] = 0;
    r.kind = KCrash::KIND_KLD;
    for (int i = 0; i < 15 && fam[i]; i++) r.family[i] = fam[i];
    r.bdf = bdf; r.vendor = ven; r.device = dev;
    r.pid = pid; r.rc = rc;
    // 128 + n is a death by signal n (139 = segv); 9 = we killed a hang. (satoru)
    r.signal = hung ? 9 : (rc > 128 && rc < 128 + 65) ? rc - 128 : 0;
    r.attempt = (uint16_t)attempt;
    r.result = gave_up ? KCrash::RES_GAVE_UP : KCrash::RES_RESTARTING;
    for (int i = 0; i < 31 && action[i]; i++) r.action[i] = action[i];
}
char g_inject_family[16];
char g_inject_kind[16];   // "segv", "hang", "nobeat", "exit0" or "segv-always", empty = none (satoru)

// heartbeat table keyed by address space. the broker writes, the runner reads.
// gen tells one container instance from the next that reuses the slot. (satoru)
struct HbSlot {
    uint64_t as;
    uint64_t last_ms;
    bool     seen;
    uint32_t gen;
};
HbSlot g_hb[HB_SLOTS];
uint32_t g_hb_gen = 0;
volatile uint32_t g_hb_ops = 0;   // heartbeat ops ever received (satoru)

// `kld reload <family>`: families currently under run_once, and a pending
// reload request per slot (satoru)
constexpr int RL_SLOTS = 8;
struct RlSlot { char fam[16]; volatile uint32_t running; volatile uint32_t want; volatile uint32_t done; };
RlSlot g_rl[RL_SLOTS];
Spinlock g_rl_lock;

int rl_slot(const char* fam, bool create)
{
    g_rl_lock.Lock();
    int free_i = -1;
    for (int i = 0; i < RL_SLOTS; i++) {
        if (g_rl[i].fam[0]) {
            bool eq = true;
            for (int k = 0; k < 16; k++) { if (g_rl[i].fam[k] != fam[k]) { eq = false; break; } if (!fam[k]) break; }
            if (eq) { g_rl_lock.Unlock(); return i; }
        } else if (free_i < 0) free_i = i;
    }
    if (create && free_i >= 0) {
        int k = 0;
        while (fam[k] && k < 15) { g_rl[free_i].fam[k] = fam[k]; k++; }
        g_rl[free_i].fam[k] = 0;
    }
    g_rl_lock.Unlock();
    return create ? free_i : -1;
}

bool streq(const char* a, const char* b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

const char* find_token(const char* s, const char* prefix)
{
    const char* p = s;
    while (p && *p) {
        while (*p == ' ' || *p == '\t') p++;
        const char* t = p;
        const char* q = prefix;
        while (*q && *t == *q) { t++; q++; }
        if (!*q) return t;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    return nullptr;
}

int hb_register(uint64_t as)
{
    for (int i = 0; i < HB_SLOTS; i++) {
        uint64_t z = 0;
        if (__atomic_compare_exchange_n(&g_hb[i].as, &z, as, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&g_hb[i].last_ms, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&g_hb[i].seen, false, __ATOMIC_RELEASE);
            __atomic_store_n(&g_hb[i].gen, __atomic_add_fetch(&g_hb_gen, 1u, __ATOMIC_ACQ_REL), __ATOMIC_RELEASE);
            return i;
        }
    }
    return -1;
}

void hb_clear(int slot)
{
    if (slot < 0) return;
    __atomic_store_n(&g_hb[slot].seen, false, __ATOMIC_RELEASE);
    __atomic_store_n(&g_hb[slot].as, 0, __ATOMIC_RELEASE);
}

void log_fam(const char* fam, const char* msg)
{
    SerialLogger::Log("[kld-sup] ");
    SerialLogger::Log(fam);
    SerialLogger::Log(msg);
}

// one container run. returns its exit code; *hung tells whether we killed it. (satoru)
int run_once(Process* p, const char* const* av, const char* const* envp, int slot,
             const char* fam, bool* hung, bool* recovered_log, int rl, bool* reloaded)
{
    *hung = false;
    *reloaded = false;
    if (!Userspace::CanSpawnDetached()) {
        // no detached launch in this boot: the old blocking run, crash restart
        // only (no hang detection possible while we are inside it). (satoru)
        return Userspace::RunProcessWithArgs(p, av, envp);
    }
    DetachedHandle h;
    int sr = Userspace::SpawnDetached(p, av, envp, 0, 0, &h);
    if (sr < 0) return Userspace::RunProcessWithArgs(p, av, envp);
    uint32_t gen = slot >= 0 ? __atomic_load_n(&g_hb[slot].gen, __ATOMIC_ACQUIRE) : 0;
    uint64_t t0 = Timer::GetRealMs64();
    while (Userspace::DetachedAlive(h)) {
        uint64_t now = Timer::GetRealMs64();
        if (!*hung && slot >= 0) {
            // the clock starts at container start: no first beat in 60 s is a
            // hang too, then g_hang_ms of silence after the last beat. (satoru)
            bool seen = __atomic_load_n(&g_hb[slot].seen, __ATOMIC_ACQUIRE);
            uint64_t last = seen ? __atomic_load_n(&g_hb[slot].last_ms, __ATOMIC_ACQUIRE) : t0;
            uint64_t bound = seen ? g_hang_ms : FIRST_BEAT_MS;
            if (now > last && now - last > bound) {
                // same instance? (our slot generation + the scheduler still has
                // this pid as this struct, not yet terminated) (satoru)
                int st = 0;
                bool same = __atomic_load_n(&g_hb[slot].gen, __ATOMIC_ACQUIRE) == gen &&
                            __atomic_load_n(&g_hb[slot].as, __ATOMIC_ACQUIRE) == h.as &&
                            Scheduler::TaskStateByPid(h.kpid, h.proc, &st, nullptr) &&
                            st != (int)Process_Terminated;
                if (same) {
                    *hung = true;
                    log_fam(fam, seen ? " hung (no heartbeat for " : " hung (no first heartbeat in ");
                    SerialLogger::LogDec((int)(now - last));
                    SerialLogger::Log(" ms)\r\n");
                    Scheduler::KillProcessGroupAsync(h.kpid, nullptr);
                }
            }
        }
        if (rl >= 0 && !*reloaded && __atomic_load_n(&g_rl[rl].want, __ATOMIC_ACQUIRE)) {
            // a requested reload: stop this instance, the loop loads the
            // (new) image from the same path without counting a crash (satoru)
            __atomic_store_n(&g_rl[rl].want, 0u, __ATOMIC_RELEASE);
            *reloaded = true;
            log_fam(fam, " reload requested, stopping the container\r\n");
            Scheduler::KillProcessGroupAsync(h.kpid, nullptr);
        }
        if (recovered_log && !*recovered_log && !*hung && now - t0 >= RECOVERED_MS) {
            *recovered_log = true;
            log_fam(fam, " recovered\r\n");
        }
        KCrash::FlushPending();   // records a busy disk deferred (satoru)
        // 5 ms like RunDetached: a leader reaped between two polls is never
        // seen Terminated. (satoru)
        Scheduler::SleepMs(5);
    }
    // the exit code is read by pid under the scheduler lock (DetachedAlive) or,
    // if the leader left the task list unseen, from its linux record (a static
    // table, revalidated by slot + linux pid under the layer lock) before
    // ReapDetached destroys it. never from the Process struct. still unknown
    // = a failure, never 0. (satoru)
    bool known = h.exit_known;
    int lp_code = 0;
    if (!known) {
        LinuxSyscall::LayerLock();
        LinuxProcess* lp = LinuxSyscall::GetProcess(h.slot);
        if (lp && lp->pid == h.lpid && lp->exited) { lp_code = lp->exit_code; known = true; }
        LinuxSyscall::LayerUnlock();
    }
    int rc = Userspace::ReapDetached(h);
    if (h.exit_known == false && known) rc = lp_code;
    if (!known) {
        // a reload killed it on purpose: its exit code does not matter (satoru)
        if (!*reloaded) log_fam(fam, " exit not observed, treated as a failure\r\n");
        rc = RC_UNOBSERVED;
    }
    return rc;
}

// the dead instance's address space (and its fixed image-window frames) goes
// to the deferred reaper; wait for it to drain before reloading, max 5 s. a
// fault-killed group may never show in the reap counters, so the load retry
// below covers the rest (up to 25 s). (satoru)
void wait_old_freed(const char* fam, uint64_t freed_before)
{
    uint64_t t0 = Timer::GetRealMs64();
    for (;;) {
        uint64_t fr = 0, lk = 0; int pe = 0;
        Scheduler::ReapStats(&fr, &pe, &lk);
        uint64_t el = Timer::GetRealMs64() - t0;
        if ((fr > freed_before && pe == 0) || el >= 5000) {
            log_fam(fam, " waiting for the old instance to be freed (");
            SerialLogger::LogDec((int)el);
            SerialLogger::Log(" ms)\r\n");
            return;
        }
        Scheduler::SleepMs(100);
    }
}

}  // namespace

void BootInit(const char* cmdline)
{
    if (!cmdline) return;
    const char* s = find_token(cmdline, "kurono.kld.sup=");
    g_enabled = s && s[0] == '1';
    const char* hg = find_token(cmdline, "kurono.kld.sup.hang=");
    if (hg) {
        uint64_t v = 0;
        while (*hg >= '0' && *hg <= '9' && v < 100000) v = v * 10 + (uint64_t)(*hg++ - '0');
        if (v >= 5 && v <= 3600) g_hang_ms = v * 1000;
    }
    const char* kc = find_token(cmdline, "kurono.kcrash=");
    g_kcrash_disk = kc && kc[0] == '1';
    g_inject_family[0] = 0;
    g_inject_kind[0] = 0;
    const char* f = find_token(cmdline, "kurono.faultinject=kld:");
    if (!f) return;
    int n = 0;
    while (f[n] && f[n] != ':' && f[n] != ' ' && n < 15) { g_inject_family[n] = f[n]; n++; }
    g_inject_family[n] = 0;
    if (f[n] != ':') { g_inject_family[0] = 0; return; }
    const char* k = f + n + 1;
    int m = 0;
    while (k[m] && k[m] != ' ' && k[m] != '\t' && m < 15) { g_inject_kind[m] = k[m]; m++; }
    g_inject_kind[m] = 0;
    if (!streq(g_inject_kind, "segv") && !streq(g_inject_kind, "hang") &&
        !streq(g_inject_kind, "exit0") && !streq(g_inject_kind, "segv-always") &&
        !streq(g_inject_kind, "nobeat")) {
        g_inject_family[0] = 0;
        g_inject_kind[0] = 0;
    }
}

bool Enabled() { return g_enabled; }

int RequestReload(const char* family)
{
    if (!g_enabled) return -1;
    int i = rl_slot(family, false);
    if (i < 0 || !__atomic_load_n(&g_rl[i].running, __ATOMIC_ACQUIRE)) return -2;
    __atomic_store_n(&g_rl[i].want, 1u, __ATOMIC_RELEASE);
    return 0;
}

uint32_t ReloadCount(const char* family)
{
    int i = rl_slot(family, false);
    return i < 0 ? 0 : __atomic_load_n(&g_rl[i].done, __ATOMIC_ACQUIRE);
}

bool Running(const char* family)
{
    int i = rl_slot(family, false);
    return i >= 0 && __atomic_load_n(&g_rl[i].running, __ATOMIC_ACQUIRE);
}

void Heartbeat(uint64_t as)
{
    if (!as) return;
    // the first op ever is logged, so a boot without it proves none came. (satoru)
    if (__atomic_fetch_add(&g_hb_ops, 1u, __ATOMIC_RELAXED) == 0)
        SerialLogger::Log("[kld-sup] first heartbeat op received\r\n");
    for (int i = 0; i < HB_SLOTS; i++) {
        if (__atomic_load_n(&g_hb[i].as, __ATOMIC_ACQUIRE) != as) continue;
        __atomic_store_n(&g_hb[i].last_ms, Timer::GetRealMs64(), __ATOMIC_RELEASE);
        __atomic_store_n(&g_hb[i].seen, true, __ATOMIC_RELEASE);
        return;
    }
}

void Run(const char* family, const char* path)
{
    int restarts = 0;
    bool first = true;
    kcrash_once();
    if (KCrash::FamilyDisabled(family)) {
        log_fam(family, " disabled in KDRIVERS.CFG, not started\r\n");
        for (;;) Scheduler::SleepMs(60000);
    }
    uint32_t bdf = 0; uint16_t ven = 0, dev = 0;
    for (;;) {
        int pid = 0;
        Process* p = ElfLoader::LoadELF64FromVFS(path, family);
        // a restart can race the reaper (wait_old_freed bounds that); a load
        // that still fails counts as a failed restart. (satoru)
        for (int tries = 0; !p && !first && tries < 50; tries++) {
            Scheduler::SleepMs(500);
            p = ElfLoader::LoadELF64FromVFS(path, family);
        }
        int rc;
        bool hung = false;
        bool recovered = false;
        bool reloaded = false;
        uint64_t ran = 0;
        if (!p) {
            SerialLogger::Log("[kld-auto] load FAILED\r\n");
            if (first) break;
            log_fam(family, " reload FAILED\r\n");
            rc = -2;
        } else {
            // the fault injection goes to the FIRST start only, so the restart must
            // come up clean. (satoru)
            // segv-always (test only) re-arms on every start. (satoru)
            bool always = streq(g_inject_kind, "segv-always");
            bool inject = (first || always) && g_inject_kind[0] && streq(g_inject_family, family);
            const char* inj_env = streq(g_inject_kind, "hang")  ? "KLD_FAULTINJECT=hang"
                                : streq(g_inject_kind, "nobeat") ? "KLD_FAULTINJECT=nobeat"
                                : streq(g_inject_kind, "exit0") ? "KLD_FAULTINJECT=exit0"
                                                                : "KLD_FAULTINJECT=segv";
            const char* av[] = { path, nullptr };
            // KLD_SUPERVISED=1 starts the shim's heartbeat thread; an unsupervised
            // container never sends the op. (satoru)
            const char* envp[] = { "PATH=/usr/bin", "KLD_SUPERVISED=1", inject ? inj_env : nullptr, nullptr };
            if (inject) {
                log_fam(family, " fault injection armed: ");
                SerialLogger::Log(g_inject_kind);
                SerialLogger::Log("\r\n");
            }
            first = false;
            uint64_t as = p->address_space;
            pid = (int)p->pid;
            KldBroker::SetDmaBudgetForName(as, path, (int)p->pid);
            uint32_t inst = KldBroker::ContainerId(as);   // broker slot identity (satoru)
            int slot = hb_register(as);
            uint64_t t0 = Timer::GetRealMs64();
            uint64_t fr0 = 0, lk0 = 0; int pe0 = 0;
            Scheduler::ReapStats(&fr0, &pe0, &lk0);   // reaps before this run (satoru)
            bool* rlog = restarts > 0 ? &recovered : nullptr;
            int rl = rl_slot(family, true);
            if (rl >= 0) __atomic_store_n(&g_rl[rl].running, 1u, __ATOMIC_RELEASE);
            rc = run_once(p, av, envp, slot, family, &hung, rlog, rl, &reloaded);
            if (rl >= 0) __atomic_store_n(&g_rl[rl].running, 0u, __ATOMIC_RELEASE);
            ran = Timer::GetRealMs64() - t0;
            SerialLogger::Log("[kld-auto] ");
            SerialLogger::Log(family);
            SerialLogger::Log(" container exited rc=");
            SerialLogger::LogDec(rc);
            SerialLogger::Log("\r\n");
            // device identity for the crash record, before the grants go. (satoru)
            // release keyed on this instance: skipped when the broker record is
            // gone or now belongs to a different container. (satoru)
            uint32_t now_id = KldBroker::ContainerId(as);
            if (now_id == inst && inst != 0) {
                KldBroker::GrantInfoForRelease(as, &bdf, &ven, &dev);
                KldBroker::ReleaseInstance(as, inst);
            } else {
                log_fam(family, now_id ? " broker record belongs to another instance, release skipped\r\n"
                                       : " broker record already released\r\n");
            }
            hb_clear(slot);
            wait_old_freed(family, fr0);
        }
        if (reloaded) {
            // not a failure: no restart budget, no crash record, no delay (satoru)
            restarts = 0;
            log_fam(family, " loading ");
            SerialLogger::Log(path);
            SerialLogger::Log(" (");
            SerialLogger::LogDec(KVFS::GetFileSize(path));
            SerialLogger::Log(" bytes)\r\n");
            int rl = rl_slot(family, false);
            if (rl >= 0) __atomic_fetch_add(&g_rl[rl].done, 1u, __ATOMIC_ACQ_REL);
            log_fam(family, " reloading after ");
            SerialLogger::LogDec((int)ran);
            SerialLogger::Log(" ms\r\n");
            continue;
        }
        if (hung && rc == 0) rc = -9;
        if (rc == 0) break;   // clean exit: no restart (satoru)
        if (restarts > 0 && (recovered || ran >= RECOVERED_MS)) {
            if (!recovered) log_fam(family, " recovered\r\n");
            restarts = 0;
        }
        if (restarts >= MAX_RESTARTS) {
            KCrash::Record r;
            fill_record(r, family, bdf, ven, dev, pid, rc, hung, restarts, true,
                        "gave up after 2 restarts");
            KCrash::Report(r);
            KCrash::NoteGiveUp(family);
            log_fam(family, " FAILED after 2 restarts\r\n");
            break;
        }
        restarts++;
        int delay_s = restarts == 1 ? 1 : 4;
        {
            // the record goes out BEFORE the retry. (satoru)
            KCrash::Record r;
            fill_record(r, family, bdf, ven, dev, pid, rc, hung, restarts, false,
                        rc == -2 ? "reload failed, restart" : hung ? "killed (hang), restart" : "restart");
            KCrash::Report(r);
        }
        log_fam(family, " stopped (rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log("), restart ");
        SerialLogger::LogDec(restarts);
        SerialLogger::Log(" of 2 in ");
        SerialLogger::LogDec(delay_s);
        SerialLogger::Log(" s\r\n");
        Scheduler::SleepMs((uint32_t)delay_s * 1000);
    }
    // flush whatever a busy disk still holds, then idle. (satoru)
    for (int i = 0; i < 600 && KCrash::Pending(); i++) {
        Scheduler::SleepMs(1000);
        KCrash::FlushPending();
    }
    for (;;) Scheduler::SleepMs(60000);
}

}  // namespace KldSupervisor

// end (satoru)
