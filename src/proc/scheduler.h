#pragma once
#include "../kernel/types.h"
#include "../hal/hal.h"

// process scheduler
// manages execution of tasks, priority queues, and context switching.

enum ProcessState {
    Process_Ready,
    Process_Running,
    Process_Blocked,
    Process_Sleeping,
    Process_Terminated
};

// Priority tiers for the preemptive kernel-process scheduler.  The
// existing numeric `priority` field (0=high..255=low) maps onto these
// tiers via the helper Process::TierFromPriority(); the timeslice budget
// and round-robin behaviour are picked from the tier.
enum ProcessPriorityTier {
    PRIO_REALTIME = 0,   //  1ms slice
    PRIO_HIGH     = 1,   //  3ms slice
    PRIO_NORMAL   = 2,   // 10ms slice
    PRIO_LOW      = 3,   // 25ms slice
    PRIO_TIER_COUNT
};

// Timeslice (milliseconds) assigned to a tier.  Centralised so the IRQ0
// preemption hook and the snapshot/Task Manager view stay in sync.
constexpr uint32_t PROCESS_TIMESLICE_MS[PRIO_TIER_COUNT] = {
    1,   // REALTIME
    3,   // HIGH
    10,  // NORMAL
    25,  // LOW
};

// Adaptive kernel-stack metadata.  Each kernel process owns a region of
// physical memory that grows on demand: a guard page sits just below
// `low` (non-present) and is moved further down as the page-fault
// handler allocates additional pages.  `cap` is the hard ceiling.
struct KernelStackInfo {
    uint64_t low;        // virtual address of lowest mapped page
    uint64_t high;       // virtual address one past the top page (= top)
    uint64_t guard_page; // page just below `low` (PTE_PRESENT == 0)
    uint64_t init_bytes; // initial committed size
    uint64_t cap_bytes;  // hard cap before panic
    uint64_t bytes;      // currently mapped bytes (high - low)
    uint32_t grow_count; // number of times we've grown via guard fault
};

enum UserMemoryRegionFlags : uint32_t {
    USER_REGION_NONE        = 0,
    USER_REGION_DEMAND_ZERO = 1 << 0,
    USER_REGION_HEAP        = 1 << 1,
    USER_REGION_MMAP        = 1 << 2,
};

constexpr int PROCESS_MAX_USER_REGIONS = 32;

struct UserMemoryRegion {
    uint64_t start;
    uint64_t end;
    uint64_t page_flags;
    uint32_t flags;
    bool     active;
};

constexpr uint32_t PROCESS_FLAG_NONE = 0;
constexpr uint32_t PROCESS_FLAG_USER = 1 << 0;
// task is a thread that shares its parent's address space (clone with
// clone_vm|clone_thread). such a task must not free the shared address_space
// or the shared user stack when it exits  -  only its own kernel stack. (satoru)
constexpr uint32_t PROCESS_FLAG_THREAD = 1 << 1;

struct Process {
    uint32_t pid;
    uint32_t parent_pid;
    char name[32];
    ProcessState state;
    uint32_t priority; // 0 = high, 255 = low
    uint32_t flags;
    uintptr_t rsp;     // stack pointer (64-bit in long mode)
    uintptr_t rbp;     // base pointer
    uintptr_t rip;     // instruction pointer (for resume)
    uint32_t sleep_ticks;
    uint64_t address_space;
    uint64_t user_stack_top;
    uint64_t kernel_stack_top;

    // ── Preemptive scheduler state ──
    // The kernel-side stack pointer captured by scheduler_switch_to.
    // Treated as opaque: the asm helper writes/reads this field directly
    // and the C++ side just plumbs it.  Zero on a fresh process.
    uint64_t saved_rsp;

    // Adaptive kernel stack metadata (zero-initialised for legacy user
    // processes that use the old fixed allocator).
    KernelStackInfo kstack;

    // Priority tier + timeslice tracking driven by the PIT IRQ.
    uint8_t  prio_tier;          // PRIO_REALTIME..PRIO_LOW
    uint32_t timeslice_ms_left;  // ticks down on each PIT IRQ
    uint64_t sleep_until_ms;     // wake target for Process_Sleeping
    uint64_t cpu_ms_total;       // wall-time the process has run
    bool     is_kernel_proc;     // true for processes spawned by SpawnKernelProcess

    int exit_code;
    InterruptFrame user_frame;
    bool has_user_frame;
    // clone child_cleartid: user va whose word is zeroed + futex-woken on thread
    // exit (0 = unset). recorded so a joiner blocked in futex wakes up. (satoru)
    uint64_t clear_child_tid;
    bool waiting_for_child;
    uint32_t waiting_child_pid;
    uint64_t waiting_status_ptr;
    uint64_t next_mmap_base;
    UserMemoryRegion regions[PROCESS_MAX_USER_REGIONS];
    Process* parent;
    Process* first_child;
    Process* next_sibling;

    // CFS-style scheduling state
    uint64_t vruntime;          // weighted virtual runtime (ticks * 1024 / weight)
    uint64_t cpu_ticks_total;   // cumulative scheduler ticks charged to this task
    int      nice;              // -20..+19, default 0
    uint8_t  sched_class;       // 0=NORMAL/CFS, 1=FIFO, 2=RR, 3=IDLE
    uint8_t  cpu_affinity;      // bitmask of allowed CPUs (bit n = CPU n)
    uint8_t  interactive_score; // 0..16; recent I/O wakes bump it, CPU burn decays it
    uint8_t  reaped;            // 1 once DestroyProcess has freed the struct (double-free guard)
    uint64_t last_wake_ms;      // wall-time of most recent block->ready transition
    uint64_t sleep_start_ms;    // wall-time the task entered the most recent sleep
    uint64_t cgroup_quota_left_us; // remaining bandwidth in current refill window
    uint64_t cgroup_throttle_until_ms; // 0 if not throttled
    uint32_t cgroup_id;         // cgroup v2 membership
    uint32_t ns_pid;            // pid namespace id
    uint32_t ns_mnt;            // mount namespace id
    uint32_t ns_net;            // network namespace id
    uint32_t ns_user;           // user namespace id
    uint32_t ns_uts;            // uts namespace id
    uint32_t ns_ipc;            // ipc namespace id
    uint32_t ns_cgroup;         // cgroup namespace id

    // elf program-header info for the sysv auxv (musl uses at_phdr to locate
    // pt_tls / pt_gnu_relro). zero for non-elf / kernel processes. (satoru)
    uint64_t user_phdr_va;      // user va of the program headers (0 if unknown)
    uint16_t user_phnum;        // e_phnum
    uint16_t user_phent;        // e_phentsize

    Process* next;

    // per-task fs base (tls) and x87/sse state, saved on switch-out and
    // restored on switch-in so concurrent threads don't clobber each other's
    // tls or vector registers. fpu_state is the 512-byte fxsave image and must
    // stay 16-byte aligned. (satoru)
    uint64_t fs_base;
    alignas(16) uint8_t fpu_state[512];

    bool is_user() const { return (flags & PROCESS_FLAG_USER) != 0; }
    bool is_thread() const { return (flags & PROCESS_FLAG_THREAD) != 0; }
};

struct SchedulerProcessSnapshot {
    uint32_t pid;
    char name[32];
    ProcessState state;
    uint32_t priority;
    uint32_t flags;
    uint64_t cpu_ticks_total;
    uint32_t memory_kb;
    int nice;
    uint8_t sched_class;
    uint8_t  prio_tier;          // PRIO_REALTIME..PRIO_LOW
    uint32_t stack_kb;           // current committed kernel stack size
    uint32_t stack_cap_kb;       // hard ceiling
    uint32_t cpu_ms_total;       // wall-time charged to this process
    uint32_t stack_grow_count;   // adaptive growths so far
    bool     is_kernel_proc;
};

// Forward declared opaque function pointer for kernel processes.  Each
// kernel process is a `void(*)()` that runs forever or calls Exit().
typedef void (*KernelProcessEntry)();

class Scheduler {
public:
    static Process* current_process;
    static Process* ready_queue;
    static uint32_t next_pid;
    
    static void Init();
    static Process* CreateProcess(const char* name, void (*entry_point)(), uint32_t priority);
    static Process* CreateUserProcess(const char* name, uint64_t entry_point, uint32_t priority);
    static Process* CloneUserProcess(Process* parent);
    // create a thread that SHARES the parent's address space (same cr3)  -  used
    // by clone(clone_vm|clone_thread). the thread gets its own kernel stack and
    // runs on the caller-supplied user stack `child_stack`; its user frame is
    // copied from the parent so it returns to the same clone() call-site with
    // rax=0. tls_base, when set_tls is true, becomes the new thread's fs base.
    // (satoru)
    static Process* CreateUserThread(Process* parent, uint64_t child_stack,
                                     uint64_t tls_base, bool set_tls);
    static void MarkProcessExited(Process* proc, int exit_code);
    static Process* WaitForChild(Process* parent, uint32_t child_pid, int* exit_code);
    static bool WaitForProcess(Process* proc, int* exit_code);
    static void SaveUserFrame(Process* proc, const InterruptFrame* frame);
    static bool LoadUserFrame(Process* proc, InterruptFrame* frame);
    static Process* GetNextRunnableUser(Process* after);
    static bool ScheduleNextUser(InterruptFrame* frame);
    // atomically claim a Ready user thread this cpu may run (smp phase 3d: the
    // application-processor dispatch loop calls this to bootstrap a thread). marks
    // the winner Running under the scheduler lock so no two cores claim one
    // thread; returns null if nothing runnable is allowed on this cpu. (satoru)
    static Process* ClaimNextUserForCpu(uint32_t cpu);
    // claim a fresh (never-entered) Ready user process this cpu may LAUNCH (the AP
    // dispatch loop runs it via Userspace::RunProcessWithArgs). marks it Running
    // under the lock; returns null if none is allowed on this cpu. (satoru)
    static Process* ClaimFreshUserForCpu(uint32_t cpu);
    // smp phase 4: called from an application processor's LAPIC-timer ISR to
    // PREEMPT the user thread it is running  -  save the interrupted frame and
    // switch to the next runnable user thread for this cpu (the threads of one
    // process share an address space, so no cr3 change). ring-3 frames only; a
    // tick in the kernel/idle is ignored. (satoru)
    static void ApTimerPreempt(InterruptFrame* frame);
    static void ReapProcess(Process* proc);
    static void DestroyProcess(Process* proc);
    static void Schedule();
    static void Yield();
    static void Sleep(uint32_t ticks);
    static void Exit();
    static void Tick(); // called by timer interrupt

    // ── Preemptive multitasking API ────────────────────────────────────
    // Spawn a long-running kernel-mode process backed by an adaptive
    // PMM-mapped stack.  init_stack_kb is committed up-front; cap_stack_kb
    // is the hard cap before the page-fault handler panics.
    static Process* SpawnKernelProcess(const char* name,
                                       KernelProcessEntry entry,
                                       ProcessPriorityTier tier,
                                       uint32_t init_stack_kb,
                                       uint32_t cap_stack_kb);

    // Boot the preemptive scheduler.  Picks the first ready process,
    // primes TSS.RSP0 + IF=1, switches to it and never returns.
    [[noreturn]] static void Start();

    // Voluntary sleep / yield used inside kernel-process loops.  Sleep
    // is millisecond-granularity; Yield gives up the remaining
    // timeslice without any sleep deadline.
    static void SleepMs(uint32_t ms);
    static void YieldNow();

    // Service the sleep queue.  Called from the PIT IRQ0 hook (and from
    // any process via Yield)  -  wakes any process whose deadline has
    // passed.  Cheap O(N) scan; N is small.
    static void ServiceSleepQueue();

    // PIT-driven hook called once per IRQ0 (typically every 2 ms).
    // Charges runtime, decrements the timeslice, and notes if the
    // current process should be preempted at its next safe point.
    static void OnTimerTick(uint32_t ms_elapsed);

    // Adaptive stack growth: invoked by the page-fault handler when
    // CR2 falls in a kernel process's guard zone.  Returns true if the
    // fault was a legitimate stack grow and was satisfied.
    static bool TryGrowGuardPage(uint64_t cr2);

    // Diagnostic: walks all kernel processes and dumps a summary line
    // (name, tier, stack KB / cap KB, CPU%) into the supplied buffer.
    // Returns bytes written (excluding NUL).
    static int  DumpKernelProcessTable(char* buf, int max_len);

    /** After Scheduler::Start(): kproc multitasking is active. */
    static bool IsPreemptiveKernelSchedulerActive();

    /** Monotonic millisecond clock advanced by the PIT IRQ (reliable, unlike
     *  the polled Timer::PollUpdate clock). Use this for frame pacing. */
    static uint64_t NowMs();
    
    // performance monitoring
    static uint32_t GetProcessCount();
    static const char* GetCurrentProcessName();
    static Process* GetCurrentProcess();
    // set the calling cpu's current task (bsp global / ap PerCpu.current). (satoru)
    static void SetCurrentForThisCpu(Process* p);
    static Process* FindProcessByPid(uint32_t pid);
    static int GetProcessSnapshot(SchedulerProcessSnapshot* out, int max_count);

    // Phase 14: load average + cpu affinity ----------------------------
    // 1m, 5m, 15m EMAs of run-queue length, scaled FIXED_1 = 1<<11.
    static void     GetLoadAverage(uint32_t out_fixed[3]);
    static void     GetLoadAverageStr(char* out, int max_len);  // "0.42 0.31 0.18"
    static int      SetAffinity(uint32_t pid, uint8_t mask);
    static int      GetAffinity(uint32_t pid, uint8_t* out_mask);
};
