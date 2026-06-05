#include "scheduler.h"
#include "spinlock.h"
#include "kernel_locks.h"
#include "../kernel/heap.h"
#include "../kernel/vmm.h"
#include "../kernel/pmm.h"
#include "../kernel/hrtimer.h"
#include "../kernel/panic.h"
#include "../drivers/serial.h"
#include "../hal/hal.h"
#include "../system/logging.h"

// ── Global kernel locks (declared in proc/kernel_locks.h) ──
Spinlock g_net_lock;
Spinlock g_input_lock;
Spinlock g_vfs_lock;
Spinlock g_fb_lock;
Spinlock g_audio_lock;
Spinlock g_log_lock;

// switch_to.asm helpers
extern "C" void scheduler_switch_to(uint64_t* prev_saved_rsp,
                                    uint64_t  next_saved_rsp);
extern "C" [[noreturn]] void scheduler_jump_to(uint64_t saved_rsp);

namespace {
// 8 mb user stack (matches the linux main-thread default) so large static
// binaries like ffmpeg have room; grows down from user_stack_top to base
// 0x3fa00000, still ~506 mb above the mmap arena at 0x20000000. (satoru)
constexpr uint64_t USER_STACK_BYTES = 8 * 1024 * 1024;
constexpr uint64_t KERNEL_STACK_BYTES = 16 * 1024;
constexpr uint64_t USER_STACK_TOP = USERSPACE_BASE + 0x00200000ULL;
constexpr uint64_t USER_MMAP_BASE = 0x20000000ULL;

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

static void init_process_common(Process* proc, const char* name, uint32_t priority) {
    memset(proc, 0, sizeof(Process));
    proc->pid = Scheduler::next_pid++;
    proc->parent_pid = 0;

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

static void enqueue_process(Process* proc) {
    if (!proc) return;
    IrqGuard g;
    // Reject duplicate enqueue  -  caused leaked queue cycles on resume races.
    for (Process* cur = Scheduler::ready_queue; cur; cur = cur->next) {
        if (cur == proc) return;
    }
    proc->next = Scheduler::ready_queue;
    Scheduler::ready_queue = proc;
}

static void remove_from_ready_queue(Process* proc) {
    if (!proc) return;
    IrqGuard g;

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

// Forward declaration  -  definition is in the preemptive-internals block
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
    if (next_pid >= 32) return nullptr; // max processes cap for now
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
    if (next_pid >= 32) return nullptr;

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
    if (!parent || !parent->is_user() || !parent->has_user_frame || next_pid >= 32) {
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
    link_child(parent, proc);
    enqueue_process(proc);
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

void Scheduler::SaveUserFrame(Process* proc, const InterruptFrame* frame) {
    if (!proc || !frame || !proc->is_user()) return;

    proc->user_frame = *frame;
    proc->has_user_frame = true;
    proc->rip = (uintptr_t)frame->rip;
    proc->rsp = (uintptr_t)frame->rsp;
    proc->rbp = (uintptr_t)frame->rbp;
}

bool Scheduler::LoadUserFrame(Process* proc, InterruptFrame* frame) {
    if (!proc || !frame || !proc->is_user() || !proc->has_user_frame) return false;

    current_process = proc;
    proc->state = Process_Running;
    HAL::SetKernelStack(proc->kernel_stack_top);
    KernelVMM::ActivateAddressSpace(proc->address_space);
    *frame = proc->user_frame;
    return true;
}

Process* Scheduler::GetNextRunnableUser(Process* after) {
    if (!ready_queue) return nullptr;

    // CFS-style pick: scan all runnable user processes and select the one
    // with the smallest virtual runtime (so lighter-weight tasks that have
    // been starved get the CPU first).  SCHED_FIFO/RR processes (sched_class
    // 1/2) preempt CFS unconditionally.  Falls back to ready_queue order
    // when nothing has accrued vruntime yet.
    Process* best = nullptr;
    Process* fifo = nullptr;
    for (Process* c = ready_queue; c; c = c->next) {
        if (!c->is_user() || c->state != Process_Ready || !c->has_user_frame) continue;
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
    Process* start = (after && after->next) ? after->next : ready_queue;
    Process* cursor = start;
    while (cursor) {
        if (cursor->is_user() && cursor->state == Process_Ready && cursor->has_user_frame)
            return cursor;
        cursor = cursor->next;
    }
    cursor = ready_queue;
    while (cursor && cursor != start) {
        if (cursor->is_user() && cursor->state == Process_Ready && cursor->has_user_frame)
            return cursor;
        cursor = cursor->next;
    }
    return nullptr;
}

bool Scheduler::ScheduleNextUser(InterruptFrame* frame) {
    Process* next = GetNextRunnableUser(current_process);
    if (!next) return false;

    if (current_process && current_process->state == Process_Running) {
        current_process->state = Process_Ready;
    }

    return LoadUserFrame(next, frame);
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

    if (proc->is_user()) {
        if (proc->address_space) {
            KernelVMM::DestroyAddressSpace(proc->address_space);
        }
        if (proc->kernel_stack_top) {
            PMM::FreeBytes((void*)(uintptr_t)(proc->kernel_stack_top - KERNEL_STACK_BYTES), KERNEL_STACK_BYTES);
        }
    }

    proc->reaped = 1;
    proc->state  = Process_Terminated;
    KernelHeap::Free(proc);
}

void Scheduler::Schedule() {
    IrqGuard g;
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

        for (Process* p = ready_queue; p; p = p->next) {
            if (p->state == Process_Blocked && p->sleep_ticks > 0) {
                if (--p->sleep_ticks == 0) {
                    p->state = Process_Ready;
                    // I/O-wake boost: just-woken tasks get priority.
                    if (p->interactive_score < 16) p->interactive_score += 2;
                }
            }
        }
    }

    // Service hrtimers only when preemptive scheduler isn't already driving
    // them from the PIT IRQ  -  avoids double dispatch.
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
    return current_process;
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
    for (Process* proc = ready_queue; proc && count < max_count; proc = proc->next) {
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
    return g_sched_now_ms;
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

    // Pass 2: relax cgroup throttling  -  would-be-throttled tasks may run
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
// jumps via the asm helper.  Caller must hold IRQs disabled  -  the asm
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
        // Nothing else runnable  -  true HLT idle.  WaitForInterrupt is
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
    HRTimer::Tick();
    wake_due_processes();

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
