//  KLD drm userspace interface, broker side. See kld_dri.h for the design and
//  kld/lx_emul/kimpl/lx_dri.h for the wire format. (satoru)
//
//  end (satoru)

#include "kld_dri.h"
#include "kld_broker.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "../drivers/serial.h"
#include "../proc/scheduler.h"
#include "../proc/smp.h"
#include "../proc/spinlock.h"
#include "../linux/dri_dev.h"
#include "kld_dri_proto.h"
#include "kld_auto.h"
#include "../proc/cgroup.h"
#include "../drivers/timer.h"

extern int  LinuxSyscallFutexWakePhys(uint64_t phys_key, int max);
extern void LinuxSyscallDriWake(uint32_t slot);   // linux_syscall.cpp (satoru)

namespace KldDri {

namespace {

constexpr uint64_t PG = 4096;
constexpr int      MAXOBJ = 512;
constexpr uint64_t OBJ_PAGE_CAP = 16384;       // 64 MiB of object frames per container (satoru)
constexpr uint64_t UA_BUDGET = 16ull << 20;    // caller bytes one request may move (satoru)

struct Req {
    bool       used;
    uint8_t    st;            // 0 queued/serving, 1 uaccess pending, 2 done (satoru)
    bool       abandoned;
    bool       ua_inflight;   // the caller holds bounce references (satoru)
    uint32_t   gen;
    Process*   task;
    uint64_t   as;            // the CALLER's address space (satoru)
    uint32_t   kind, file;
    uint64_t   ua_ptr; uint32_t ua_len, ua_dir;
    uint64_t   ua_total;
    uint32_t   cmd; uint64_t arg, mm_off, mm_len;   // the syscall this request belongs to (satoru)
    Reply      rep;
};

struct File {
    bool     used;
    bool     closing;         // queued for the container, index not reusable yet (satoru)
    bool     close_pending;   // closeq was full, retry (satoru)
    uint32_t gen;
    uint32_t cgen;            // container generation it was opened under (satoru)
    uint32_t minor;
    uint32_t pos;             // closeq position (satoru)
    bool     close_on_reply;  // an abandoned open: close after its reply, never before (satoru)
};

struct Obj {
    bool      used;
    uint32_t  id;
    uint32_t  pages;
    uint64_t  va;
    uint64_t* frames;
};

Spinlock  g_lock;
bool      g_attached;
uint64_t  g_as;
uint32_t  g_cgen;
uint64_t  g_ring_phys;
uint64_t  g_ring_va;
uint8_t*  g_ring;
uint32_t  g_minors;
Req       g_req[LXDRI_SLOTS];
File      g_file[LXDRI_FILES];
Obj       g_obj[MAXOBJ];
uint32_t  g_obj_next = 1;
uint64_t  g_obj_pages;
uint32_t  g_req_ok, g_req_eio, g_ua_ops, g_ua_fail;

inline lxdri_hdr*  H()          { return (lxdri_hdr*)g_ring; }
inline lxdri_slot* S(uint32_t i){ return (lxdri_slot*)(g_ring + LXDRI_SLOT_OFF(i)); }
inline lxdri_file* F(uint32_t i){ return (lxdri_file*)(g_ring + LXDRI_FILES_OFF) + i; }

// ── deferred reference drops (TLB proof) ─────────────────────────────────────
// a frame that was mapped in the container may still be reachable through a stale
// translation on a cpu that missed the shootdown (SMP::BroadcastTlbFlush gives up
// after 20 ms). the references a container mapping held are therefore dropped through
// KldBroker::FreeAfterTlbProof (one mechanism for all of KLD): it stamps the entry
// after the pte is gone and releases it only once a full shootdown passed the stamp.
// the rig knob noflush skips this file's own broadcast, so the frames stay queued until
// somebody else's shootdown proves them. (satoru)
uint32_t  g_dq_pages;      // pages handed to the proof queue (satoru)
bool      g_noflush;       // rig: skip the broadcast so the queue is what proves (satoru)
uint32_t  g_dq_nflog;

uint64_t  g_nf_stamp;      // rig noflush: stamp of the last queued unmap, 0 once proven and logged (satoru)
uint32_t  g_nf_pages;
uint32_t  g_nf_plog;

// syscall context: release what is proven; buy the proof with one broadcast when the queue is not empty
void dq_drain() {
    KldBroker::DrainTlbProven(!g_noflush);
    if (g_noflush && g_nf_stamp && SMP::TlbFlushFullSeq() > g_nf_stamp) {     // rig: say when somebody else's shootdown proved it (satoru)
        if (g_nf_plog++ < 6) {
            SerialLogger::Log("[kld-dri] noflush: stamp "); SerialLogger::LogDec((int)g_nf_stamp);
            SerialLogger::Log(" is now proven (full shootdown seq "); SerialLogger::LogDec((int)SMP::TlbFlushFullSeq());
            SerialLogger::Log(") by another shootdown, the broker queue releases "); SerialLogger::LogDec((int)g_nf_pages);
            SerialLogger::Log(" pages, free frames "); SerialLogger::LogDec((int)PMM::GetFreeFrames()); SerialLogger::Log("\r\n");
        }
        g_nf_stamp = 0;
    }
}

// the ptes of [phys, +pages) are gone: `refs` references per page are dropped once proven
void dq_push_run(uint64_t phys, uint64_t pages, uint32_t refs) {
    g_dq_pages += (uint32_t)pages;
    KldBroker::FreeAfterTlbProof(phys, pages, refs);
}

// clear one container pte and keep the cgroup ledger balanced (vmm uncharges only when it frees
// the frame itself, which we must not do before the proof). (satoru)
void unmap_page_ledger(uint64_t as, uint64_t va) {
    bool was = KernelVMM::QueryMappingInAddressSpace(as, va) != 0;
    KernelVMM::UnmapPageInAddressSpace(as, va, false);
    if (was) Cgroup::UnchargeUserPages(as, PG);
}

// unmap pages and hand the container's mapping references (one per page) to the proof queue
// in `refs_per_page` copies each (the mapping reference, plus the owner's when released too). (satoru)
void unmap_defer(uint64_t as, uint64_t va, const uint64_t* frames, uint64_t first_phys,
                 uint64_t pages, int refs_per_page) {
    for (uint64_t i = 0; i < pages; i++) unmap_page_ledger(as, va + i * PG);
    if (!g_noflush && SMP::OnlineCount() > 1) SMP::BroadcastTlbFlush();
    // consecutive frames go in as one run (the broker queue has few slots) (satoru)
    uint64_t i = 0;
    while (i < pages) {
        uint64_t f = frames ? frames[i] : first_phys + i * PG;
        uint64_t n = 1;
        while (i + n < pages && (frames ? frames[i + n] : first_phys + (i + n) * PG) == f + n * PG) n++;
        dq_push_run(f, n, (uint32_t)refs_per_page);
        i += n;
    }
    if (g_noflush) {
        g_nf_stamp = SMP::TlbFlushStartSeq(); g_nf_pages = (uint32_t)pages;
        if (g_dq_nflog < 6) {
            g_dq_nflog++;
            SerialLogger::Log("[kld-dri] noflush: "); SerialLogger::LogDec((int)pages);
            SerialLogger::Log(" pages unmapped without a broadcast; stamp "); SerialLogger::LogDec((int)g_nf_stamp);
            SerialLogger::Log(", full shootdown seq "); SerialLogger::LogDec((int)SMP::TlbFlushFullSeq());
            SerialLogger::Log(" (not past the stamp: the frames stay in the broker proof queue), free frames "); SerialLogger::LogDec((int)PMM::GetFreeFrames());
            SerialLogger::Log("\r\n");
        }
    }
    KldBroker::DrainTlbProven(false);
}

// a mapping that was never visible to the container (a failed build): no proof needed. (satoru)
void unmap_unseen(uint64_t as, uint64_t va, uint64_t pages) {
    for (uint64_t i = 0; i < pages; i++) unmap_page_ledger(as, va + i * PG);
}

bool va_free(uint64_t as, uint64_t va, uint64_t pages) {
    if (va & (PG - 1)) return false;
    if (pages == 0 || pages > (1ull << 20)) return false;
    if (va < 0x10000ull) return false;
    uint64_t end = va + pages * PG;
    if (end < va || end > USER_SPACE_TOP + 1) return false;
    for (uint64_t i = 0; i < pages; i++)
        if (KernelVMM::QueryMappingInAddressSpace(as, va + i * PG)) return false;
    return true;
}

// mark every live request failed (container is gone). caller holds g_lock;
// slots to wake are returned in wake[]. (satoru)
int fail_all_locked(uint32_t* wake) {
    int n = 0;
    for (uint32_t i = 0; i < LXDRI_SLOTS; i++) {
        Req& r = g_req[i];
        if (!r.used || r.st == 2) continue;
        r.rep.result = -5;       // EIO (satoru)
        r.rep.nseg = 0;
        r.st = 2;
        g_req_eio++;
        if (r.abandoned) { r.used = false; continue; }
        wake[n++] = i;
    }
    return n;
}

void flush_closes_locked() {
    if (!g_ring) return;
    for (uint32_t i = 0; i < LXDRI_FILES; i++) {
        File& f = g_file[i];
        if (!f.used || !f.close_pending) continue;
        lxdri_hdr* h = H();
        uint32_t prod = h->closeq_prod;
        uint32_t cons = __atomic_load_n(&h->closeq_cons, __ATOMIC_ACQUIRE);
        if (prod - cons >= LXDRI_CLOSEQ) return;
        h->closeq[prod % LXDRI_CLOSEQ] = (f.gen << 8) | i;
        f.pos = prod;
        __atomic_store_n(&h->closeq_prod, prod + 1, __ATOMIC_RELEASE);
        f.close_pending = false;
        f.closing = true;
    }
}

void reap_closed_locked() {
    if (!g_ring) return;
    uint32_t cons = __atomic_load_n(&H()->closeq_cons, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < LXDRI_FILES; i++) {
        File& f = g_file[i];
        if (f.used && f.closing && (int32_t)(cons - f.pos) > 0) { f.used = false; f.closing = false; }
    }
}

void drop_ring_refs(bool unmap, uint64_t as) {
    // the allocation reference is dropped frame by frame; the container's mapping
    // reference goes with its address space (or with the explicit unmap, which waits for
    // the shootdown proof). (satoru)
    if (unmap) { unmap_defer(as, g_ring_va, nullptr, g_ring_phys, LXDRI_RING_PAGES, 2); return; }
    for (uint32_t i = 0; i < LXDRI_RING_PAGES; i++)
        PMM::FreeFrame(g_ring_phys + (uint64_t)i * PG);
}

// drop every object. caller does NOT hold g_lock. (satoru)
void drop_objects(bool unmap, uint64_t as) {
    for (int i = 0; i < MAXOBJ; i++) {
        Obj o;
        {
            SpinLockGuard g(g_lock);
            if (!g_obj[i].used || !g_obj[i].id) continue;
            o = g_obj[i];
            g_obj[i].used = false;
            g_obj_pages -= o.pages;
        }
        if (unmap) unmap_defer(as, o.va, o.frames, 0, o.pages, 2);   // owner + mapping references, after the proof (satoru)
        else for (uint32_t k = 0; k < o.pages; k++) PMM::FreeFrame(o.frames[k]);   // owner reference; the dying address space drops the mapping one (satoru)
        KernelHeap::Free(o.frames);
    }
}

void detach_common(uint64_t as, bool explicit_unmap) {
    uint32_t wake[LXDRI_SLOTS]; int nw = 0;
    uint64_t rphys = 0;
    {
        SpinLockGuard g(g_lock);
        if (!g_attached || g_as != as) return;
        g_attached = false;
        nw = fail_all_locked(wake);
        rphys = g_ring_phys;
        g_ring = nullptr;
        g_minors = 0;
    }
    for (int i = 0; i < nw; i++) LinuxSyscallDriWake(wake[i]);
    drop_objects(explicit_unmap, as);
    (void)rphys;
    drop_ring_refs(explicit_unmap, as);
    DriDev::OnDetach();
    SerialLogger::Log("[kld-dri] container detached; open files are dead, free frames ");
    SerialLogger::LogDec((int)PMM::GetFreeFrames());
    SerialLogger::Log("\r\n");
}

}  // namespace

static void abandon_locked(uint32_t slot, uint64_t* wake_ack);
static void ring_doorbell_locked(uint64_t* wake);
static void open_orphan_locked(uint32_t file, int64_t result, uint64_t* wake);

void SetNoFlush(bool on) { g_noflush = on; }

bool Attached() { return __atomic_load_n(&g_attached, __ATOMIC_ACQUIRE); }

// ── files ──────────────────────────────────────────────────────────────────
int FileAlloc(uint32_t minor) {
    SpinLockGuard g(g_lock);
    if (!g_attached) return -19;
    uint32_t bit = (minor == 0) ? 1u : (minor == 128 ? 2u : 0u);
    if (!(g_minors & bit)) return -19;
    reap_closed_locked();
    flush_closes_locked();
    for (uint32_t i = 0; i < LXDRI_FILES; i++) {
        File& f = g_file[i];
        if (f.used) continue;
        f.used = true; f.closing = false; f.close_pending = false; f.close_on_reply = false;
        f.gen = (f.gen + 1) & 0xFFFFFFu;
        if (f.gen == 0) f.gen = 1;
        f.cgen = g_cgen;
        f.minor = minor;
        return (int)((f.gen << 8) | i);
    }
    return -24;
}

void FileFree(uint32_t id) {
    SpinLockGuard g(g_lock);
    File& f = g_file[id & 0xFF];
    if (f.used && f.gen == (id >> 8) && !f.closing) { f.used = false; f.close_pending = false; }
}

void FileClose(uint32_t id) {
    uint64_t wk = 0;
    uint64_t wacks[LXDRI_SLOTS]; uint32_t wslots[LXDRI_SLOTS]; int nw = 0;
    {
        SpinLockGuard g(g_lock);
        File& f = g_file[id & 0xFF];
        if (!f.used || f.gen != (id >> 8) || f.closing || f.close_pending) return;
        // a thread of this process still parked on this file must not resume into a closed one (satoru)
        for (uint32_t i = 0; i < LXDRI_SLOTS; i++) {
            if (g_req[i].used && g_req[i].file == id && g_req[i].task) {
                uint64_t w = 0;
                abandon_locked(i, &w);
                wacks[nw] = w; wslots[nw] = i; nw++;
            }
        }
        if (!g_attached || f.cgen != g_cgen) f.used = false;   // dead file: nothing to release (satoru)
        else {
            f.close_pending = true;
            flush_closes_locked();
            if (f.closing) ring_doorbell_locked(&wk);   // wake a worker so the release does not wait (satoru)
        }
    }
    for (int i = 0; i < nw; i++) {
        if (wacks[i]) LinuxSyscallFutexWakePhys(wacks[i], 8);
        LinuxSyscallDriWake(wslots[i]);
    }
    if (wk) LinuxSyscallFutexWakePhys(wk, 8);
}

bool FileAlive(uint32_t id) {
    SpinLockGuard g(g_lock);
    File& f = g_file[id & 0xFF];
    return g_attached && f.used && f.gen == (id >> 8) && !f.closing && !f.close_pending && f.cgen == g_cgen;
}

uint32_t FileEvBytes(uint32_t id) {
    SpinLockGuard g(g_lock);
    File& f = g_file[id & 0xFF];
    if (!g_attached || !g_ring || !f.used || f.gen != (id >> 8)) return 0;
    return F(id & 0xFF)->ev_bytes;
}

uint32_t FileMinor(uint32_t id) {
    SpinLockGuard g(g_lock);
    return g_file[id & 0xFF].minor;
}

bool MinorAvailable(uint32_t minor) {
    SpinLockGuard g(g_lock);
    uint32_t bit = (minor == 0) ? 1u : (minor == 128 ? 2u : 0u);
    return g_attached && (g_minors & bit);
}

// ── requests (caller side) ─────────────────────────────────────────────────
int FindByTask(Process* task) {
    SpinLockGuard g(g_lock);
    for (uint32_t i = 0; i < LXDRI_SLOTS; i++)
        if (g_req[i].used && g_req[i].task == task) return (int)i;
    return -1;
}

// the pending request of this task, only if it is the very syscall that resumes now (same kind, file,
// cmd, argument, offset and length); a different one means the old call was interrupted (a signal
// handler issued its own) and is abandoned, so a reply never reaches the wrong syscall. -1: none. (satoru)
int FindBound(Process* task, uint32_t kind, uint32_t file, uint32_t cmd, uint64_t arg, uint64_t mm_off, uint64_t mm_len) {
    uint64_t wk = 0;
    int found = -1;
    {
        SpinLockGuard g(g_lock);
        for (uint32_t i = 0; i < LXDRI_SLOTS; i++) {
            Req& r = g_req[i];
            if (!r.used || r.task != task) continue;
            if (r.kind == kind && (file == ~0u || r.file == file) && r.cmd == cmd && r.arg == arg && r.mm_off == mm_off && r.mm_len == mm_len)
                found = (int)i;
            else
                abandon_locked(i, &wk);
            break;
        }
    }
    if (wk) LinuxSyscallFutexWakePhys(wk, 8);
    return found;
}

uint32_t ReqFile(int slot) {
    SpinLockGuard g(g_lock);
    return (slot >= 0 && slot < (int)LXDRI_SLOTS && g_req[slot].used) ? g_req[slot].file : 0;
}

int Submit(Process* task, uint64_t as, uint32_t kind, uint32_t file, uint32_t cmd,
           uint64_t arg, uint32_t pid, uint32_t tgid, uint32_t euid, uint32_t egid,
           uint32_t flags, uint64_t mm_off, uint64_t mm_len, uint32_t mm_prot, uint32_t mm_flags) {
    dq_drain();
    uint64_t wk;
    int idx = -1;
    {
        SpinLockGuard g(g_lock);
        if (!g_attached || !g_ring) return -19;
        for (uint32_t i = 0; i < LXDRI_SLOTS; i++) {
            if (g_req[i].used) { if (g_req[i].task == task) return -16; continue; }
            if (idx < 0) idx = (int)i;
        }
        if (idx < 0) return -11;   // all 32 slots busy: caller retries (satoru)
        Req& r = g_req[idx];
        uint32_t gen = r.gen + 1;
        memset(&r, 0, sizeof(r));
        r.used = true; r.gen = gen; r.task = task; r.as = as; r.kind = kind; r.file = file;
        r.cmd = cmd; r.arg = arg; r.mm_off = mm_off; r.mm_len = mm_len;
        lxdri_slot* s = S((uint32_t)idx);
        s->kind = kind; s->file_id = file; s->cmd = cmd; s->arg = arg;
        s->pid = pid; s->tgid = tgid; s->euid = euid; s->egid = egid;
        s->req_id = ((uint64_t)gen << 8) | (uint32_t)idx;
        s->mm_offset = mm_off; s->mm_len = mm_len; s->mm_prot = mm_prot; s->mm_flags = mm_flags;
        s->open_flags = flags; s->open_minor = (kind == LXDRI_K_OPEN) ? cmd : 0;
        s->ua_ptr = 0; s->ua_len = 0; s->ua_dir = 0; s->ua_res = 0;
        s->result = 0; s->nseg = 0; s->reply_flags = 0;
        __atomic_store_n(&s->state, LXDRI_S_QUEUED, __ATOMIC_RELEASE);
        __atomic_fetch_add(&H()->doorbell, 1u, __ATOMIC_RELEASE);
        wk = g_ring_phys + __builtin_offsetof(lxdri_hdr, doorbell);
    }
    LinuxSyscallFutexWakePhys(wk, 1);
    return idx;
}

bool ReqReady(uint32_t slot) {
    if (slot >= LXDRI_SLOTS) return true;
    SpinLockGuard g(g_lock);
    return !g_req[slot].used || g_req[slot].st != 0;
}

int Poll(int slot, UaReq* ua, Reply* rep) {
    if (slot < 0 || slot >= (int)LXDRI_SLOTS) return POLL_DONE;
    SpinLockGuard g(g_lock);
    Req& r = g_req[slot];
    if (!r.used) { rep->result = -5; rep->nseg = 0; rep->file = 0; return POLL_DONE; }
    if (r.st == 2) { *rep = r.rep; rep->file = r.file; return POLL_DONE; }
    if (r.st == 1 && !r.abandoned && g_ring) {
        ua->ptr = r.ua_ptr; ua->len = r.ua_len; ua->dir = r.ua_dir;
        ua->bounce_phys = g_ring_phys + LXDRI_BOUNCE_OFF((uint32_t)slot);
        for (uint32_t k = 0; k < LXDRI_BOUNCE / PG; k++) PMM::RetainFrame(ua->bounce_phys + (uint64_t)k * PG);
        r.ua_inflight = true;
        return POLL_UACC;
    }
    return POLL_WAIT;
}

void UaFinish(int slot, const UaReq* ua, int64_t not_copied) {
    uint64_t wk = 0;
    {
        SpinLockGuard g(g_lock);
        Req& r = g_req[slot];
        if (r.used && r.st == 1 && g_ring) {
            lxdri_slot* s = S((uint32_t)slot);
            s->ua_res = not_copied;
            __atomic_fetch_add(&s->ua_ack, 1u, __ATOMIC_RELEASE);
            wk = g_ring_phys + LXDRI_SLOT_OFF((uint32_t)slot) + __builtin_offsetof(lxdri_slot, ua_ack);
            r.st = 0;
            r.ua_inflight = false;
            if (not_copied) g_ua_fail++;
        }
    }
    if (wk) LinuxSyscallFutexWakePhys(wk, 8);
    dq_push_run(ua->bounce_phys, LXDRI_BOUNCE / PG, 1);   // may be the last reference of a detached ring (satoru)
}

// the caller leaves (SIGKILL, thread exit). a request still queued is cancelled; one a
// worker already has is abandoned: its uaccess fails, its reply is discarded and the slot
// is recycled by the reply or by container death. (satoru)
static void ring_doorbell_locked(uint64_t* wake) {
    if (!g_ring) return;
    __atomic_fetch_add(&H()->doorbell, 1u, __ATOMIC_RELEASE);
    *wake = g_ring_phys + __builtin_offsetof(lxdri_hdr, doorbell);
}

// the container's drm_file of an open that nobody will use: close it, but only once the open has
// really finished there (result 0); a failed open left nothing behind. caller holds g_lock. (satoru)
static void open_orphan_locked(uint32_t file, int64_t result, uint64_t* wake) {
    File& f = g_file[file & 0xFF];
    if (!f.used || f.gen != (file >> 8) || f.closing || f.close_pending) return;
    if (result != 0 || !g_attached || f.cgen != g_cgen) { f.used = false; return; }
    f.close_pending = true;
    flush_closes_locked();
    ring_doorbell_locked(wake);
}

// the caller leaves (SIGKILL, thread exit, a mismatched or stale resume). a request still queued is
// cancelled; one a worker already has is abandoned: its uaccess fails, its reply is discarded and the
// slot is recycled by the reply or by container death. (satoru)
static void abandon_locked(uint32_t slot, uint64_t* wake_ack) {
    Req& r = g_req[slot];
    if (!r.used) return;
    if (r.st == 2) {
        if (r.kind == LXDRI_K_OPEN) open_orphan_locked(r.file, r.rep.result, wake_ack);
        r.used = false;
        r.task = nullptr;
        if (g_ring) __atomic_store_n(&S(slot)->state, LXDRI_S_FREE, __ATOMIC_RELEASE);
        return;
    }
    if (g_ring) {
        lxdri_slot* s = S(slot);
        unsigned int q = LXDRI_S_QUEUED;
        if (r.st == 0 && __atomic_compare_exchange_n(&s->state, &q, LXDRI_S_FREE, false,
                                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            r.used = false;        // no worker ever saw it (satoru)
            r.task = nullptr;
            if (r.kind == LXDRI_K_OPEN) { File& f = g_file[r.file & 0xFF]; if (f.used && f.gen == (r.file >> 8)) f.used = false; }
            return;
        }
        if (r.st == 1 && !r.ua_inflight) {
            s->ua_res = s->ua_len;
            __atomic_fetch_add(&s->ua_ack, 1u, __ATOMIC_RELEASE);
            *wake_ack = g_ring_phys + LXDRI_SLOT_OFF(slot) + __builtin_offsetof(lxdri_slot, ua_ack);
            r.st = 0;
        }
    }
    r.abandoned = true;
    r.task = nullptr;
    // a served open: its file is closed when the reply arrives (op_reply), strictly after the open (satoru)
    if (r.kind == LXDRI_K_OPEN) { File& f = g_file[r.file & 0xFF]; if (f.used && f.gen == (r.file >> 8)) f.close_on_reply = true; }
}

void Abandon(int slot) {
    if (slot < 0 || slot >= (int)LXDRI_SLOTS) return;
    uint64_t wk = 0;
    {
        SpinLockGuard g(g_lock);
        abandon_locked((uint32_t)slot, &wk);
    }
    if (wk) LinuxSyscallFutexWakePhys(wk, 8);
}

void Release(int slot) {
    if (slot < 0 || slot >= (int)LXDRI_SLOTS) return;
    SpinLockGuard g(g_lock);
    Req& r = g_req[slot];
    if (!r.used) return;
    r.used = false;
    r.task = nullptr;
    if (g_ring) __atomic_store_n(&S((uint32_t)slot)->state, LXDRI_S_FREE, __ATOMIC_RELEASE);
}

void TaskGone(Process* task) {
    uint64_t wk[LXDRI_SLOTS]; int n = 0;
    {
        SpinLockGuard g(g_lock);
        for (uint32_t i = 0; i < LXDRI_SLOTS; i++) {
            if (!g_req[i].used || g_req[i].task != task) continue;
            uint64_t w = 0;
            abandon_locked(i, &w);
            if (w) wk[n++] = w;
        }
    }
    for (int i = 0; i < n; i++) LinuxSyscallFutexWakePhys(wk[i], 8);
}

// ── broker ops (container side) ─────────────────────────────────────────────
static int64_t op_attach(Process* cur, uint64_t va, uint64_t pages, uint64_t minors) {
    uint64_t as = cur->address_space;
    if (!DriDev::Enabled()) return KldBroker::KLD_EPERM;
    // only a display family (the manifest "display 1" flag the display bridge attach requires) may
    // be the drm server; one server at a time (the g_attached test below). (satoru)
    {
        const char* fam = KldBroker::FamilyOfAddressSpace(as);
        if (!fam[0] || !(KldAuto::FamilyFlags(fam) & KldAuto::FAMILY_DISPLAY)) return KldBroker::KLD_EPERM;
    }
    if (pages != LXDRI_RING_PAGES || (minors & ~3ull) || !minors) return KldBroker::KLD_EINVAL;
    if (!va_free(as, va, pages)) return KldBroker::KLD_EBUSY;
    {
        SpinLockGuard g(g_lock);
        if (g_attached) return KldBroker::KLD_EBUSY;
    }
    uint64_t phys = PMM::AllocContiguous(pages);
    if (!phys) return KldBroker::KLD_ENOMEM;
    if (phys + pages * PG > (16ull << 30)) { PMM::FreeContiguous(phys, pages); return KldBroker::KLD_ENOMEM; }
    memset(PMM::PhysToVirt(phys), 0, pages * PG);
    for (uint64_t i = 0; i < pages; i++) PMM::RetainFrame(phys + i * PG);   // mapping reference (satoru)
    uint64_t fl = PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX;
    for (uint64_t i = 0; i < pages; i++) {
        if (!KernelVMM::MapPageInAddressSpace(as, va + i * PG, phys + i * PG, fl)) {
            unmap_unseen(as, va, i);      // never visible to the container: no proof needed (satoru)
            for (uint64_t j = 0; j < pages; j++) PMM::FreeFrame(phys + j * PG);
            for (uint64_t j = 0; j < pages; j++) PMM::FreeFrame(phys + j * PG);
            return KldBroker::KLD_ENOMEM;
        }
    }
    bool lost = false;
    {
        SpinLockGuard g(g_lock);
        if (g_attached) {
            lost = true;     // another attach won the race (satoru)
        } else {
            lxdri_hdr* h = (lxdri_hdr*)PMM::PhysToVirt(phys);
            h->magic = LXDRI_MAGIC; h->version = LXDRI_VERSION;
            h->nslots = LXDRI_SLOTS; h->nfiles = LXDRI_FILES;
            h->minors = (uint32_t)minors;
            h->test = DriDev::TestId();
            g_ring = (uint8_t*)h;
            g_ring_phys = phys; g_ring_va = va; g_as = as;
            g_minors = (uint32_t)minors;
            g_cgen++;
            g_attached = true;
            memset(g_req, 0, sizeof(g_req));
        }
    }
    if (lost) {
        unmap_unseen(as, va, pages);
        for (uint64_t j = 0; j < pages; j++) PMM::FreeFrame(phys + j * PG);
        for (uint64_t j = 0; j < pages; j++) PMM::FreeFrame(phys + j * PG);
        return KldBroker::KLD_EBUSY;
    }
    DriDev::OnAttach((uint32_t)minors);
    SerialLogger::Log("[kld-dri] container attached, minors=");
    SerialLogger::LogDec((int)minors);
    SerialLogger::Log("\r\n");
    return KldBroker::KLD_OK;
}

static int64_t op_uaccess(uint64_t as, uint64_t req, uint64_t ptr, uint64_t len, uint64_t dir) {
    uint32_t idx = (uint32_t)(req & 0xFF);
    uint32_t gen = (uint32_t)(req >> 8);
    if (idx >= LXDRI_SLOTS || len == 0 || len > LXDRI_BOUNCE) return -1;
    if (dir != LXDRI_UA_FROM_USER && dir != LXDRI_UA_TO_USER) return -1;
    {
        SpinLockGuard g(g_lock);
        if (!g_attached || g_as != as || !g_ring) return -1;
        Req& r = g_req[idx];
        g_ua_ops++;
        if (!r.used || r.gen != gen || r.abandoned || r.st != 0) { g_ua_fail++; return -1; }
        if (__atomic_load_n(&S(idx)->state, __ATOMIC_ACQUIRE) != LXDRI_S_SERVING) return -1;
        if (r.ua_total + len > UA_BUDGET) { g_ua_fail++; return -1; }
        if (ptr >= TASK_SIZE || ptr + len < ptr || ptr + len > TASK_SIZE) { g_ua_fail++; return -1; }
        r.ua_total += len;
        r.ua_ptr = ptr; r.ua_len = (uint32_t)len; r.ua_dir = (uint32_t)dir;
        r.st = 1;
    }
    LinuxSyscallDriWake(idx);
    return 0;
}

static int64_t op_reply(uint64_t as, uint64_t req, int64_t result) {
    uint32_t idx = (uint32_t)(req & 0xFF);
    uint32_t gen = (uint32_t)(req >> 8);
    if (idx >= LXDRI_SLOTS) return KldBroker::KLD_EINVAL;
    bool wake = false;
    uint64_t wkdb = 0;
    {
        SpinLockGuard g(g_lock);
        if (!g_attached || g_as != as || !g_ring) return KldBroker::KLD_EPERM;
        Req& r = g_req[idx];
        if (!r.used || r.gen != gen || r.st == 2) return KldBroker::KLD_EINVAL;
        lxdri_slot* s = S(idx);
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != LXDRI_S_SERVING) return KldBroker::KLD_EINVAL;
        if (r.abandoned) {
            if (r.kind == LXDRI_K_OPEN) open_orphan_locked(r.file, result, &wkdb);
            r.used = false;
            __atomic_store_n(&s->state, LXDRI_S_FREE, __ATOMIC_RELEASE);
        } else {
            r.rep.result = result;
            uint32_t ns = s->nseg;
            if (ns > LXDRI_MAX_SEG) ns = 0;
            r.rep.nseg = ns;
            r.rep.flags = s->reply_flags;
            for (uint32_t i = 0; i < ns; i++) {
                r.rep.seg[i].obj = s->seg[i].obj; r.rep.seg[i].first = s->seg[i].first;
                r.rep.seg[i].npages = s->seg[i].npages; r.rep.seg[i].flags = s->seg[i].flags;
            }
            r.st = 2;
            g_req_ok++;
            wake = true;
        }
    }
    if (wkdb) LinuxSyscallFutexWakePhys(wkdb, 8);
    if (wake) LinuxSyscallDriWake(idx);
    return KldBroker::KLD_OK;
}

// a bounded wait in the container asks: has the caller of this request gone (killed, terminated but
// not yet reaped, abandoned)? if so the request is abandoned here, so the worker can fail its access
// and move on. (satoru)
static int64_t op_ua_check(uint64_t as, uint64_t req) {
    uint32_t idx = (uint32_t)(req & 0xFF);
    uint32_t gen = (uint32_t)(req >> 8);
    if (idx >= LXDRI_SLOTS) return KldBroker::KLD_EINVAL;
    uint64_t wk = 0;
    int64_t gone = 0;
    {
        SpinLockGuard g(g_lock);
        if (!g_attached || g_as != as || !g_ring) return KldBroker::KLD_EPERM;
        Req& r = g_req[idx];
        if (!r.used || r.gen != gen) return 1;
        if (r.abandoned) return 1;
        if (!r.task || r.task->state == Process_Terminated) {
            abandon_locked(idx, &wk);
            gone = 1;
        }
    }
    if (wk) LinuxSyscallFutexWakePhys(wk, 8);
    return gone;
}

static int64_t op_obj_alloc(Process* cur, uint64_t va, uint64_t pages) {
    uint64_t as = cur->address_space;
    if (pages == 0 || pages > 8192) return KldBroker::KLD_EINVAL;
    int slot = -1;
    {
        SpinLockGuard g(g_lock);
        if (!g_attached || g_as != as) return KldBroker::KLD_EPERM;
        if (g_obj_pages + pages > OBJ_PAGE_CAP) return KldBroker::KLD_ENOMEM;
        for (int i = 0; i < MAXOBJ; i++) if (!g_obj[i].used) { slot = i; break; }
        if (slot < 0) return KldBroker::KLD_ENOSPC;
        g_obj[slot].used = true;          // reserved, not yet visible to resolve (id 0) (satoru)
        g_obj[slot].id = 0;
        g_obj[slot].pages = (uint32_t)pages;
        g_obj[slot].frames = nullptr;
        g_obj_pages += pages;
    }
    uint64_t* frames = (uint64_t*)KernelHeap::Alloc(pages * 8);
    bool ok = frames && va_free(as, va, pages);
    uint64_t got = 0;
    if (ok) {
        uint64_t fl = PTE_PRESENT | PTE_WRITABLE | PTE_USER | PTE_NX;
        for (; got < pages; got++) {
            uint64_t f = PMM::AllocFrame();
            if (!f) { ok = false; break; }
            memset(PMM::PhysToVirt(f), 0, PG);
            PMM::RetainFrame(f);     // container mapping reference (satoru)
            frames[got] = f;
            if (!KernelVMM::MapPageInAddressSpace(as, va + got * PG, f, fl)) {
                PMM::FreeFrame(f); PMM::FreeFrame(f); ok = false; break;
            }
        }
    }
    if (!ok) {
        unmap_unseen(as, va, got);        // never visible to the container: no proof needed (satoru)
        for (uint64_t k = 0; k < got; k++) { PMM::FreeFrame(frames[k]); PMM::FreeFrame(frames[k]); }
        if (frames) KernelHeap::Free(frames);
        SpinLockGuard g(g_lock);
        g_obj[slot].used = false;
        g_obj_pages -= pages;
        return KldBroker::KLD_ENOMEM;
    }
    bool gone = false;
    uint32_t id = 0;
    {
        SpinLockGuard g(g_lock);
        if (!g_attached || g_as != as) {
            gone = true;
            g_obj[slot].used = false;
            g_obj_pages -= pages;
        } else {
            g_obj[slot].va = va;
            g_obj[slot].frames = frames;
            id = g_obj_next++;
            g_obj[slot].id = id;
        }
    }
    if (gone) {
        // the container went away while the object was built: its address space
        // teardown drops the mapping references, we drop the owner ones. (satoru)
        for (uint64_t k = 0; k < pages; k++) PMM::FreeFrame(frames[k]);
        KernelHeap::Free(frames);
        return KldBroker::KLD_EPERM;
    }
    return (int64_t)id;
}

static int64_t op_obj_free(uint64_t as, uint64_t id) {
    Obj o; bool found = false;
    {
        SpinLockGuard g(g_lock);
        if (!g_attached || g_as != as) return KldBroker::KLD_EPERM;
        for (int i = 0; i < MAXOBJ; i++) {
            if (g_obj[i].used && g_obj[i].id == id && id) {
                o = g_obj[i]; found = true;
                g_obj[i].used = false;
                g_obj_pages -= o.pages;
                break;
            }
        }
    }
    if (!found) return KldBroker::KLD_EINVAL;
    // the container's mapping reference and the owner reference are dropped once the shootdown is
    // proven; caller mappings keep their own references (satoru)
    unmap_defer(as, o.va, o.frames, 0, o.pages, 2);
    KernelHeap::Free(o.frames);
    return KldBroker::KLD_OK;
}

int64_t Call(Process* cur, uint32_t op, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint64_t as = cur->address_space;
    dq_drain();
    switch (op) {
        case LX_KLD_OP_DRI_UA_CHECK:  return op_ua_check(as, a0);
        case LX_KLD_OP_DRI_ATTACH:    return op_attach(cur, a0, a1, a2);
        case LX_KLD_OP_DRI_DETACH:    detach_common(as, true); return KldBroker::KLD_OK;
        case LX_KLD_OP_DRI_UACCESS:   return op_uaccess(as, a0, a1, a2, a3);
        case LX_KLD_OP_DRI_REPLY:     return op_reply(as, a0, (int64_t)a1);
        case LX_KLD_OP_DRI_OBJ_ALLOC: return op_obj_alloc(cur, a0, a1);
        case LX_KLD_OP_DRI_OBJ_FREE:  return op_obj_free(as, a0);
        case LX_KLD_OP_DRI_STATUS: {
            // rig only (kurono.kld.dri.test): the counters and the free frame count (satoru)
            if (!DriDev::TestId()) return KldBroker::KLD_EPERM;
            char b[200]; Status(b, sizeof b);
            SerialLogger::Log("[kld-dri] "); SerialLogger::Log(b);
            // free frames PLUS what the kernel heap arena holds: the heap grows in big steps (24 MiB seen) that
            // would otherwise read as a leak (satoru)
            return (int64_t)(PMM::GetFreeFrames() + KernelHeap::GetTotal() / 4096);
        }
        case 0xABu:       // rig only: the container ends itself, the way a crash would (satoru)
            if (!DriDev::TestId()) return KldBroker::KLD_EPERM;
            DriDev::RequestKill((uint32_t)cur->pid);
            return 0;
        case LX_KLD_OP_DRI_CHILD_SPAWN:
        case LX_KLD_OP_DRI_CHILD_KILL:
        case LX_KLD_OP_DRI_CHILD_STATE:
        case LX_KLD_OP_DRI_CHILD_FREE:
            if (!DriDev::TestId()) return KldBroker::KLD_EPERM;
            return DriDev::ChildOp(op, a0, a1);
        default: return KldBroker::KLD_EINVAL;
    }
}

void ContainerGone(uint64_t as) {
    if (!Attached()) return;
    detach_common(as, false);
}

void AsDestroyed(uint64_t as) {
    if (!DriDev::Enabled()) return;   // flag off: this runs on every address space destroy (satoru)
    if (Attached()) {
        bool is_cont;
        { SpinLockGuard g(g_lock); is_cont = g_attached && g_as == as; }
        if (is_cont) { detach_common(as, false); return; }
    }
    // a caller's address space: its requests are abandoned (uaccess fails, replies
    // discarded). sticky, and no unmap happens here. (satoru)
    uint64_t wk[LXDRI_SLOTS]; int n = 0;
    {
        SpinLockGuard g(g_lock);
        for (uint32_t i = 0; i < LXDRI_SLOTS; i++) {
            if (!g_req[i].used || g_req[i].as != as) continue;
            uint64_t w = 0;
            abandon_locked(i, &w);
            if (w) wk[n++] = w;
        }
    }
    for (int i = 0; i < n; i++) LinuxSyscallFutexWakePhys(wk[i], 8);
}

bool RangeIsContainerMapping(uint64_t as, uint64_t va, uint64_t len) {
    if (!g_attached) return false;
    SpinLockGuard g(g_lock);
    if (!g_attached || g_as != as) return false;
    uint64_t end = va + len;
    if (end < va) return true;   // wrapping range: refuse like the broker does (satoru)
    if (va < g_ring_va + LXDRI_RING_PAGES * PG && end > g_ring_va) return true;
    for (int i = 0; i < MAXOBJ; i++)
        if (g_obj[i].used && g_obj[i].id && va < g_obj[i].va + (uint64_t)g_obj[i].pages * PG && end > g_obj[i].va)
            return true;
    return false;
}

// ── object frames for caller mappings ───────────────────────────────────────
int ObjResolve(const Reply* rep, uint64_t max_pages, uint64_t* frames, uint64_t* n_out) {
    uint64_t n = 0;
    SpinLockGuard g(g_lock);
    if (!g_attached) return -19;
    for (uint32_t s = 0; s < rep->nseg; s++) {
        const auto& sg = rep->seg[s];
        Obj* o = nullptr;
        for (int i = 0; i < MAXOBJ; i++)
            if (g_obj[i].used && g_obj[i].id == sg.obj && sg.obj) { o = &g_obj[i]; break; }
        if (!o || sg.npages == 0 || (uint64_t)sg.first + sg.npages > o->pages || n + sg.npages > max_pages) {
            for (uint64_t k = 0; k < n; k++) PMM::FreeFrame(frames[k]);
            return -22;
        }
        for (uint32_t p = 0; p < sg.npages; p++) {
            uint64_t f = o->frames[sg.first + p];
            if (PMM::GetFrameRefCount(f) >= 0xFF00) {      // RetainFrame saturates silently: refuse instead (satoru)
                for (uint64_t k = 0; k < n; k++) PMM::FreeFrame(frames[k]);
                return -12;
            }
            PMM::RetainFrame(f);          // the caller's mapping reference (satoru)
            frames[n++] = f;
        }
    }
    *n_out = n;
    return 0;
}

void FramesRelease(const uint64_t* frames, uint64_t n) {
    for (uint64_t k = 0; k < n; k++) PMM::FreeFrame(frames[k]);
}

int Status(char* out, int mx) {
    SpinLockGuard g(g_lock);
    int used = 0;
    for (uint32_t i = 0; i < LXDRI_SLOTS; i++) if (g_req[i].used) used++;
    int objs = 0, files = 0;
    for (int i = 0; i < MAXOBJ; i++) if (g_obj[i].used) objs++;
    for (uint32_t i = 0; i < LXDRI_FILES; i++) if (g_file[i].used) files++;
    int p = 0;
    auto cat = [&](const char* s) { while (*s && p < mx - 1) out[p++] = *s++; out[p] = 0; };
    auto num = [&](uint64_t v) { char t[24]; int k = 0; if (!v) t[k++] = '0'; while (v) { t[k++] = (char)('0' + v % 10); v /= 10; } while (k) { char c[2] = { t[--k], 0 }; cat(c); } };
    cat("kld-dri: attached="); num(g_attached); cat(" reqs="); num(used); cat(" files="); num(files);
    cat(" objs="); num(objs); cat(" ok="); num(g_req_ok); cat(" eio="); num(g_req_eio);
    cat(" ua="); num(g_ua_ops); cat(" uafail="); num(g_ua_fail);
    cat(" dqpages="); num(g_dq_pages);
    { Cgroup::Node* cg = Cgroup::Get(1); cat(" cg_mem="); num(cg ? cg->mem_current : 0); }
    if (DriDev::TestCg()) { Cgroup::Node* t = Cgroup::Get(DriDev::TestCg()); cat(" tcg_mem="); num(t ? t->mem_current : 0); cat(" tcg_max="); num(t ? t->mem_max : 0); cat(" tcg_oom="); num(t ? t->mem_oom_count : 0); }
    cat(" heap_kb="); num(KernelHeap::GetUsed() / 1024); cat(" heap_total_kb="); num(KernelHeap::GetTotal() / 1024);
    cat("\n");
    return p;
}

}  // namespace KldDri

// end (satoru)
