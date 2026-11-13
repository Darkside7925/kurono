#include "userspace.h"

#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "../drivers/serial.h"
#include "../hal/hal.h"
#include "../linux/linux_syscall.h"
#include "../drivers/timer.h"   // sched=2 detached wait bounds (satoru)

bool Userspace::initialized = false;
UserspaceCpuState Userspace::cpu_state[SMP_MAX_CPUS] = {};
//  the calling cpu's user-execution context (bsp = index 0). (satoru)
UserspaceCpuState& Userspace::cpu() { return cpu_state[SMP::CpuIndex()]; }

namespace {
constexpr uint64_t USER_DEMO_CODE_ADDR = USERSPACE_BASE + 0x00100000ULL;

static int append_demo_text(uint8_t* page, int pos, const char* text) {
    while (*text) {
        page[pos++] = (uint8_t)*text++;
    }
    return pos;
}
}

void Userspace::Init() {
    initialized = true;
    cpu().kernel_address_space = KernelVMM::GetCurrentAddressSpace();
}

bool Userspace::IsReady() {
    return initialized;
}

bool Userspace::IsActive() {
    return cpu().active_process != nullptr;
}

uint64_t Userspace::SessionIdleRsp(uint32_t cpu) {
    if (cpu >= SMP_MAX_CPUS || !cpu_state[cpu].return_context_valid) return 0;
    uint64_t r = cpu_state[cpu].return_context.kernel_rsp;
    if (!r) return 0;
    return (r - 1024) & ~0xFULL;   // a margin below the saved frame, 16-aligned (satoru)
}

int Userspace::SessionCpuOf(const Process* p) {
    if (!p) return -1;
    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        if (cpu_state[i].active_process == p && cpu_state[i].return_context_valid) return i;
    }
    return -1;
}

// smp thread dispatch: an ap resumes claimed sibling threads directly (no
// RunProcessWithArgs session), so it must mark its per-cpu active process
// explicitly or every IsActive() gate on that cpu stays false and the linux
// syscall layer silently no-ops there. (satoru)
void Userspace::SetActiveForThisCpu(Process* p) {
    cpu().active_process = p;
}

Process* Userspace::ActiveProcess() {
    return cpu().active_process;
}

bool Userspace::MapUserPage(Process* proc, uint64_t virt_addr, const void* data,
                            size_t len, uint64_t flags) {
    if (!proc || !proc->is_user()) return false;

    void* page = PMM::AllocBytes(PAGE_SIZE);
    if (!page) return false;

    if (data && len > 0) {
        memcpy(page, data, len);
    }

    if (!KernelVMM::MapPageInAddressSpace(proc->address_space, virt_addr,
                                          (uint64_t)(uintptr_t)page,
                                          flags | PTE_USER)) {
        PMM::FreeBytes(page, PAGE_SIZE);
        return false;
    }

    return true;
}

Process* Userspace::CreateDemoProcess() {
    Process* proc = Scheduler::CreateUserProcess("userdemo", USER_DEMO_CODE_ADDR, 1);
    if (!proc) return nullptr;

    uint8_t page[PAGE_SIZE];
    memset(page, 0, sizeof(page));

    const char* msg = "[ring3] int 0x80 path is alive\n";
    int msg_len = 0;
    while (msg[msg_len]) msg_len++;

    int p = 0;
    page[p++] = 0xB8; page[p++] = 0x04; page[p++] = 0x00; page[p++] = 0x00; page[p++] = 0x00; // mov eax,4
    page[p++] = 0xBB; page[p++] = 0x01; page[p++] = 0x00; page[p++] = 0x00; page[p++] = 0x00; // mov ebx,1
    page[p++] = 0xB9; // mov ecx, imm32
    uint32_t msg_va = (uint32_t)(USER_DEMO_CODE_ADDR + 0x80);
    memcpy(&page[p], &msg_va, sizeof(msg_va)); p += sizeof(msg_va);
    page[p++] = 0xBA; // mov edx, imm32
    uint32_t msg_count = (uint32_t)msg_len;
    memcpy(&page[p], &msg_count, sizeof(msg_count)); p += sizeof(msg_count);
    page[p++] = 0xCD; page[p++] = 0x80; // int 0x80
    page[p++] = 0xB8; page[p++] = 0x01; page[p++] = 0x00; page[p++] = 0x00; page[p++] = 0x00; // mov eax,1
    page[p++] = 0x31; page[p++] = 0xDB; // xor ebx, ebx
    page[p++] = 0xCD; page[p++] = 0x80; // int 0x80
    page[p++] = 0xEB; page[p++] = 0xFE; // jmp $

    append_demo_text(page, 0x80, msg);

    if (!MapUserPage(proc, USER_DEMO_CODE_ADDR, page, sizeof(page), 0)) {
        Scheduler::DestroyProcess(proc);
        return nullptr;
    }

    return proc;
}

int Userspace::RunProcess(Process* proc) {
    return RunProcessWithArgs(proc, nullptr, nullptr);
}

namespace {

static size_t kstrlen(const char* s) {
    size_t n = 0;
    while (s && s[n]) ++n;
    return n;
}

static size_t kstrcpy(char* dst, const char* src) {
    size_t n = 0;
    while (src && src[n]) { dst[n] = src[n]; ++n; }
    dst[n] = '\0';
    return n;
}

// Build a System-V x86_64 initial process stack.  Layout (high → low):
//
//   string area  : argv[i] strings, envp[i] strings, 16 random bytes,
//                  then a "x86_64\0" string for AT_PLATFORM
//   alignment pad
//   auxv         : AT_PHDR (skipped - caller doesn't always know),
//                  AT_PAGESZ=4096, AT_RANDOM=ptr, AT_PLATFORM=ptr,
//                  AT_UID=0, AT_EUID=0, AT_GID=0, AT_EGID=0,
//                  AT_SECURE=0, AT_NULL=0
//   envp[]+NULL
//   argv[]+NULL
//   argc
//
// All pointers are absolute user-space VAs.  Returns the new RSP that
// should be loaded into the user process on entry.  Returns 0 on error.
//
// IMPORTANT: this function runs in kernel context with the user's
// address space active (caller has activated it).  The user stack is
// identity-mapped via the address-space clone of the kernel PML4, so we
// can write through the user VA directly.
static uint64_t build_initial_stack(uint64_t stack_top,
                                    const char* const* argv,
                                    const char* const* envp,
                                    const Process* proc) {
    // Default to a minimal "argv[0]=program" if caller passed nullptr.
    static const char* default_argv[] = { "program", nullptr };
    if (!argv) argv = default_argv;
    if (!envp) {
        static const char* empty_envp[] = { nullptr };
        envp = empty_envp;
    }

    int argc = 0;
    while (argv[argc]) ++argc;
    int envc = 0;
    while (envp[envc]) ++envc;

    // Total bytes needed for strings.
    size_t str_bytes = 0;
    for (int i = 0; i < argc; ++i) str_bytes += kstrlen(argv[i]) + 1;
    for (int i = 0; i < envc; ++i) str_bytes += kstrlen(envp[i]) + 1;
    const char kPlatform[] = "x86_64";
    str_bytes += sizeof(kPlatform);   // includes NUL
    const size_t kRandomBytes = 16;
    str_bytes += kRandomBytes;

    // We'll lay out from the top down.
    uint64_t sp = stack_top;

    // Reserve string area, 16-aligned.
    sp -= str_bytes;
    sp &= ~(uint64_t)0xF;
    uint64_t str_base = sp;

    // Pack strings into [str_base, str_base + str_bytes).
    char* p = (char*)(uintptr_t)str_base;
    uint64_t* argv_user = (uint64_t*)__builtin_alloca(sizeof(uint64_t) * (argc + 1));
    uint64_t* envp_user = (uint64_t*)__builtin_alloca(sizeof(uint64_t) * (envc + 1));
    for (int i = 0; i < argc; ++i) {
        argv_user[i] = (uint64_t)(uintptr_t)p;
        p += kstrcpy(p, argv[i]) + 1;
    }
    argv_user[argc] = 0;
    for (int i = 0; i < envc; ++i) {
        envp_user[i] = (uint64_t)(uintptr_t)p;
        p += kstrcpy(p, envp[i]) + 1;
    }
    envp_user[envc] = 0;

    // 16 bytes of "randomness" for AT_RANDOM (used by glibc/musl for
    // stack canary + pointer mangling secret).  Use TSC for now.
    uint32_t tsc_lo, tsc_hi;
    asm volatile("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
    uint64_t tsc = ((uint64_t)tsc_hi << 32) | tsc_lo;
    uint8_t* rndp = (uint8_t*)p;
    for (size_t i = 0; i < kRandomBytes; ++i) {
        tsc = tsc * 6364136223846793005ULL + 1442695040888963407ULL;
        rndp[i] = (uint8_t)(tsc >> 33);
    }
    uint64_t random_ptr = (uint64_t)(uintptr_t)rndp;
    p += kRandomBytes;

    // AT_PLATFORM string.
    uint64_t platform_ptr = (uint64_t)(uintptr_t)p;
    for (size_t i = 0; i < sizeof(kPlatform); ++i) p[i] = kPlatform[i];

    // Aux vector: pairs of (uint64 type, uint64 value).
    struct AuxEntry { uint64_t a_type, a_val; };
    static constexpr uint64_t AT_NULL     = 0;
    static constexpr uint64_t AT_PHDR     = 3;
    static constexpr uint64_t AT_PHENT    = 4;
    static constexpr uint64_t AT_PHNUM    = 5;
    static constexpr uint64_t AT_BASE     = 7;
    static constexpr uint64_t AT_ENTRY    = 9;
    static constexpr uint64_t AT_PAGESZ   = 6;
    static constexpr uint64_t AT_PLATFORM = 15;
    static constexpr uint64_t AT_HWCAP    = 16;
    static constexpr uint64_t AT_CLKTCK   = 17;
    static constexpr uint64_t AT_SECURE   = 23;
    static constexpr uint64_t AT_RANDOM   = 25;
    static constexpr uint64_t AT_EXECFN   = 31;
    static constexpr uint64_t AT_UID      = 11;
    static constexpr uint64_t AT_EUID     = 12;
    static constexpr uint64_t AT_GID      = 13;
    static constexpr uint64_t AT_EGID     = 14;

    // at_phdr/phent/phnum/entry let musl locate pt_tls + pt_gnu_relro and the
    // program entry. at_base=0 (static, no dynamic loader). a zero at_phdr is
    // safely ignored by musl (treated as absent). (satoru)
    uint64_t at_phdr  = proc ? proc->user_phdr_va : 0;
    uint64_t at_phent = proc ? proc->user_phent  : 0;
    uint64_t at_phnum = proc ? proc->user_phnum  : 0;
    uint64_t at_entry = proc ? proc->rip         : 0;
    AuxEntry aux[] = {
        { AT_PHDR,     at_phdr },
        { AT_PHENT,    at_phent },
        { AT_PHNUM,    at_phnum },
        { AT_BASE,     0 },
        { AT_ENTRY,    at_entry },
        { AT_PAGESZ,   4096 },
        { AT_CLKTCK,   100 },
        { AT_HWCAP,    0 },
        { AT_UID,      0 },
        { AT_EUID,     0 },
        { AT_GID,      0 },
        { AT_EGID,     0 },
        { AT_SECURE,   0 },
        { AT_RANDOM,   random_ptr },
        { AT_PLATFORM, platform_ptr },
        { AT_EXECFN,   argc > 0 ? argv_user[0] : 0 },
        { AT_NULL,     0 },
    };
    constexpr int kAuxCount = sizeof(aux) / sizeof(aux[0]);

    // Compute the size of [argc][argv...][NULL][envp...][NULL][auxv...]
    // and pre-align so that *after* pushing argc, RSP is 16-aligned (SysV
    // requirement for _start).
    size_t header_qwords = 1                           // argc
                         + (size_t)(argc + 1)          // argv + NULL
                         + (size_t)(envc + 1)          // envp + NULL
                         + (size_t)(kAuxCount * 2);    // auxv pairs
    uint64_t header_bytes = header_qwords * 8;

    // Align: ((sp - header_bytes) & ~0xF) must equal final RSP.
    uint64_t final_rsp = (sp - header_bytes) & ~(uint64_t)0xF;
    sp = final_rsp;

    // Write header in order.
    uint64_t* w = (uint64_t*)(uintptr_t)sp;
    *w++ = (uint64_t)argc;
    for (int i = 0; i <= argc; ++i) *w++ = argv_user[i];
    for (int i = 0; i <= envc; ++i) *w++ = envp_user[i];
    for (int i = 0; i < kAuxCount; ++i) {
        *w++ = aux[i].a_type;
        *w++ = aux[i].a_val;
    }

    return final_rsp;
}

}  // namespace

// ── sched=2 detached launch ─────────────────────────────────────────────────
// the old launcher entered ring 3 ON THE CALLING CPU and only came back when the
// program exited, so the calling kernel proc (the shell, which serves every
// terminal and every desktop launch) was frozen for the program's whole life,
// and ring 3 owned the bsp the kernel services need. under sched=2 a call from a
// kernel proc instead builds the program's first user frame, marks it
// ap-resumable and lets the application processors run it like any other task,
// while the caller waits cooperatively (SleepMs keeps every other kernel proc on
// the bsp running). same contract for the caller: blocks until exit, returns the
// exit code, the caller still owns proc. (satoru)
static Process* g_cancel_owner = nullptr;
static bool (*g_cancel_probe)() = nullptr;
void Userspace::SetCancelProbe(Process* owner, bool (*probe)()) {
    g_cancel_owner = owner;
    g_cancel_probe = probe;
}

static bool detached_eligible() {
    if (!Scheduler::DetachedLaunch() || !SMP::ApThreadSched()) return false;
    // cpu 0, or a service cpu (kurono.svccpu): the launchers and the shell live
    // on the desktop cpu there, and the old in-place launch would run the
    // program on that cpu, under the gui. (satoru)
    if (SMP::OnlineCount() < 2 || !Scheduler::KernelProcCpu(SMP::CpuIndex())) return false;
    if (!Scheduler::IsPreemptiveKernelSchedulerActive()) return false;
    Process* me = Scheduler::GetCurrentProcess();
    return me && me->is_kernel_proc;
}

// true while `p` still has a cpu under it: running somewhere, or its last cpu
// has not yet left its kernel stack (on_cpu, published by FinishSwitch). (satoru)
static bool task_on_some_cpu(Process* p) {
    if (__atomic_load_n(&p->on_cpu, __ATOMIC_ACQUIRE)) return true;
    for (uint32_t ci = 0; ci < SMP::CpuCount(); ci++) {
        PerCpu* pc = SMP::ByIndex(ci);
        if (pc && pc->current == p) return true;
    }
    return false;
}

// async ring-3 spawn (phase3 finding 1). builds the program's linux record and first
// user frame and hands it to the application processors, then returns at once: no
// launcher frame, no per-cpu return context, nothing for a second runner (firefox's
// cmd_firefox) to clobber, and the caller is free. sched=2: any ap may run it, as for
// RunDetached. old mode: the leader and the threads it creates are pinned to one ap
// (the highest online one) and never run on the bsp, so the daemon can neither starve
// the cooperative kernel procs there nor busy-wait in their place (findings 2/3).
// returns 0 and fills *h, or <0. (satoru)
static int spawn_ap_cpu() {
    // the highest ap that is really online: a contained-fault ap parks itself and
    // lowers OnlineCount, so count-1 can name a dead cpu (review). (satoru)
    uint32_t n = SMP::CpuCount();
    if (n > 8) n = 8;   // cpu_affinity is 8 bits (satoru)
    for (int c = (int)n - 1; c >= 1; c--) {
        PerCpu* pc = SMP::ByIndex((uint32_t)c);
        if (pc && pc->online) return c;
    }
    return -1;
}

bool Userspace::CanSpawnDetached() {
    if (!SMP::ApThreadSched() || SMP::OnlineCount() < 2) return false;
    return Scheduler::SchedV2() || spawn_ap_cpu() > 0;
}

int Userspace::SpawnDetached(Process* proc, const char* const* argv, const char* const* envp,
                             uint32_t uid, uint32_t gid, DetachedHandle* h) {
    if (!initialized) Init();
    if (!proc || !proc->is_user() || !h) return -1;
    if (!CanSpawnDetached()) return -2;
    int pin = Scheduler::SchedV2() ? -1 : spawn_ap_cpu();
    LinuxSyscall::LayerLock();
    int slot = LinuxSyscall::CreateProcess(proc->name, uid, gid);
    LinuxProcess* lp = (slot >= 0) ? LinuxSyscall::GetProcess(slot) : nullptr;
    uint32_t lpid = 0;
    if (lp) {
        lp->task = proc;
        lpid = lp->pid;
        // same high brk window as the bsp launcher below (see its comment). (satoru)
        constexpr uint64_t HIGH_BRK_BASE = 0x0000180000000000ULL;
        constexpr uint64_t HIGH_BRK_SIZE = 0x0000000010000000ULL;
        lp->brk_base    = HIGH_BRK_BASE;
        lp->brk_current = HIGH_BRK_BASE;
        lp->brk_max     = HIGH_BRK_BASE + HIGH_BRK_SIZE;
    }
    LinuxSyscall::LayerUnlock();
    if (!lp) return -3;
    LinuxSyscall::SetProcessCmdline(slot, argv);   // /proc/<pid>/cmdline (satoru)
    h->proc = proc; h->slot = slot; h->lpid = lpid; h->as = proc->address_space; h->kpid = proc->pid;

    // build the sysv stack with the program's address space active on this
    // cpu (the writes go through its user va), then return to ours. a dynamic
    // pie came with its stack already built by ld-kurono. (satoru)
    uint64_t entry_rsp;
    {
        uint64_t kas = KernelVMM::GetCurrentAddressSpace();
        KernelVMM::ActivateAddressSpace(proc->address_space);
        if (proc->flags & PROCESS_FLAG_STACK_READY) {
            entry_rsp = proc->rsp;
        } else {
            entry_rsp = build_initial_stack(proc->user_stack_top, argv, envp, proc);
            if (!entry_rsp) entry_rsp = proc->user_stack_top;
        }
        KernelVMM::ActivateAddressSpace(kas);
    }

    // the first user frame: every gpr zero (the sysv entry contract, and glibc
    // reads rdx as rtld_fini), user selectors, IF on. fs base and the fxsave
    // image are already in proc (ld-kurono's install_main_tls / the seeded
    // default) and LoadUserFrame installs them. (satoru)
    memset(&proc->user_frame, 0, sizeof(proc->user_frame));
    proc->user_frame.rip    = proc->rip;
    proc->user_frame.rsp    = entry_rsp;
    proc->user_frame.cs     = (uint64_t)(GDT_USER_CODE_SELECTOR | 3);
    proc->user_frame.ss     = (uint64_t)(GDT_USER_DATA_SELECTOR | 3);
    proc->user_frame.rflags = 0x202ULL;
    proc->rsp = entry_rsp;
    proc->has_user_frame = true;
    proc->on_cpu = 0;
    if (pin > 0) {
        // old mode: one ap only. migrating between aps is the old mode's unsafe hand-off,
        // and the bsp must stay with the kernel procs. (satoru)
        proc->cpu_affinity = (uint8_t)(1u << pin);
        proc->last_run_cpu = (uint8_t)pin;
    }
    proc->state = Process_Ready;
    Scheduler::MarkApResumable(proc);   // from here an ap may claim it (satoru)
    Scheduler::KickIdleCpu();

    SerialLogger::Log(Scheduler::SchedV2() ? "[sched2] spawned pid=" : "[spawn] detached pid=");
    SerialLogger::LogDec((int)lpid);
    SerialLogger::Log(" ");
    SerialLogger::Log(proc->name);
    if (pin > 0) { SerialLogger::Log(" on cpu"); SerialLogger::LogDec(pin); }
    else SerialLogger::Log(" on the aps");
    SerialLogger::Log("\r\n");
    return 0;
}

bool Userspace::DetachedAlive(DetachedHandle& h) {
    // by pid under the scheduler lock: a group kill can reap the leader and the
    // free-later ring free it while a slow supervisor still holds h (review). the
    // exit code is copied the first time the leader is seen Terminated. (satoru)
    int st = 0, code = -1;
    if (!h.proc || !Scheduler::TaskStateByPid(h.kpid, h.proc, &st, &code)) return false;
    if (st == (int)Process_Terminated) {
        if (!h.exit_known) { h.exit_code = code; h.exit_known = true; }
        return false;
    }
    return true;
}

int Userspace::RunDetached(Process* proc, const char* const* argv,
                           const char* const* envp, uint32_t uid, uint32_t gid) {
    Process* waiter = Scheduler::GetCurrentProcess();
    DetachedHandle h;
    int sr = SpawnDetached(proc, argv, envp, uid, gid, &h);
    if (sr < 0) return sr == -3 ? -3 : -1;

    // wait for the leader, cooperatively. ctrl+c from the owning terminal kills
    // the whole group. (satoru)
    bool cancelled = false;
    while (DetachedAlive(h)) {
        if (!cancelled && g_cancel_probe && (waiter == g_cancel_owner || !g_cancel_owner) && g_cancel_probe()) {
            cancelled = true;
            SerialLogger::Log("[sched2] cancel: killing the program's group\r\n");
            Scheduler::KillProcessGroupAsync(h.kpid, nullptr);
        }
        Scheduler::SleepMs(5);
    }
    // a leader reaped within one 5 ms poll was never seen Terminated: read its code
    // the old way, the free-later ring keeps the struct >= 2 s (and the caller owns
    // proc). only this 5 ms waiter may do that, not a slow supervisor. (satoru)
    if (!h.exit_known) { h.exit_code = proc->exit_code; h.exit_known = true; }
    return ReapDetached(h);
}

// the leader is dead (DetachedAlive false), or the caller gave up on it: kill what is
// left of the group, wait for it to drain, release the linux record. callable from a
// kernel proc only (it sleeps). returns the leader's exit code. (satoru)
int Userspace::ReapDetached(DetachedHandle& h) {
    Process* proc = h.proc;
    if (!proc) return -1;
    int slot = h.slot;
    uint32_t lpid = h.lpid;
    if (DetachedAlive(h)) Scheduler::KillProcessGroupAsync(h.kpid, nullptr);
    {
        uint64_t t0w = Timer::GetRealMs64();
        while (DetachedAlive(h) && Timer::GetRealMs64() - t0w < 3000) Scheduler::SleepMs(5);
    }
    // the struct is only touched while it is still in the task list (then the
    // free-later ring keeps it for >= 2 s); a reaped leader is just "gone". (satoru)
    int exit_code = h.exit_known ? h.exit_code : -1;
    if (!Scheduler::TaskStateByPid(h.kpid, proc, nullptr, h.exit_known ? nullptr : &exit_code)) proc = nullptr;

    // the leader is dead. give siblings that did not go down with it (a leader
    // that ended with a plain exit, e.g. firefox's exit_group exemption) a
    // moment, then kill what is left so nothing keeps running in the address
    // space the caller is about to release. (satoru)
    uint64_t as = h.as;
    uint64_t t0 = Timer::GetRealMs64();
    bool killed_rest = false;
    for (;;) {
        uint32_t live = Scheduler::FindLivePidInAddressSpace(as);
        // old mode: an exited task's on_cpu stays 1, so "current on no cpu" is the proof. (satoru)
        if (proc && !Scheduler::TaskStateByPid(h.kpid, proc, nullptr, nullptr)) proc = nullptr;
        bool on = proc && (Scheduler::SchedV2() ? task_on_some_cpu(proc) : Scheduler::IsCurrentAnywhere(proc));
        bool busy = on || (as && Scheduler::AddressSpaceLiveElsewhere(as, proc));
        if (!live && !busy) break;
        uint64_t el = Timer::GetRealMs64() - t0;
        if (live && !killed_rest && el > 300) {
            killed_rest = true;
            Scheduler::KillProcessGroupAsync(live, nullptr);
        }
        if (el > 3000) {
            SerialLogger::Log("[sched2] group did not drain in 3s, leaving it to the reaper\r\n");
            break;
        }
        Scheduler::SleepMs(5);
    }

    // release the linux record exactly once: the task manager may already have
    // destroyed (and an exec reused) this slot, so revalidate by pid. (satoru)
    LinuxSyscall::LayerLock();
    // members killed with the group never ran sys_exit, so their records would
    // stay active and leak LINUX_MAX_PROCS slots (fix agent's note). (satoru)
    LinuxSyscall::ReleaseDeadGroupRecords(h.proc, as);   // compared, never dereferenced (satoru)
    LinuxProcess* lp2 = LinuxSyscall::GetProcess(slot);
    if (lp2 && lp2->pid == lpid && (lp2->task == h.proc || lp2->task == nullptr))
        LinuxSyscall::DestroyProcess(slot);
    LinuxSyscall::LayerUnlock();

    // a leader that ended with a normal exit is Terminated but never reaped:
    // MarkProcessExited took it off the task list, its parent is the kernel
    // runner that loaded it (this caller), which never waitpid()s, and the group
    // drain above only kills what is still live. its address space (~2100 frames
    // per churn/audiod run) stayed allocated forever. reap it like a waitpid
    // would: WaitForChild finds it under the tree lock only while it is still an
    // unreaped child (so the pointer is live), DestroyProcess claims it
    // atomically (a concurrent reaper wins harmlessly), refuses to free page
    // tables still live elsewhere, and parks the struct in the free-later ring.
    // a caller that is not the leader's parent finds nothing and changes nothing. (satoru)
    {
        Process* self = Scheduler::GetCurrentProcess();
        Process* z = self ? Scheduler::WaitForChild(self, h.kpid, nullptr) : nullptr;
        if (z && z == h.proc) Scheduler::ReapProcess(z);
    }

    SerialLogger::Log("[sched2] pid=");
    SerialLogger::LogDec((int)lpid);
    SerialLogger::Log(" exited code=");
    SerialLogger::LogDec(exit_code);
    {
        uint64_t fr = 0, lk = 0; int pe = 0;
        Scheduler::ReapStats(&fr, &pe, &lk);
        SerialLogger::Log(" | reaped="); SerialLogger::LogDec((int)fr);
        SerialLogger::Log(" pending="); SerialLogger::LogDec(pe);
        SerialLogger::Log(" leaked="); SerialLogger::LogDec((int)lk);
        SerialLogger::Log(" heap_kb="); SerialLogger::LogDec((int)(KernelHeap::GetUsed() / 1024));
        SerialLogger::Log(" pmm_free_mb="); SerialLogger::LogDec((int)(PMM::GetFreeMemory() / (1024 * 1024)));
    }
    SerialLogger::Log("\r\n");
    h.proc = nullptr;
    return exit_code;
}

int Userspace::RunProcessWithArgs(Process* proc, const char* const* argv,
                                  const char* const* envp, uint32_t uid, uint32_t gid) {
    if (!initialized) Init();
    if (!proc || !proc->is_user()) return -1;
    if (detached_eligible()) return RunDetached(proc, argv, envp, uid, gid);
    // per-cpu context: this cpu runs its own active user process. another core
    // can be running a different one at the same time. (satoru)
    UserspaceCpuState& u = cpu();
    if (u.active_process) return -2;

    u.active_process = proc;
    u.previous_process = Scheduler::GetCurrentProcess();
    u.kernel_address_space = KernelVMM::GetCurrentAddressSpace();
    u.previous_linux_process = LinuxSyscall::GetCurrentIndex();
    u.active_linux_process = LinuxSyscall::CreateProcess(proc->name, uid, gid);
    if (u.active_linux_process < 0) {
        u.active_process = nullptr;
        u.previous_process = nullptr;
        u.previous_linux_process = -1;
        return -3;
    }

    LinuxProcess* linux_proc = LinuxSyscall::GetProcess(u.active_linux_process);
    if (linux_proc) {
        linux_proc->task = proc;
        // relocate the brk heap above all identity-mapped physical ram. the
        // default brk window (0x08100000..0x0C000000) ALIASES low physical
        // addresses - the framebuffer, kernel heap, and pmm frames all live
        // there - and the kernel reaches every physical frame through the low
        // identity map (phys==virt). when a user brk page is mapped at e.g. va
        // 0x83f9000 and later released, it clears that frame's identity leaf in
        // the process's private page tables, so the kernel's next identity write
        // to that physical frame (pmm zero-on-alloc) #pf'd. firefox's heap grew
        // into that range and crashed the kernel. park brk at 24tb - above ram,
        // clear of the mmap arena (16tb) and ld-kurono's aslr region (64tb+). we
        // set it here, in an allowed file, via the public accessor - the default
        // is assigned in linux_syscall (a separately-owned file). (satoru)
        constexpr uint64_t HIGH_BRK_BASE = 0x0000180000000000ULL;
        constexpr uint64_t HIGH_BRK_SIZE = 0x0000000010000000ULL;  // 256mb window
        linux_proc->brk_base    = HIGH_BRK_BASE;
        linux_proc->brk_current = HIGH_BRK_BASE;
        linux_proc->brk_max     = HIGH_BRK_BASE + HIGH_BRK_SIZE;
    }

    LinuxSyscall::SetProcessCmdline(u.active_linux_process, argv);   // /proc/<pid>/cmdline (satoru)
    LinuxSyscall::SetCurrent(u.active_linux_process);

    Scheduler::SetCurrentForThisCpu(proc);
    proc->state = Process_Running;
    HAL::SetKernelStack(proc->kernel_stack_top);
    KernelVMM::ActivateAddressSpace(proc->address_space);

    // Build SysV initial stack with argc/argv/envp/auxv.  Must happen
    // *after* address-space activation so writes land in the user's
    // physical pages.  a dynamic pie is the exception: ld-kurono already
    // built its stack (with the complete auxv: at_phdr/at_base/at_entry of the
    // loaded image) at proc->rsp, so enter there as-is - rebuilding would drop
    // that auxv and break musl's tls/dynamic startup. (satoru)
    uint64_t entry_rsp;
    if (proc->flags & PROCESS_FLAG_STACK_READY) {
        entry_rsp = proc->rsp;
    } else {
        entry_rsp = build_initial_stack(proc->user_stack_top, argv, envp, proc);
        if (!entry_rsp) entry_rsp = proc->user_stack_top;
    }

    // enter ring-3. the tls thread pointer (fs base) is programmed INSIDE
    // UserspaceEnter - after it reloads the fs selector (which on x86-64 zeroes
    // fsbase) and right before iretq. a dynamic pie set proc->fs_base via
    // ld-kurono's install_main_tls; static binaries pass 0 and set their own fs
    // later via arch_prctl. resume (preempt) reprograms fs via LoadUserFrame. (satoru)
    // mark the launcher return context LIVE across UserspaceEnter: a fault-exit
    // on THIS cpu may UserspaceResume back to here. cleared right after so a
    // later fault on this same cpu (now running a dispatched thread with no
    // launcher frame) can't longjmp through a stale context. (satoru)
    u.return_context_valid = true;
    int exit_code = UserspaceEnter(proc->rip, entry_rsp, &u.return_context, proc->fs_base);
    u.return_context_valid = false;
    // sched=2: we are back on the launcher's stack, so the leader's frame is
    // done with - publish on_cpu=0, or DestroyProcess would treat the dead
    // leader as still live and leak it. (satoru)
    if (Scheduler::SchedV2()) {
        Scheduler::ReleaseOnCpu(proc);
        Scheduler::FinishSwitch();
    }

    KernelVMM::ActivateAddressSpace(u.kernel_address_space);
    if (u.previous_process && u.previous_process->is_user()) {
        HAL::SetKernelStack(u.previous_process->kernel_stack_top);
    }

    LinuxSyscall::DestroyProcess(u.active_linux_process);
    LinuxSyscall::SetCurrent(u.previous_linux_process);
    Scheduler::SetCurrentForThisCpu(u.previous_process);
    u.previous_process = nullptr;
    u.previous_linux_process = -1;
    u.active_linux_process = -1;
    u.active_process = nullptr;

    return exit_code;
}

void Userspace::HandleProcessExit(int exit_code) {
    UserspaceCpuState& u = cpu();
    if (!u.active_process) return;

    // ONLY longjmp back to the launcher if this cpu actually HAS a live launcher
    // frame. an AP running a dispatched thread (ap_enter_user_frame) set
    // active_process but never ran RunProcessWithArgs, so return_context is
    // ZERO - UserspaceResume on it restored rip=0/rsp=0 and, already inside the
    // #PF handler, that second null-jump faulted into a #DF (rip=0 rsp=0), the
    // ap firefox-crash kernel panic. return instead so the caller (hal.cpp ap
    // containment) marks the group + parks this cpu. (satoru)
    if (!u.return_context_valid) {
        Scheduler::MarkProcessExited(u.active_process, exit_code);
        return;
    }

    Scheduler::MarkProcessExited(u.active_process, exit_code);
    KernelVMM::ActivateAddressSpace(u.kernel_address_space);
    UserspaceResume(&u.return_context, exit_code);
    __builtin_unreachable();
}