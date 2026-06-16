//  kurono os  -  linux syscall abi translation layer  -  implementation
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
#include "../shell/shell.h"
#include "../system/logging.h"
#include "../proc/smp.h"          // per-cpu syscall-entry scratch (smp phase 3d) (satoru)

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
// high mmap regions, and anonymous maps may run past 4gb  -  their
// pointers round-trip through the now-64-bit syscall abi intact (satoru)
constexpr uint64_t USER_MMAP_LIMIT = USER_SPACE_TOP;

constexpr uint32_t PFERR_PRESENT = 1U << 0;
constexpr uint32_t PFERR_WRITE   = 1U << 1;
constexpr uint32_t PFERR_USER    = 1U << 2;
constexpr uint32_t PFERR_FETCH   = 1U << 4;

constexpr uint32_t LINUX_PROT_WRITE = 0x2;
constexpr uint32_t LINUX_PROT_EXEC  = 0x4;

// per-cpu syscall-entry scratch (smp phase 3d): each cpu has its own in-flight
// frame pointer + rewrite/resume flags, so two cores can be inside int 0x80 /
// SYSCALL at the same time without clobbering each other's state. the macros
// keep the existing ~50 use sites textually unchanged; the cpu doesn't migrate
// mid-syscall, so re-reading CpuIndex() per access is stable. on the bsp this is
// slot 0  -  identical to the old single-frame behaviour. (satoru)
static InterruptFrame* g_cur_syscall_frame[SMP_MAX_CPUS] = {};
static bool g_cur_frame_rewritten[SMP_MAX_CPUS]  = {};
static bool g_resume_us_session[SMP_MAX_CPUS]    = {};
static int  g_resume_us_exit[SMP_MAX_CPUS]       = {};
#define current_syscall_frame      g_cur_syscall_frame[SMP::CpuIndex()]
#define current_frame_rewritten    g_cur_frame_rewritten[SMP::CpuIndex()]
#define resume_userspace_session   g_resume_us_session[SMP::CpuIndex()]
#define resume_userspace_exit_code g_resume_us_exit[SMP::CpuIndex()]

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
static uint32_t fd_readiness(LinuxProcess* p, int fd, uint32_t interest) {
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) {
        // closed/invalid fd: report error so the loop drops it (satoru)
        return L_EPOLLERR | (interest & (L_EPOLLERR | L_EPOLLHUP));
    }
    LinuxFd* lfd = &p->fds[fd];
    uint32_t ready = 0;
    switch (lfd->type) {
        case LFD_SOCKET:
            if (UnixSocket::PendingBytes(lfd->backend_fd) > 0) ready |= L_EPOLLIN;
            ready |= L_EPOLLOUT;   // our unix sockets never block on write (satoru)
            break;
        case LFD_PIPE:
            // pipes are socketpair-backed: same PendingBytes path (satoru)
            if (UnixSocket::PendingBytes(lfd->backend_fd) > 0) ready |= L_EPOLLIN;
            ready |= L_EPOLLOUT;
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
    // EPOLLHUP/EPOLLERR are always reported regardless of interest; otherwise
    // mask to what the caller asked for (satoru)
    return ready & (interest | L_EPOLLERR | L_EPOLLHUP);
}

// fwd decl: defined further down. poll's cooperative block uses it to hand the
// cpu to a sibling user thread. (satoru)
static bool switch_to_ready_user(InterruptFrame* frame);

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
    task->state           = Process_Blocked;
    task->sleep_ticks     = 2;                          // re-ready in ~2 ticks, then re-scan (satoru)
    if (!switch_to_ready_user(current_syscall_frame)) {
        task->sleep_ticks     = 0;                      // no sibling: undo + block in-place (satoru)
        task->state           = Process_Running;
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
        // single user thread (no sibling): cooperative in-place wait, then re-scan (satoru)
        KuronoShell::PumpUI();
        Scheduler::SleepMs(1);
    }
}

static void clone_file_descriptors(const LinuxProcess* parent, LinuxProcess* child) {
    for (int fd = 0; fd < LINUX_MAX_FDS; fd++) {
        child->fds[fd].open = false;
        if (!parent->fds[fd].open) continue;

        const LinuxFd* src = &parent->fds[fd];
        LinuxFd* dst = &child->fds[fd];
        memcpy(dst, src, sizeof(LinuxFd));

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

static UserMemoryRegion* find_region(Process* proc, uint64_t addr) {
    if (!proc) return nullptr;

    for (int i = 0; i < PROCESS_MAX_USER_REGIONS; i++) {
        UserMemoryRegion* region = &proc->regions[i];
        if (!region->active) continue;
        if (addr >= region->start && addr < region->end) return region;
    }
    return nullptr;
}

static UserMemoryRegion* find_region_by_flag(Process* proc, uint32_t flag) {
    if (!proc) return nullptr;

    for (int i = 0; i < PROCESS_MAX_USER_REGIONS; i++) {
        UserMemoryRegion* region = &proc->regions[i];
        if (region->active && (region->flags & flag)) return region;
    }
    return nullptr;
}

static UserMemoryRegion* find_free_region_slot(Process* proc) {
    if (!proc) return nullptr;

    for (int i = 0; i < PROCESS_MAX_USER_REGIONS; i++) {
        if (!proc->regions[i].active) return &proc->regions[i];
    }
    return nullptr;
}

static bool region_overlaps(Process* proc, uint64_t start, uint64_t end) {
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
    if (!slot) return nullptr;

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

static void unmap_user_range(Process* proc, uint64_t start, uint64_t end) {
    if (!proc || start >= end) return;

    uint64_t page_start = align_down_u64(start, PAGE_SIZE);
    uint64_t page_end = align_up_u64(end, PAGE_SIZE);
    for (uint64_t page = page_start; page < page_end; page += PAGE_SIZE) {
        if (!KernelVMM::QueryMappingInAddressSpace(proc->address_space, page)) continue;
        KernelVMM::UnmapPageInAddressSpace(proc->address_space, page, true);
        if (Scheduler::GetCurrentProcess() == proc) {
            KernelVMM::InvalidatePage(page);
        }
    }
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

static uint64_t choose_mmap_base(Process* task, uint64_t requested, uint64_t length) {
    uint64_t base = requested ? align_down_u64(requested, PAGE_SIZE)
                              : align_up_u64(task->next_mmap_base, PAGE_SIZE);

    while (base + length <= USER_MMAP_LIMIT) {
        if (!region_overlaps(task, base, base + length)) return base;
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
static LinuxShmObj g_linux_shm[64];

static int shm_alloc_slot() {
    for (int i = 0; i < 64; i++) {
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
    if (idx < 0 || idx >= 64 || !g_linux_shm[idx].used) return nullptr;
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
    if (s->base) return s->size >= size;        // already sized; never shrink
    uint64_t rounded = align_up_u64(size, PAGE_SIZE);
    void* mem = PMM::AllocBytes((size_t)rounded);
    if (!mem) return false;
    memset(mem, 0, (size_t)rounded);
    s->base = (uint8_t*)mem;
    s->size = rounded;
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
    Scheduler::SaveUserFrame(task, frame);
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
        PMM::FreeFrame(phys);
    } else {
        if (!KernelVMM::MapPageInAddressSpace(task->address_space, page_base, phys, new_flags)) {
            return false;
        }
    }

    if (Scheduler::GetCurrentProcess() == task) {
        KernelVMM::InvalidatePage(page_base);
    }
    Scheduler::SaveUserFrame(task, frame);
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

    // ~4 ms timeslice so the fxsave/fxrstor + cr3 switch cost stays modest. (satoru)
    if ((++g_preempt_ticks & 3u) != 0) return;

    Scheduler::SaveUserFrame(cur, frame);            // capture live regs+fs+fpu
    if (!Scheduler::ScheduleNextUser(frame)) return; // nothing else ready

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
    if (parent_task->state == Process_Blocked) {
        parent_task->state = Process_Ready;
    }

    Scheduler::ReapProcess(child->task);
    child->task = nullptr;
    LinuxSyscall::DestroyProcess(child_index);
}

// ── real futex wait-queue ────────────────────────────────────────────────
// a waiter is keyed by (address_space, uaddr): two distinct processes can map
// the same virtual address yet must not cross-wake, and threads in one process
// share an address space so they DO match. (satoru)
constexpr int FUTEX_MAX_WAITERS = 64;
struct FutexWaiter {
    Process*  task;
    uint64_t  addr_space;
    uintptr_t uaddr;
    uint64_t  phys_key;   // physical page|offset backing uaddr, so a SHARED futex
                          // can cross-wake between processes with different address
                          // spaces / virtual addresses (e10s ipc). (satoru)
    uint32_t  bitset;
    bool      active;
};
static FutexWaiter g_futex_waiters[FUTEX_MAX_WAITERS];

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
                                    uint32_t bitset) {
    if (!task || !current_syscall_frame) return false;

    int slot = -1;
    for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
        if (!g_futex_waiters[i].active) { slot = i; break; }
    }
    if (slot < 0) return false;  // queue full  -  caller returns -EAGAIN (satoru)

    g_futex_waiters[slot].task       = task;
    g_futex_waiters[slot].addr_space = task->address_space;
    g_futex_waiters[slot].uaddr      = uaddr;
    g_futex_waiters[slot].phys_key   = futex_phys_key(task, uaddr);
    g_futex_waiters[slot].bitset     = bitset ? bitset : 0xFFFFFFFFu;
    g_futex_waiters[slot].active     = true;

    task->state = Process_Blocked;
    task->user_frame.rax = 0;

    if (!switch_to_ready_user(current_syscall_frame)) {
        // nothing else runnable  -  undo the block so we don't wedge the only
        // user task off the run queue with no one left to wake it. (satoru)
        g_futex_waiters[slot].active = false;
        task->state = Process_Running;
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
            w->task->user_frame.rax = 0;   // futex() returns 0 to the waiter
            if (w->task->state == Process_Blocked) {
                w->task->state = Process_Ready;
            }
        }
        w->active = false;
        w->task   = nullptr;
        woken++;
    }
    return woken;
}
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
    }

    // int 0x80 enters through an interrupt gate, so IF is cleared on entry.
    // Re-enable interrupts once the kernel stack/frame are in place so timer
    // ticks continue to advance during longer syscall handlers.
    HAL::EnableInterrupts();
    KuronoShell::PumpUI();

    // the syscall number is small, but the arg registers carry full
    // 64-bit user pointers/addresses  -  pass them through untruncated so a
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

    if (resume_userspace_session) {
        Userspace::HandleProcessExit(resume_userspace_exit_code);
    }

    HAL::DisableInterrupts();
}

// the x86_64 syscall core lives in linux_syscall_x64.cpp (translates the amd64
// nr and dispatches). we call it from the frame handler below  -  defined here in
// the same TU as current_syscall_frame and the switch helpers so the SYSCALL
// fast path can block/switch/exit threads exactly like int 0x80. (satoru)
extern "C" int64_t SyscallEntryX64Handler(uint64_t nr, uint64_t a0, uint64_t a1,
                                          uint64_t a2, uint64_t a3, uint64_t a4,
                                          uint64_t a5);

// x86_64 SYSCALL fast-path frame handler. the asm stub (src/hal/syscall_entry.asm)
// builds a full InterruptFrame from the live registers and calls here; on return
// it restores the (possibly rewritten) frame and IRETQs. this mirrors
// LinuxInt80Entry field-for-field: save the caller's frame so clone snapshots a
// fresh parent, dispatch, then either write the result back or  -  if a handler
// rewrote the frame (futex block / thread exit / clone)  -  leave it for the stub
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
    }

    // amd64 syscall abi: nr in rax, args in rdi, rsi, rdx, r10, r8, r9. the
    // frame captured r9 pristine (musl's clone child-fn), so clone no longer
    // needs the g_user_syscall_*_save shim. (satoru)
    int64_t result = SyscallEntryX64Handler(
        frame->rax,
        frame->rdi, frame->rsi, frame->rdx,
        frame->r10, frame->r8,  frame->r9);

    if (!current_frame_rewritten) {
        frame->rax = (uint64_t)(int64_t)result;
        Process* current = Scheduler::GetCurrentProcess();
        if (Userspace::IsActive() && current && current->is_user()) {
            Scheduler::SaveUserFrame(current, frame);
        }
    }

    current_syscall_frame = nullptr;

    if (resume_userspace_session) {
        Userspace::HandleProcessExit(resume_userspace_exit_code);
    }

    HAL::DisableInterrupts();
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
    if (!frame || !(frame->error_code & PFERR_USER)) return false;

    Process* task = Scheduler::GetCurrentProcess();
    if (!task || !task->is_user()) return false;

    uint64_t page_base = align_down_u64(frame->cr2, PAGE_SIZE);
    uint64_t page_flags = KernelVMM::QueryPageFlagsInAddressSpace(task->address_space, page_base);

    if (frame->error_code & PFERR_PRESENT) {
        // Standard COW path first.
        if (handle_cow_fault(task, page_base, page_flags, frame)) return true;

        // Otherwise: a user-mode access hit a present mapping that
        // didn't have PTE_USER set (typically the kernel identity map
        // covering the brk/mmap window in low physical memory).  If we
        // have a registered user region covering this address, promote
        // by unmapping the supervisor PTE and falling through to the
        // demand-zero allocator below.
        UserMemoryRegion* r = find_region(task, frame->cr2);
        if (!r) return false;
        if (!(page_flags & PTE_USER)) {
            KernelVMM::UnmapPageInAddressSpace(task->address_space, page_base, false);
            KernelVMM::InvalidatePage(page_base);
            return handle_demand_zero_fault(task, r, page_base, frame);
        }
        return false;
    }

    UserMemoryRegion* region = find_region(task, frame->cr2);
    if (!region) return false;

    return handle_demand_zero_fault(task, region, page_base, frame);
}

//  process management

int LinuxSyscall::CreateProcess(const char* name, uint32_t uid, uint32_t gid) {
    for (int i = 0; i < LINUX_MAX_PROCS; i++) {
        if (!procs[i].active) {
            LinuxProcess* p = &procs[i];
            memset(p, 0, sizeof(LinuxProcess));
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

// registered once at boot; turns on round-robin preemption of user threads. the
// static kls_timer_preempt handler lives earlier in this TU (internal linkage,
// still visible here). (satoru)
void LinuxSyscall::EnableTimerPreemption() {
    HAL::RegisterIRQHandler(0, kls_timer_preempt);
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
// translation is invisible to the caller  -  they pass the linux path they
// expect, kurono serves from the kurono path.

void LinuxSyscall::ResolvePath(const char* linux_path, char* kurono_path,
                                int max_len, LinuxProcess* p) {
    char abs[256];

    if (linux_path[0] != '/') {
        // relative path  -  prepend cwd. guard against an empty cwd (ls_slen==0
        // would index abs[-1]). (satoru)
        ls_scpy(abs, p->cwd, sizeof(abs));
        int alen = ls_slen(abs);
        if (alen == 0 || abs[alen - 1] != '/') ls_cat(abs, "/", sizeof(abs));
        ls_cat(abs, linux_path, sizeof(abs));
    } else {
        ls_scpy(abs, linux_path, sizeof(abs));
    }

    // table of (linux_prefix, kurono_prefix) substitutions.
    // ordering matters  -  longer prefixes must come first so /usr/lib64
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

    // Pass-through prefixes  -  these are already kurono-native.
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

    KuronoShell::PumpUI();

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
        case LSYS_IOCTL:       return sys_ioctl((int)ebx, ecx, edx);
        case LSYS_WRITEV:      return sys_writev((int)ebx, ecx, edx);
        case LSYS_MMAP:        return sys_mmap(ebx, ecx, edx, esi, (int)edi, 0);
        case LSYS_MUNMAP:      return sys_munmap(ebx, ecx);
        case LSYS_NANOSLEEP:   return sys_nanosleep(ebx, ecx);
        case LSYS_GETDENTS64:  return sys_getdents64((int)ebx, ecx, edx);
        case LSYS_CLOCK_GETTIME: return sys_clock_gettime(ebx, ecx);
        case LSYS_SET_THREAD_AREA: return sys_set_thread_area(ebx);
        case LSYS_EXIT_GROUP:  return sys_exit_group(ebx);

        // mprotect: real  -  flip page-table perms so w^x jits (rw->rx) work (satoru)
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
            // tgkill(tgid, tid, sig)  -  we map to plain kill(tid, sig)
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
            if (eax == LSYS_GETRLIMIT) rl = (uint32_t*)(uintptr_t)ecx;
            else if (eax == LSYS_PRLIMIT64) rl = (uint32_t*)(uintptr_t)esi;
            if (rl){
                // rlim_cur, rlim_max as 64-bit
                rl[0] = 0xFFFFFFFFu; rl[1] = 0x7FFFFFFFu;
                rl[2] = 0xFFFFFFFFu; rl[3] = 0x7FFFFFFFu;
            }
            return 0;
        }
        case LSYS_PERSONALITY:
            return 0;  // PER_LINUX
        case LSYS_CAPGET:
        case LSYS_CAPSET:
            return 0;  // we run as root-equivalent
        case LSYS_FTRUNCATE: {
            // size a memfd's backing; non-memfd fds grow on write (kvfs). (satoru)
            LinuxProcess* p = Current();
            LinuxShmObj* s = shm_for_fd(p, (int)ebx);
            if (s) return shm_set_size(s, (uint64_t)ecx) ? 0 : -28;  // -ENOSPC
            return 0;
        }
        case LSYS_FSYNC:
        case LSYS_FDATASYNC:
        case LSYS_MADVISE:
        case LSYS_MSYNC:
            return 0;
        case LSYS_DUP3:
            // dup3(oldfd, newfd, flags)  -  flags ignored, behaves like dup2
            return sys_dup2((int)ebx, (int)ecx);
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
            // pread64(fd, buf, count, offset)  -  emulate by lseek+read
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
            if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;
            LinuxFd* lfd = &p->fds[fd];
            switch (cmd) {
                case 0:  // F_DUPFD
                case 1030: { // F_DUPFD_CLOEXEC
                    for (int i = (int)arg; i < LINUX_MAX_FDS; i++) {
                        if (!p->fds[i].open) {
                            memcpy(&p->fds[i], lfd, sizeof(LinuxFd));
                            return i;
                        }
                    }
                    return -24;
                }
                case 1:  return 0;             // F_GETFD
                case 2:  return 0;             // F_SETFD (CLOEXEC ignored)
                case 3:  return (int32_t)lfd->flags;       // F_GETFL
                case 4:  lfd->flags = arg; return 0;        // F_SETFL
                case 5:  case 6:  case 7:                  // F_GETLK/SETLK/SETLKW
                    return 0;                              // no advisory locks
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
            uint32_t op    = ecx & ~(FUTEX_PRIVATE | FUTEX_CLOCK_RT);
            uint64_t uaddr = ebx;
            uint32_t val   = (uint32_t)edx;

            Process* task = Scheduler::GetCurrentProcess();
            if (!task || !task->is_user()) return -22;  // -EINVAL

            switch (op) {
                case 0:   /* FUTEX_WAIT */
                case 9: { /* FUTEX_WAIT_BITSET (match-any) */
                    uint32_t cur = 0;
                    if (!read_user_u32(task, uaddr, &cur)) return -14;  // -EFAULT
                    if (cur != val) return -11;  // -EAGAIN  -  value already changed

                    // preferred path: enqueue + deschedule by rewriting the trap
                    // frame (works exactly like sys_waitpid). both the int 0x80 and
                    // the x86_64 SYSCALL paths set current_syscall_frame now, so on
                    // success the frame is rewritten and our return value is
                    // ignored. (satoru)
                    if (futex_enqueue_and_block(task, (uintptr_t)uaddr, 0xFFFFFFFFu)) {
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
                default:
                    // FUTEX_REQUEUE/CMP_REQUEUE/WAKE_OP/PI variants: accept and
                    // no-op so callers don't see -ENOSYS mid-lock. (satoru)
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
            const uint32_t F_THREAD        = 0x00010000;
            const uint32_t F_SETTLS        = 0x00080000;
            const uint32_t F_PARENT_SETTID = 0x00100000;
            const uint32_t F_CHILD_CLEARTID= 0x00200000;
            const uint32_t F_CHILD_SETTID  = 0x01000000;

            // not a thread (no shared VM) → keep the existing fork-like path. (satoru)
            if (!(flags & (F_THREAD | F_VM)) || !child_stack) {
                LinuxProcess* p = Current();
                int idx = CreateProcess(p ? p->name : "child", p ? p->uid : 0, p ? p->gid : 0);
                if (idx < 0) return -11;
                return (int32_t)procs[idx].pid;
            }

            LinuxProcess* parent       = Current();
            Process*      parent_task  = Scheduler::GetCurrentProcess();
            if (!parent || !parent_task || !parent_task->is_user()) return -22;

            // spawn the schedulable thread task that shares parent_task's cr3. (satoru)
            Process* thread_task = Scheduler::CreateUserThread(
                parent_task, child_stack, tls, (flags & F_SETTLS) != 0);
            if (!thread_task) return -11;  // -EAGAIN (out of task slots)

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
            clone_file_descriptors(parent, tproc);

            int32_t tid = (int32_t)thread_task->pid;

            // CLONE_PARENT_SETTID / CHILD_SETTID: publish the tid through the
            // shared address space (visible to parent and child). (satoru)
            if (flags & F_PARENT_SETTID) write_user_u32(parent_task, ptid, (uint32_t)tid);
            if (flags & F_CHILD_SETTID)  write_user_u32(parent_task, ctid, (uint32_t)tid);

            // CLONE_CHILD_CLEARTID: remember ctid so thread exit zeroes it and
            // futex-wakes any joiner (pthread_join waits on exactly this). (satoru)
            if (flags & F_CHILD_CLEARTID) thread_task->clear_child_tid = ctid;

            return tid;  // parent sees the child tid; the child returns 0 (frame)
        }

        // Posix realtime signal stubs  -  accept both i386 numbering (174..)
        // and x86_64 numbering via the LSYS_RT_SIG* synonyms.  No delivery.
        case 174:  // i386 rt_sigaction
        case 175:  // i386 rt_sigprocmask
        case 173:  // i386 rt_sigreturn
        case LSYS_RT_SIGACTION:
        case LSYS_RT_SIGPROCMASK:
        case LSYS_RT_SIGSUSPEND:
        case LSYS_RT_SIGPENDING:
        case LSYS_RT_SIGRETURN:
            return 0;

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
                KuronoShell::PumpUI();
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
            return fd;
        }
        // timerfd_create: ebx = clockid (ignored  -  single monotonic base),
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
        // signalfd4: harmless stub  -  we have no real signal delivery, so the
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
        // inotify_init1  -  a real but inert fd (no events fire). memfd is handled
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
                return -28;   // enospc  -  watch table full (satoru)
            }
            return -22;
        }
        // epoll_wait(epfd, events, maxevents, timeout): scan the watch table,
        // compute readiness per fd, fill the user epoll_event array, return the
        // count. if nothing ready and timeout != 0, do a bounded cooperative
        // spin (PumpUI) re-scanning each pass, then return what we have (which
        // may be 0). this is NOT a true blocking wait  -  it spins a fixed number
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
            if (slot < 0 || slot >= EPOLL_MAX || !g_epoll[slot].used) return -22;
            EpollState* es = &g_epoll[slot];
            // bound the spin: 0 => single pass; otherwise a fixed budget so an
            // infinite (-1) timeout still returns to the cooperative loop. (satoru)
            int spins = (timeout == 0) ? 0 : 128;
            int n = 0;
            for (int s = 0; ; s++) {
                n = 0;
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
                if (n > 0 || s >= spins) break;
                KuronoShell::PumpUI();   // let kernel servers push socket data (satoru)
            }
            return n;
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

        // sendfile/splice/tee  -  fall back to bounded read/write loop.
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
                KuronoShell::PumpUI();
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

        case LSYS_FALLOCATE:
            return 0;
        case LSYS_RENAMEAT2:
            return 0;

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
        case LSYS_SETNS:        return 0;  // attach to ns by fd  -  accept
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
        case LSYS_KEYCTL:
        case LSYS_KCMP:
        case LSYS_USERFAULTFD:
        case LSYS_FANOTIFY_INIT:
        case LSYS_FANOTIFY_MARK:
        case LSYS_NAME_TO_HANDLE_AT:
        case LSYS_OPEN_BY_HANDLE_AT:
            return 0;

        // ===== Firefox-required modern syscalls =====================

        // statx(dirfd, pathname, flags, mask, statxbuf)  -  full impl.
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
                KuronoShell::PumpUI();
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

        // close_range(first, last, flags)  -  close every fd in [first,last].
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

        // clone3(args, size)  -  Firefox uses to spawn content processes.
        // We implement enough to behave like fork() for the common case
        // (CLONE_VM not requested).  Returns child pid in parent, 0 in
        // child like classic fork.
        case LSYS_CLONE3: {
            // For now route to plain fork.
            return sys_fork();
        }

        // pidfd_getfd(pidfd, target_fd, flags)  -  duplicate target_fd
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

        // landlock_create_ruleset / add_rule / restrict_self  -  sandbox
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

        // io_uring_setup(entries, params)  -  ring file descriptor.
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

        // seccomp(operation, flags, args)  -  accept BPF programs without
        // installing them so Firefox sandbox init doesn't crash.
        case LSYS_SECCOMP:
            return 0;

        // process_vm_readv(pid, local_iov, liovcnt, remote_iov, riovcnt, flags)
        // process_vm_writev  -  same shape.  Same-process IPC; in this
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
            if (!sa || !lp || fd < 0 || fd >= LINUX_MAX_FDS ||
                lp->fds[fd].type != LFD_SOCKET) return -9;
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
            if (!sa || fd < 0 || fd >= LINUX_MAX_FDS ||
                lp->fds[fd].type != LFD_SOCKET) return -9;
            const char* path = (const char*)(sa + 2);
            return UnixSocket::Connect(lp->fds[fd].backend_fd, path);
        }
        case LSYS_SENDTO: {
            int fd = (int)ebx;
            const void* buf = (const void*)(uintptr_t)ecx;
            int len = (int)edx;
            LinuxProcess* lp = Current();
            if (fd < 0 || fd >= LINUX_MAX_FDS ||
                lp->fds[fd].type != LFD_SOCKET) return -9;
            return UnixSocket::Send(lp->fds[fd].backend_fd, buf, len, 0);
        }
        case LSYS_SENDMSG: {
            // sendmsg(fd, const struct msghdr*, flags): gather the iov payload
            // and parse SCM_RIGHTS, so a passed memfd (e.g. a wl_shm pool fd)
            // reaches the in-kernel server resolved to its shm backing. (satoru)
            int fd = (int)ebx;
            const uint8_t* m = (const uint8_t*)(uintptr_t)ecx;   // struct msghdr*
            LinuxProcess* lp = Current();
            if (fd < 0 || fd >= LINUX_MAX_FDS ||
                lp->fds[fd].type != LFD_SOCKET) return -9;
            if (!m) return -14;
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
            if (fd < 0 || fd >= LINUX_MAX_FDS ||
                lp->fds[fd].type != LFD_SOCKET) return -9;
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
            if (fd < 0 || fd >= LINUX_MAX_FDS ||
                lp->fds[fd].type != LFD_SOCKET) return -9;
            if (!m) return -14;
            uint8_t* iov     = *(uint8_t* const*)(m + 16);      // msg_iov
            uint64_t iovlen  = *(const uint64_t*)(m + 24);      // msg_iovlen
            uint8_t* ctl     = *(uint8_t* const*)(m + 32);      // msg_control
            uint64_t ctllen  = *(const uint64_t*)(m + 40);      // msg_controllen

            // total room the caller offered across all iov segments  -  bound to
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
                        if (cm.passed_shm_base[k]) {
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
            if (fd < 0 || fd >= LINUX_MAX_FDS ||
                lp->fds[fd].type != LFD_SOCKET) return -9;
            return UnixSocket::Shutdown(lp->fds[fd].backend_fd, (int)ecx);
        }
        case LSYS_SETSOCKOPT: {
            // setsockopt(fd, level, optname, optval, optlen). we don't model
            // most options on the AF_UNIX backend, so accept the common ones
            // and return success rather than failing the caller. silently
            // ignoring an unknown option is also fine here  -  programs treat a
            // 0 return as "applied". (satoru)
            int fd = (int)ebx;
            LinuxProcess* lp = Current();
            if (fd < 0 || fd >= LINUX_MAX_FDS ||
                lp->fds[fd].type != LFD_SOCKET) return -9;   // -EBADF
            // SOL_SOCKET(1): SO_REUSEADDR(2)/SO_SNDBUF(7)/SO_RCVBUF(8)/
            //   SO_KEEPALIVE(9)/SO_REUSEPORT(15); IPPROTO_TCP(6): TCP_NODELAY(1).
            // all accepted-and-ignored. (satoru)
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
                lp->fds[fd].type != LFD_SOCKET) return -9;   // -EBADF
            if (!optval || !optlen) return -14;              // -EFAULT
            if (*optlen < sizeof(int)) return -22;           // -EINVAL
            int val = 0;
            if (level == 1) {                 // SOL_SOCKET
                switch (optname) {
                    case 4:  val = 0;     break;  // SO_ERROR  -> no error
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
            if (!sa || fd < 0 || fd >= LINUX_MAX_FDS ||
                lp->fds[fd].type != LFD_SOCKET) return -9;
            sa[0] = 1; sa[1] = 0;        // AF_UNIX
            return UnixSocket::GetSockName(lp->fds[fd].backend_fd,
                                           (char*)(sa + 2), 108);
        }
        case LSYS_GETPEERNAME: {
            int fd = (int)ebx;
            uint8_t* sa = (uint8_t*)(uintptr_t)ecx;
            LinuxProcess* lp = Current();
            if (!sa || fd < 0 || fd >= LINUX_MAX_FDS ||
                lp->fds[fd].type != LFD_SOCKET) return -9;
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

        // Linux x86_64 number synonyms  -  Firefox's static glibc emits
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
            SerialLogger::Log("[LinuxSyscall] Unhandled syscall: ");
            SerialLogger::LogDec((int)eax);
            SerialLogger::Log("\r\n");
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

            Scheduler::MarkProcessExited(p->task, (int)code);
            wake_waiting_parent(p, current_index);

            if (Userspace::IsActive() && current_syscall_frame) {
                if (!switch_to_ready_user(current_syscall_frame)) {
                    current_frame_rewritten = true;
                    resume_userspace_session = true;
                    resume_userspace_exit_code = (int)code;
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
    parent_task->state = Process_Blocked;

    if (!switch_to_ready_user(current_syscall_frame)) {
        parent_task->state = Process_Running;
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

    uint64_t entry = 0, rsp = 0;
    bool ok = LdKurono::ExecPIE(task, image, (uint64_t)fsz, resolved, av, ev,
                                proc->uid, proc->gid, &entry, &rsp);
    KernelHeap::Free(image); KernelHeap::Free(abuf); KernelHeap::Free(ebuf);
    KernelHeap::Free((void*)av); KernelHeap::Free((void*)ev);
    if (!ok) {
        SerialLogger::Log("execve: ld-kurono failed to load dynamic image\r\n");
        task->address_space = old_as;                 // keep the caller's AS live
        KernelVMM::DestroyAddressSpace(new_as);
        return -8;
    }

    KernelVMM::ActivateAddressSpace(new_as);
    HAL::SetKernelStack(task->kernel_stack_top);
    KernelVMM::DestroyAddressSpace(old_as);

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
            // stdin  -  read from injection buffer (non-blocking)
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

        // eventfd read: returns the 8-byte counter and zeroes it (or returns 1
        // and decrements in semaphore mode). EAGAIN when the counter is 0  -  the
        // event loop epoll_waits for EPOLLIN before reading. (satoru)
        case LFD_EVENTFD: {
            int s = lfd->backend_fd;
            if (s < 0 || s >= EVENTFD_MAX || count < 8) return -22;  // einval
            uint64_t v = g_eventfd[s].counter;
            if (v == 0) return -11;   // eagain  -  nothing to read (satoru)
            uint64_t ret;
            if (g_eventfd[s].semaphore) { ret = 1; g_eventfd[s].counter = v - 1; }
            else                        { ret = v; g_eventfd[s].counter = 0; }
            *(uint64_t*)dst = ret;
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
        case LFD_SIGNALFD:   // signalfd never delivers  -  reads as empty (satoru)
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
                // single anonymous heap region
                char hex[24];
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
                    int r = KVFS::Read(kfd, dst, (uint32_t)count);
                    KVFS::Close(kfd);
                    return r > 0 ? r : 0;
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

int32_t LinuxSyscall::sys_write(int fd, uintptr_t buf, uint64_t count) {
    LinuxProcess* p = Current();
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;

    LinuxFd* lfd = &p->fds[fd];
    const uint8_t* src = (const uint8_t*)buf;

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
            int r = KVFS::Write(lfd->backend_fd, src, (uint32_t)count);
            if (r > 0) lfd->offset += r;
            return r;
        }

        case LFD_EXT4: {
            int r = Ext4::Write(lfd->backend_fd, src, (uint32_t)count);
            if (r > 0) lfd->offset += r;
            return r;
        }

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
    int32_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        // iov_base is a 64-bit user pointer, iov_len a 64-bit length (satoru)
        int32_t r = sys_write(fd, (uintptr_t)vecs[i].iov_base, vecs[i].iov_len);
        if (r < 0) return r;
        total += r;
    }
    return total;
}

// readv  -  the read counterpart of writev: fill each iovec from fd in order.
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
    if (lfd->type == LFD_KVFS) KVFS::Close(lfd->backend_fd);
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
    // the region table  -  e.g. musl fstat()s into a stack buffer while loading a
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
    return -1;
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
    return newfd;
}

int32_t LinuxSyscall::sys_ioctl(int fd, uint32_t cmd, uint32_t arg) {
    (void)fd; (void)cmd; (void)arg;
    // terminal ioctls  -  return enotty for non-ttys
    LinuxProcess* p = Current();
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;
    if (p->fds[fd].type == LFD_CONSOLE) return 0;  // pretend success
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
        // kernel-heap address is 64-bit under mcmodel=large  -  return it
        // through the widened result so it is not truncated (satoru)
        return (int64_t)(uintptr_t)mem;
    }

    if (fd >= 0) {
        // (a) memfd / shared-memory objects: map the object's physical pages
        // straight into the caller so writes are shared (wl_shm pixel pools,
        // posix shm). (satoru)
        LinuxShmObj* shm = shm_for_fd(proc, fd);
        if (shm && shm->base) {
            uint64_t msize = align_up_u64(length, PAGE_SIZE);
            if (offset + msize > shm->size) return -22;
            uint64_t vbase = choose_mmap_base(task, addr, msize);
            if (!vbase) return -12;
            uint64_t pflags = page_flags_from_prot(prot);
            uint64_t phys0 = (uint64_t)(uintptr_t)shm->base + offset;
            for (uint64_t o = 0; o < msize; o += PAGE_SIZE) {
                if (!KernelVMM::MapPageInAddressSpace(task->address_space,
                                                      vbase + o, phys0 + o, pflags)) {
                    return -12;
                }
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
        // every .so segment  -  mmap(fd, MAP_PRIVATE[, MAP_FIXED], offset)  -  so
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
            uint64_t pflags = page_flags_from_prot(prot);
            bool active = (Scheduler::GetCurrentProcess() == task);
            for (uint64_t o = 0; o < msize; o += PAGE_SIZE) {
                uint64_t va = vbase + o;
                if (fixed) {
                    uint64_t old = KernelVMM::QueryMappingInAddressSpace(task->address_space, va);
                    if (old) PMM::FreeBytes((void*)(uintptr_t)old, PAGE_SIZE);
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
    // returned that  -  musl saw addr != requested and failed the load with EINVAL
    // ("Error loading shared library ...: Invalid argument"): the
    // libgraphite2/harfbuzz blocker. honor the fixed address by REMAPPING the
    // pages in place  -  exactly like the file-backed FIXED branch above: free the
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
                if (old) PMM::FreeBytes((void*)(uintptr_t)old, PAGE_SIZE);
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
            return (int64_t)fbase;
        }
    }

    uint64_t base = choose_mmap_base(task, addr, size);
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

// mprotect(addr, len, prot): change the protection of an existing user mapping.
// this is what makes w^x jits work  -  e.g. spidermonkey mmaps a code buffer rw,
// writes machine code into it, then mprotect()s it rx and jumps in. a no-op stub
// leaves the pages writable+nx, so the cpu either faults on the instruction
// fetch (nx) or runs with the wrong perms. here we (1) re-derive the pte flags
// from prot, (2) update every overlapping user region's page_flags (splitting
// regions on partial coverage so future demand-zero faults pick up the new prot)
// and (3) rewrite the live pte of any already-faulted-in page in place + flush
// the tlb. nx is handled by page_flags_from_prot: PROT_EXEC clears PTE_NX,
// PROT_WRITE without PROT_EXEC sets it, and efer.nxe enforces it. (satoru)
int32_t LinuxSyscall::sys_mprotect(uintptr_t addr, uint64_t length, uint32_t prot) {
    if (length == 0) return 0;

    LinuxProcess* proc = Current();
    Process* task = proc ? proc->task : nullptr;
    if (!task || !task->is_user()) {
        // no per-process address space to reprotect; nothing to do (satoru)
        return 0;
    }

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
            // whole region covered  -  just restamp its protection (satoru)
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
    // not yet present are skipped  -  they will fault in later with the region
    // flags updated above. collect whether anything changed so we can flush. (satoru)
    bool active_cr3 = (Scheduler::GetCurrentProcess() == task);
    bool mapped_any = false;
    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        if (KernelVMM::ProtectPageInAddressSpace(task->address_space, page, new_flags)) {
            mapped_any = true;
            if (active_cr3) KernelVMM::InvalidatePage(page);
        }
    }

    // linux returns 0 even when the range has no backing region yet (a fresh
    // demand-zero mmap region still counts). it ALSO succeeds for a range that
    // is live in the page tables but not in our region array  -  e.g. elf segments
    // mapped directly by ld-kurono, which musl then mprotect()s read-only for
    // RELRO. (returning enomem there made musl treat RELRO as a fatal error and
    // abort with exit 127, even though step 2 above already applied the new
    // protection.) only reject when the range overlaps NO region AND maps no
    // live page at all -> genuinely unmapped -> enomem per posix. (satoru)
    if (!covered_any && !mapped_any) return -12;
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
                KuronoShell::PumpUI();
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
    // uses this for performance timing  -  drift/discontinuity shows as
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
    // tls setup  -  simplified: pretend success
    return 0;
}

//  console output capture  -  read syscall output back into the shell

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

//  stdin injection  -  push data from shell into linux process stdin

void LinuxSyscall::InjectStdin(const char* data, int len) {
    for (int i = 0; i < len; i++) {
        int next = (stdin_head + 1) % STDIN_BUF_SIZE;
        if (next == stdin_tail) break; // buffer full
        stdin_buf[stdin_head] = data[i];
        stdin_head = next;
    }
}

//  runprogram  -  execute a simulated linux program via syscalls
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