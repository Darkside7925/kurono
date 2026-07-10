#pragma once

#include "types.h"
#include "../proc/scheduler.h"
#include "../proc/smp.h"        // SMP_MAX_CPUS - userspace state is per-cpu now (satoru)

struct UserspaceReturnContext {
    uint64_t kernel_rsp;
    uint64_t resume_rip;
    int32_t exit_code;
    uint32_t reserved;
} __attribute__((packed));

// per-cpu user-execution context. smp phase 3d: each cpu (the bsp and every
// application processor) runs its OWN active user process with its OWN return
// context, so an exit/fault longjmps back to THAT cpu's RunProcessWithArgs frame
// rather than a single shared one. on the bsp this is just index 0, identical to
// the old single-active-process behaviour. (satoru)
struct UserspaceCpuState {
    uint64_t kernel_address_space;
    Process* previous_process;
    int      previous_linux_process;
    Process* active_process;
    int      active_linux_process;
    UserspaceReturnContext return_context;
};

extern "C" int UserspaceEnter(uint64_t rip, uint64_t rsp, UserspaceReturnContext* ctx,
                              uint64_t fs_base);
extern "C" void UserspaceResume(UserspaceReturnContext* ctx, int exit_code);

class Userspace {
public:
    static void Init();
    static bool IsReady();
    static bool IsActive();
    // smp thread dispatch: mark a directly-resumed thread as this cpu's active
    // user process. an ap that irets into a claimed sibling thread never goes
    // through RunProcessWithArgs, and IsActive() (per-cpu) gates the syscall
    // entry's frame save + linux-current sync AND sys_exit's switch path - all
    // of which silently no-op'd on the aps without this. (satoru)
    static void SetActiveForThisCpu(Process* p);

    static Process* CreateDemoProcess();
    static int RunProcess(Process* proc);
    // Same as RunProcess but pushes argc/argv/envp/auxv onto the user
    // stack before entering ring 3.  argv and envp are NULL-terminated
    // arrays of C strings (envp may be nullptr to mean empty env).
    static int RunProcessWithArgs(Process* proc, const char* const* argv,
                                  const char* const* envp);
    static void HandleProcessExit(int exit_code);

private:
    static bool MapUserPage(Process* proc, uint64_t virt_addr, const void* data,
                            size_t len, uint64_t flags);

    static bool initialized;
    // one user-execution context per cpu; cpu() returns the caller's. (satoru)
    static UserspaceCpuState cpu_state[SMP_MAX_CPUS];
    static UserspaceCpuState& cpu();
};