//  kurono os - linux syscall abi translation layer - implementation
//  intercepts int 0x80 and translates linux syscalls to kurono operations

#include "linux_syscall.h"
#include "ext4.h"
#include "kls.h"
#include "ld_kurono.h"          // ExecPIE for x86_64 dynamic execve (firefox content procs) (satoru)
#include "../fs/kvfs.h"
#include "../hal/hal.h"
#include "../kernel/heap.h"
#include "../kernel/userspace.h"
#include "../kernel/vmm.h"
#include "../kernel/time.h"
#include "../drivers/timer.h"
#include "../drivers/serial.h"
#include "../security/supr.h"
#include "../net/unix_socket.h"
#include "linux_netbridge.h"      // af_inet tcp/udp bridge over the kurono stack (satoru)
#include "../shell/shell.h"
#include "../system/logging.h"
#include "../proc/smp.h"          // per-cpu syscall-entry scratch (smp phase 3d) (satoru)
#include "../proc/spinlock.h"     // futex-queue + kls locks (smp thread dispatch) (satoru)

LinuxProcess LinuxSyscall::procs[LINUX_MAX_PROCS];
int LinuxSyscall::current_proc_cpu[SMP_MAX_CPUS] = {};
// keep the ~8 internal uses of `current_proc` textually unchanged; each resolves
// to the calling cpu's slot. Init() seeds every slot to -1. (smp 3d) (satoru)
#define current_proc (current_proc_cpu[SMP::CpuIndex()])

// console output capture ring buffer
char LinuxSyscall::console_buf[CONSOLE_BUF_SIZE];
int  LinuxSyscall::console_head = 0;
int  LinuxSyscall::console_tail = 0;

// stdin injection ring buffer
char LinuxSyscall::stdin_buf[STDIN_BUF_SIZE];
int  LinuxSyscall::stdin_head = 0;
int  LinuxSyscall::stdin_tail = 0;

namespace {
constexpr uint64_t EXEC_STACK_BYTES = 16 * 1024;
constexpr int EXEC_MAX_ARGC = 16;
constexpr uint64_t USER_MMAP_BASE = 0x20000000ULL;
// the mmap arena still grows up from USER_MMAP_BASE, but the ceiling is
// the canonical user-space top rather than the old sub-4gb cap. a pie
// binary placed above 4gb by ld-kurono can now request (or be handed)
// high mmap regions, and anonymous maps may run past 4gb - their
// pointers round-trip through the now-64-bit syscall abi intact (satoru)
constexpr uint64_t USER_MMAP_LIMIT = USER_SPACE_TOP;

constexpr uint32_t PFERR_PRESENT = 1U << 0;
constexpr uint32_t PFERR_WRITE   = 1U << 1;
constexpr uint32_t PFERR_USER    = 1U << 2;
constexpr uint32_t PFERR_FETCH   = 1U << 4;

constexpr uint32_t LINUX_PROT_WRITE = 0x2;
constexpr uint32_t LINUX_PROT_EXEC  = 0x4;

// madvise advice values we act on (linux x86 abi). MADV_DONTNEED discards the pages
// now; MADV_FREE is lazy on linux but kurono has no swap so we treat it the same. (satoru)
constexpr uint32_t LINUX_MADV_DONTNEED = 4;
constexpr uint32_t LINUX_MADV_FREE     = 8;

// per-cpu syscall-entry scratch (smp phase 3d): each cpu has its own in-flight
// frame pointer + rewrite/resume flags, so two cores can be inside int 0x80 /
// SYSCALL at the same time without clobbering each other's state. the macros
// keep the existing ~50 use sites textually unchanged; the cpu doesn't migrate
// mid-syscall, so re-reading CpuIndex() per access is stable. on the bsp this is
// slot 0 - identical to the old single-frame behaviour. (satoru)
static InterruptFrame* g_cur_syscall_frame[SMP_MAX_CPUS] = {};
static bool g_cur_frame_rewritten[SMP_MAX_CPUS]  = {};
static bool g_resume_us_session[SMP_MAX_CPUS]    = {};
static int  g_resume_us_exit[SMP_MAX_CPUS]       = {};
#define current_syscall_frame      g_cur_syscall_frame[SMP::CpuIndex()]
#define current_frame_rewritten    g_cur_frame_rewritten[SMP::CpuIndex()]
#define resume_userspace_session   g_resume_us_session[SMP::CpuIndex()]
#define resume_userspace_exit_code g_resume_us_exit[SMP::CpuIndex()]

// ── the kls lock (smp thread dispatch) ──────────────────────────────────────
// one recursive per-cpu-owner lock serializing the linux syscall layer across
// cores: with kurono.apthreads the aps run sibling threads of one process in
// parallel, and this layer's shared state (fd tables, sockets, epoll/eventfd
// slots, the mmap regions + page tables, kvfs) was written single-core. ring-3
// stays fully parallel - which is what breaks firefox's parking_lot cycle - 
// while ring-0 syscall bodies serialize. recursion (owner re-lock) happens when
// a syscall's kernel-mode touch of lazy user memory page-faults and the fault
// handler re-enters this layer. spins with IF untouched so the tlb-shootdown
// ipi and timer keep being serviceable where the caller had IF=1. on a single
// active core the lock is always uncontended. (satoru)
// single-word lock state: 0 = free, else owner cpu index + 1. ownership and
// the lock bit MUST be one atomic word: the old separate g_kls_owner had a
// two-store unlock (owner=-1, then word=0) and a captured stall snapshot
// showed the fallout - the unlocker preempted between the two stores leaves
// word=1/owner=-1, which matches NO cpu, so all four cores spun in kls_lock
// forever (the #PF-path spinners with IF=0) while the descheduled unlocker
// could never be run again = the whole-box bringup wedge. with owner packed
// into the word there is no torn window: the lock is either free or visibly
// owned, and the recursion test is a single load. (satoru)
static volatile uint32_t g_kls_word  = 0;
static int               g_kls_depth[SMP_MAX_CPUS] = {};

static void kls_lock() {
    uint32_t cpu = SMP::CpuIndex();
    if (cpu >= SMP_MAX_CPUS) cpu = 0;
    uint32_t me = cpu + 1u;
    if (__atomic_load_n(&g_kls_word, __ATOMIC_RELAXED) == me) { g_kls_depth[cpu]++; return; }
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&g_kls_word, &expected, me, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        do { __asm__ __volatile__("pause" ::: "memory"); }
        while (__atomic_load_n(&g_kls_word, __ATOMIC_RELAXED) != 0);
    }
    g_kls_depth[cpu] = 1;
}

static void kls_unlock() {
    uint32_t cpu = SMP::CpuIndex();
    if (cpu >= SMP_MAX_CPUS) cpu = 0;
    uint32_t me = cpu + 1u;
    if (__atomic_load_n(&g_kls_word, __ATOMIC_RELAXED) != me) return;  // never taken (bsp boot paths) (satoru)
    if (--g_kls_depth[cpu] > 0) return;             // recursive hold (satoru)
    __atomic_store_n(&g_kls_word, 0u, __ATOMIC_RELEASE);
}

// fully release the kls lock, breathe, reacquire at the same depth - for the
// in-kernel wait loops (poll/epoll with no sibling to switch to): holding the
// lock across a spin-until-ready loop would starve every other core's syscalls
// (and on an ap there is no PumpUI/SleepMs to yield into). (satoru)
static void kls_relax() {
    uint32_t cpu = SMP::CpuIndex();
    if (cpu >= SMP_MAX_CPUS) cpu = 0;
    uint32_t me = cpu + 1u;
    if (__atomic_load_n(&g_kls_word, __ATOMIC_RELAXED) != me) {
        __asm__ __volatile__("pause" ::: "memory");
        return;
    }
    int depth = g_kls_depth[cpu];
    __atomic_store_n(&g_kls_word, 0u, __ATOMIC_RELEASE);
    for (int i = 0; i < 128; i++) __asm__ __volatile__("pause" ::: "memory");
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&g_kls_word, &expected, me, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
        do { __asm__ __volatile__("pause" ::: "memory"); }
        while (__atomic_load_n(&g_kls_word, __ATOMIC_RELAXED) != 0);
    }
    g_kls_depth[cpu] = depth;
}

// futex wait-queue lock: the one kls structure ALSO touched from irq context
// (kls_timer_preempt's sweep on the bsp), so it gets its own short lock rather
// than the kls lock. always leaf - never take another lock under it. (satoru)
static Spinlock g_futex_lock;

// (satoru) serialize user sys_mmap across cores. threads of one process share the
// cr3 but each LinuxProcess has its own next_mmap_base + regions[] bump state; under
// -smp, two threads calling mmap concurrently both run choose_mmap_base (see "no
// overlap") for the SAME vbase BEFORE either add_region()s it, then both map + reserve
// it -> one map fails / the range is double-handed -> the frozen-memfd read-only map
// (SharedStringMap) returns null -> MOZ_RELEASE_ASSERT(mapping.IsValid()) #PF at
// page-load. holding this lock across choose_mmap_base -> map -> add_region makes the
// reserve-and-map atomic. leaf lock, process context only. (satoru)
static Spinlock g_mmap_lock;

// (satoru) serialize the memfd/shm object table (g_linux_shm[64]) across cores. two
// firefox threads building SharedStringMaps concurrently both run memfd_create ->
// shm_alloc_slot (a non-atomic scan-and-claim of the `used` flag) and bump refcounts;
// under -smp they can claim the SAME slot or corrupt a refcount, so one map's builder
// writes its data into frames a sibling then reused -> the frozen read-only map reads
// mismatched content -> MOZ_RELEASE_ASSERT in SharedStringMap's ctor. this lock makes
// slot claim + sizing atomic. leaf lock, process context only. (satoru)
static Spinlock g_shm_lock;

static inline uint64_t align_down_u64(uint64_t value, uint64_t align) {
    return value & ~(align - 1);
}

static inline uint64_t align_up_u64(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

static int find_process_index_by_task(Process* task) {
    if (!task) return -1;

    for (int i = 0; i < LINUX_MAX_PROCS; i++) {
        LinuxProcess* proc = LinuxSyscall::GetProcess(i);
        if (proc && proc->task == task) return i;
    }
    return -1;
}

static uint8_t kvfs_open_flags(uint32_t flags) {
    uint8_t kvfs_flags = 0;
    if ((flags & 3) == L_O_RDONLY) kvfs_flags = 1;
    else if ((flags & 3) == L_O_WRONLY) kvfs_flags = 2;
    else if ((flags & 3) == L_O_RDWR) kvfs_flags = 3;
    if (flags & L_O_APPEND) kvfs_flags |= 4;
    return kvfs_flags;
}

static uint8_t ext4_open_flags(uint32_t flags) {
    if ((flags & 3) == L_O_RDONLY) return 1;
    if ((flags & 3) == L_O_WRONLY) return 2;
    return 3;
}

// ---- epoll / eventfd / timerfd backing tables --------------------------
// firefox's main loop is glib's epoll loop: it eventfd-wakes a worker thread,
// arms timerfds for timeouts, and epoll_waits on wayland/pulse/dbus unix
// sockets. these tables give those fds real state so epoll_wait/poll return
// actual readiness instead of an empty set (the old dead loop). (satoru)

// x86_64 epoll_event is packed: 4-byte events + 8-byte data = 12 bytes (satoru)
struct LinuxEpollEvent {
    uint32_t events;
    uint64_t data;   // epoll_data_t (u64 / ptr / fd) opaque cookie (satoru)
} __attribute__((packed));
static_assert(sizeof(LinuxEpollEvent) == 12, "epoll_event must be 12 bytes packed");

// epoll readiness bits we actually compute (satoru)
static const uint32_t L_EPOLLIN  = 0x001;
static const uint32_t L_EPOLLOUT = 0x004;
static const uint32_t L_EPOLLERR = 0x008;
static const uint32_t L_EPOLLHUP = 0x010;

static const int EVENTFD_MAX = 64;
struct EventfdState {
    bool     used;
    bool     semaphore;   // EFD_SEMAPHORE: read() decrements by 1 (satoru)
    uint64_t counter;     // readable when > 0 (satoru)
};
static EventfdState g_eventfd[EVENTFD_MAX];

static const int TIMERFD_MAX = 64;
struct TimerfdState {
    bool     used;
    bool     armed;
    uint64_t expiry_ms;     // absolute ms (Timer::GetRealMs timebase) (satoru)
    uint64_t interval_ms;   // 0 = one-shot (satoru)
    uint64_t expirations;   // accumulated, returned + cleared on read() (satoru)
};
static TimerfdState g_timerfd[TIMERFD_MAX];

static const int EPOLL_MAX        = 32;   // epoll instances (satoru)
static const int EPOLL_MAX_WATCH  = 64;   // fds watched per instance (satoru)
struct EpollWatch {
    bool     used;
    int      fd;        // the watched linux fd number (satoru)
    uint32_t events;    // interest mask from epoll_ctl (satoru)
    uint64_t data;      // user cookie echoed back on epoll_wait (satoru)
};
struct EpollState {
    bool       used;
    EpollWatch watch[EPOLL_MAX_WATCH];
};
static EpollState g_epoll[EPOLL_MAX];

static int eventfd_alloc() {
    for (int i = 0; i < EVENTFD_MAX; i++)
        if (!g_eventfd[i].used) {
            g_eventfd[i].used = true;
            g_eventfd[i].semaphore = false;
            g_eventfd[i].counter = 0;
            return i;
        }
    return -1;
}

static int timerfd_alloc() {
    for (int i = 0; i < TIMERFD_MAX; i++)
        if (!g_timerfd[i].used) {
            g_timerfd[i].used = true;
            g_timerfd[i].armed = false;
            g_timerfd[i].expiry_ms = 0;
            g_timerfd[i].interval_ms = 0;
            g_timerfd[i].expirations = 0;
            return i;
        }
    return -1;
}

static int epoll_alloc() {
    for (int i = 0; i < EPOLL_MAX; i++)
        if (!g_epoll[i].used) {
            g_epoll[i].used = true;
            for (int w = 0; w < EPOLL_MAX_WATCH; w++) g_epoll[i].watch[w].used = false;
            return i;
        }
    return -1;
}

// roll a timerfd forward: count how many intervals have elapsed since the
// stored expiry and re-arm if periodic. call before reading readiness. (satoru)
static void timerfd_tick(int slot) {
    if (slot < 0 || slot >= TIMERFD_MAX) return;
    TimerfdState* t = &g_timerfd[slot];
    if (!t->used || !t->armed) return;
    uint64_t now = (uint64_t)Timer::GetRealMs();
    if (now < t->expiry_ms) return;
    if (t->interval_ms == 0) {
        // one-shot: a single expiration, then disarm (satoru)
        t->expirations += 1;
        t->armed = false;
        return;
    }
    // periodic: account every interval boundary we've crossed (satoru)
    uint64_t elapsed = now - t->expiry_ms;
    uint64_t n = elapsed / t->interval_ms + 1;
    t->expirations += n;
    t->expiry_ms += n * t->interval_ms;
}

// compute the ready event mask for one linux fd against an interest mask.
// this is the single source of truth shared by epoll_wait and poll. (satoru)
// sockaddr_in helpers for the af_inet cases: parse a user sockaddr into
// host-order ip/port, and write one back for recvfrom/getsockname/peername.
// family 2 = AF_INET; anything else is the caller's problem. (satoru)
static bool parse_sin(const uint8_t* sa, uint32_t* ip_host, uint16_t* port_host) {
    if (!sa) return false;
    uint16_t fam = (uint16_t)(sa[0] | (sa[1] << 8));
    if (fam != 2) return false;
    if (port_host) *port_host = (uint16_t)((sa[2] << 8) | sa[3]);           // big-endian on the wire (satoru)
    if (ip_host)   *ip_host   = ((uint32_t)sa[4] << 24) | ((uint32_t)sa[5] << 16) |
                                ((uint32_t)sa[6] << 8)  |  (uint32_t)sa[7];
    return true;
}
static void write_sin(uint8_t* sa, uint32_t ip_host, uint16_t port_host) {
    if (!sa) return;
    sa[0] = 2; sa[1] = 0;                                // AF_INET (satoru)
    sa[2] = (uint8_t)(port_host >> 8); sa[3] = (uint8_t)port_host;
    sa[4] = (uint8_t)(ip_host >> 24);  sa[5] = (uint8_t)(ip_host >> 16);
    sa[6] = (uint8_t)(ip_host >> 8);   sa[7] = (uint8_t)ip_host;
    for (int z = 8; z < 16; z++) sa[z] = 0;
}

static uint32_t fd_readiness(LinuxProcess* p, int fd, uint32_t interest) {
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) {
        // closed/invalid fd: report error so the loop drops it (satoru)
        return L_EPOLLERR | (interest & (L_EPOLLERR | L_EPOLLHUP));
    }
    LinuxFd* lfd = &p->fds[fd];
    uint32_t ready = 0;
    switch (lfd->type) {
        case LFD_SOCKET:
            { // (satoru) TEMP sockchk probe: sample child-process socket polls so
              // we see which fd the spinning child waits on + its pending bytes.
              static uint64_t _dbg = 0;
              if (false && p->pid >= 100 && ((++_dbg & 8191) == 0)) {  // gated off (satoru)
                  SerialLogger::Log("[sockchk] pid="); SerialLogger::LogDec((int)p->pid);
                  SerialLogger::Log(" fd="); SerialLogger::LogDec(fd);
                  SerialLogger::Log(" sd="); SerialLogger::LogDec(lfd->backend_fd);
                  SerialLogger::Log(" pend="); SerialLogger::LogDec(UnixSocket::PendingBytes(lfd->backend_fd));
                  SerialLogger::Log(" conn="); SerialLogger::LogDec(UnixSocket::HasPendingConnection(lfd->backend_fd) ? 1 : 0);
                  SerialLogger::Log("\r\n");
              }
            }
            // readable when there are bytes OR (for a listen fd) a pending
            // connection -- the latter is what wakes firefox's WaylandProxy
            // accept loop so it forwards get_registry to wayland-0. (satoru)
            if (UnixSocket::PendingBytes(lfd->backend_fd) > 0 ||
                UnixSocket::HasPendingConnection(lfd->backend_fd)) ready |= L_EPOLLIN;
            ready |= L_EPOLLOUT;   // our unix sockets never block on write (satoru)
            break;
        case LFD_PIPE:
            // pipes are socketpair-backed: same PendingBytes path (satoru)
            if (UnixSocket::PendingBytes(lfd->backend_fd) > 0) ready |= L_EPOLLIN;
            ready |= L_EPOLLOUT;
            break;
        case LFD_INET:
            // af_inet tcp/udp: the bridge's lock-free readiness (LNET_POLL*
            // values match L_EPOLL*). this is what wakes firefox's non-blocking
            // connect (POLLOUT on established, ERR|HUP on refused). (satoru)
            ready |= LinuxNetBridge::Readiness(lfd->backend_fd);
            break;
        case LFD_EVENTFD: {
            int s = lfd->backend_fd;
            if (s >= 0 && s < EVENTFD_MAX && g_eventfd[s].counter > 0) ready |= L_EPOLLIN;
            ready |= L_EPOLLOUT;   // writable unless counter saturated (satoru)
            break;
        }
        case LFD_TIMERFD: {
            int s = lfd->backend_fd;
            timerfd_tick(s);
            if (s >= 0 && s < TIMERFD_MAX && g_timerfd[s].expirations > 0) ready |= L_EPOLLIN;
            break;
        }
        case LFD_CONSOLE:
            // stdin readable when injection buffer has data; stdout/stderr
            // are always writable. caller passes interest to disambiguate. (satoru)
            if (LinuxSyscall::StdinReadable()) ready |= L_EPOLLIN;
            ready |= L_EPOLLOUT;
            break;
        case LFD_KVFS:
        case LFD_EXT4:
        case LFD_PROC:
            // regular files are always ready for read and write (satoru)
            ready |= L_EPOLLIN | L_EPOLLOUT;
            break;
        default:
            // devnull / stubs: writable, never readable (satoru)
            ready |= L_EPOLLOUT;
            break;
    }
    // (satoru) TEMP [rdy] probe: for firefox pids, sample fds they want readable
    // but that aren't ready, tagged with the fd TYPE (4=pipe 7=socket 13=eventfd
    // 14=timerfd). reveals whether an IO thread is wedged on an eventfd/pipe that
    // should have been signaled (a fixable kernel readiness bug) vs the content
    // socket that legitimately has no data yet (firefox-internal). debug-only.
    {
        static uint64_t _rdy = 0;
        if (false && p->pid >= 100 && p->pid < 140 && (interest & L_EPOLLIN) &&
            (ready & L_EPOLLIN) == 0 && ((++_rdy & 2047) == 0)) {  // gated off (satoru)
            SerialLogger::Log("[rdy] pid="); SerialLogger::LogDec(p->pid);
            SerialLogger::Log(" fd="); SerialLogger::LogDec(fd);
            SerialLogger::Log(" type="); SerialLogger::LogDec((int)lfd->type);
            SerialLogger::Log(" sd="); SerialLogger::LogDec(lfd->backend_fd);
            SerialLogger::Log(" NOTREADY\r\n");
        }
    }
    // EPOLLHUP/EPOLLERR are always reported regardless of interest; otherwise
    // mask to what the caller asked for (satoru)
    return ready & (interest | L_EPOLLERR | L_EPOLLHUP);
}

// fwd decl: defined further down. poll's cooperative block uses it to hand the
// cpu to a sibling user thread. (satoru)
static bool switch_to_ready_user(InterruptFrame* frame);
static void futex_sweep_timeouts();   // release timed-out futex waiters (satoru)
static inline bool wake_blocked_to_ready(Process* t);   // atomic Blocked->Ready (satoru)

// deschedule a user thread that is about to block in poll/ppoll: rewind its saved
// user frame to RE-RUN the syscall on wake, mark it Blocked with a short
// sleep_ticks (the timer tick re-readies threads in the ready queue), and switch
// to a sibling user thread. returns true if it switched (poll's return value is
// then ignored via current_frame_rewritten, and the thread re-enters poll when
// rescheduled). false == no sibling runnable, so the caller blocks in-place.
// restart_nr is the x86_64 syscall number to re-issue (7 poll / 271 ppoll). this
// is what stops a blocking poller from starving its sibling threads: SleepMs only
// context-switches kernel procs, so an in-place user-thread wait would monopolize
// the cpu in ring-0 (timer preempt is ignored mid-syscall). (satoru)
static bool poll_try_deschedule(LinuxProcess* p, int restart_nr) {
    Process* task = p ? p->task : nullptr;
    if (!task || !current_syscall_frame) return false;
    task->user_frame.rip -= 2;                          // SYSCALL/int0x80 are both 2 bytes (satoru)
    task->user_frame.rax  = (uint64_t)(uint32_t)restart_nr;
    // single-owner: state + sleep_ticks under g_sched_lock. (satoru)
    { uint64_t sf; Scheduler::StateLock(&sf); task->state = Process_Blocked; task->sleep_ticks = 2; Scheduler::StateUnlock(sf); }
    if (!switch_to_ready_user(current_syscall_frame)) {
        uint64_t sf; Scheduler::StateLock(&sf);
        task->sleep_ticks     = 0;                      // no sibling: undo + block in-place (satoru)
        task->state           = Process_Running;
        Scheduler::StateUnlock(sf);
        task->user_frame.rip += 2;
        return false;
    }
    return true;
}

// shared poll/ppoll wait. scan readiness; if nothing is ready, hand the cpu to a
// sibling user thread and re-run this syscall on wake, until an fd is ready or the
// timeout elapses. timeout_ms < 0 == infinite, 0 == one non-blocking pass.
// honouring the timeout stops glib's main loop busy-spinning on ppoll; the
// deschedule stops a blocked thread from starving its siblings. the deadline is
// kept in the LinuxProcess so it survives the syscall re-runs. (satoru)
static int do_poll_wait(LinuxProcess* p, void* fdsp, uint64_t nfds,
                        int64_t timeout_ms, int restart_nr) {
    struct LinuxPollfd { int fd; int16_t events; int16_t revents; } __attribute__((packed));
    LinuxPollfd* fds = (LinuxPollfd*)fdsp;
    if (!p || !fds || nfds == 0 || nfds > 1024) { if (p) p->poll_blocking = false; return 0; }
    // [p8] one-shot: log the FIRST firefox poll that includes the launch wakeup
    // eventfd (fd 8), with the pid + full set size. finds the fd-8 pump even if it
    // polls a big set (which the size-limited [pw] probe skips). (satoru)
    if (p->pid >= 100 && p->pid < 140) {
        static bool _p8seen[140] = {false};
        for (uint64_t i = 0; i < nfds; i++) {
            if (fds[i].fd == 8 && !_p8seen[p->pid]) {
                _p8seen[p->pid] = true;
                SerialLogger::Log("[p8] pid="); SerialLogger::LogDec(p->pid);
                SerialLogger::Log(" polls fd8 nfds="); SerialLogger::LogDec((int)nfds);
                SerialLogger::Log(" ev="); SerialLogger::LogHex((uint32_t)(uint16_t)fds[i].events);
                SerialLogger::Log("\r\n");
                break;
            }
        }
    }
    for (;;) {
        int ready_total = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            int16_t want = fds[i].events;
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;   // negative fd ignored (satoru)
            uint32_t interest = (uint32_t)(uint16_t)want | L_EPOLLERR | L_EPOLLHUP;
            uint32_t r = fd_readiness(p, fds[i].fd, interest);
            if (r) { fds[i].revents = (int16_t)(uint16_t)r; ready_total++; }
        }
        if (ready_total > 0) { p->poll_blocking = false; return ready_total; }
        // [pw] probe: a firefox poll/ppoll found NOTHING ready -> dump its pollfd set
        // (fd/type/interest/readiness) so we can see the launcher/MessageLoop thread's
        // wakeup fd (a pipe/eventfd that should read ready after a cross-thread post).
        // sampled, small sets only. (satoru)
        {
            static uint64_t _pw = 0;
            if (false && p->pid >= 100 && p->pid < 140 && nfds <= 12 && ((++_pw & 1023) == 0)) {
                SerialLogger::Log("[pw] pid="); SerialLogger::LogDec(p->pid);
                for (uint64_t i = 0; i < nfds; i++) {
                    int wfd = fds[i].fd;
                    if (wfd < 0) continue;
                    int wt = (wfd < LINUX_MAX_FDS && p->fds[wfd].open) ? (int)p->fds[wfd].type : -1;
                    SerialLogger::Log(" f="); SerialLogger::LogDec(wfd);
                    SerialLogger::Log("t"); SerialLogger::LogDec(wt);
                    SerialLogger::Log("e"); SerialLogger::LogHex((uint32_t)(uint16_t)fds[i].events);
                    SerialLogger::Log("r"); SerialLogger::LogHex(fd_readiness(p, wfd, 0xFFFFFFFFu));
                }
                SerialLogger::Log("\r\n");
            }
        }
        if (timeout_ms == 0) { p->poll_blocking = false; return 0; }   // non-blocking pass (satoru)
        uint64_t now = Time::GetTicks();
        if (!p->poll_blocking) {                                       // first block: arm deadline (satoru)
            p->poll_blocking    = true;
            p->poll_deadline_ms = (timeout_ms < 0) ? 0xFFFFFFFFFFFFFFFFULL
                                                   : now + (uint64_t)timeout_ms;
        }
        if (now >= p->poll_deadline_ms) { p->poll_blocking = false; return 0; }  // timed out (satoru)
        // hand the cpu to a sibling user thread; this poll re-runs when we wake (satoru)
        if (poll_try_deschedule(p, restart_nr)) return 0;   // switched; return value ignored
        // no sibling to switch to: cooperative in-place wait, then re-scan. on the
        // bsp that means pumping the ui + yielding to kernel procs; on an ap there
        // are no kernel procs and no ui - release the kls lock so the other cores'
        // syscalls keep flowing, breathe, retake it. (satoru)
        if (SMP::CpuIndex() == 0) {
            // drain the nic between scans so a poll on an inet socket sees dns
            // replies / handshake completions faster than the NetworkProcess's
            // 10 ms cadence (no-op with no live inet socket). (satoru)
            LinuxNetBridge::PumpTick();
            KuronoShell::PumpUI();
            Scheduler::SleepMs(1);
        } else {
            kls_relax();
        }
    }
}

static void clone_file_descriptors(const LinuxProcess* parent, LinuxProcess* child) {
    for (int fd = 0; fd < LINUX_MAX_FDS; fd++) {
        child->fds[fd].open = false;
        if (!parent->fds[fd].open) continue;

        const LinuxFd* src = &parent->fds[fd];
        LinuxFd* dst = &child->fds[fd];
        memcpy(dst, src, sizeof(LinuxFd));

        // epoll/eventfd/timerfd/signalfd reference GLOBAL kernel slots (g_epoll/
        // g_eventfd/g_timerfd), not per-process state. a plain memcpy makes the
        // parent + child share one slot, and the child's close (or CLOEXEC at
        // execve, or its exit) would FREE the parent's slot -> the parent's
        // epoll_wait then returns EINVAL forever (firefox's libevent spun 295k times
        // on this right after a vforked glxtest exited). a vfork/fork child execve's
        // and doesn't use the inherited ones, so stub the backend handle: its close
        // becomes a no-op and the parent keeps its live slot. (satoru)
        if (src->type == LFD_EPOLL || src->type == LFD_EVENTFD ||
            src->type == LFD_TIMERFD || src->type == LFD_SIGNALFD) {
            dst->backend_fd = -1;
            continue;
        }

        if (src->type == LFD_SOCKET) {
            // parent + child now both reference the SAME global unix-socket sd
            // (backend_fd copied verbatim). count the inherited reference so the
            // parent's standard post-fork close of the child's socketpair end
            // doesn't sever its own still-live ipc end -- the root of the firefox
            // e10s busy-spin (child replies had nowhere to land). (satoru)
            UnixSocket::Retain(dst->backend_fd);
            continue;
        }

        if (src->type == LFD_INET) {
            // same aliasing story for af_inet: bump the bridge refcount so
            // either side's close doesn't tear down the shared stack socket.
            // (satoru)
            LinuxNetBridge::Retain(dst->backend_fd);
            continue;
        }

        if (src->type == LFD_KVFS) {
            int backend = KVFS::Open(src->path, kvfs_open_flags(src->flags));
            if (backend < 0) {
                dst->open = false;
                continue;
            }
            dst->backend_fd = backend;
            if (src->offset) {
                KVFS::Seek(backend, (int32_t)src->offset, 0);
            }
            continue;
        }

        if (src->type == LFD_EXT4) {
            int backend = Ext4::Open(src->path, ext4_open_flags(src->flags));
            if (backend < 0) {
                dst->open = false;
                continue;
            }
            dst->backend_fd = backend;
            if (src->offset) {
                Ext4::Seek(backend, (int32_t)src->offset, 0);
            }
        }
    }
}

static bool write_user_u32(Process* proc, uint64_t user_addr, uint32_t value) {
    if (!proc || !proc->is_user() || !user_addr) return false;
    if ((user_addr & 0xFFFULL) > PAGE_SIZE - sizeof(uint32_t)) return false;

    uint64_t phys = KernelVMM::QueryMappingInAddressSpace(proc->address_space, user_addr);
    if (!phys) return false;

    *(uint32_t*)(uintptr_t)phys = value;
    return true;
}

// read a 32-bit word from a user address via the task's own address space so
// the value is valid even if a different cr3 is active. used by futex to test
// *uaddr == val without trusting the currently-loaded address space. (satoru)
static bool read_user_u32(Process* proc, uint64_t user_addr, uint32_t* out) {
    if (!proc || !proc->is_user() || !user_addr || !out) return false;
    if ((user_addr & 0xFFFULL) > PAGE_SIZE - sizeof(uint32_t)) return false;

    uint64_t phys = KernelVMM::QueryMappingInAddressSpace(proc->address_space, user_addr);
    if (!phys) return false;

    *out = *(const uint32_t*)(uintptr_t)phys;
    return true;
}

// read a 64-bit user word as two page-checked u32 halves. (satoru)
static bool read_user_u64(Process* proc, uint64_t addr, uint64_t* out) {
    uint32_t lo, hi;
    if (!read_user_u32(proc, addr, &lo)) return false;
    if (!read_user_u32(proc, addr + 4, &hi)) return false;
    *out = ((uint64_t)hi << 32) | lo;
    return true;
}

// copy a NUL-terminated user string into kbuf (bounded). returns length, or -1
// if a page in the string isn't mapped. walks page-by-page via the process
// page tables (low phys is identity-mapped, so phys == kernel ptr). (satoru)
static int read_user_str(Process* proc, uint64_t addr, char* kbuf, int max) {
    if (!proc || !addr || !kbuf || max <= 0) return -1;
    uint64_t cur_page = ~0ULL, cur_phys = 0;
    int i = 0;
    for (; i < max - 1; i++) {
        uint64_t a  = addr + (uint64_t)i;
        uint64_t pg = a & ~0xFFFULL;
        if (pg != cur_page) {
            cur_phys = KernelVMM::QueryMappingInAddressSpace(proc->address_space, pg);
            if (!cur_phys) return -1;
            cur_page = pg;
        }
        char c = *(const char*)(uintptr_t)((cur_phys & ~0xFFFULL) + (a & 0xFFFULL));
        kbuf[i] = c;
        if (!c) return i;
    }
    kbuf[i] = 0;
    return i;
}

// copy a user argv/envp vector (NULL-terminated array of user char* pointers)
// into kernel storage; fills out[] with kernel string pointers (NULL-terminated)
// and returns the entry count. used by the x86_64 dynamic execve path. (satoru)
static int copy_user_strv(Process* proc, uint64_t arr_addr, char* storage,
                          int storage_sz, const char** out, int max_entries) {
    int n = 0, used = 0;
    if (arr_addr) {
        for (; n < max_entries; n++) {
            uint64_t p = 0;
            if (!read_user_u64(proc, arr_addr + (uint64_t)n * 8, &p)) break;
            if (!p) break;                       // NULL terminator (satoru)
            if (used >= storage_sz - 1) break;
            int len = read_user_str(proc, p, storage + used, storage_sz - used);
            if (len < 0) break;
            out[n] = storage + used;
            used += len + 1;
        }
    }
    out[n] = nullptr;
    return n;
}

// CLONE_VM threads share one address space but each Process carries its own user
// region table; the regions actually live in the thread-group leader (the
// non-thread ancestor that ran the mmaps). resolve to it so a worker thread's
// demand-zero fault (e.g. growing its own stack) finds the region the leader
// registered, instead of SIGSEGV'ing on an empty per-thread table. every region
// helper below funnels through this. (satoru)
static Process* region_owner(Process* p) {
    int guard = 0;
    while (p && p->is_thread() && p->parent && guard++ < 64) p = p->parent;
    return p;
}

// per-cpu one-entry cache of the last region find_region matched. demand-zero
// faults walk a freshly-mmap'd region page-by-page (a GC chunk = 512 consecutive
// faults ALL in the same region), and the linear scan over 4096 slots per fault
// made that O(regions) each - the chrome main pinned in the GC's chunk-init
// faults. checking the last hit first turns those 511 follow-on faults into O(1).
// keyed by owner so a stale entry from another process never matches, and
// re-validated (active + bounds) so a munmap/reuse can't return a dead slot.
// per-cpu so no locking. (satoru)
static Process*          g_fr_cache_owner[SMP_MAX_CPUS] = {};
static UserMemoryRegion* g_fr_cache_region[SMP_MAX_CPUS] = {};

static UserMemoryRegion* find_region(Process* proc, uint64_t addr) {
    proc = region_owner(proc);
    if (!proc) return nullptr;

    uint32_t cpu = SMP::CpuIndex();
    if (cpu < SMP_MAX_CPUS && g_fr_cache_owner[cpu] == proc) {
        UserMemoryRegion* c = g_fr_cache_region[cpu];
        if (c && c->active && addr >= c->start && addr < c->end) return c;
    }

    for (int i = 0; i < PROCESS_MAX_USER_REGIONS; i++) {
        UserMemoryRegion* region = &proc->regions[i];
        if (!region->active) continue;
        if (addr >= region->start && addr < region->end) {
            if (cpu < SMP_MAX_CPUS) { g_fr_cache_owner[cpu] = proc; g_fr_cache_region[cpu] = region; }
            return region;
        }
    }
    return nullptr;
}

static UserMemoryRegion* find_region_by_flag(Process* proc, uint32_t flag) {
    proc = region_owner(proc);
    if (!proc) return nullptr;

    for (int i = 0; i < PROCESS_MAX_USER_REGIONS; i++) {
        UserMemoryRegion* region = &proc->regions[i];
        if (region->active && (region->flags & flag)) return region;
    }
    return nullptr;
}

static UserMemoryRegion* find_free_region_slot(Process* proc) {
    proc = region_owner(proc);
    if (!proc) return nullptr;

    for (int i = 0; i < PROCESS_MAX_USER_REGIONS; i++) {
        if (!proc->regions[i].active) return &proc->regions[i];
    }
    return nullptr;
}

static bool region_overlaps(Process* proc, uint64_t start, uint64_t end) {
    proc = region_owner(proc);
    if (!proc) return false;

    for (int i = 0; i < PROCESS_MAX_USER_REGIONS; i++) {
        const UserMemoryRegion* region = &proc->regions[i];
        if (!region->active) continue;
        if (start < region->end && end > region->start) return true;
    }
    return false;
}

static UserMemoryRegion* add_region(Process* proc, uint64_t start, uint64_t end,
                                    uint64_t page_flags, uint32_t flags) {
    proc = region_owner(proc);
    if (!proc || start >= end) return nullptr;

    for (int i = 0; i < PROCESS_MAX_USER_REGIONS; i++) {
        UserMemoryRegion* region = &proc->regions[i];
        if (!region->active) continue;
        if (region->page_flags != page_flags || region->flags != flags) continue;

        if (region->end == start) {
            region->end = end;
            return region;
        }
        if (region->start == end) {
            region->start = start;
            return region;
        }
    }

    UserMemoryRegion* slot = find_free_region_slot(proc);
    if (!slot) {
        // (satoru) TEMP: the region table is full -> every further mmap returns
        // -ENOMEM and the caller's malloc fails ("memory allocation of N bytes
        // failed"). log once so a raised cap can be verified / sized.
        static bool s_region_full_warned = false;
        if (!s_region_full_warned) {
            s_region_full_warned = true;
            SerialLogger::Log("[region] TABLE FULL (");
            SerialLogger::LogDec(PROCESS_MAX_USER_REGIONS);
            SerialLogger::Log(" slots) owner pid=");
            SerialLogger::LogDec((int)proc->pid);
            SerialLogger::Log(" -> mmap ENOMEM (raise PROCESS_MAX_USER_REGIONS)\r\n");
        }
        return nullptr;
    }

    slot->start = start;
    slot->end = end;
    slot->page_flags = page_flags;
    slot->flags = flags;
    slot->active = true;
    return slot;
}

static uint64_t page_flags_from_prot(uint32_t prot) {
    uint64_t flags = PTE_USER;
    if (prot & LINUX_PROT_WRITE) flags |= PTE_WRITABLE;
    if (!(prot & LINUX_PROT_EXEC)) flags |= PTE_NX;
    return flags;
}

// ── deferred user-frame free (smp) ──────────────────────────────────────────
// a frame unmapped from a shared address space may still be reachable through
// another core's stale tlb entry until that core reloads cr3 (its next thread
// switch / timer tick, or our shootdown ipi - which is best-effort when the
// peer has IF masked). freeing the frame right away lets the allocator hand it
// to someone else while the stale writer can still hit it = the random-victim
// memory corruption. quarantine instead - and release on PROOF, not a timer:
// the old ~4ms age bound assumed every core reloads cr3 within one timer tick,
// but a peer sitting IF=0 through a log burst misses its tick AND the
// shootdown ipi for tens of ms; its stale writable entry then sprayed a
// recycled frame (the fontconfig string over firefox's stack canary in
// nsCaret::GetPaintGeometry). a frame is safe only once a shootdown that
// STARTED after its pte was cleared has been acked by EVERY other cpu - track
// that with the smp flush epochs and gate release on it (the age check stays
// as a cheap first filter). callers all run under the kls lock, so no extra
// locking. single-core boots bypass entirely. (satoru)
static uint64_t g_fq_phys[4096];
static uint64_t g_fq_ms[4096];
static uint64_t g_fq_seq[4096];
static int g_fq_head = 0, g_fq_tail = 0;
static void user_frame_quarantine(uint64_t phys) {
    if (!phys) return;
    phys &= ~(uint64_t)(PAGE_SIZE - 1);
    if (SMP::OnlineCount() <= 1) { PMM::FreeFrame(phys); return; }
    uint64_t now = Timer::GetRealMs64();
    // a frame is provably safe to reuse once a shootdown that STARTED after its
    // pte was cleared has been fully acked (epoch advanced past its enqueue
    // stamp). primary release rule: aged past one tick AND epoch-safe. NOTE we
    // must never call SMP::BroadcastTlbFlush() from here - this runs from cow/
    // demand-zero FAULT context (IF=0), and spinning there for acks would keep
    // this core from acking everyone else's shootdown = the [tlbto] cascade.
    // shootdowns happen on their own in the mmap/munmap/mprotect syscalls. (satoru)
    uint64_t safe_seq = SMP::TlbFlushFullSeq();
    while (g_fq_tail != g_fq_head && now - g_fq_ms[g_fq_tail] >= 4 &&
           safe_seq > g_fq_seq[g_fq_tail]) {
        PMM::FreeFrame(g_fq_phys[g_fq_tail]);
        g_fq_tail = (g_fq_tail + 1) & 4095;
    }
    int next = (g_fq_head + 1) & 4095;
    if (next == g_fq_tail) {
        // ring full and the epoch still hasn't advanced past the oldest entry
        // (a peer wedged with IF=0, no fully-acked flush yet). the alternative
        // to reuse is unbounded growth; fall back to a conservative AGE bound:
        // ~50ms is many timer ticks, by which every core has taken at least one
        // preempt tick (each reloads cr3), so even a missed ipi is covered.
        // still no broadcast/serial here. (satoru)
        while (g_fq_tail != g_fq_head &&
               (now - g_fq_ms[g_fq_tail] >= 50 || safe_seq > g_fq_seq[g_fq_tail])) {
            PMM::FreeFrame(g_fq_phys[g_fq_tail]);
            g_fq_tail = (g_fq_tail + 1) & 4095;
        }
        if (next == g_fq_tail) {   // still full: force the single oldest (satoru)
            PMM::FreeFrame(g_fq_phys[g_fq_tail]);
            g_fq_tail = (g_fq_tail + 1) & 4095;
        }
    }
    g_fq_phys[g_fq_head] = phys;
    g_fq_ms[g_fq_head]   = now;
    g_fq_seq[g_fq_head]  = SMP::TlbFlushStartSeq();
    g_fq_head = next;
}

static void unmap_user_range(Process* proc, uint64_t start, uint64_t end) {
    if (!proc || start >= end) return;

    uint64_t page_start = align_down_u64(start, PAGE_SIZE);
    uint64_t page_end = align_up_u64(end, PAGE_SIZE);
    bool any = false;
    bool multi = SMP::OnlineCount() > 1;
    for (uint64_t page = page_start; page < page_end; page += PAGE_SIZE) {
        uint64_t phys = KernelVMM::QueryMappingInAddressSpace(proc->address_space, page);
        if (!phys) continue;
        // with other cores online, unmap WITHOUT freeing and quarantine the
        // frame instead - a sibling's stale tlb entry must never alias a frame
        // the allocator has already reused. (satoru)
        KernelVMM::UnmapPageInAddressSpace(proc->address_space, page, !multi);
        if (multi) user_frame_quarantine(phys);
        any = true;
        if (Scheduler::GetCurrentProcess() == proc) {
            KernelVMM::InvalidatePage(page);
        }
    }
    // smp: sibling threads of this address space may be running on other cores
    // with the just-cleared translations cached. shoot them down now - when the
    // broadcast gets every ack it also advances the flush epoch, which is what
    // lets the quarantine release the frames enqueued above. no-op with a
    // single online cpu. (satoru)
    if (any) SMP::BroadcastTlbFlush();
}

static bool ensure_heap_region(LinuxProcess* proc, Process* task, uint64_t new_break) {
    if (!proc || !task) return false;

    uint64_t region_start = align_up_u64(proc->brk_base, PAGE_SIZE);
    uint64_t region_end = align_up_u64(new_break, PAGE_SIZE);
    UserMemoryRegion* region = find_region_by_flag(task, USER_REGION_HEAP);

    if (region_end <= region_start) {
        if (region && region->active) {
            unmap_user_range(task, region->start, region->end);
            region->active = false;
        }
        return true;
    }

    if (!region) {
        region = add_region(task, region_start, region_end,
                            PTE_USER | PTE_WRITABLE | PTE_NX,
                            USER_REGION_DEMAND_ZERO | USER_REGION_HEAP);
        return region != nullptr;
    }

    if (region_end < region->end) {
        unmap_user_range(task, region_end, region->end);
    }
    region->start = region_start;
    region->end = region_end;
    region->page_flags = PTE_USER | PTE_WRITABLE | PTE_NX;
    region->flags = USER_REGION_DEMAND_ZERO | USER_REGION_HEAP;
    region->active = true;
    return true;
}

// overlap test across EVERY task sharing this task's address space. threads share
// one cr3 but each LinuxProcess has its own regions[] + its own next_mmap_base
// bump pointer, so a thread auto-placing an mmap (a fresh gecko thread stack) only
// saw ITS OWN regions and could pick a base a SIBLING thread already owned -- two
// threads then shared one stack and clobbered each other's frames + return
// addresses, which is the post-wayland glib futex-storm: a g_mutex_lock waiter
// transferred through a corrupted return address and the main thread spun on a
// contended futex forever. consult all same-AS tasks so the allocator never
// double-hands one virtual range. (cherry of cc96426) (satoru)
static bool region_overlaps_cross_thread(Process* proc, uint64_t start, uint64_t end) {
    if (!proc) return false;
    // region_overlaps() resolves region_owner(proc) - the thread-group LEADER - 
    // and add_region() likewise puts EVERY thread's region on the leader's table.
    // so one region_overlaps(proc) already covers the whole thread group; the old
    // loop over all 256 LinuxProcess slots re-scanned that SAME shared table once
    // per sibling thread (~40x for firefox), making each mmap O(threads * regions).
    // that is exactly what made GC chunk allocation (an mmap per 2MB chunk) crawl
    // and pin the chrome main in getOrAllocChunk. a single scan is correct AND
    // ~40x faster. (satoru)
    return region_overlaps(proc, start, end);
}

static uint64_t choose_mmap_base(Process* task, uint64_t requested, uint64_t length) {
    uint64_t base = requested ? align_down_u64(requested, PAGE_SIZE)
                              : align_up_u64(task->next_mmap_base, PAGE_SIZE);

    while (base + length <= USER_MMAP_LIMIT) {
        // cross-thread: never hand back a range a SIBLING thread (same cr3, own
        // regions[]) already owns -- that collision gave two thread stacks one
        // address and corrupted control flow under the futex storm. (satoru)
        if (!region_overlaps_cross_thread(task, base, base + length)) return base;
        base = align_up_u64(base + length + PAGE_SIZE, PAGE_SIZE);
    }
    return 0;
}

static bool split_or_trim_region(Process* task, UserMemoryRegion* region,
                                 uint64_t trim_start, uint64_t trim_end) {
    if (!task || !region || !region->active || trim_start >= trim_end) return false;

    if (trim_start <= region->start && trim_end >= region->end) {
        region->active = false;
        return true;
    }

    if (trim_start <= region->start) {
        region->start = trim_end;
        return true;
    }

    if (trim_end >= region->end) {
        region->end = trim_start;
        return true;
    }

    UserMemoryRegion* split = find_free_region_slot(task);
    if (!split) return false;

    *split = *region;
    split->start = trim_end;
    region->end = trim_start;
    return true;
}

// ---- shm objects: memfd_create backing for wl_shm / posix shared memory ----
// a memfd is backed by a contiguous PMM allocation. mmap(MAP_SHARED, memfd)
// eager-maps those physical pages into the caller, and the in-kernel wayland
// compositor reads the very same pages directly (low ram is identity-mapped,
// so the kernel pointer == physical addr). that is how a real wl_shm pixel
// buffer round-trips client -> compositor without a copy. (satoru)
struct LinuxShmObj {
    uint8_t* base;      // kernel ptr == phys addr (identity-mapped low ram)
    uint64_t size;
    int      refcount;
    bool     used;
};
// (satoru) firefox creates HUNDREDS of memfds during startup (every SharedStringMap =
// prefs/font-list snapshot, every wl_shm pixel pool, every graphics buffer) and close()
// leaks the slot (see sys_close). at 64 slots the table EXHAUSTED mid-startup ->
// memfd_create returned -1 -> shared_memory::CreateFreezable failed -> MemMapSnapshot::Init
// Err -> SharedStringMapBuilder::Finalize Err -> the ctor's MOZ_RELEASE_ASSERT(result.isOk())
// #PF (line 48). the exhaustion point drifts with what's loaded, which is why the crash was
// FLAKY. size the table for a real browsing session; sys_close now also reclaims slots. (satoru)
static constexpr int LINUX_SHM_SLOTS = 2048;
static LinuxShmObj g_linux_shm[LINUX_SHM_SLOTS];

static int shm_alloc_slot() {
    // (satoru) atomic scan-and-claim: without the lock two cores read the same
    // `used==false` slot and both claim it -> aliased memfd objects (see g_shm_lock).
    // short leaf critical section (no alloc under it), so it can't stall a sibling.
    SpinLockGuard _g(g_shm_lock);
    for (int i = 0; i < LINUX_SHM_SLOTS; i++) {
        if (!g_linux_shm[i].used) {
            g_linux_shm[i].used = true;
            g_linux_shm[i].base = nullptr;
            g_linux_shm[i].size = 0;
            g_linux_shm[i].refcount = 1;
            return i;
        }
    }
    return -1;
}
static LinuxShmObj* shm_slot(int idx) {
    if (idx < 0 || idx >= LINUX_SHM_SLOTS || !g_linux_shm[idx].used) return nullptr;
    return &g_linux_shm[idx];
}
// resolve a process fd to its shm object, or null when the fd isn't a memfd (satoru)
static LinuxShmObj* shm_for_fd(LinuxProcess* p, int fd) {
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS) return nullptr;
    if (!p->fds[fd].open || p->fds[fd].type != LFD_MEMFD) return nullptr;
    return shm_slot(p->fds[fd].backend_fd);
}
// grow a memfd's backing to at least `size` bytes; first ftruncate wins (satoru)
static bool shm_set_size(LinuxShmObj* s, uint64_t size) {
    if (!s || size == 0) return false;
    if (s->base) return s->size >= size;        // already sized; never shrink (fast path)
    // (satoru) alloc + zero OUTSIDE the lock (a big memfd is a long memset; holding a
    // spinlock across it stalled firefox before window-create under -smp). then commit
    // under a SHORT lock, re-checking base so "first ftruncate wins": if a sibling won
    // the race, free our spare copy and use theirs. (satoru)
    uint64_t rounded = align_up_u64(size, PAGE_SIZE);
    void* mem = PMM::AllocBytes((size_t)rounded);
    if (!mem) return false;
    memset(mem, 0, (size_t)rounded);
    bool won;
    {
        SpinLockGuard _g(g_shm_lock);
        if (!s->base) { s->base = (uint8_t*)mem; s->size = rounded; won = true; }
        else          { won = false; }
    }
    if (!won) { PMM::FreeBytes(mem, (size_t)rounded); return s->size >= size; }
    return true;
}

static bool handle_demand_zero_fault(Process* task, UserMemoryRegion* region,
                                     uint64_t page_base, InterruptFrame* frame) {
    if (!task || !region || !frame) return false;
    if (!(region->flags & USER_REGION_DEMAND_ZERO)) return false;

    if ((frame->error_code & PFERR_WRITE) && !(region->page_flags & PTE_WRITABLE)) {
        return false;
    }
    if ((frame->error_code & PFERR_FETCH) && (region->page_flags & PTE_NX)) {
        return false;
    }

    // smp: two threads on two cores can fault the SAME page back-to-back (the
    // kls lock serializes the handlers, not the faults). the loser must NOT
    // alloc+remap - that would replace the winner's frame and silently discard
    // whatever it already wrote (plus leave the winner's tlb pointing at the
    // orphaned frame). if the page is present by the time we get here, just
    // flush our local tlb and retry the instruction. (satoru)
    if (KernelVMM::QueryMappingInAddressSpace(task->address_space, page_base)) {
        KernelVMM::InvalidatePage(page_base);
        if (frame->error_code & PFERR_USER) {
            Scheduler::SaveUserFrame(task, frame);
        }
        return true;
    }

    void* page = PMM::AllocBytes(PAGE_SIZE);
    if (!page) return false;

    if (!KernelVMM::MapPageInAddressSpace(task->address_space, page_base,
                                          (uint64_t)(uintptr_t)page,
                                          region->page_flags)) {
        PMM::FreeBytes(page, PAGE_SIZE);
        return false;
    }

    if (Scheduler::GetCurrentProcess() == task) {
        KernelVMM::InvalidatePage(page_base);
    }

    // fault-ahead batching: firefox touches its big anon regions (jemalloc
    // arenas, gc chunks, decoded images) mostly sequentially, so a fault at
    // page N is a strong predictor of faults at N+1..N+15. map them now while
    // we already paid the kls entry + region walk - one fault does 16 pages'
    // worth of work, cutting the demand-zero fault count ~16x on linear
    // touches. safety: only never-mapped pages inside the SAME region get a
    // fresh zero frame (never replaces an existing mapping, so no other
    // thread's writes can be discarded), and a new mapping needs no tlb
    // shootdown - no core can hold a stale entry for a page that was never
    // present. alloc failure just stops the batch; the faulting page is
    // already in. EXCLUDED: giant reservations (>1gb - the rlbox/wasm2c 16gb
    // sandboxes): batching inside them reproducibly ended in a ring-3 null
    // write at a fixed sandbox rip (the sandbox appears to reason about which
    // of its pages are actually resident); normal-size regions batch fine and
    // hold nearly all of the win. (satoru)
    // stability baseline: batching disabled while the paint-rate regression is
    // bisected (full batching = fast paints but a fixed-rip crash on page load;
    // size-capped sampled 0/3 paints). re-enable behind the ci gate once the
    // startup stall is understood. (satoru)
    bool batch_ok = false && (region->end - region->start) <= (1ULL << 30);
    for (int ahead = 1; batch_ok && ahead < 16; ahead++) {
        uint64_t next = page_base + (uint64_t)ahead * PAGE_SIZE;
        if (next >= region->end) break;                    // stay inside the region (satoru)
        if (KernelVMM::QueryMappingInAddressSpace(task->address_space, next))
            continue;                                      // already mapped (another thread) (satoru)
        void* np = PMM::AllocBytes(PAGE_SIZE);
        if (!np) break;                                    // low memory: stop batching (satoru)
        if (!KernelVMM::MapPageInAddressSpace(task->address_space, next,
                                              (uint64_t)(uintptr_t)np,
                                              region->page_flags)) {
            PMM::FreeBytes(np, PAGE_SIZE);
            break;
        }
    }

    // only persist the trap frame as the task's USER state for a ring-3 fault; a
    // kernel-mode fault (the kernel touching lazy user mem mid-syscall, e.g.
    // mremap's memcpy) carries the kernel exception frame -- saving it would corrupt
    // the task's saved user frame. the kernel just re-executes the faulting
    // instruction once we've mapped the page. (satoru)
    if (frame->error_code & PFERR_USER) {
        Scheduler::SaveUserFrame(task, frame);
    }
    return true;
}

static bool handle_cow_fault(Process* task, uint64_t page_base,
                             uint64_t page_flags, InterruptFrame* frame) {
    if (!task || !frame) return false;
    if (!(page_flags & PTE_COW) || !(frame->error_code & PFERR_WRITE)) return false;

    uint64_t phys = KernelVMM::QueryMappingInAddressSpace(task->address_space, page_base);
    if (!phys) return false;

    uint64_t new_flags = (page_flags | PTE_WRITABLE) & ~PTE_COW;
    if (PMM::GetFrameRefCount(phys) > 1) {
        uint64_t new_frame = PMM::AllocFrame();
        if (!new_frame) return false;

        memcpy((void*)(uintptr_t)new_frame, (const void*)(uintptr_t)phys, PAGE_SIZE);
        if (!KernelVMM::MapPageInAddressSpace(task->address_space, page_base, new_frame, new_flags)) {
            PMM::FreeFrame(new_frame);
            return false;
        }
        user_frame_quarantine(phys);   // stale-tlb-safe deferred free (smp) (satoru)
    } else {
        if (!KernelVMM::MapPageInAddressSpace(task->address_space, page_base, phys, new_flags)) {
            return false;
        }
    }

    if (Scheduler::GetCurrentProcess() == task) {
        KernelVMM::InvalidatePage(page_base);
    }
    // only persist the trap frame as the task's USER state for a ring-3 fault; a
    // kernel-mode COW fault (the kernel writing a forked/threaded task's not-yet-
    // resolved COW page mid-syscall -- e.g. recvmsg scattering bytes into a shared
    // page) carries the ring-0 exception frame. saving it overwrites the task's
    // saved user frame with kernel cs/ss + a kernel rip, and the next schedule
    // iretq's into ring-0 garbage -> the firefox ppoll-switch #UD (RIP=3 CS=8).
    // mirror handle_demand_zero_fault's gate. (satoru)
    if (frame->error_code & PFERR_USER) {
        Scheduler::SaveUserFrame(task, frame);
    }
    return true;
}

static bool is_valid_exec_elf(const void* data, uint32_t size) {
    if (size < sizeof(Elf32Header)) return false;

    const Elf32Header* header = (const Elf32Header*)data;
    if (header->e_magic != ELF_MAGIC) return false;
    if (header->e_class != 1 || header->e_data != 1) return false;
    if (header->e_machine != 3) return false;
    return true;
}

static bool load_exec_segments(uint64_t address_space, const uint8_t* image,
                               uint32_t size, uint32_t* entry_point,
                               uint32_t* brk_end) {
    const Elf32Header* header = (const Elf32Header*)image;
    // compute in 64-bit so a crafted ELF (e_phnum/e_phentsize = 0xFFFF) can't
    // overflow the bound check and pass, then OOB-read past the image. (satoru)
    if ((uint64_t)header->e_phoff +
        (uint64_t)header->e_phnum * (uint64_t)header->e_phentsize > (uint64_t)size)
        return false;

    uint32_t highest_end = 0;
    for (int i = 0; i < header->e_phnum; i++) {
        const Elf32Phdr* ph = (const Elf32Phdr*)(image + header->e_phoff + i * header->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_offset + ph->p_filesz > size) return false;

        uint32_t seg_start = ph->p_vaddr & ~(uint32_t)(PAGE_SIZE - 1);
        uint32_t seg_end = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
        uint64_t page_flags = PTE_USER;
        if (ph->p_flags & 0x2U) page_flags |= PTE_WRITABLE;
        if (!(ph->p_flags & 0x1U)) page_flags |= PTE_NX;

        for (uint32_t page_va = seg_start; page_va < seg_end; page_va += PAGE_SIZE) {
            void* page = PMM::AllocBytes(PAGE_SIZE);
            if (!page) return false;

            uint32_t file_begin = ph->p_vaddr;
            uint32_t file_end = ph->p_vaddr + ph->p_filesz;
            uint32_t copy_begin = (page_va > file_begin) ? page_va : file_begin;
            uint32_t copy_end = ((page_va + PAGE_SIZE) < file_end) ? (page_va + PAGE_SIZE) : file_end;
            if (copy_begin < copy_end) {
                memcpy((uint8_t*)page + (copy_begin - page_va),
                       image + ph->p_offset + (copy_begin - ph->p_vaddr),
                       copy_end - copy_begin);
            }

            if (!KernelVMM::MapPageInAddressSpace(address_space, page_va,
                                                  (uint64_t)(uintptr_t)page,
                                                  page_flags)) {
                PMM::FreeBytes(page, PAGE_SIZE);
                return false;
            }
        }

        uint32_t segment_end = ph->p_vaddr + ph->p_memsz;
        if (segment_end > highest_end) highest_end = segment_end;
    }

    if (entry_point) *entry_point = header->e_entry;
    if (brk_end) {
        uint32_t aligned = (highest_end + PAGE_SIZE - 1) & ~(uint32_t)(PAGE_SIZE - 1);
        *brk_end = aligned > LINUX_BRK_INITIAL ? aligned : LINUX_BRK_INITIAL;
    }
    return true;
}

static bool map_exec_stack(uint64_t address_space, uint64_t stack_top, void** out_stack_phys) {
    void* stack_phys = PMM::AllocBytes(EXEC_STACK_BYTES);
    if (!stack_phys) return false;

    uint64_t stack_base = stack_top - EXEC_STACK_BYTES;
    for (uint64_t offset = 0; offset < EXEC_STACK_BYTES; offset += PAGE_SIZE) {
        if (!KernelVMM::MapPageInAddressSpace(address_space, stack_base + offset,
                                              (uint64_t)(uintptr_t)stack_phys + offset,
                                              PTE_USER | PTE_WRITABLE)) {
            PMM::FreeBytes(stack_phys, EXEC_STACK_BYTES);
            return false;
        }
    }

    *out_stack_phys = stack_phys;
    return true;
}

static bool build_exec_stack(void* stack_phys, uint64_t stack_top, uint32_t argv_ptr,
                             uint64_t* out_rsp) {
    uint64_t stack_base = stack_top - EXEC_STACK_BYTES;
    uint64_t sp = stack_top;
    uint32_t arg_ptrs[EXEC_MAX_ARGC];
    int argc = 0;

    memset(arg_ptrs, 0, sizeof(arg_ptrs));
    if (argv_ptr) {
        uint32_t* argv = (uint32_t*)(uintptr_t)argv_ptr;
        while (argc < EXEC_MAX_ARGC && argv[argc]) {
            const char* arg = (const char*)(uintptr_t)argv[argc];
            int len = 0;
            while (arg[len]) len++;

            sp -= (uint64_t)(len + 1);
            memcpy((uint8_t*)stack_phys + (sp - stack_base), arg, len + 1);
            arg_ptrs[argc++] = (uint32_t)sp;
        }
    }

    sp &= ~0xFULL;

    sp -= sizeof(uint32_t);
    *(uint32_t*)((uint8_t*)stack_phys + (sp - stack_base)) = 0;

    sp -= sizeof(uint32_t);
    *(uint32_t*)((uint8_t*)stack_phys + (sp - stack_base)) = 0;

    for (int i = argc - 1; i >= 0; i--) {
        sp -= sizeof(uint32_t);
        *(uint32_t*)((uint8_t*)stack_phys + (sp - stack_base)) = arg_ptrs[i];
    }

    sp -= sizeof(uint32_t);
    *(uint32_t*)((uint8_t*)stack_phys + (sp - stack_base)) = (uint32_t)argc;

    *out_rsp = sp;
    return true;
}

static bool switch_to_ready_user(InterruptFrame* frame) {
    if (!frame) return false;
    futex_sweep_timeouts();  // re-ready any timed-out futex waiters before picking next (satoru)
    if (!Scheduler::ScheduleNextUser(frame)) return false;

    int next_idx = find_process_index_by_task(Scheduler::GetCurrentProcess());
    if (next_idx < 0) return false;

    LinuxSyscall::SetCurrent(next_idx);
    current_frame_rewritten = true;
    return true;
}

// timer-driven preemption (registered as the irq0 handler). when the 1 khz pit
// tick interrupts ring-3 user code and another user task is ready, round-robin
// to it: save the interrupted task's full state (regs + fs base + fpu) and load
// the next; the irq's iretq then resumes that task. ring-3 frames only, so a
// tick during a syscall (ring-0) or a kernel process is ignored, and a single-
// threaded user program (no other ready task) is never switched. this is what
// lets clone+futex threads actually time-share the cpu. (satoru)
static uint32_t g_preempt_ticks = 0;
static void kls_timer_preempt(InterruptFrame* frame) {
    if (!frame || (frame->cs & 3) != 3) return;      // ring-3 user only
    if (!Userspace::IsActive()) return;
    Process* cur = Scheduler::GetCurrentProcess();
    if (!cur || !cur->is_user()) return;

    // ~1 ms timeslice (every tick): firefox's startup has MANY tight ring-3
    // spin-then-park thread-handshakes that each race; maximum preemption interleaves
    // the spawned threads as often as possible so a handshake's sibling always gets cpu
    // before the window closes (the marker-delay heisenbug = exactly this). fxsave is
    // cheap vs the alternative (a wedged browser). (satoru)
    ++g_preempt_ticks;

    Scheduler::SaveUserFrame(cur, frame);            // capture live regs+fs+fpu
    // re-ready any futex waiter whose (repoll/timeout) deadline has passed BEFORE we
    // pick the next task. without this, a thread spinning in pure ring-3 (a parking_lot
    // /rayon spin or spinlock that never enters the kernel) never triggers a sweep, so
    // its blocked siblings stay off the run queue and ScheduleNextUser just resumes the
    // spinner -> firefox's multi-thread startup deadlocks. SAFE here: this handler only
    // runs when the PIT irq interrupted RING-3, so no syscall is mid-updating the futex
    // queue on this cpu (the reason the sweep is otherwise syscall-context-only). this
    // is the timer-IRQ half of "true preemption": preempt the spinner AND advance the
    // futex deadlines so a ready sibling exists to switch to. (satoru)
    futex_sweep_timeouts();
    if (!Scheduler::ScheduleNextUser(frame)) {
        // staying on the same thread: reload cr3 so a stale translation from a
        // sibling's munmap on another core (a missed shootdown-ipi window) is
        // bounded to one tick - mirrors ApTimerPreempt. (satoru)
        uint64_t c3;
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(c3));
        __asm__ __volatile__("mov %0, %%cr3" : : "r"(c3) : "memory");
        return; // nothing else ready
    }
    // irq0 (isr_common, no swapgs): fix up the resumed thread's gs base. (satoru)
    Scheduler::FixupGsAfterIsrSwitch();

    int next_idx = find_process_index_by_task(Scheduler::GetCurrentProcess());
    if (next_idx >= 0) LinuxSyscall::SetCurrent(next_idx);
}

static void wake_waiting_parent(LinuxProcess* child, int child_index) {
    if (!child || !child->task || !child->task->parent) return;

    Process* parent_task = child->task->parent;
    if (!parent_task->waiting_for_child) return;

    if (parent_task->waiting_child_pid != 0 && parent_task->waiting_child_pid != child->pid) {
        return;
    }

    if (parent_task->waiting_status_ptr) {
        write_user_u32(parent_task, parent_task->waiting_status_ptr,
                       ((uint32_t)child->exit_code & 0xFFU) << 8);
    }

    parent_task->user_frame.rax = child->pid;
    parent_task->waiting_for_child = false;
    parent_task->waiting_child_pid = 0;
    parent_task->waiting_status_ptr = 0;
    // atomic Blocked->Ready (see wake_blocked_to_ready): the cpu that blocked
    // the parent may still be unwinding, but on_cpu (not a deferral tick) is
    // what gates a cross-cpu resume until its frame is released, so the wake
    // can be immediate. (satoru)
    if (wake_blocked_to_ready(parent_task)) parent_task->sleep_ticks = 0;

    Scheduler::ReapProcess(child->task);
    child->task = nullptr;
    LinuxSyscall::DestroyProcess(child_index);
}

// ── real futex wait-queue ────────────────────────────────────────────────
// a waiter is keyed by (address_space, uaddr): two distinct processes can map
// the same virtual address yet must not cross-wake, and threads in one process
// share an address space so they DO match. (satoru)
constexpr int FUTEX_MAX_WAITERS = 256;  // 64->256: firefox's many threads can have lots of concurrent futex waiters (satoru)
// (satoru) an originally-INFINITE futex wait is re-polled every this many ms instead
// of blocking truly forever, so a lost/missed FUTEX_WAKE self-heals (the caller
// re-tests *uaddr + re-waits) rather than deadlocking the process. this is what
// unblocks firefox startup on kurono: its stylo/rayon + ipc wake/wait timing races
// occasionally drop a wakeup, and without a re-poll the chrome main wedges (the
// hang moved as instrumentation shifted the timing - classic missed-wake). (satoru)
// 100->8: under smp thread dispatch the browser-chrome startup performs MANY
// futex handshakes and a lost cross-core wake stalls each until the repoll heals
// it. at 100ms/handshake the chrome main crawled and non-deterministically
// wedged (some runs reached the event loop, some stalled early). 8ms heals a
// lost wake ~12x faster so startup progresses reliably to the window; the extra
// re-test overhead (a cheap *uaddr read + re-enqueue) is dwarfed by the win. a
// wake that ISN'T lost still wakes immediately - this only bounds the worst
// case. (satoru)
constexpr uint64_t FUTEX_REPOLL_MS = 8;

// ── posix byte-range file locks (fcntl F_GETLK / F_SETLK / F_SETLKW) ─────────
// REAL advisory locks keyed by (file path hash, [start,end), owner). the old
// stub granted every F_SETLK unconditionally - sqlite's wal shm-lock dance then
// let two connections both hold "exclusive" wal locks, sqlite detects the
// inconsistency and fails with SQLITE_PROTOCOL ("locking protocol" console
// errors) in retry loops: wedged places init (delayedStartupFinished never
// fires) and a wedged nss cert9.db open (the SOCKET THREAD's first act) = dead
// networking. owner = thread-group leader, so threads of one process share
// locks (posix semantics) while distinct processes conflict. (satoru)
struct KFileLock {
    bool     active;
    int16_t  type;          // 0=F_RDLCK 1=F_WRLCK (satoru)
    uint64_t path_hash;
    uint64_t start, end;    // [start,end); end==~0ull means to-eof (satoru)
    Process* owner;
};
constexpr int KFILE_LOCK_SLOTS = 256;
static KFileLock g_file_locks[KFILE_LOCK_SLOTS];
static Spinlock  g_flock_lock;

static uint64_t flock_path_hash(const char* s) {
    uint64_t h = 1469598103934665603ull;              // fnv-1a (satoru)
    if (s) while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ull; }
    return h;
}

// drop every lock `owner` holds on `path_hash` - posix close semantics (any
// close of a file drops the process's locks on it; sqlite's unix vfs is built
// around exactly this behaviour). (satoru)
static void flock_drop(Process* owner, uint64_t path_hash) {
    SpinLockGuard g(g_flock_lock);
    for (int i = 0; i < KFILE_LOCK_SLOTS; i++) {
        KFileLock* L = &g_file_locks[i];
        if (L->active && L->owner == owner && L->path_hash == path_hash)
            L->active = false;
    }
}
// (satoru) NON-static + the table NON-static below so gdb can resolve the type +
// the array by name (file-statics get no DWARF entry in this -g build) to dump the
// full live futex wait-graph while chasing the firefox startup deadlock. (satoru)
struct FutexWaiter {
    Process*  task;
    uint64_t  addr_space;
    uintptr_t uaddr;
    uint64_t  phys_key;   // physical page|offset backing uaddr, so a SHARED futex
                          // can cross-wake between processes with different address
                          // spaces / virtual addresses (e10s ipc). (satoru)
    uint32_t  bitset;
    bool      active;
    uint64_t  deadline_ms;  // 0 == infinite; else absolute Timer::GetRealMs64() ms
                            // at which futex_sweep_timeouts releases the waiter with
                            // -ETIMEDOUT, so a timed FUTEX_WAIT can't block forever
                            // when no explicit FUTEX_WAKE ever arrives. (satoru)
    bool      repoll;       // true when this is an originally-INFINITE wait we gave a
                            // re-poll deadline: on sweep it's released with a SPURIOUS
                            // wake (rax=0), not -ETIMEDOUT, so the caller re-tests
                            // *uaddr and re-waits (lost-wake self-heal). (satoru)
};
FutexWaiter g_futex_waiters[FUTEX_MAX_WAITERS];  // (satoru) non-static: gdb-visible for wait-graph dump

// atomically transition a task Blocked -> Ready. this is the SINGLE-OWNER-STATE
// fix (the "light CAS" the notes prescribe): the wake path and the scheduler
// live in two different lock domains (g_futex_lock vs g_sched_lock), so a wake
// that merely READ state==Blocked could act on a task the scheduler had already
// picked (set Running) - "spending" a wake on a non-blocked task and starving a
// genuinely blocked sibling. that lost wake is the herd/bringup STALL. the CAS
// makes the wake take effect ONLY if the task is still truly Blocked; if it
// lost the race (already Running/Ready) the caller must NOT consume the wake,
// so it flows to a real blocked waiter. returns true iff this call performed
// the transition. immediate (no sleep_ticks deferral): on_cpu already stops a
// cross-cpu resume before the owning cpu released the frame, so there is no
// need to also delay the state flip a tick. (satoru)
static inline bool wake_blocked_to_ready(Process* t) {
    if (!t) return false;
    int expect = (int)Process_Blocked;
    return __atomic_compare_exchange_n((int*)&t->state, &expect,
                                       (int)Process_Ready, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

// translate a user futex VA to a stable cross-process key: the physical
// page|offset backing it. two processes that MAP_SHARED the same object see the
// same word at the same physical address, so keying on phys lets a shared futex
// wake across address spaces. returns 0 if the page isn't mapped (the caller
// then falls back to matching on the address_space+uaddr key). (satoru)
static uint64_t futex_phys_key(Process* task, uintptr_t uaddr) {
    if (!task) return 0;
    return KernelVMM::QueryMappingInAddressSpace(task->address_space, uaddr);
}

// block the current task on (address_space, uaddr) and yield to another ready
// user task by rewriting the int 0x80 trap frame (same mechanism as waitpid).
// returns true if the task was enqueued AND successfully descheduled. (satoru)
static bool futex_enqueue_and_block(Process* task, uintptr_t uaddr,
                                    uint32_t expected_val, uint32_t bitset,
                                    uint64_t deadline_ms) {
    if (!task || !current_syscall_frame) return false;

    // never block a futex truly forever: convert an infinite wait (deadline 0) into a
    // periodic re-poll so a lost/missed FUTEX_WAKE self-heals (see FUTEX_REPOLL_MS).
    // on its sweep a repoll waiter is released with a spurious wake (rax=0), so the
    // caller just re-tests *uaddr in ring-3 and re-waits. (satoru)
    bool repoll = false;
    if (deadline_ms == 0) {
        deadline_ms = Timer::GetRealMs64() + FUTEX_REPOLL_MS;
        repoll = true;
    }
    // resolve the phys key AND the raw phys address of *uaddr BEFORE taking the
    // queue lock: the page-table walk must not run under a spinlock a cross-cpu
    // waker could be spinning on. the pre-resolved phys lets the atomic section
    // below read the futex word with a single load (no walk) while holding the
    // lock. (satoru)
    uint64_t pkey  = futex_phys_key(task, uaddr);
    uint64_t uphys = KernelVMM::QueryMappingInAddressSpace(task->address_space, uaddr);

    int slot = -1;   // (satoru) enqueue happens atomically with the value-check below

    // (satoru) ATOMIC value-check + enqueue + block-commit, all under g_futex_lock.
    // this is the classic lost-wake-free futex pattern, and the one rust-std /
    // rayon raw-futex condvars (SW-WR's WRWorker pool + SwComposite thread) demand
    // in a way musl's pthread condvars tolerated. a cross-cpu FUTEX_WAKE mutates
    // *uaddr in userspace and THEN takes this same lock to scan; serialising on the
    // lock means we EITHER observe the new *uaddr here (value != expected -> we do
    // NOT block, caller re-tests in ring-3) OR the waker sees us already enqueued
    // AND already Process_Blocked (state committed under the lock) -> it wakes us.
    // there is no window where we are enqueued-but-Running (the old multi-phase
    // enqueue/recheck/commit left one, which also made FUTEX_WAKE's max-count
    // inexact - it could "spend" a wake on a Running waiter that was going to abort
    // anyway, starving a genuinely blocked sibling; a lost render-thread wakeup =
    // the blank content frame). read_user_u32 is a page-table-walk read (never
    // faults), so it is safe to hold the spinlock across it. (satoru)
    if (!uphys) return false;   // uaddr not mapped -> spurious, re-test in ring-3 (satoru)
    {
        SpinLockGuard fg(g_futex_lock);
        // single load via the pre-resolved phys - NO page-table walk under the
        // lock (see the resolve above). (satoru)
        uint32_t curval = *(const volatile uint32_t*)(uintptr_t)uphys;
        if (curval != expected_val) {
            return false;   // *uaddr changed (a wake raced in) (satoru)
        }
        for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
            if (!g_futex_waiters[i].active) { slot = i; break; }
        }
        if (slot < 0) return false;  // queue full - caller returns -EAGAIN (satoru)
        g_futex_waiters[slot].task       = task;
        g_futex_waiters[slot].addr_space = task->address_space;
        g_futex_waiters[slot].uaddr      = uaddr;
        g_futex_waiters[slot].phys_key   = pkey;
        g_futex_waiters[slot].bitset     = bitset ? bitset : 0xFFFFFFFFu;
        g_futex_waiters[slot].deadline_ms= deadline_ms;
        g_futex_waiters[slot].repoll     = repoll;
        g_futex_waiters[slot].active     = true;
        // commit the blocked state WHILE STILL HOLDING THE LOCK, so the instant a
        // waker can see us in the queue we are already Blocked (never Running).
        // single-owner: the state write ALSO nests g_sched_lock so the scheduler
        // never reads a half-written state from the futex domain. (satoru)
        uint64_t sf; Scheduler::StateLock(&sf);
        task->state = Process_Blocked;
        Scheduler::StateUnlock(sf);
        task->user_frame.rax = 0;
    }

    if (!switch_to_ready_user(current_syscall_frame)) {
        // nothing else runnable - undo the block so we don't wedge the only
        // user task off the run queue with no one left to wake it. (satoru)
        {
            SpinLockGuard fg(g_futex_lock);
            // same ownership check as the recheck-abort above: a cross-cpu wake
            // may have consumed this slot and another waiter reused it while we
            // were inside switch_to_ready_user - clearing it blindly would make
            // the new owner unwakeable. (satoru)
            if (g_futex_waiters[slot].active && g_futex_waiters[slot].task == task)
                g_futex_waiters[slot].active = false;
        }
        // single-owner: undo the block (state + sleep_ticks) under g_sched_lock.
        // a raced wake may have set the deferred-promotion tick while we were
        // blocked; we are resuming ourselves, so drop it - a stale sleep_ticks
        // would spuriously re-ready this task out of its NEXT unrelated block. (satoru)
        {
            uint64_t sf; Scheduler::StateLock(&sf);
            task->state = Process_Running;
            task->sleep_ticks = 0;
            Scheduler::StateUnlock(sf);
        }
        return false;
    }
    return true;
}

// wake up to `max` waiters matching (address_space, uaddr) whose bitset
// intersects `bitset`. each woken task is made ready and its futex syscall made
// to return 0. returns the number woken. (satoru)
static int futex_do_wake(uint64_t addr_space, uintptr_t uaddr, uint64_t phys_key,
                         int max, uint32_t bitset) {
    if (max <= 0) return 0;
    uint32_t want = bitset ? bitset : 0xFFFFFFFFu;
    int woken = 0;
    SpinLockGuard fg(g_futex_lock);   // atomic vs cross-cpu enqueue + sweep (satoru)
    for (int i = 0; i < FUTEX_MAX_WAITERS && woken < max; i++) {
        FutexWaiter* w = &g_futex_waiters[i];
        if (!w->active) continue;
        // match by EITHER the (address_space,uaddr) key (same-process threads,
        // the common case) OR the same physical page|offset, so a SHARED-memory
        // futex can wake a waiter in a different process (e10s ipc). (satoru)
        bool same_va   = (w->addr_space == addr_space && w->uaddr == uaddr);
        bool same_phys = (phys_key && w->phys_key && w->phys_key == phys_key);
        if (!same_va && !same_phys) continue;
        if ((w->bitset & want) == 0) continue;

        if (w->task) {
            // atomic CAS Blocked->Ready. if it FAILS the task wasn't blocked
            // (the scheduler already picked it / another waker got it), so this
            // wake must NOT be spent here - leave the slot for a genuinely
            // blocked waiter and move on. only a successful transition consumes
            // the wake + retires the slot. this is the lost-wake fix that was
            // the herd/bringup stall. (satoru)
            if (wake_blocked_to_ready(w->task)) {
                w->task->user_frame.rax = 0;   // futex() returns 0 to the waiter
                w->task->sleep_ticks = 0;      // cancel any stale deferred tick (satoru)
                w->active = false;
                w->task   = nullptr;
                woken++;
            }
            // CAS failed: task not Blocked. it is on its way to running anyway
            // (a raced wake already took it), so retire the slot without
            // counting - it is effectively already awake. (satoru)
            else {
                w->active = false;
                w->task   = nullptr;
            }
        } else {
            w->active = false;
        }
    }
    return woken;
}

// release every timed futex waiter whose deadline has elapsed, making its
// futex() return -ETIMEDOUT. without this a FUTEX_WAIT/_BITSET that carried a
// timeout (what pthread_cond_timedwait and gecko's main-loop condvars issue)
// would block forever - the thread that should wake after N ms to re-poll its
// state never does, stalling the whole process (this is the firefox e10s
// parent stall). mirrors do_poll_wait honouring its poll_deadline_ms. runs only
// from switch_to_ready_user (syscall context, never irq) so it can't reenter a
// half-updated queue. (satoru)
static void futex_sweep_timeouts() {
    uint64_t now = Timer::GetRealMs64();
    SpinLockGuard fg(g_futex_lock);   // atomic vs cross-cpu enqueue + wake (satoru)
    for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
        FutexWaiter* w = &g_futex_waiters[i];
        if (!w->active || w->deadline_ms == 0) continue;   // inactive / infinite (satoru)
        if (now < w->deadline_ms) continue;                // not due yet (satoru)
        if (w->task) {
            // repoll (originally-infinite) waiters wake SPURIOUSLY (0) so the caller
            // re-tests *uaddr + re-waits; real timed waits get -ETIMEDOUT so poll /
            // pthread_cond_timedwait see their timeout. (satoru)
            w->task->user_frame.rax = w->repoll ? 0 : (uint64_t)(int64_t)(-110);
            // atomic Blocked->Ready (see wake_blocked_to_ready): immediate, and
            // it no-ops harmlessly if the task is already runnable. (satoru)
            if (wake_blocked_to_ready(w->task)) w->task->sleep_ticks = 0;
        }
        w->active = false;
        w->task   = nullptr;
    }
}
}

// ── vfork / posix_spawn support ──────────────────────────────────────────────
// firefox launches its content process with posix_spawn = clone(CLONE_VM |
// CLONE_VFORK): the child shares our address space, runs the file-action setup on
// its own child_stack, then execve's into a FRESH address space. CLONE_VFORK
// requires SUSPENDING the parent until the child execve's or _exit's. we track the
// in-flight links here; the clone handler blocks+registers the parent, and
// execve/exit wakes it. without this firefox's GeckoChildProcessHost never gets a
// content process and the main thread deadlocks in WaitForProcessHandle. (satoru)
struct VforkLink { Process* child; Process* parent; bool active; };
static VforkLink g_vfork_links[16];

static void vfork_register(Process* child, Process* parent) {
    for (int i = 0; i < 16; i++) {
        if (!g_vfork_links[i].active) {
            g_vfork_links[i].child  = child;
            g_vfork_links[i].parent = parent;
            g_vfork_links[i].active = true;
            return;
        }
    }
}

// wake the suspended vfork parent of `child` (the child has execve'd or exited).
// returns true iff `child` was a registered vfork child. (satoru)
static bool vfork_wake_parent(Process* child) {
    for (int i = 0; i < 16; i++) {
        if (g_vfork_links[i].active && g_vfork_links[i].child == child) {
            Process* p = g_vfork_links[i].parent;
            // deferred promotion (multicore) - see futex_do_wake. single-owner
            // state transition under g_sched_lock. (satoru)
            if (p && wake_blocked_to_ready(p)) p->sleep_ticks = 0;   // atomic (satoru)
            g_vfork_links[i].active = false;
            return true;
        }
    }
    return false;
}

static int ls_slen(const char* s) {
    int n = 0; while (s && s[n]) n++; return n;
}

static void ls_scpy(char* d, const char* s, int mx) {
    int i = 0;
    while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static bool ls_seq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

static bool ls_starts(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s != *prefix) return false;
        s++; prefix++;
    }
    return true;
}

static void ls_cat(char* d, const char* s, int mx) {
    int dl = ls_slen(d);
    int i = 0;
    while (s[i] && dl + i < mx - 1) { d[dl + i] = s[i]; i++; }
    d[dl + i] = 0;
}

// minimal int-to-ascii (decimal or hex, base 10/16) for /proc generation
static void ls_itoa(int v, char* out, int base) {
    char tmp[24];
    int i = 0;
    bool neg = false;
    unsigned int uv;
    if (base == 10 && v < 0) { neg = true; uv = (unsigned int)(-v); }
    else uv = (unsigned int)v;
    if (uv == 0) tmp[i++] = '0';
    else while (uv && i < (int)sizeof(tmp)) {
        unsigned int d = uv % (unsigned int)base;
        tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        uv /= (unsigned int)base;
    }
    int o = 0;
    if (neg) out[o++] = '-';
    while (i > 0) out[o++] = tmp[--i];
    out[o] = 0;
}

static void LinuxInt80Entry(InterruptFrame* frame) {
    if (!frame) return;

    current_syscall_frame = frame;
    current_frame_rewritten = false;
    resume_userspace_session = false;
    resume_userspace_exit_code = 0;

    Process* running = Scheduler::GetCurrentProcess();
    if (Userspace::IsActive() && running && running->is_user()) {
        Scheduler::SaveUserFrame(running, frame);
        // derive this cpu's linux current from the SCHEDULER current at every
        // entry: the ap timer preempt switches threads without touching the
        // linux index, so trusting the cached per-cpu index would resolve the
        // WRONG LinuxProcess after such a switch. (satoru)
        int ridx = find_process_index_by_task(running);
        if (ridx >= 0) LinuxSyscall::SetCurrent(ridx);
    }

    // int 0x80 enters through an interrupt gate, so IF is cleared on entry.
    // Re-enable interrupts once the kernel stack/frame are in place so timer
    // ticks continue to advance during longer syscall handlers.
    HAL::EnableInterrupts();
    // serialize the syscall body across cores; ring-3 stays parallel. (satoru)
    kls_lock();
    // the ui pump drives the bsp's shell/desktop - never from an ap. (satoru)
    if (SMP::CpuIndex() == 0) KuronoShell::PumpUI();

    // the syscall number is small, but the arg registers carry full
    // 64-bit user pointers/addresses - pass them through untruncated so a
    // pie binary mapped above 4gb reaches the handlers with intact
    // pointers, and keep the result 64-bit so a >4gb mmap return is not
    // truncated before it lands in rax (satoru)
    uint64_t syscall_number = frame->rax;
    int64_t result = LinuxSyscall::Dispatch(
        syscall_number,
        frame->rbx,
        frame->rcx,
        frame->rdx,
        frame->rsi,
        frame->rdi
    );

    if (!current_frame_rewritten) {
        frame->rax = (uint64_t)(int64_t)result;

        Process* current = Scheduler::GetCurrentProcess();
        if (Userspace::IsActive() && current && current->is_user()) {
            Scheduler::SaveUserFrame(current, frame);
        }
    }

    current_syscall_frame = nullptr;

    kls_unlock();

    if (resume_userspace_session) {
        Userspace::HandleProcessExit(resume_userspace_exit_code);
    }

    HAL::DisableInterrupts();
}

// the x86_64 syscall core lives in linux_syscall_x64.cpp (translates the amd64
// nr and dispatches). we call it from the frame handler below - defined here in
// the same TU as current_syscall_frame and the switch helpers so the SYSCALL
// fast path can block/switch/exit threads exactly like int 0x80. (satoru)
extern "C" int64_t SyscallEntryX64Handler(uint64_t nr, uint64_t a0, uint64_t a1,
                                          uint64_t a2, uint64_t a3, uint64_t a4,
                                          uint64_t a5);

// x86_64 SYSCALL fast-path frame handler. the asm stub (src/hal/syscall_entry.asm)
// builds a full InterruptFrame from the live registers and calls here; on return
// it restores the (possibly rewritten) frame and IRETQs. this mirrors
// LinuxInt80Entry field-for-field: save the caller's frame so clone snapshots a
// fresh parent, dispatch, then either write the result back or - if a handler
// rewrote the frame (futex block / thread exit / clone) - leave it for the stub
// to IRETQ into the next task. (satoru)
extern "C" void SyscallEntryX64FrameHandler(InterruptFrame* frame) {
    if (!frame) return;

    current_syscall_frame      = frame;
    current_frame_rewritten    = false;
    resume_userspace_session   = false;
    resume_userspace_exit_code = 0;

    Process* running = Scheduler::GetCurrentProcess();
    if (Userspace::IsActive() && running && running->is_user()) {
        Scheduler::SaveUserFrame(running, frame);
        // sync the linux current from the scheduler current - see the int 0x80
        // entry: an ap timer preempt switches threads without updating the
        // per-cpu linux index. (satoru)
        int ridx = find_process_index_by_task(running);
        if (ridx >= 0) LinuxSyscall::SetCurrent(ridx);
    }

    // SFMASK cleared IF at SYSCALL entry. re-enable ONLY on the aps (so an ap
    // inside a long handler still takes the tlb-shootdown ipi + timer). the bsp
    // x64 path historically ran with IF off through the whole syscall; keep that
    // on the bsp so device IRQ handlers can't re-enter a lock the syscall holds
    // (single-core self-deadlock / reentrancy). the int 0x80 path enables IF as
    // before. (satoru)
    if (SMP::CpuIndex() != 0) HAL::EnableInterrupts();
    // serialize the syscall body across cores; ring-3 stays parallel. (satoru)
    kls_lock();

    // amd64 syscall abi: nr in rax, args in rdi, rsi, rdx, r10, r8, r9. the
    // frame captured r9 pristine (musl's clone child-fn), so clone no longer
    // needs the g_user_syscall_*_save shim. (satoru)
    uint64_t _nr_dbg = frame->rax;
    int64_t result = SyscallEntryX64Handler(
        frame->rax,
        frame->rdi, frame->rsi, frame->rdx,
        frame->r10, frame->r8,  frame->r9);

    // (satoru) TEMP [eag]: log EAGAIN (-11) returns for firefox pids so we can see
    // which fd/syscall the sw_compositor WouldBlock-panics on. remove before commit.
    // (satoru) TEMP [eag]: trace clone(56) always + any -11/-12 (EAGAIN/ENOMEM)
    // return - the SwComposite thread::spawn fails when its stack mmap or clone
    // fails. logs nr/args/result. remove before commit.
    if (false) {   // gated off: 900 lines/run of pure serial drag (satoru)
        LinuxProcess* ep = LinuxSyscall::Current();
        bool interesting = (_nr_dbg == 56 || result == -11 || result == -12);
        // also trace mmap(9)/mprotect(10)/munmap(11) around the SwComposite spawn
        // to see the stack setup sequence + returned addresses. (satoru)
        bool mm = (_nr_dbg == 9 || _nr_dbg == 10 || _nr_dbg == 11) &&
                  ep && ep->pid == 103;
        if (interesting || mm) {
            static int neag = 0;
            if (neag++ < 900) {
                SerialLogger::Log("[eag] pid="); SerialLogger::LogDec(ep ? ep->pid : -1);
                SerialLogger::Log(" nr="); SerialLogger::LogDec((int)_nr_dbg);
                SerialLogger::Log(" a0="); SerialLogger::LogHex((uint32_t)frame->rdi);
                SerialLogger::Log(" a1="); SerialLogger::LogHex((uint32_t)frame->rsi);
                SerialLogger::Log(" a2="); SerialLogger::LogHex((uint32_t)frame->rdx);
                SerialLogger::Log(" a3="); SerialLogger::LogHex((uint32_t)frame->r10);
                SerialLogger::Log(" ret="); SerialLogger::LogHex((uint32_t)(uint64_t)result);
                SerialLogger::Log("\r\n");
            }
        }
    }

    if (!current_frame_rewritten) {
        frame->rax = (uint64_t)(int64_t)result;
        Process* current = Scheduler::GetCurrentProcess();
        if (Userspace::IsActive() && current && current->is_user()) {
            Scheduler::SaveUserFrame(current, frame);
        }
    }

    current_syscall_frame = nullptr;

    // release BEFORE a session unwind: HandleProcessExit longjmps away and
    // would leave the kls lock held forever. (satoru)
    kls_unlock();

    if (resume_userspace_session) {
        Userspace::HandleProcessExit(resume_userspace_exit_code);
    }

    HAL::DisableInterrupts();
}

// permanent, rate-limited enosys trace shared by the i386 dispatch default and
// the x64 translation miss. keeps a tiny ring of recently-seen numbers so a
// busy retry loop on one missing nr logs once, not thousands of times; a brand
// new number always logs. format is the strace-friendly "[kls] ENOSYS nr=<n>
// <name>" the audit recipe greps for. (satoru)
void LinuxSyscall::LogEnosys(uint64_t nr, const char* name) {
    static uint32_t seen[32] = {};
    static int      seen_head = 0;
    uint32_t key = (uint32_t)nr;
    for (int i = 0; i < 32; i++) {
        if (seen[i] == key + 1) return;  // already logged this nr (satoru)
    }
    seen[seen_head] = key + 1;           // +1 so a zeroed slot never matches nr 0 (satoru)
    seen_head = (seen_head + 1) & 31;
    SerialLogger::Log("[kls] ENOSYS nr=");
    SerialLogger::LogDec((int)nr);
    if (name && name[0]) {
        SerialLogger::Log(" ");
        SerialLogger::Log(name);
    }
    SerialLogger::Log("\r\n");
}

// headless syscall-abi self-test (gated by kurono.klstest). exercises a
// representative syscall from each tier through Dispatch and logs PASS/FAIL per
// check. runs in kernel context where identity-mapped low memory is directly
// dereferenceable, so user-pointer args can point at kernel stack buffers. (satoru)
int LinuxSyscall::SelfTest() {
    int fails = 0;
    auto check = [&](const char* name, bool ok) {
        SerialLogger::Log(ok ? "[klstest] PASS " : "[klstest] FAIL ");
        SerialLogger::Log(name);
        SerialLogger::Log("\r\n");
        if (!ok) fails++;
    };

    // stand up a throwaway linux process so Current() resolves and fd allocation
    // works; remember + restore the caller's current index. (satoru)
    int saved = GetCurrentIndex();
    int idx = CreateProcess("klstest", 4242, 4243);
    if (idx < 0) { check("create_test_process", false); return 1; }
    SetCurrent(idx);

    // ── Tier 9: identity. setuid/setgid then read back via the LinuxProcess. ──
    Dispatch(LSYS_SETUID_, 1234, 0, 0, 0, 0);
    Dispatch(LSYS_SETGID_, 5678, 0, 0, 0, 0);
    check("setuid_getuid", sys_getuid() == 1234);
    check("setgid_getgid", sys_getgid() == 5678);
    {
        uint32_t r = 0, e = 0, s = 0;
        Dispatch(LSYS_GETRESUID, (uint64_t)(uintptr_t)&r, (uint64_t)(uintptr_t)&e,
                 (uint64_t)(uintptr_t)&s, 0, 0);
        check("getresuid", r == 1234 && e == 1234);
        check("setfsuid_returns_prev", Dispatch(LSYS_SETFSUID, 99, 0, 0, 0, 0) == 1234);
        check("getgroups_zero", Dispatch(LSYS_GETGROUPS, 0, 0, 0, 0, 0) == 0);
    }

    // ── Tier 3: scheduler/priority. nice biasing is read/written on the backing
    //    kernel Process; this throwaway LinuxProcess has task==nullptr, so the
    //    handler correctly leaves nice at the default and getpriority reports
    //    20-0=20. (the nice round-trip itself is exercised by a real process.) ──
    {
        Dispatch(LSYS_SETPRIORITY, 0, 0, (uint64_t)(int64_t)5, 0, 0);  // no-op (no task) (satoru)
        int64_t gp = Dispatch(LSYS_GETPRIORITY, 0, 0, 0, 0, 0);
        check("getpriority_default_no_task", gp == 20);
        check("sched_prio_max_fifo", Dispatch(LSYS_SCHED_GET_PRIORITY_MAX, 1, 0, 0, 0, 0) == 99);
        check("sched_prio_min_normal", Dispatch(LSYS_SCHED_GET_PRIORITY_MIN, 0, 0, 0, 0, 0) == 0);
        check("sched_getscheduler_normal", Dispatch(LSYS_SCHED_GETSCHEDULER, 0, 0, 0, 0, 0) == 0);
        check("capget_zeroes", Dispatch(LSYS_CAPGET, 0, 0, 0, 0, 0) == 0);
    }

    // ── Tier 10: statfs fills a sane struct (f_type + f_namelen). ──
    {
        uint8_t sb[128];
        for (int i = 0; i < 128; i++) sb[i] = 0xAB;
        int64_t r = Dispatch(LSYS_STATFS_, 0, (uint64_t)(uintptr_t)sb, 0, 0, 0);
        uint64_t* w = (uint64_t*)sb;
        check("statfs_ok", r == 0 && w[1] == 4096 && *(uint64_t*)(sb + 56) == 255);
        check("sysfs_count", Dispatch(LSYS_SYSFS, 3, 0, 0, 0, 0) == 1);
    }

    // ── Tier 6: mincore marks pages resident; mlock/mlockall accept. ──
    {
        uint8_t vec[4] = {0, 0, 0, 0};
        int64_t r = Dispatch(LSYS_MINCORE, 0x20000000ULL, 4096 * 3,
                             (uint64_t)(uintptr_t)vec, 0, 0);
        check("mincore_resident", r == 0 && vec[0] == 1 && vec[2] == 1);
        check("mlockall_ok", Dispatch(LSYS_MLOCKALL, 0, 0, 0, 0, 0) == 0);
        check("get_mempolicy_default", Dispatch(LSYS_GET_MEMPOLICY, 0, 0, 0, 0, 0) == 0);
    }

    // ── Tier 4: xattr reports the right "absent/unsupported" errnos. ──
    check("getxattr_enodata", Dispatch(LSYS_GETXATTR, 0, 0, 0, 0, 0) == -61);
    check("setxattr_eopnotsupp", Dispatch(LSYS_SETXATTR, 0, 0, 0, 0, 0) == -95);
    check("listxattr_zero", Dispatch(LSYS_LISTXATTR, 0, 0, 0, 0, 0) == 0);

    // ── Tier 7: rt_sigpending reports the (empty) pending mask; kill self posts. ──
    {
        uint64_t set = 0xDEAD;
        Dispatch(LSYS_RT_SIGPENDING, (uint64_t)(uintptr_t)&set, 8, 0, 0, 0);
        check("rt_sigpending_empty", set == 0);
        check("pause_eintr", Dispatch(LSYS_PAUSE, 0, 0, 0, 0, 0) == -4);
    }

    // ── Tier 8: times returns a tick count; alarm reports no prior alarm. ──
    {
        uint64_t tms[4] = {1, 1, 1, 1};
        int64_t r = Dispatch(LSYS_TIMES, (uint64_t)(uintptr_t)tms, 0, 0, 0, 0);
        check("times_ok", r >= 0 && tms[2] == 0 && tms[3] == 0);
        check("alarm_zero", Dispatch(LSYS_ALARM, 5, 0, 0, 0, 0) == 0);
        check("getitimer_ok", Dispatch(LSYS_GETITIMER, 0, 0, 0, 0, 0) == 0);
    }

    // ── Tier 2/1: dup3 rejects oldfd==newfd; vmsplice consumes 0; fadvise ok. ──
    check("dup3_einval_same", Dispatch(LSYS_DUP3, 1, 1, 0, 0, 0) == -22);
    check("vmsplice_zero", Dispatch(LSYS_VMSPLICE, 0, 0, 0, 0, 0) == 0);
    check("posix_fadvise_ok", Dispatch(LSYS_POSIX_FADVISE, 0, 0, 0, 0, 0) == 0);
    check("seccomp_ok", Dispatch(LSYS_SECCOMP, 0, 0, 0, 0, 0) == 0);

    // ── Tier 11: ptrace(TRACEME) accepted; an unsupported request is ENOSYS. ──
    check("ptrace_traceme_ok", Dispatch(LSYS_PTRACE, 0, 0, 0, 0, 0) == 0);
    check("ptrace_attach_enosys", Dispatch(LSYS_PTRACE, 16 /*ATTACH*/, 0, 0, 0, 0) == -38);

    // ── Tier 6: mremap grow-with-move actually returns a new usable region. ──
    {
        int64_t base = sys_mmap(0, 4096, 0x3, 0x22, -1, 0);  // anon rw (satoru)
        if (base > 0) {
            *(volatile uint32_t*)(uintptr_t)base = 0xCAFEF00D;
            int64_t grown = Dispatch(LSYS_MREMAP, (uint64_t)base, 4096, 8192,
                                     1 /*MAYMOVE*/, 0);
            bool ok = grown > 0 &&
                      *(volatile uint32_t*)(uintptr_t)grown == 0xCAFEF00D;
            check("mremap_grow_copies", ok);
            if (grown > 0) sys_munmap((uintptr_t)grown, 8192);
        } else {
            check("mremap_grow_copies(skipped-no-mmap)", true);
        }
    }

    // tear down the throwaway process + restore the caller's current index. (satoru)
    DestroyProcess(idx);
    SetCurrent(saved);

    SerialLogger::Log("[klstest] ");
    SerialLogger::LogDec(fails);
    SerialLogger::Log(" FAILED\r\n");
    return fails;
}

//  init

void LinuxSyscall::Init() {
    for (int i = 0; i < LINUX_MAX_PROCS; i++) {
        procs[i].active = false;
        procs[i].pid = 0;
    }
    for (uint32_t c = 0; c < SMP_MAX_CPUS; c++) current_proc_cpu[c] = -1;  // none, every cpu (satoru)
    console_head = 0;
    console_tail = 0;
    stdin_head = 0;
    stdin_tail = 0;
    HAL::RegisterSystemCallHandler(LinuxInt80Entry);
    SerialLogger::Log("[LinuxSyscall] Initialized\r\n");
}

bool LinuxSyscall::HandlePageFault(InterruptFrame* frame) {
    if (!frame) return false;

    Process* task = Scheduler::GetCurrentProcess();
    if (!task || !task->is_user()) return false;

    // smp: demand-zero/cow mutate the shared region table + page tables + pmm,
    // racing sibling threads' mmap/munmap syscalls on other cores. take the kls
    // lock like a syscall body; recursion-safe when the faulting access was the
    // kernel's own (a syscall already holding the lock touched lazy user
    // memory). raii so every return path below releases. (satoru)
    struct KlsGuard {
        KlsGuard()  { kls_lock(); }
        ~KlsGuard() { kls_unlock(); }
    } _kls_guard;

    // A KERNEL-mode fault (PFERR_USER clear) on a user address occurs when the
    // kernel itself touches not-yet-faulted demand-zero user memory on behalf of a
    // syscall -- e.g. mremap's memcpy into the freshly (lazily) mmap'd destination
    // region, which otherwise panics the whole kernel. Honour it ONLY when a
    // registered user region covers cr2; a genuine kernel bug faults outside any
    // region and still returns false here (-> panics, as before). (satoru)
    if (!(frame->error_code & PFERR_USER) && !find_region(task, frame->cr2)) {
        return false;
    }

    uint64_t page_base = align_down_u64(frame->cr2, PAGE_SIZE);
    uint64_t page_flags = KernelVMM::QueryPageFlagsInAddressSpace(task->address_space, page_base);

    if (frame->error_code & PFERR_PRESENT) {
        // Standard COW path first.
        if (handle_cow_fault(task, page_base, page_flags, frame)) return true;

        // pte-authoritative stale-tlb retry: if the LIVE pte already permits
        // exactly this access (user + writable for a write), the mapping is
        // fine and the fault can only be this core's stale tlb entry - retry
        // after invlpg even when find_region says nothing (the region table
        // is mutated by sibling threads' mmap/munmap on other cores and can
        // transiently miss an address whose pte is long since valid; observed
        // as a fatal ring-3 present-write #PF err=0x7 on an AP that killed
        // the boot). bounded per-cpu so a genuinely wrong pte still goes
        // fatal instead of looping. (satoru)
        if ((frame->error_code & PFERR_USER) && (page_flags & PTE_USER) &&
            (!(frame->error_code & PFERR_WRITE) || (page_flags & PTE_WRITABLE))) {
            static uint64_t s_pf_retry_va[SMP_MAX_CPUS];
            static uint32_t s_pf_retry_n[SMP_MAX_CPUS];
            uint32_t rcpu = SMP::CpuIndex();
            if (rcpu >= SMP_MAX_CPUS) rcpu = 0;
            if (s_pf_retry_va[rcpu] != page_base) {
                s_pf_retry_va[rcpu] = page_base;
                s_pf_retry_n[rcpu]  = 0;
            }
            if (s_pf_retry_n[rcpu]++ < 8) {
                KernelVMM::InvalidatePage(page_base);
                if (frame->error_code & PFERR_USER) Scheduler::SaveUserFrame(task, frame);
                return true;
            }
        }

        // Otherwise: a user-mode access hit a present mapping that
        // didn't have PTE_USER set (typically the kernel identity map
        // covering the brk/mmap window in low physical memory).  If we
        // have a registered user region covering this address, promote
        // by unmapping the supervisor PTE and falling through to the
        // demand-zero allocator below.
        UserMemoryRegion* r = find_region(task, frame->cr2);
        if (!r) {
            // fatal refusal: say WHY on the way out so the [RAWEXC] boots
            // self-identify (no region vs region-not-writable). (satoru)
            SerialLogger::Log("[pfref] present fault, no region. pteflags=");
            SerialLogger::LogHex((uint32_t)page_flags);
            SerialLogger::Log("\r\n");
            return false;
        }
        if (!(page_flags & PTE_USER)) {
            KernelVMM::UnmapPageInAddressSpace(task->address_space, page_base, false);
            KernelVMM::InvalidatePage(page_base);
            return handle_demand_zero_fault(task, r, page_base, frame);
        }
        // smp coherency: a write faulted on a PRESENT user page whose REGION is
        // writable. either this core's TLB is stale (a sibling core faulted the
        // page in RW after this core cached a read-only/older entry) or the live
        // PTE lags the region's protection. re-apply the region's flags to the
        // PTE and flush this core's TLB, then retry the write. bounded: the retry
        // finds a RW PTE and succeeds (no fault loop). a genuine write to a
        // READ-ONLY region (relro, ro mmap) has !(page_flags&WRITABLE) on the
        // region and still SIGSEGVs below. this was the mallocng "write to RO
        // present page" #PF that crashed firefox on an AP once a real profile
        // drove multi-core browser startup. (satoru)
        if ((frame->error_code & PFERR_WRITE) && (r->page_flags & PTE_WRITABLE)) {
            KernelVMM::ProtectPageInAddressSpace(task->address_space, page_base, r->page_flags);
            KernelVMM::InvalidatePage(page_base);
            if (frame->error_code & PFERR_USER) Scheduler::SaveUserFrame(task, frame);
            return true;
        }
        SerialLogger::Log("[pfref] present fault refused. pteflags=");
        SerialLogger::LogHex((uint32_t)page_flags);
        SerialLogger::Log(" regflags=");
        SerialLogger::LogHex((uint32_t)r->page_flags);
        SerialLogger::Log("\r\n");
        return false;
    }

    UserMemoryRegion* region = find_region(task, frame->cr2);
    if (!region) {
        // (satoru) STACK AUTO-GROW. the flaky "firefox stalls before window-create"
        // was a pthread's InitCommon pushing at RSP-8 and #PF'ing with NO registered
        // region -> SIGSEGV killed the thread. under -smp the leader's region table
        // for a fresh thread stack can lag the core that's already running the thread,
        // or musl runs slightly past a stack the leader hasn't finished registering.
        // mirror linux's expand_stack: if this is a user not-present WRITE within a
        // stack's reach of the trap RSP, register a demand-zero page and fault it in.
        // a wild write far from RSP still finds no region here and SIGSEGVs. (satoru)
        if ((frame->error_code & PFERR_USER) &&
            (frame->error_code & PFERR_WRITE) &&
            !(frame->error_code & PFERR_PRESENT)) {
            uint64_t ursp = frame->rsp;
            // plausible stack access: at/just-above rsp (a push crossing a page) down
            // to 128kb below it. map the ONE faulting page DIRECTLY - do NOT add_region:
            // a whole missing thread-stack faults page-by-page and an add_region per page
            // would bloat the 4096-slot table + slow the linear find_region, itself
            // stalling startup. one page per fault, no bookkeeping. (satoru)
            if (frame->cr2 + 0x20000ull >= ursp && frame->cr2 <= ursp + 0x1000ull) {
                void* pg = PMM::AllocBytes(PAGE_SIZE);
                if (pg) {
                    memset(pg, 0, PAGE_SIZE);
                    uint64_t pflags = page_flags_from_prot(0x1 | 0x2);  // PROT_READ|PROT_WRITE (satoru)
                    if (KernelVMM::MapPageInAddressSpace(task->address_space, page_base,
                                                         (uint64_t)(uintptr_t)pg, pflags)) {
                        KernelVMM::InvalidatePage(page_base);
                        if (frame->error_code & PFERR_USER) Scheduler::SaveUserFrame(task, frame);
                        return true;
                    }
                    PMM::FreeBytes(pg, PAGE_SIZE);
                }
            }
        }
        return false;
    }

    {
        bool hz = handle_demand_zero_fault(task, region, page_base, frame);
        // (satoru) TEMP [pfr]: region EXISTS but the fault was not handled - dump
        // the region's flags (the present-RO write crash lands here if the loader
        // mapped a data page read-only). capped. remove before commit.
        if (false && !hz) {
            static int npfr2 = 0;
            if (npfr2 < 20) {
                npfr2++;
                LinuxProcess* fp = Current();
                SerialLogger::Log("[pfr] pid="); SerialLogger::LogDec(fp ? fp->pid : -1);
                SerialLogger::Log(" UNHANDLED cr2=");
                SerialLogger::LogHex((uint32_t)(frame->cr2 >> 32));
                SerialLogger::Log(":"); SerialLogger::LogHex((uint32_t)frame->cr2);
                SerialLogger::Log(" ec="); SerialLogger::LogDec((int)frame->error_code);
                SerialLogger::Log(" reg=");
                SerialLogger::LogHex((uint32_t)(region->start >> 32));
                SerialLogger::Log(":"); SerialLogger::LogHex((uint32_t)region->start);
                SerialLogger::Log("-"); SerialLogger::LogHex((uint32_t)region->end);
                SerialLogger::Log(" pf="); SerialLogger::LogHex((uint32_t)region->page_flags);
                SerialLogger::Log(" fl="); SerialLogger::LogHex((uint32_t)region->flags);
                SerialLogger::Log("\r\n");
            }
        }
        return hz;
    }
}

//  process management

int LinuxSyscall::CreateProcess(const char* name, uint32_t uid, uint32_t gid) {
    for (int i = 0; i < LINUX_MAX_PROCS; i++) {
        if (!procs[i].active) {
            LinuxProcess* p = &procs[i];
            memset(p, 0, sizeof(LinuxProcess));
            // heap-allocate this slot's fd table (the struct now holds a pointer, not
            // an inline array). a CLONE_FILES thread re-points this at its leader's
            // table below so threads share fds. NOTE: not freed on exit yet -- a
            // small bounded leak; refcounted free is a follow-up. (satoru)
            p->fds = (LinuxFd*)KernelHeap::Alloc(sizeof(LinuxFd) * LINUX_MAX_FDS);
            if (!p->fds) return -1;
            memset(p->fds, 0, sizeof(LinuxFd) * LINUX_MAX_FDS);
            p->pid = (uint32_t)(i + 100);  // linux pids start at 100
            LinuxProcess* parent = Current();
            p->ppid = parent ? parent->pid : 1;
            p->uid = uid;
            p->gid = gid;
            p->euid = uid;
            p->egid = gid;
            ls_scpy(p->cwd, "/", sizeof(p->cwd));
            ls_scpy(p->name, name, sizeof(p->name));
            p->brk_base = LINUX_BRK_INITIAL;
            p->brk_current = LINUX_BRK_INITIAL;
            p->brk_max = LINUX_BRK_MAX;
            p->active = true;
            p->exited = false;
            p->exit_code = -1;
            p->task = nullptr;
            p->signal_mask = 0;
            p->pending_signals = 0;

            // session/pgrp: child inherits from parent; otherwise it's its own
            // session leader so getsid()/getpgid() return real values.
            if (parent) {
                p->sid  = parent->sid  ? parent->sid  : p->pid;
                p->pgid = parent->pgid ? parent->pgid : p->pid;
            } else {
                p->sid  = p->pid;
                p->pgid = p->pid;
            }
            p->ctty_pgrp = (int)p->pgid;
            p->is_session_leader = (p->sid == p->pid);

            // initialize file descriptors
            for (int j = 0; j < LINUX_MAX_FDS; j++)
                p->fds[j].open = false;
            InitStdFds(p);

            SerialLogger::Log("[LinuxSyscall] Created process: ");
            SerialLogger::Log(name);
            SerialLogger::Log(" pid=");
            SerialLogger::LogDec((int)p->pid);
            SerialLogger::Log("\r\n");

            char detail[64];
            char num[24];
            detail[0] = 0;
            ls_cat(detail, "uid=", sizeof(detail));
            ls_itoa((int)uid, num, 10);
            ls_cat(detail, num, sizeof(detail));
            ls_cat(detail, " gid=", sizeof(detail));
            ls_itoa((int)gid, num, 10);
            ls_cat(detail, num, sizeof(detail));
            RuntimeLog::LogProcessEvent(p->name, (int)p->pid, "created", detail);

            return i;
        }
    }
    return -1;
}

void LinuxSyscall::DestroyProcess(int pid_idx) {
    if (pid_idx < 0 || pid_idx >= LINUX_MAX_PROCS) return;
    LinuxProcess* p = &procs[pid_idx];

    char detail[64];
    char num[24];
    detail[0] = 0;
    ls_cat(detail, "exit_code=", sizeof(detail));
    ls_itoa(p->exit_code, num, 10);
    ls_cat(detail, num, sizeof(detail));
    RuntimeLog::LogProcessEvent(p->name, (int)p->pid, "destroyed", detail);

    // close all fds
    for (int i = 0; i < LINUX_MAX_FDS; i++) {
        if (p->fds[i].open) {
            if (p->fds[i].type == LFD_EXT4)
                Ext4::Close(p->fds[i].backend_fd);
            // kvfs fds closed similarly
            p->fds[i].open = false;
        }
    }
    p->task = nullptr;
    p->exited = false;
    p->active = false;
    if (current_proc == pid_idx) current_proc = -1;
}

LinuxProcess* LinuxSyscall::GetProcess(int pid_idx) {
    if (pid_idx < 0 || pid_idx >= LINUX_MAX_PROCS) return nullptr;
    return procs[pid_idx].active ? &procs[pid_idx] : nullptr;
}

LinuxProcess* LinuxSyscall::Current() {
    if (current_proc < 0 || current_proc >= LINUX_MAX_PROCS) return nullptr;
    return procs[current_proc].active ? &procs[current_proc] : nullptr;
}

int LinuxSyscall::GetCurrentIndex() {
    return current_proc;
}

void LinuxSyscall::SetCurrent(int pid_idx) {
    current_proc = pid_idx;
}

void LinuxSyscall::SyncCurrentToTask(Process* task) {
    int idx = find_process_index_by_task(task);
    if (idx >= 0) current_proc = idx;   // per-cpu (macro) (satoru)
}

// registered once at boot; turns on round-robin preemption of user threads. the
// static kls_timer_preempt handler lives earlier in this TU (internal linkage,
// still visible here). (satoru)
// (satoru) AP-side futex maintenance: the aps' LAPIC-timer preempt
// (Scheduler::ApTimerPreempt) calls this to run the linux futex repoll/timeout
// heal, so AP-parked render/rayon threads are released even while the bsp main
// thread is itself blocked. same ring-3-only safety as kls_timer_preempt.
// (satoru)
static void kls_ap_futex_maint() {
    futex_sweep_timeouts();
}

void LinuxSyscall::EnableTimerPreemption() {
    HAL::RegisterIRQHandler(0, kls_timer_preempt);
    Scheduler::SetApFutexMaintHook(kls_ap_futex_maint);
}

int LinuxSyscall::ActiveProcessCount() {
    int n = 0;
    for (int i = 0; i < LINUX_MAX_PROCS; i++)
        if (procs[i].active) n++;
    return n;
}

int LinuxSyscall::AllocFd(LinuxProcess* p) {
    for (int i = 0; i < LINUX_MAX_FDS; i++) {
        if (!p->fds[i].open) return i;
    }
    return -1;
}

void LinuxSyscall::InitStdFds(LinuxProcess* p) {
    // stdin
    p->fds[0].type = LFD_CONSOLE;
    p->fds[0].backend_fd = 0;
    ls_scpy(p->fds[0].path, "/dev/stdin", sizeof(p->fds[0].path));
    p->fds[0].flags = L_O_RDONLY;
    p->fds[0].offset = 0;
    p->fds[0].open = true;

    // stdout
    p->fds[1].type = LFD_CONSOLE;
    p->fds[1].backend_fd = 1;
    ls_scpy(p->fds[1].path, "/dev/stdout", sizeof(p->fds[1].path));
    p->fds[1].flags = L_O_WRONLY;
    p->fds[1].offset = 0;
    p->fds[1].open = true;

    // stderr
    p->fds[2].type = LFD_CONSOLE;
    p->fds[2].backend_fd = 2;
    ls_scpy(p->fds[2].path, "/dev/stderr", sizeof(p->fds[2].path));
    p->fds[2].flags = L_O_WRONLY;
    p->fds[2].offset = 0;
    p->fds[2].open = true;
}

// linux paths get translated to kurono paths transparently:
//   /proc/...     -> /system/proc/...
//   /dev/...      -> /system/dev/...
//   /etc/...      -> /system/etc/...
//   /tmp/...      -> /system/tmp/...
//   /run/...      -> /system/run/...
//   /var/log/...  -> /system/log/...
//   /var/...      -> /system/var/...
//   /usr/lib/...  -> /system/lib/...
//   /usr/lib64/.. -> /system/lib/...
//   /usr/bin/...  -> /system/bin/...
//   /usr/share/.. -> /system/share/...
//   /lib/...      -> /system/lib/...
//   /lib64/...    -> /system/lib/...
//   /sbin/...     -> /system/bin/...
//   /bin/...      -> /system/bin/...
//   /sys/...      -> /system/sys/...
//   /home/...     -> passthrough (/home/user is shared)
//   /apps/...     -> passthrough
//   /system/...   -> passthrough
//   /linux/...    -> passthrough (legacy debian rootfs)
// translation is invisible to the caller - they pass the linux path they
// expect, kurono serves from the kurono path.

void LinuxSyscall::ResolvePath(const char* linux_path, char* kurono_path,
                                int max_len, LinuxProcess* p) {
    char abs[256];

    if (linux_path[0] != '/') {
        // relative path - prepend cwd. guard against an empty cwd (ls_slen==0
        // would index abs[-1]). (satoru)
        ls_scpy(abs, p->cwd, sizeof(abs));
        int alen = ls_slen(abs);
        if (alen == 0 || abs[alen - 1] != '/') ls_cat(abs, "/", sizeof(abs));
        ls_cat(abs, linux_path, sizeof(abs));
    } else {
        ls_scpy(abs, linux_path, sizeof(abs));
    }

    // table of (linux_prefix, kurono_prefix) substitutions.
    // ordering matters - longer prefixes must come first so /usr/lib64
    // matches before /usr.
    struct Xlate { const char* lin; const char* kur; };
    static const Xlate table[] = {
        { "/usr/lib64/",   "/system/lib/" },
        { "/usr/lib/",     "/system/lib/" },
        { "/usr/libexec/", "/system/libexec/" },
        { "/usr/bin/",     "/system/bin/" },
        { "/usr/sbin/",    "/system/bin/" },
        { "/usr/share/",   "/system/share/" },
        { "/usr/include/", "/system/include/" },
        { "/usr/local/",   "/system/local/" },
        { "/var/log/",     "/system/log/" },
        { "/var/run/",     "/system/run/" },
        { "/var/tmp/",     "/system/tmp/" },
        { "/var/cache/",   "/system/var/cache/" },
        { "/var/lib/",     "/system/var/lib/" },
        { "/var/spool/",   "/system/var/spool/" },
        { "/var/",         "/system/var/" },
        { "/etc/",         "/system/etc/" },
        { "/proc/",        "/system/proc/" },
        { "/proc",         "/system/proc" },
        { "/dev/",         "/system/dev/" },
        { "/dev",          "/system/dev" },
        { "/sys/",         "/system/sys/" },
        { "/sys",          "/system/sys" },
        { "/run/",         "/system/run/" },
        { "/tmp/",         "/system/tmp/" },
        { "/tmp",          "/system/tmp" },
        { "/lib64/",       "/system/lib/" },
        { "/lib/",         "/system/lib/" },
        { "/sbin/",        "/system/bin/" },
        { "/bin/",         "/system/bin/" },
        { nullptr, nullptr }
    };

    // Pass-through prefixes - these are already kurono-native.
    if (ls_starts(abs, "/system/") || ls_starts(abs, "/home/") ||
        ls_starts(abs, "/apps/")   || ls_starts(abs, "/linux/") ||
        ls_starts(abs, "/boot/")) {
        ls_scpy(kurono_path, abs, max_len);
        return;
    }

    for (int i = 0; table[i].lin; i++) {
        if (ls_starts(abs, table[i].lin)) {
            int lin_len = ls_slen(table[i].lin);
            ls_scpy(kurono_path, table[i].kur, max_len);
            // Append the suffix after the matched prefix.
            int klen = ls_slen(kurono_path);
            int j = lin_len;
            while (abs[j] && klen < max_len - 1) {
                kurono_path[klen++] = abs[j++];
            }
            kurono_path[klen] = 0;
            return;
        }
    }

    // default: pass through unchanged
    ls_scpy(kurono_path, abs, max_len);
}

//  syscall dispatcher

int64_t LinuxSyscall::Dispatch(uint64_t eax, uint64_t ebx, uint64_t ecx,
                                uint64_t edx, uint64_t esi, uint64_t edi) {
    (void)esi; (void)edi;

    if (SMP::CpuIndex() == 0) KuronoShell::PumpUI();   // ui pump is bsp-only (satoru)

    switch (eax) {
        case LSYS_EXIT:        return sys_exit(ebx);
        case LSYS_FORK:        return sys_fork();
        case LSYS_READ:        return sys_read((int)ebx, ecx, edx);
        case LSYS_WRITE:       return sys_write((int)ebx, ecx, edx);
        case LSYS_OPEN:        return sys_open(ebx, ecx, edx);
        case LSYS_CLOSE:       return sys_close((int)ebx);
        case LSYS_WAITPID:     return sys_waitpid(ebx, ecx, edx);
        case LSYS_EXECVE:      return sys_execve(ebx, ecx, edx);
        case LSYS_LSEEK:       return sys_lseek((int)ebx, (int32_t)ecx, edx);
        case LSYS_BRK:         return sys_brk(ebx);
        case LSYS_GETPID:      return sys_getpid();
        case LSYS_GETUID:      return sys_getuid();
        case LSYS_GETGID:      return sys_getgid();
        case LSYS_GETEUID:     return sys_geteuid();
        case LSYS_GETEGID:     return sys_getegid();
        case LSYS_GETPPID:     return sys_getppid();
        case LSYS_STAT:        return sys_stat(ebx, ecx);
        case LSYS_FSTAT:       return sys_fstat((int)ebx, ecx);
        case LSYS_UNAME:       return sys_uname(ebx);
        case LSYS_GETCWD:      return sys_getcwd(ebx, ecx);
        case LSYS_CHDIR:       return sys_chdir(ebx);
        case LSYS_MKDIR:       return sys_mkdir(ebx, ecx);
        case LSYS_RMDIR:       return sys_rmdir(ebx);
        case LSYS_UNLINK:      return sys_unlink(ebx);
        case LSYS_ACCESS:      return sys_access(ebx, ecx);
        case LSYS_DUP:         return sys_dup((int)ebx);
        case LSYS_DUP2:        return sys_dup2((int)ebx, (int)ecx);
        case LSYS_IOCTL:       return sys_ioctl((int)ebx, (uint32_t)ecx, edx);
        case LSYS_WRITEV:      return sys_writev((int)ebx, ecx, edx);
        case LSYS_MMAP:        return sys_mmap(ebx, ecx, edx, esi, (int)edi, 0);
        case LSYS_MUNMAP:      return sys_munmap(ebx, ecx);
        case LSYS_NANOSLEEP:   return sys_nanosleep(ebx, ecx);
        case LSYS_GETDENTS64:  return sys_getdents64((int)ebx, ecx, edx);
        case LSYS_CLOCK_GETTIME: return sys_clock_gettime(ebx, ecx);
        case LSYS_SET_THREAD_AREA: return sys_set_thread_area(ebx);
        case LSYS_EXIT_GROUP:  return sys_exit_group(ebx);

        // mprotect: real - flip page-table perms so w^x jits (rw->rx) work (satoru)
        case LSYS_MPROTECT:    return sys_mprotect(ebx, ecx, (uint32_t)edx);

        // stubs that return success
        case LSYS_SIGNAL:
        case LSYS_SIGACTION:
        case LSYS_SYNC:
            return 0;

        // session / process group (real, backed by LinuxProcess fields)
        case LSYS_SETSID: {
            LinuxProcess* p = Current();
            if (!p) return -1;
            // setsid() fails if caller is already a process-group leader
            if (p->pgid == p->pid && p->is_session_leader && p->sid == p->pid) {
                // already a session leader
            }
            p->sid = p->pid;
            p->pgid = p->pid;
            p->is_session_leader = true;
            p->ctty_pgrp = -1;  // no controlling terminal after setsid
            return (int32_t)p->sid;
        }
        case LSYS_GETPGRP: {
            LinuxProcess* p = Current();
            return p ? (int32_t)p->pgid : -1;
        }
        case LSYS_GETPGID: {
            int target = (int)ebx;
            if (target == 0){
                LinuxProcess* p = Current();
                return p ? (int32_t)p->pgid : -1;
            }
            for (int i = 0; i < LINUX_MAX_PROCS; i++)
                if (procs[i].active && (int)procs[i].pid == target)
                    return (int32_t)procs[i].pgid;
            return -3;  // ESRCH
        }
        case LSYS_SETPGID: {
            int target_pid = (int)ebx;
            int new_pgid   = (int)ecx;
            LinuxProcess* p = nullptr;
            if (target_pid == 0) p = Current();
            else for (int i = 0; i < LINUX_MAX_PROCS; i++)
                if (procs[i].active && (int)procs[i].pid == target_pid){ p = &procs[i]; break; }
            if (!p) return -3;
            if (new_pgid == 0) new_pgid = (int)p->pid;
            p->pgid = (uint32_t)new_pgid;
            return 0;
        }
        case LSYS_GETSID: {
            int target = (int)ebx;
            if (target == 0){
                LinuxProcess* p = Current();
                return p ? (int32_t)p->sid : -1;
            }
            for (int i = 0; i < LINUX_MAX_PROCS; i++)
                if (procs[i].active && (int)procs[i].pid == target)
                    return (int32_t)procs[i].sid;
            return -3;
        }

        // thread / process metadata. the per-thread id is the scheduler pid of
        // the currently running task (distinct for each clone-thread); only the
        // thread-group leader has tid == pid. (satoru)
        case LSYS_GETTID: {
            Process* t = Scheduler::GetCurrentProcess();
            if (t && t->is_user()) return (int32_t)t->pid;
            return sys_getpid();
        }
        // set_tid_address(tidptr): record the clear-on-exit tid pointer for the
        // current task and return its tid (glibc nptl uses this on the main
        // thread). (satoru)
        case LSYS_SET_TID_ADDRESS: {
            Process* t = Scheduler::GetCurrentProcess();
            if (t && t->is_user()) {
                t->clear_child_tid = ebx;
                return (int32_t)t->pid;
            }
            return sys_getpid();
        }
        case LSYS_TGKILL: {
            // tgkill(tgid, tid, sig) - we map to plain kill(tid, sig)
            int tid = (int)ecx; int sig = (int)edx;
            for (int i = 0; i < LINUX_MAX_PROCS; i++)
                if (procs[i].active && (int)procs[i].pid == tid){
                    procs[i].pending_signals |= (1u << sig);
                    return 0;
                }
            return -3;
        }
        case LSYS_PRCTL: {
            // PR_SET_NAME = 15, PR_GET_NAME = 16
            int op = (int)ebx;
            LinuxProcess* p = Current();
            if (!p) return -1;
            if (op == 15 && ecx){
                // copy up to 16 bytes from user space (ecx is a pointer)
                const char* src = (const char*)(uintptr_t)ecx;
                int i = 0;
                while (i < (int)sizeof(p->name)-1 && src[i]) { p->name[i] = src[i]; i++; }
                p->name[i] = 0;
                return 0;
            }
            if (op == 16 && ecx){
                char* dst = (char*)(uintptr_t)ecx;
                int i = 0;
                while (i < 15 && p->name[i]) { dst[i] = p->name[i]; i++; }
                dst[i] = 0;
                return 0;
            }
            return 0;
        }
        case LSYS_SYSINFO: {
            // struct sysinfo: uptime, loads[3], totalram, freeram, sharedram,
            // bufferram, totalswap, freeswap, procs, pad, totalhigh, freehigh, mem_unit
            if (!ebx) return -14;
            uint32_t* si = (uint32_t*)(uintptr_t)ebx;
            si[0] = Time::GetTicks() / 1000;
            si[1] = si[2] = si[3] = 0;          // load averages
            si[4] = (uint32_t)(10ull * 1024 * 1024 * 1024 / 4096);  // 10 GB / mem_unit
            si[5] = si[4] / 2;                  // freeram (rough)
            si[6] = si[7] = 0;
            si[8] = si[9] = 0;
            si[10] = (uint32_t)ActiveProcessCount();
            si[11] = si[12] = si[13] = 0;
            si[14] = 4096;                      // mem_unit
            return 0;
        }
        case LSYS_GETRLIMIT:
        case LSYS_SETRLIMIT:
        case LSYS_PRLIMIT64: {
            // Provide sane fixed limits.  Real implementation would track
            // per-process limits; for now we expose generous defaults.
            uint32_t* rl = nullptr;
            uint64_t resource = 0xFFFFFFFFu;
            if (eax == LSYS_GETRLIMIT)      { rl = (uint32_t*)(uintptr_t)ecx; resource = ebx; }
            else if (eax == LSYS_PRLIMIT64) { rl = (uint32_t*)(uintptr_t)esi; resource = ecx; }
            if (rl){
                // RLIMIT_STACK(3): firefox/musl derive the MAIN-THREAD stack SIZE from this.
                // returning RLIM_INFINITY (the generous default below) gave them no usable
                // size, so they fell back to discovering the stack extent with a per-page
                // mremap(p,PAGE,2*PAGE) walk of the whole 8MB stack -- thousands of syscalls
                // that dominated cpu and wedged firefox at nsWindow::Create. report the real
                // fixed kurono user stack (8MB, = USER_STACK_TOP-8MB..top) so they use it
                // directly and skip the walk. (satoru)
                if (resource == 3) {
                    ((uint64_t*)rl)[0] = 8ULL * 1024 * 1024;   // rlim_cur = 8MB
                    ((uint64_t*)rl)[1] = 8ULL * 1024 * 1024;   // rlim_max = 8MB
                } else {
                    // rlim_cur, rlim_max as 64-bit (generous "unlimited"-ish defaults)
                    rl[0] = 0xFFFFFFFFu; rl[1] = 0x7FFFFFFFu;
                    rl[2] = 0xFFFFFFFFu; rl[3] = 0x7FFFFFFFu;
                }
            }
            return 0;
        }
        // personality / capget / capset are now handled in the Tier-3 build-out
        // block below (same LSYS_ ids, more spec-correct: capget zeroes the user
        // data struct, the stubs here returned 0 without touching it). (satoru)
        case LSYS_FTRUNCATE: {
            // size a memfd's backing; non-memfd fds grow on write (kvfs). (satoru)
            LinuxProcess* p = Current();
            LinuxShmObj* s = shm_for_fd(p, (int)ebx);
            if (s) return shm_set_size(s, (uint64_t)ecx) ? 0 : -28;  // -ENOSPC
            return 0;
        }
        case LSYS_MADVISE:    return sys_madvise(ebx, ecx, (uint32_t)edx);
        case LSYS_FSYNC:
        case LSYS_FDATASYNC:
        case LSYS_MSYNC:
            return 0;
        // dup3 is handled in the Tier-2 build-out block below (adds the
        // oldfd==newfd -> EINVAL rule the bare dup2 forward here skipped). (satoru)
        case LSYS_PIPE:
        case LSYS_PIPE2: {
            // back a pipe with a connected unix-socket pair: read/write already
            // route through LFD_SOCKET, and write(fd[1]) lands in the peer's rx
            // for read(fd[0]). O_CLOEXEC/O_NONBLOCK (ecx on pipe2) are accepted
            // but not yet enforced. needed for firefox/glibc ipc. (satoru)
            LinuxProcess* p = Current();
            if (!p) return -1;
            uint32_t* ufds = (uint32_t*)(uintptr_t)ebx;   // int pipefd[2]
            if (!ufds) return -14;                        // EFAULT
            int sd0 = -1, sd1 = -1;
            UnixSocket::Pair(UnixSocket::UNIX_SOCK_STREAM, &sd0, &sd1);
            if (sd0 < 0 || sd1 < 0) return -24;           // EMFILE
            int rfd = AllocFd(p);
            if (rfd < 0) { UnixSocket::Close(sd0); UnixSocket::Close(sd1); return -24; }
            memset(&p->fds[rfd], 0, sizeof(LinuxFd));
            p->fds[rfd].type = LFD_SOCKET;
            p->fds[rfd].backend_fd = sd0;
            p->fds[rfd].open = true;
            int wfd = AllocFd(p);
            if (wfd < 0) {
                p->fds[rfd].open = false;
                UnixSocket::Close(sd0); UnixSocket::Close(sd1);
                return -24;
            }
            memset(&p->fds[wfd], 0, sizeof(LinuxFd));
            p->fds[wfd].type = LFD_SOCKET;
            p->fds[wfd].backend_fd = sd1;
            p->fds[wfd].open = true;
            ufds[0] = (uint32_t)rfd;   // read end
            ufds[1] = (uint32_t)wfd;   // write end
            return 0;
        }
        case LSYS_PREAD64: {
            // pread64(fd, buf, count, offset) - emulate by lseek+read
            int fd = (int)ebx;
            uintptr_t buf = (uintptr_t)ecx;  // 64-bit user buffer (satoru)
            uint64_t cnt = edx;
            uint32_t off = (uint32_t)esi;
            uint32_t old = sys_lseek(fd, 0, 1);  // SEEK_CUR
            sys_lseek(fd, (int32_t)off, 0);      // SEEK_SET
            int32_t r = sys_read(fd, buf, cnt);
            sys_lseek(fd, (int32_t)old, 0);
            return r;
        }
        case LSYS_PWRITE64: {
            int fd = (int)ebx;
            uintptr_t buf = (uintptr_t)ecx;  // 64-bit user buffer (satoru)
            uint64_t cnt = edx;
            uint32_t off = (uint32_t)esi;
            uint32_t old = sys_lseek(fd, 0, 1);
            sys_lseek(fd, (int32_t)off, 0);
            int32_t r = sys_write(fd, buf, cnt);
            sys_lseek(fd, (int32_t)old, 0);
            return r;
        }
        case LSYS_REBOOT: {
            // 0xfee1dead, 672274793, cmd, arg
            uint32_t cmd = edx;
            // LINUX_REBOOT_CMD_RESTART = 0x01234567
            // LINUX_REBOOT_CMD_HALT    = 0xCDEF0123
            // LINUX_REBOOT_CMD_POWER_OFF = 0x4321FEDC
            if (cmd == 0x01234567u){
                // 8042 reset
                __asm__ __volatile__("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
            }
            // halt
            __asm__ __volatile__("cli; hlt");
            while (1) __asm__ __volatile__("hlt");
            return 0;
        }
        case LSYS_SYSLOG: {
            // type 3 = SYSLOG_ACTION_READ_ALL: copy kernel ring buffer
            // type 10 = SYSLOG_ACTION_SIZE_BUFFER
            int type = (int)ebx;
            if (type == 10) return 16384;
            if (type == 3 && ecx && edx > 0){
                // We have no formal dmesg ring; return empty for now.
                char* dst = (char*)(uintptr_t)ecx;
                if (dst && edx > 0) dst[0] = 0;
                return 0;
            }
            return 0;
        }
        case LSYS_CLOCK_GETRES: {
            // Return 1 ns resolution
            if (ecx){
                uint32_t* ts = (uint32_t*)(uintptr_t)ecx;
                ts[0] = 0; ts[1] = 1;
            }
            return 0;
        }

        // openat / mkdirat / unlinkat / fstatat / renameat
        // (special-fd handling: AT_FDCWD == -100 means relative to cwd; we
        //  always resolve through the existing path-based syscalls.)
        case LSYS_OPENAT: {
            // (dirfd, pathname, flags, mode)
            return sys_open(ecx, edx, esi);
        }
        case LSYS_MKDIRAT: {
            return sys_mkdir(ecx, edx);
        }
        case LSYS_UNLINKAT: {
            // ignore flags (AT_REMOVEDIR not supported)
            return sys_unlink(ecx);
        }
        case LSYS_FSTATAT: {
            return sys_stat(ecx, edx);
        }

        // fcntl: minimal but real (F_GETFL, F_SETFL, F_GETFD, F_SETFD,
        //        F_DUPFD, F_DUPFD_CLOEXEC).
        case LSYS_FCNTL: {
            int fd = (int)ebx; int cmd = (int)ecx; uint32_t arg = edx;
            LinuxProcess* p = Current();
            // (satoru) [seal] probe: F_ADD_SEALS(1033) is what firefox's SharedMemory
            // Freeze() calls; if it returns !=0 the freeze fails -> SharedStringMap
            // ctor MOZ_RELEASE_ASSERT(result.isOk()) line 48. log the fd/open state +
            // return so the flaky freeze failure is visible. remove before commit.
            if (p && p->pid >= 100 && p->pid < 140 && (cmd == 1033 || cmd == 1034)) {
                SerialLogger::Log("[seal] pid="); SerialLogger::LogDec((int)p->pid);
                SerialLogger::Log(" fd="); SerialLogger::LogDec(fd);
                SerialLogger::Log(" cmd="); SerialLogger::LogDec(cmd);
                SerialLogger::Log(" arg="); SerialLogger::LogHex((uint32_t)arg);
                bool okfd = (fd >= 0 && fd < LINUX_MAX_FDS && p->fds[fd].open);
                SerialLogger::Log(" open="); SerialLogger::LogDec(okfd ? 1 : 0);
                SerialLogger::Log(" type="); SerialLogger::LogDec(okfd ? (int)p->fds[fd].type : -1);
                SerialLogger::Log("\r\n");
            }
            if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;
            LinuxFd* lfd = &p->fds[fd];
            switch (cmd) {
                case 0:  // F_DUPFD
                case 1030: { // F_DUPFD_CLOEXEC
                    for (int i = (int)arg; i < LINUX_MAX_FDS; i++) {
                        if (!p->fds[i].open) {
                            memcpy(&p->fds[i], lfd, sizeof(LinuxFd));
                            // refcount the aliased inet backend (satoru)
                            if (lfd->type == LFD_INET) LinuxNetBridge::Retain(lfd->backend_fd);
                            return i;
                        }
                    }
                    return -24;
                }
                case 1:  return 0;             // F_GETFD
                case 2:  return 0;             // F_SETFD (CLOEXEC ignored)
                case 3:  return (int32_t)lfd->flags;       // F_GETFL
                case 4:  lfd->flags = arg; return 0;        // F_SETFL
                case 5: case 6: case 7: {   // F_GETLK / F_SETLK / F_SETLKW - REAL locks (satoru)
                    // struct flock (x86_64): i16 l_type; i16 l_whence; pad;
                    // i64 l_start; i64 l_len; i32 l_pid. sqlite always uses
                    // SEEK_SET, so whence is taken as SET. the struct lives in
                    // the caller's address space and we are in its cr3. (satoru)
                    if (!arg) return -14;                             // -EFAULT (satoru)
                    volatile int16_t* fl_type  = (volatile int16_t*)(uintptr_t)arg;
                    volatile int64_t* fl_start = (volatile int64_t*)(uintptr_t)((uint64_t)arg + 8);
                    volatile int64_t* fl_len   = (volatile int64_t*)(uintptr_t)((uint64_t)arg + 16);
                    volatile int32_t* fl_pid   = (volatile int32_t*)(uintptr_t)((uint64_t)arg + 24);
                    int16_t  want = *fl_type;                         // 0 rd / 1 wr / 2 unlck (satoru)
                    uint64_t st   = (uint64_t)*fl_start;
                    uint64_t ln   = (uint64_t)*fl_len;
                    uint64_t en   = ln ? (st + ln) : ~0ull;           // len 0 = to eof (satoru)
                    uint64_t ph   = flock_path_hash(lfd->path);
                    Process* owner = region_owner(p->task);

                    if (cmd == 5) {                                   // F_GETLK (satoru)
                        SpinLockGuard g(g_flock_lock);
                        for (int i = 0; i < KFILE_LOCK_SLOTS; i++) {
                            KFileLock* L = &g_file_locks[i];
                            if (!L->active || L->path_hash != ph || L->owner == owner) continue;
                            if (L->start >= en || L->end <= st) continue;   // no overlap (satoru)
                            if (want == 1 || L->type == 1) {          // would conflict (satoru)
                                *fl_type  = L->type;                  // F_RDLCK/F_WRLCK (satoru)
                                *fl_start = (int64_t)L->start;
                                *fl_len   = (L->end == ~0ull) ? 0 : (int64_t)(L->end - L->start);
                                *fl_pid   = 1;                        // some other "process" (satoru)
                                return 0;
                            }
                        }
                        *fl_type = 2;                                 // F_UNLCK - no conflict (satoru)
                        return 0;
                    }

                    if (want == 2) {                                  // unlock (satoru)
                        SpinLockGuard g(g_flock_lock);
                        for (int i = 0; i < KFILE_LOCK_SLOTS; i++) {
                            KFileLock* L = &g_file_locks[i];
                            if (L->active && L->owner == owner && L->path_hash == ph &&
                                !(L->start >= en || L->end <= st))
                                L->active = false;
                        }
                        return 0;
                    }
                    if (want != 0 && want != 1) return -22;           // -EINVAL (satoru)

                    // acquire: F_SETLK fails fast on conflict; F_SETLKW yields
                    // (bounded ~10s - a longer wait is a deadlock) and retries. (satoru)
                    uint64_t fl_deadline = Time::GetTicks() + 10000;
                    for (;;) {
                        {
                            SpinLockGuard g(g_flock_lock);
                            bool conflict = false;
                            for (int i = 0; i < KFILE_LOCK_SLOTS; i++) {
                                KFileLock* L = &g_file_locks[i];
                                if (!L->active || L->path_hash != ph || L->owner == owner) continue;
                                if (L->start >= en || L->end <= st) continue;
                                if (want == 1 || L->type == 1) { conflict = true; break; }
                            }
                            if (!conflict) {
                                // replace this owner's overlapping ranges (posix
                                // re-lock = upgrade/downgrade in place). (satoru)
                                for (int i = 0; i < KFILE_LOCK_SLOTS; i++) {
                                    KFileLock* L = &g_file_locks[i];
                                    if (L->active && L->owner == owner && L->path_hash == ph &&
                                        !(L->start >= en || L->end <= st))
                                        L->active = false;
                                }
                                for (int i = 0; i < KFILE_LOCK_SLOTS; i++) {
                                    KFileLock* L = &g_file_locks[i];
                                    if (L->active) continue;
                                    L->active = true; L->type = want;
                                    L->path_hash = ph; L->start = st; L->end = en;
                                    L->owner = owner;
                                    return 0;
                                }
                                return -37;                           // -ENOLCK table full (satoru)
                            }
                        }
                        if (cmd == 6) return -11;                     // -EAGAIN (satoru)
                        if (Time::GetTicks() > fl_deadline) return -35;  // -EDEADLK (satoru)
                        if (SMP::CpuIndex() == 0) { KuronoShell::PumpUI(); Scheduler::SleepMs(1); }
                        else kls_relax();
                    }
                }
                default: return 0;
            }
        }

        // Real futex. WAIT enqueues the calling task on (address_space, uaddr)
        // and blocks (deschedules to another ready user task) when *uaddr still
        // equals val; WAKE moves up to `val` matching waiters back to runnable.
        // FUTEX_PRIVATE_FLAG (0x80) and FUTEX_CLOCK_REALTIME (0x100) are stripped
        // when matching the op. the bitset (val3) isn't plumbed through this
        // 5-arg dispatch, so the *_bitset ops are treated as match-any (what
        // glibc/musl pass for condvars). the timeout arg is infinite for now. (satoru)
        case LSYS_FUTEX: {
            const uint32_t FUTEX_PRIVATE   = 0x80;
            const uint32_t FUTEX_CLOCK_RT  = 0x100;
            uint32_t op       = ecx & ~(FUTEX_PRIVATE | FUTEX_CLOCK_RT);
            bool     clock_rt = (ecx & FUTEX_CLOCK_RT) != 0;
            uint64_t uaddr = ebx;
            uint32_t val   = (uint32_t)edx;

            Process* task = Scheduler::GetCurrentProcess();
            if (!task || !task->is_user()) return -22;  // -EINVAL

            switch (op) {
                case 0:   /* FUTEX_WAIT (relative timeout) */
                case 9: { /* FUTEX_WAIT_BITSET (absolute timeout, match-any) */
                    uint32_t cur = 0;
                    if (!read_user_u32(task, uaddr, &cur)) return -14;  // -EFAULT
                    if (cur != val) return -11;  // -EAGAIN - value already changed

                    // honour the timeout (4th arg r10 == esi). NULL == wait forever;
                    // op 0's timespec is RELATIVE, op 9 (WAIT_BITSET)'s is ABSOLUTE
                    // against CLOCK_MONOTONIC (== Timer::GetRealMs64()), or
                    // CLOCK_REALTIME when FUTEX_CLOCK_REALTIME was set. a timed wait
                    // that never receives an explicit FUTEX_WAKE is then released with
                    // -ETIMEDOUT by futex_sweep_timeouts so the caller re-polls, rather
                    // than the whole process deadlocking. this completes the poll/ppoll
                    // timeout fix for condvars (pthread_cond_timedwait, gecko's
                    // main-loop waits). a 64-bit timespec read; an i386 caller's 32-bit
                    // pair just reads large -> effectively infinite (no regression). (satoru)
                    uint64_t deadline_ms = 0;  // 0 == infinite (satoru)
                    if (esi != 0) {
                        uint64_t sec = 0, nsec = 0;
                        if (read_user_u64(task, esi, &sec) &&
                            read_user_u64(task, esi + 8, &nsec)) {
                            uint64_t now     = Timer::GetRealMs64();
                            uint64_t want_ms = sec * 1000ull + nsec / 1000000ull;
                            if (op == 9) {
                                if (clock_rt) {
                                    uint64_t real_now = TimeManager::NowUTC().us / 1000ull;
                                    deadline_ms = now + (want_ms > real_now ? want_ms - real_now : 0);
                                } else {
                                    deadline_ms = want_ms;  // monotonic == GetRealMs64 (satoru)
                                }
                            } else {
                                deadline_ms = now + want_ms;  // relative (satoru)
                            }
                            if (deadline_ms <= now) return -110;  // -ETIMEDOUT (already due) (satoru)
                        }
                    }

                    // preferred path: enqueue + deschedule by rewriting the trap
                    // frame (works exactly like sys_waitpid). both the int 0x80 and
                    // the x86_64 SYSCALL paths set current_syscall_frame now, so on
                    // success the frame is rewritten and our return value is
                    // ignored. (satoru)
                    if (futex_enqueue_and_block(task, (uintptr_t)uaddr, val, 0xFFFFFFFFu, deadline_ms)) {
                        return 0;
                    }

                    // fallback: enqueue failed because no other user task is
                    // runnable (every sibling is blocked) or the wait queue is
                    // full. we can't deschedule the last runnable task or userspace
                    // would wedge, so return a spurious wake; the caller re-tests
                    // *uaddr in ring-3 where the timer (irq0) can preempt it once a
                    // sibling becomes ready. (satoru)
                    return 0;
                }
                case 1:    /* FUTEX_WAKE */
                case 10: { /* FUTEX_WAKE_BITSET (match-any) */
                    int max = (int)val;
                    if (max < 0) max = 0x7FFFFFFF;
                    return futex_do_wake(task->address_space, (uintptr_t)uaddr,
                                         futex_phys_key(task, (uintptr_t)uaddr),
                                         max, 0xFFFFFFFFu);
                }
                case 3:    /* FUTEX_REQUEUE */
                case 4: {  /* FUTEX_CMP_REQUEUE */
                    // we don't implement true requeue to uaddr2; instead WAKE all
                    // waiters on uaddr. the woken threads simply re-contend on the
                    // target lock via their own FUTEX_WAIT - functionally correct
                    // (spurious wakes are always allowed) and, crucially, this
                    // unblocks pthread_cond_signal/broadcast, which MUSL implements
                    // with FUTEX_REQUEUE. these were previously no-op'd below, so a
                    // condvar broadcast never woke its waiters and gecko's startup
                    // deadlocked - every worker + the main thread parked forever in
                    // FUTEX_WAIT. the *uaddr==val3 compare for CMP_REQUEUE is
                    // skipped (a spurious wake is safe). (satoru)
                    return futex_do_wake(task->address_space, (uintptr_t)uaddr,
                                         futex_phys_key(task, (uintptr_t)uaddr),
                                         0x7FFFFFFF, 0xFFFFFFFFu);
                }
                default:
                    // FUTEX_WAKE_OP / PI variants: accept and no-op so callers
                    // don't see -ENOSYS mid-lock. (satoru)
                    return 0;
            }
        }

        // clone(). CLONE_THREAD (as glibc/musl pthread_create issues it) creates
        // a REAL kernel thread sharing the caller's address space; everything
        // else stays fork-like. x86_64 arg order: flags,stack,ptid,ctid,tls. (satoru)
        case LSYS_CLONE: {
            uint32_t  flags       = (uint32_t)ebx;
            uint64_t  child_stack = ecx;
            uint64_t  ptid        = edx;   // CLONE_PARENT_SETTID target
            uint64_t  ctid        = esi;   // CLONE_CHILD_*TID target (x86_64)
            uint64_t  tls         = edi;   // CLONE_SETTLS new fs base (x86_64)

            const uint32_t F_VM            = 0x00000100;
            const uint32_t F_VFORK         = 0x00004000;
            const uint32_t F_THREAD        = 0x00010000;
            const uint32_t F_SETTLS        = 0x00080000;
            const uint32_t F_PARENT_SETTID = 0x00100000;
            const uint32_t F_CHILD_CLEARTID= 0x00200000;
            const uint32_t F_CHILD_SETTID  = 0x01000000;

            // (satoru) TEMP [spawn]: confirm firefox attempts a SUBPROCESS launch
            // (fork/vfork, not a thread) - the GeckoChildProcessHost content-process
            // launch that kurono can't satisfy. remove before commit.
            {
                LinuxProcess* lcs = Current();
                if (lcs && lcs->pid >= 100 && lcs->pid < 140 &&
                    ((flags & F_VFORK) || !(flags & (F_THREAD | F_VM)))) {
                    SerialLogger::Log("[spawn] pid="); SerialLogger::LogDec(lcs->pid);
                    SerialLogger::Log(" flags="); SerialLogger::LogHex(flags);
                    SerialLogger::Log("\r\n");
                }
            }

            // CLONE_VFORK (musl posix_spawn / vfork): the child shares our VM and
            // runs the file-action setup on its own child_stack, then execve's into
            // a fresh address space. we now IMPLEMENT the vfork contract: fall
            // through to the thread path to create the VM-sharing child (with its
            // OWN copied fd table - no CLONE_FILES), then SUSPEND this parent until
            // the child execve's/_exit's (vfork_register + the block below;
            // execve_dynamic64 and process exit call vfork_wake_parent). this is
            // what finally lets firefox launch its content process instead of the
            // GeckoChildProcessHost::WaitForProcessHandle deadlock. (satoru)
            /* no early return - vfork proceeds through the thread path below. */

            // not a thread (no shared VM) = a real fork()/subprocess. kurono can't
            // fork+exec a working subprocess: the old fork-like path only allocated a
            // pid, the child never ran/execve'd, so firefox's GeckoChildProcessHost
            // got a FAKE pid and WaitForProcessHandle() futex-waited forever for a
            // child that never reports - THE paint stall (symbolized to
            // GeckoChildProcessHost::LaunchAndWaitForProcessHandle). fail honestly
            // with EAGAIN so firefox treats the launch as failed and falls back to
            // doing that work in-process (gpu/rdd/socket/utility degrade gracefully).
            // (satoru)
            if (!(flags & (F_THREAD | F_VM)) || !child_stack) {
                return -11;  // -EAGAIN
            }

            LinuxProcess* parent       = Current();
            Process*      parent_task  = Scheduler::GetCurrentProcess();
            if (!parent || !parent_task || !parent_task->is_user()) return -22;

            // vfork ISOLATION: a CLONE_VFORK child_stack that lies inside the
            // parent's main execve stack [0x3FA00000,0x40200000] means the child
            // shares the parent's stack (raw vfork passes the parent's sp). the
            // child then runs its whole pre-execve setup - and, when execve FAILS
            // (glxtest exit 127), its error/cleanup fallback - growing DOWN into
            // the region the parent's live frames occupy. that is the DETERMINISTIC
            // corruptor: a font/cmap buffer written straight through the chrome
            // main thread's nsCaret frame -> the stack-canary #gp that blocked page
            // paint. relocate the child onto a fresh isolated stack; it still
            // reaches the parent's spawn args through absolute pointers (shared VM),
            // only its OWN locals move off the parent's stack. copy a window of the
            // parent's stack top so the child's return address + immediate frame
            // survive the move (rbp starts 0, so no stale frame-chain walk). (satoru)
            uint64_t eff_child_stack = child_stack;
            if ((flags & F_VFORK) &&
                child_stack >= 0x3FA00000ULL && child_stack < 0x40200000ULL) {
                const uint64_t VF_BYTES = 4ULL * 1024 * 1024;
                const uint64_t VF_WIN   = 64ULL * 1024;   // copied parent-frame window
                uint64_t vbase = choose_mmap_base(parent_task, 0, VF_BYTES);
                bool okmap = (vbase != 0);
                for (uint64_t o = 0; okmap && o < VF_BYTES; o += PAGE_SIZE) {
                    void* pg = PMM::AllocBytes(PAGE_SIZE);
                    if (!pg) { okmap = false; break; }
                    memset(pg, 0, PAGE_SIZE);
                    if (!KernelVMM::MapPageInAddressSpace(parent_task->address_space,
                            vbase + o, (uint64_t)(uintptr_t)pg, PTE_USER | PTE_WRITABLE)) {
                        PMM::FreeBytes(pg, PAGE_SIZE); okmap = false; break;
                    }
                }
                if (okmap) {
                    add_region(parent_task, vbase, vbase + VF_BYTES,
                               PTE_USER | PTE_WRITABLE, USER_REGION_MMAP);
                    uint64_t fresh_sp = (vbase + VF_BYTES - VF_WIN) & ~0xFULL;
                    // both VAs live in the active (parent) address space; the
                    // kernel may touch these user pages directly. (satoru)
                    memcpy((void*)(uintptr_t)fresh_sp, (const void*)(uintptr_t)child_stack, VF_WIN);
                    eff_child_stack = fresh_sp;
                }
            }

            // spawn the schedulable thread task that shares parent_task's cr3. (satoru)
            Process* thread_task = Scheduler::CreateUserThread(
                parent_task, eff_child_stack, tls, (flags & F_SETTLS) != 0);
            if (!thread_task) return -11;  // -EAGAIN (out of task slots)

            // smp: CreateUserThread enqueued the child Ready, but its
            // LinuxProcess / fd table / settid setup happens below - pin it to
            // the bsp until the setup is complete so an ap's thread-dispatch
            // claim can't run a half-built thread. released after settid. the
            // bsp itself can't run it mid-syscall (timer preempt is ring-3
            // only), so this closes the window entirely. (satoru)
            thread_task->cpu_affinity = 1;   // bit 0 = bsp only (satoru)

            // both the int 0x80 and x86_64 SYSCALL paths now build a full
            // InterruptFrame and set current_syscall_frame before dispatch, so the
            // parent's user_frame was freshly saved and CreateUserThread copied a
            // correct post-clone rip/rflags with musl's child start fn live in r9.
            // no frame fixup needed. (satoru)

            // every schedulable user task needs a LinuxProcess whose ->task
            // points back at it. give the thread its own context copying the
            // parent's fds/cwd/brk. (satoru)
            int tidx = CreateProcess(parent->name, parent->uid, parent->gid);
            if (tidx < 0) {
                Scheduler::DestroyProcess(thread_task);
                return -11;
            }
            LinuxProcess* tproc = &procs[tidx];
            tproc->ppid        = parent->pid;
            tproc->euid        = parent->euid;
            tproc->egid        = parent->egid;
            tproc->brk_base    = parent->brk_base;
            tproc->brk_current = parent->brk_current;
            tproc->brk_max     = parent->brk_max;
            tproc->exit_code   = -1;
            tproc->exited      = false;
            tproc->task        = thread_task;
            tproc->signal_mask = parent->signal_mask;
            ls_scpy(tproc->cwd,  parent->cwd,  sizeof(tproc->cwd));
            ls_scpy(tproc->name, parent->name, sizeof(tproc->name));
            // CLONE_FILES (0x400): the thread SHARES the leader's fd table rather
            // than getting a private copy -- so fds opened by one thread are visible
            // to its siblings (firefox's WaylandProxy thread + GTK main thread depend
            // on this; without it the proxy can't see GTK's connection). drop the
            // fresh table CreateProcess just allocated and alias the parent's. (satoru)
            if (flags & 0x400u) {
                if (tproc->fds) KernelHeap::Free(tproc->fds);
                tproc->fds = parent->fds;
            } else {
                clone_file_descriptors(parent, tproc);
            }

            int32_t tid = (int32_t)thread_task->pid;

            // [tnew] trace thread creation (parent -> new tid + flags) so we can map
            // the launcher/IO thread + correlate with [texit]. (satoru)
            {
                LinuxProcess* _pp = Current();
                if (false && _pp && _pp->pid >= 100 && _pp->pid < 140) {   // gated off (satoru)
                    SerialLogger::Log("[tnew] par="); SerialLogger::LogDec(_pp->pid);
                    SerialLogger::Log(" tid="); SerialLogger::LogDec(tid);
                    SerialLogger::Log(" flags="); SerialLogger::LogHex(flags);
                    // (satoru) ptid(edx/rdx) vs ctid(esi/r10): musl sets both = &new->tid.
                    // if they DIFFER here, kurono mis-captures r10 (the clear_child_tid
                    // deadlock: exit wakes the wrong addr, pthread_join never wakes).
                    SerialLogger::Log(" ptid="); SerialLogger::LogHex((uint32_t)ptid);
                    SerialLogger::Log(" ctid="); SerialLogger::LogHex((uint32_t)ctid);
                    SerialLogger::Log("\r\n");
                }
            }

            // CLONE_PARENT_SETTID / CHILD_SETTID: publish the tid through the
            // shared address space (visible to parent and child). (satoru)
            if (flags & F_PARENT_SETTID) write_user_u32(parent_task, ptid, (uint32_t)tid);
            if (flags & F_CHILD_SETTID)  write_user_u32(parent_task, ctid, (uint32_t)tid);

            // CLONE_CHILD_CLEARTID: remember ctid so thread exit zeroes it and
            // futex-wakes any joiner (pthread_join waits on exactly this). (satoru)
            if (flags & F_CHILD_CLEARTID) thread_task->clear_child_tid = ctid;

            // setup complete - release the bsp pin so any core may run it. (satoru)
            thread_task->cpu_affinity = 0;

            // CLONE_VFORK: suspend this parent until the child execve's or _exit's.
            // mirror the futex block - set our resume return value (the child tid),
            // mark blocked, register the link, then yield straight to the child so
            // IT (not us) runs next in the shared VM, with no parent/child race.
            // on wake (execve/exit), the parent resumes its clone returning tid.
            // (satoru)
            if (flags & F_VFORK) {
                vfork_register(thread_task, parent_task);
                parent_task->user_frame.rax = (uint64_t)(uint32_t)tid;
                { uint64_t sf; Scheduler::StateLock(&sf); parent_task->state = Process_Blocked; Scheduler::StateUnlock(sf); }
                if (!switch_to_ready_user(current_syscall_frame)) {
                    vfork_wake_parent(thread_task);
                    uint64_t sf; Scheduler::StateLock(&sf); parent_task->state = Process_Running; Scheduler::StateUnlock(sf);
                }
            }

            return tid;  // parent sees the child tid; the child returns 0 (frame)
        }

        // Posix realtime signal stubs - accept both i386 numbering (174..)
        // and x86_64 numbering via the LSYS_RT_SIG* synonyms.  No delivery.
        case 174:  // i386 rt_sigaction
        case 175:  // i386 rt_sigprocmask
        case 173:  // i386 rt_sigreturn
        case LSYS_RT_SIGACTION:
        case LSYS_RT_SIGPROCMASK:
        case LSYS_RT_SIGSUSPEND:
        case LSYS_RT_SIGRETURN:
            return 0;
        // LSYS_RT_SIGPENDING moved to the Tier-7 build-out block below, where it
        // writes the process's real pending-signal mask into the user set. (satoru)

        // poll / select / ppoll / pselect6: return readiness count of 0 (no I/O).
        // For polled-stdin reads, the existing read() handler does its own
        // blocking wait via console buffering, so a short cooperative spin is
        // enough; we then report real per-fd readiness. (satoru)
        // poll/ppoll: struct pollfd { int fd; short events; short revents; }
        // is 8 bytes each on x86_64. we walk the user array, compute revents
        // from the same fd_readiness() logic epoll uses, and return the count
        // of fds with nonzero revents. the actual blocking + timeout handling
        // lives in do_poll_wait() above; poll passes a ms timeout, ppoll a
        // timespec. (satoru)
        case LSYS_POLL: {
            // poll(fds, nfds, timeout_ms): edx == timeout in ms, <0 == infinite (satoru)
            int32_t ms = (int32_t)edx;
            return do_poll_wait(Current(), (void*)(uintptr_t)ebx, ecx,
                                ms < 0 ? (int64_t)-1 : (int64_t)ms, 7);  // x86_64 poll nr (satoru)
        }
        case LSYS_PPOLL: {
            // ppoll(fds, nfds, const timespec*, sigmask, sigsetsize): edx == a
            // timespec* (null == infinite). parse it to ms and honour it so
            // glib's main loop blocks instead of busy-spinning on ppoll. (satoru)
            int64_t to = -1;
            if (edx) {
                struct TS { int64_t tv_sec; int64_t tv_nsec; };
                TS* ts = (TS*)(uintptr_t)edx;
                to = ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
            }
            return do_poll_wait(Current(), (void*)(uintptr_t)ebx, ecx, to, 271);  // x86_64 ppoll nr (satoru)
        }
        // select/pselect6: translate the three fd_sets into the same readiness
        // logic. fd_set is a bitmap of LINUX_MAX_FDS bits; we honour read/write
        // sets and clear bits that aren't ready. exceptfds is left untouched
        // (we never report exceptional conditions). nfds bounds the scan. (satoru)
        case LSYS_SELECT:
        case LSYS_PSELECT6: {
            LinuxProcess* p = Current();
            int maxfd = (int)ebx;
            uint64_t* rset = (uint64_t*)(uintptr_t)ecx;
            uint64_t* wset = (uint64_t*)(uintptr_t)edx;
            if (!p || maxfd <= 0) return 0;
            if (maxfd > LINUX_MAX_FDS) maxfd = LINUX_MAX_FDS;
            int spins = 64;
            int ready_total = 0;
            for (int s = 0; ; s++) {
                ready_total = 0;
                for (int fd = 0; fd < maxfd; fd++) {
                    uint64_t bit = 1ULL << (fd & 63);
                    int word = fd >> 6;
                    bool want_r = rset && (rset[word] & bit);
                    bool want_w = wset && (wset[word] & bit);
                    if (!want_r && !want_w) continue;
                    uint32_t interest = (want_r ? L_EPOLLIN : 0) |
                                        (want_w ? L_EPOLLOUT : 0);
                    uint32_t r = fd_readiness(p, fd, interest);
                    if (want_r && !(r & L_EPOLLIN)) rset[word] &= ~bit;
                    else if (want_r) ready_total++;
                    if (want_w && !(r & L_EPOLLOUT)) wset[word] &= ~bit;
                    else if (want_w) ready_total++;
                }
                if (ready_total > 0 || s >= spins) break;
                if (SMP::CpuIndex() == 0) KuronoShell::PumpUI();   // bsp-only (satoru)
                else kls_relax();                                   // let other cores' syscalls flow (satoru)
            }
            return ready_total;
        }

        // epoll_create1: allocate a real epoll instance with its own watch
        // table so epoll_ctl/epoll_wait can track interest and compute
        // readiness. backend_fd = epoll table slot. (satoru)
        case LSYS_EPOLL_CREATE1: {
            LinuxProcess* p = Current();
            if (!p) return -1;
            int slot = epoll_alloc();
            if (slot < 0) return -24;
            int fd = AllocFd(p);
            if (fd < 0) { g_epoll[slot].used = false; return -24; }
            memset(&p->fds[fd], 0, sizeof(LinuxFd));
            p->fds[fd].type = LFD_EPOLL;
            p->fds[fd].backend_fd = slot;
            p->fds[fd].open = true;
            return fd;
        }
        // eventfd2: a real 64-bit counter. ebx = initial value, ecx = flags
        // (EFD_SEMAPHORE = 1). write() adds, read() drains (or -1 in semaphore
        // mode); readable when counter > 0. (satoru)
        case LSYS_EVENTFD2: {
            LinuxProcess* p = Current();
            if (!p) return -1;
            int slot = eventfd_alloc();
            if (slot < 0) return -24;
            int fd = AllocFd(p);
            if (fd < 0) { g_eventfd[slot].used = false; return -24; }
            const uint32_t EFD_SEMAPHORE = 1;
            g_eventfd[slot].counter = (uint64_t)ebx;
            g_eventfd[slot].semaphore = (ecx & EFD_SEMAPHORE) != 0;
            memset(&p->fds[fd], 0, sizeof(LinuxFd));
            p->fds[fd].type = LFD_EVENTFD;
            p->fds[fd].backend_fd = slot;
            p->fds[fd].flags = ecx;
            p->fds[fd].open = true;
            // [evc] trace which thread OWNS each eventfd, so we can map the launch
            // wakeup fd (the one the main writes via ScheduleWork) to the pump thread
            // that must poll it -> find why that thread never enters its epoll. (satoru)
            if (false && p->pid >= 100 && p->pid < 140) {   // [evc]/[evcbt] gated off (satoru)
                SerialLogger::Log("[evc] pid="); SerialLogger::LogDec(p->pid);
                SerialLogger::Log(" fd="); SerialLogger::LogDec(fd);
                SerialLogger::Log("\r\n");
                // [evcbt] dump the creator's user caller chain (0x1800-region = libxul
                // + deps) so we can symbolize WHICH pump owns this eventfd -> which
                // thread must run it. (satoru)
                if (p->task) {
                    uint64_t sp = p->task->user_frame.rsp;
                    for (int i = 0; i < 48; i++) {
                        uint64_t v = 0;
                        if (read_user_u64(p->task, sp + (uint64_t)i*8, &v) &&
                            (v >> 32) == 0x1800ULL &&
                            (uint32_t)v > 0x100000u && (uint32_t)v < 0x19000000u) {
                            SerialLogger::Log("[evcbt] fd="); SerialLogger::LogDec(fd);
                            SerialLogger::Log(" +"); SerialLogger::LogDec(i*8);
                            SerialLogger::Log("=0x"); SerialLogger::LogHex((uint32_t)v);
                            SerialLogger::Log("\r\n");
                        }
                    }
                }
            }
            return fd;
        }
        // timerfd_create: ebx = clockid (ignored - single monotonic base),
        // ecx = flags. armed later by timerfd_settime; readable once expired.
        // backend_fd = timerfd table slot. (satoru)
        case LSYS_TIMERFD_CREATE: {
            LinuxProcess* p = Current();
            if (!p) return -1;
            int slot = timerfd_alloc();
            if (slot < 0) return -24;
            int fd = AllocFd(p);
            if (fd < 0) { g_timerfd[slot].used = false; return -24; }
            memset(&p->fds[fd], 0, sizeof(LinuxFd));
            p->fds[fd].type = LFD_TIMERFD;
            p->fds[fd].backend_fd = slot;
            p->fds[fd].flags = ecx;
            p->fds[fd].open = true;
            return fd;
        }
        // signalfd4: harmless stub - we have no real signal delivery, so the
        // fd never becomes readable. accepted so setup code doesn't crash. (satoru)
        case LSYS_SIGNALFD4: {
            LinuxProcess* p = Current();
            if (!p) return -1;
            int fd = AllocFd(p);
            if (fd < 0) return -24;
            memset(&p->fds[fd], 0, sizeof(LinuxFd));
            p->fds[fd].type = LFD_SIGNALFD;   // never reports EPOLLIN (satoru)
            p->fds[fd].open = true;
            return fd;
        }
        // inotify_init1 - a real but inert fd (no events fire). memfd is handled
        // separately below with real shm backing. (satoru)
        case LSYS_INOTIFY_INIT1: {
            LinuxProcess* p = Current();
            if (!p) return -1;
            int fd = AllocFd(p);
            if (fd < 0) return -24;
            memset(&p->fds[fd], 0, sizeof(LinuxFd));
            p->fds[fd].type = LFD_DEVNULL;
            p->fds[fd].open = true;
            return fd;
        }
        // memfd_create: a real, sizeable shared-memory object (wl_shm pools,
        // posix shm). backed by contiguous physical pages once ftruncate sets
        // the size; mmap(MAP_SHARED) then maps those pages in. (satoru)
        case LSYS_MEMFD_CREATE: {
            LinuxProcess* p = Current();
            if (!p) return -1;
            int slot = shm_alloc_slot();
            if (slot < 0) return -24;
            int fd = AllocFd(p);
            if (fd < 0) { g_linux_shm[slot].used = false; return -24; }
            memset(&p->fds[fd], 0, sizeof(LinuxFd));
            p->fds[fd].type = LFD_MEMFD;
            p->fds[fd].backend_fd = slot;
            p->fds[fd].open = true;
            return fd;
        }
        // epoll_ctl(epfd, op, fd, event): op 1=ADD 2=DEL 3=MOD. Records the
        // {fd, interest, user-data} tuple in the epoll instance's watch table
        // so epoll_wait can scan it. (satoru)
        case LSYS_EPOLL_CTL: {
            LinuxProcess* p = Current();
            int epfd = (int)ebx; int op = (int)ecx; int tfd = (int)edx;
            LinuxEpollEvent* ev = (LinuxEpollEvent*)(uintptr_t)esi;
            if (!p || epfd < 0 || epfd >= LINUX_MAX_FDS || !p->fds[epfd].open) return -9;
            if (p->fds[epfd].type != LFD_EPOLL) return -22;  // einval (satoru)
            int slot = p->fds[epfd].backend_fd;
            if (slot < 0 || slot >= EPOLL_MAX || !g_epoll[slot].used) return -22;
            EpollState* es = &g_epoll[slot];
            // [epc] trace epoll registrations so we can find which thread owns the
            // epoll that watches the launch wakeup eventfd (fd 8). (satoru)
            if (p->pid >= 100 && p->pid < 140) {
                SerialLogger::Log("[epc] pid="); SerialLogger::LogDec(p->pid);
                SerialLogger::Log(" op="); SerialLogger::LogDec(op);
                SerialLogger::Log(" tfd="); SerialLogger::LogDec(tfd);
                SerialLogger::Log(" epfd="); SerialLogger::LogDec(epfd);
                SerialLogger::Log("\r\n");
            }
            const int EPOLL_CTL_ADD = 1, EPOLL_CTL_DEL = 2, EPOLL_CTL_MOD = 3;
            // find an existing watch for this fd (satoru)
            int found = -1;
            for (int w = 0; w < EPOLL_MAX_WATCH; w++)
                if (es->watch[w].used && es->watch[w].fd == tfd) { found = w; break; }
            if (op == EPOLL_CTL_DEL) {
                if (found < 0) return -2;   // enoent (satoru)
                es->watch[found].used = false;
                return 0;
            }
            if (op == EPOLL_CTL_MOD) {
                if (found < 0) return -2;
                if (!ev) return -14;        // efault (satoru)
                es->watch[found].events = ev->events;
                es->watch[found].data   = ev->data;
                return 0;
            }
            if (op == EPOLL_CTL_ADD) {
                if (found >= 0) return -17; // eexist (satoru)
                if (!ev) return -14;
                for (int w = 0; w < EPOLL_MAX_WATCH; w++) {
                    if (!es->watch[w].used) {
                        es->watch[w].used   = true;
                        es->watch[w].fd     = tfd;
                        es->watch[w].events = ev->events;
                        es->watch[w].data   = ev->data;
                        return 0;
                    }
                }
                return -28;   // enospc - watch table full (satoru)
            }
            return -22;
        }
        // epoll_wait(epfd, events, maxevents, timeout): scan the watch table,
        // compute readiness per fd, fill the user epoll_event array, return the
        // count. if nothing ready and timeout != 0, do a bounded cooperative
        // spin (PumpUI) re-scanning each pass, then return what we have (which
        // may be 0). this is NOT a true blocking wait - it spins a fixed number
        // of iterations so the cooperative scheduler keeps making progress
        // rather than parking the caller. (satoru)
        case LSYS_EPOLL_WAIT: {
            LinuxProcess* p = Current();
            int epfd = (int)ebx;
            LinuxEpollEvent* out = (LinuxEpollEvent*)(uintptr_t)ecx;
            int maxevents = (int)edx;
            int timeout = (int)esi;   // ms; -1 = infinite, 0 = nonblocking
            if (!p || epfd < 0 || epfd >= LINUX_MAX_FDS || !p->fds[epfd].open) return -9;
            if (p->fds[epfd].type != LFD_EPOLL || !out || maxevents <= 0) return -22;
            int slot = p->fds[epfd].backend_fd;
            if (slot < 0 || slot >= EPOLL_MAX || !g_epoll[slot].used) { p->poll_blocking = false; return -22; }
            EpollState* es = &g_epoll[slot];
            // same cooperative wait as do_poll_wait: scan readiness; if nothing is
            // ready, hand the cpu to a SIBLING user thread (poll_try_deschedule) and
            // re-run on wake, honouring `timeout` via a deadline kept in the
            // LinuxProcess so it survives the syscall re-runs. the old code only
            // spun with Scheduler::SleepMs(1), which context-switches KERNEL procs
            // but NOT sibling user threads -- so a thread parked in epoll_pwait
            // starved its siblings. that wedged firefox: the e10s IPC I/O thread,
            // once it entered its epoll_pwait pump loop, never yielded to the chrome
            // main thread it had just FUTEX_WAKE'd (the WaitableEvent in
            // base::Thread::StartWithOptions), so the main thread stayed Ready-but-
            // never-run and NS_InitXPCOM never returned. (satoru)
            for (;;) {
                int n = 0;
                for (int w = 0; w < EPOLL_MAX_WATCH && n < maxevents; w++) {
                    if (!es->watch[w].used) continue;
                    uint32_t interest = es->watch[w].events | L_EPOLLERR | L_EPOLLHUP;
                    uint32_t r = fd_readiness(p, es->watch[w].fd, interest);
                    if (r) {
                        out[n].events = r;
                        out[n].data   = es->watch[w].data;
                        n++;
                    }
                }
                if (n > 0) { p->poll_blocking = false; return n; }
                // [epw] probe: firefox epoll found NOTHING ready -> dump the watch set
                // with each fd's type + interest + current readiness, so we can see
                // the wedged IO/launcher thread's wakeup fd (a pipe/eventfd that
                // should read as ready after a cross-thread post). sampled. (satoru)
                {
                    static uint64_t _epw = 0;
                    if (false && p->pid >= 100 && p->pid < 140 && ((++_epw & 1023) == 0)) {
                        SerialLogger::Log("[epw] pid="); SerialLogger::LogDec(p->pid);
                        for (int w = 0; w < EPOLL_MAX_WATCH; w++) {
                            if (!es->watch[w].used) continue;
                            int wfd = es->watch[w].fd;
                            int wt = (wfd >= 0 && wfd < LINUX_MAX_FDS && p->fds[wfd].open)
                                         ? (int)p->fds[wfd].type : -1;
                            SerialLogger::Log(" f="); SerialLogger::LogDec(wfd);
                            SerialLogger::Log("t"); SerialLogger::LogDec(wt);
                            SerialLogger::Log("i"); SerialLogger::LogHex(es->watch[w].events);
                            SerialLogger::Log("r"); SerialLogger::LogHex(fd_readiness(p, wfd, 0xFFFFFFFFu));
                        }
                        SerialLogger::Log("\r\n");
                    }
                }
                if (timeout == 0) { p->poll_blocking = false; return 0; }   // non-blocking pass (satoru)
                uint64_t now = Time::GetTicks();
                if (!p->poll_blocking) {                                    // first block: arm deadline (satoru)
                    p->poll_blocking    = true;
                    p->poll_deadline_ms = (timeout < 0) ? 0xFFFFFFFFFFFFFFFFULL
                                                        : now + (uint64_t)timeout;
                }
                if (now >= p->poll_deadline_ms) { p->poll_blocking = false; return 0; }  // timed out (satoru)
                // hand the cpu to a sibling user thread; re-issue epoll_wait (232) on
                // wake (epoll_pwait shares the first four args, so 232 restarts both). (satoru)
                if (poll_try_deschedule(p, 232)) return 0;   // switched; return ignored
                // no sibling: cooperative in-place wait -- bsp pumps ui + yields to
                // kernel procs; an ap releases the kls lock and breathes. (satoru)
                if (SMP::CpuIndex() == 0) {
                    LinuxNetBridge::PumpTick();   // nic drain for inet fds in the set (satoru)
                    KuronoShell::PumpUI();
                    Scheduler::SleepMs(1);
                } else {
                    kls_relax();
                }
            }
        }
        // timerfd_settime(fd, flags, new_value, old_value): arm/disarm. The
        // itimerspec is two timespecs { it_interval, it_value }, each
        // { time_t tv_sec; long tv_nsec } = 16 bytes on x86_64, so new_value is
        // 32 bytes. flags bit0 = TFD_TIMER_ABSTIME. it_value == 0 disarms.
        // (satoru)
        case LSYS_TIMERFD_SETTIME: {
            LinuxProcess* p = Current();
            int fd = (int)ebx; int flags = (int)ecx;
            const uint64_t* nv = (const uint64_t*)(uintptr_t)edx;  // itimerspec (satoru)
            uint64_t* ov = (uint64_t*)(uintptr_t)esi;
            if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;
            if (p->fds[fd].type != LFD_TIMERFD) return -22;
            int slot = p->fds[fd].backend_fd;
            if (slot < 0 || slot >= TIMERFD_MAX || !g_timerfd[slot].used) return -22;
            TimerfdState* t = &g_timerfd[slot];
            // report the old setting if the caller asked (best-effort) (satoru)
            if (ov) {
                uint64_t now = (uint64_t)Timer::GetRealMs();
                uint64_t remain = (t->armed && t->expiry_ms > now) ? (t->expiry_ms - now) : 0;
                ov[0] = t->interval_ms / 1000;            // it_interval.tv_sec
                ov[1] = (t->interval_ms % 1000) * 1000000ULL;
                ov[2] = remain / 1000;                    // it_value.tv_sec
                ov[3] = (remain % 1000) * 1000000ULL;
            }
            if (!nv) return -14;
            uint64_t interval_ms = nv[0] * 1000ULL + nv[1] / 1000000ULL;  // it_interval
            uint64_t value_ms    = nv[2] * 1000ULL + nv[3] / 1000000ULL;  // it_value
            const int TFD_TIMER_ABSTIME = 1;
            uint64_t now = (uint64_t)Timer::GetRealMs();
            t->interval_ms = interval_ms;
            t->expirations = 0;
            if (nv[2] == 0 && nv[3] == 0) {
                t->armed = false;          // it_value == 0 disarms (satoru)
            } else {
                t->armed = true;
                t->expiry_ms = (flags & TFD_TIMER_ABSTIME) ? value_ms : (now + value_ms);
            }
            return 0;
        }
        // timerfd_gettime(fd, curr_value): report remaining time + interval.
        // (satoru)
        case LSYS_TIMERFD_GETTIME: {
            LinuxProcess* p = Current();
            int fd = (int)ebx;
            uint64_t* cv = (uint64_t*)(uintptr_t)ecx;
            if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;
            if (p->fds[fd].type != LFD_TIMERFD || !cv) return -22;
            int slot = p->fds[fd].backend_fd;
            if (slot < 0 || slot >= TIMERFD_MAX || !g_timerfd[slot].used) return -22;
            TimerfdState* t = &g_timerfd[slot];
            uint64_t now = (uint64_t)Timer::GetRealMs();
            uint64_t remain = (t->armed && t->expiry_ms > now) ? (t->expiry_ms - now) : 0;
            cv[0] = t->interval_ms / 1000;
            cv[1] = (t->interval_ms % 1000) * 1000000ULL;
            cv[2] = remain / 1000;
            cv[3] = (remain % 1000) * 1000000ULL;
            return 0;
        }
        case LSYS_INOTIFY_ADD_WATCH:
        case LSYS_INOTIFY_RM_WATCH:
            return 0;

        // sendfile/splice/tee - fall back to bounded read/write loop.
        case LSYS_SENDFILE: {
            int out_fd = (int)ebx; int in_fd = (int)ecx;
            uintptr_t off = (uintptr_t)edx; uint64_t cnt = esi;  // off is a 64-bit user ptr (satoru)
            if (off) {
                uint32_t* off_p = (uint32_t*)off;
                sys_lseek(in_fd, (int32_t)*off_p, 0);
            }
            char buf[512];
            uint64_t total = 0;
            while (total < cnt) {
                if (SMP::CpuIndex() == 0) KuronoShell::PumpUI();   // bsp-only (satoru)
                uint64_t chunk = cnt - total > 512 ? 512 : cnt - total;
                // buf is a kernel stack address (64-bit under mcmodel=large) (satoru)
                int r = sys_read(in_fd, (uintptr_t)buf, chunk);
                if (r <= 0) break;
                int w = sys_write(out_fd, (uintptr_t)buf, (uint64_t)r);
                if (w <= 0) break;
                total += (uint32_t)w;
            }
            return (int32_t)total;
        }
        case LSYS_SPLICE:
        case LSYS_TEE:
            return 0;

        case LSYS_FALLOCATE: {
            // grow a memfd's backing for posix_fallocate-sized shm pools. wayland's
            // os_create_anonymous_file sizes the wl_shm pool memfd via fallocate (not
            // ftruncate); the old no-op left it 0-length, so the pool's mmap failed,
            // wl_cursor_theme_load returned NULL and GDK aborted on the missing
            // cursor theme -- and ALL wl_shm rendering would fail the same way. args:
            // (fd, mode, offset, len) -> size to offset+len. non-memfd fds: no-op.
            // (satoru)
            LinuxProcess* p = Current();
            LinuxShmObj* s = shm_for_fd(p, (int)ebx);
            if (s) {
                uint64_t end = (uint64_t)edx + (uint64_t)esi;   // offset + len (satoru)
                return shm_set_size(s, end) ? 0 : -28;          // -ENOSPC
            }
            return 0;
        }
        case LSYS_RENAMEAT2: {
            // rename/renameat/renameat2 all normalize here to (oldpath=ecx,
            // newpath=esi). firefox does EVERY durable write as write-tmp +
            // rename-over-final (session store, prefs, startup cache, the rust
            // std fs). the old no-op stub silently dropped the rename, so the
            // final file never appeared -> "could not move .tmp" / "No such file
            // or directory" all over chrome startup. do a real kvfs move,
            // replacing any existing destination (rename overwrites). (satoru)
            LinuxProcess* p = Current();
            const char* oldp = (const char*)(uintptr_t)ecx;
            const char* newp = (const char*)(uintptr_t)esi;
            if (!p || !oldp || !newp) return -22;   // -EINVAL
            char ro[256], rn[256];
            ResolvePath(oldp, ro, sizeof(ro), p);
            ResolvePath(newp, rn, sizeof(rn), p);
            KVFS::Unlink(rn);                        // replace an existing target (satoru)
            if (KVFS::Move(ro, rn) == KVFS_OK) return 0;
            return -2;   // -ENOENT (source missing / move failed)
        }

        case LSYS_PIDFD_OPEN:
        case LSYS_PIDFD_SEND_SIGNAL:
            return 0;

        case LSYS_EXECVEAT:
            return sys_execve(ecx, edx, esi);

        // getrandom: fill with RDTSC-derived pseudo-random bytes.
        case LSYS_GETRANDOM: {
            uint8_t* dst = (uint8_t*)(uintptr_t)ebx;
            uint32_t len = ecx;
            uint64_t tsc;
            for (uint32_t i = 0; i < len; i++) {
                if ((i & 7) == 0) {
                    __asm__ __volatile__("rdtsc" : "=A"(tsc));
                    tsc ^= (tsc >> 33) * 0x9E3779B97F4A7C15ULL;
                }
                dst[i] = (uint8_t)(tsc & 0xFF);
                tsc >>= 8;
            }
            return (int32_t)len;
        }

        // ----- Containers / namespaces / cgroups plumbing -----
        // unshare(flags): create new namespaces in current process.
        // Real implementation would clone-on-write the namespace state;
        // for now we just bump the relevant ns_* counters so callers can
        // observe distinct ids in /proc/self/ns/*.
        case LSYS_UNSHARE: {
            LinuxProcess* lp = Current();
            if (!lp) return -1;
            Process* k = lp->task;
            if (!k) return 0;
            uint32_t flags = ebx;
            static uint32_t ns_seq = 1000;
            const uint32_t CLONE_NEWNS    = 0x00020000;
            const uint32_t CLONE_NEWUTS   = 0x04000000;
            const uint32_t CLONE_NEWIPC   = 0x08000000;
            const uint32_t CLONE_NEWUSER  = 0x10000000;
            const uint32_t CLONE_NEWPID   = 0x20000000;
            const uint32_t CLONE_NEWNET   = 0x40000000;
            const uint32_t CLONE_NEWCGROUP = 0x02000000;
            if (flags & CLONE_NEWNS)    k->ns_mnt    = ++ns_seq;
            if (flags & CLONE_NEWUTS)   k->ns_uts    = ++ns_seq;
            if (flags & CLONE_NEWIPC)   k->ns_ipc    = ++ns_seq;
            if (flags & CLONE_NEWUSER)  k->ns_user   = ++ns_seq;
            if (flags & CLONE_NEWPID)   k->ns_pid    = ++ns_seq;
            if (flags & CLONE_NEWNET)   k->ns_net    = ++ns_seq;
            if (flags & CLONE_NEWCGROUP) k->ns_cgroup = ++ns_seq;
            return 0;
        }
        case LSYS_SETNS:        return 0;  // attach to ns by fd - accept
        case LSYS_PIVOT_ROOT:   return 0;  // accept root pivot for containers
        case LSYS_CHROOT: {
            LinuxProcess* lp = Current();
            const char* p = (const char*)(uintptr_t)ebx;
            if (lp && p) ls_scpy(lp->cwd, p, sizeof(lp->cwd));
            return 0;
        }
        case LSYS_SETHOSTNAME: {
            const char* p = (const char*)(uintptr_t)ebx;
            uint32_t n = ecx;
            char hn[64]; if (n >= sizeof(hn)) n = sizeof(hn) - 1;
            for (uint32_t i = 0; i < n; i++) hn[i] = p[i];
            hn[n] = 0;
            KVFS::WriteString("/etc/hostname", hn);
            KVFS::WriteString("/proc/sys/kernel/hostname", hn);
            return 0;
        }
        case LSYS_SETDOMAINNAME: return 0;

        // ----- Modules: accept but no real load yet -----
        case LSYS_INIT_MODULE:
        case LSYS_FINIT_MODULE:
        case LSYS_DELETE_MODULE:
        case LSYS_KEXEC_LOAD:
            return 0;

        // ----- bpf(): accept BPF_MAP_CREATE / MAP_LOOKUP / MAP_UPDATE / MAP_DELETE
        //              and PROG_LOAD as a no-op returning a fresh fd. -----
        case LSYS_BPF: {
            int cmd = (int)ebx;
            // 0=BPF_MAP_CREATE 1=MAP_LOOKUP 2=MAP_UPDATE 3=MAP_DELETE
            // 4=MAP_GET_NEXT_KEY 5=PROG_LOAD 6=OBJ_PIN 7=OBJ_GET
            if (cmd == 0 || cmd == 5 || cmd == 6 || cmd == 7) {
                LinuxProcess* lp = Current();
                if (!lp) return -1;
                int fd = AllocFd(lp);
                if (fd < 0) return -24;
                memset(&lp->fds[fd], 0, sizeof(LinuxFd));
                lp->fds[fd].type = LFD_DEVNULL;
                lp->fds[fd].open = true;
                return fd;
            }
            return 0;  // map ops succeed silently
        }

        // ----- perf_event_open(): return a fresh fd, RDPMC reads land
        //       on read() of the fd via LFD_DEVNULL (returns 0). -----
        case LSYS_PERF_EVENT_OPEN: {
            LinuxProcess* lp = Current();
            if (!lp) return -1;
            int fd = AllocFd(lp);
            if (fd < 0) return -24;
            memset(&lp->fds[fd], 0, sizeof(LinuxFd));
            lp->fds[fd].type = LFD_DEVNULL;
            lp->fds[fd].open = true;
            return fd;
        }

        // ----- Misc that programs poke -----
        // (LSYS_USERFAULTFD moved to the Tier-6 build-out block: it now returns a
        //  real fd instead of 0, which aliased stdin. name_to_handle_at /
        //  open_by_handle_at keep their accept-0 here and are now reachable from
        //  the x64 path via kNrMap. (satoru))
        case LSYS_KEYCTL:
        case LSYS_KCMP:
        case LSYS_FANOTIFY_INIT:
        case LSYS_FANOTIFY_MARK:
        case LSYS_NAME_TO_HANDLE_AT:
        case LSYS_OPEN_BY_HANDLE_AT:
            return 0;

        // ===== Firefox-required modern syscalls =====================

        // statx(dirfd, pathname, flags, mask, statxbuf) - full impl.
        case LSYS_STATX: {
            int dirfd = (int)ebx;
            (void)dirfd;
            const char* path = (const char*)(uintptr_t)ecx;
            uint32_t flags  = edx;
            uint32_t mask   = esi;
            LinuxStatx* sbuf = (LinuxStatx*)(uintptr_t)edi;
            (void)flags;
            if (!path || !sbuf) return -14;        // EFAULT
            LinuxProcess* lp = Current();
            char resolved[256];
            ResolvePath(path, resolved, sizeof(resolved), lp);
            KVFSNode* n = KVFS::Resolve(resolved);
            if (!n) return -2;                     // ENOENT
            for (uint32_t i = 0; i < sizeof(LinuxStatx); i++)
                ((uint8_t*)sbuf)[i] = 0;
            sbuf->stx_mask        = mask;
            sbuf->stx_blksize     = 4096;
            sbuf->stx_attributes  = 0;
            sbuf->stx_nlink       = 1;
            sbuf->stx_uid         = n->perms.uid;
            sbuf->stx_gid         = n->perms.gid;
            sbuf->stx_mode        = n->perms.mode;
            if (n->type == KVFS_DIR)     sbuf->stx_mode |= 040000;
            else if (n->type == KVFS_FILE) sbuf->stx_mode |= 0100000;
            else if (n->type == KVFS_SYMLINK) sbuf->stx_mode |= 0120000;
            else if (n->type == KVFS_DEVICE)  sbuf->stx_mode |= 020000;
            sbuf->stx_ino         = (uint64_t)(uintptr_t)n;
            sbuf->stx_size        = n->size;
            sbuf->stx_blocks      = (n->size + 511) / 512;
            sbuf->stx_attributes_mask = 0;
            sbuf->stx_atime.tv_sec = n->accessed;
            sbuf->stx_btime.tv_sec = n->created;
            sbuf->stx_ctime.tv_sec = n->modified;
            sbuf->stx_mtime.tv_sec = n->modified;
            sbuf->stx_dev_major   = 8;
            sbuf->stx_dev_minor   = 1;
            sbuf->stx_mnt_id      = 1;
            return 0;
        }

        // copy_file_range(fd_in, off_in, fd_out, off_out, len, flags)
        case LSYS_COPY_FILE_RANGE: {
            int    fd_in   = (int)ebx;
            uint64_t* offi = (uint64_t*)(uintptr_t)ecx;
            int    fd_out  = (int)edx;
            uint64_t* offo = (uint64_t*)(uintptr_t)esi;
            uint32_t len   = edi;
            LinuxProcess* lp = Current();
            if (!lp) return -9;
            if (fd_in < 0 || fd_in >= LINUX_MAX_FDS) return -9;
            if (fd_out < 0 || fd_out >= LINUX_MAX_FDS) return -9;
            uint8_t buf[2048];
            uint32_t copied = 0;
            while (copied < len) {
                if (SMP::CpuIndex() == 0) KuronoShell::PumpUI();   // bsp-only (satoru)
                uint32_t chunk = len - copied;
                if (chunk > sizeof(buf)) chunk = sizeof(buf);
                // buf is a kernel stack address (64-bit under mcmodel=large) (satoru)
                int r = sys_read(fd_in, (uintptr_t)buf, chunk);
                if (r <= 0) break;
                int w = sys_write(fd_out, (uintptr_t)buf, (uint64_t)r);
                if (w <= 0) break;
                copied += (uint32_t)w;
                if (offi) *offi += w;
                if (offo) *offo += w;
                if (w < r) break;
            }
            return (int32_t)copied;
        }

        // close_range(first, last, flags) - close every fd in [first,last].
        case LSYS_CLOSE_RANGE: {
            uint32_t first = ebx, last = ecx;
            LinuxProcess* lp = Current();
            if (!lp) return -9;
            if (last >= LINUX_MAX_FDS) last = LINUX_MAX_FDS - 1;
            for (uint32_t i = first; i <= last; i++) {
                if (lp->fds[i].open) sys_close((int)i);
            }
            return 0;
        }

        // clone3(args, size) - Firefox uses to spawn content processes.
        // We implement enough to behave like fork() for the common case
        // (CLONE_VM not requested).  Returns child pid in parent, 0 in
        // child like classic fork.
        case LSYS_CLONE3: {
            // For now route to plain fork.
            return sys_fork();
        }

        // pidfd_getfd(pidfd, target_fd, flags) - duplicate target_fd
        // from the pidfd's process into ours.  Not multi-process safe in
        // this build; behave as dup() on the local fd.
        case LSYS_PIDFD_GETFD: {
            int target_fd = (int)ecx;
            LinuxProcess* lp = Current();
            if (!lp) return -9;
            int newfd = AllocFd(lp);
            if (newfd < 0) return -24;
            if (target_fd < 0 || target_fd >= LINUX_MAX_FDS ||
                !lp->fds[target_fd].open) return -9;
            lp->fds[newfd] = lp->fds[target_fd];
            return newfd;
        }

        // landlock_create_ruleset / add_rule / restrict_self - sandbox
        // primitives.  We accept the call shape and return real fds so
        // libsandbox is satisfied; rules are recorded but not enforced.
        case LSYS_LANDLOCK_CREATE: {
            LinuxProcess* lp = Current();
            int fd = AllocFd(lp);
            if (fd < 0) return -24;
            memset(&lp->fds[fd], 0, sizeof(LinuxFd));
            lp->fds[fd].type = LFD_LANDLOCK;
            lp->fds[fd].open = true;
            return fd;
        }
        case LSYS_LANDLOCK_ADD:
        case LSYS_LANDLOCK_RESTRICT:
            return 0;

        // io_uring_setup(entries, params) - ring file descriptor.
        case LSYS_IO_URING_SETUP: {
            LinuxProcess* lp = Current();
            int fd = AllocFd(lp);
            if (fd < 0) return -24;
            memset(&lp->fds[fd], 0, sizeof(LinuxFd));
            lp->fds[fd].type = LFD_URING;
            lp->fds[fd].open = true;
            return fd;
        }
        case LSYS_IO_URING_ENTER:
        case LSYS_IO_URING_REGISTER:
            return 0;

        // seccomp(operation, flags, args) - accept BPF programs without
        // installing them so Firefox sandbox init doesn't crash.
        case LSYS_SECCOMP:
            return 0;

        // process_vm_readv(pid, local_iov, liovcnt, remote_iov, riovcnt, flags)
        // process_vm_writev - same shape.  Same-process IPC; in this
        // single-address-space build we just memcpy.
        case LSYS_PROCESS_VM_READV:
        case LSYS_PROCESS_VM_WRITEV: {
            LinuxIovec* liov = (LinuxIovec*)(uintptr_t)ecx;
            uint32_t lcnt    = edx;
            LinuxIovec* riov = (LinuxIovec*)(uintptr_t)esi;
            uint32_t rcnt    = edi;
            if (!liov || !riov || !lcnt || !rcnt) return -14;
            // byte offsets/counts are 64-bit to match the widened iovec (satoru)
            uint64_t total = 0;
            uint32_t li = 0, ri = 0;
            uint64_t lo = 0, ro = 0;
            while (li < lcnt && ri < rcnt) {
                uint64_t want = liov[li].iov_len - lo;
                uint64_t have = riov[ri].iov_len - ro;
                uint64_t n = want < have ? want : have;
                if (eax == LSYS_PROCESS_VM_READV) {
                    memcpy((void*)(uintptr_t)(liov[li].iov_base + lo),
                           (void*)(uintptr_t)(riov[ri].iov_base + ro), n);
                } else {
                    memcpy((void*)(uintptr_t)(riov[ri].iov_base + ro),
                           (void*)(uintptr_t)(liov[li].iov_base + lo), n);
                }
                lo += n; ro += n; total += n;
                if (lo == liov[li].iov_len) { li++; lo = 0; }
                if (ro == riov[ri].iov_len) { ri++; ro = 0; }
            }
            return (int32_t)total;
        }

        case LSYS_MEMBARRIER:
        case LSYS_RSEQ:
            return 0;

        // ===== AF_UNIX socket family =================================

        case LSYS_SOCKET: {
            int domain = (int)ebx;
            int type   = (int)ecx;
            (void)edx;
            // af_inet: real tcp/udp over the kurono stack via LinuxNetBridge.
            // af_inet6 stays EAFNOSUPPORT - nspr probes v6 once, falls back to
            // v4. SOCK_NONBLOCK (0x800) maps onto the fd's O_NONBLOCK. (satoru)
            if (domain == 2 /* AF_INET */) {
                int t = type & 0xFF;
                if (t != 1 && t != 2) return -94;        // -ESOCKTNOSUPPORT (satoru)
                int bfd = LinuxNetBridge::Socket(2, t, (int)edx,
                                                 Current() ? Current()->pid : 0);
                if (bfd < 0) return bfd;                 // -EMFILE etc (satoru)
                LinuxProcess* lp = Current();
                int fd = AllocFd(lp);
                if (fd < 0) { LinuxNetBridge::Close(bfd); return -24; }
                memset(&lp->fds[fd], 0, sizeof(LinuxFd));
                lp->fds[fd].type = LFD_INET;
                lp->fds[fd].backend_fd = bfd;
                lp->fds[fd].open = true;
                if (type & 0x800 /* SOCK_NONBLOCK */) lp->fds[fd].flags |= L_O_NONBLOCK;
                // (satoru) [inet] socket-creation trace, gated off. flip to true to
                // watch the af_inet bridge hand out fds.
                if (false) {
                    SerialLogger::Log("[inet] socket t="); SerialLogger::LogDec(t);
                    SerialLogger::Log(" fd="); SerialLogger::LogDec(fd);
                    SerialLogger::Log(" bfd="); SerialLogger::LogDec(bfd);
                    SerialLogger::Log("\r\n");
                }
                return fd;
            }
            if (domain != 1 /* AF_UNIX */) return -97;   // EAFNOSUPPORT
            UnixSocket::SockType st = UnixSocket::UNIX_SOCK_STREAM;
            int t = type & 0xFF;
            if (t == 2) st = UnixSocket::UNIX_SOCK_DGRAM;
            else if (t == 5) st = UnixSocket::UNIX_SOCK_SEQPACKET;
            int sd = UnixSocket::Create(st);
            if (sd < 0) return -24;
            LinuxProcess* lp = Current();
            int fd = AllocFd(lp);
            if (fd < 0) { UnixSocket::Close(sd); return -24; }
            memset(&lp->fds[fd], 0, sizeof(LinuxFd));
            lp->fds[fd].type = LFD_SOCKET;
            lp->fds[fd].backend_fd = sd;
            lp->fds[fd].open = true;
            return fd;
        }
        case LSYS_BIND: {
            int fd = (int)ebx;
            // sockaddr_un layout: u16 family, char path[108]
            const uint8_t* sa = (const uint8_t*)(uintptr_t)ecx;
            LinuxProcess* lp = Current();
            if (!sa || !lp || fd < 0 || fd >= LINUX_MAX_FDS) return -9;
            // af_inet: 0.0.0.0:0 (musl's resolver) defers the ephemeral port
            // to the first sendto. (satoru)
            if (lp->fds[fd].type == LFD_INET) {
                LinuxSockaddrIn bsin;
                memcpy(&bsin, sa, sizeof(bsin));
                if (bsin.sin_family != 2) return -22;    // -EINVAL (satoru)
                return LinuxNetBridge::Bind(lp->fds[fd].backend_fd, &bsin);
            }
            if (lp->fds[fd].type != LFD_SOCKET) return -9;
            const char* path = (const char*)(sa + 2);
            return UnixSocket::Bind(lp->fds[fd].backend_fd, path);
        }
        case LSYS_LISTEN: {
            int fd = (int)ebx;
            LinuxProcess* lp = Current();
            if (fd < 0 || fd >= LINUX_MAX_FDS ||
                lp->fds[fd].type != LFD_SOCKET) return -9;
            return UnixSocket::Listen(lp->fds[fd].backend_fd, (int)ecx);
        }
        case LSYS_ACCEPT:
        case LSYS_ACCEPT4: {
            int fd = (int)ebx;
            LinuxProcess* lp = Current();
            if (fd < 0 || fd >= LINUX_MAX_FDS ||
                lp->fds[fd].type != LFD_SOCKET) return -9;
            char peer[128];
            int sd2 = UnixSocket::Accept(lp->fds[fd].backend_fd, peer, sizeof(peer));
            if (sd2 < 0) return sd2;
            int newfd = AllocFd(lp);
            if (newfd < 0) { UnixSocket::Close(sd2); return -24; }
            memset(&lp->fds[newfd], 0, sizeof(LinuxFd));
            lp->fds[newfd].type = LFD_SOCKET;
            lp->fds[newfd].backend_fd = sd2;
            lp->fds[newfd].open = true;
            return newfd;
        }
        case LSYS_CONNECT: {
            int fd = (int)ebx;
            const uint8_t* sa = (const uint8_t*)(uintptr_t)ecx;
            LinuxProcess* lp = Current();
            if (!sa || fd < 0 || fd >= LINUX_MAX_FDS) return -9;
            // af_inet: parse sockaddr_in, non-blocking connect through the
            // bridge. a BLOCKING socket emulates the wait here with the same
            // yield discipline as do_poll_wait (bsp pumps, ap relaxes). (satoru)
            if (lp->fds[fd].type == LFD_INET) {
                uint16_t fam = (uint16_t)(sa[0] | (sa[1] << 8));
                if (fam == 10) return -97;               // AF_INET6 probe (satoru)
                LinuxSockaddrIn sin;
                memcpy(&sin, sa, sizeof(sin));
                if (sin.sin_family != 2) return -97;
                int r = LinuxNetBridge::Connect(lp->fds[fd].backend_fd, &sin);
                // (satoru) [inet] connect trace, gated off. flip to true to watch
                // the dst + EINPROGRESS/established verdict per connect.
                if (false) {
                    SerialLogger::Log("[inet] connect fd="); SerialLogger::LogDec(fd);
                    SerialLogger::Log(" r="); SerialLogger::LogDec(r);
                    SerialLogger::Log("\r\n");
                }
                if (r == -115 && !(lp->fds[fd].flags & L_O_NONBLOCK)) {
                    // blocking connect: bounded wait (~10s) for the handshake. (satoru)
                    uint64_t deadline = Time::GetTicks() + 10000;
                    for (;;) {
                        r = LinuxNetBridge::ConnectPoll(lp->fds[fd].backend_fd);
                        if (r != -115) break;
                        if (Time::GetTicks() > deadline) { r = -110; break; }  // -ETIMEDOUT (satoru)
                        if (SMP::CpuIndex() == 0) {
                            LinuxNetBridge::PumpTick();
                            KuronoShell::PumpUI();
                            Scheduler::SleepMs(1);
                        } else {
                            kls_relax();
                        }
                    }
                }
                return r;
            }
            if (lp->fds[fd].type != LFD_SOCKET) return -9;
            const char* path = (const char*)(sa + 2);
            return UnixSocket::Connect(lp->fds[fd].backend_fd, path);
        }
        case LSYS_SENDTO: {
            int fd = (int)ebx;
            const void* buf = (const void*)(uintptr_t)ecx;
            int len = (int)edx;
            LinuxProcess* lp = Current();
            if (fd < 0 || fd >= LINUX_MAX_FDS) return -9;
            // af_inet: dest sockaddr rides in arg5 (edi); null dest = send on
            // the connected remote. esi carries the flags (MSG_DONTWAIT ok - 
            // the bridge never blocks anyway). (satoru)
            if (lp->fds[fd].type == LFD_INET) {
                const uint8_t* dsa = (const uint8_t*)(uintptr_t)edi;
                LinuxSockaddrIn dsin;
                LinuxSockaddrIn* pd = nullptr;
                if (dsa) { memcpy(&dsin, dsa, sizeof(dsin)); pd = &dsin; }
                return LinuxNetBridge::Sendto(lp->fds[fd].backend_fd, buf, len,
                                              (int)esi, pd);
            }
            if (lp->fds[fd].type != LFD_SOCKET) return -9;
            return UnixSocket::Send(lp->fds[fd].backend_fd, buf, len, 0);
        }
        case LSYS_SENDMSG: {
            // sendmsg(fd, const struct msghdr*, flags): gather the iov payload
            // and parse SCM_RIGHTS, so a passed memfd (e.g. a wl_shm pool fd)
            // reaches the in-kernel server resolved to its shm backing. (satoru)
            int fd = (int)ebx;
            const uint8_t* m = (const uint8_t*)(uintptr_t)ecx;   // struct msghdr*
            LinuxProcess* lp = Current();
            if (fd < 0 || fd >= LINUX_MAX_FDS) return -9;
            if (!m) return -14;
            // af_inet: gather the iov and send through the bridge - no cmsg
            // (SCM_RIGHTS is a unix-domain concept). msg_name, when set on a
            // udp socket, is the datagram destination. (satoru)
            if (lp->fds[fd].type == LFD_INET) {
                const uint8_t* nm  = *(const uint8_t* const*)(m + 0);   // msg_name
                const uint8_t* iv  = *(const uint8_t* const*)(m + 16);
                uint64_t ivl       = *(const uint64_t*)(m + 24);
                static uint8_t s_inet_tx[8192];
                int tot = 0;
                for (uint64_t i = 0; iv && i < ivl && tot < (int)sizeof(s_inet_tx); i++) {
                    const uint8_t* ib = *(const uint8_t* const*)(iv + i * 16);
                    uint64_t il       = *(const uint64_t*)(iv + i * 16 + 8);
                    for (uint64_t k = 0; ib && k < il && tot < (int)sizeof(s_inet_tx); k++)
                        s_inet_tx[tot++] = ib[k];
                }
                LinuxSockaddrIn dsin;
                LinuxSockaddrIn* pd = nullptr;
                if (nm) { memcpy(&dsin, nm, sizeof(dsin)); pd = &dsin; }
                return LinuxNetBridge::Sendto(lp->fds[fd].backend_fd,
                                              s_inet_tx, tot, (int)edx, pd);
            }
            if (lp->fds[fd].type != LFD_SOCKET) return -9;
            const uint8_t* iov = *(const uint8_t* const*)(m + 16);  // msg_iov
            uint64_t iovlen    = *(const uint64_t*)(m + 24);        // msg_iovlen
            const uint8_t* ctl = *(const uint8_t* const*)(m + 32);  // msg_control
            uint64_t ctllen    = *(const uint64_t*)(m + 40);        // msg_controllen

            // gather iov into a static scratch (cooperative single-cpu: safe;
            // libwayland flushes are <= one 4 KB connection buffer). (satoru)
            static uint8_t s_sendmsg_buf[8192];
            int total = 0;
            for (uint64_t i = 0; iov && i < iovlen &&
                                 total < (int)sizeof(s_sendmsg_buf); i++) {
                const uint8_t* ibase = *(const uint8_t* const*)(iov + i * 16);
                uint64_t ilen        = *(const uint64_t*)(iov + i * 16 + 8);
                for (uint64_t k = 0; ibase && k < ilen &&
                                     total < (int)sizeof(s_sendmsg_buf); k++)
                    s_sendmsg_buf[total++] = ibase[k];
            }

            // parse the control buffer for SCM_RIGHTS fd arrays. (satoru)
            UnixSocket::ControlMsg cm = {};
            if (ctl && ctllen >= 16) {
                uint64_t off = 0;
                while (off + 16 <= ctllen) {
                    uint64_t clen = *(const uint64_t*)(ctl + off);   // cmsg_len
                    int level     = *(const int*)(ctl + off + 8);    // cmsg_level
                    int ctype     = *(const int*)(ctl + off + 12);   // cmsg_type
                    if (clen < 16 || off + clen > ctllen) break;
                    if (level == 1 /*SOL_SOCKET*/ && ctype == 1 /*SCM_RIGHTS*/) {
                        const int* cfds = (const int*)(ctl + off + 16);
                        uint64_t ndata = (clen - 16) / 4;
                        for (uint64_t k = 0; k < ndata &&
                             cm.passed_fd_count < UnixSocket::UNIX_MAX_PASSED_FD; k++) {
                            int pfd = cfds[k];
                            int idx = cm.passed_fd_count++;
                            cm.passed_fds[idx] = pfd;
                            LinuxShmObj* s = shm_for_fd(lp, pfd);
                            if (s && s->base) {
                                cm.passed_shm_base[idx] = (uint64_t)(uintptr_t)s->base;
                                cm.passed_shm_size[idx] = s->size;
                            } else if (pfd >= 0 && pfd < LINUX_MAX_FDS &&
                                       lp->fds[pfd].open &&
                                       lp->fds[pfd].type == LFD_SOCKET) {
                                // passed unix-socket fd (firefox e10s ipc channel to
                                // the fork server): record the global sd so the
                                // receiver aliases the SAME socket, refcounted, not a
                                // /dev/null placeholder. (satoru)
                                cm.passed_sd[idx] = lp->fds[pfd].backend_fd;
                                cm.passed_is_socket[idx] = true;
                                if (false) {   // [scmTX] gated off (satoru)
                                    SerialLogger::Log("[scmTX] pass sock sd="); SerialLogger::LogDec(lp->fds[pfd].backend_fd);
                                    SerialLogger::Log(" by pid="); SerialLogger::LogDec(lp ? (int)lp->pid : -1); SerialLogger::Log("\r\n");
                                }
                            }
                        }
                    }
                    off += (clen + 7) & ~7ULL;   // CMSG_ALIGN
                }
            }
            return UnixSocket::SendMsg(lp->fds[fd].backend_fd,
                                       s_sendmsg_buf, total, 0, &cm);
        }
        case LSYS_RECVFROM: {
            // raw recv: ecx is a plain buffer, no msghdr/cmsg parsing. (satoru)
            int fd = (int)ebx;
            void* buf = (void*)(uintptr_t)ecx;
            int len = (int)edx;
            LinuxProcess* lp = Current();
            if (fd < 0 || fd >= LINUX_MAX_FDS) return -9;
            // af_inet: src sockaddr out-param rides in arg5 (edi). the addrlen
            // ptr (6th arg) is beyond Dispatch's reach - musl's resolver uses
            // its own length for the reply-source memcmp, so it's tolerable.
            // -EAGAIN when empty; blocking emulation stays in sys_read-style
            // callers (the resolver polls first). (satoru)
            if (lp->fds[fd].type == LFD_INET) {
                LinuxSockaddrIn ssin;
                int r = LinuxNetBridge::Recvfrom(lp->fds[fd].backend_fd, buf, len,
                                                 (int)esi, &ssin);
                if (r >= 0 && edi) {
                    write_sin((uint8_t*)(uintptr_t)edi,
                              LinuxNetBridge::Ntohl(ssin.sin_addr),
                              LinuxNetBridge::Ntohs(ssin.sin_port));
                }
                return r;
            }
            if (lp->fds[fd].type != LFD_SOCKET) return -9;
            return UnixSocket::Recv(lp->fds[fd].backend_fd, buf, len, 0);
        }
        case LSYS_RECVMSG: {
            // recvmsg(fd, struct msghdr*, flags): the symmetric inverse of the
            // sendmsg path. parse the iov, recv into scratch with a ControlMsg
            // out-param, scatter the bytes back into the user iov, then for any
            // SCM_RIGHTS fds the sender passed install NEW fds in this process
            // and write a proper SCM_RIGHTS cmsg into msg_control. (satoru)
            int fd = (int)ebx;
            uint8_t* m = (uint8_t*)(uintptr_t)ecx;              // struct msghdr*
            LinuxProcess* lp = Current();
            if (fd < 0 || fd >= LINUX_MAX_FDS) return -9;
            if (!m) return -14;
            // af_inet: recv through the bridge and scatter into the iov; fill
            // msg_name with the datagram source when the caller wants it. no
            // cmsg for inet - zero msg_controllen so the caller doesn't parse
            // stale bytes. (satoru)
            if (lp->fds[fd].type == LFD_INET) {
                uint8_t* nm      = *(uint8_t* const*)(m + 0);       // msg_name
                uint8_t* iv      = *(uint8_t* const*)(m + 16);
                uint64_t ivl     = *(const uint64_t*)(m + 24);
                static uint8_t s_inet_rx[8192];
                int want = 0;
                for (uint64_t i = 0; iv && i < ivl; i++)
                    want += (int)*(const uint64_t*)(iv + i * 16 + 8);
                if (want > (int)sizeof(s_inet_rx)) want = (int)sizeof(s_inet_rx);
                LinuxSockaddrIn ssin;
                int r = LinuxNetBridge::Recvfrom(lp->fds[fd].backend_fd,
                                                 s_inet_rx, want, (int)edx, &ssin);
                if (r < 0) return r;
                int off = 0;
                for (uint64_t i = 0; iv && i < ivl && off < r; i++) {
                    uint8_t* ib = *(uint8_t* const*)(iv + i * 16);
                    uint64_t il = *(const uint64_t*)(iv + i * 16 + 8);
                    int take = (int)il < (r - off) ? (int)il : (r - off);
                    for (int k = 0; ib && k < take; k++) ib[k] = s_inet_rx[off + k];
                    off += take;
                }
                if (nm) memcpy(nm, &ssin, sizeof(ssin));
                *(uint64_t*)(m + 40) = 0;                       // msg_controllen = 0 (satoru)
                return r;
            }
            if (lp->fds[fd].type != LFD_SOCKET) return -9;
            uint8_t* iov     = *(uint8_t* const*)(m + 16);      // msg_iov
            uint64_t iovlen  = *(const uint64_t*)(m + 24);      // msg_iovlen
            uint8_t* ctl     = *(uint8_t* const*)(m + 32);      // msg_control
            uint64_t ctllen  = *(const uint64_t*)(m + 40);      // msg_controllen

            // total room the caller offered across all iov segments - bound to
            // scratch so a cooperative single-cpu copy stays safe. (satoru)
            static uint8_t s_recvmsg_buf[8192];
            uint64_t want = 0;
            for (uint64_t i = 0; iov && i < iovlen; i++)
                want += *(const uint64_t*)(iov + i * 16 + 8);
            if (want > sizeof(s_recvmsg_buf)) want = sizeof(s_recvmsg_buf);

            UnixSocket::ControlMsg cm = {};
            int got = UnixSocket::Recv(lp->fds[fd].backend_fd,
                                       s_recvmsg_buf, (int)want, 0, &cm);
            if (got < 0) return got;

            // scatter received bytes back into the user iov segments. (satoru)
            int copied = 0;
            for (uint64_t i = 0; iov && i < iovlen && copied < got; i++) {
                uint8_t* ibase = *(uint8_t* const*)(iov + i * 16);
                uint64_t ilen  = *(const uint64_t*)(iov + i * 16 + 8);
                for (uint64_t k = 0; ibase && k < ilen && copied < got; k++)
                    ibase[k] = s_recvmsg_buf[copied++];
            }

            // clear msg_flags (offset +48) before reporting any condition. (satoru)
            uint32_t* mflags = (uint32_t*)(m + 48);
            *mflags = 0;

            // install received SCM_RIGHTS fds + write the cmsg back. for a passed
            // memfd the sender resolved its shm backing into the ControlMsg, so we
            // re-create a real shm fd that maps the SAME pages; otherwise install a
            // closeable placeholder (full cross-process aliasing of pipes/sockets
            // is a follow-up). (satoru)
            if (cm.passed_fd_count > 0) {
                int n = cm.passed_fd_count;
                if (n > UnixSocket::UNIX_MAX_PASSED_FD)
                    n = UnixSocket::UNIX_MAX_PASSED_FD;
                uint64_t need = 16 + (uint64_t)4 * n;   // CMSG_LEN(sizeof(int)*n)
                if (!ctl || ctllen < need) {
                    *mflags |= 0x8;                      // MSG_CTRUNC
                    *(uint64_t*)(m + 40) = 0;            // msg_controllen = 0
                } else {
                    int newfds[UnixSocket::UNIX_MAX_PASSED_FD];
                    int ninst = 0;
                    for (int k = 0; k < n; k++) {
                        int nf = AllocFd(lp);
                        if (nf < 0) break;               // fd table full; stop
                        memset(&lp->fds[nf], 0, sizeof(LinuxFd));
                        if (cm.passed_is_socket[k]) {
                            // alias the SAME global unix socket (refcounted) so the
                            // passed ipc channel actually carries data cross-process
                            // -- this is the firefox e10s channel handed to the fork
                            // server; the old /dev/null placeholder is why the parent
                            // busy-spun on a reply that never came. (satoru)
                            lp->fds[nf].type = LFD_SOCKET;
                            lp->fds[nf].backend_fd = cm.passed_sd[k];
                            UnixSocket::Retain(cm.passed_sd[k]);
                            if (false) {   // [scmRX] gated off (satoru)
                                SerialLogger::Log("[scmRX] aliased sock sd="); SerialLogger::LogDec(cm.passed_sd[k]);
                                SerialLogger::Log(" -> fd="); SerialLogger::LogDec(nf);
                                SerialLogger::Log(" by pid="); SerialLogger::LogDec(lp ? (int)lp->pid : -1); SerialLogger::Log("\r\n");
                            }
                        } else if (cm.passed_shm_base[k]) {
                            // re-wrap the shared backing as a real memfd. (satoru)
                            int slot = shm_alloc_slot();
                            if (slot >= 0) {
                                g_linux_shm[slot].base = (uint8_t*)(uintptr_t)cm.passed_shm_base[k];
                                g_linux_shm[slot].size = cm.passed_shm_size[k];
                                lp->fds[nf].type = LFD_MEMFD;
                                lp->fds[nf].backend_fd = slot;
                            } else {
                                lp->fds[nf].type = LFD_DEVNULL;
                            }
                        } else {
                            // no resolved backing: closeable placeholder. (satoru)
                            lp->fds[nf].type = LFD_DEVNULL;
                        }
                        lp->fds[nf].open = true;
                        newfds[ninst++] = nf;
                    }
                    if (ninst == 0) {
                        *mflags |= 0x8;                  // MSG_CTRUNC
                        *(uint64_t*)(m + 40) = 0;
                    } else {
                        uint64_t clen = 16 + (uint64_t)4 * ninst;
                        *(uint64_t*)(ctl + 0)  = clen;   // cmsg_len
                        *(int*)(ctl + 8)       = 1;      // cmsg_level SOL_SOCKET
                        *(int*)(ctl + 12)      = 1;      // cmsg_type  SCM_RIGHTS
                        int* outfds = (int*)(ctl + 16);
                        for (int k = 0; k < ninst; k++) outfds[k] = newfds[k];
                        *(uint64_t*)(m + 40) = clen;     // msg_controllen written
                        if (ninst < cm.passed_fd_count) *mflags |= 0x8;
                    }
                }
            } else {
                // nothing ancillary delivered: report zero control bytes. (satoru)
                *(uint64_t*)(m + 40) = 0;
            }
            return got;
        }
        case LSYS_SHUTDOWN: {
            int fd = (int)ebx;
            LinuxProcess* lp = Current();
            if (fd < 0 || fd >= LINUX_MAX_FDS) return -9;
            if (lp->fds[fd].type == LFD_INET)
                return LinuxNetBridge::Shutdown(lp->fds[fd].backend_fd, (int)ecx);
            if (lp->fds[fd].type != LFD_SOCKET) return -9;
            return UnixSocket::Shutdown(lp->fds[fd].backend_fd, (int)ecx);
        }
        case LSYS_SETSOCKOPT: {
            // setsockopt(fd, level, optname, optval, optlen). we don't model
            // most options on the AF_UNIX backend, so accept the common ones
            // and return success rather than failing the caller. silently
            // ignoring an unknown option is also fine here - programs treat a
            // 0 return as "applied". (satoru)
            int fd = (int)ebx;
            LinuxProcess* lp = Current();
            if (fd < 0 || fd >= LINUX_MAX_FDS ||
                (lp->fds[fd].type != LFD_SOCKET &&
                 lp->fds[fd].type != LFD_INET)) return -9;   // -EBADF
            // SOL_SOCKET(1): SO_REUSEADDR(2)/SO_SNDBUF(7)/SO_RCVBUF(8)/
            //   SO_KEEPALIVE(9)/SO_REUSEPORT(15); IPPROTO_TCP(6): TCP_NODELAY(1).
            // all accepted-and-ignored (inet too - the stack has fixed
            // buffers/nagle-off behavior anyway). (satoru)
            return 0;
        }
        case LSYS_GETSOCKOPT: {
            // getsockopt(fd, level, optname, optval*, optlen*). return sane
            // int values for the options programs probe after connect() so
            // non-blocking connect loops don't spin on a bogus SO_ERROR. write
            // the int into the user optval and 4 into the user optlen. (satoru)
            int fd       = (int)ebx;
            int level    = (int)ecx;
            int optname  = (int)edx;
            int* optval  = (int*)(uintptr_t)esi;
            uint32_t* optlen = (uint32_t*)(uintptr_t)edi;
            LinuxProcess* lp = Current();
            if (fd < 0 || fd >= LINUX_MAX_FDS ||
                (lp->fds[fd].type != LFD_SOCKET &&
                 lp->fds[fd].type != LFD_INET)) return -9;   // -EBADF
            if (!optval || !optlen) return -14;              // -EFAULT
            if (*optlen < sizeof(int)) return -22;           // -EINVAL
            bool is_inet = (lp->fds[fd].type == LFD_INET);
            int val = 0;
            if (level == 1) {                 // SOL_SOCKET
                switch (optname) {
                    case 4:                       // SO_ERROR: the non-blocking
                        // connect verdict - 0 / ECONNREFUSED / ETIMEDOUT. (satoru)
                        val = is_inet ? LinuxNetBridge::SockError(lp->fds[fd].backend_fd) : 0;
                        break;
                    case 3:  val = 1;     break;  // SO_TYPE   -> SOCK_STREAM
                    case 7:                       // SO_SNDBUF
                    case 8:  val = 65536; break;  // SO_RCVBUF -> sane default
                    case 2:                       // SO_REUSEADDR
                    case 9:                       // SO_KEEPALIVE
                    case 15: val = 0;     break;  // SO_REUSEPORT (default off)
                    default: val = 0;     break;
                }
            } else if (level == 6) {          // IPPROTO_TCP
                // TCP_NODELAY(1) and friends: report default off. (satoru)
                val = 0;
            } else {
                val = 0;
            }
            *optval = val;
            *optlen = sizeof(int);
            return 0;
        }
        case LSYS_GETSOCKNAME: {
            int fd = (int)ebx;
            uint8_t* sa = (uint8_t*)(uintptr_t)ecx;
            LinuxProcess* lp = Current();
            if (!sa || fd < 0 || fd >= LINUX_MAX_FDS) return -9;
            if (lp->fds[fd].type == LFD_INET) {
                LinuxSockaddrIn nsin = {};
                if (LinuxNetBridge::Getsockname(lp->fds[fd].backend_fd, &nsin) < 0) return -9;
                write_sin(sa, LinuxNetBridge::Ntohl(nsin.sin_addr),
                          LinuxNetBridge::Ntohs(nsin.sin_port));
                uint32_t* alen = (uint32_t*)(uintptr_t)edx;
                if (alen) *alen = 16;
                return 0;
            }
            if (lp->fds[fd].type != LFD_SOCKET) return -9;
            sa[0] = 1; sa[1] = 0;        // AF_UNIX
            return UnixSocket::GetSockName(lp->fds[fd].backend_fd,
                                           (char*)(sa + 2), 108);
        }
        case LSYS_GETPEERNAME: {
            int fd = (int)ebx;
            uint8_t* sa = (uint8_t*)(uintptr_t)ecx;
            LinuxProcess* lp = Current();
            if (!sa || fd < 0 || fd >= LINUX_MAX_FDS) return -9;
            if (lp->fds[fd].type == LFD_INET) {
                LinuxSockaddrIn psin = {};
                if (LinuxNetBridge::Getpeername(lp->fds[fd].backend_fd, &psin) < 0)
                    return -107;                         // -ENOTCONN (satoru)
                write_sin(sa, LinuxNetBridge::Ntohl(psin.sin_addr),
                          LinuxNetBridge::Ntohs(psin.sin_port));
                uint32_t* alen = (uint32_t*)(uintptr_t)edx;
                if (alen) *alen = 16;
                return 0;
            }
            if (lp->fds[fd].type != LFD_SOCKET) return -9;
            sa[0] = 1; sa[1] = 0;
            return UnixSocket::GetPeerName(lp->fds[fd].backend_fd,
                                           (char*)(sa + 2), 108);
        }
        case LSYS_SOCKETPAIR: {
            (void)ebx; (void)ecx; (void)edx;
            int* sv = (int*)(uintptr_t)esi;
            if (!sv) return -14;
            int sd0, sd1;
            if (UnixSocket::Pair(UnixSocket::UNIX_SOCK_STREAM, &sd0, &sd1) < 0)
                return -24;
            LinuxProcess* lp = Current();
            int fd0 = AllocFd(lp);
            int fd1 = AllocFd(lp);
            if (fd0 < 0 || fd1 < 0) {
                UnixSocket::Close(sd0); UnixSocket::Close(sd1);
                return -24;
            }
            memset(&lp->fds[fd0], 0, sizeof(LinuxFd));
            memset(&lp->fds[fd1], 0, sizeof(LinuxFd));
            lp->fds[fd0].type = LFD_SOCKET;
            lp->fds[fd0].backend_fd = sd0;
            lp->fds[fd0].open = true;
            lp->fds[fd1].type = LFD_SOCKET;
            lp->fds[fd1].backend_fd = sd1;
            lp->fds[fd1].open = true;
            sv[0] = fd0; sv[1] = fd1;
            return 0;
        }

        // ════════════════════════════════════════════════════════════════
        //  x86_64 ABI completeness build-out (satoru)
        //  these are routed here from the real amd64 numbers via the kNrMap
        //  in linux_syscall_x64.cpp. they return real values where the kernel
        //  has the state, a harmless 0 where a no-op is correct, and -ENOSYS
        //  (logged) only where the app genuinely cannot proceed. (satoru)
        // ════════════════════════════════════════════════════════════════

        // ── Tier 1: io/transfer + sandbox plumbing ──────────────────────
        // vmsplice(fd, iov, nr_segs, flags): we cannot share user pages into a
        // pipe, so report "consumed 0" which callers treat as a short splice and
        // fall back to write(). returning -ENOSYS would break gzip/coreutils that
        // probe vmsplice once then fall back. (satoru)
        case LSYS_VMSPLICE:
            return 0;

        // openat2(dirfd, path, open_how*, size): open_how = { flags, mode,
        // resolve }. resolve flags (RESOLVE_NO_SYMLINKS etc.) are advisory here;
        // pull flags+mode out of the struct and route to open. (satoru)
        case LSYS_OPENAT2: {
            uint64_t how = esi;   // struct open_how* (satoru)
            uint64_t flags = 0, mode = 0;
            if (how) {
                flags = *(uint64_t*)(uintptr_t)how;
                mode  = *(uint64_t*)(uintptr_t)(how + 8);
            }
            return sys_open(edx, (uint32_t)flags, (uint32_t)mode);
        }

        // pidfd_open(pid, flags): hand back a real fd referring to the target.
        // we store the pid in offset so pidfd_send_signal/getfd can read it. (satoru)
        case LSYS_PIDFD_OPEN_: {
            LinuxProcess* lp = Current();
            if (!lp) return -1;
            int fd = AllocFd(lp);
            if (fd < 0) return -24;
            memset(&lp->fds[fd], 0, sizeof(LinuxFd));
            lp->fds[fd].type = LFD_DEVNULL;   // readable-as-empty pid handle (satoru)
            lp->fds[fd].offset = ebx;          // remember the target pid (satoru)
            lp->fds[fd].open = true;
            return fd;
        }
        // pidfd_send_signal(pidfd, sig, info, flags): resolve the pidfd's stored
        // pid and post the signal to that linux process. (satoru)
        case LSYS_PIDFD_SEND_SIG: {
            int pidfd = (int)ebx; int sig = (int)ecx;
            LinuxProcess* lp = Current();
            if (!lp || pidfd < 0 || pidfd >= LINUX_MAX_FDS || !lp->fds[pidfd].open)
                return -9;  // EBADF
            uint32_t pid = (uint32_t)lp->fds[pidfd].offset;
            for (int i = 0; i < LINUX_MAX_PROCS; i++)
                if (procs[i].active && procs[i].pid == pid) {
                    if (sig > 0 && sig < 64) procs[i].pending_signals |= (1u << sig);
                    return 0;
                }
            return -3;  // ESRCH
        }

        // fanotify_init / fanotify_mark: accept the call shape, hand back a real
        // fd for init so the group exists; marks are recorded as no-ops (no event
        // backend yet). callers that only watch (not block) keep working. (satoru)
        case LSYS_FANOTIFY_INIT_: {
            LinuxProcess* lp = Current();
            if (!lp) return -1;
            int fd = AllocFd(lp);
            if (fd < 0) return -24;
            memset(&lp->fds[fd], 0, sizeof(LinuxFd));
            lp->fds[fd].type = LFD_FANOTIFY;
            lp->fds[fd].open = true;
            return fd;
        }
        case LSYS_FANOTIFY_MARK_:
            return 0;

        // ── Tier 2: posix file/io ────────────────────────────────────────
        // sync_file_range / posix_fadvise / readahead: advisory, the kernel has
        // no page cache to act on, so success-as-no-op is the correct contract. (satoru)
        case LSYS_SYNC_FILE_RANGE:
        case LSYS_POSIX_FADVISE:
        case LSYS_READAHEAD:
            return 0;

        // dup3(oldfd, newfd, flags): like dup2 but with O_CLOEXEC in flags and an
        // error if oldfd==newfd. flags are ignored (no cloexec tracking). (satoru)
        case LSYS_DUP3: {
            int oldfd = (int)ebx, newfd = (int)ecx;
            if (oldfd == newfd) return -22;  // EINVAL per dup3 (satoru)
            return sys_dup2(oldfd, newfd);
        }

        // epoll_pwait2(epfd, events, maxevents, timespec*, sigmask): same as
        // epoll_wait but the timeout is a timespec*. convert to ms and route. (satoru)
        case LSYS_EPOLL_PWAIT2: {
            int timeout_ms = -1;
            if (edx /*maxevents*/ && esi) {
                const int64_t* ts = (const int64_t*)(uintptr_t)esi;
                timeout_ms = (int)(ts[0] * 1000 + ts[1] / 1000000);
            }
            return Dispatch(LSYS_EPOLL_WAIT, ebx, ecx, edx, (uint64_t)(int64_t)timeout_ms, 0);
        }

        // preadv / pwritev / preadv2 / pwritev2 (fd, iov, iovcnt, pos_lo, pos_hi):
        // seek to pos then run the scatter read/write. the *v2 forms add a flags
        // word we ignore. a negative offset means "use the current offset". (satoru)
        case LSYS_PREADV:
        case LSYS_PREADV2: {
            int fd = (int)ebx; uintptr_t iov = (uintptr_t)ecx; uint64_t cnt = edx;
            int64_t pos = (int64_t)esi;
            if (pos >= 0) sys_lseek(fd, (int32_t)pos, 0);
            return sys_readv(fd, iov, cnt);
        }
        case LSYS_PWRITEV:
        case LSYS_PWRITEV2: {
            int fd = (int)ebx; uintptr_t iov = (uintptr_t)ecx; uint64_t cnt = edx;
            int64_t pos = (int64_t)esi;
            if (pos >= 0) sys_lseek(fd, (int32_t)pos, 0);
            return sys_writev(fd, iov, cnt);
        }

        // ── Tier 3: scheduler / priority / capabilities ──────────────────
        // sched_setaffinity(pid, cpusetsize, mask): take the low byte of the user
        // mask and push it to the scheduler's per-task affinity bitmap. (satoru)
        case LSYS_SCHED_SETAFFINITY: {
            uint32_t pid = ebx;
            const uint8_t* mask = (const uint8_t*)(uintptr_t)edx;
            if (!mask) return -14;  // EFAULT
            LinuxProcess* lp = Current();
            if (pid == 0 && lp && lp->task) pid = lp->task->pid;
            uint8_t m = mask[0] ? mask[0] : 0x1;
            return Scheduler::SetAffinity(pid, m) == 0 ? 0 : -3;
        }
        // sched_getaffinity(pid, cpusetsize, mask): write the task's affinity
        // bitmap into the user mask; return the number of bytes written. (satoru)
        case LSYS_SCHED_GETAFFINITY: {
            uint32_t pid = ebx; uint32_t size = ecx;
            uint8_t* mask = (uint8_t*)(uintptr_t)edx;
            if (!mask || size == 0) return -14;
            LinuxProcess* lp = Current();
            if (pid == 0 && lp && lp->task) pid = lp->task->pid;
            uint8_t m = 0;
            if (Scheduler::GetAffinity(pid, &m) != 0) return -3;
            if (m == 0) m = 0x1;
            for (uint32_t i = 0; i < size; i++) mask[i] = (i == 0) ? m : 0;
            return (int32_t)size;
        }
        // sched_getscheduler: report the task's class (0=NORMAL,1=FIFO,2=RR,
        // 5=IDLE). sched_setscheduler/setattr accept the policy onto the task. (satoru)
        case LSYS_SCHED_GETSCHEDULER: {
            LinuxProcess* lp = Current();
            if (!lp || !lp->task) return 0;  // SCHED_NORMAL
            switch (lp->task->sched_class) {
                case 1: return 1;   // SCHED_FIFO
                case 2: return 2;   // SCHED_RR
                case 3: return 5;   // SCHED_IDLE
                default: return 0;  // SCHED_NORMAL
            }
        }
        case LSYS_SCHED_SETSCHEDULER: {
            int policy = (int)ecx;
            LinuxProcess* lp = Current();
            if (!lp || !lp->task) return 0;
            uint8_t cls = 0;
            if (policy == 1) cls = 1;        // SCHED_FIFO
            else if (policy == 2) cls = 2;   // SCHED_RR
            else if (policy == 5) cls = 3;   // SCHED_IDLE
            lp->task->sched_class = cls;
            return 0;
        }
        // sched_setattr/getattr: minimal - getattr fills sched_policy + sched_nice
        // from the task; setattr applies the policy. the struct is sched_attr. (satoru)
        case LSYS_SCHED_SETATTR: {
            LinuxProcess* lp = Current();
            if (!lp || !lp->task || !ecx) return -14;
            uint32_t* attr = (uint32_t*)(uintptr_t)ecx;
            uint32_t policy = attr[1];  // size,sched_policy,... (satoru)
            uint8_t cls = 0;
            if (policy == 1) cls = 1; else if (policy == 2) cls = 2; else if (policy == 5) cls = 3;
            lp->task->sched_class = cls;
            return 0;
        }
        case LSYS_SCHED_GETATTR: {
            LinuxProcess* lp = Current();
            uint8_t* attr = (uint8_t*)(uintptr_t)ecx;
            uint32_t size = edx;
            if (!attr || size < 48) return -22;
            for (uint32_t i = 0; i < size; i++) attr[i] = 0;
            uint32_t* w = (uint32_t*)attr;
            w[0] = size;  // sched_attr.size
            if (lp && lp->task) {
                uint32_t pol = 0;
                if (lp->task->sched_class == 1) pol = 1;
                else if (lp->task->sched_class == 2) pol = 2;
                else if (lp->task->sched_class == 3) pol = 5;
                w[1] = pol;                                   // sched_policy
                *(int32_t*)(attr + 20) = lp->task->nice;      // sched_nice (offset 20) (satoru)
            }
            return 0;
        }
        // sched_setparam/getparam: only realtime classes carry a priority; we
        // store nothing extra, so getparam reports priority 0 and setparam ok. (satoru)
        case LSYS_SCHED_SETPARAM:
            return 0;
        case LSYS_SCHED_GETPARAM: {
            int* prio = (int*)(uintptr_t)ecx;  // struct sched_param { int sched_priority; } (satoru)
            if (prio) *prio = 0;
            return 0;
        }
        // sched_get_priority_max/min(policy): SCHED_FIFO/RR span 1..99, the other
        // classes are 0..0 - the values glibc/musl validate against. (satoru)
        case LSYS_SCHED_GET_PRIORITY_MAX: {
            int policy = (int)ebx;
            return (policy == 1 || policy == 2) ? 99 : 0;
        }
        case LSYS_SCHED_GET_PRIORITY_MIN: {
            int policy = (int)ebx;
            return (policy == 1 || policy == 2) ? 1 : 0;
        }
        // sched_rr_get_interval(pid, timespec*): report the RR quantum (the PIT
        // timeslice, ~10ms) so RR apps size their loops sanely. (satoru)
        case LSYS_SCHED_RR_GET_INTERVAL: {
            int64_t* ts = (int64_t*)(uintptr_t)ecx;
            if (ts) { ts[0] = 0; ts[1] = 10 * 1000000; }  // 10ms (satoru)
            return 0;
        }
        // getpriority(which, who): nice is -20..19; getpriority returns it as the
        // already-biased value the kernel ABI uses (20 - nice). setpriority pushes
        // the requested nice onto the current task. PRIO_PROCESS only. (satoru)
        case LSYS_GETPRIORITY: {
            LinuxProcess* lp = Current();
            int nice = (lp && lp->task) ? lp->task->nice : 0;
            return 20 - nice;  // kernel returns 20-nice so the value is non-negative (satoru)
        }
        case LSYS_SETPRIORITY: {
            int prio = (int)edx;  // requested nice value (satoru)
            LinuxProcess* lp = Current();
            if (lp && lp->task) {
                if (prio < -20) prio = -20; else if (prio > 19) prio = 19;
                lp->task->nice = prio;
            }
            return 0;
        }
        // ioprio_set/get(which, who[, ioprio]): no block-io scheduler classes, so
        // get reports the default best-effort class, set is accepted. (satoru)
        case LSYS_IOPRIO_SET:
            return 0;
        case LSYS_IOPRIO_GET:
            return (2 << 13);  // IOPRIO_CLASS_BE << IOPRIO_CLASS_SHIFT, prio 0 (satoru)
        case LSYS_SCHED_YIELD: {
            // [yld] a firefox thread spinning on sched_yield never reaches its event
            // loop (invisible to [stk]/[pw]). dump its stack once after it clearly
            // spins, to catch the orphaned launch pump (fd 8). (satoru)
            LinuxProcess* yp = Current();
            if (yp && yp->pid >= 100 && yp->pid < 140 && yp->task) {
                static int yc[140] = {0};
                static bool yd[140] = {false};
                if (++yc[yp->pid] >= 500 && !yd[yp->pid]) {
                    yd[yp->pid] = true;
                    Process* yt = yp->task;
                    SerialLogger::Log("[yld] === PID="); SerialLogger::LogDec(yp->pid);
                    SerialLogger::Log(" rip=");
                    SerialLogger::LogHex((uint32_t)(yt->user_frame.rip >> 32));
                    SerialLogger::Log(":"); SerialLogger::LogHex((uint32_t)(yt->user_frame.rip & 0xFFFFFFFFu));
                    SerialLogger::Log("\r\n");
                    for (int i = 0; i < 56; i++) {
                        uint64_t v = 0;
                        if (read_user_u64(yt, yt->user_frame.rsp + (uint64_t)i*8, &v) &&
                            v > 0x10000ULL && v < 0x800000000000ULL) {
                            SerialLogger::Log("[yld] +"); SerialLogger::LogDec(i*8);
                            SerialLogger::Log("=");
                            SerialLogger::LogHex((uint32_t)(v >> 32));
                            SerialLogger::Log(":"); SerialLogger::LogHex((uint32_t)(v & 0xFFFFFFFFu));
                            SerialLogger::Log("\r\n");
                        }
                    }
                }
            }
            // sched_yield must actually hand the cpu to a READY SIBLING user thread.
            // Scheduler::Yield()->Schedule() only reschedules KERNEL procs (YieldNow even
            // early-returns for non-kernel procs) and returns to the SAME firefox thread,
            // so a userspace yield-spin (parking_lot/rayon waiting on a sibling) starves
            // that sibling and LIVELOCKS - seen as ~9 threads stuck Ready at this exact
            // sched_yield stub with zero forward progress, the systemic wall firefox kept
            // hitting. mirror the timer-preempt + futex path: save our frame so we resume
            // returning 0, then switch_to_ready_user picks the next ready sibling and the
            // syscall's iretq resumes IT; we are re-queued and run again on our turn. this
            // is what lets firefox's heavily-yielding startup actually make progress on a
            // single cpu. (satoru)
            Process* yt2 = Scheduler::GetCurrentProcess();
            if (yt2 && current_syscall_frame) {
                current_syscall_frame->rax = 0;                 // resume value of sched_yield (satoru)
                Scheduler::SaveUserFrame(yt2, current_syscall_frame);
                if (switch_to_ready_user(current_syscall_frame)) return 0;
            }
            // the kernel-proc yield is bsp-only; an ap with no ready sibling just
            // returns to the caller (it re-yields on its next spin). (satoru)
            if (SMP::CpuIndex() == 0) Scheduler::Yield();
            return 0;
        }
        // capget/capset: report an empty (no-capabilities) set for capget and
        // accept capset. the v3 header is { version, pid }; data is two
        // __user_cap_data{ effective, permitted, inheritable }. (satoru)
        case LSYS_CAPGET: {
            uint32_t* data = (uint32_t*)(uintptr_t)ecx;
            if (data) { for (int i = 0; i < 6; i++) data[i] = 0; }
            return 0;
        }
        case LSYS_CAPSET:
            return 0;
        case LSYS_PERSONALITY:
            return 0;  // report/accept ADDR_NO_RANDOMIZE etc. as no-op (satoru)

        // ── Tier 4: filesystem at-family + xattr ─────────────────────────
        // linkat(olddirfd, old, newdirfd, new, flags): kvfs has no hardlinks, so
        // copy the source to the destination path (AT_FDCWD only). (satoru)
        case LSYS_LINKAT: {
            const char* oldp = (const char*)(uintptr_t)ecx;
            const char* newp = (const char*)(uintptr_t)esi;
            if (!oldp || !newp) return -14;
            LinuxProcess* lp = Current();
            char ro[256], rn[256];
            ResolvePath(oldp, ro, sizeof(ro), lp);
            ResolvePath(newp, rn, sizeof(rn), lp);
            return KVFS::Copy(ro, rn) == 0 ? 0 : -2;
        }
        // symlinkat(target, newdirfd, linkpath): create a real kvfs symlink. (satoru)
        case LSYS_SYMLINKAT: {
            const char* target = (const char*)(uintptr_t)ebx;
            const char* linkp  = (const char*)(uintptr_t)edx;
            if (!target || !linkp) return -14;
            LinuxProcess* lp = Current();
            char rl[256];
            ResolvePath(linkp, rl, sizeof(rl), lp);
            return KVFS::Symlink(rl, target) == 0 ? 0 : -2;
        }
        // fchmodat(dirfd, path, mode, flags): resolve + chmod through kvfs. (satoru)
        case LSYS_FCHMODAT: {
            const char* path = (const char*)(uintptr_t)ecx;
            uint32_t mode = edx;
            if (!path) return -14;
            LinuxProcess* lp = Current();
            char rp[256];
            ResolvePath(path, rp, sizeof(rp), lp);
            return KVFS::Chmod(rp, (uint16_t)(mode & 07777)) == 0 ? 0 : -2;
        }
        // faccessat / faccessat2(dirfd, path, mode[, flags]): existence + perm
        // check via the path-based access handler (dirfd AT_FDCWD assumed). (satoru)
        case LSYS_FACCESSAT:
            return sys_access(ecx, edx);
        case LSYS_FACCESSAT2:
            return sys_access(ecx, edx);
        // utimensat(dirfd, path, times[2], flags) / futimesat(dirfd, path,
        // times[2]): touch the node's accessed/modified stamps. a null path with
        // utimensat means "the dirfd itself" which we cannot stat, so accept. (satoru)
        case LSYS_UTIMENSAT: {
            const char* path = (const char*)(uintptr_t)ecx;
            if (!path) return 0;  // fd-relative form: accept (satoru)
            LinuxProcess* lp = Current();
            char rp[256];
            ResolvePath(path, rp, sizeof(rp), lp);
            KVFSNode* n = KVFS::Resolve(rp);
            if (!n) return -2;
            uint32_t now = (uint32_t)(Timer::GetRealMs() / 1000);
            n->accessed = now; n->modified = now;
            return 0;
        }
        case LSYS_FUTIMESAT: {
            const char* path = (const char*)(uintptr_t)ecx;
            if (!path) return 0;
            LinuxProcess* lp = Current();
            char rp[256];
            ResolvePath(path, rp, sizeof(rp), lp);
            KVFSNode* n = KVFS::Resolve(rp);
            if (!n) return -2;
            uint32_t now = (uint32_t)(Timer::GetRealMs() / 1000);
            n->accessed = now; n->modified = now;
            return 0;
        }
        // mount/umount2/swapon/swapoff/quotactl: single-root in-RAM fs; accept the
        // common no-op shapes. mount of a real fs is not supported; returning 0
        // keeps container/init scripts moving (they tolerate a flat namespace). (satoru)
        case LSYS_MOUNT:
        case LSYS_UMOUNT2:
        case LSYS_SWAPON:
        case LSYS_SWAPOFF:
        case LSYS_QUOTACTL:
            return 0;

        // xattr family - kvfs has no extended-attribute store. POSIX lets a fs
        // report "not supported" via ENOTSUP for set, and getxattr returns
        // -ENODATA when the named attr is absent (which is always here). these
        // are the values glibc/coreutils/ls expect and silently tolerate; they
        // are NOT fatal -ENOSYS. (satoru)
        case LSYS_SETXATTR:
        case LSYS_LSETXATTR:
        case LSYS_FSETXATTR:
            return -95;   // -EOPNOTSUPP (satoru)
        case LSYS_GETXATTR:
        case LSYS_LGETXATTR:
        case LSYS_FGETXATTR:
            return -61;   // -ENODATA (satoru)
        case LSYS_LISTXATTR:
        case LSYS_LLISTXATTR:
        case LSYS_FLISTXATTR:
            return 0;     // zero-length attribute-name list (satoru)
        case LSYS_REMOVEXATTR:
        case LSYS_LREMOVEXATTR:
        case LSYS_FREMOVEXATTR:
            return -61;   // -ENODATA (satoru)

        // ── Tier 5: networking multi-message ─────────────────────────────
        // sendmmsg/recvmmsg(fd, mmsghdr*, vlen, flags[, timeout]): loop the
        // single-message handler over the array and set each msg_len. (satoru)
        case LSYS_SENDMMSG:
        case LSYS_RECVMMSG: {
            int fd = (int)ebx;
            uint8_t* mmsg = (uint8_t*)(uintptr_t)ecx;  // struct mmsghdr[] (satoru)
            uint32_t vlen = edx; uint32_t flags = esi;
            if (!mmsg || vlen == 0) return -14;
            // struct mmsghdr = { struct msghdr msg_hdr; unsigned msg_len; }.
            // x86_64 msghdr is 56 bytes, so mmsghdr stride is 64 (4-byte msg_len
            // + 4 pad). we forward msg_hdr to the scalar sendmsg/recvmsg. (satoru)
            const uint32_t MSGHDR_SZ = 56, MMSG_STRIDE = 64;
            uint32_t done = 0;
            for (uint32_t i = 0; i < vlen; i++) {
                uint8_t* slot = mmsg + (uint64_t)i * MMSG_STRIDE;
                uint64_t hdr = (uint64_t)(uintptr_t)slot;
                int64_t r = (eax == LSYS_SENDMMSG)
                    ? Dispatch(LSYS_SENDMSG, (uint64_t)fd, hdr, flags, 0, 0)
                    : Dispatch(LSYS_RECVMSG, (uint64_t)fd, hdr, flags, 0, 0);
                if (r < 0) { if (done == 0) return (int32_t)r; break; }
                *(uint32_t*)(slot + MSGHDR_SZ) = (uint32_t)r;  // msg_len (satoru)
                done++;
            }
            return (int32_t)done;
        }

        // ── Tier 6: memory ───────────────────────────────────────────────
        // mremap(old, oldsz, newsz, flags, newaddr): only shrink-in-place and
        // grow-when-it-fits are honoured cheaply; otherwise we map a fresh region
        // and copy. MREMAP_MAYMOVE (bit1) gates the move. shrink/same is a no-op
        // that returns the same address. (satoru)
        case LSYS_MREMAP: {
            uintptr_t old_addr = (uintptr_t)ebx;
            uint64_t  old_sz   = ecx;
            uint64_t  new_sz   = edx;
            uint32_t  flags    = (uint32_t)esi;
            // (satoru) TEMP probe: log mremap addr+flags to see the musl stack-probe walk.
            { static uint64_t s_mr=0; if(false && ((++s_mr)&0x3F)==0){ LinuxProcess* _l=Current(); Process* _t=_l?_l->task:nullptr; SerialLogger::Log("[mremap] n="); SerialLogger::LogHex(s_mr); SerialLogger::Log(" addr="); SerialLogger::LogHex((uint64_t)old_addr); SerialLogger::Log(" fl="); SerialLogger::LogHex(flags); SerialLogger::Log(" o="); SerialLogger::LogHex(old_sz); SerialLogger::Log(" stktop="); SerialLogger::LogHex(_t?_t->user_stack_top:0); SerialLogger::Log("\r\n"); } }  // gated off (satoru)
            if (new_sz == 0) return -22;
            if (new_sz <= old_sz) return (int64_t)old_addr;  // shrink/same: in place (satoru)
            const uint32_t MREMAP_MAYMOVE = 1;
            if (!(flags & MREMAP_MAYMOVE)) {
                // in-place grow only. musl's pthread_getattr_np probes the main
                // thread's stack extent with mremap(page, PAGE, 2*PAGE, 0) walking
                // DOWN one page at a time: a MAPPED page can't grow in place (the
                // next page is occupied) and MUST return ENOMEM so the probe keeps
                // going, but an UNMAPPED page past the stack bottom MUST return a
                // different error (EFAULT) so the loop TERMINATES. returning ENOMEM
                // unconditionally span that loop forever -> the firefox chrome main
                // thread wedged in nsThread::InitCommon -> pthread_getattr_np.
                // distinguish by whether old_addr is actually mapped. (satoru)
                LinuxProcess* lp = Current();
                Process* tk = lp ? lp->task : nullptr;
                uint64_t stk_top = tk ? tk->user_stack_top : 0;
                if (stk_top) {
                    // musl walks DOWN the stack; ENOMEM while still inside the 8MB
                    // user stack (can't grow in place), EFAULT once past the base so
                    // the probe TERMINATES at the real stack bottom -- not all the way
                    // down through kurono's identity-mapped low memory, which over-
                    // sized the stack to ~1GB (base 0x1000) on the QueryMapping pass.
                    // (satoru)
                    uint64_t stk_base = ((stk_top + PAGE_SIZE) & ~(uint64_t)(PAGE_SIZE - 1)) - (8ULL * 1024 * 1024);
                    return (old_addr < stk_base) ? -14 : -12;
                }
                // no task info (e.g. an ap-dispatched thread whose linux current
                // hasn't synced): DON'T return ENOMEM unconditionally - musl's
                // getattr_np walk then never sees the terminating EFAULT and spins
                // forever, saturating a core (the KX5:6 pre-getattr_np stall). fall
                // back to the FIXED kurono main-stack layout (top USER_STACK_TOP =
                // 0x40200000, base = top-8MB) so the probe still terminates. (satoru)
                {
                    uint64_t stk_base_default = 0x40200000ULL - (8ULL * 1024 * 1024);
                    return (old_addr < stk_base_default) ? -14 : -12;
                }
            }
            // allocate a fresh anonymous region big enough, copy the old bytes in,
            // and release the old mapping. PROT_READ|WRITE, MAP_PRIVATE|ANON. (satoru)
            int64_t neu = sys_mmap(0, new_sz, 0x3, 0x22, -1, 0);
            if (neu < 0) return neu;
            memcpy((void*)(uintptr_t)neu, (void*)old_addr, old_sz);
            sys_munmap(old_addr, old_sz);
            return neu;
        }
        // mlock/munlock/mlockall/munlockall: nothing is ever paged out (no swap),
        // so pages are effectively always resident - accept as success. (satoru)
        case LSYS_MLOCK:
        case LSYS_MUNLOCK:
        case LSYS_MLOCKALL:
        case LSYS_MUNLOCKALL:
            return 0;
        // mincore(addr, length, vec): every mapped page is resident; set bit0 of
        // each vec byte. length is rounded up to pages of 4096. (satoru)
        case LSYS_MINCORE: {
            uint64_t length = ecx;
            uint8_t* vec = (uint8_t*)(uintptr_t)edx;
            if (!vec) return -14;
            uint64_t pages = (length + 4095) / 4096;
            for (uint64_t i = 0; i < pages; i++) vec[i] = 1;  // resident (satoru)
            return 0;
        }
        // memfd_secret(flags): a secret memory fd. we back it with the same anon
        // file machinery as memfd_create (no hardware secrecy), so the fd is
        // mmap-able. route to the existing memfd handler. (satoru)
        case LSYS_MEMFD_SECRET:
            return Dispatch(LSYS_MEMFD_CREATE, ebx, 0, 0, 0, 0);
        // userfaultfd(flags): hand back an fd so libc's uffd probe succeeds; we
        // never deliver fault events, which callers tolerate (they fall back to
        // SIGSEGV handling). (satoru)
        case LSYS_USERFAULTFD: {
            LinuxProcess* lp = Current();
            if (!lp) return -1;
            int fd = AllocFd(lp);
            if (fd < 0) return -24;
            memset(&lp->fds[fd], 0, sizeof(LinuxFd));
            lp->fds[fd].type = LFD_EVENTFD;  // readable-as-empty (satoru)
            lp->fds[fd].open = true;
            return fd;
        }
        // NUMA policy calls: single memory node, so set/get are no-ops and
        // get_mempolicy reports node 0 / default policy. migrate/move report the
        // pages as already on the only node. (satoru)
        case LSYS_MBIND:
        case LSYS_SET_MEMPOLICY:
            return 0;
        case LSYS_GET_MEMPOLICY: {
            int* mode = (int*)(uintptr_t)ebx;
            if (mode) *mode = 0;  // MPOL_DEFAULT (satoru)
            return 0;
        }
        case LSYS_MIGRATE_PAGES:
            return 0;  // nothing to migrate (single node) (satoru)
        case LSYS_MOVE_PAGES: {
            // move_pages(pid, count, pages, nodes, status, flags): if a status
            // array is given, report every page on node 0. (satoru)
            uint64_t count = ecx;
            int* status = (int*)(uintptr_t)edi;
            if (status) for (uint64_t i = 0; i < count; i++) status[i] = 0;
            return 0;
        }

        // ── Tier 7: signals ──────────────────────────────────────────────
        // rt_sigpending(set, sigsetsize): report this process's pending mask. (satoru)
        case LSYS_RT_SIGPENDING: {
            uint64_t* set = (uint64_t*)(uintptr_t)ebx;
            LinuxProcess* lp = Current();
            if (set) *set = lp ? lp->pending_signals : 0;
            return 0;
        }
        // rt_sigtimedwait(set, info, timeout, sigsetsize): no real signal delivery
        // path, so behave like the timeout always elapses with nothing pending. (satoru)
        case LSYS_RT_SIGTIMEDWAIT:
            return -11;  // -EAGAIN (timed out, no signal) (satoru)
        // rt_sigqueueinfo(tgid, sig, info) / rt_tgsigqueueinfo(tgid, tid, sig,
        // info): post the signal bit to the target process. (satoru)
        case LSYS_RT_SIGQUEUEINFO: {
            uint32_t tgid = ebx; int sig = (int)ecx;
            for (int i = 0; i < LINUX_MAX_PROCS; i++)
                if (procs[i].active && procs[i].pid == tgid) {
                    if (sig > 0 && sig < 64) procs[i].pending_signals |= (1u << sig);
                    return 0;
                }
            return -3;
        }
        case LSYS_RT_TGSIGQUEUEINFO: {
            int tid = (int)ecx; int sig = (int)edx;
            for (int i = 0; i < LINUX_MAX_PROCS; i++)
                if (procs[i].active && (int)procs[i].pid == tid) {
                    if (sig > 0 && sig < 64) procs[i].pending_signals |= (1u << sig);
                    return 0;
                }
            return -3;
        }
        // kill(pid, sig) / tkill(tid, sig): post the signal bit. pid<=0 group
        // forms are treated as "current process". sig 0 = existence probe. (satoru)
        case LSYS_KILL_:
        case LSYS_TKILL: {
            int pid = (int)ebx; int sig = (int)ecx;
            LinuxProcess* lp = Current();
            if (pid <= 0) {  // self / process-group: target current (satoru)
                if (lp) { if (sig > 0 && sig < 64) lp->pending_signals |= (1u << sig); return 0; }
                return -3;
            }
            for (int i = 0; i < LINUX_MAX_PROCS; i++)
                if (procs[i].active && (int)procs[i].pid == pid) {
                    if (sig > 0 && sig < 64) procs[i].pending_signals |= (1u << sig);
                    return 0;
                }
            return -3;  // ESRCH
        }
        // pause(): block until a signal. with no async delivery we yield once and
        // return -EINTR so callers don't spin forever holding the cpu. (satoru)
        case LSYS_PAUSE:
            if (SMP::CpuIndex() == 0) {   // the kernel-proc yield is bsp-only (satoru)
                KuronoShell::PumpUI();
                Scheduler::Yield();
            } else {
                kls_relax();
            }
            return -4;  // -EINTR (satoru)

        // ── Tier 8: time ─────────────────────────────────────────────────
        // clock_settime: accept (we don't let userspace move the monotonic/real
        // clock backwards, but report success so date/ntp scripts proceed). (satoru)
        case LSYS_CLOCK_SETTIME:
            return 0;
        // getitimer/setitimer(which, new, old): no interval-timer SIGALRM backend
        // yet; clear the old value and accept. alarm() likewise returns 0 (no
        // previously-armed alarm). (satoru)
        case LSYS_GETITIMER: {
            uint8_t* old = (uint8_t*)(uintptr_t)ecx;  // struct itimerval (32 bytes) (satoru)
            if (old) for (int i = 0; i < 32; i++) old[i] = 0;
            return 0;
        }
        case LSYS_SETITIMER: {
            uint8_t* old = (uint8_t*)(uintptr_t)edx;
            if (old) for (int i = 0; i < 32; i++) old[i] = 0;
            return 0;
        }
        case LSYS_ALARM:
            return 0;  // no alarm was previously set (satoru)
        // times(tms*): report cumulative cpu time of the calling task in the tms
        // struct and the clock tick count as the return value (CLK_TCK=100). (satoru)
        case LSYS_TIMES: {
            uint64_t* tms = (uint64_t*)(uintptr_t)ebx;  // tms{utime,stime,cutime,cstime} (satoru)
            LinuxProcess* lp = Current();
            uint64_t ticks = lp && lp->task ? (lp->task->cpu_ms_total / 10) : 0;  // 100 Hz (satoru)
            if (tms) { tms[0] = ticks; tms[1] = 0; tms[2] = 0; tms[3] = 0; }
            return (int32_t)(Timer::GetRealMs() / 10);
        }
        // adjtimex/clock_adjtime: no kernel clock discipline; report TIME_OK and
        // accept the call so chrony/ntpd init does not abort. (satoru)
        case LSYS_ADJTIMEX:
        case LSYS_CLOCK_ADJTIME:
            return 0;  // TIME_OK (satoru)

        // ── Tier 9: user / group identity ────────────────────────────────
        // the LinuxProcess carries uid/gid/euid/egid. setuid/setgid set all of the
        // real+effective (root can, and we run as root); the get-forms return them.
        // supplementary groups + fs-uid are not tracked, so those accept/report
        // sane constants. (satoru)
        case LSYS_SETUID_: {
            LinuxProcess* lp = Current();
            if (lp) { lp->uid = ebx; lp->euid = ebx; }
            return 0;
        }
        case LSYS_SETGID_: {
            LinuxProcess* lp = Current();
            if (lp) { lp->gid = ebx; lp->egid = ebx; }
            return 0;
        }
        case LSYS_SETREUID_: {
            LinuxProcess* lp = Current();
            if (lp) {
                if ((int)ebx != -1) lp->uid = ebx;
                if ((int)ecx != -1) lp->euid = ecx;
            }
            return 0;
        }
        case LSYS_SETREGID_: {
            LinuxProcess* lp = Current();
            if (lp) {
                if ((int)ebx != -1) lp->gid = ebx;
                if ((int)ecx != -1) lp->egid = ecx;
            }
            return 0;
        }
        case LSYS_SETRESUID: {
            LinuxProcess* lp = Current();
            if (lp) {
                if ((int)ebx != -1) lp->uid  = ebx;
                if ((int)ecx != -1) lp->euid = ecx;
            }
            return 0;
        }
        case LSYS_SETRESGID: {
            LinuxProcess* lp = Current();
            if (lp) {
                if ((int)ebx != -1) lp->gid  = ebx;
                if ((int)ecx != -1) lp->egid = ecx;
            }
            return 0;
        }
        case LSYS_GETRESUID: {
            uint32_t* ruid = (uint32_t*)(uintptr_t)ebx;
            uint32_t* euid = (uint32_t*)(uintptr_t)ecx;
            uint32_t* suid = (uint32_t*)(uintptr_t)edx;
            LinuxProcess* lp = Current();
            uint32_t r = lp ? lp->uid : 0, e = lp ? lp->euid : 0;
            if (ruid) *ruid = r;
            if (euid) *euid = e;
            if (suid) *suid = e;   // no saved-uid tracked; report effective (satoru)
            return 0;
        }
        case LSYS_GETRESGID: {
            uint32_t* rgid = (uint32_t*)(uintptr_t)ebx;
            uint32_t* egid = (uint32_t*)(uintptr_t)ecx;
            uint32_t* sgid = (uint32_t*)(uintptr_t)edx;
            LinuxProcess* lp = Current();
            uint32_t r = lp ? lp->gid : 0, e = lp ? lp->egid : 0;
            if (rgid) *rgid = r;
            if (egid) *egid = e;
            if (sgid) *sgid = e;   // no saved-gid tracked; report effective (satoru)
            return 0;
        }
        // setfsuid/setfsgid return the PREVIOUS fsuid/fsgid (== euid/egid here). (satoru)
        case LSYS_SETFSUID: {
            LinuxProcess* lp = Current();
            return lp ? (int32_t)lp->euid : 0;
        }
        case LSYS_SETFSGID: {
            LinuxProcess* lp = Current();
            return lp ? (int32_t)lp->egid : 0;
        }
        // getgroups(size, list): no supplementary groups -> return 0 (count). a
        // size of 0 is a "how many?" probe, also 0. (satoru)
        case LSYS_GETGROUPS:
            return 0;
        // setgroups(size, list): accept (we don't track supplementary groups). (satoru)
        case LSYS_SETGROUPS:
            return 0;

        // ── Tier 10: system info ─────────────────────────────────────────
        // (syslog is already implemented above at LSYS_SYSLOG; the x64 path
        //  routes amd64 nr 103 to it via kNrMap, so it is now reachable. (satoru))
        // acct(path): process accounting off; accept enabling/disabling. (satoru)
        case LSYS_ACCT:
            return 0;
        // sysfs(option, ...): legacy fs-type enumeration. option 3 = count of
        // filesystem types; report 1 (our single kvfs). others accept. (satoru)
        case LSYS_SYSFS: {
            int option = (int)ebx;
            if (option == 3) return 1;
            return 0;
        }
        // ustat(dev, ubuf): legacy fs stat. zero the buffer + succeed. (satoru)
        case LSYS_USTAT: {
            uint8_t* ubuf = (uint8_t*)(uintptr_t)ecx;  // struct ustat (20 bytes) (satoru)
            if (ubuf) for (int i = 0; i < 20; i++) ubuf[i] = 0;
            return 0;
        }
        // statfs(path, buf) / fstatfs(fd, buf): fill a struct statfs describing the
        // single in-RAM kvfs. x86_64 struct statfs is 120 bytes; fields we set:
        // f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_namelen. (satoru)
        case LSYS_STATFS_:
        case LSYS_FSTATFS_: {
            uint64_t* sb = (uint64_t*)(uintptr_t)ecx;
            if (!sb) return -14;
            for (int i = 0; i < 15; i++) sb[i] = 0;  // 120 bytes (satoru)
            sb[0] = 0x858458f6ULL;     // f_type = RAMFS_MAGIC (low 32 bits) (satoru)
            sb[1] = 4096;              // f_bsize (satoru)
            sb[2] = 262144;            // f_blocks (~1GB at 4k) (satoru)
            sb[3] = 131072;            // f_bfree (satoru)
            sb[4] = 131072;            // f_bavail (satoru)
            sb[5] = 65536;             // f_files (satoru)
            sb[6] = 60000;             // f_ffree (satoru)
            // sb[7] = f_fsid (2x int32), sb[8] = f_namelen, ... (satoru)
            *(uint64_t*)((uint8_t*)sb + 56) = 255;  // f_namelen at offset 56 (satoru)
            return 0;
        }

        // ── Tier 11: advanced / debug ────────────────────────────────────
        // ptrace(request, pid, addr, data): no trap/single-step delivery
        // mechanism exists in this cooperative kernel, so the tracer-side
        // requests cannot be honoured. PTRACE_TRACEME (0) is accepted (a child
        // setting itself traceable is harmless); everything else returns -ENOSYS
        // and is logged so a debugger's failure is visible. (satoru)
        case LSYS_PTRACE: {
            int request = (int)ebx;
            if (request == 0) return 0;  // PTRACE_TRACEME (satoru)
            LogEnosys(101, "ptrace");
            return -38;  // -ENOSYS: no debugger trap mechanism (satoru)
        }
        // kexec_file_load: no in-place kernel replacement from a fd. accept the
        // staging call (the real reboot is handled elsewhere) as a no-op. (satoru)
        case LSYS_KEXEC_FILE_LOAD:
            return 0;
        // lookup_dcookie: kernel-profiler dentry cookie -> path. no dcookie db, so
        // report -ENOENT (the value oprofile tools tolerate). (satoru)
        case LSYS_LOOKUP_DCOOKIE:
            return -2;
        // add_key/request_key: kernel keyring. no keyring backend; report -ENOSYS
        // (logged) - callers fall back to userspace key storage. (satoru)
        case LSYS_ADD_KEY:
        case LSYS_REQUEST_KEY:
            LogEnosys(eax == LSYS_ADD_KEY ? 248 : 249,
                      eax == LSYS_ADD_KEY ? "add_key" : "request_key");
            return -38;

        // Linux x86_64 number synonyms - Firefox's static glibc emits
        // these directly via syscall().  We only expose numbers that do
        // not collide with the i386-style LSYS_* constants used elsewhere.
        case 43:  return Dispatch(LSYS_ACCEPT, ebx, ecx, edx, esi, edi);
        case 44:  return Dispatch(LSYS_SENDTO, ebx, ecx, edx, esi, edi);
        case 51:  return Dispatch(LSYS_GETSOCKNAME, ebx, ecx, edx, esi, edi);
        case 52:  return Dispatch(LSYS_GETPEERNAME, ebx, ecx, edx, esi, edi);
        case 53:  return Dispatch(LSYS_SOCKETPAIR, ebx, ecx, edx, esi, edi);
        case 288: return Dispatch(LSYS_ACCEPT4, ebx, ecx, edx, esi, edi);
        case 436: return Dispatch(LSYS_CLOSE_RANGE, ebx, ecx, edx, esi, edi);
        case 435: return Dispatch(LSYS_CLONE3, ebx, ecx, edx, esi, edi);
        case 438: return Dispatch(LSYS_PIDFD_GETFD, ebx, ecx, edx, esi, edi);
        case 444: return Dispatch(LSYS_LANDLOCK_CREATE, ebx, ecx, edx, esi, edi);
        case 445: return Dispatch(LSYS_LANDLOCK_ADD, ebx, ecx, edx, esi, edi);
        case 446: return Dispatch(LSYS_LANDLOCK_RESTRICT, ebx, ecx, edx, esi, edi);
        case 425: return Dispatch(LSYS_IO_URING_SETUP, ebx, ecx, edx, esi, edi);
        case 426: return Dispatch(LSYS_IO_URING_ENTER, ebx, ecx, edx, esi, edi);
        case 427: return Dispatch(LSYS_IO_URING_REGISTER, ebx, ecx, edx, esi, edi);
        case 317: return Dispatch(LSYS_SECCOMP, ebx, ecx, edx, esi, edi);
        case 310: return Dispatch(LSYS_PROCESS_VM_READV, ebx, ecx, edx, esi, edi);
        case 311: return Dispatch(LSYS_PROCESS_VM_WRITEV, ebx, ecx, edx, esi, edi);
        case 324: return Dispatch(LSYS_MEMBARRIER, ebx, ecx, edx, esi, edi);
        case 334: return Dispatch(LSYS_RSEQ, ebx, ecx, edx, esi, edi);

        default:
            // permanent rate-limited enosys trace (eax here is the internal
            // dispatch id, which equals the i386 number on the int 0x80 path). (satoru)
            LogEnosys(eax, nullptr);
            return -38;  // enosys
    }
}

//  individual syscall implementations

int32_t LinuxSyscall::sys_exit(uint32_t code) {
    LinuxProcess* p = Current();
    if (p) {
        p->exit_code = (int)code;
        p->exited = true;
        SerialLogger::Log("[LinuxSyscall] Process exited: ");
        SerialLogger::LogDec((int)code);
        SerialLogger::Log(" pid="); SerialLogger::LogDec(p->pid);   // [texit] which thread (satoru)
        SerialLogger::Log("\r\n");

        if (p->task) {
            int current_index = current_proc;

            // clone child_cleartid: a thread exiting must zero *clear_child_tid
            // and futex-wake it so a joiner blocked in pthread_join wakes up.
            // the write goes through this task's (shared) address space. (satoru)
            if (p->task->clear_child_tid) {
                uint64_t ctid = p->task->clear_child_tid;
                write_user_u32(p->task, ctid, 0);
                futex_do_wake(p->task->address_space, (uintptr_t)ctid,
                              futex_phys_key(p->task, (uintptr_t)ctid),
                              0x7FFFFFFF, 0xFFFFFFFFu);
                p->task->clear_child_tid = 0;
            }

            // if a vfork child exits before execve (e.g. the exec failed), wake the
            // suspended vfork parent so it doesn't hang. (no-op if it already
            // execve'd - the link was cleared there.) (satoru)
            vfork_wake_parent(p->task);

            Scheduler::MarkProcessExited(p->task, (int)code);
            wake_waiting_parent(p, current_index);

            if (Userspace::IsActive() && current_syscall_frame) {
                if (!switch_to_ready_user(current_syscall_frame)) {
                    if (SMP::CpuIndex() != 0) {
                        // an ap's thread exited and nothing is claimable for this
                        // cpu: iret back into the ap dispatch loop. the userspace
                        // SESSION belongs to the bsp (its RunProcessWithArgs
                        // context) - unwinding it from an ap would longjmp across
                        // cores. (satoru)
                        SMP::ApIdleFrame(current_syscall_frame);
                        current_frame_rewritten = true;
                    } else {
                        current_frame_rewritten = true;
                        resume_userspace_session = true;
                        resume_userspace_exit_code = (int)code;
                    }
                }
            }
        } else {
            p->active = false;
        }
    }
    return 0;
}

int32_t LinuxSyscall::sys_fork() {
    LinuxProcess* parent = Current();
    Process* parent_task = Scheduler::GetCurrentProcess();
    if (!parent || !parent_task || !parent_task->is_user() || !current_syscall_frame) {
        return -38;
    }

    Process* child_task = Scheduler::CloneUserProcess(parent_task);
    if (!child_task) return -12;

    int child_idx = CreateProcess(parent->name, parent->uid, parent->gid);
    if (child_idx < 0) {
        Scheduler::DestroyProcess(child_task);
        return -12;
    }

    LinuxProcess* child = &procs[child_idx];
    child->euid = parent->euid;
    child->egid = parent->egid;
    child->brk_base = parent->brk_base;
    child->brk_current = parent->brk_current;
    child->brk_max = parent->brk_max;
    child->exit_code = -1;
    child->exited = false;
    child->task = child_task;
    child->signal_mask = parent->signal_mask;
    child->pending_signals = parent->pending_signals;
    ls_scpy(child->cwd, parent->cwd, sizeof(child->cwd));
    ls_scpy(child->name, parent->name, sizeof(child->name));
    clone_file_descriptors(parent, child);

    child_task->user_frame.rax = 0;
    child_task->rip = (uintptr_t)child_task->user_frame.rip;
    child_task->rsp = (uintptr_t)child_task->user_frame.rsp;
    child_task->rbp = (uintptr_t)child_task->user_frame.rbp;

    return (int32_t)child->pid;
}

int32_t LinuxSyscall::sys_exit_group(uint32_t code) {
    return sys_exit(code);
}

int32_t LinuxSyscall::sys_waitpid(uint32_t pid, uintptr_t status, uint32_t options) {
    (void)options;

    LinuxProcess* parent = Current();
    Process* parent_task = Scheduler::GetCurrentProcess();
    if (!parent || !parent_task || !parent_task->is_user()) return -10;

    int32_t requested_pid = (int32_t)pid;
    bool any_child = requested_pid <= 0;
    bool found_child = false;

    for (int i = 0; i < LINUX_MAX_PROCS; i++) {
        LinuxProcess* child = GetProcess(i);
        if (!child || child->ppid != parent->pid) continue;
        if (!any_child && child->pid != (uint32_t)requested_pid) continue;

        found_child = true;
        if (!child->exited || !child->task) continue;

        if (status) {
            write_user_u32(parent_task, status, ((uint32_t)child->exit_code & 0xFFU) << 8);
        }

        int32_t child_pid = (int32_t)child->pid;
        Scheduler::ReapProcess(child->task);
        child->task = nullptr;
        DestroyProcess(i);
        return child_pid;
    }

    if (!found_child) return -10;
    if (!current_syscall_frame) return -11;

    parent_task->waiting_for_child = true;
    parent_task->waiting_child_pid = any_child ? 0 : (uint32_t)requested_pid;
    parent_task->waiting_status_ptr = status;
    { uint64_t sf; Scheduler::StateLock(&sf); parent_task->state = Process_Blocked; Scheduler::StateUnlock(sf); }

    if (!switch_to_ready_user(current_syscall_frame)) {
        { uint64_t sf; Scheduler::StateLock(&sf); parent_task->state = Process_Running; Scheduler::StateUnlock(sf); }
        parent_task->waiting_for_child = false;
        parent_task->waiting_child_pid = 0;
        parent_task->waiting_status_ptr = 0;
        return -11;
    }

    return 0;
}

// x86_64 dynamic execve: load `resolved` (an elf64 dynamic PIE, e.g. firefox
// re-execing /proc/self/exe for a content process) via ld-kurono ExecPIE into a
// FRESH address space for the calling task, copying argv/envp out of the caller's
// still-active old AS first, then rewrite the syscall frame to the new entry.
// mirrors the i386 static path but for elf64 dynamic binaries. (satoru)
static int32_t execve_dynamic64(const char* resolved, uintptr_t argv_u, uintptr_t envp_u,
                                LinuxProcess* proc, Process* task) {
    int fsz = KVFS::GetFileSize(resolved);
    if (fsz <= 0) return -2;
    uint8_t* image = (uint8_t*)KernelHeap::Alloc((uint32_t)fsz);
    if (!image) return -12;
    if (KVFS::ReadFile(resolved, image, (uint32_t)fsz) != fsz) { KernelHeap::Free(image); return -2; }

    // copy argv/envp out of the caller's address space (still active here). (satoru)
    char* abuf = (char*)KernelHeap::Alloc(16384);
    char* ebuf = (char*)KernelHeap::Alloc(16384);
    const char** av = (const char**)KernelHeap::Alloc(128 * sizeof(char*));
    const char** ev = (const char**)KernelHeap::Alloc(256 * sizeof(char*));
    if (!abuf || !ebuf || !av || !ev) {
        KernelHeap::Free(image);
        if (abuf) KernelHeap::Free(abuf);
        if (ebuf) KernelHeap::Free(ebuf);
        if (av) KernelHeap::Free((void*)av);
        if (ev) KernelHeap::Free((void*)ev);
        return -12;
    }
    copy_user_strv(task, (uint64_t)argv_u, abuf, 16384, av, 127);
    copy_user_strv(task, (uint64_t)envp_u, ebuf, 16384, ev, 255);

    uint64_t old_as = task->address_space;
    uint64_t new_as = KernelVMM::CreateAddressSpace();
    if (!new_as) {
        KernelHeap::Free(image); KernelHeap::Free(abuf); KernelHeap::Free(ebuf);
        KernelHeap::Free((void*)av); KernelHeap::Free((void*)ev);
        return -12;
    }
    task->address_space = new_as;
    memset(task->regions, 0, sizeof(task->regions));
    task->next_mmap_base = USER_MMAP_BASE;
    // ACTIVATE new_as BEFORE ExecPIE so the loader's stack/segment writes land in
    // the new address space, not the still-active OLD one. for a vfork child the
    // old AS is SHARED with the suspended parent, so loading through it corrupts the
    // parent and leaves the child's new stack unpopulated/RO (the glxtest #PF:
    // stack present-but-RO + fs_base poison 0xDEAD). the initial firefox launch
    // activates before loading too; this execve path was the only one that didn't.
    // (satoru)
    KernelVMM::ActivateAddressSpace(new_as);
    HAL::SetKernelStack(task->kernel_stack_top);

    // map a FRESH writable user stack in new_as. ExecPIE only WRITES argv/envp/auxv
    // to task->user_stack_top - it does NOT map the stack (the initial launch maps
    // it in Scheduler::CreateUserProcess). a plain execve never exercised this, and
    // a vfork child's user_stack_top is the vfork child_stack (unmapped in new_as),
    // so the new image had no mapped stack -> the glxtest #PF (stack push faulted).
    // (satoru)
    {
        const uint64_t U_STACK_TOP   = 0x40200000ULL;
        const uint64_t U_STACK_BYTES = 8ULL * 1024 * 1024;
        void* ustk = PMM::AllocBytes((size_t)U_STACK_BYTES);
        if (!ustk) {
            task->address_space = old_as;
            KernelVMM::ActivateAddressSpace(old_as);
            KernelVMM::DestroyAddressSpace(new_as);
            KernelHeap::Free(image); KernelHeap::Free(abuf); KernelHeap::Free(ebuf);
            KernelHeap::Free((void*)av); KernelHeap::Free((void*)ev);
            return -12;
        }
        uint64_t sbase = U_STACK_TOP - U_STACK_BYTES;
        for (uint64_t off = 0; off < U_STACK_BYTES; off += PAGE_SIZE) {
            KernelVMM::MapPageInAddressSpace(new_as, sbase + off,
                                             (uint64_t)(uintptr_t)ustk + off,
                                             PTE_USER | PTE_WRITABLE);
        }
        task->user_stack_top = U_STACK_TOP - 16;
        // RESERVE the main-thread stack in the region table. without this the
        // mmap arena (choose_mmap_base grows up from USER_MMAP_BASE=0x20000000)
        // eventually reaches 0x3fa00000 and, seeing no region there, hands the
        // range to a fresh clone thread's stack mmap -> the font-info loader
        // thread's cmapdata buffer wrote straight through the chrome main
        // thread's live nsCaret frame = the deterministic stack-canary #gp that
        // blocked page paint. this is a plain reservation (no coalescing with
        // heap/mmap regions thanks to the distinct flag). (satoru)
        add_region(task, sbase, U_STACK_TOP, PTE_USER | PTE_WRITABLE,
                   USER_REGION_STACK);
    }

    // (satoru) [exv] trace: name every dynamic execve (which child re-execs).
    // gated off - one line per exec, low volume, flip to true to watch. (satoru)
    if (false) {
        SerialLogger::Log("[exv] pid="); SerialLogger::LogDec(proc->pid);
        SerialLogger::Log(" "); SerialLogger::Log(resolved);
        SerialLogger::Log("\r\n");
    }

    uint64_t entry = 0, rsp = 0;
    bool ok = LdKurono::ExecPIE(task, image, (uint64_t)fsz, resolved, av, ev,
                                proc->uid, proc->gid, &entry, &rsp);
    KernelHeap::Free(image); KernelHeap::Free(abuf); KernelHeap::Free(ebuf);
    KernelHeap::Free((void*)av); KernelHeap::Free((void*)ev);
    if (!ok) {
        SerialLogger::Log("execve: ld-kurono failed to load dynamic image\r\n");
        task->address_space = old_as;                 // keep the caller's AS live
        KernelVMM::ActivateAddressSpace(old_as);      // restore (we activated new_as above)
        KernelVMM::DestroyAddressSpace(new_as);
        return -8;
    }

    // new_as already activated + kernel stack set above (before ExecPIE). if this
    // task is a vfork child (posix_spawn), old_as is SHARED with the suspended
    // parent: wake the parent and do NOT destroy the shared address space.
    // otherwise old_as is our own previous image - free it. (satoru)
    if (vfork_wake_parent(task)) {
        /* shared AS retained for the resuming vfork parent */
    } else {
        KernelVMM::DestroyAddressSpace(old_as);
    }

    ls_scpy(task->exe_path, resolved, sizeof(task->exe_path));
    ls_scpy(proc->name, resolved, sizeof(proc->name));

    current_syscall_frame->rip = entry;
    current_syscall_frame->rsp = rsp;
    current_syscall_frame->rbp = 0;
    current_syscall_frame->rax = 0; current_syscall_frame->rbx = 0;
    current_syscall_frame->rcx = 0; current_syscall_frame->rdx = 0;
    current_syscall_frame->rsi = 0; current_syscall_frame->rdi = 0;
    current_syscall_frame->r8  = 0; current_syscall_frame->r9  = 0;
    current_syscall_frame->r10 = 0; current_syscall_frame->r11 = 0;
    current_syscall_frame->r12 = 0; current_syscall_frame->r13 = 0;
    current_syscall_frame->r14 = 0; current_syscall_frame->r15 = 0;
    current_syscall_frame->rflags = 0x202ULL;
    // reset the TLS base like linux does on execve: the fresh image starts with
    // fs_base = 0 and musl's __init_tls installs its own via arch_prctl. WITHOUT
    // this the execve'd child inherits the PARENT's fs_base (a fork/vfork child
    // keeps the MAIN thread's static-TLS fs), so the new musl crt runs with the
    // wrong TLS and, when it sets its own fs mid-function, the stack-canary read
    // (%fs:0x28) changes underneath it -> __stack_chk_fail -> #GP; the child then
    // dies holding a shared musl lock and the firefox main thread deadlocks. (satoru)
    {
        constexpr uint32_t MSR_FS_BASE = 0xC0000100;
        __asm__ __volatile__("wrmsr" : : "c"(MSR_FS_BASE), "a"(0u), "d"(0u));
        task->fs_base = 0;
    }
    Scheduler::SaveUserFrame(task, current_syscall_frame);
    current_frame_rewritten = true;
    return 0;
}

int32_t LinuxSyscall::sys_execve(uintptr_t filename, uintptr_t argv, uintptr_t envp) {
    (void)envp;

    LinuxProcess* proc = Current();
    Process* task = Scheduler::GetCurrentProcess();
    if (!proc || !task || !task->is_user() || !current_syscall_frame) return -38;

    const char* path = (const char*)(uintptr_t)filename;
    char resolved[256];
    ResolvePath(path, resolved, sizeof(resolved), proc);

    // x86_64 dynamic ELF (firefox re-execs /proc/self/exe for content procs):
    // route through ld-kurono ExecPIE. is_valid_exec_elf below only accepts i386
    // static ELFs, so an elf64 execve would otherwise be rejected. (satoru)
    {
        uint8_t eh[5] = {0,0,0,0,0};
        int en = KVFS::ReadFile(resolved, eh, 5);
        if (en < 5 && Ext4::IsMounted()) en = Ext4::ReadWholeFile(resolved, eh, 5);
        if (en >= 5 && eh[0] == 0x7F && eh[1] == 'E' && eh[2] == 'L' && eh[3] == 'F' && eh[4] == 2) {
            return execve_dynamic64(resolved, argv, envp, proc, task);
        }
    }

    uint8_t* image = (uint8_t*)KernelHeap::Alloc(1024 * 1024);
    if (!image) return -12;

    int size = KVFS::ReadFile(resolved, image, 1024 * 1024);
    if (size <= 0 && Ext4::IsMounted()) {
        size = Ext4::ReadWholeFile(resolved, image, 1024 * 1024);
    }
    if (size <= 0 || !is_valid_exec_elf(image, (uint32_t)size)) {
        KernelHeap::Free(image);
        return -2;
    }

    uint64_t new_address_space = KernelVMM::CreateAddressSpace();
    if (!new_address_space) {
        KernelHeap::Free(image);
        return -12;
    }

    uint32_t entry_point = 0;
    uint32_t brk_end = LINUX_BRK_INITIAL;
    if (!load_exec_segments(new_address_space, image, (uint32_t)size, &entry_point, &brk_end)) {
        KernelVMM::DestroyAddressSpace(new_address_space);
        KernelHeap::Free(image);
        return -8;
    }

    void* stack_phys = nullptr;
    uint64_t stack_top = task->user_stack_top + 16;
    if (!map_exec_stack(new_address_space, stack_top, &stack_phys)) {
        KernelVMM::DestroyAddressSpace(new_address_space);
        KernelHeap::Free(image);
        return -12;
    }

    uint64_t new_rsp = 0;
    // this is the i386 elf32 static exec path (is_valid_exec_elf requires
    // EM_386/ELFCLASS32), so argv is a genuine sub-4gb pointer; the explicit
    // narrow keeps the 32-bit stack builder unchanged (satoru)
    if (!build_exec_stack(stack_phys, stack_top, (uint32_t)argv, &new_rsp)) {
        KernelVMM::DestroyAddressSpace(new_address_space);
        KernelHeap::Free(image);
        return -12;
    }

    uint64_t old_address_space = task->address_space;
    task->address_space = new_address_space;
    task->rip = (uintptr_t)entry_point;
    task->rsp = (uintptr_t)new_rsp;
    task->rbp = 0;
    task->user_stack_top = stack_top - 16;
    task->waiting_for_child = false;
    task->waiting_child_pid = 0;
    task->waiting_status_ptr = 0;
    memset(task->regions, 0, sizeof(task->regions));
    task->next_mmap_base = USER_MMAP_BASE;

    proc->brk_base = brk_end;
    proc->brk_current = brk_end;
    proc->brk_max = brk_end + 0x01000000U;
    proc->exited = false;
    ls_scpy(proc->name, resolved, sizeof(proc->name));
    // record the real exec path on the task too, so /proc/self/exe resolves to
    // it (the dynamic-pie path records this in ld-kurono::ExecPIE). (satoru)
    ls_scpy(task->exe_path, resolved, sizeof(task->exe_path));

    KernelVMM::ActivateAddressSpace(new_address_space);
    HAL::SetKernelStack(task->kernel_stack_top);

    current_syscall_frame->rip = entry_point;
    current_syscall_frame->rsp = new_rsp;
    current_syscall_frame->rbp = 0;
    current_syscall_frame->rax = 0;
    current_syscall_frame->rbx = 0;
    current_syscall_frame->rcx = 0;
    current_syscall_frame->rdx = 0;
    current_syscall_frame->rsi = 0;
    current_syscall_frame->rdi = 0;
    current_syscall_frame->r8 = 0;
    current_syscall_frame->r9 = 0;
    current_syscall_frame->r10 = 0;
    current_syscall_frame->r11 = 0;
    current_syscall_frame->r12 = 0;
    current_syscall_frame->r13 = 0;
    current_syscall_frame->r14 = 0;
    current_syscall_frame->r15 = 0;
    current_syscall_frame->rflags = 0x202ULL;

    // reset TLS base like linux execve (see the dynamic-PIE path for the full
    // rationale): the fresh image must start with fs_base = 0 so musl installs
    // its own TLS, not inherit the parent's fs_base. (satoru)
    {
        constexpr uint32_t MSR_FS_BASE = 0xC0000100;
        __asm__ __volatile__("wrmsr" : : "c"(MSR_FS_BASE), "a"(0u), "d"(0u));
        task->fs_base = 0;
    }
    Scheduler::SaveUserFrame(task, current_syscall_frame);
    current_frame_rewritten = true;

    KernelVMM::DestroyAddressSpace(old_address_space);
    KernelHeap::Free(image);
    return 0;
}

int32_t LinuxSyscall::sys_read(int fd, uintptr_t buf, uint64_t count) {
    LinuxProcess* p = Current();
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9; // ebadf

    LinuxFd* lfd = &p->fds[fd];
    uint8_t* dst = (uint8_t*)buf;

    switch (lfd->type) {
        case LFD_CONSOLE: {
            // stdin - read from injection buffer (non-blocking)
            uint64_t read = 0;
            while (read < count && stdin_head != stdin_tail) {
                dst[read++] = (uint8_t)stdin_buf[stdin_tail];
                stdin_tail = (stdin_tail + 1) % STDIN_BUF_SIZE;
            }
            return (int32_t)read;
        }

        case LFD_KVFS: {
            int r = KVFS::Read(lfd->backend_fd, dst, (uint32_t)count);
            if (r > 0) lfd->offset += r;
            return r;
        }

        case LFD_EXT4: {
            int r = Ext4::Read(lfd->backend_fd, dst, (uint32_t)count);
            if (r > 0) lfd->offset += r;
            return r;
        }

        // af_inet tcp/udp: recv through the bridge. nspr/necko reads sockets
        // via plain read() as often as recv(). a BLOCKING socket waits with the
        // eventfd discipline below (deschedule or breathe + retry). (satoru)
        case LFD_INET: {
            int r = LinuxNetBridge::Recv(lfd->backend_fd, dst, (int)count, 0);
            if (r == -11 && !(lfd->flags & L_O_NONBLOCK)) {
                if (poll_try_deschedule(p, 0)) return 0;   // switched; frame rewritten (satoru)
                if (SMP::CpuIndex() == 0) {
                    LinuxNetBridge::PumpTick();            // drain the nic while we wait (satoru)
                    KuronoShell::PumpUI(); Scheduler::SleepMs(1);
                } else kls_relax();
                return -11;   // re-issued by the caller's retry (satoru)
            }
            return r;
        }

        // unix sockets + pipes: plain read() must work, not just recv() - the
        // missing case fell through to -EBADF (mirror of the sys_write gap that
        // broke nspr's PollableEvent). EAGAIN when empty: sockets are polled by
        // their owners (glib/nspr) before reading; a BLOCKING fd yields like the
        // inet path. (satoru)
        case LFD_SOCKET:
        case LFD_PIPE: {
            int r = UnixSocket::Recv(lfd->backend_fd, dst, (int)count, 0);
            if (r == -11 && !(lfd->flags & L_O_NONBLOCK)) {
                if (poll_try_deschedule(p, 0)) return 0;   // switched; frame rewritten (satoru)
                if (SMP::CpuIndex() == 0) { KuronoShell::PumpUI(); Scheduler::SleepMs(1); }
                else kls_relax();
                return -11;   // re-issued by the caller's retry (satoru)
            }
            return r;
        }

        // eventfd read: returns the 8-byte counter and zeroes it (or returns 1
        // and decrements in semaphore mode). EAGAIN when the counter is 0 - the
        // event loop epoll_waits for EPOLLIN before reading. (satoru)
        case LFD_EVENTFD: {
            int s = lfd->backend_fd;
            if (s < 0 || s >= EVENTFD_MAX || count < 8) return -22;  // einval
            uint64_t v = g_eventfd[s].counter;
            // a BLOCKING eventfd (no O_NONBLOCK) read with counter 0 must BLOCK
            // until a write bumps it - returning EAGAIN made webrender's software
            // sw_compositor thread (which drains a blocking eventfd job-queue
            // wakeup) unwrap() a WouldBlock io::Error and panic ("Resource
            // temporarily unavailable") right as the first frame composited. hand
            // the cpu to a sibling and re-run the read on wake, exactly like
            // poll/futex; only a genuinely O_NONBLOCK fd still gets EAGAIN. (satoru)
            if (v == 0 && !(lfd->flags & L_O_NONBLOCK)) {
                // restart nr 0 = x86_64 read (musl uses SYSCALL, so the frame
                // re-executes as an amd64 read). (satoru)
                if (poll_try_deschedule(p, 0)) return 0;   // switched; frame rewritten (satoru)
                // no sibling to run: cooperative wait then re-scan (bsp pumps ui;
                // an ap breathes on the kls lock). (satoru)
                if (SMP::CpuIndex() == 0) { KuronoShell::PumpUI(); Scheduler::SleepMs(1); }
                else kls_relax();
                return -11;   // frame not rewritten: re-issued by the caller's retry (satoru)
            }
            if (v == 0) return -11;   // O_NONBLOCK - genuine eagain (satoru)
            uint64_t ret;
            if (g_eventfd[s].semaphore) { ret = 1; g_eventfd[s].counter = v - 1; }
            else                        { ret = v; g_eventfd[s].counter = 0; }
            *(uint64_t*)dst = ret;
            // [evr] trace eventfd drains so we can see if the launcher's pump woke
            // and consumed its wakeup. (satoru)
            if (p->pid >= 100 && p->pid < 140) {
                SerialLogger::Log("[evr] pid="); SerialLogger::LogDec(p->pid);
                SerialLogger::Log(" fd="); SerialLogger::LogDec(fd);
                SerialLogger::Log(" was="); SerialLogger::LogDec((int)v);
                SerialLogger::Log("\r\n");
            }
            return 8;
        }

        // timerfd read: returns the 8-byte count of expirations since last read
        // (re-armed if periodic), then clears it. EAGAIN if not yet expired.
        // (satoru)
        case LFD_TIMERFD: {
            int s = lfd->backend_fd;
            if (s < 0 || s >= TIMERFD_MAX || count < 8) return -22;
            timerfd_tick(s);
            uint64_t v = g_timerfd[s].expirations;
            if (v == 0) return -11;   // eagain (satoru)
            g_timerfd[s].expirations = 0;
            *(uint64_t*)dst = v;
            return 8;
        }

        case LFD_DEVNULL:
        case LFD_SIGNALFD:   // signalfd never delivers - reads as empty (satoru)
            return 0;

        case LFD_PROC: {
            // /proc virtual files generated on demand from live process state
            char procdata[1024];
            int plen = 0;
            LinuxProcess* curp = Current();

            // helpers (local lambdas would be nicer but we're freestanding)
            #define PROC_APPEND(s) do { \
                int _n = ls_slen(s); \
                if (plen + _n < (int)sizeof(procdata)) { \
                    memcpy(procdata + plen, s, _n); plen += _n; \
                } \
            } while(0)
            #define PROC_APPEND_INT(v) do { \
                char _ib[16]; ls_itoa((int)(v), _ib, 10); PROC_APPEND(_ib); \
            } while(0)

            // The path may be either /proc/... (legacy) or /system/proc/...
            // (post-translation).  Strip the /system prefix once if present so
            // the rest of this block can match against /proc/... uniformly.
            const char* pp = lfd->path;
            if (ls_starts(pp, "/system/proc")) pp += 7;     // -> "/proc..."
            // (satoru) TEMP probe: log every proc-file read path + whether curp is set, to
            // see if/how firefox reads /proc/self/maps (why the maps branch never fires).
            { static uint64_t s_pr=0; if(((++s_pr)&0x3)==0){ SerialLogger::Log("[procrd] curp="); SerialLogger::LogHex((uint64_t)(uintptr_t)curp); SerialLogger::Log(" "); SerialLogger::Log(pp); SerialLogger::Log("\r\n"); } }

            if (ls_starts(pp, "/proc/self/status") && curp) {
                PROC_APPEND("Name:\t");      PROC_APPEND(curp->name); PROC_APPEND("\n");
                PROC_APPEND("State:\tR (running)\n");
                PROC_APPEND("Tgid:\t");      PROC_APPEND_INT(curp->pid);  PROC_APPEND("\n");
                PROC_APPEND("Ngid:\t0\n");
                PROC_APPEND("Pid:\t");       PROC_APPEND_INT(curp->pid);  PROC_APPEND("\n");
                PROC_APPEND("PPid:\t");      PROC_APPEND_INT(curp->ppid); PROC_APPEND("\n");
                PROC_APPEND("TracerPid:\t0\n");
                PROC_APPEND("Uid:\t");       PROC_APPEND_INT(curp->uid);
                PROC_APPEND("\t");           PROC_APPEND_INT(curp->euid);
                PROC_APPEND("\t");           PROC_APPEND_INT(curp->euid);
                PROC_APPEND("\t");           PROC_APPEND_INT(curp->euid); PROC_APPEND("\n");
                PROC_APPEND("Gid:\t");       PROC_APPEND_INT(curp->gid);
                PROC_APPEND("\t");           PROC_APPEND_INT(curp->egid);
                PROC_APPEND("\t");           PROC_APPEND_INT(curp->egid);
                PROC_APPEND("\t");           PROC_APPEND_INT(curp->egid); PROC_APPEND("\n");
                PROC_APPEND("VmSize:\t");    PROC_APPEND_INT((curp->brk_current - curp->brk_base)/1024); PROC_APPEND(" kB\n");
                PROC_APPEND("VmRSS:\t");     PROC_APPEND_INT((curp->brk_current - curp->brk_base)/1024); PROC_APPEND(" kB\n");
                PROC_APPEND("Threads:\t1\n");
                PROC_APPEND("SigQ:\t0/16384\n");
                PROC_APPEND("Sid:\t");       PROC_APPEND_INT(curp->sid);  PROC_APPEND("\n");
            } else if (ls_starts(pp, "/proc/self/cmdline") && curp) {
                // single-arg cmdline = process name, NUL-terminated
                PROC_APPEND(curp->name);
                if (plen + 1 < (int)sizeof(procdata)) procdata[plen++] = 0;
            } else if (ls_starts(pp, "/proc/self/comm") && curp) {
                PROC_APPEND(curp->name); PROC_APPEND("\n");
            } else if (ls_starts(pp, "/proc/self/maps") && curp) {
                char hex[24];
                // [stack] MUST be present: musl's pthread_getattr_np (main-thread
                // path) scans /proc/self/maps for the line whose range contains the
                // current SP to derive the stack base/size. with only [heap] here,
                // gecko's nsThread::InitCommon -> pthread_getattr_np never found the
                // stack and the chrome main thread wedged in early startup. emit the
                // full 8MB user stack [user_stack_top-8MB, user_stack_top]. (satoru)
                uint64_t stk_top = curp->task ? curp->task->user_stack_top : 0;
                if (stk_top) {
                    uint64_t stk_top_pg = (stk_top + PAGE_SIZE) & ~(uint64_t)(PAGE_SIZE - 1);
                    uint64_t stk_base   = stk_top_pg - (8ULL * 1024 * 1024);
                    ls_itoa((int)stk_base, hex, 16);   PROC_APPEND(hex); PROC_APPEND("-");
                    ls_itoa((int)stk_top_pg, hex, 16); PROC_APPEND(hex);
                    PROC_APPEND(" rw-p 00000000 00:00 0                          [stack]\n");
                    // (satoru) TEMP probe: confirm the dynamic /proc/self/maps handler is hit
                    // + the exact [stack] line it emits (vs the mremap-probe walk addresses).
                    SerialLogger::Log("[pmaps] gen stk="); SerialLogger::LogHex(stk_base);
                    SerialLogger::Log("-"); SerialLogger::LogHex(stk_top_pg); SerialLogger::Log("\r\n");
                }
                // anonymous heap region (satoru)
                ls_itoa((int)curp->brk_base, hex, 16);
                PROC_APPEND(hex); PROC_APPEND("-");
                ls_itoa((int)curp->brk_current, hex, 16);
                PROC_APPEND(hex);
                PROC_APPEND(" rw-p 00000000 00:00 0                          [heap]\n");
            } else if (ls_starts(pp, "/proc/self/exe") && curp) {
                // prefer the real recorded exec path (e.g. /apps/firefox/firefox)
                // so gecko anchors its app dir correctly; fall back to the old
                // synthesized /system/bin/<name> when no path was recorded. (satoru)
                if (curp->task && curp->task->exe_path[0]) {
                    PROC_APPEND(curp->task->exe_path);
                } else {
                    PROC_APPEND("/system/bin/"); PROC_APPEND(curp->name);
                }
            } else if (ls_starts(pp, "/proc/self/cwd") && curp) {
                PROC_APPEND(curp->cwd);
            } else if (ls_starts(pp, "/proc/self/stat") && curp) {
                // Minimal /proc/self/stat: "pid (name) state ppid pgid sid ..."
                PROC_APPEND_INT(curp->pid); PROC_APPEND(" (");
                PROC_APPEND(curp->name); PROC_APPEND(") R ");
                PROC_APPEND_INT(curp->ppid); PROC_APPEND(" ");
                PROC_APPEND_INT(curp->pgid); PROC_APPEND(" ");
                PROC_APPEND_INT(curp->sid); PROC_APPEND(" 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 0 0 0\n");
            } else if (ls_starts(pp, "/proc/filesystems")) {
                PROC_APPEND("nodev\tproc\n"
                            "nodev\tsysfs\n"
                            "nodev\tdevtmpfs\n"
                            "nodev\ttmpfs\n"
                            "\text4\n"
                            "\tfat32\n"
                            "\tkvfs\n");
            } else {
                // Fall back to the KVFS-backed copy populated by ProcFS at boot
                // (e.g. /proc/cpuinfo, /proc/meminfo, /proc/version, /proc/uptime,
                //  /proc/loadavg, /proc/mounts, /proc/net/dev). Re-open lazily.
                int kfd = KVFS::Open(lfd->path, 1);
                if (kfd >= 0) {
                    // honor lfd->offset across repeated reads. KVFS::Open starts the
                    // backing fd at offset 0, so without this seek EVERY read() returned
                    // the same first `count` bytes and the file NEVER reached EOF. that
                    // wedged firefox at CompMgrInit: its rust num_cpus/cgroup `Once`
                    // reads /proc/cpuinfo (+ /proc/self/cgroup) via std::io::Lines, which
                    // loops until EOF - so it spun forever on the repeating first bytes
                    // (gdb: chrome main thread state=Ready, stuck in std::io::Lines::next
                    // -> Once::call_once). seek to the fd offset, advance it, and return
                    // 0 at EOF so the line reader terminates. (satoru)
                    KVFS::Seek(kfd, (int32_t)lfd->offset, 0);
                    int r = KVFS::Read(kfd, dst, (uint32_t)count);
                    KVFS::Close(kfd);
                    if (r > 0) { lfd->offset += (uint64_t)r; return r; }
                    return 0;
                }
            }
            // honour offset for repeated reads (so cat/strings see EOF)
            int avail = plen - (int)lfd->offset;
            if (avail <= 0) return 0;
            int n = (int)count < avail ? (int)count : avail;
            memcpy(dst, procdata + lfd->offset, n);
            lfd->offset += n;
            return n;
            #undef PROC_APPEND
            #undef PROC_APPEND_INT
        }

        default:
            return -9;
    }
}

// (satoru) TEMP mozlog probe: dump firefox MOZ_LOG-file writes to serial. logging
// IS compiled in (xpcom/base/Logging.h: MOZ_LOGGING_ENABLED 1), so routing MOZ_LOG
// to a FILE (MOZ_LOG_FILE / logging.config.LOG_FILE) and capturing the file writes
// here bypasses the content-process stderr redirect that hides child logs. only
// dumps gecko-log lines (those carrying the "]: " thread-prefix delimiter) so
// cache/pref writes are skipped. remove before commit.
static void mozlog_probe(LinuxProcess* p, LinuxFd* lfd, const uint8_t* src, uint64_t count) {
    return;  // (satoru) DISABLED: serial-dumping firefox gecko-log writes (~8.7ms/line
             // ring-0 UART busy-wait at 115200 baud) throttles firefox to a near-halt.
             // flip the guard below to fish socket-transport (nsSocketTransport) lines
             // out of a .moz_log sink for a targeted trace only. (satoru)
    {
        if (!p || p->pid < 100 || p->pid >= 140 || !lfd || !src || count < 8) return;
        static int nmzl = 0;
        if (nmzl >= 300) return;
        uint64_t scan = count < 400 ? count : 400;
        bool hit = false;
        for (uint64_t i = 0; i + 7 < scan && !hit; i++) {
            if ((src[i]=='o'&&src[i+1]=='c'&&src[i+2]=='k'&&src[i+3]=='e'&&src[i+4]=='t'&&src[i+5]=='T') ||
                (src[i]=='o'&&src[i+1]=='l'&&src[i+2]=='l'&&src[i+3]=='a'&&src[i+4]=='b'&&src[i+5]=='l') ||
                (src[i]=='S'&&src[i+1]=='T'&&src[i+2]=='S'&&src[i+3]==' '))
                hit = true;
        }
        if (!hit) return;
        nmzl++;
        SerialLogger::Log("[mzl] ");
        char b[2] = {0, 0};
        for (uint64_t i = 0; i < scan; i++) {
            char c = (char)src[i];
            if (c == '\n') break;
            b[0] = (c >= 32 && c < 127) ? c : '.';
            SerialLogger::Log(b);
        }
        SerialLogger::Log("\r\n");
        return;
    }
    if (!p || p->pid < 100 || p->pid >= 140 || !lfd || !src || count < 1) return;
    // dump (a) the FIRST write to each file (offset 0) so we see every file firefox
    // creates + its first bytes -> spot the MOZ_LOG file, and (b) any later write
    // carrying a gecko-log line ("] D/" / "] V/" etc.). (satoru)
    bool dump = (lfd->offset == 0);
    if (!dump) {
        // gecko log lines are "...]: <level>/<module> msg" - match the "]: "
        // delimiter so EVERY log line (not just the first write) is dumped. (satoru)
        uint64_t scan = count < 300 ? count : 300;
        for (uint64_t i = 0; i + 2 < scan; i++) {
            if (src[i] == ']' && src[i+1] == ':' && src[i+2] == ' ') { dump = true; break; }
        }
    }
    if (!dump) return;
    SerialLogger::Log("[mozw p="); SerialLogger::LogDec(p->pid);
    SerialLogger::Log("] ");
    uint64_t lim = count < 400 ? count : 400;
    for (uint64_t i = 0; i < lim; i++) {
        char c = (char)src[i];
        char s[2] = { (c == '\n' || (c >= 32 && c < 127)) ? c : '.', 0 };
        SerialLogger::Log(s);
    }
    SerialLogger::Log("\r\n");
}

int32_t LinuxSyscall::sys_write(int fd, uintptr_t buf, uint64_t count) {
    LinuxProcess* p = Current();
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;

    LinuxFd* lfd = &p->fds[fd];
    const uint8_t* src = (const uint8_t*)buf;

    // (satoru) firefox bring-up: this build has MOZ_LOG compiled in (MOZ_LOGGING_ENABLED=1)
    // and env MOZ_LOG=all:0 does NOT silence the default Debug-level modules. with
    // MOZ_LOG_FILE=/dev/null, gecko appends a suffix and creates a REAL file
    // "<...>/dev/null.moz_log", then writes the whole startup log flood to it - every
    // line a slow KVFS/EXT4 write that throttles the chrome main thread to a crawl
    // during CompMgrInit (gdb: main repeatedly in LogModuleManager::ActuallyLog ->
    // SprintfAppend; the file grew only ~68KB in 480s). a path ending in .moz_log is a
    // throwaway log sink gecko never reads back, so drop the write entirely: pretend it
    // succeeded (advance offset, return count) without touching the backend. (satoru)
    if ((lfd->type == LFD_KVFS || lfd->type == LFD_EXT4) && lfd->path[0]) {
        int pl = ls_slen(lfd->path);
        if (pl >= 8) {
            const char* suf = ".moz_log";
            bool m = true;
            for (int i = 0; i < 8; i++) if (lfd->path[pl - 8 + i] != suf[i]) { m = false; break; }
            if (m) { lfd->offset += count; return (int32_t)count; }
        }
    }

    switch (lfd->type) {
        case LFD_CONSOLE: {
            // stdout/stderr → capture to console ring buffer + serial
            for (uint64_t i = 0; i < count; i++) {
                char c = (char)src[i];
                // write to capture buffer
                int next = (console_head + 1) % CONSOLE_BUF_SIZE;
                if (next != console_tail) {
                    console_buf[console_head] = c;
                    console_head = next;
                }
                // also echo to serial for debugging
                char s[2] = { c, 0 };
                SerialLogger::Log(s);
            }
            return (int32_t)count;
        }

        case LFD_KVFS: {
            mozlog_probe(p, lfd, src, count);   // (satoru) TEMP capture MOZ_LOG file writes
            int r = KVFS::Write(lfd->backend_fd, src, (uint32_t)count);
            if (r > 0) lfd->offset += r;
            return r;
        }

        case LFD_EXT4: {
            mozlog_probe(p, lfd, src, count);   // (satoru) TEMP capture MOZ_LOG file writes
            int r = Ext4::Write(lfd->backend_fd, src, (uint32_t)count);
            if (r > 0) lfd->offset += r;
            return r;
        }

        // af_inet tcp/udp: send through the bridge. -EAGAIN passes through - 
        // necko writes are poll-driven, the caller retries on POLLOUT. (satoru)
        case LFD_INET:
            return LinuxNetBridge::Send(lfd->backend_fd, src, (int)count, 0);

        // unix sockets + pipes: plain write() must work, not just send() - this
        // case was MISSING and every write() on a socket/pipe fd fell through to
        // the default -EBADF. nspr's PollableEvent (firefox's SOCKET THREAD wake
        // mechanism) self-tests exactly that: pipe(); write(fd[1]) - the -9 made
        // it tear the event down and the socket thread ran wake-less forever
        // (transports queued, zero AF_INET traffic, page loads hung). (satoru)
        case LFD_SOCKET:
        case LFD_PIPE:
            return UnixSocket::Send(lfd->backend_fd, src, (int)count, 0);

        // eventfd write: adds an 8-byte value to the counter, making the fd
        // readable (EPOLLIN). this is how glib's main loop wakes its epoll. an
        // add of 0 is a no-op; UINT64_MAX is rejected per the eventfd(2) abi.
        // (satoru)
        case LFD_EVENTFD: {
            int s = lfd->backend_fd;
            if (s < 0 || s >= EVENTFD_MAX || count < 8) return -22;  // einval
            uint64_t add = *(const uint64_t*)src;
            if (add == 0xFFFFFFFFFFFFFFFFULL) return -22;
            // saturate at UINT64_MAX-1 rather than overflow-wrap (satoru)
            if (g_eventfd[s].counter + add < g_eventfd[s].counter)
                g_eventfd[s].counter = 0xFFFFFFFFFFFFFFFEULL;
            else
                g_eventfd[s].counter += add;
            // [evw] trace the MessagePump wakeup write so we can see whether the
            // launcher's wakeup eventfd is being signaled. (satoru)
            if (p->pid >= 100 && p->pid < 140) {
                SerialLogger::Log("[evw] pid="); SerialLogger::LogDec(p->pid);
                SerialLogger::Log(" fd="); SerialLogger::LogDec(fd);
                SerialLogger::Log(" cnt="); SerialLogger::LogDec((int)g_eventfd[s].counter);
                SerialLogger::Log("\r\n");
            }
            return 8;
        }

        case LFD_DEVNULL:
        case LFD_SIGNALFD:   // writes to a signalfd are meaningless; swallow (satoru)
            return (int32_t)count;

        default:
            return -9;
    }
}

int32_t LinuxSyscall::sys_writev(int fd, uintptr_t iov, uint64_t iovcnt) {
    LinuxIovec* vecs = (LinuxIovec*)iov;
    // (satoru) [wvd] capture the looping main thread's (pid 100) writev text. the [ff]
    // syscall trace shows pid 100 stuck looping writev/getpid/clock_gettime = gecko
    // logging one line per iteration -> a retry loop. the log content names the
    // failing operation (the root of the stall). capped at 40 lines. (satoru)
    {
        LinuxProcess* wp = Current();
        static int wvd_n = 0;
        if (false && wp && wp->pid == 100 && iovcnt > 0 && vecs[0].iov_base && wvd_n < 40 &&
            Timer::GetRealMs64() > 70000) {  // gated off (satoru)
            wvd_n++;
            const char* b = (const char*)(uintptr_t)vecs[0].iov_base;
            uint64_t L = vecs[0].iov_len; if (L > 160) L = 160;
            SerialLogger::Log("[wvd] fd="); SerialLogger::LogDec(fd); SerialLogger::Log(" :");
            for (uint64_t k = 0; k < L; k++) {
                char c = b[k];
                char s[2] = { (c == '\n' ? ' ' : (c >= 32 && c < 127) ? c : '.'), 0 };
                SerialLogger::Log(s);
            }
            SerialLogger::Log("\r\n");
        }
    }
    int32_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        // iov_base is a 64-bit user pointer, iov_len a 64-bit length (satoru)
        int32_t r = sys_write(fd, (uintptr_t)vecs[i].iov_base, vecs[i].iov_len);
        if (r < 0) return r;
        total += r;
    }
    return total;
}

// readv - the read counterpart of writev: fill each iovec from fd in order.
// modelled on sys_writev (iterate base/len, route to sys_read). returns the
// running total; an error on the first vector propagates, otherwise we return
// what we've read so far. a short read (fewer bytes than the vector asked for)
// stops the scatter, matching readv(2) semantics. gecko uses this once running.
// (satoru)
int32_t LinuxSyscall::sys_readv(int fd, uintptr_t iov, uint64_t iovcnt) {
    LinuxIovec* vecs = (LinuxIovec*)iov;
    int32_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        uint64_t want = vecs[i].iov_len;
        if (want == 0) continue;
        int32_t r = sys_read(fd, (uintptr_t)vecs[i].iov_base, want);
        if (r < 0) return total > 0 ? total : r;
        total += r;
        if ((uint64_t)r < want) break;  // short read → source drained (satoru)
    }
    return total;
}

int32_t LinuxSyscall::sys_open(uintptr_t pathname, uint32_t flags, uint32_t mode) {
    LinuxProcess* p = Current();
    if (!p) return -9;

    const char* path = (const char*)pathname;
    char resolved[256];
    ResolvePath(path, resolved, sizeof(resolved), p);

    int lfd_idx = AllocFd(p);
    if (lfd_idx < 0) return -24;  // emfile

    LinuxFd* lfd = &p->fds[lfd_idx];
    memset(lfd, 0, sizeof(LinuxFd));
    ls_scpy(lfd->path, resolved, sizeof(lfd->path));
    lfd->flags = flags;

    // /dev/null  (translation already mapped /dev/* -> /system/dev/*)
    if (ls_seq(resolved, "/system/dev/null") || ls_seq(resolved, "/dev/null")) {
        lfd->type = LFD_DEVNULL;
        lfd->open = true;
        return lfd_idx;
    }

    // /proc/self/fd/N (or /proc/<pid>/fd/N): reopening an EXISTING fd by path.
    // firefox's shared-memory DupReadOnly() reopens its writable memfd this way
    // (O_RDONLY) to obtain the frozen read-only handle used by SharedStringMap /
    // the shared font list. the reopened fd MUST keep pointing at the same
    // underlying object - especially a memfd's shm pages - or the later mmap
    // returns nothing and gecko hits MOZ_RELEASE_ASSERT(mapping.IsValid()). so
    // clone the source fd here instead of returning a generic /proc file. (satoru)
    if (ls_starts(resolved, "/system/proc/") || ls_starts(resolved, "/proc/")) {
        const char* after_fd = nullptr;
        for (const char* c = resolved; c[0]; c++) {
            if (c[0] == '/' && c[1] == 'f' && c[2] == 'd' && c[3] == '/')
                after_fd = c + 4;            // last "/fd/" segment
        }
        if (after_fd && *after_fd) {
            int srcfd = 0; bool num = true;
            for (const char* d = after_fd; *d; d++) {
                if (*d < '0' || *d > '9') { num = false; break; }
                srcfd = srcfd * 10 + (*d - '0');
            }
            if (num && srcfd >= 0 && srcfd < LINUX_MAX_FDS &&
                srcfd != lfd_idx && p->fds[srcfd].open) {
                LinuxFd* src = &p->fds[srcfd];
                *lfd = *src;                  // clone type/backend/path/offset
                lfd->flags  = flags;
                lfd->offset = 0;
                if (src->type == LFD_MEMFD) {
                    LinuxShmObj* s = shm_slot(src->backend_fd);
                    if (s) __atomic_add_fetch(&s->refcount, 1, __ATOMIC_SEQ_CST);  // shared shm survives either close (atomic under -smp) (satoru)
                } else if (src->type == LFD_KVFS) {
                    // give the dup its own kvfs cursor; dir fds keep backend -1.
                    // reopen access mirrors the requested open flags. (satoru)
                    uint8_t rf = ((flags & 3) == L_O_WRONLY) ? 2
                               : ((flags & 3) == L_O_RDWR)   ? 3 : 1;
                    lfd->backend_fd = (src->backend_fd >= 0)
                                          ? KVFS::Open(src->path, rf) : -1;
                }
                lfd->open = true;
                return lfd_idx;
            }
        }
    }

    // /proc files  (translation maps /proc/* -> /system/proc/*; also accept legacy)
    if (ls_starts(resolved, "/system/proc") || ls_starts(resolved, "/proc")) {
        lfd->type = LFD_PROC;
        lfd->open = true;
        return lfd_idx;
    }

    // try kvfs first
    uint8_t kvfs_flags = 0;
    if ((flags & 3) == L_O_RDONLY) kvfs_flags = 1;
    else if ((flags & 3) == L_O_WRONLY) kvfs_flags = 2;
    else if ((flags & 3) == L_O_RDWR) kvfs_flags = 3;
    if (flags & L_O_APPEND) kvfs_flags |= 4;

    if (flags & L_O_CREAT) {
        if (!KVFS::Exists(resolved)) {
            KVFS::CreateFile(resolved, (uint16_t)(mode & 0777));
        }
    }

    if (KVFS::Exists(resolved)) {
        // opening a directory (opendir): KVFS::Open deliberately rejects dirs
        // (KVFS_ERR_IS_DIR), but getdents64 and fstat both resolve by lfd->path,
        // not the backend handle. so register an LFD_KVFS dir fd with no backend
        // file handle. WITHOUT this, opendir() fell through to ENOENT and every
        // linux readdir saw an empty tree - fontconfig then enumerated zero fonts
        // and firefox MOZ_CRASHed in GetDefaultFontLocked. (satoru)
        KVFSNode* knode = KVFS::Resolve(resolved);
        if (knode && knode->is_dir()) {
            lfd->type = LFD_KVFS;
            lfd->backend_fd = -1;     // dir fd: no kvfs file handle
            lfd->open = true;
            return lfd_idx;
        }
        int kfd = KVFS::Open(resolved, kvfs_flags);
        if (kfd >= 0) {
            lfd->type = LFD_KVFS;
            lfd->backend_fd = kfd;
            lfd->open = true;
            return lfd_idx;
        }
    }

    // try ext4
    if (Ext4::IsMounted()) {
        uint8_t e4flags = 0;
        if ((flags & 3) == L_O_RDONLY) e4flags = 1;
        else if ((flags & 3) == L_O_WRONLY) e4flags = 2;
        else e4flags = 3;

        if (flags & L_O_CREAT) {
            if (!Ext4::Exists(resolved)) {
                Ext4::CreateFile(resolved, (uint16_t)(mode & 0777));
            }
        }

        int e4fd = Ext4::Open(resolved, e4flags);
        if (e4fd >= 0) {
            lfd->type = LFD_EXT4;
            lfd->backend_fd = e4fd;
            lfd->open = true;
            return lfd_idx;
        }
    }

    return -2;  // enoent
}

int32_t LinuxSyscall::sys_close(int fd) {
    LinuxProcess* p = Current();
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;

    LinuxFd* lfd = &p->fds[fd];
    // posix: closing ANY fd of a file drops the process's byte-range locks on
    // it. sqlite's unix vfs is designed around exactly this, so honour it. (satoru)
    if ((lfd->type == LFD_KVFS || lfd->type == LFD_EXT4) && lfd->path[0])
        flock_drop(region_owner(p->task), flock_path_hash(lfd->path));
    if (lfd->type == LFD_KVFS) { if (lfd->backend_fd >= 0) KVFS::Close(lfd->backend_fd); }  // dir fds have no backend handle (satoru)
    else if (lfd->type == LFD_EXT4) Ext4::Close(lfd->backend_fd);
    // release epoll/eventfd/timerfd table slots back to the pool so the
    // bounded tables don't leak across a long-lived process (satoru)
    else if (lfd->type == LFD_EPOLL) {
        int s = lfd->backend_fd;
        if (s >= 0 && s < EPOLL_MAX) g_epoll[s].used = false;
    } else if (lfd->type == LFD_EVENTFD) {
        int s = lfd->backend_fd;
        if (s >= 0 && s < EVENTFD_MAX) g_eventfd[s].used = false;
    } else if (lfd->type == LFD_TIMERFD) {
        int s = lfd->backend_fd;
        if (s >= 0 && s < TIMERFD_MAX) g_timerfd[s].used = false;
    } else if (lfd->type == LFD_INET) {
        // refcounted release - the 16-slot kurono stack table makes close
        // discipline critical (firefox churns connections). (satoru)
        LinuxNetBridge::Close(lfd->backend_fd);
    } else if (lfd->type == LFD_MEMFD) {
        // (satoru) just drop the refcount here; do NOT reclaim the slot on close. a
        // wl_shm pool memfd is shared with the in-kernel wayland compositor, which may
        // still need the object after firefox closes its own fd, so nulling the slot
        // races the compositor. the 2048-slot table (see g_linux_shm) is sized so
        // startup + a browsing session can't exhaust it without reclaim. (satoru)
        LinuxShmObj* s = shm_slot(lfd->backend_fd);
        if (s) __atomic_sub_fetch(&s->refcount, 1, __ATOMIC_SEQ_CST);
    }
    lfd->open = false;
    return 0;
}

int32_t LinuxSyscall::sys_lseek(int fd, int32_t offset, uint32_t whence) {
    LinuxProcess* p = Current();
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;

    LinuxFd* lfd = &p->fds[fd];
    if (lfd->type == LFD_KVFS) {
        return KVFS::Seek(lfd->backend_fd, offset, (int)whence);
    }
    if (lfd->type == LFD_EXT4) {
        return Ext4::Seek(lfd->backend_fd, offset, (int)whence);
    }
    return -29;  // espipe
}

int64_t LinuxSyscall::sys_brk(uintptr_t addr) {
    LinuxProcess* p = Current();
    if (!p) return -1;

    // brk returns the (possibly >4gb) break address; validate against the
    // 64-bit per-process brk window and return it without truncation (satoru)
    if (addr == 0) return (int64_t)p->brk_current;
    if (addr > p->brk_max) return (int64_t)p->brk_current;
    if (addr < p->brk_base) return (int64_t)p->brk_current;

    if (p->task && p->task->is_user()) {
        if (!ensure_heap_region(p, p->task, addr)) {
            return (int64_t)p->brk_current;
        }
    }

    p->brk_current = addr;
    return (int64_t)addr;
}

int32_t LinuxSyscall::sys_getpid() {
    // getpid() returns the THREAD-GROUP id (the leader thread's pid), identical for
    // every thread in the group -- Linux semantics. region_owner() walks the
    // CLONE_THREAD parent chain to the leader Process. without this each pthread
    // returned its own per-thread scheduler pid, so gecko's
    // EndpointProcInfo::Current() (== getpid()) mismatched the pid baked into an IPC
    // endpoint created on another thread, firing MOZ_RELEASE_ASSERT in
    // mozilla::ipc::UntypedEndpoint::Bind. gettid() / Process.pid still return the
    // distinct per-thread id. (satoru)
    Process* t = Scheduler::GetCurrentProcess();
    if (t && t->is_user()) {
        Process* leader = region_owner(t);
        if (leader) return (int32_t)leader->pid;
    }
    LinuxProcess* p = Current();
    return p ? (int32_t)p->pid : -1;
}

int32_t LinuxSyscall::sys_getppid() {
    LinuxProcess* p = Current();
    return p ? (int32_t)p->ppid : -1;
}

int32_t LinuxSyscall::sys_getuid() {
    LinuxProcess* p = Current();
    return p ? (int32_t)p->uid : 0;
}

int32_t LinuxSyscall::sys_getgid() {
    LinuxProcess* p = Current();
    return p ? (int32_t)p->gid : 0;
}

int32_t LinuxSyscall::sys_geteuid() {
    LinuxProcess* p = Current();
    return p ? (int32_t)p->euid : 0;
}

int32_t LinuxSyscall::sys_getegid() {
    LinuxProcess* p = Current();
    return p ? (int32_t)p->egid : 0;
}

int32_t LinuxSyscall::sys_stat(uintptr_t pathname, uintptr_t statbuf) {
    LinuxProcess* p = Current();
    if (!p) return -1;

    const char* path = (const char*)pathname;
    char resolved[256];
    ResolvePath(path, resolved, sizeof(resolved), p);

    LinuxStat* st = (LinuxStat*)statbuf;
    memset(st, 0, sizeof(LinuxStat));

    // try kvfs
    KVFSNode* node = KVFS::Resolve(resolved);
    if (node) {
        st->st_ino = (uint32_t)(uintptr_t)node;
        st->st_mode = node->perms.mode;
        if (node->is_dir()) st->st_mode |= EXT4_S_IFDIR;
        else st->st_mode |= EXT4_S_IFREG;
        st->st_nlink = 1;
        st->st_uid = node->perms.uid;
        st->st_gid = node->perms.gid;
        st->st_size = node->size;
        st->st_blksize = 4096;
        st->st_blocks = (node->size + 511) / 512;
        return 0;
    }

    // try ext4
    if (Ext4::IsMounted()) {
        Ext4Inode in;
        if (Ext4::Stat(resolved, &in) == 0) {
            st->st_mode = in.i_mode;
            st->st_nlink = in.i_links_count;
            st->st_uid = in.i_uid;
            st->st_gid = in.i_gid;
            st->st_size = in.i_size_lo;
            st->st_blksize = Ext4::BlockSize();
            st->st_blocks = in.i_blocks_lo;
            st->st_atime = in.i_atime;
            st->st_mtime = in.i_mtime;
            st->st_ctime = in.i_ctime;
            return 0;
        }
    }

    return -2;  // enoent
}

int32_t LinuxSyscall::sys_fstat(int fd, uintptr_t statbuf) {
    LinuxProcess* p = Current();
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;

    // use the path stored in the fd
    LinuxStat* st = (LinuxStat*)statbuf;
    memset(st, 0, sizeof(LinuxStat));

    LinuxFd* lfd = &p->fds[fd];

    if (lfd->type == LFD_CONSOLE) {
        st->st_mode = 0020666;  // character device
        st->st_blksize = 1024;
        return 0;
    }

    if (lfd->type == LFD_DEVNULL) {
        st->st_mode = 0020666;
        st->st_blksize = 4096;
        return 0;
    }

    // delegate to stat with the stored path. lfd->path is a kernel
    // address (64-bit under mcmodel=large) so pass the full pointer (satoru)
    return sys_stat((uintptr_t)lfd->path, statbuf);
}

// ── x86_64 stat family ──────────────────────────────────────────────────────
// x64 musl/glibc expect the 144-byte `struct LinuxStat64`, not the i386 layout
// that sys_stat/sys_fstat fill.  These handlers share the same kvfs/ext4
// resolution but format the 64-bit struct and validate the user statbuf.

namespace {

// Reject a statbuf the kernel must not write through: null, a wrapping range,
// or one not fully contained in a single mapped user region of the caller.
// Demand-zero pages *inside* a valid region fault in correctly on write, so we
// only require region membership, not that every page is already present. (satoru)
static bool stat_buf_writable(LinuxProcess* p, uint64_t ptr, uint64_t len) {
    if (!p || !p->task || !p->task->is_user()) return false;
    if (!ptr || len == 0) return false;
    uint64_t end = ptr + len;
    if (end < ptr) return false;                       // address-space wrap
    // a tracked region that fully covers it is fine (and handles demand-zero
    // pages not yet faulted in). (satoru)
    UserMemoryRegion* region = find_region(p->task, ptr);
    if (region && end <= region->end) return true;
    // otherwise accept it if the pages are actually mapped: the user stack and
    // ld-kurono-mapped elf segments are valid write targets that live OUTSIDE
    // the region table - e.g. musl fstat()s into a stack buffer while loading a
    // shared library. (rejecting those returned EFAULT and broke .so loading.)
    // (satoru)
    uint64_t first = ptr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last  = (end - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (uint64_t pg = first; pg <= last; pg += PAGE_SIZE) {
        if (!KernelVMM::QueryMappingInAddressSpace(p->task->address_space, pg))
            return false;
    }
    return true;
}

// Neutral stat result, shared by the formatter so the kvfs/ext4 resolution
// lives in one place.
struct KStatInfo {
    uint64_t ino;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t blksize;
    uint64_t blocks;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
};

// Resolve an already-translated kurono path via kvfs then ext4.  Returns 0 on
// success, or a negative errno mirroring sys_stat (-2 ENOENT). (satoru)
static int gather_stat_path(const char* resolved, KStatInfo* out) {
    memset(out, 0, sizeof(*out));

    KVFSNode* node = KVFS::Resolve(resolved);
    if (node) {
        out->ino  = (uint64_t)(uintptr_t)node;
        out->mode = node->perms.mode;
        switch (node->type) {
            case KVFS_DIR:
            case KVFS_MOUNTPOINT: out->mode |= 0040000; break;  // S_IFDIR
            case KVFS_SYMLINK:    out->mode |= 0120000; break;  // S_IFLNK
            case KVFS_DEVICE:     out->mode |= 0020000; break;  // S_IFCHR
            case KVFS_PIPE:       out->mode |= 0010000; break;  // S_IFIFO
            default:              out->mode |= 0100000; break;  // S_IFREG
        }
        out->nlink   = 1;
        out->uid     = node->perms.uid;
        out->gid     = node->perms.gid;
        out->size    = node->size;
        out->blksize = 4096;
        out->blocks  = (node->size + 511) / 512;
        out->atime   = node->accessed;
        out->mtime   = node->modified;
        out->ctime   = node->modified;
        return 0;
    }

    if (Ext4::IsMounted()) {
        Ext4Inode in;
        if (Ext4::Stat(resolved, &in) == 0) {
            out->mode    = in.i_mode;
            out->nlink   = in.i_links_count;
            out->uid     = in.i_uid;
            out->gid     = in.i_gid;
            out->size    = in.i_size_lo;
            out->blksize = Ext4::BlockSize();
            out->blocks  = in.i_blocks_lo;
            out->atime   = in.i_atime;
            out->mtime   = in.i_mtime;
            out->ctime   = in.i_ctime;
            return 0;
        }
    }
    return -2;  // ENOENT
}

static void fill_stat64(LinuxStat64* st, const KStatInfo* in) {
    memset(st, 0, sizeof(*st));
    st->st_dev     = 1;
    st->st_ino     = in->ino;
    st->st_nlink   = in->nlink;
    st->st_mode    = in->mode;
    st->st_uid     = in->uid;
    st->st_gid     = in->gid;
    st->st_rdev    = 0;
    st->st_size    = (int64_t)in->size;
    st->st_blksize = (int64_t)in->blksize;
    st->st_blocks  = (int64_t)in->blocks;
    st->st_atime   = in->atime;
    st->st_mtime   = in->mtime;
    st->st_ctime   = in->ctime;
}

}  // namespace

int32_t LinuxSyscall::sys_stat64(uintptr_t pathname, uintptr_t statbuf) {
    LinuxProcess* p = Current();
    if (!p) return -1;
    if (!stat_buf_writable(p, statbuf, sizeof(LinuxStat64))) return -14;  // EFAULT

    char resolved[256];
    ResolvePath((const char*)pathname, resolved, sizeof(resolved), p);

    KStatInfo info;
    int r = gather_stat_path(resolved, &info);
    if (r != 0) return r;

    fill_stat64((LinuxStat64*)statbuf, &info);
    return 0;
}

int32_t LinuxSyscall::sys_fstat64(int fd, uintptr_t statbuf) {
    LinuxProcess* p = Current();
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;   // EBADF
    if (!stat_buf_writable(p, statbuf, sizeof(LinuxStat64))) return -14;      // EFAULT

    LinuxFd* lfd = &p->fds[fd];

    if (lfd->type == LFD_CONSOLE || lfd->type == LFD_DEVNULL) {
        KStatInfo info;
        memset(&info, 0, sizeof(info));
        info.mode    = 0020666;   // S_IFCHR | rw-rw-rw-
        info.nlink   = 1;
        info.blksize = (lfd->type == LFD_CONSOLE) ? 1024 : 4096;
        fill_stat64((LinuxStat64*)statbuf, &info);
        return 0;
    }

    char resolved[256];
    ResolvePath(lfd->path, resolved, sizeof(resolved), p);

    KStatInfo info;
    int r = gather_stat_path(resolved, &info);
    if (r != 0) return r;

    fill_stat64((LinuxStat64*)statbuf, &info);
    return 0;
}

int LinuxSyscall::ReadlinkResolve(const char* path, char* buf, int bufsiz) {
    if (!path || !buf || bufsiz <= 0) return -22;   // -EINVAL
    LinuxProcess* p = Current();
    auto streq = [](const char* x, const char* y) {
        while (*x && *y) { if (*x != *y) return false; x++; y++; }
        return *x == *y;
    };
    // /proc/self/exe -> the recorded exec path. (satoru)
    if (streq(path, "/proc/self/exe")) {
        const char* exe = (p && p->task && p->task->exe_path[0]) ? p->task->exe_path : nullptr;
        if (!exe) return -2;
        int n = 0; while (exe[n] && n < bufsiz) { buf[n] = exe[n]; n++; }
        return n;
    }
    // /proc/self/fd/N (or /proc/<pid>/fd/N) -> the fd's recorded path. (satoru)
    {
        bool procish = ls_starts(path, "/proc/") || ls_starts(path, "/system/proc/");
        if (procish && p) {
            const char* after = nullptr;
            for (const char* c = path; c[0]; c++)
                if (c[0]=='/' && c[1]=='f' && c[2]=='d' && c[3]=='/') after = c + 4;
            if (after && *after) {
                int fd = 0; bool num = true;
                for (const char* d = after; *d; d++) {
                    if (*d < '0' || *d > '9') { num = false; break; }
                    fd = fd * 10 + (*d - '0');
                }
                if (num && fd >= 0 && fd < LINUX_MAX_FDS && p->fds[fd].open &&
                    p->fds[fd].path[0]) {
                    const char* fp = p->fds[fd].path;
                    int n = 0; while (fp[n] && n < bufsiz) { buf[n] = fp[n]; n++; }
                    return n;
                }
            }
        }
    }
    char resolved[256];
    ResolvePath(path, resolved, sizeof(resolved), p);
    KVFSNode* n = KVFS::Resolve(resolved);
    if (n && n->type == KVFS_SYMLINK) {
        // a real symlink -> return its target. (satoru)
        const char* t = n->link_target;
        int k = 0; while (t[k] && k < bufsiz) { buf[k] = t[k]; k++; }
        return k;
    }
    // exists but is NOT a symlink: -EINVAL, the posix answer. this was -ENOENT
    // on purpose for a while (a SUCCEEDING musl realpath() on the GRE/omni path
    // pushed 07-04-era firefox into a component-load path that deadlocked a
    // parking_lot futex) - but that deadlock class is now fixed (futex repoll +
    // the waiter-slot ownership fix + smp thread dispatch), and the broken
    // realpath had grown its own casualties: rust fs::canonicalize (musl
    // realpath) fails NotFound for EVERY existing dir, so rkv/kvstore
    // (extension-store, ProfD stores) errors out and firefox's chrome init
    // spams "uncaught exception: I/O error: NotFound" retry loops. missing
    // paths still return -ENOENT below. (satoru)
    if (n) return -22;   // -EINVAL: real node, not a symlink (satoru)
    return -2;           // -ENOENT: nothing at that path (satoru)
}

int32_t LinuxSyscall::sys_fstatat64(int dirfd, uintptr_t pathname,
                                    uintptr_t statbuf, int flags) {
    LinuxProcess* p = Current();
    if (!p) return -1;
    if (!stat_buf_writable(p, statbuf, sizeof(LinuxStat64))) return -14;  // EFAULT

    const char* path = (const char*)pathname;
    constexpr int AT_EMPTY_PATH = 0x1000;
    // musl/glibc implement fstat() as newfstatat(fd, "", buf, AT_EMPTY_PATH):
    // an empty path means "stat the dirfd itself" rather than a named file.
    if ((flags & AT_EMPTY_PATH) && (!path || path[0] == '\0')) {
        return sys_fstat64(dirfd, statbuf);
    }

    // dirfd is otherwise ignored: relative paths resolve against the process
    // cwd, matching the i386 LSYS_FSTATAT handler. (satoru)
    char resolved[256];
    ResolvePath(path, resolved, sizeof(resolved), p);

    KStatInfo info;
    int r = gather_stat_path(resolved, &info);
    if (r != 0) return r;

    fill_stat64((LinuxStat64*)statbuf, &info);
    return 0;
}

int32_t LinuxSyscall::sys_uname(uintptr_t buf) {
    LinuxUtsname* u = (LinuxUtsname*)buf;
    memset(u, 0, sizeof(LinuxUtsname));
    ls_scpy(u->sysname, "Linux", sizeof(u->sysname));
    ls_scpy(u->nodename, "kurono", sizeof(u->nodename));
    ls_scpy(u->release, "6.1.0-kurono", sizeof(u->release));
    ls_scpy(u->version, "#1 SMP Kurono OS Linux Subsystem", sizeof(u->version));
    ls_scpy(u->machine, "i686", sizeof(u->machine));
    ls_scpy(u->domainname, "(none)", sizeof(u->domainname));
    return 0;
}

int64_t LinuxSyscall::sys_getcwd(uintptr_t buf, uint64_t size) {
    LinuxProcess* p = Current();
    if (!p) return -1;
    char* dst = (char*)buf;
    ls_scpy(dst, p->cwd, (int)size);
    // this impl returns the (possibly >4gb) user buffer pointer; widen the
    // return so it is not truncated/sign-mangled back to userspace (satoru)
    return (int64_t)(uintptr_t)dst;
}

int32_t LinuxSyscall::sys_chdir(uintptr_t pathname) {
    LinuxProcess* p = Current();
    if (!p) return -1;
    const char* path = (const char*)pathname;
    char resolved[256];
    ResolvePath(path, resolved, sizeof(resolved), p);

    // verify it's a directory
    if (KVFS::IsDir(resolved) || (Ext4::IsMounted() && Ext4::IsDir(resolved))) {
        ls_scpy(p->cwd, resolved, sizeof(p->cwd));
        return 0;
    }
    return -20;  // enotdir
}

int32_t LinuxSyscall::sys_mkdir(uintptr_t pathname, uint32_t mode) {
    LinuxProcess* p = Current();
    if (!p) return -1;
    const char* path = (const char*)pathname;
    char resolved[256];
    ResolvePath(path, resolved, sizeof(resolved), p);

    int r = KVFS::Mkdir(resolved, (uint16_t)(mode & 0777));
    if (r == 0) return 0;

    if (Ext4::IsMounted()) {
        r = Ext4::Mkdir(resolved, (uint16_t)(mode & 0777));
        if (r == 0) return 0;
    }
    // the path already exists -> return -EEXIST, NOT -EPERM. musl/glibc map the
    // mkdir errno straight through, and gecko's nsLocalFileUnix maps EPERM(1) to
    // NS_ERROR_FILE_ACCESS_DENIED while EnsureDirectoryExists SWALLOWS EEXIST as
    // success. returning -1 made firefox's GetUserAppDataDirectory fail with
    // ACCESS_DENIED on the pre-created ~/.mozilla/firefox -> the ToolkitProfile
    // service came up null -> the "Profile Missing" dialog instead of a browser
    // window. (satoru)
    if (KVFS::Exists(resolved) || (Ext4::IsMounted() && Ext4::Exists(resolved)))
        return -17;   // -EEXIST
    return -13;       // -EACCES for a genuine create failure (satoru)
}

int32_t LinuxSyscall::sys_rmdir(uintptr_t pathname) {
    LinuxProcess* p = Current();
    if (!p) return -1;
    const char* path = (const char*)pathname;
    char resolved[256];
    ResolvePath(path, resolved, sizeof(resolved), p);

    if (KVFS::Rmdir(resolved) == 0) return 0;
    if (Ext4::IsMounted() && Ext4::Rmdir(resolved) == 0) return 0;
    return -2;
}

int32_t LinuxSyscall::sys_unlink(uintptr_t pathname) {
    LinuxProcess* p = Current();
    if (!p) return -1;
    const char* path = (const char*)pathname;
    char resolved[256];
    ResolvePath(path, resolved, sizeof(resolved), p);

    if (KVFS::Unlink(resolved) == 0) return 0;
    if (Ext4::IsMounted() && Ext4::Unlink(resolved) == 0) return 0;
    return -2;
}

int32_t LinuxSyscall::sys_access(uintptr_t pathname, uint32_t mode) {
    (void)mode;
    LinuxProcess* p = Current();
    if (!p) return -1;
    const char* path = (const char*)pathname;
    char resolved[256];
    ResolvePath(path, resolved, sizeof(resolved), p);

    if (KVFS::Exists(resolved)) return 0;
    if (Ext4::IsMounted() && Ext4::Exists(resolved)) return 0;
    return -2;
}

int32_t LinuxSyscall::sys_dup(int oldfd) {
    LinuxProcess* p = Current();
    if (!p || oldfd < 0 || oldfd >= LINUX_MAX_FDS || !p->fds[oldfd].open)
        return -9;

    int newfd = AllocFd(p);
    if (newfd < 0) return -24;

    memcpy(&p->fds[newfd], &p->fds[oldfd], sizeof(LinuxFd));
    // aliased backends are refcounted so either fd's close is safe (satoru)
    if (p->fds[newfd].type == LFD_INET) LinuxNetBridge::Retain(p->fds[newfd].backend_fd);
    return newfd;
}

int32_t LinuxSyscall::sys_dup2(int oldfd, int newfd) {
    LinuxProcess* p = Current();
    if (!p || oldfd < 0 || oldfd >= LINUX_MAX_FDS || !p->fds[oldfd].open)
        return -9;
    if (newfd < 0 || newfd >= LINUX_MAX_FDS) return -9;

    if (p->fds[newfd].open) {
        sys_close(newfd);
    }
    memcpy(&p->fds[newfd], &p->fds[oldfd], sizeof(LinuxFd));
    if (p->fds[newfd].type == LFD_INET) LinuxNetBridge::Retain(p->fds[newfd].backend_fd);
    return newfd;
}

int32_t LinuxSyscall::sys_ioctl(int fd, uint32_t cmd, uint64_t arg) {
    LinuxProcess* p = Current();
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;
    LinuxFd* lfd = &p->fds[fd];

    // FIONREAD: bytes available to read without blocking. firefox's wayland
    // proxy issues this on its client socket to size each read; returning enotty
    // for it made ProxiedConnection::Process abort with "Failed to read data
    // from client" and tear the whole connection down. (satoru)
    if (cmd == 0x541Bu /* FIONREAD */) {
        int navail = 0;
        if ((lfd->type == LFD_SOCKET || lfd->type == LFD_PIPE) && lfd->backend_fd >= 0)
            navail = UnixSocket::PendingBytes(lfd->backend_fd);
        else if (lfd->type == LFD_INET)
            navail = LinuxNetBridge::RxAvail(lfd->backend_fd);   // nspr PR_Available (satoru)
        if (navail < 0) navail = 0;
        if (arg && p->task) write_user_u32(p->task, arg, (uint32_t)navail);
        return 0;
    }
    // FIONBIO: set/clear O_NONBLOCK from the user int - nspr toggles inet
    // sockets this way as often as via fcntl. (satoru)
    if (cmd == 0x5421u /* FIONBIO */ && lfd->type == LFD_INET) {
        uint32_t v = 0;
        if (arg && p->task) read_user_u32(p->task, arg, &v);
        if (v) lfd->flags |= L_O_NONBLOCK; else lfd->flags &= ~(uint32_t)L_O_NONBLOCK;
        return 0;
    }
    // FIONBIO: set/clear non-blocking. we drive o_nonblock through the fcntl
    // flags, so just acknowledge instead of returning enotty to probes. (satoru)
    if (cmd == 0x5421u /* FIONBIO */) return 0;

    // terminal ioctls - return enotty for non-ttys
    if (lfd->type == LFD_CONSOLE) return 0;  // pretend success
    return -25;    // enotty
}

int64_t LinuxSyscall::sys_mmap(uintptr_t addr, uint64_t length, uint32_t prot,
                                uint32_t flags, int fd, uint64_t offset) {
    if (length == 0) return -22;

    LinuxProcess* proc = Current();
    Process* task = proc ? proc->task : nullptr;
    if (!task || !task->is_user()) {
        void* mem = KernelHeap::Alloc(length);
        if (!mem) return -12;
        memset(mem, 0, length);
        // kernel-heap address is 64-bit under mcmodel=large - return it
        // through the widened result so it is not truncated (satoru)
        return (int64_t)(uintptr_t)mem;
    }

    // (satoru) serialize the reserve-and-map so concurrent same-cr3 threads under -smp
    // can't double-hand one virtual range (the SharedStringMap frozen-memfd crash). the
    // guard auto-releases on every return below. no cli/sti (maps can touch the buddy
    // allocator) - process-context leaf lock, so the same-core-preempt case resolves via
    // the timer re-scheduling the holder; no path re-enters sys_mmap under it. (satoru)
    SpinLockCpuGuard _mmap_guard(g_mmap_lock);

    if (fd >= 0) {
        // (a) memfd / shared-memory objects: map the object's physical pages
        // straight into the caller so writes are shared (wl_shm pixel pools,
        // posix shm). (satoru)
        LinuxShmObj* shm = shm_for_fd(proc, fd);
        if (shm && shm->base) {
            uint64_t msize = align_up_u64(length, PAGE_SIZE);
            // (satoru) THE SharedStringMap CRASH: shm->size is the exact ftruncate
            // size (often NOT page-aligned, e.g. 0x10A76). the physical backing is
            // page-ROUNDED though (shm_set_size allocs align_up(size)), so a full-size
            // read-only map of the whole memfd needs align_up(length) pages == the
            // rounded backing. comparing the rounded map length against the UNrounded
            // shm->size spuriously rejected that last map -> MAP_FAILED ->
            // MOZ_RELEASE_ASSERT(mapping.IsValid()). bound against the rounded backing
            // (what actually exists); the tail page past shm->size is zero-filled. this
            // is the real, deterministic fix - the "race" was just the frozen memfd's
            // byte size happening to land on a page boundary on lucky runs. (satoru)
            if (offset + msize > align_up_u64(shm->size, PAGE_SIZE)) return -22;
            uint64_t vbase = choose_mmap_base(task, addr, msize);
            if (!vbase) return -12;
            uint64_t pflags = page_flags_from_prot(prot);
            uint64_t phys0 = (uint64_t)(uintptr_t)shm->base + offset;
            for (uint64_t o = 0; o < msize; o += PAGE_SIZE) {
                if (!KernelVMM::MapPageInAddressSpace(task->address_space,
                                                      vbase + o, phys0 + o, pflags)) {
                    return -12;
                }
                // the mapping BORROWS the shm object's frames - count the
                // reference so a later munmap (or the freeze() munmap+remap
                // dance mozilla's base::SharedMemory does) only DROPS the ref
                // instead of freeing the object's live backing. without this,
                // remapping after an unmap read REUSED frames = garbage in the
                // shared string/font maps (the AssignLiteral MOZ_CRASH). (satoru)
                if (SMP::OnlineCount() > 1) PMM::RetainFrame(phys0 + o);
            }
            // tracked so munmap works; not demand-zero (pages already mapped). (satoru)
            add_region(task, vbase, vbase + msize, pflags, USER_REGION_MMAP);
            if (Scheduler::GetCurrentProcess() == task) {
                for (uint64_t o = 0; o < msize; o += PAGE_SIZE)
                    KernelVMM::InvalidatePage(vbase + o);
            }
            task->next_mmap_base = vbase + msize;
            return (int64_t)vbase;
        }

        // (b) regular file (kvfs): copy the file's bytes into freshly-allocated
        // private pages and map them. THIS is how musl's dynamic linker loads
        // every .so segment - mmap(fd, MAP_PRIVATE[, MAP_FIXED], offset) - so
        // it's the gate for the firefox closure. mapped eagerly (no demand
        // paging of file content); bytes past EOF are zeroed (.bss tail). a
        // MAP_FIXED map overlays an earlier reservation, so we free the page we
        // displace to keep peak memory at ~one copy of the image. (satoru)
        constexpr uint32_t MAP_FIXED_FLAG = 0x10;
        LinuxFd* lfd = (fd < LINUX_MAX_FDS) ? &proc->fds[fd] : nullptr;
        if (lfd && lfd->open && lfd->type == LFD_KVFS) {
            KVFSNode* node = KVFS::Resolve(lfd->path);
            if (!node || node->is_dir()) return -13;          // eacces
            const uint8_t* content = node->content;
            uint32_t fsize = node->size;
            uint64_t msize = align_up_u64(length, PAGE_SIZE);
            bool fixed = (flags & MAP_FIXED_FLAG) != 0;
            uint64_t vbase = fixed ? (addr & ~(uint64_t)(PAGE_SIZE - 1))
                                   : choose_mmap_base(task, addr, msize);
            if (!vbase) return -12;
            // (satoru) [libmap] logs each big .so segment map (name + base) to
            // symbolize a stalled/faulting rip against the lib. gated off for the
            // checkpoint; flip the guard to true when a fresh base is needed.
            if (false && msize > 0x2000000) {  // libxul is the ~64MB one (satoru)
                SerialLogger::Log("[libmap] "); SerialLogger::Log(lfd->path);
                SerialLogger::Log(" @"); SerialLogger::LogHex((uint32_t)(vbase >> 32));
                SerialLogger::Log(":"); SerialLogger::LogHex((uint32_t)(vbase & 0xFFFFFFFFu));
                SerialLogger::Log(" len="); SerialLogger::LogHex((uint32_t)length);
                SerialLogger::Log(" off="); SerialLogger::LogHex((uint32_t)offset);
                SerialLogger::Log("\r\n");
            }
            uint64_t pflags = page_flags_from_prot(prot);
            bool active = (Scheduler::GetCurrentProcess() == task);
            for (uint64_t o = 0; o < msize; o += PAGE_SIZE) {
                uint64_t va = vbase + o;
                if (fixed) {
                    uint64_t old = KernelVMM::QueryMappingInAddressSpace(task->address_space, va);
                    if (old) user_frame_quarantine(old);   // stale-tlb-safe (smp) (satoru)
                }
                void* pg = PMM::AllocBytes(PAGE_SIZE);
                if (!pg) return -12;
                uint64_t fpos = offset + o;
                uint32_t copy = 0;
                if (content && fpos < fsize) {
                    uint64_t avail = (uint64_t)fsize - fpos;
                    copy = (uint32_t)(avail < PAGE_SIZE ? avail : PAGE_SIZE);
                    memcpy(pg, content + fpos, copy);
                }
                if (copy < PAGE_SIZE) memset((uint8_t*)pg + copy, 0, PAGE_SIZE - copy);
                if (!KernelVMM::MapPageInAddressSpace(task->address_space, va,
                                                      (uint64_t)(uintptr_t)pg, pflags)) {
                    PMM::FreeBytes(pg, PAGE_SIZE);
                    return -12;
                }
                if (active) KernelVMM::InvalidatePage(va);
            }
            // a fixed overlay sits inside an already-tracked reservation; only
            // record + bump the arena for a fresh (non-fixed) mapping. (satoru)
            if (!fixed) {
                add_region(task, vbase, vbase + msize, pflags, USER_REGION_MMAP);
                task->next_mmap_base = vbase + msize;
            } else {
                // smp: the overlay REPLACED live translations - flush the other
                // cores so a sibling can't keep writing the displaced frames. (satoru)
                SMP::BroadcastTlbFlush();
            }
            return (int64_t)vbase;
        }

        return -38;   // other fd types are not mmappable
    }

    uint64_t size = align_up_u64(length, PAGE_SIZE);
    uint64_t page_flags = page_flags_from_prot(prot);

    // anonymous MAP_FIXED: the caller demands this exact address and expects any
    // existing mapping there to be REPLACED. musl's map_library uses this to map
    // the .bss tail of a dso's data segment that spills past the file-backed
    // pages (mmap(base+vaddr, n, RW, MAP_ANON|MAP_FIXED, -1, 0)). that range sits
    // INSIDE the dso's prior PROT_READ reservation. the old anon path ran
    // choose_mmap_base() which, on overlap, SLID to a different address and
    // returned that - musl saw addr != requested and failed the load with EINVAL
    // ("Error loading shared library ...: Invalid argument"): the
    // libgraphite2/harfbuzz blocker. honor the fixed address by REMAPPING the
    // pages in place - exactly like the file-backed FIXED branch above: free the
    // displaced frame, map a fresh zeroed page with the requested prot. crucially
    // we do NOT split/carve the underlying reservation region: doing so desynced
    // region_overlaps() from the page tables, so a later malloc mmap got handed an
    // address still physically mapped PROT_READ and #PF'd writing free-list
    // metadata into it. leave the reservation region intact (the pages are now
    // present+writable; region flags only gate not-yet-faulted demand-zero pages,
    // and these are faulted in). only add a region if the fixed range lands
    // outside every existing one. (satoru)
    {
        constexpr uint32_t MAP_FIXED_FLAG = 0x10;
        if (flags & MAP_FIXED_FLAG) {
            uint64_t fbase = align_down_u64(addr, PAGE_SIZE);
            if (!fbase) return -22;                 // can't fix at NULL
            bool active = (Scheduler::GetCurrentProcess() == task);
            for (uint64_t o = 0; o < size; o += PAGE_SIZE) {
                uint64_t va = fbase + o;
                uint64_t old = KernelVMM::QueryMappingInAddressSpace(task->address_space, va);
                if (old) user_frame_quarantine(old);   // stale-tlb-safe (smp) (satoru)
                void* pg = PMM::AllocBytes(PAGE_SIZE);
                if (!pg) return -12;
                memset(pg, 0, PAGE_SIZE);
                if (!KernelVMM::MapPageInAddressSpace(task->address_space, va,
                                                      (uint64_t)(uintptr_t)pg, page_flags)) {
                    PMM::FreeBytes(pg, PAGE_SIZE);
                    return -12;
                }
                if (active) KernelVMM::InvalidatePage(va);
            }
            // if no existing region covers the fixed range, track it so munmap and
            // region_overlaps stay correct; otherwise leave the covering region as
            // is (splitting it is what caused the desync). (satoru)
            if (!region_overlaps(task, fbase, fbase + size))
                add_region(task, fbase, fbase + size, page_flags, USER_REGION_MMAP);
            if (task->next_mmap_base < fbase + size) task->next_mmap_base = fbase + size;
            // smp: the fixed overlay replaced live translations - flush the other
            // cores so a sibling can't keep using the displaced frames. (satoru)
            SMP::BroadcastTlbFlush();
            return (int64_t)fbase;
        }
    }

    uint64_t base = choose_mmap_base(task, addr, size);
    // (satoru) TEMP [bigmap]: trace giant anon reservations (the rlbox/wasm2c
    // 4-8gb PROT_NONE linear-memory mmap) - result base + prot. remove before commit.
    if (length >= (1ULL << 30)) {
        SerialLogger::Log("[bigmap] len="); SerialLogger::LogHex((uint32_t)(length >> 32));
        SerialLogger::Log(":"); SerialLogger::LogHex((uint32_t)length);
        SerialLogger::Log(" prot="); SerialLogger::LogDec((int)prot);
        SerialLogger::Log(" base="); SerialLogger::LogHex((uint32_t)(base >> 32));
        SerialLogger::Log(":"); SerialLogger::LogHex((uint32_t)base);
        SerialLogger::Log("\r\n");
    }
    if (!base) return -12;

    if (!add_region(task, base, base + size, page_flags,
                    USER_REGION_DEMAND_ZERO | USER_REGION_MMAP)) {
        return -12;
    }

    task->next_mmap_base = base + size;
    // base may sit above 4gb; return the full 64-bit address (satoru)
    return (int64_t)base;
}

int32_t LinuxSyscall::sys_munmap(uintptr_t addr, uint64_t length) {
    if (length == 0) return 0;

    LinuxProcess* proc = Current();
    Process* task = proc ? proc->task : nullptr;
    if (!task || !task->is_user()) {
        return 0;
    }
    // (satoru) same g_mmap_lock as sys_mmap: firefox's SharedMemory::Freeze() munmaps
    // the writable memfd map then re-maps it read-only. if that munmap (region/shm
    // ref bookkeeping) races a sibling thread's concurrent mmap, the shm object's
    // base/size is corrupted and the read-only re-map returns null -> the crash. serialize
    // all address-space mutation. (satoru)
    SpinLockCpuGuard _munmap_guard(g_mmap_lock);
    // the region table lives on the thread-group LEADER (add_region/find_region
    // resolve region_owner). a WORKER thread that mmap'd a range - e.g. musl
    // pthread_create building the SwComposite thread's stack from a webrender
    // worker - registered it on the leader too, so scan the leader here, not the
    // worker's own (empty) table. scanning the wrong table returned "no region"
    // and failed the thread's stack teardown/setup. (satoru)
    task = region_owner(task);

    uint64_t start = align_down_u64(addr, PAGE_SIZE);
    uint64_t end = align_up_u64((uint64_t)addr + length, PAGE_SIZE);
    bool unmapped = false;

    for (int i = 0; i < PROCESS_MAX_USER_REGIONS; i++) {
        UserMemoryRegion* region = &task->regions[i];
        if (!region->active) continue;

        uint64_t overlap_start = start > region->start ? start : region->start;
        uint64_t overlap_end = end < region->end ? end : region->end;
        if (overlap_start >= overlap_end) continue;

        unmap_user_range(task, overlap_start, overlap_end);
        if (!split_or_trim_region(task, region, overlap_start, overlap_end)) {
            return -12;
        }
        unmapped = true;
    }

    if (task->next_mmap_base > start) {
        task->next_mmap_base = start;
    }

    return unmapped ? 0 : -22;
}

// madvise(addr, len, advice): honor MADV_DONTNEED / MADV_FREE for real. a no-op here
// (the old behavior) let firefox's allocator + js GC "decommit" pages without ever
// reclaiming them - kurono has no swap, so the GC's freed-memory accounting diverged
// from real rss and it grew without bound until the host OOM'd. we free the present
// pages of every covered DEMAND_ZERO (anonymous) region and KEEP the region, so the
// next touch refaults a fresh zero page - exactly MADV_DONTNEED semantics on anon
// memory. non-demand-zero mappings (elf text/data) are skipped: zeroing those on
// refault would destroy code/initialized data. other advice stays a hint we ignore. (satoru)
int32_t LinuxSyscall::sys_madvise(uintptr_t addr, uint64_t length, uint32_t advice) {
    if (advice != LINUX_MADV_DONTNEED && advice != LINUX_MADV_FREE) return 0;
    if (length == 0) return 0;

    LinuxProcess* proc = Current();
    Process* task = proc ? proc->task : nullptr;
    if (!task || !task->is_user()) return 0;
    task = region_owner(task);   // regions live on the thread-group leader (satoru)

    uint64_t start = align_down_u64(addr, PAGE_SIZE);
    uint64_t end = align_up_u64((uint64_t)addr + length, PAGE_SIZE);
    if (end <= start) return 0;

    for (int i = 0; i < PROCESS_MAX_USER_REGIONS; i++) {
        UserMemoryRegion* region = &task->regions[i];
        if (!region->active) continue;
        if (!(region->flags & USER_REGION_DEMAND_ZERO)) continue;   // anon only (satoru)
        uint64_t os = start > region->start ? start : region->start;
        uint64_t oe = end < region->end ? end : region->end;
        if (os >= oe) continue;
        unmap_user_range(task, os, oe);   // frees present frames; region stays -> refaults zero (satoru)
    }
    return 0;
}

// mprotect(addr, len, prot): change the protection of an existing user mapping.
// this is what makes w^x jits work - e.g. spidermonkey mmaps a code buffer rw,
// writes machine code into it, then mprotect()s it rx and jumps in. a no-op stub
// leaves the pages writable+nx, so the cpu either faults on the instruction
// fetch (nx) or runs with the wrong perms. here we (1) re-derive the pte flags
// from prot, (2) update every overlapping user region's page_flags (splitting
// regions on partial coverage so future demand-zero faults pick up the new prot)
// and (3) rewrite the live pte of any already-faulted-in page in place + flush
// the tlb. nx is handled by page_flags_from_prot: PROT_EXEC clears PTE_NX,
// PROT_WRITE without PROT_EXEC sets it, and efer.nxe enforces it. (satoru)
int32_t LinuxSyscall::sys_mprotect(uintptr_t addr, uint64_t length, uint32_t prot) {
    // NOTE (ws4): the RW->RX transition here is already W^X-correct - the pte
    // flip below sets/clears PTE_NX via page_flags_from_prot (PROT_EXEC clears
    // NX), and SMP::BroadcastTlbFlush() is SYNCHRONOUS (it spins until every
    // online peer acks, smp.cpp), so no core can instruction-fetch a stale
    // writable/NX translation of freshly-jitted code. the residual firefox JIT
    // crash (rip 0x1800...) is therefore NOT a kernel shootdown-timing bug; it
    // is gecko-internal codegen/W^X coherency (the memfd dual-map path). (satoru)
    if (length == 0) return 0;

    LinuxProcess* proc = Current();
    Process* task = proc ? proc->task : nullptr;
    if (!task || !task->is_user()) {
        // no per-process address space to reprotect; nothing to do (satoru)
        return 0;
    }
    // (satoru) same g_mmap_lock as sys_mmap/sys_munmap: mprotect mutates the shared
    // leader region table + page-table perms; must not race a sibling mmap/munmap in
    // the shared cr3. (the freeze dance + rlbox reservations mprotect concurrently.)
    SpinLockCpuGuard _mprot_guard(g_mmap_lock);
    // regions live on the thread-group leader (add_region/find_region resolve
    // region_owner). a worker thread mprotecting a range it mmap'd - musl
    // pthread_create unprotecting a new thread's stack - must scan the leader's
    // table, not its own empty one, or every such mprotect returns ENOMEM and
    // the thread spawn fails ("Failed creating SwComposite thread"). (satoru)
    task = region_owner(task);

    // addr must be page-aligned per posix; len rounds up to a page (satoru)
    if (addr & (PAGE_SIZE - 1)) return -22;        // einval
    uint64_t start = align_down_u64(addr, PAGE_SIZE);
    uint64_t end = align_up_u64((uint64_t)addr + length, PAGE_SIZE);
    if (end <= start) return -22;                  // einval (overflow / empty)
    if (end > USER_SPACE_TOP + 1) return -22;      // outside the user half

    uint64_t new_flags = page_flags_from_prot(prot);

    // step 1: bring the region table in line so demand-zero pages that have not
    // faulted in yet get the new protection on first touch. for each active
    // region overlapping [start,end) we split off the covered span and stamp it
    // with new_flags, leaving the non-covered remainder(s) on their old flags.
    // iterating the whole array is safe: a remainder slot we create lies outside
    // [start,end) (empty overlap -> skipped on re-scan) and a covered slot we
    // create already carries new_flags (re-stamp is idempotent). (satoru)
    bool covered_any = false;
    for (int i = 0; i < PROCESS_MAX_USER_REGIONS; i++) {
        UserMemoryRegion* region = &task->regions[i];
        if (!region->active) continue;

        uint64_t os = start > region->start ? start : region->start;
        uint64_t oe = end < region->end ? end : region->end;
        if (os >= oe) continue;                    // no overlap (satoru)
        covered_any = true;

        if (region->page_flags == new_flags) continue;  // already this prot (satoru)

        uint64_t rs = region->start, re = region->end;
        uint32_t rflags = region->flags;           // DEMAND_ZERO / MMAP / HEAP (satoru)

        if (os <= rs && oe >= re) {
            // whole region covered - just restamp its protection (satoru)
            region->page_flags = new_flags;
            continue;
        }

        // partial coverage: shrink this slot to one of the surviving pieces and
        // re-add the others. need up to two free slots (middle split). if none
        // are free we cannot split correctly, so fail rather than silently
        // reprotecting bytes outside [start,end). (satoru)
        if (os <= rs) {
            // covered prefix [rs,oe): keep tail [oe,re) on old flags here,
            // add the covered head with new flags. (satoru)
            region->start = oe;
            if (!add_region(task, rs, oe, new_flags, rflags)) return -12;  // enomem
        } else if (oe >= re) {
            // covered suffix [os,re): keep head [rs,os) on old flags here,
            // add the covered tail with new flags. (satoru)
            region->end = os;
            if (!add_region(task, os, re, new_flags, rflags)) return -12;
        } else {
            // covered middle [os,oe): head [rs,os) stays here on old flags,
            // add covered middle (new flags) + tail [oe,re) (old flags). (satoru)
            region->end = os;
            if (!add_region(task, os, oe, new_flags, rflags)) return -12;
            if (!add_region(task, oe, re, region->page_flags, rflags)) return -12;
        }
    }

    // step 2: rewrite the live pte of every page in [start,end) that is already
    // mapped (faulted-in rw jit buffers land here and get flipped to rx). pages
    // not yet present are skipped - they will fault in later with the region
    // flags updated above. collect whether anything changed so we can flush. (satoru)
    bool active_cr3 = (Scheduler::GetCurrentProcess() == task);
    bool mapped_any = false;
    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        if (KernelVMM::ProtectPageInAddressSpace(task->address_space, page, new_flags)) {
            mapped_any = true;
            if (active_cr3) KernelVMM::InvalidatePage(page);
        }
    }
    // smp: flush stale translations on the other cores running this address
    // space's sibling threads (a page just flipped ro/nx must not stay writable
    // in another core's tlb). no-op with one online cpu. (satoru)
    if (mapped_any) SMP::BroadcastTlbFlush();

    // linux returns 0 even when the range has no backing region yet (a fresh
    // demand-zero mmap region still counts). it ALSO succeeds for a range that
    // is live in the page tables but not in our region array - e.g. elf segments
    // mapped directly by ld-kurono, which musl then mprotect()s read-only for
    // RELRO. (returning enomem there made musl treat RELRO as a fatal error and
    // abort with exit 127, even though step 2 above already applied the new
    // protection.) only reject when the range overlaps NO region AND maps no
    // live page at all -> genuinely unmapped -> enomem per posix. (satoru)
    if (!covered_any && !mapped_any) {
        return -12;
    }
    return 0;
}

int32_t LinuxSyscall::sys_nanosleep(uintptr_t req, uintptr_t rem) {
    (void)rem;
    struct { uint32_t tv_sec; uint32_t tv_nsec; }* ts =
        (decltype(ts))req;
    if (ts) {
        uint32_t ms = ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
        if (ms > 5000) ms = 5000;  // cap at 5 seconds
        if (ms > 0) {
            uint32_t start = Time::GetTicks();
            while ((Time::GetTicks() - start) < ms) {
                if (SMP::CpuIndex() == 0) KuronoShell::PumpUI();   // bsp-only (satoru)
                // drop the kls lock across the wait so a multi-second sleep on one
                // core does not stall every other core syscalls. (satoru)
                kls_relax();
                HAL::WaitForInterrupt();
            }
        }
    }
    return 0;
}

int32_t LinuxSyscall::sys_getdents64(int fd, uintptr_t dirp, uint64_t count) {
    LinuxProcess* p = Current();
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;

    LinuxFd* lfd = &p->fds[fd];
    uint8_t* buf = (uint8_t*)dirp;
    uint32_t pos = 0;

    if (lfd->type == LFD_KVFS) {
        KVFSNode* node = KVFS::Resolve(lfd->path);
        if (!node || !node->is_dir()) return -20;

        // (satoru) TEMP: debug fontconfig finding no fonts -- log each dir listing
        // a firefox thread requests (path + child_count). if /system/fonts shows
        // child_count 0 the extracted fonts aren't linked as children (kvfs bug);
        // if >0 the listing works and the issue is downstream (parse/cache).
        if (false && p->pid >= 100 && lfd->offset == 0) {  // [gdents] gated off - fonts confirmed (satoru)
            SerialLogger::Log("[gdents] "); SerialLogger::Log(lfd->path);
            SerialLogger::Log(" n="); SerialLogger::LogDec(node->child_count);
            SerialLogger::Log("\r\n");
        }

        int start = (int)lfd->offset;
        for (int i = start; i < node->child_count && pos + 280 < count; i++) {
            KVFSNode* child = node->children[i];
            if (!child) continue;

            LinuxDirent64* de = (LinuxDirent64*)(buf + pos);
            de->d_ino = (uint64_t)(uintptr_t)child;
            de->d_type = child->is_dir() ? 4 : 8;
            int nl = ls_slen(child->name);
            memcpy(de->d_name, child->name, nl);
            de->d_name[nl] = 0;
            uint16_t reclen = (uint16_t)(19 + nl + 1 + 7) & ~7;  // align 8
            de->d_reclen = reclen;
            de->d_off = (uint64_t)(i + 1);
            pos += reclen;
            lfd->offset = (uint64_t)(i + 1);
        }
    } else if (lfd->type == LFD_EXT4 && Ext4::IsMounted()) {
        Ext4DirInfo entries[32];
        int n = Ext4::ListDir(lfd->path, entries, 32);

        int start = (int)lfd->offset;
        for (int i = start; i < n && pos + 280 < count; i++) {
            LinuxDirent64* de = (LinuxDirent64*)(buf + pos);
            de->d_ino = entries[i].inode;
            de->d_type = entries[i].file_type;
            int nl = ls_slen(entries[i].name);
            memcpy(de->d_name, entries[i].name, nl);
            de->d_name[nl] = 0;
            uint16_t reclen = (uint16_t)(19 + nl + 1 + 7) & ~7;
            de->d_reclen = reclen;
            de->d_off = (uint64_t)(i + 1);
            pos += reclen;
            lfd->offset = (uint64_t)(i + 1);
        }
    }

    return (int32_t)pos;
}

int32_t LinuxSyscall::sys_clock_gettime(uint32_t clk_id, uintptr_t tp) {
    // x86_64 struct timespec is { long tv_sec; long tv_nsec; } = 16 bytes;
    // both fields are 8 bytes. the old code wrote a 32-bit pair (8 bytes total)
    // which under-fills the user buffer on the x64 path. (satoru)
    struct timespec64 { int64_t tv_sec; int64_t tv_nsec; };
    if (!tp) return -14;        // -EFAULT
    timespec64* ts = (timespec64*)tp;

    // CLOCK_REALTIME(0) / CLOCK_REALTIME_COARSE(5): wall-clock. source the
    // microsecond utc clock (RTC unix-epoch seconds captured at boot + the
    // monotonic uptime + the sub-ms PIT fraction) from TimeManager::NowUTC.
    // if the RTC epoch was never set this is "boot epoch + uptime" instead,
    // but it is still wall-clock-shaped and never jumps. (satoru)
    if (clk_id == 0 || clk_id == 5) {
        uint64_t us = TimeManager::NowUTC().us;
        ts->tv_sec  = (int64_t)(us / 1000000ull);
        ts->tv_nsec = (int64_t)((us % 1000000ull) * 1000ull);
        return 0;
    }

    // CLOCK_MONOTONIC(1) / CLOCK_MONOTONIC_RAW(4) / CLOCK_BOOTTIME(7): a
    // strictly increasing nanosecond value derived from the TSC-based
    // millisecond uptime clock (no 49.7-day wrap, cadence-independent). a
    // static floor guards against any one-time backward step when the clock
    // source switches at boot, so MONOTONIC never goes backwards. firefox
    // uses this for performance timing - drift/discontinuity shows as
    // stutter. (satoru)
    static uint64_t mono_floor_ms = 0;
    uint64_t ms = Timer::GetRealMs64();
    if (ms < mono_floor_ms) ms = mono_floor_ms; else mono_floor_ms = ms;
    ts->tv_sec  = (int64_t)(ms / 1000ull);
    ts->tv_nsec = (int64_t)((ms % 1000ull) * 1000000ull);
    return 0;
}

int32_t LinuxSyscall::sys_set_thread_area(uintptr_t u_info) {
    (void)u_info;
    // tls setup - simplified: pretend success
    return 0;
}

//  console output capture - read syscall output back into the shell

bool LinuxSyscall::HasConsoleOutput() {
    return console_head != console_tail;
}

bool LinuxSyscall::StdinReadable() {
    return stdin_head != stdin_tail;
}

int LinuxSyscall::ReadConsoleOutput(char* buf, int max_len) {
    int read = 0;
    while (read < max_len && console_head != console_tail) {
        buf[read++] = console_buf[console_tail];
        console_tail = (console_tail + 1) % CONSOLE_BUF_SIZE;
    }
    return read;
}

void LinuxSyscall::ClearConsoleOutput() {
    console_head = 0;
    console_tail = 0;
}

//  stdin injection - push data from shell into linux process stdin

void LinuxSyscall::InjectStdin(const char* data, int len) {
    for (int i = 0; i < len; i++) {
        int next = (stdin_head + 1) % STDIN_BUF_SIZE;
        if (next == stdin_tail) break; // buffer full
        stdin_buf[stdin_head] = data[i];
        stdin_head = next;
    }
}

//  runprogram - execute a simulated linux program via syscalls
//  this creates a process context, runs the named builtin, captures
//  all console output, and returns it to the caller.

// built-in linux programs that exercise real syscalls
static void builtin_hello(LinuxSyscall* /*sys*/) {
    const char msg[] = "Hello from Linux syscall layer!\n";
    LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)msg,
                           sizeof(msg) - 1, 0, 0);
}

static void builtin_uname(LinuxSyscall* /*sys*/) {
    LinuxUtsname u;
    LinuxSyscall::Dispatch(LSYS_UNAME, (uint32_t)(uintptr_t)&u, 0, 0, 0, 0);
    char line[256];
    int p = 0;
    auto sa = [&](const char* s) { while (*s && p < 250) line[p++] = *s++; };
    sa(u.sysname); line[p++] = ' ';
    sa(u.nodename); line[p++] = ' ';
    sa(u.release); line[p++] = ' ';
    sa(u.version); line[p++] = ' ';
    sa(u.machine); line[p++] = '\n';
    line[p] = 0;
    LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)line, p, 0, 0);
}

static void builtin_getpid(LinuxSyscall* /*sys*/) {
    int32_t pid = LinuxSyscall::Dispatch(LSYS_GETPID, 0, 0, 0, 0, 0);
    char line[64];
    int p = 0;
    auto sa = [&](const char* s) { while (*s && p < 60) line[p++] = *s++; };
    sa("PID: ");
    // int to string
    if (pid <= 0) { line[p++] = '0'; } else {
        char tmp[12]; int ti = 0;
        int v = pid;
        while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; }
        while (ti > 0) line[p++] = tmp[--ti];
    }
    line[p++] = '\n'; line[p] = 0;
    LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)line, p, 0, 0);
}

static void builtin_pwd(LinuxSyscall* /*sys*/) {
    char cwd[256];
    LinuxSyscall::Dispatch(LSYS_GETCWD, (uint32_t)(uintptr_t)cwd, 256, 0, 0, 0);
    int len = 0; while (cwd[len]) len++;
    cwd[len++] = '\n'; cwd[len] = 0;
    LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)cwd, len, 0, 0);
}

static void builtin_ls(LinuxSyscall* /*sys*/) {
    // open current directory via syscall, read entries via getdents64
    const char* path = ".";
    int fd = LinuxSyscall::Dispatch(LSYS_OPEN, (uint32_t)(uintptr_t)path,
                                     L_O_RDONLY | L_O_DIRECTORY, 0, 0, 0);
    if (fd < 0) {
        const char* msg = "ls: cannot open directory\n";
        LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)msg, 26, 0, 0);
        return;
    }
    uint8_t buf[1024];
    int32_t n = LinuxSyscall::Dispatch(LSYS_GETDENTS64, (uint32_t)fd,
                                        (uint32_t)(uintptr_t)buf, 1024, 0, 0);
    if (n > 0) {
        uint32_t off = 0;
        while (off < (uint32_t)n) {
            LinuxDirent64* de = (LinuxDirent64*)(buf + off);
            int nl = 0; while (de->d_name[nl]) nl++;
            LinuxSyscall::Dispatch(LSYS_WRITE, 1,
                (uint32_t)(uintptr_t)de->d_name, nl, 0, 0);
            const char* nl_s = "  ";
            LinuxSyscall::Dispatch(LSYS_WRITE, 1,
                (uint32_t)(uintptr_t)nl_s, 2, 0, 0);
            off += de->d_reclen;
        }
        const char* nl_c = "\n";
        LinuxSyscall::Dispatch(LSYS_WRITE, 1,
            (uint32_t)(uintptr_t)nl_c, 1, 0, 0);
    }
    LinuxSyscall::Dispatch(LSYS_CLOSE, (uint32_t)fd, 0, 0, 0, 0);
}

static void builtin_cat(const char* path) {
    int fd = LinuxSyscall::Dispatch(LSYS_OPEN, (uint32_t)(uintptr_t)path,
                                     L_O_RDONLY, 0, 0, 0);
    if (fd < 0) {
        const char* msg = "cat: No such file\n";
        LinuxSyscall::Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)msg, 18, 0, 0);
        return;
    }
    uint8_t buf[1024];
    int32_t n;
    while ((n = LinuxSyscall::Dispatch(LSYS_READ, (uint32_t)fd,
                (uint32_t)(uintptr_t)buf, 1024, 0, 0)) > 0) {
        LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)buf, n, 0, 0);
    }
    LinuxSyscall::Dispatch(LSYS_CLOSE, (uint32_t)fd, 0, 0, 0, 0);
}

static void builtin_mkdir(const char* path) {
    int32_t r = LinuxSyscall::Dispatch(LSYS_MKDIR, (uint32_t)(uintptr_t)path,
                                        0755, 0, 0, 0);
    if (r < 0) {
        const char* msg = "mkdir: failed\n";
        LinuxSyscall::Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)msg, 14, 0, 0);
    }
}

static void builtin_write_file(const char* path, const char* content) {
    int fd = LinuxSyscall::Dispatch(LSYS_OPEN, (uint32_t)(uintptr_t)path,
                                     L_O_WRONLY | L_O_CREAT | L_O_TRUNC, 0644, 0, 0);
    if (fd < 0) {
        const char* msg = "write: cannot create file\n";
        LinuxSyscall::Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)msg, 26, 0, 0);
        return;
    }
    int len = 0; while (content[len]) len++;
    LinuxSyscall::Dispatch(LSYS_WRITE, (uint32_t)fd,
                           (uint32_t)(uintptr_t)content, len, 0, 0);
    LinuxSyscall::Dispatch(LSYS_CLOSE, (uint32_t)fd, 0, 0, 0, 0);
    const char* ok = "Written OK\n";
    LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)ok, 11, 0, 0);
}

static void builtin_sleep(int seconds) {
    struct { uint32_t tv_sec; uint32_t tv_nsec; } ts;
    ts.tv_sec = (uint32_t)seconds;
    ts.tv_nsec = 0;
    const char* msg = "Sleeping...\n";
    LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)msg, 12, 0, 0);
    LinuxSyscall::Dispatch(LSYS_NANOSLEEP, (uint32_t)(uintptr_t)&ts, 0, 0, 0, 0);
    const char* done = "Done.\n";
    LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)done, 6, 0, 0);
}

static void builtin_stat_file(const char* path) {
    LinuxStat st;
    int32_t r = LinuxSyscall::Dispatch(LSYS_STAT, (uint32_t)(uintptr_t)path,
                                        (uint32_t)(uintptr_t)&st, 0, 0, 0);
    if (r < 0) {
        const char* msg = "stat: not found\n";
        LinuxSyscall::Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)msg, 16, 0, 0);
        return;
    }
    char line[128];
    int p = 0;
    auto sa = [&](const char* s) { while (*s && p < 120) line[p++] = *s++; };
    auto si = [&](uint32_t v) {
        if (v == 0) { line[p++] = '0'; return; }
        char t[12]; int ti = 0;
        while (v > 0) { t[ti++] = '0' + (v % 10); v /= 10; }
        while (ti > 0) line[p++] = t[--ti];
    };
    sa("  Size: "); si(st.st_size);
    sa("  Mode: 0"); si((st.st_mode >> 6) & 7); si((st.st_mode >> 3) & 7); si(st.st_mode & 7);
    sa("  Blocks: "); si(st.st_blocks);
    line[p++] = '\n'; line[p] = 0;
    LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)line, p, 0, 0);
}

int LinuxSyscall::RunProgram(const char* name, int argc, const char** argv,
                              char* output, int max_output) {
    // clear console buffer
    ClearConsoleOutput();

    // create a process
    int idx = CreateProcess(name, 0, 0);
    if (idx < 0) {
        const char* err = "linux-exec: cannot create process\n";
        int len = 0; while (err[len]) len++;
        for (int i = 0; i < len && i < max_output - 1; i++) output[i] = err[i];
        output[len < max_output ? len : max_output - 1] = 0;
        return len;
    }

    int saved = current_proc;
    SetCurrent(idx);

    // dispatch to built-in programs
    if (ls_seq(name, "hello") || ls_seq(name, "/bin/hello")) {
        builtin_hello(nullptr);
    } else if (ls_seq(name, "uname") || ls_seq(name, "/bin/uname")) {
        builtin_uname(nullptr);
    } else if (ls_seq(name, "getpid") || ls_seq(name, "/bin/getpid")) {
        builtin_getpid(nullptr);
    } else if (ls_seq(name, "pwd") || ls_seq(name, "/bin/pwd")) {
        builtin_pwd(nullptr);
    } else if (ls_seq(name, "ls") || ls_seq(name, "/bin/ls")) {
        builtin_ls(nullptr);
    } else if (ls_seq(name, "cat") || ls_seq(name, "/bin/cat")) {
        if (argc > 1) builtin_cat(argv[1]);
        else {
            const char* msg = "cat: missing operand\n";
            Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)msg, 21, 0, 0);
        }
    } else if (ls_seq(name, "mkdir") || ls_seq(name, "/bin/mkdir")) {
        if (argc > 1) builtin_mkdir(argv[1]);
        else {
            const char* msg = "mkdir: missing operand\n";
            Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)msg, 23, 0, 0);
        }
    } else if (ls_seq(name, "stat") || ls_seq(name, "/bin/stat")) {
        if (argc > 1) builtin_stat_file(argv[1]);
        else {
            const char* msg = "stat: missing operand\n";
            Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)msg, 22, 0, 0);
        }
    } else if (ls_seq(name, "sleep") || ls_seq(name, "/bin/sleep")) {
        int secs = 1;
        if (argc > 1) {
            secs = 0;
            const char* s = argv[1];
            while (*s >= '0' && *s <= '9') { secs = secs * 10 + (*s - '0'); s++; }
        }
        builtin_sleep(secs);
    } else if (ls_seq(name, "echo") || ls_seq(name, "/bin/echo")) {
        for (int i = 1; i < argc; i++) {
            int len = 0; while (argv[i][len]) len++;
            Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)argv[i], len, 0, 0);
            if (i < argc - 1) {
                const char* sp = " ";
                Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)sp, 1, 0, 0);
            }
        }
        const char* nl = "\n";
        Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)nl, 1, 0, 0);
    } else if (ls_seq(name, "write") || ls_seq(name, "/bin/write")) {
        if (argc > 2) builtin_write_file(argv[1], argv[2]);
        else {
            const char* msg = "write: usage: write <file> <content>\n";
            Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)msg, 37, 0, 0);
        }
    } else if (ls_seq(name, "id") || ls_seq(name, "/bin/id")) {
        char line[128];
        int p = 0;
        auto sa = [&](const char* s) { while (*s && p < 120) line[p++] = *s++; };
        auto si = [&](int32_t v) {
            if (v == 0) { line[p++] = '0'; return; }
            char t[12]; int ti = 0;
            while (v > 0) { t[ti++] = '0' + (v % 10); v /= 10; }
            while (ti > 0) line[p++] = t[--ti];
        };
        sa("uid="); si(Dispatch(LSYS_GETUID, 0, 0, 0, 0, 0));
        sa("(root) gid="); si(Dispatch(LSYS_GETGID, 0, 0, 0, 0, 0));
        sa("(root)\n");
        line[p] = 0;
        Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)line, p, 0, 0);
    } else {
        // unknown program
        const char* pre = "linux-exec: unknown program: ";
        Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)pre, 29, 0, 0);
        int nl = 0; while (name[nl]) nl++;
        Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)name, nl, 0, 0);
        const char* suf = "\nAvailable: hello uname getpid pwd ls cat mkdir stat sleep echo write id\n";
        int sl = 0; while (suf[sl]) sl++;
        Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)suf, sl, 0, 0);
    }

    // process exits
    Dispatch(LSYS_EXIT, 0, 0, 0, 0, 0);

    // read captured output
    int out_len = ReadConsoleOutput(output, max_output - 1);
    output[out_len] = 0;

    // cleanup
    DestroyProcess(idx);
    SetCurrent(saved);

    return out_len;
}