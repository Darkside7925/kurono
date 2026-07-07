#include "userspace.h"

#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "../drivers/serial.h"
#include "../hal/hal.h"
#include "../linux/linux_syscall.h"

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

// smp thread dispatch: an ap resumes claimed sibling threads directly (no
// RunProcessWithArgs session), so it must mark its per-cpu active process
// explicitly or every IsActive() gate on that cpu stays false and the linux
// syscall layer silently no-ops there. (satoru)
void Userspace::SetActiveForThisCpu(Process* p) {
    cpu().active_process = p;
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
//   auxv         : AT_PHDR (skipped  -  caller doesn't always know),
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

int Userspace::RunProcessWithArgs(Process* proc, const char* const* argv,
                                  const char* const* envp) {
    if (!initialized) Init();
    if (!proc || !proc->is_user()) return -1;
    // per-cpu context: this cpu runs its own active user process. another core
    // can be running a different one at the same time. (satoru)
    UserspaceCpuState& u = cpu();
    if (u.active_process) return -2;

    u.active_process = proc;
    u.previous_process = Scheduler::GetCurrentProcess();
    u.kernel_address_space = KernelVMM::GetCurrentAddressSpace();
    u.previous_linux_process = LinuxSyscall::GetCurrentIndex();
    u.active_linux_process = LinuxSyscall::CreateProcess(proc->name, 0, 0);
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
        // addresses  -  the framebuffer, kernel heap, and pmm frames all live
        // there  -  and the kernel reaches every physical frame through the low
        // identity map (phys==virt). when a user brk page is mapped at e.g. va
        // 0x83f9000 and later released, it clears that frame's identity leaf in
        // the process's private page tables, so the kernel's next identity write
        // to that physical frame (pmm zero-on-alloc) #pf'd. firefox's heap grew
        // into that range and crashed the kernel. park brk at 24tb  -  above ram,
        // clear of the mmap arena (16tb) and ld-kurono's aslr region (64tb+). we
        // set it here, in an allowed file, via the public accessor  -  the default
        // is assigned in linux_syscall (a separately-owned file). (satoru)
        constexpr uint64_t HIGH_BRK_BASE = 0x0000180000000000ULL;
        constexpr uint64_t HIGH_BRK_SIZE = 0x0000000010000000ULL;  // 256mb window
        linux_proc->brk_base    = HIGH_BRK_BASE;
        linux_proc->brk_current = HIGH_BRK_BASE;
        linux_proc->brk_max     = HIGH_BRK_BASE + HIGH_BRK_SIZE;
    }

    LinuxSyscall::SetCurrent(u.active_linux_process);

    Scheduler::SetCurrentForThisCpu(proc);
    proc->state = Process_Running;
    HAL::SetKernelStack(proc->kernel_stack_top);
    KernelVMM::ActivateAddressSpace(proc->address_space);

    // Build SysV initial stack with argc/argv/envp/auxv.  Must happen
    // *after* address-space activation so writes land in the user's
    // physical pages.  a dynamic pie is the exception: ld-kurono already
    // built its stack (with the complete auxv: at_phdr/at_base/at_entry of the
    // loaded image) at proc->rsp, so enter there as-is  -  rebuilding would drop
    // that auxv and break musl's tls/dynamic startup. (satoru)
    uint64_t entry_rsp;
    if (proc->flags & PROCESS_FLAG_STACK_READY) {
        entry_rsp = proc->rsp;
    } else {
        entry_rsp = build_initial_stack(proc->user_stack_top, argv, envp, proc);
        if (!entry_rsp) entry_rsp = proc->user_stack_top;
    }

    // enter ring-3. the tls thread pointer (fs base) is programmed INSIDE
    // UserspaceEnter  -  after it reloads the fs selector (which on x86-64 zeroes
    // fsbase) and right before iretq. a dynamic pie set proc->fs_base via
    // ld-kurono's install_main_tls; static binaries pass 0 and set their own fs
    // later via arch_prctl. resume (preempt) reprograms fs via LoadUserFrame. (satoru)
    int exit_code = UserspaceEnter(proc->rip, entry_rsp, &u.return_context, proc->fs_base);

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

    Scheduler::MarkProcessExited(u.active_process, exit_code);
    KernelVMM::ActivateAddressSpace(u.kernel_address_space);
    UserspaceResume(&u.return_context, exit_code);
    __builtin_unreachable();
}