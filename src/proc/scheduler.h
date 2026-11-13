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
    // the main thread's initial (execve) stack. it is mapped eagerly but was
    // never recorded as a region, so choose_mmap_base()'s arena (growing up from
    // USER_MMAP_BASE) eventually handed a clone thread's stack right on top of
    // it once ~480 mb was mapped -> the font-loader thread's buffer stomped the
    // chrome main thread's live frame (deterministic nsCaret stack-canary #gp).
    // registering it makes region_overlaps_cross_thread() reserve the range. (satoru)
    USER_REGION_STACK       = 1 << 3,
};

// firefox's gecko closure is ~54 shared objects; even with adjacent same-prot
// reservations coalescing, libxul (loaded last, 133 mb) + the scattered musl
// malloc arenas + per-dso .bss anon maps blow past 32 -> add_region() returned
// null -> mmap -ENOMEM -> musl "Out of memory" loading libxul. 256 was enough to
// LOAD firefox but once it reaches the webrender render thread, mozjemalloc
// scatters hundreds more arena chunks across the shared address space (regions are
// per-address-space via region_owner) and 256 is exhausted -> a 512-byte rust
// alloc in the render thread returns null -> "memory allocation of 512 bytes
// failed" -> abort. give it real headroom for startup + early use. each slot is
// 28 bytes; 4096 -> ~114 kb/process (heap-allocated, so this is fine). (satoru)
constexpr int PROCESS_MAX_USER_REGIONS = 4096;

// most thread-group members one group kill handles in one pass; matches the
// 256 the scheduler's kill/reap paths use. the #pf containment in hal.cpp used
// a literal 64 while firefox with a few tabs runs ~80-120 threads, so members
// past 64 kept running on an address space about to be freed. (satoru)
constexpr int SCHED_GROUP_MAX = 256;

struct UserMemoryRegion {
    uint64_t start;
    uint64_t end;
    uint64_t page_flags;
    uint32_t flags;
    bool     active;
    // task 32 (boot speed): what file/offset backs this reservation, so a later
    // MAP_FIXED overlay of the SAME bytes (musl re-maps every PT_LOAD over the
    // whole-image map) can keep the frame instead of re-alloc+re-copying it.
    // file_off_base == ~0ull means "not file backed". (satoru)
    const void* file_node;
    uint64_t    file_off_base;
};

constexpr uint32_t PROCESS_FLAG_NONE = 0;
constexpr uint32_t PROCESS_FLAG_USER = 1 << 0;
// task is a thread that shares its parent's address space (clone with
// clone_vm|clone_thread). such a task must not free the shared address_space
// or the shared user stack when it exits - only its own kernel stack. (satoru)
constexpr uint32_t PROCESS_FLAG_THREAD = 1 << 1;
// the user stack (argc/argv/envp/auxv) is already built - by ld-kurono's
// ExecPIE for a dynamic pie - so the runner must enter at proc->rsp as-is and
// must NOT rebuild the stack (that would drop ld-kurono's complete auxv). (satoru)
constexpr uint32_t PROCESS_FLAG_STACK_READY = 1 << 2;
// sched=2: this non-thread task (a detached leader or a finished fork child) is
// fully built and may be claimed + resumed + parked by any application
// processor, exactly like a clone thread. a bit in the existing flags word, so
// struct Process keeps its size and layout. (satoru)
constexpr uint32_t PROCESS_FLAG_AP_OK = 1 << 3;
// the task is parked in an interruptible nanosleep/clock_nanosleep. its absolute
// wake deadline is sleep_until_ms and only the linux sleeper side table owns it,
// so sleep_ticks promotion must not re-ready it before that wall-clock deadline
// and a signal post can end the sleep with -EINTR. cleared by the promotion path
// (deadline reached) or by the sleeper/wake paths. never set for poll/futex
// waits, whose sleep_ticks cadence is unchanged. (satoru)
constexpr uint32_t PROCESS_FLAG_SLEEPING = 1 << 4;

// DO NOT add fields to this struct. last time i did, the APs started crashing 1 in 5 boots and firefox stopped painting. use a side table
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
    // NOTE: do NOT add fields to this struct casually. Inserting two here
    // (seed_entry/kp_ran, for the RIP=0x3 hunt) produced a NEW deterministic
    // "#GP rip=0x100BD4 err=0x6F90" on the APs - 4 boots in 20, and zero in the
    // same run without them - and took paint from 45% to 0%. Something depends
    // on this layout or on the struct's size. Whatever you are instrumenting,
    // do it in a side table, not in here. (satoru)

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
    // smp thread dispatch: which cpu last executed this task + when that cpu
    // last switched away from it. (satoru)
    uint8_t  last_run_cpu;
    // frame-handoff barrier (finish_task model). on_cpu==1 means this task's
    // user_frame is LIVE on last_run_cpu (being modified by execution); ==0 means
    // the owning cpu has finished saving it and another cpu may safely load it.
    // set 1 in LoadUserFrame (dispatch); store-RELEASED to 0 in ScheduleNextUser
    // AFTER the frame save, by the owning cpu; the picker load-ACQUIREs it. this
    // pairs the release of the saved frame with the acquire before another cpu
    // reads it - closing the half-saved-frame read that ran a task with corrupt
    // registers (the RIP=0x3 / userspace #PF). (satoru)
    volatile uint8_t on_cpu;
    uint64_t released_ms;
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

    // real on-disk path of the executed image, recorded at exec time so
    // /proc/self/exe (readlink + open) resolves to the actual install path
    // (e.g. /apps/firefox/firefox) instead of a synthesized /system/bin/<name>.
    // gecko 140 anchors its app directory off this; empty = unknown. (satoru)
    char     exe_path[256];

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
    // per-thread USER gs base (arch_prctl ARCH_SET_GS). firefox's rlbox/wasm2c
    // sandbox reads thread-locals through %gs, so a thread that set its gs base
    // must have it restored on every switch-in - exactly like fs_base. 0 = never
    // set (the common case; musl uses fs for tls). NOT read from an msr on
    // save-out: it only ever changes via arch_prctl, which persists it here, so
    // the stable stored value is authoritative. (satoru)
    uint64_t gs_base;
    alignas(16) uint8_t fpu_state[512];

    // torn-frame forensics (TAIL fields - appended so stale .o offsets survive
    // the makefile's missing header deps; a full rebuild is still the rule):
    // which code path last saved this task's user_frame (1=int80 entry 2=int80
    // exit 3=bsp preempt 4=ap preempt 5=fault 6=signal 7=sched_yield 8=execve
    // 0=other) + a running save counter. the contained ring-3 fault dump prints
    // them - at fault time they name the save the faulting thread RESUMED from
    // (the crossbeam rbx=0xA mixed-frame hunt). (satoru)
    uint8_t  last_save_site;
    uint32_t save_seq;

    // parked-stack canary (the residual stray-writer hunt): a snapshot of
    // [saved user rsp, rsp+256) captured when a PARKER-style futex waiter
    // (expected 0xFFFFFFFF - std/crossbeam parkers, whose stacks nothing
    // legitimately touches while parked) blocks; verified at the next
    // LoadUserFrame. any diff = a stray write into a parked thread's stack,
    // and the changed qwords (old vs new) fingerprint the writer. musl
    // condvar waiters (exp=2) are NOT canaried - siblings legitimately write
    // their on-stack wait nodes. (satoru)
    uint8_t  stk_canary[256];
    uint64_t stk_canary_rsp;
    uint16_t stk_canary_len;
    uint8_t  stk_canary_valid;

    // saved-frame integrity check (the deeper stray-writer hunt): a hash of the
    // CALLEE-SAVED regs (rbx/rbp/r12-r15) captured at SaveUserFrame and verified
    // at LoadUserFrame. those regs CANNOT legitimately change between a save and
    // its paired restore (no kernel path rewrites them - only caller-saved rax/
    // rcx/... get syscall-return values). a mismatch = a stray KERNEL write into
    // this Process' user_frame in the heap (e.g. a recycled-struct or cross-task
    // frame write) - the rbx=0xA crossbeam signature. (satoru)
    uint64_t frame_csum;
    uint8_t  frame_csum_valid;

    // [YRET] instrument (the lost-store hunt): at a site-7 sched_yield save,
    // fingerprint the just-pushed yield-return slot ([user rsp+8]) - value +
    // backing phys + the save_seq it belongs to. LoadUserFrame re-verifies at
    // the paired resume; any mismatch names the mechanism (stale-tlb split
    // view / generation swap / in-place writer / stale resume). (satoru)
    uint64_t yret_va;
    uint64_t yret_val;
    uint64_t yret_phys;
    uint32_t yret_seq;
    uint32_t last_load_seq;   // save_seq of the frame this task last RESUMED from (satoru)
    uint8_t  yprot_armed;     // task 27 trap: fingerprint page is write-protected (satoru)

    // task 28: lazily stamped by the picker when first seen Ready; cleared on
    // every successful pick. >64ms of Ready-unpicked = the starvation override
    // wins the pick (the early-stall crawl fix). (satoru)
    uint64_t ready_since_ms;

    // task 26 v3 (two-threads-one-stack): a sibling munmapped THIS thread's
    // in-use stack range while it was live; the pages + va were kept (claimed)
    // and the munmap DEFERRED here - sys_exit re-runs it as this thread (whose
    // own pages are then no longer protected), releasing va + frames properly.
    // 0 = none pending. (satoru)
    uint64_t stk_unmap_start;
    uint64_t stk_unmap_end;

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
    uint64_t address_space;      // cr3 of the task (0 for kernel tasks): lets the task manager walk its page tables for rss (satoru)
    int nice;
    uint8_t sched_class;
    uint8_t  prio_tier;          // PRIO_REALTIME..PRIO_LOW
    uint32_t stack_kb;           // current committed kernel stack size
    uint32_t stack_cap_kb;       // hard ceiling
    uint32_t cpu_ms_total;       // wall-time charged to this process
    uint32_t parent_pid;         // scheduler pid of the parent (kernel task /proc) (satoru)
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
    // create a thread that SHARES the parent's address space (same cr3) - used
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
    static void SaveUserFrame(Process* proc, const InterruptFrame* frame,
                              uint8_t save_site = 0);
    static bool LoadUserFrame(Process* proc, InterruptFrame* frame);
    // single-owner state: task->state and sleep_ticks are written ONLY under
    // g_sched_lock. the futex layer (a separate g_futex_lock domain) nests these
    // around its state transitions (g_futex_lock -> g_sched_lock, never the
    // reverse) so a woken/blocked task's state is never torn between the futex
    // and scheduler domains - the two-lock-one-field race that replayed user loop
    // iterations. (satoru)
    static void StateLock(uint64_t* out_flags);
    static void StateUnlock(uint64_t flags);
    static Process* GetNextRunnableUser(Process* after);
    static bool ScheduleNextUser(InterruptFrame* frame);
    // true once the running user thread has had its slice on this cpu (restarts it). (satoru)
    static bool SliceExpired();
    // atomically claim a Ready user thread this cpu may run (smp phase 3d: the
    // application-processor dispatch loop calls this to bootstrap a thread). marks
    // the winner Running under the scheduler lock so no two cores claim one
    // thread; returns null if nothing runnable is allowed on this cpu. (satoru)
    static Process* ClaimNextUserForCpu(uint32_t cpu);
    // claim a fresh (never-entered) Ready user process this cpu may LAUNCH (the AP
    // dispatch loop runs it via Userspace::RunProcessWithArgs). marks it Running
    // under the lock; returns null if none is allowed on this cpu. (satoru)
    static Process* ClaimFreshUserForCpu(uint32_t cpu);
    static void EndApLaunch(uint32_t cpu);   // agent_sched_10 (satoru)
    // claim a Ready sibling THREAD (clone with a saved user frame) this cpu may
    // RESUME - the smp thread-dispatch path. an ap pulls a ready thread of a
    // multi-threaded process (cpu_affinity 0 = any cpu) and irets into its saved
    // frame, so it runs in parallel with the thread-group leader on the bsp. the
    // leader itself is never claimed here (its userspace session lives on the
    // bsp). the winner is marked Running under the scheduler lock so no two
    // cores can double-run one thread. (satoru)
    static Process* ClaimReadyThreadForCpu(uint32_t cpu);
    // smp phase 4: called from an application processor's LAPIC-timer ISR to
    // PREEMPT the user thread it is running - save the interrupted frame and
    // switch to the next runnable user thread for this cpu (the threads of one
    // process share an address space, so no cr3 change). ring-3 frames only; a
    // tick in the kernel/idle is ignored. (satoru)
    static void ApTimerPreempt(InterruptFrame* frame);
    // gs-base fixup for the IRQ preempt paths (kls_timer_preempt / ApTimerPreempt).
    // those enter via isr_common, which does NOT swapgs, so on iret the ACTIVE gs
    // must already hold the resumed thread's user gs and KERNEL_GS_BASE must hold
    // the per-cpu ptr - the opposite of the syscall (swapgs) path that LoadUserFrame
    // targets. call after a successful ScheduleNextUser in an IRQ context. (satoru)
    static void FixupGsAfterIsrSwitch();
    static void ReapProcess(Process* proc);
    static void DestroyProcess(Process* proc);
    // is this address space still live on any cpu or in any non-terminated
    // task other than `exclude`? callers that destroy an address space use
    // this to defer/leak instead of freeing live page tables (execve old-as
    // guard). (satoru)
    static bool AddressSpaceLiveElsewhere(uint64_t as, Process* exclude);
    static void Schedule();
    static void Yield();
    static void Sleep(uint32_t ticks);
    static void Exit();
    static void Tick(); // called by timer interrupt
    // last time the cooperative Schedule() loop ran - the bsp-starve detector
    // reads this from the timer isr to catch a ring-3 monopolist. (satoru)
    static uint64_t LastScheduleMs();
    // when the cooperative KERNEL-PROC scheduler last handed over the cpu. this
    // is the clock the starvation valve must watch: LastScheduleMs() tracks the
    // USER-thread picker, which IRQ0 refreshes every tick and so never goes
    // stale enough to trip a >50ms trigger. (satoru)
    static uint64_t LastKernelSwitchMs();
    // kurono.donate=0. lets the A/B run one binary on both arms. (satoru)
    static void SetDonateEnabled(bool on);
    static bool DonateEnabled();
    // capture the parked-stack canary for a PARKER futex waiter (see the
    // Process fields). called by the futex block path right after the block
    // commits; verified inside LoadUserFrame on the next resume. (satoru)
    static void CaptureStackCanary(Process* proc);
    // find a live user task in address space `as` (other than `exclude`) whose
    // saved rsp falls in [start,end) - the unmap-overlaps-live-stack detector.
    // returns null if none. (satoru)
    static Process* FindStackOwnerInRange(uint64_t as, Process* exclude,
                                          uint64_t start, uint64_t end);
    // is page `pg` (page-aligned) at or above the saved rsp of a LIVE thread in
    // address space `as` (other than `exclude`), within `window` bytes of that
    // rsp? i.e. is it IN-USE stack the kernel must not free out from under a
    // running thread. (satoru)
    static bool PageIsLiveStack(uint64_t as, Process* exclude, uint64_t pg,
                                uint64_t window);

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
    // voluntary preemption point (linux's cond_resched) for long kernel-proc work:
    // yields only when the timer asked for a resched (slice used up, or a higher tier
    // is ready), only from a kernel proc on the bsp with IF on and no kls held. a
    // no-op anywhere else, so it is safe to call from code shared with syscalls.
    // shared-points step 5 stage 1. (satoru)
    static void CondResched();
    // service cpus (kurono.svccpu, sched=2): kernel procs homed on aps of their
    // own (the desktop, audio + network, the vm host), each such ap running its
    // own cooperative kernel-proc loop. see scheduler.cpp. (satoru)
    static void SetServiceCpus(bool on);
    static void SetServiceCpuRequest(int ui, int io, int vm);   // -1 = default (satoru)
    static void PlanServiceCpus();            // after SMP::StartAPs, before SpawnAll (satoru)
    static bool IsServiceCpu(uint32_t cpu);
    static uint32_t UiCpu();                  // 0 = the desktop stays on cpu 0 (satoru)
    static uint32_t VmCpu();
    // parallel vm vcpus: kurono.vm.par asks for n extra service cpus (cpu of vcpu k+1 = VcpuCpu(k+1)),
    // 0 = none was planned. call before PlanServiceCpus (satoru)
    static void SetVcpuCpus(int n);
    static uint32_t VcpuCpu(int k);
    static bool KernelProcCpu(uint32_t cpu);  // cpu 0 or a service cpu (satoru)
    static uint32_t FirstProgramCpu();        // lowest cpu that runs programs (satoru)
    [[noreturn]] static void ServiceCpuLoop();
    static void ServiceCpuTick();             // lapic tick of a service cpu (satoru)
    static void ServiceCensus(char* out, int max);
    // is the task with this scheduler pid still in the task list (and, if expect is
    // set, the same struct)? copies its state / exit code under the scheduler lock, so
    // a supervisor never reads a reaped struct the free-later ring may recycle. (satoru)
    static bool TaskStateByPid(uint32_t pid, const Process* expect, int* state, int* exit_code);

    // donate a slice from a USER task (ring-0 syscall context, bsp only) to the
    // cooperative kernel processes. SleepMs/YieldNow both early-return for a
    // non-kernel caller, so without this there is NO way to run a kernel proc
    // from user context - which is the starvation livelock that stalls ~50% of
    // boots. CALLER MUST HAVE RELEASED THE KLS LOCK. Returns true if a slice
    // was actually donated. See the block comment in scheduler.cpp. (satoru)
    static bool DonateToKernelProcs(uint32_t ms);
    // targeted version for the power worker: same user-task bracket and donor
    // slot, but the caller names the kernel proc. a power request must not
    // depend on the general picker's starvation heuristics. (satoru)
    static bool DonateToKernelProc(Process* target, uint32_t ms);
    // donation telemetry: (valve fired, slice handed over, nothing runnable).
    // a null stall result with ok==0 means the trigger never tripped, which is
    // a different bug from "donated and it did not help". (satoru)
    static void GetDonateStats(uint64_t* calls, uint64_t* ok, uint64_t* none);

    // Service the sleep queue.  Called from the PIT IRQ0 hook (and from
    // any process via Yield) - wakes any process whose deadline has
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
    // kernel-proc stack guard (see alloc_adaptive_stack): a genuinely unmapped
    // page below every kernel proc stack; kurono.kstackguard=1, default on under
    // sched=2. call before the first SpawnKernelProcess. KstackCensus prints the
    // high-water marks from the heartbeat. (satoru)
    static void SetKstackGuard(bool on);
    static void KstackCensus();
    // kurono.kplat=1: kernel-proc wake-to-run latency + longest-run probe, one
    // [kplat] line per proc every 10 s from the heartbeat (step 5 measurement). (satoru)
    static void SetKprocLatProbe(bool on);
    // diagnostic: log why the "power" kernel proc is or is not picked. (satoru)
    static void SetPickTrace(bool on);
    static void KprocLatReport();
    // kurono.wakelat=1: permanent wake-to-run accounting in side tables, per task
    // and per cpu, with the Ready-vs-Blocked attribution. one [wakelat] block
    // every 10 s from the heartbeat. clock = Scheduler::NowMs (cpu 0 advances it,
    // every cpu reads the same word; no cross-cpu tsc subtraction). (satoru)
    static void SetWakeLat(bool on);
    static void WakeLatReport();
    // kurono.vrblock=1: charge the real time a task ran at every deschedule,
    // including a block (default off: it changes pick order at every cpu count
    // and needs a real firefox batch before it ships). (satoru)
    static void SetVrBlock(bool on);
    // kurono.claimage=1: the claim picker's ready_since override (default off;
    // also gates the eager Ready stamps and the claim pack-min refresh). (satoru)
    static void SetClaimAge(bool on);
    // the kld broker's isr futex wake (LinuxSyscallFutexWakePhys) counts here. (satoru)
    static void WakeLatNotePhysIrq(bool woke);
    // kurono.wakelat: a blocking connect took the park instead of a ring0 spin. (satoru)
    static void WakeLatNoteConnPark();
    // ... and the gated lock(1)/pty(2)/job(3)/sigsuspend(4) parks. (satoru)
    static void WakeLatNoteWaitPark(uint32_t kind);
    // kurono.wakelat: record the caller of a ring-0 wait loop (kls_relax_sleep_ms),
    // so a busy ap's spin names its syscall. (satoru)
    static void WakeLatNoteRing0(uint64_t caller);
    // #DF path: an overflow whose #PF push itself faulted arrives as a double
    // fault with cr2 = the guard; name the proc there too. (satoru)
    static bool KstackGuardHit(uint64_t cr2);

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
    static uint32_t LiveTaskCount();   // the live-task cap counter (MAX_LIVE_PROCS) (satoru)
    // a CLONE_THREAD task that left through sys_exit: it no longer counts toward
    // the live-task cap. nothing reaped exited threads, so the cap counter only
    // ever grew; firefox churns pool threads, and once it reached 256 every
    // clone() failed (MOZ_CRASH(pthread_create failed), "Failed to create IPC
    // I/O Thread", glean "Unable to spawn"). drops the count, marks the task
    // reaped and frees its kernel stack once no cpu can land on it; the struct
    // stays leaked as before, and retirements stop after 1024 to bound that
    // leak. returns false (retry later) while a cpu may still run it. (satoru)
    static bool RetireExitedThread(Process* t);
    static const char* GetCurrentProcessName();
    static Process* GetCurrentProcess();
    // set the calling cpu's current task (bsp global / ap PerCpu.current). (satoru)
    static void SetCurrentForThisCpu(Process* p);
    static Process* FindProcessByPid(uint32_t pid);
    static int GetProcessSnapshot(SchedulerProcessSnapshot* out, int max_count);
    // one task by scheduler pid, copied under g_sched_lock (short). (satoru)
    static int GetProcessSnapshotByPid(uint32_t pid, SchedulerProcessSnapshot* out);
    // every non-user task's scheduler pid, copied under g_sched_lock. (satoru)
    static int CollectKernelTaskPids(uint32_t* out, int max);

    // collect every live user task sharing pid's address space (the thread
    // group: leader + clone threads). returns the count written to out. (satoru)
    static int CollectAddressSpaceGroup(uint32_t pid, Process** out, int max);
    // pid-only variant: copies the pids out UNDER the scheduler lock. callers
    // that only need identities must use this - raw Process* from the pointer
    // variant can be heap-freed by a concurrent DestroyProcess the instant the
    // lock drops (the task-manager refresh uaf). (satoru)
    static int CollectAddressSpacePids(uint32_t pid, uint32_t* out, int max);
    // safely end an entire user thread group under smp: mark every member
    // terminated, wait until no cpu still runs any of them, then free kernel
    // stacks + the shared address space. the Process structs are deliberately
    // leaked (futex waiter slots may still hold raw pointers; a stray wake's
    // CAS fails harmlessly on a Terminated struct but corrupts a recycled
    // one). returns 0 if pid is unknown, not a user task, or the group
    // contains the calling task; 1 = quiesced + reaped; 2 = marked dead but a
    // cpu never let go in time - the group went to the DEFERRED reaper, so the
    // caller must NOT tear per-process records down yet. (satoru)
    static int KillProcessGroup(uint32_t pid);
    // mark the group dead NOW and hand the quiesce+reap to the deferred reaper.
    // never sleeps - safe from the kls syscall body (the sync form quiesce-
    // slept under kls_lock while a target member spun on that same lock, an
    // unwinnable wait). `skip` is exempted (a fatal-signal self-exit whose own
    // exit path handles it). (satoru)
    static bool KillProcessGroupAsync(uint32_t pid, Process* skip);
    // true while a task of address space `as` other than `self` is current on
    // some other cpu (a killed sibling still running its last slice) (satoru)
    static bool AsRunningOnOtherCpu(uint64_t as, Process* self);
    // bookkeeping-only enqueue of already-Terminated members for a later reap;
    // isr-safe (the hal fault containment queues from the #pf handler). (satoru)
    // pid of any live (unreaped) user task in this address space, or 0. (satoru)
    static uint32_t FindLivePidInAddressSpace(uint64_t as);
    static void QueueDeferredReap(Process** members, int n);
    // retry-per-call, never-sleeping drain: reap each queued group once every
    // member is observably off every cpu. runs from the scheduler kernel
    // process heartbeat. (satoru)
    static void DrainDeferredReaps();
    static void drain_deferred_reaps_locked();   // DrainDeferredReaps body, one consumer at a time (satoru)

    // Phase 14: load average + cpu affinity ----------------------------
    // 1m, 5m, 15m EMAs of run-queue length, scaled FIXED_1 = 1<<11.
    static void     GetLoadAverage(uint32_t out_fixed[3]);
    static void     GetLoadAverageStr(char* out, int max_len);  // "0.42 0.31 0.18"
    static int      SetAffinity(uint32_t pid, uint8_t mask);
    static int      GetAffinity(uint32_t pid, uint8_t* out_mask);

    // SMP futex liveness. the deferred-wake promotion (sleep_ticks -> Ready) and
    // the linux futex repoll/timeout heal used to run ONLY on the bsp (via
    // Scheduler::Tick + kls_timer_preempt). when the bsp task blocks - e.g. the
    // firefox chrome main thread waiting on the software-WebRender render, whose
    // WRWorker/SwComposite threads run on APs - those AP threads that park on a
    // futex were never promoted/healed, so the render frame never completes
    // (the blank-content deadlock). the APs now run both in ApTimerPreempt:
    // PromoteDeferredWakes advances sleep_ticks->Ready; the registered hook runs
    // the linux futex sweep (futex_sweep_timeouts). (satoru)
    static void PromoteDeferredWakes();
    static void SetApFutexMaintHook(void (*fn)());
    // idle-ap maintenance: futex heal + deferred-wake promotion, called by the
    // ap dispatch loop while parked in ring-0 (task 17). (satoru)
    static void ApIdleMaint();
    // task 21 a/b: hard-pin threads to their last cpu (kurono.pincpu=1). (satoru)
    static void SetPinCpu(bool on);
    // ── sched=2 (kurono.sched=2) ───────────────────────────────────────────
    // the smp scheduling mode, the default since the smp job; kurono.sched=1
    // boots the old one. everything below is a no-op / old behavior when it is
    // off. call before SMP::Init. (satoru)
    static void SetSchedMode(int mode);
    static bool SchedV2();
    // kurono.schedrel=0 (a/b only): keep sched=2 but publish on_cpu=0 at the
    // pick, the old early release, so one binary can test the hand-off. (satoru)
    static void SetDeferredRelease(bool on);
    // kurono.schedlaunch=0 (a/b only): keep sched=2 but launch programs the old
    // way (ring 3 on the calling bsp kernel proc). DetachedLaunch() is the gate. (satoru)
    static void SetDetachedLaunch(bool on);
    static bool DetachedLaunch();
    // sched=2: sched_getaffinity reports every online cpu (musl's cpu count and
    // rust's available_parallelism read it). kurono.schedcpus=0 restores the
    // old one-cpu answer for an a/b. (satoru)
    static void SetReportAllCpus(bool on);
    static bool ReportAllCpus();
    // finish_task_switch for kurono's iret model. the cpu that stops running a
    // user task is still executing on that task's kernel stack until it irets
    // away, so on_cpu must not drop to 0 yet: another cpu resuming the task
    // would push its next entry frame onto the very stack being unwound. under
    // sched=2 ReleaseOnCpu only queues the task in a per-cpu pending slot;
    // FinishSwitch publishes on_cpu=0 once this cpu's rsp is outside the task's
    // kernel stack. every place that used to store on_cpu=0 calls ReleaseOnCpu;
    // every kernel entry calls FinishSwitch. old mode: immediate store. (satoru)
    static void ReleaseOnCpu(Process* p);
    static void FinishSwitch();
    // may an application processor claim/resume/park this task? clone threads
    // always; under sched=2 also tasks marked PROCESS_FLAG_AP_OK. (satoru)
    static bool ApResumable(const Process* p);
    // sched=2: mark a fully built non-thread task resumable by the aps (the
    // caller has already made it Ready). (satoru)
    static void MarkApResumable(Process* p);
    // sched=2: wake one halted application processor so a task that just
    // became runnable is picked up now instead of at that cpu's next timer
    // tick. no-op in the old mode (the aps poll there). (satoru)
    static void KickIdleCpu();
    static bool CpuMayRun(const Process* p, uint32_t cpu);
    static bool IsCurrentAnywhere(const Process* p);   // current on some cpu (satoru)   // affinity + sched=2 partition (satoru)
    // sched=2 asynchronous delivery: at every ring-3 interrupt on an ap (the
    // lapic tick, the resched ipi) a pending signal or a kill is acted on, so a
    // program spinning in ring 3 can be interrupted / killed. the linux layer
    // provides the delivery. KickTaskCpu sends the ipi to the cpu running t. (satoru)
    static void SetIrqSignalHook(bool (*pending)(Process* t), bool (*deliver)(InterruptFrame* frame));
    static bool ApUserIrqCheck(InterruptFrame* frame);
    static void KickTaskCpu(Process* t);
    // sched=2: called right before a reaped task's struct is freed, so the linux
    // layer can drop every raw pointer it holds to it. (satoru)
    static void SetTaskFreeHook(void (*fn)(Process*), bool (*referenced)(Process*));
    // sched=2 reap census: structs freed, waiting in the free-later ring, leaked (ring full). (satoru)
    static void ReapStats(uint64_t* freed, int* pending, uint64_t* leaked);
    // diagnostic: the cpu whose deferred-release slot still holds p, or -1. (satoru)
    static int DebugReleasePendingCpu(const Process* p);
    // task 22: linux-model min_vruntime clamp on wake - a woken task may lag
    // the runnable pack by at most one slice, never by its whole spin history.
    // lock-free racy scan; callable from any wake path. (satoru)
    static void NormalizeWakeVruntime(Process* p);
    // task 25: directed wake-next hand-off - the thread this cpu just woke runs
    // next on it (linux wakeup baton-pass). called from every wake site. (satoru)
    static void NoteWakeNext(Process* woken);
    // task 27: is phys a parked thread's fingerprinted yield-return page?
    // kernel copy paths probe their write destinations with this. (satoru)
    static Process* PhysIsParkedYieldStack(uint64_t phys);
    // task 27: a live sibling on the SAME musl stack slot as `top` (the exact
    // reuse discriminator for the two-threads-one-stack corruptor). (satoru)
    static Process* LiveSiblingWithStackTop(uint64_t as, uint64_t top);
    // task 27 THE TRAP: write-protect the fingerprinted page across the park;
    // a user writer faults into [YWRITE] red-handed. arm at the site-7 save,
    // auto-disarm at the resume verify; the fault hook restores RW + retries. (satoru)
    static bool YProtArm(Process* p);
    static void YProtDisarm(Process* p);
    static bool YProtCheckFault(uint64_t as, uint64_t fault_va, uint64_t rip);
};
