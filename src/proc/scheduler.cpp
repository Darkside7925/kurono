#include "scheduler.h"
#include "spinlock.h"
#include "kernel_locks.h"
#include "smp.h"            // per-cpu current process for application processors (satoru)
#include "../kernel/heap.h"
#include "../kernel/vmm.h"
#include "../kernel/pmm.h"
#include "../kernel/hrtimer.h"
#include "../kernel/panic.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"   // TSC ms clock for sleep deadlines (satoru)
#include "../hal/hal.h"
#include "../system/logging.h"

// ── Global kernel locks (declared in proc/kernel_locks.h) ──
Spinlock g_net_lock;
Spinlock g_input_lock;
Spinlock g_vfs_lock;
Spinlock g_fb_lock;
Spinlock g_audio_lock;
Spinlock g_log_lock;

// cross-core scheduler lock - protects ready_queue membership and the atomic
// "pick a Ready user thread and mark it Running" claim, so the bsp and the
// application processors can pull from one ready_queue without racing. the
// _nolock helpers below assume this is held; the public wrappers take it with
// LockIrqSave (cli first) so a timer irq that re-enters the scheduler on the
// SAME core can't self-deadlock, and so two CORES never corrupt the list. on a
// single active core it is always uncontended, so the bsp path is unchanged.
// this is the "_nolock discipline" smp phase 3d needs. (satoru)
static Spinlock g_sched_lock;

// switch_to.asm helpers
extern "C" void scheduler_switch_to(uint64_t* prev_saved_rsp,
                                    uint64_t  next_saved_rsp);
extern "C" [[noreturn]] void scheduler_jump_to(uint64_t saved_rsp);

namespace {
// 8 mb user stack (matches the linux main-thread default) so large static
// binaries like ffmpeg have room; grows down from user_stack_top to base
// 0x3fa00000, still ~506 mb above the mmap arena at 0x20000000. (satoru)
constexpr uint64_t USER_STACK_BYTES = 8 * 1024 * 1024;
// 16K->64K: a wayland client's sendmsg runs the in-kernel compositor SYNCHRONOUSLY
// on the caller's kernel stack (sendmsg -> UnixSocket::send_core -> wayland on_data
// -> handle_request -> commit_surface -> WindowManager blit), and each hop also
// carries ~824-byte ControlMsg locals. that chain exceeds 16K and overflowed the
// kernel stack, smashing a return address -> a ring-0 #UD (RIP=3) the first time a
// firefox surface committed. 64K gives the deep path headroom. (satoru)
// 64K->128K: once firefox's crypto (NSS softoken: cert-db SQLite I/O + login
// manager + SDR) runs during browser-window init, the syscall chain deepens
// further and, combined with that synchronous compositor path, overflowed 64K - 
// a smashed return jumped to garbage and #UD-panicked (ring-0, RIP not in kernel
// text). 128K covers the combined crypto + compositor depth. (satoru)
constexpr uint64_t KERNEL_STACK_BYTES = 128 * 1024;
constexpr uint64_t USER_STACK_TOP = USERSPACE_BASE + 0x00200000ULL;
// user mmap arena base. MUST sit above all identity-mapped physical ram: the
// kernel accesses every physical frame through the low identity map (phys==virt),
// so a user mapping whose VA aliases a physical frame the kernel later
// identity-touches (pmm zero-on-alloc, page-table walks, the framebuffer)
// clobbers that frame's identity leaf in the process's private page tables and
// #pf's the kernel. the old 0x20000000 (512mb) base aliased real ram; firefox's
// mmap-heavy musl mallocng hit it. park it at 16tb - above ram, below ld-kurono's
// 64tb aslr region. (satoru)
constexpr uint64_t USER_MMAP_BASE = 0x0000100000000000ULL;

// Lightweight IRQ-disable RAII guard for short scheduler critical sections.
// Cheaper than the global spinlocks and safe to nest (each guard snapshots
// its own caller-IF state, restores it on destruction).
struct IrqGuard {
    uint64_t flags;
    inline IrqGuard() {
        __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    }
    inline ~IrqGuard() {
        if (flags & 0x200ULL) __asm__ __volatile__("sti" ::: "memory");
    }
    IrqGuard(const IrqGuard&) = delete;
    IrqGuard& operator=(const IrqGuard&) = delete;
};

static inline uint8_t prio_tier_for_safe(Process* p) {
    if (!p) return (uint8_t)PRIO_NORMAL;
    return (p->prio_tier < PRIO_TIER_COUNT) ? p->prio_tier : (uint8_t)PRIO_NORMAL;
}

static void init_user_frame(Process* proc, uint64_t rip, uint64_t rsp) {
    memset(&proc->user_frame, 0, sizeof(proc->user_frame));
    proc->user_frame.rip = rip;
    proc->user_frame.rsp = rsp;
    proc->user_frame.cs = (uint64_t)(GDT_USER_CODE_SELECTOR | 3);
    proc->user_frame.ss = (uint64_t)(GDT_USER_DATA_SELECTOR | 3);
    proc->user_frame.rflags = 0x202ULL;
    proc->has_user_frame = true;
}

static bool alloc_kernel_stack(Process* proc) {
    void* kernel_stack = PMM::AllocBytes(KERNEL_STACK_BYTES);
    if (!kernel_stack) return false;

    proc->kernel_stack_top = (uint64_t)(uintptr_t)kernel_stack + KERNEL_STACK_BYTES;
    return true;
}

// count of currently-live (allocated, not yet reaped) Process objects. the
// creation cap gates on THIS, not the monotonic next_pid - next_pid is a unique
// id source that only ever increases, so capping on it wedged the system after
// ~32 TOTAL creations across the whole boot (firefox alone churns far more
// threads than that). gating on the live count lets thread create/exit cycles
// reuse capacity, which firefox 140 (heavily multithreaded) needs. (satoru)
static uint32_t g_live_proc_count = 0;
// ceiling on simultaneously-live tasks. kept at/below the linux process table
// size (LINUX_MAX_PROCS=64) since every schedulable user task pairs with a
// LinuxProcess slot. (satoru)
static constexpr uint32_t MAX_LIVE_PROCS = 256;  // 64->256 to match LINUX_MAX_PROCS (firefox thread/proc count) (satoru)

static void init_process_common(Process* proc, const char* name, uint32_t priority) {
    memset(proc, 0, sizeof(Process));
    proc->pid = Scheduler::next_pid++;
    proc->parent_pid = 0;
    g_live_proc_count++;

    int i = 0;
    while (name[i] && i < 31) {
        proc->name[i] = name[i];
        i++;
    }
    proc->name[i] = 0;

    proc->state = Process_Ready;
    proc->priority = priority;
    proc->sleep_ticks = 0;
    proc->exit_code = -1;
    proc->has_user_frame = false;
    proc->next_mmap_base = USER_MMAP_BASE;

    // Preemptive defaults; SpawnKernelProcess overrides these.
    proc->prio_tier         = (uint8_t)PRIO_NORMAL;
    proc->timeslice_ms_left = PROCESS_TIMESLICE_MS[PRIO_NORMAL];
    proc->sleep_until_ms    = 0;
    proc->cpu_ms_total      = 0;
    proc->saved_rsp         = 0;
    proc->is_kernel_proc    = false;
    proc->kstack            = {};
    proc->interactive_score = 8;        // start mid-range, decays under CPU
    proc->reaped            = 0;
    proc->last_wake_ms      = 0;
    proc->sleep_start_ms    = 0;
    proc->cgroup_quota_left_us      = 0;
    proc->cgroup_throttle_until_ms  = 0;

    // seed a valid fxsave image so the first fxrstor on switch-in doesn't #GP:
    // the memset zeroed the regs; set the default x87 control word (0x037f) and
    // mxcsr (0x1f80). user code resets these as it likes. (satoru)
    *(uint16_t*)(proc->fpu_state + 0)  = 0x037F;   // fcw
    *(uint32_t*)(proc->fpu_state + 24) = 0x1F80;   // mxcsr
}

static uint32_t estimate_process_memory_kb(const Process* proc) {
    if (!proc) return 0;

    uint64_t bytes = 0;
    if (proc->kernel_stack_top) bytes += KERNEL_STACK_BYTES;
    if (proc->is_user()) bytes += USER_STACK_BYTES;

    for (int i = 0; i < PROCESS_MAX_USER_REGIONS; i++) {
        const UserMemoryRegion& region = proc->regions[i];
        if (!region.active || region.end <= region.start) continue;
        bytes += region.end - region.start;
    }

    return (uint32_t)((bytes + 1023ULL) / 1024ULL);
}

// ── ready_queue helpers ────────────────────────────────────────────────────
// _nolock variants assume g_sched_lock is already held; the public wrappers
// take it. callers that already hold the lock (the atomic claim, the user pick)
// use the _nolock forms to avoid a self-deadlock on the non-recursive lock.
// (satoru)
static void enqueue_process_nolock(Process* proc) {
    if (!proc) return;
    // Reject duplicate enqueue - caused leaked queue cycles on resume races.
    for (Process* cur = Scheduler::ready_queue; cur; cur = cur->next) {
        if (cur == proc) return;
    }
    proc->next = Scheduler::ready_queue;
    Scheduler::ready_queue = proc;
}

static void remove_from_ready_queue_nolock(Process* proc) {
    if (!proc) return;
    if (Scheduler::ready_queue == proc) {
        Scheduler::ready_queue = proc->next;
        proc->next = nullptr;
        return;
    }
    Process* cursor = Scheduler::ready_queue;
    while (cursor && cursor->next != proc) cursor = cursor->next;
    if (cursor) {
        cursor->next = proc->next;
    }
    proc->next = nullptr;
}

static void enqueue_process(Process* proc) {
    if (!proc) return;
    uint64_t f; g_sched_lock.LockIrqSave(&f);
    enqueue_process_nolock(proc);
    g_sched_lock.UnlockIrqRestore(f);
}


static void remove_from_ready_queue(Process* proc) {
    if (!proc) return;
    uint64_t f; g_sched_lock.LockIrqSave(&f);
    remove_from_ready_queue_nolock(proc);
    g_sched_lock.UnlockIrqRestore(f);
}

static void link_child(Process* parent, Process* child) {
    if (!parent || !child) return;
    child->parent = parent;
    child->parent_pid = parent->pid;
    child->next_sibling = parent->first_child;
    parent->first_child = child;
}

static void unlink_child(Process* parent, Process* child) {
    if (!parent || !child) return;

    if (parent->first_child == child) {
        parent->first_child = child->next_sibling;
        child->next_sibling = nullptr;
        return;
    }

    Process* cursor = parent->first_child;
    while (cursor && cursor->next_sibling != child) cursor = cursor->next_sibling;
    if (cursor) {
        cursor->next_sibling = child->next_sibling;
    }
    child->next_sibling = nullptr;
}

static void sched_int_to_str(int value, char* out, int out_len) {
    if (!out || out_len < 2) return;
    if (value < 0) {
        out[0] = '-';
        sched_int_to_str(-value, out + 1, out_len - 1);
        return;
    }

    char tmp[16];
    int len = 0;
    do {
        tmp[len++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value && len < (int)sizeof(tmp));

    int i = 0;
    while (len > 0 && i < out_len - 1) {
        out[i++] = tmp[--len];
    }
    out[i] = 0;
}

static void sched_log_process_event(Process* proc, const char* event, const char* detail) {
    if (!proc) return;
    RuntimeLog::LogProcessEvent(proc->name, (int)proc->pid, event, detail);
}

static void sched_log_exit(Process* proc, int exit_code) {
    if (!proc) return;

    char detail[64];
    char exit_text[16];
    sched_int_to_str(exit_code, exit_text, sizeof(exit_text));

    detail[0] = 0;
    int i = 0;
    const char* prefix = "exit_code=";
    while (prefix[i] && i < (int)sizeof(detail) - 1) {
        detail[i] = prefix[i];
        i++;
    }
    detail[i] = 0;
    int j = 0;
    while (exit_text[j] && i < (int)sizeof(detail) - 1) {
        detail[i++] = exit_text[j++];
    }
    detail[i] = 0;

    sched_log_process_event(proc, "exited", detail);
}
}

// Forward declaration - definition is in the preemptive-internals block
// further down.  Plain file-scope linkage so both halves of this TU see it.
extern bool g_preemptive_active;

Process* Scheduler::current_process = nullptr;
Process* Scheduler::ready_queue = nullptr;
uint32_t Scheduler::next_pid = 1;

void Scheduler::Init() {
    // create "idle" process (pid 0)
    // in a real os, we would set up the stack for the current execution flow
    // as the first process.
}

Process* Scheduler::CreateProcess(const char* name, void (*entry_point)(), uint32_t priority) {
    (void)entry_point;
    if (g_live_proc_count >= MAX_LIVE_PROCS) return nullptr; // live-task cap (satoru)
    Process* proc = (Process*)KernelHeap::Alloc(sizeof(Process));
    if (!proc) return nullptr;

    init_process_common(proc, name, priority);
    if (current_process) {
        link_child(current_process, proc);
    }
    
    // stack setup would happen here (allocating stack page, pushing initial context)
    // proc->esp = ...
    
    // add to queue
    enqueue_process(proc);
    sched_log_process_event(proc, "created", "native kernel task");
    
    return proc;
}

Process* Scheduler::CreateUserProcess(const char* name, uint64_t entry_point, uint32_t priority) {
    if (g_live_proc_count >= MAX_LIVE_PROCS) return nullptr;  // live-task cap (satoru)

    Process* proc = (Process*)KernelHeap::Alloc(sizeof(Process));
    if (!proc) return nullptr;
    init_process_common(proc, name, priority);
    proc->flags = PROCESS_FLAG_USER;
    proc->rip = (uintptr_t)entry_point;

    proc->address_space = KernelVMM::CreateAddressSpace();
    if (!proc->address_space) {
        KernelHeap::Free(proc);
        return nullptr;
    }

    if (!alloc_kernel_stack(proc)) {
        KernelVMM::DestroyAddressSpace(proc->address_space);
        KernelHeap::Free(proc);
        return nullptr;
    }

    void* user_stack_phys = PMM::AllocBytes(USER_STACK_BYTES);
    if (!user_stack_phys) {
        PMM::FreeBytes((void*)(uintptr_t)(proc->kernel_stack_top - KERNEL_STACK_BYTES), KERNEL_STACK_BYTES);
        KernelVMM::DestroyAddressSpace(proc->address_space);
        KernelHeap::Free(proc);
        return nullptr;
    }

    uint64_t user_stack_base = USER_STACK_TOP - USER_STACK_BYTES;
    for (uint64_t offset = 0; offset < USER_STACK_BYTES; offset += PAGE_SIZE) {
        bool mapped = KernelVMM::MapPageInAddressSpace(
            proc->address_space,
            user_stack_base + offset,
            (uint64_t)(uintptr_t)user_stack_phys + offset,
            PTE_USER | PTE_WRITABLE
        );
        if (!mapped) {
            for (uint64_t rollback = 0; rollback < offset; rollback += PAGE_SIZE) {
                KernelVMM::UnmapPageInAddressSpace(proc->address_space, user_stack_base + rollback, false);
            }
            PMM::FreeBytes(user_stack_phys, USER_STACK_BYTES);
            PMM::FreeBytes((void*)(uintptr_t)(proc->kernel_stack_top - KERNEL_STACK_BYTES), KERNEL_STACK_BYTES);
            KernelVMM::DestroyAddressSpace(proc->address_space);
            KernelHeap::Free(proc);
            return nullptr;
        }
    }

    proc->user_stack_top = USER_STACK_TOP - 16;
    proc->rsp = (uintptr_t)proc->user_stack_top;
    init_user_frame(proc, entry_point, proc->user_stack_top);
    if (current_process) {
        link_child(current_process, proc);
    }
    enqueue_process(proc);
    sched_log_process_event(proc, "created", "native user task");
    return proc;
}

Process* Scheduler::CloneUserProcess(Process* parent) {
    if (!parent || !parent->is_user() || !parent->has_user_frame ||
        g_live_proc_count >= MAX_LIVE_PROCS) {
        return nullptr;
    }

    Process* proc = (Process*)KernelHeap::Alloc(sizeof(Process));
    if (!proc) return nullptr;

    {
        char detail[64];
        char pid_text[16];
        sched_int_to_str((int)parent->pid, pid_text, sizeof(pid_text));
        detail[0] = 0;
        int i = 0;
        const char* prefix = "cloned_from=";
        while (prefix[i] && i < (int)sizeof(detail) - 1) {
            detail[i] = prefix[i];
            i++;
        }
        detail[i] = 0;
        int j = 0;
        while (pid_text[j] && i < (int)sizeof(detail) - 1) {
            detail[i++] = pid_text[j++];
        }
        detail[i] = 0;
        sched_log_process_event(proc, "created", detail);
    }

    init_process_common(proc, parent->name, parent->priority);
    proc->flags = parent->flags;
    proc->rip = parent->rip;
    proc->rbp = parent->rbp;
    proc->rsp = parent->rsp;
    proc->user_stack_top = parent->user_stack_top;

    proc->address_space = KernelVMM::CloneAddressSpace(parent->address_space);
    if (!proc->address_space) {
        KernelHeap::Free(proc);
        return nullptr;
    }

    if (!alloc_kernel_stack(proc)) {
        KernelVMM::DestroyAddressSpace(proc->address_space);
        KernelHeap::Free(proc);
        return nullptr;
    }

    proc->user_frame = parent->user_frame;
    proc->has_user_frame = parent->has_user_frame;
    proc->next_mmap_base = parent->next_mmap_base;
    memcpy(proc->regions, parent->regions, sizeof(proc->regions));
    // the forked child IS a copy of the calling thread, so it must inherit that
    // thread's tls pointer (fs base) and fpu/sse state. without this the child
    // runs with fs base == 0 and musl's first tls access (%fs:0) reads NULL and
    // #PFs in set_tid_address/thread setup. (satoru)
    proc->fs_base = parent->fs_base;
    for (int b = 0; b < 512; b++) proc->fpu_state[b] = parent->fpu_state[b];
    link_child(parent, proc);
    enqueue_process(proc);
    return proc;
}

// create a real thread: a schedulable task that SHARES the parent's address
// space (same cr3 - page tables are not cloned) but owns a fresh kernel stack
// and runs on the caller-provided user stack. mirrors CloneUserProcess except
// for the shared address_space + the PROCESS_FLAG_THREAD marker that keeps
// DestroyProcess from tearing the address space down on thread exit. (satoru)
Process* Scheduler::CreateUserThread(Process* parent, uint64_t child_stack,
                                     uint64_t tls_base, bool set_tls) {
    if (!parent || !parent->is_user() || !parent->has_user_frame ||
        g_live_proc_count >= MAX_LIVE_PROCS) {
        return nullptr;
    }
    if (!child_stack) return nullptr;

    Process* proc = (Process*)KernelHeap::Alloc(sizeof(Process));
    if (!proc) return nullptr;

    init_process_common(proc, parent->name, parent->priority);
    // user + thread: shares the address space, must not free it on exit (satoru)
    proc->flags = PROCESS_FLAG_USER | PROCESS_FLAG_THREAD;

    // share the parent's address space verbatim - same pml4 phys / cr3 (satoru)
    proc->address_space = parent->address_space;

    if (!alloc_kernel_stack(proc)) {
        KernelHeap::Free(proc);
        return nullptr;
    }

    // start from the parent's saved user frame so cs/ss/rflags and the clone()
    // call-site rip are correct, then point the thread at its own stack and make
    // the syscall "return" 0 in the child like a real clone(). (satoru)
    proc->user_frame = parent->user_frame;
    proc->user_frame.rsp = child_stack;
    proc->user_frame.rbp = 0;
    proc->user_frame.rax = 0;
    proc->has_user_frame = true;

    proc->user_stack_top = child_stack;
    proc->rip = (uintptr_t)proc->user_frame.rip;
    proc->rsp = (uintptr_t)child_stack;
    proc->rbp = 0;
    proc->next_mmap_base = parent->next_mmap_base;
    // (satoru) start the child at the parent's vruntime, not 0. with the cfs charge
    // now applied in ScheduleNextUser, a vruntime-0 child would otherwise monopolize
    // the cpu until it caught up to its already-accumulated siblings. (satoru)
    proc->vruntime = parent->vruntime;

    // clone_settls: store the thread's tls as its saved fs base so LoadUserFrame
    // installs it when this thread is switched in. do NOT wrmsr here - the
    // parent is still running, and writing fs base now would clobber the
    // parent's tls. inherit the parent's fs base otherwise. (satoru)
    proc->fs_base = set_tls ? tls_base : parent->fs_base;
    // start from a copy of the parent's vector state (valid fxsave image). (satoru)
    for (int b = 0; b < 512; b++) proc->fpu_state[b] = parent->fpu_state[b];

    link_child(parent, proc);
    enqueue_process(proc);
    sched_log_process_event(proc, "created", "native user thread");
    return proc;
}

void Scheduler::MarkProcessExited(Process* proc, int exit_code) {
    if (!proc || proc->reaped) return;

    proc->exit_code = exit_code;
    proc->state = Process_Terminated;
    proc->sleep_ticks = 0;
    remove_from_ready_queue(proc);
    sched_log_exit(proc, exit_code);

    if (current_process == proc) {
        current_process = nullptr;
    }
}

Process* Scheduler::WaitForChild(Process* parent, uint32_t child_pid, int* exit_code) {
    if (!parent) return nullptr;

    Process* child = parent->first_child;
    while (child) {
        bool pid_match = child_pid == 0 || child->pid == child_pid;
        if (pid_match && child->state == Process_Terminated) {
            if (exit_code) *exit_code = child->exit_code;
            return child;
        }
        child = child->next_sibling;
    }

    return nullptr;
}

bool Scheduler::WaitForProcess(Process* proc, int* exit_code) {
    if (!proc || proc->state != Process_Terminated) return false;
    if (exit_code) *exit_code = proc->exit_code;
    return true;
}

// raw current-task pointer for the syscall asm stub. the SYSCALL fast path
// fxsaves the user's pristine fpu/sse state on entry and must fxrstor it before
// returning to ring-3 - otherwise kernel code that touches xmm (memcpy/graphics
// inline asm) leaves the user's xmm registers clobbered, which corrupted musl's
// __init_tp movups store of the main thread's tcb next/prev links and #pf'd the
// first pthread_create. but when the handler SWITCHED tasks (clone/futex/exit),
// LoadUserFrame already loaded the next task's fpu, so the stub must skip its
// restore. the stub compares this pointer before/after the handler to tell the
// two apart. (satoru)
extern "C" void* sched_current_task_raw() {
    return (void*)Scheduler::GetCurrentProcess();
}

void Scheduler::SaveUserFrame(Process* proc, const InterruptFrame* frame) {
    if (!proc || !frame || !proc->is_user()) return;

    proc->user_frame = *frame;
    proc->has_user_frame = true;
    proc->rip = (uintptr_t)frame->rip;
    proc->rsp = (uintptr_t)frame->rsp;
    proc->rbp = (uintptr_t)frame->rbp;

    // capture this task's live tls (fs base) + x87/sse state so a switch to a
    // sibling thread doesn't clobber them. fxsave/rdmsr are explicit asm (safe
    // under -mno-sse; sse is enabled in cr0/cr4 at boot). (satoru)
    constexpr uint32_t MSR_FS_BASE = 0xC0000100;
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_FS_BASE));
    proc->fs_base = ((uint64_t)hi << 32) | lo;
    __asm__ __volatile__("fxsave %0" : "=m"(proc->fpu_state));
}

// expose g_sched_lock so the futex layer can nest it around task->state /
// sleep_ticks writes (see scheduler.h). g_futex_lock is the OUTER lock there;
// g_sched_lock nested inside is safe because no path takes g_sched_lock then
// g_futex_lock (verified). LockIrqSave nests correctly: with IF already off the
// inner acquire captures off and the inner release leaves it off. (satoru)
// Step 1 (single-owner state) is intentionally a NO-OP. it IS needed for state
// integrity (no-op'ing it lets a fresh proc's state tear -> two cpus run its
// main(), the oracle "double-start"), BUT taking g_sched_lock on EVERY futex wake
// STALLS firefox at about:blank (measured: KDOC stuck) and spikes boot stalls - 
// firefox does enormous futex traffic. Step 3's on_cpu frame-handoff barrier is
// the actual corruption fix; the +50ms grace masks the residual state-tear. a
// proper Step 1 needs LIGHT (atomic CAS) state protection, not this global lock - 
// left as no-op until then. the call sites (Scheduler::StateLock/Unlock in the
// futex paths) are harmless no-ops. (satoru)
void Scheduler::StateLock(uint64_t* out_flags)  { if (out_flags) *out_flags = 0; }
void Scheduler::StateUnlock(uint64_t flags)     { (void)flags; }

bool Scheduler::LoadUserFrame(Process* proc, InterruptFrame* frame) {
    if (!proc || !frame || !proc->is_user() || !proc->has_user_frame) return false;

    SetCurrentForThisCpu(proc);   // per-cpu: bsp global / ap PerCpu.current (satoru)
    proc->last_run_cpu = (uint8_t)SMP::CpuIndex();   // cross-cpu resume grace (satoru)
    // frame is now live on THIS cpu - mark on_cpu so no other cpu resumes it
    // until we save + release it. plain store: this cpu is the sole writer, and
    // the picker's ACQUIRE-load pairs with our later RELEASE-store to 0. (satoru)
    proc->on_cpu = 1;
    proc->state = Process_Running;
    HAL::SetKernelStack(proc->kernel_stack_top);
    KernelVMM::ActivateAddressSpace(proc->address_space);

    // restore this task's tls (fs base) + vector state. (satoru)
    constexpr uint32_t MSR_FS_BASE = 0xC0000100;
    uint32_t lo = (uint32_t)proc->fs_base, hi = (uint32_t)(proc->fs_base >> 32);
    __asm__ __volatile__("wrmsr" : : "c"(MSR_FS_BASE), "a"(lo), "d"(hi));
    __asm__ __volatile__("fxrstor %0" : : "m"(proc->fpu_state));

    // restore this task's USER gs base into KERNEL_GS_BASE (0xC0000102). we run
    // in the kernel with active gs = per-cpu ptr; the syscall stub's swapgs on
    // exit swaps active <-> KERNEL_GS_BASE, so writing the thread's gs_base here
    // installs it in ring-3 %gs while the per-cpu ptr (currently active) lands
    // back in KERNEL_GS_BASE for the next entry. threads that never set a gs
    // base carry 0 = the default. firefox's rlbox/wasm2c reads thread-locals via
    // %gs, so without this the sandbox faults at the raw tls offset. (satoru)
    constexpr uint32_t MSR_KERNEL_GS_BASE_ = 0xC0000102;
    uint32_t glo = (uint32_t)proc->gs_base, ghi = (uint32_t)(proc->gs_base >> 32);
    __asm__ __volatile__("wrmsr" : : "c"(MSR_KERNEL_GS_BASE_), "a"(glo), "d"(ghi));

    *frame = proc->user_frame;
    return true;
}

// affinity gate: a thread with cpu_affinity 0 may run on any cpu; otherwise bit
// n must be set to run on cpu n. (satoru)
static inline bool cpu_allowed(const Process* p, uint32_t cpu) {
    return p->cpu_affinity == 0 || (p->cpu_affinity & (1u << cpu)) != 0;
}

// cross-cpu resume grace: after a cpu switches away from a task, its kernel
// stack stays live until that cpu's final iretq. the same cpu may re-pick the
// task immediately (it can't race itself), but ANOTHER cpu must wait ~2ms past
// the release timestamp so the unwind window (microseconds, but real) has
// passed. fresh tasks (released_ms 0) pass trivially. (satoru)
static inline bool cross_run_grace_ok(const Process* p, uint32_t cpu) {
    // cross-cpu resume guard. the SAME cpu can always re-pick its own task (it
    // can't race its own iretq). MITIGATION (2026-07-08): a genuine but
    // ultra-tight cross-cpu MIGRATION race replays user loop iterations (the
    // futex torture oracle over-count, src/userprogs/pthread_test.c) - an
    // event-based on_cpu fence did NOT close it and the window is so tight that
    // any fine instrumentation masks it, so it is NOT a simple unwind/load race.
    // the CONFIRMED trigger is cross-cpu migration itself; widening this grace
    // 1ms -> 50ms suppresses migration of a briefly-blocked thread (its own cpu
    // re-picks it first) and drove the oracle corruption to 0 across many boots.
    // this is a suppress-the-trigger MITIGATION, not the root fix; the principled
    // fix is single-owner state (unify the futex-wake + scheduler state writes
    // under one lock / dequeue-on-block, per linux __schedule/ttwu). (satoru)
    if ((uint32_t)p->last_run_cpu == cpu) return true;
    // frame-handoff barrier (the correctness fix): another cpu may resume p only
    // once its owning cpu RELEASE-stored on_cpu=0 (frame fully saved). ACQUIRE-load
    // pairs with that release -> never a half-saved user_frame -> no RIP=0x3/#PF. (satoru)
    if (__atomic_load_n(&p->on_cpu, __ATOMIC_ACQUIRE) != 0) return false;
    // TIME-DECAYED migration grace. 50ms masks the SMP-bringup fragility (its own
    // campaign) - but held forever it STARVES firefox: released_ms is re-stamped
    // every slice, so each cpu forms an eligibility island that keeps re-running
    // its residents while the ENTIRE ready render pipeline (Renderer/WRWorkers/
    // SwComposite/SceneBuilder - all st=Ready on=-1 in the loaded-page FWG dump)
    // waits 50ms per migration hop and the content never finishes rasterizing.
    // after the fragile bringup window, drop to 2ms so ready threads rebalance at
    // real speed. (satoru)
    uint64_t now = Scheduler::NowMs();
    uint64_t g   = (now < 90000) ? 50 : 2;
    return now > p->released_ms + g;
}

// the user pick - assumes g_sched_lock is held. selects the best Ready user
// thread THIS cpu is allowed to run (cfs vruntime, fifo/rr preempt, round-robin
// fallback). a thread already Running on another core has state != Ready, so it
// is skipped here; that, plus marking the winner Running under the SAME lock, is
// what stops two cores grabbing one thread. (satoru)
static Process* pick_next_user_nolock(Process* after, uint32_t cpu) {
    if (!Scheduler::ready_queue) return nullptr;
    Process* best = nullptr;
    Process* fifo = nullptr;
    for (Process* c = Scheduler::ready_queue; c; c = c->next) {
        if (!c->is_user() || c->state != Process_Ready || !c->has_user_frame) continue;
        if (!cpu_allowed(c, cpu)) continue;
        if (!cross_run_grace_ok(c, cpu)) continue;   // old cpu may still be unwinding (satoru)
        if (c->sched_class == 1 || c->sched_class == 2) {       // FIFO/RR
            if (!fifo || c->priority < fifo->priority) fifo = c;
            continue;
        }
        if (c->sched_class == 3) continue;                       // IDLE last
        if (!best || c->vruntime < best->vruntime) best = c;
    }
    if (fifo) return fifo;
    if (best) return best;

    // Final fallback: round-robin starting after `after`.
    Process* start = (after && after->next) ? after->next : Scheduler::ready_queue;
    for (Process* cursor = start; cursor; cursor = cursor->next)
        if (cursor->is_user() && cursor->state == Process_Ready &&
            cursor->has_user_frame && cpu_allowed(cursor, cpu) &&
            cross_run_grace_ok(cursor, cpu))
            return cursor;
    for (Process* cursor = Scheduler::ready_queue; cursor && cursor != start; cursor = cursor->next)
        if (cursor->is_user() && cursor->state == Process_Ready &&
            cursor->has_user_frame && cpu_allowed(cursor, cpu) &&
            cross_run_grace_ok(cursor, cpu))
            return cursor;
    return nullptr;
}

Process* Scheduler::GetNextRunnableUser(Process* after) {
    uint64_t f; g_sched_lock.LockIrqSave(&f);
    Process* p = pick_next_user_nolock(after, SMP::CpuIndex());
    g_sched_lock.UnlockIrqRestore(f);
    return p;
}

bool Scheduler::ScheduleNextUser(InterruptFrame* frame) {
    // per-cpu, smp-safe: atomically release THIS cpu's current user thread and
    // claim the next Ready one this cpu may run. EVERY state transition happens
    // under g_sched_lock so there is never a window where a thread is Ready-but-
    // unowned that another core could double-claim. on the bsp with a single
    // contender this behaves exactly as the old version. (satoru)
    uint32_t cpu = SMP::CpuIndex();
    Process* cur = GetCurrentProcess();
    Process* next = nullptr;
    {
        uint64_t f; g_sched_lock.LockIrqSave(&f);
        if (cur) {
            // stamp the release BEFORE the pick: whatever happens next, this
            // cpu is abandoning cur's kernel context (its stack stays in use
            // until our iretq - the grace the pickers honour). (satoru)
            cur->last_run_cpu = (uint8_t)cpu;
            cur->released_ms  = NowMs();
        }
        if (cur && cur->state == Process_Running) {
            // (satoru) charge cpu time to the OUTGOING thread so the cfs pick rotates.
            // this per-cpu user-thread switch (ApTimerPreempt + the futex/yield paths)
            // never passed through Tick's vruntime accounting - so a firefox worker that
            // spins without blocking kept the LOWEST vruntime and was re-picked forever,
            // starving the ready chrome main (the "main st=0 Ready but never runs, one
            // worker always Running" stall). mirror Tick's nice-weighted charge. (satoru)
            static const uint32_t kNiceW[40] = {
                88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
                 9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
                 1024,   820,   655,   526,   423,   335,   272,   215,   172,   137,
                  110,    87,    70,    56,    45,    36,    29,    23,    18,    15 };
            int nn = cur->nice; if (nn < -20) nn = -20; if (nn > 19) nn = 19;
            uint32_t ww = kNiceW[nn + 20];
            cur->vruntime += (1024u * 1024u) / (ww ? ww : 1);
            cur->state = Process_Ready;
        }
        Process* cand = pick_next_user_nolock(cur, cpu);
        if (cand && cand != cur) {
            cand->state = Process_Running;
            next = cand;
            // finish_task: we are switching AWAY from cur. its user_frame is fully
            // saved by now (preempt SaveUserFrame ran before this call; a blocking
            // syscall saved at entry + set its result). RELEASE-store on_cpu=0 so
            // another cpu that ACQUIRE-loads 0 is guaranteed to observe the complete
            // frame - never a half-saved one. only clear when actually leaving cur. (satoru)
            if (cur && cur->is_user())
                __atomic_store_n(&cur->on_cpu, (uint8_t)0, __ATOMIC_RELEASE);
        } else if (cur && cur->is_user() && cur->state == Process_Ready) {
            // keep running cur - but ONLY if it was Ready because WE released it
            // above. resurrecting a Blocked/Terminated cur to Running here let a
            // thread that just exited on an ap live forever (and hid it from the
            // pickers). the block-undo paths set Running themselves. (satoru)
            cur->state = Process_Running;
        }
        g_sched_lock.UnlockIrqRestore(f);
    }
    if (!next) return false;
    SetCurrentForThisCpu(next);
    return LoadUserFrame(next, frame);
}

// atomic bootstrap claim for an application processor that has no current user
// thread: pick + mark Running under the lock. returns null if nothing this cpu
// may run is Ready. the caller sets PerCpu.current and enters ring-3. (satoru)
Process* Scheduler::ClaimNextUserForCpu(uint32_t cpu) {
    uint64_t f; g_sched_lock.LockIrqSave(&f);
    Process* next = pick_next_user_nolock(nullptr, cpu);
    if (next) next->state = Process_Running;
    g_sched_lock.UnlockIrqRestore(f);
    return next;
}

// claim a FRESH (never-entered) Ready user process this cpu is allowed to run,
// for LAUNCH via RunProcessWithArgs - as opposed to ClaimNextUserForCpu, which
// claims an already-running thread to RESUME. "fresh" = has_user_frame == false.
// marks it Running under the lock so the bsp / another ap can't also grab it.
// the smp phase 3d AP dispatch loop uses this to run independent user processes
// on the secondary cores in parallel with the bsp. (satoru)
Process* Scheduler::ClaimFreshUserForCpu(uint32_t cpu) {
    uint64_t f; g_sched_lock.LockIrqSave(&f);
    // require an EXPLICIT affinity pin to this cpu - NOT the "affinity 0 = any cpu"
    // default. this closes a load race: CreateUserProcess enqueues a proc Ready
    // (affinity 0) and the CALLER then maps its ELF segments; a spinning ap must
    // not grab it mid-load. the launcher pins the proc to an ap (sets the affinity
    // bit) only AFTER it is fully loaded, so a pinned proc is always ready to run.
    // (satoru)
    Process* pick = nullptr;
    for (Process* c = ready_queue; c; c = c->next) {
        if (!c->is_user() || c->state != Process_Ready) continue;
        // never LAUNCH a clone sibling thread - it must be RESUMED from its saved
        // frame (the timer-preempt ScheduleNextUser path does that on the core
        // running the parent). launching it via RunProcessWithArgs would build a
        // fresh stack over its real one and corrupt it (RIP jumps to garbage). only
        // the thread-group process is claimable here. (satoru)
        if (c->is_thread()) continue;
        if ((c->cpu_affinity & (1u << cpu)) == 0) continue;   // explicit pin only (satoru)
        pick = c; break;
    }
    if (pick) {
        pick->state = Process_Running;
        // narrow the pin to THIS cpu. while the launching cpu runs this process it
        // time-shares the process's threads via the timer preempt, which releases
        // the leader back to Ready between slices - without this, ANOTHER ap would
        // see the released leader and launch a second copy. pinned to the owner,
        // only the owner (busy in RunProcessWithArgs) can re-pick it. (satoru)
        pick->cpu_affinity = (uint8_t)(1u << cpu);
    }
    g_sched_lock.UnlockIrqRestore(f);
    return pick;
}

// resume-claim for the ap thread-dispatch loop: pick the lowest-vruntime Ready
// SIBLING THREAD this cpu may run and mark it Running under the lock. threads
// only - the thread-group leader is bsp-owned (its exit must unwind the bsp's
// userspace session), and a fresh process without a saved frame must be
// LAUNCHED (ClaimFreshUserForCpu), not resumed. (satoru)
Process* Scheduler::ClaimReadyThreadForCpu(uint32_t cpu) {
    uint64_t f; g_sched_lock.LockIrqSave(&f);
    Process* pick = nullptr;
    for (Process* c = ready_queue; c; c = c->next) {
        if (!c->is_user() || !c->is_thread()) continue;
        if (c->state != Process_Ready || !c->has_user_frame) continue;
        if (c->sched_class == 3) continue;                    // idle class last (satoru)
        if (!cpu_allowed(c, cpu)) continue;
        if (!cross_run_grace_ok(c, cpu)) continue;   // old cpu may still be unwinding (satoru)
        if (!pick || c->vruntime < pick->vruntime) pick = c;
    }
    if (pick) pick->state = Process_Running;
    g_sched_lock.UnlockIrqRestore(f);
    return pick;
}

// see the header: undo LoadUserFrame's swapgs-oriented KERNEL_GS_BASE write for
// the no-swapgs IRQ path. after this, ring-3 iret resumes with active gs =
// resumed thread's user gs and KERNEL_GS_BASE = per-cpu ptr (so the next syscall
// entry swapgs still finds the per-cpu block). a thread that never set a gs base
// carries 0, which is the correct default. (satoru)
void Scheduler::FixupGsAfterIsrSwitch() {
    Process* cur = GetCurrentProcess();
    uint64_t gs  = cur ? cur->gs_base : 0;
    uint64_t percpu = (uint64_t)(uintptr_t)SMP::Current();
    constexpr uint32_t MSR_GS_BASE        = 0xC0000101;
    constexpr uint32_t MSR_KERNEL_GS_BASE = 0xC0000102;
    uint32_t alo = (uint32_t)gs, ahi = (uint32_t)(gs >> 32);
    __asm__ __volatile__("wrmsr" : : "c"(MSR_GS_BASE), "a"(alo), "d"(ahi));
    uint32_t plo = (uint32_t)percpu, phi = (uint32_t)(percpu >> 32);
    __asm__ __volatile__("wrmsr" : : "c"(MSR_KERNEL_GS_BASE), "a"(plo), "d"(phi));
}

// SMP futex liveness (see scheduler.h): APs run the deferred-wake promotion and
// the linux futex repoll/timeout heal so a bsp wedge can't strand AP-parked
// render/rayon threads forever. (satoru)
static void (*g_ap_futex_maint)() = nullptr;
void Scheduler::SetApFutexMaintHook(void (*fn)()) { g_ap_futex_maint = fn; }

void Scheduler::PromoteDeferredWakes() {
    uint64_t f; g_sched_lock.LockIrqSave(&f);
    for (Process* p = ready_queue; p; p = p->next) {
        if (p->state == Process_Blocked && p->sleep_ticks > 0) {
            if (--p->sleep_ticks == 0) {
                p->state = Process_Ready;
                if (p->interactive_score < 16) p->interactive_score += 2;
            }
        }
    }
    g_sched_lock.UnlockIrqRestore(f);
}

void Scheduler::ApTimerPreempt(InterruptFrame* frame) {
    if (!frame || (frame->cs & 3) != 3) return;   // ring-3 user code only (satoru)
    Process* cur = GetCurrentProcess();
    if (!cur || !cur->is_user()) return;
    // ring-3 only, so this ap holds no kernel lock here -> it is safe to run the
    // futex heal + deferred-wake promotion (same safety argument as the bsp's
    // kls_timer_preempt). doing it on the ap is what keeps AP-parked render/rayon
    // threads live when the bsp main thread is itself blocked. (satoru)
    if (g_ap_futex_maint) g_ap_futex_maint();   // linux futex repoll/timeout sweep
    PromoteDeferredWakes();                      // sleep_ticks -> Ready
    // save the interrupted thread's full state, then switch to the next runnable
    // user thread for this cpu. ScheduleNextUser rewrites *frame in place; the
    // ISR's iretq resumes whatever it picks. the threads of one process share cr3,
    // so no address-space switch is needed. (satoru)
    SaveUserFrame(cur, frame);
    if (ScheduleNextUser(frame)) {
        // isr path (no swapgs on iret): fix up gs so the resumed thread's user
        // gs is active and KERNEL_GS_BASE holds the per-cpu ptr. (satoru)
        FixupGsAfterIsrSwitch();
    } else {
        // staying on the same thread: reload cr3 anyway so a stale translation
        // (a sibling's munmap/madvise on another core that raced the shootdown
        // ipi) is bounded to one timer period, not forever. (satoru)
        uint64_t c3;
        __asm__ __volatile__("mov %%cr3, %0" : "=r"(c3));
        __asm__ __volatile__("mov %0, %%cr3" : : "r"(c3) : "memory");
    }
}

void Scheduler::ReapProcess(Process* proc) {
    if (!proc || proc->reaped || proc->state != Process_Terminated) return;
    DestroyProcess(proc);
}

void Scheduler::DestroyProcess(Process* proc) {
    if (!proc || proc->reaped) return;

    sched_log_process_event(proc, "destroyed", proc->is_user() ? "native user task released" : "native kernel task released");

    remove_from_ready_queue(proc);

    if (proc->parent) {
        unlink_child(proc->parent, proc);
        proc->parent = nullptr;
        proc->parent_pid = 0;
    }

    Process* child = proc->first_child;
    while (child) {
        child->parent = nullptr;
        child->parent_pid = 0;
        Process* next_child = child->next_sibling;
        child->next_sibling = nullptr;
        child = next_child;
    }
    proc->first_child = nullptr;

    if (current_process == proc) current_process = nullptr;

    // is the task's kernel context possibly still LIVE somewhere? two cases:
    // (a) we are reaping from inside the task's OWN exit syscall (our rsp is on
    //     its kernel stack), so the exit path keeps unwinding on that stack and
    //     reading this struct until its final iretq;
    // (b) another cpu still has it as its current task (mid-exit pre-iretq, or
    //     the released_ms grace window). (satoru)
    bool ctx_live = false;
    {
        uint64_t rsp_now;
        __asm__ __volatile__("mov %%rsp, %0" : "=r"(rsp_now));
        uint64_t kbase = proc->kernel_stack_top - KERNEL_STACK_BYTES;
        if (proc->kernel_stack_top &&
            rsp_now >= kbase && rsp_now < proc->kernel_stack_top) ctx_live = true;
        if (current_process == proc) ctx_live = true;
        for (uint32_t ci = 0; ci < SMP::CpuCount(); ci++) {
            PerCpu* pc = SMP::ByIndex(ci);
            if (pc && pc->current == proc) ctx_live = true;
        }
    }

    if (proc->is_user()) {
        // a thread shares its parent's address space + user stack - only the
        // process that owns the address space may tear it down. tearing it down
        // from a thread would unmap the parent and every sibling. (satoru)
        if (proc->address_space && !proc->is_thread()) {
            KernelVMM::DestroyAddressSpace(proc->address_space);
        }
        if (proc->kernel_stack_top && !ctx_live) {
            // never free the kernel stack a cpu is still EXECUTING ON (a thread
            // reaped from inside its own exit syscall, or a cross-cpu reap racing
            // the exiting cpu's unwind): with smp another core reuses + zeroes
            // the frames immediately, smashing the live exit path under its
            // feet. leak it instead - rare, and only 64k. (satoru)
            uint64_t kbase = proc->kernel_stack_top - KERNEL_STACK_BYTES;
            PMM::FreeBytes((void*)(uintptr_t)kbase, KERNEL_STACK_BYTES);
        }
    }

    proc->reaped = 1;
    proc->state  = Process_Terminated;
    if (g_live_proc_count > 0) g_live_proc_count--;   // free a live-task slot (satoru)
    // freeing the struct while a cpu still unwinds on it hands the heap a block
    // that gets recycled instantly (path buffers were observed landing in it) - 
    // the cpu then iretq's through ascii garbage (#UD/#GP with string-data
    // registers). leak it in the live cases, same tradeoff as the stack. (satoru)
    if (!ctx_live) KernelHeap::Free(proc);
}

void Scheduler::Schedule() {
    // hold the cross-core scheduler lock for the whole pick: an application
    // processor may be removing a just-exited user thread from ready_queue at the
    // same time. SpinLockGuard does cli + lock and releases on every return path.
    // (satoru)
    SpinLockGuard g(g_sched_lock);
    if (!ready_queue) return;

    if (current_process && current_process->state == Process_Running) {
        current_process->state = Process_Ready;
    }

    // CFS pick with interactivity bias: tasks recently woken by I/O get
    // a small effective-vruntime discount so they preempt CPU hogs.
    Process* best = nullptr;
    uint64_t best_eff = (uint64_t)-1;
    for (Process* p = ready_queue; p; p = p->next) {
        if (p->state != Process_Ready) continue;
        // Discount up to ~interactive_score * 65536 ticks of vruntime.
        uint64_t bias = (uint64_t)p->interactive_score << 16;
        uint64_t eff  = (p->vruntime > bias) ? (p->vruntime - bias) : 0;
        if (!best || eff < best_eff) { best = p; best_eff = eff; }
    }

    if (best) {
        current_process = best;
        current_process->state = Process_Running;
        if (current_process->is_user()) {
            HAL::SetKernelStack(current_process->kernel_stack_top);
        }
    } else if (current_process && current_process->state != Process_Running) {
        // nothing else runnable. keep current_process pointing at the (now
        // non-running) task instead of nulling it, so the idle window always has
        // a valid current task as the fallback and no reader can fault on a null
        // current_process; the blocking caller still drives the hlt/idle loop.
        // the active preemptive scheduler (pick_next_kernel/perform_switch) has
        // its own hlt idle fallback and never nulls current_process. (satoru)
    }
}

void Scheduler::Yield() {
    Schedule();
}

void Scheduler::Sleep(uint32_t ticks) {
    if (current_process) {
        current_process->sleep_ticks = ticks;
        current_process->state = Process_Blocked;
        Schedule();
    }
}

void Scheduler::Tick() {
    static const uint32_t nice_weight[40] = {
        88761, 71755, 56483, 46273, 36291,
        29154, 23254, 18705, 14949, 11916,
         9548,  7620,  6100,  4904,  3906,
         3121,  2501,  1991,  1586,  1277,
         1024,   820,   655,   526,   423,
          335,   272,   215,   172,   137,
          110,    87,    70,    56,    45,
           36,    29,    23,    18,    15
    };
    bool need_resched = false;
    {
        IrqGuard g;
        Process* cur = current_process;
        if (cur && cur->state == Process_Running) {
            cur->cpu_ticks_total++;
            int n = cur->nice;
            if (n < -20) n = -20;
            if (n > 19) n = 19;
            uint32_t w = nice_weight[n + 20];
            cur->vruntime += (1024u * 1024u) / (w ? w : 1);

            // Decay interactivity under sustained CPU burn.
            if (cur->interactive_score > 0 && (cur->cpu_ticks_total & 0x3) == 0) {
                cur->interactive_score--;
            }

            // Preemptive timeslice for the cooperative path too.
            if (cur->timeslice_ms_left == 0) {
                cur->timeslice_ms_left = PROCESS_TIMESLICE_MS[prio_tier_for_safe(cur)];
                need_resched = true;
            } else if (cur->timeslice_ms_left == 1) {
                cur->timeslice_ms_left = 0;
                need_resched = true;
            } else {
                cur->timeslice_ms_left--;
            }
        }

    }
    // (satoru) RING-0 HEARTBEAT for the futex machinery. this Tick runs from the
    // cooperative BSP kernel main loop (kurono_kernel), which keeps running even
    // when EVERY user thread has parked - unlike kls_timer_preempt / ApTimerPreempt,
    // which are both ring-3-gated and therefore STOP the instant the last user
    // thread blocks (the firefox SW-WR full wedge: chrome main blocked on the
    // render, render/rayon threads parked on futexes, no ring-3 left to preempt ->
    // the repoll heal never fires -> permanent deadlock). run the heal here so it
    // has a ring-0 pulse, then promote. cooperative context: this cpu holds no
    // futex/sched lock and is not mid-user-syscall, so taking the futex lock is
    // safe (an AP holding it just makes us spin briefly). (satoru)
    if (g_ap_futex_maint) g_ap_futex_maint();   // linux futex repoll/timeout heal
    PromoteDeferredWakes();                      // sleep_ticks -> Ready

    // Service hrtimers only when preemptive scheduler isn't already driving
    // them from the PIT IRQ - avoids double dispatch.
    if (!g_preemptive_active) HRTimer::Tick();

    if (need_resched) Schedule();

    // ---- Phase 14: load average sampling --------------------------
    // Sample run-queue length once per ~5 seconds (assuming HZ=100 ->
    // every 500 ticks) and feed it through three EMAs with Linux's
    // exponential weights: 1m=0.92044, 5m=0.98341, 15m=0.99445, scaled
    // FIXED_1 = 1<<11 = 2048.
    extern uint32_t g_load_tick_acc;
    extern uint32_t g_load_avg_fixed[3];
    g_load_tick_acc++;
    if (g_load_tick_acc >= 500) {
        g_load_tick_acc = 0;
        // active = ready + running
        uint32_t active = 0;
        Process* it = ready_queue;
        while (it) {
            if (it->state == Process_Ready || it->state == Process_Running)
                active++;
            it = it->next;
        }
        // sample in fixed-point
        uint32_t sample = active << 11;
        // EXP weights * 2048: ~1885, ~2014, ~2037
        static const uint32_t exp_w[3] = { 1885, 2014, 2037 };
        for (int i = 0; i < 3; i++) {
            uint64_t a = (uint64_t)g_load_avg_fixed[i] * exp_w[i];
            uint64_t b = (uint64_t)sample * (2048u - exp_w[i]);
            g_load_avg_fixed[i] = (uint32_t)((a + b) >> 11);
        }
    }
}

uint32_t Scheduler::GetProcessCount() {
    uint32_t c = 0;
    Process* p = ready_queue;
    while(p) { c++; p = p->next; }
    return c;
}

const char* Scheduler::GetCurrentProcessName() {
    if (current_process) return current_process->name;
    return "None";
}

Process* Scheduler::GetCurrentProcess() {
    // per-cpu: an application processor tracks its own running task in its PerCpu
    // block; the bsp keeps using the global. so the (shared) syscall handler asks
    // GetCurrentProcess() and transparently gets the right task per core. (satoru)
    if (SMP::CpuIndex() != 0) return SMP::Current()->current;
    return current_process;
}

// set the calling cpu's current task (global on the bsp, PerCpu.current on an ap). (satoru)
void Scheduler::SetCurrentForThisCpu(Process* p) {
    if (SMP::CpuIndex() != 0) SMP::Current()->current = p;
    else current_process = p;
}

Process* Scheduler::FindProcessByPid(uint32_t pid) {
    for (Process* proc = ready_queue; proc; proc = proc->next) {
        if (proc->pid == pid) return proc;
    }
    return nullptr;
}

int Scheduler::GetProcessSnapshot(SchedulerProcessSnapshot* out, int max_count) {
    if (!out || max_count <= 0) return 0;

    int count = 0;
    int guard = 0;
    for (Process* proc = ready_queue; proc && count < max_count; proc = proc->next) {
        // backstop against a corrupt/cyclic ready_queue: a bad `next` (e.g.
        // a freed-then-reused node, or memory clobbered by an out-of-bounds
        // write) would otherwise be dereferenced and #gp on a non-canonical
        // address. validate the pointer is plausible (kernel-heap range, in
        // physical ram, 8-aligned) and cap the walk. (satoru)
        uintptr_t pa = (uintptr_t)proc;
        if ((pa & 0x7u) || pa < 0x100000ULL || pa >= 0x400000000ULL) break;
        if (++guard > 4096) break;
        SchedulerProcessSnapshot& snap = out[count++];
        memset(&snap, 0, sizeof(snap));

        snap.pid = proc->pid;
        memcpy(snap.name, proc->name, sizeof(snap.name));
        snap.state = proc->state;
        snap.priority = proc->priority;
        snap.flags = proc->flags;
        snap.cpu_ticks_total = proc->cpu_ticks_total;
        snap.memory_kb = estimate_process_memory_kb(proc);
        snap.nice = proc->nice;
        snap.sched_class = proc->sched_class;
        snap.prio_tier = proc->prio_tier;
        snap.stack_kb = (uint32_t)(proc->kstack.bytes / 1024);
        snap.stack_cap_kb = (uint32_t)(proc->kstack.cap_bytes / 1024);
        snap.cpu_ms_total = (uint32_t)proc->cpu_ms_total;
        snap.stack_grow_count = proc->kstack.grow_count;
        snap.is_kernel_proc = proc->is_kernel_proc;
    }

    return count;
}

void Scheduler::Exit() {
    if (current_process) {
        MarkProcessExited(current_process, current_process->exit_code);
        Schedule();
    }
}

// ---- Phase 14: load average + cpu affinity ----------------------------

uint32_t g_load_tick_acc       = 0;
uint32_t g_load_avg_fixed[3]   = { 0, 0, 0 };

void Scheduler::GetLoadAverage(uint32_t out_fixed[3]) {
    out_fixed[0] = g_load_avg_fixed[0];
    out_fixed[1] = g_load_avg_fixed[1];
    out_fixed[2] = g_load_avg_fixed[2];
}

void Scheduler::GetLoadAverageStr(char* out, int max_len) {
    if (!out || max_len < 32) return;
    int p = 0;
    auto put = [&](char c){ if (p < max_len-1) out[p++] = c; };
    auto put_load = [&](uint32_t fx){
        // integer.fraction (2 decimal digits) from FIXED_1=2048
        uint32_t whole = fx >> 11;
        uint32_t frac  = ((fx & 0x7FFu) * 100u) >> 11;
        char tmp[12]; int ti = 0;
        if (whole == 0) tmp[ti++] = '0';
        while (whole) { tmp[ti++] = (char)('0' + (whole % 10)); whole /= 10; }
        while (ti) put(tmp[--ti]);
        put('.');
        put((char)('0' + (frac / 10)));
        put((char)('0' + (frac % 10)));
    };
    put_load(g_load_avg_fixed[0]); put(' ');
    put_load(g_load_avg_fixed[1]); put(' ');
    put_load(g_load_avg_fixed[2]); put(' ');
    // running/total + last_pid
    uint32_t total = 0, running = 0;
    Process* it = ready_queue;
    while (it) {
        total++;
        if (it->state == Process_Running || it->state == Process_Ready) running++;
        it = it->next;
    }
    char tmp[12]; int ti = 0;
    if (running == 0) tmp[ti++] = '0';
    { uint32_t v = running; ti = 0; if (v==0) tmp[ti++]='0'; while(v){tmp[ti++]=(char)('0'+(v%10));v/=10;} }
    while (ti) put(tmp[--ti]);
    put('/');
    { uint32_t v = total ? total : 1; ti = 0; if (v==0) tmp[ti++]='0'; while(v){tmp[ti++]=(char)('0'+(v%10));v/=10;} }
    while (ti) put(tmp[--ti]);
    put(' ');
    { uint32_t v = next_pid ? next_pid - 1 : 1; ti = 0; if (v==0) tmp[ti++]='0';
      while(v){tmp[ti++]=(char)('0'+(v%10));v/=10;} }
    while (ti) put(tmp[--ti]);
    put('\n');
    if (p < max_len) out[p] = 0;
}

int Scheduler::SetAffinity(uint32_t pid, uint8_t mask) {
    if (mask == 0) return -1;                  // EINVAL: empty mask
    Process* p = ready_queue;
    while (p) {
        if (p->pid == pid) {
            p->cpu_affinity = mask;
            return 0;
        }
        p = p->next;
    }
    return -1;                                 // ESRCH
}

int Scheduler::GetAffinity(uint32_t pid, uint8_t* out_mask) {
    if (!out_mask) return -1;
    Process* p = ready_queue;
    while (p) {
        if (p->pid == pid) {
            *out_mask = p->cpu_affinity;
            return 0;
        }
        p = p->next;
    }
    return -1;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Preemptive scheduler internals
//
//  The original cooperative scheduler API is preserved above so existing
//  callers (test_suite, linux_syscall, kurono_kernel boot) keep working.
//  Below are the new pieces required for full preemptive multitasking with
//  PMM-backed adaptive kernel stacks.
// ═══════════════════════════════════════════════════════════════════════════

// True once Scheduler::Start() has run.  File scope so SleepMs/YieldNow and
// other TUs (via Scheduler::IsPreemptiveKernelSchedulerActive) can see it.
bool g_preemptive_active = false;

namespace {

constexpr uint64_t KP_PAGE_SIZE   = 4096ULL;
constexpr uint64_t KP_GUARD_PAGES = 1;     // single guard page per process
constexpr int      KP_MAX         = 32;    // soft cap on tracked kernel procs

// Tracking table for kernel processes (those with adaptive stacks).
// Used by Scheduler::TryGrowGuardPage() when handling #PF faults.
Process* g_kernel_procs[KP_MAX] = {};
int      g_kernel_proc_count    = 0;

// Cumulative milliseconds of wall time the scheduler has observed.
volatile uint64_t g_sched_now_ms = 0;

// Set by OnTimerTick() when the running process exhausts its
// timeslice; consumed by SleepMs/YieldNow/CheckPreempt.
volatile bool g_need_resched = false;

// (g_preemptive_active lives at file scope above)

// Bootstrap process representing kernel_main itself.  We never destroy
// it; it just owns the boot stack.
Process g_bootstrap_proc;

inline void kp_serial_log(const char* msg) {
    SerialLogger::Log(msg);
}

inline uint64_t now_ms() {
    // TSC-based real-time ms, NOT the PIT-IRQ count g_sched_now_ms. VMware
    // COALESCES timer interrupts, so g_sched_now_ms (incremented by
    // OnTimerTick(1) per IRQ) crawls - which made every SleepMs(N) wait for
    // that slow clock and sleep ~10x too long, dragging the whole system to a
    // crawl (the laggy/micro-freeze + slow boot). the TSC advances at real
    // rate regardless of interrupt delivery, so sleep deadlines stay accurate
    // and processes wake on time. (satoru)
    return Timer::GetRealMs64();
}

inline uint8_t prio_tier_for(Process* p) {
    if (!p) return PRIO_NORMAL;
    if (p->prio_tier < PRIO_TIER_COUNT) return p->prio_tier;
    return PRIO_NORMAL;
}

inline uint32_t timeslice_for(Process* p) {
    return PROCESS_TIMESLICE_MS[prio_tier_for(p)];
}

static void kp_track(Process* p) {
    if (!p) return;
    if (g_kernel_proc_count >= KP_MAX) return;
    g_kernel_procs[g_kernel_proc_count++] = p;
}

// Round up to whole pages.
inline uint64_t round_up_page(uint64_t bytes) {
    return (bytes + KP_PAGE_SIZE - 1ULL) & ~(KP_PAGE_SIZE - 1ULL);
}

// Allocate a contiguous PMM region for the initial kernel stack and
// configure the KernelStackInfo metadata.  Maps a guard page just
// below `low` (zero-mapped, marked not-present so #PF triggers grow).
//
// Layout (low to high addresses):
//
//   guard_page  [unmapped]      <-- triggers #PF on overflow
//   low         [mapped]
//   ...         [mapped]
//   high - PAGE [mapped]
//   high                         <-- initial top of stack (rsp starts here)
//
// On growth, guard_page is decremented by one page, the previously-
// guard page becomes mapped, and a fresh guard page sits below.
static bool alloc_adaptive_stack(KernelStackInfo* k,
                                 uint64_t init_bytes,
                                 uint64_t cap_bytes) {
    init_bytes = round_up_page(init_bytes);
    cap_bytes  = round_up_page(cap_bytes);
    if (init_bytes < KP_PAGE_SIZE) init_bytes = KP_PAGE_SIZE;
    if (cap_bytes < init_bytes) cap_bytes = init_bytes;

    void* mem = PMM::AllocBytes(init_bytes);
    if (!mem) {
        kp_serial_log("[Sched] FATAL: PMM out of memory for kernel stack\r\n");
        return false;
    }

    k->low        = (uint64_t)(uintptr_t)mem;
    k->high       = k->low + init_bytes;
    k->guard_page = k->low - KP_PAGE_SIZE;
    k->init_bytes = init_bytes;
    k->cap_bytes  = cap_bytes;
    k->bytes      = init_bytes;
    k->grow_count = 0;
    return true;
}

// Build the initial stack frame so that scheduler_switch_to()'s
// matching pops + ret will land at `entry`.
//
// SysV AMD64 ABI: at function entry, (RSP % 16) == 8 (because a CALL
// instruction pushed an 8-byte return address onto a 16-aligned stack).
// Our kstack.high is page-aligned (mod 16 == 0), so we insert one 8-byte
// alignment slot at the very top before the simulated return address.
//
// Layout (top to bottom):
//
//      [high -  8]  alignment pad            <-- never popped
//      [high - 16]  entry                    <-- ret target (post-ret RSP=high-8)
//      [high - 24]  rflags (IF=1)
//      [high - 32]  rbp = 0
//      [high - 40]  rbx = 0
//      [high - 48]  r12 = 0
//      [high - 56]  r13 = 0
//      [high - 64]  r14 = 0
//      [high - 72]  r15 = 0                  <-- saved_rsp points here
static void seed_kernel_stack(Process* proc, KernelProcessEntry entry) {
    uint64_t* sp = (uint64_t*)(uintptr_t)proc->kstack.high;
    *(--sp) = 0;                                   // alignment pad @ high-8
    *(--sp) = (uint64_t)(uintptr_t)entry;          // ret address @ high-16
    *(--sp) = 0x0000000000000202ULL;               // rflags: IF=1, reserved bit
    *(--sp) = 0;                                   // rbp
    *(--sp) = 0;                                   // rbx
    *(--sp) = 0;                                   // r12
    *(--sp) = 0;                                   // r13
    *(--sp) = 0;                                   // r14
    *(--sp) = 0;                                   // r15

    proc->saved_rsp = (uint64_t)(uintptr_t)sp;
}

// Find the next runnable kernel process using a priority-tiered round
// robin.  We always prefer the highest tier with a Ready/Running task
// _other than `after`_, falling back to `after` only if literally no
// other process is runnable.  Excluding `after` lets a busy process
// (e.g. GUI calling YieldNow without sleeping) actually hand off to
// lower tiers; otherwise the picker would loop on the same process and
// LOW tier would starve forever.
static Process* pick_next_kernel(Process* after) {
    if (g_kernel_proc_count == 0) return nullptr;

    int start = 0;
    if (after) {
        for (int i = 0; i < g_kernel_proc_count; i++) {
            if (g_kernel_procs[i] == after) {
                start = i + 1;
                break;
            }
        }
    }

    uint64_t t = now_ms();

    // Pass 1: highest-tier first, within tier pick the most-interactive
    // candidate (highest interactive_score); ties go to round-robin order.
    // Skip cgroup-throttled tasks unless their bucket has refilled.
    for (uint8_t tier = 0; tier < PRIO_TIER_COUNT; tier++) {
        Process* best = nullptr;
        int best_score = -1;
        for (int step = 0; step < g_kernel_proc_count; step++) {
            int idx = (start + step) % g_kernel_proc_count;
            Process* cand = g_kernel_procs[idx];
            if (!cand || cand == after || cand->reaped) continue;
            if (cand->prio_tier != tier) continue;
            if (cand->state != Process_Ready &&
                cand->state != Process_Running) continue;
            if (cand->cgroup_throttle_until_ms > t) continue;
            int score = (int)cand->interactive_score;
            if (score > best_score) {
                best = cand;
                best_score = score;
            }
        }
        if (best) return best;
    }

    // Pass 2: relax cgroup throttling - would-be-throttled tasks may run
    // rather than leaving the CPU idle (Linux's CFS bandwidth approach).
    for (uint8_t tier = 0; tier < PRIO_TIER_COUNT; tier++) {
        for (int step = 0; step < g_kernel_proc_count; step++) {
            int idx = (start + step) % g_kernel_proc_count;
            Process* cand = g_kernel_procs[idx];
            if (!cand || cand == after || cand->reaped) continue;
            if (cand->prio_tier != tier) continue;
            if (cand->state != Process_Ready &&
                cand->state != Process_Running) continue;
            return cand;
        }
    }

    if (after && !after->reaped &&
        (after->state == Process_Ready || after->state == Process_Running)) {
        return after;
    }
    return nullptr;
}

// Update Process_Sleeping -> Process_Ready when the deadline passes.
// Apply an I/O-wake interactivity boost proportional to sleep length:
// short sleeps (typical of interactive event-loops) get the biggest bump.
static void wake_due_processes() {
    // this runs from BOTH IRQ0 (OnTimerTick) and process context (SleepMs idle
    // loop, ServiceSleepQueue) and does non-atomic RMW on p->state /
    // interactive_score. without the guard, IRQ0 firing mid-scan in process
    // context loses or double-applies a wake. IrqGuard is save/restore, so it
    // nests harmlessly when already called from IRQ context. (satoru)
    IrqGuard _g;
    uint64_t t = now_ms();
    for (int i = 0; i < g_kernel_proc_count; i++) {
        Process* p = g_kernel_procs[i];
        if (!p || p->reaped) continue;
        if (p->state == Process_Sleeping && p->sleep_until_ms <= t) {
            p->state = Process_Ready;
            p->last_wake_ms = t;
            uint64_t slept = (p->sleep_start_ms && t > p->sleep_start_ms)
                             ? (t - p->sleep_start_ms) : 0;
            uint8_t bump = (slept <= 4) ? 4 : (slept <= 16 ? 2 : 1);
            uint32_t s = (uint32_t)p->interactive_score + bump;
            p->interactive_score = (uint8_t)(s > 16 ? 16 : s);
        }
    }
}

// Perform the actual switch.  Saves prev's state, updates TSS.RSP0 to
// next's stack top (so any subsequent IRQ uses the right stack), and
// jumps via the asm helper.  Caller must hold IRQs disabled - the asm
// helper restores IF from the saved rflags slot on the new stack.
static void perform_switch(Process* prev, Process* next) {
    if (!next || prev == next) return;

    Scheduler::current_process = next;
    next->state = Process_Running;
    next->timeslice_ms_left = timeslice_for(next);
    g_need_resched = false;
    HAL::SetKernelStack(next->kstack.high);

    if (prev) {
        if (prev->state == Process_Running) prev->state = Process_Ready;
        scheduler_switch_to(&prev->saved_rsp, next->saved_rsp);
    } else {
        scheduler_jump_to(next->saved_rsp);
    }
}

} // namespace

bool Scheduler::IsPreemptiveKernelSchedulerActive() {
    return g_preemptive_active;
}

uint64_t Scheduler::NowMs() {
    return g_sched_now_ms;
}

Process* Scheduler::SpawnKernelProcess(const char* name,
                                       KernelProcessEntry entry,
                                       ProcessPriorityTier tier,
                                       uint32_t init_stack_kb,
                                       uint32_t cap_stack_kb) {
    if (!entry) return nullptr;
    Process* proc = (Process*)KernelHeap::Alloc(sizeof(Process));
    if (!proc) return nullptr;

    init_process_common(proc, name ? name : "kproc", (uint32_t)tier);
    proc->prio_tier         = (uint8_t)tier;
    proc->is_kernel_proc    = true;
    proc->state             = Process_Ready;
    proc->timeslice_ms_left = PROCESS_TIMESLICE_MS[tier];

    if (!alloc_adaptive_stack(&proc->kstack,
                              (uint64_t)init_stack_kb * 1024ULL,
                              (uint64_t)cap_stack_kb  * 1024ULL)) {
        KernelHeap::Free(proc);
        return nullptr;
    }
    proc->kernel_stack_top = proc->kstack.high;
    seed_kernel_stack(proc, entry);

    enqueue_process(proc);
    kp_track(proc);
    sched_log_process_event(proc, "spawned", "kernel process");
    return proc;
}

[[noreturn]] void Scheduler::Start() {
    Process* first = pick_next_kernel(nullptr);
    if (!first) {
        kp_serial_log("[Sched] FATAL: Start() with no kernel processes\r\n");
        for (;;) HAL::Halt();
    }

    // The kernel boot path becomes the bootstrap process so that future
    // switches have somewhere to "save" prev state.  We never resume it.
    memset(&g_bootstrap_proc, 0, sizeof(g_bootstrap_proc));
    int i = 0;
    const char* nm = "boot_idle";
    while (nm[i] && i < 31) { g_bootstrap_proc.name[i] = nm[i]; i++; }
    g_bootstrap_proc.name[i] = 0;
    g_bootstrap_proc.state = Process_Terminated;
    g_bootstrap_proc.is_kernel_proc = true;

    g_preemptive_active = true;

    // Hard-enable interrupts so PIT IRQ0 starts driving preemption.
    HAL::EnableInterrupts();

    perform_switch(nullptr, first);

    // perform_switch() with prev==nullptr never returns.
    for (;;) HAL::Halt();
}

void Scheduler::SleepMs(uint32_t ms) {
    if (!g_preemptive_active || !current_process || !current_process->is_kernel_proc) {
        // Pre-Start() fallback: HLT until the next IRQ, never busy spin.
        uint64_t target = now_ms() + ms;
        while (now_ms() < target) {
            HAL::WaitForInterrupt();
        }
        return;
    }

    Process* prev = current_process;
    Process* next = nullptr;
    {
        IrqGuard g;
        uint64_t t = now_ms();
        prev->sleep_start_ms = t;
        prev->sleep_until_ms = t + ms;
        prev->state = Process_Sleeping;
        next = pick_next_kernel(prev);
    }

    if (!next) {
        // Nothing else runnable - true HLT idle.  WaitForInterrupt is
        // atomic `sti; hlt`, so any pending wake is not missed.
        while (prev->state == Process_Sleeping) {
            HAL::WaitForInterrupt();
            wake_due_processes();
        }
        prev->state = Process_Running;
        return;
    }

    IrqGuard g;
    perform_switch(prev, next);
}

void Scheduler::YieldNow() {
    if (!g_preemptive_active || !current_process || !current_process->is_kernel_proc) {
        return;
    }
    IrqGuard g;
    Process* prev = current_process;
    Process* next = pick_next_kernel(prev);
    if (!next || next == prev) return;
    perform_switch(prev, next);
}

void Scheduler::ServiceSleepQueue() {
    wake_due_processes();
}

void Scheduler::OnTimerTick(uint32_t ms_elapsed) {
    g_sched_now_ms += ms_elapsed;
    // NOTE: HRTimer::Tick() is intentionally NOT called here. It fires periodic
    // callbacks INLINE, and the only one (proc_refresh -> RuntimeLayout::
    // RefreshProc) writes /proc files via KVFS -> KernelHeap. Running that from
    // IRQ context re-entered KVFS/heap while a process was mid-operation,
    // corrupting node->content/tree state and the heap (the "Free() bad magic"
    // storm). SchedulerProcessEntry calls HRTimer::Tick() every 1ms in PROCESS
    // context instead, so periodic callbacks still fire on time but safely.
    // wake_due_processes() stays (it only flips process states, no heap/KVFS).
    // (satoru)
    wake_due_processes();

    // re-ready descheduled user-thread pollers. poll_try_deschedule parks a
    // blocking poll as Process_Blocked + sleep_ticks and relies on a periodic
    // decrement to wake it, but the decrement in Tick() only runs on the kernel
    // scheduler path; while a user thread is the current process this handler
    // returns early below, so without this the parked poll never re-runs to
    // consume data that arrived while it slept (firefox's wayland proxy hung
    // here waiting on get_registry). wake them so a spinning sibling's
    // poll_try_deschedule retry can switch back into them. (satoru)
    {
        // hold the cross-core scheduler lock for the walk: with smp thread
        // dispatch an ap may be claiming/removing queue nodes at this exact
        // moment (the documented -smp 4 re-ready race). LockIrqSave is safe
        // here (irq context, cli while held, aps hold it only briefly). (satoru)
        uint64_t lf; g_sched_lock.LockIrqSave(&lf);
        for (Process* q = ready_queue; q; q = q->next) {
            if (q->is_user() && q->state == Process_Blocked && q->sleep_ticks > 0) {
                if (q->sleep_ticks > ms_elapsed) {
                    q->sleep_ticks -= ms_elapsed;
                } else {
                    q->sleep_ticks = 0;
                    q->state = Process_Ready;
                    if (q->interactive_score < 16) q->interactive_score += 2;
                }
            }
        }
        g_sched_lock.UnlockIrqRestore(lf);
    }

    Process* p = current_process;
    if (!p || !p->is_kernel_proc) return;

    p->cpu_ms_total += ms_elapsed;

    // CPU-burn decay of interactivity.  A long-running compute task
    // gradually loses its interactive_score, so when an interactive task
    // wakes, the picker prefers it.
    if (p->interactive_score > 0 && (p->cpu_ms_total & 0x7) == 0) {
        p->interactive_score--;
    }

    if (p->timeslice_ms_left <= ms_elapsed) {
        p->timeslice_ms_left = 0;
        g_need_resched = true;
    } else {
        p->timeslice_ms_left -= ms_elapsed;
    }

    // Higher-priority preemption: if a strictly higher tier task is now
    // ready, request a resched at the next safe point.  We can't switch
    // from inside the IRQ stack, but flagging it means the very next
    // SleepMs/YieldNow handles the handoff with single-ms latency.
    for (int i = 0; i < g_kernel_proc_count; i++) {
        Process* cand = g_kernel_procs[i];
        if (!cand || cand == p || cand->reaped) continue;
        if (cand->state != Process_Ready) continue;
        if (cand->prio_tier < p->prio_tier) {
            g_need_resched = true;
            break;
        }
    }
}

bool Scheduler::TryGrowGuardPage(uint64_t cr2) {
    uint64_t fault_page = cr2 & ~(KP_PAGE_SIZE - 1ULL);
    for (int i = 0; i < g_kernel_proc_count; i++) {
        Process* p = g_kernel_procs[i];
        if (!p) continue;
        KernelStackInfo* k = &p->kstack;
        if (fault_page != k->guard_page) continue;

        // Detect cap exhaustion.
        if (k->bytes + KP_PAGE_SIZE > k->cap_bytes) {
            kp_serial_log("[Sched] STACK OVERFLOW (cap reached) in process: ");
            kp_serial_log(p->name);
            kp_serial_log("\r\n");
            KernelPanic::KeBugCheckEx(StopCode::STACK_OVERFLOW_FAULT,
                                       (uint64_t)(uintptr_t)p->name,
                                       cr2,
                                       k->cap_bytes,
                                       0,
                                       "kernel stack overflow",
                                       __FILE__, (uint32_t)__LINE__);
            return false;
        }

        // Allocate new page; the PMM identity-maps it so no MapPage
        // is required (matches alloc_kernel_stack's contract).
        void* page = PMM::AllocBytes(KP_PAGE_SIZE);
        if (!page) {
            kp_serial_log("[Sched] STACK GROW: PMM exhausted for ");
            kp_serial_log(p->name);
            kp_serial_log("\r\n");
            KernelPanic::KeBugCheckEx(StopCode::OUT_OF_MEMORY,
                                       (uint64_t)(uintptr_t)p->name,
                                       cr2, 0, 0,
                                       "kernel stack grow PMM exhausted",
                                       __FILE__, (uint32_t)__LINE__);
            return false;
        }

        // The freshly allocated page becomes the new low; the previous
        // low remains, and the guard page moves down by one page.
        // PMM::AllocBytes returns identity-mapped memory, so for the
        // grow to be contiguous we require the new page to land just
        // below `k->low`.  In practice the buddy allocator does NOT
        // guarantee adjacency, so we instead remap the new physical
        // frame at (k->low - PAGE) using KernelVMM.
        uint64_t new_low = k->low - KP_PAGE_SIZE;
        uint64_t phys    = (uint64_t)(uintptr_t)page;

        // If PMM happened to return adjacent memory we can skip the
        // remap; either way we set up the virt mapping explicitly.
        KernelVMM::MapPage(new_low, phys, PTE_PRESENT | PTE_WRITABLE);
        KernelVMM::InvalidatePage(new_low);

        k->low        = new_low;
        k->bytes     += KP_PAGE_SIZE;
        k->guard_page = new_low - KP_PAGE_SIZE;
        k->grow_count++;

        kp_serial_log("[Sched] stack grew for ");
        kp_serial_log(p->name);
        kp_serial_log("\r\n");
        return true;
    }
    return false;
}

int Scheduler::DumpKernelProcessTable(char* buf, int max_len) {
    if (!buf || max_len <= 0) return 0;
    int p = 0;
    auto put = [&](const char* s) {
        while (*s && p < max_len - 1) buf[p++] = *s++;
    };
    auto put_u32 = [&](uint32_t v) {
        char tmp[16]; int ti = 0;
        if (v == 0) tmp[ti++] = '0';
        while (v) { tmp[ti++] = (char)('0' + (v % 10)); v /= 10; }
        while (ti) { if (p < max_len - 1) buf[p++] = tmp[--ti]; }
    };
    static const char* tier_names[PRIO_TIER_COUNT] = { "RT", "HI", "NM", "LO" };
    for (int i = 0; i < g_kernel_proc_count; i++) {
        Process* k = g_kernel_procs[i];
        if (!k) continue;
        put(k->name); put(" tier="); put(tier_names[prio_tier_for(k)]);
        put(" stack="); put_u32((uint32_t)(k->kstack.bytes / 1024));
        put("/"); put_u32((uint32_t)(k->kstack.cap_bytes / 1024)); put("KB");
        put(" cpu="); put_u32((uint32_t)k->cpu_ms_total); put("ms\n");
    }
    if (p < max_len) buf[p] = 0;
    return p;
}
