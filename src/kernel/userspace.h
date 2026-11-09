#pragma once

#include "types.h"
#include "../proc/scheduler.h"
#include "../proc/smp.h"        // SMP_MAX_CPUS - userspace state is per-cpu now (satoru)

struct UserspaceReturnContext {
    uint64_t kernel_rsp;
    uint64_t resume_rip;
    int32_t exit_code;
    uint32_t reserved;
    // this cpu's PER-CPU gs base, captured in UserspaceEnter before the gs
    // SELECTOR load zeroes gsbase. loading a segment selector on x86-64 resets
    // that segment's base to the descriptor base (0) - already handled for fs,
    // but gs was missed, and .resume_from_user zeroed it again on the way back.
    // the result: after ONE process exited, the kernel ran with no per-cpu gs,
    // so the NEXT process's first syscall swapgs'd to 0 and syscall_entry_x64
    // read its kernel rsp out of low memory (rsp=0xF000FF53F000E2C3, bios rom)
    // -> #DF. offset 24; the asm hardcodes it. (satoru)
    uint64_t percpu_gs_base;
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
    // true ONLY while this cpu is inside a RunProcessWithArgs launcher frame
    // (return_context holds a live UserspaceEnter save point). an AP that
    // RESUMES a dispatched thread via ap_enter_user_frame sets active_process
    // but NEVER inits return_context, so a fault-exit that blindly
    // UserspaceResume'd it longjmp'd through a zero context -> rip=0/rsp=0 ->
    // #DF (the ap firefox-crash panic). HandleProcessExit checks this. (satoru)
    bool     return_context_valid;
};

extern "C" int UserspaceEnter(uint64_t rip, uint64_t rsp, UserspaceReturnContext* ctx,
                              uint64_t fs_base);
extern "C" void UserspaceResume(UserspaceReturnContext* ctx, int exit_code);

class Userspace {
public:
    static void Init();
    static bool IsReady();
    static bool IsActive();
    // which cpu holds a live RunProcessWithArgs launcher frame for p, or -1.
    // the sched=2 partition keeps such a leader on that cpu. (satoru)
    static int SessionCpuOf(const Process* p);
    // the rsp an ap's idle loop may use when this cpu holds a live
    // RunProcessWithArgs launcher frame (the kurono.apsched path): just below
    // that frame, so the loop never overwrites it. 0 = no session here. (satoru)
    static uint64_t SessionIdleRsp(uint32_t cpu);
    // sched=2: the waiting launcher (a kernel proc) asks this probe whether the
    // user cancelled (ctrl+c in the terminal) and kills the program if so. only
    // the registered owner proc is cancelled - a desktop launcher is not. owner
    // nullptr = any waiter; the probe itself answers per caller (the terminal
    // workers' per-context cancel flag). (satoru)
    static void SetCancelProbe(Process* owner, bool (*probe)());
    // smp thread dispatch: mark a directly-resumed thread as this cpu's active
    // user process. an ap that irets into a claimed sibling thread never goes
    // through RunProcessWithArgs, and IsActive() (per-cpu) gates the syscall
    // entry's frame save + linux-current sync AND sys_exit's switch path - all
    // of which silently no-op'd on the aps without this. (satoru)
    static void SetActiveForThisCpu(Process* p);
    // this cpu's userspace session root (the process RunProcessWithArgs entered),
    // or nullptr. the task manager must not quiesce + reap that group from inside
    // the session; it ends it through the signal path instead. (satoru)
    static Process* ActiveProcess();

    static Process* CreateDemoProcess();
    static int RunProcess(Process* proc);
    // Same as RunProcess but pushes argc/argv/envp/auxv onto the user
    // stack before entering ring 3.  argv and envp are NULL-terminated
    // arrays of C strings (envp may be nullptr to mean empty env).
    // uid/gid default to 0 (kernel-internal runners and test programs). the
    // app launch paths pass 1000 so ordinary ring-3 programs are normal users
    // and go through the admin gate for privileged operations. (satoru)
    static int RunProcessWithArgs(Process* proc, const char* const* argv,
                                  const char* const* envp,
                                  uint32_t uid = 0, uint32_t gid = 0);
    static void HandleProcessExit(int exit_code);

private:
    // sched=2 detached launch (see RunProcessWithArgs). (satoru)
    static int RunDetached(Process* proc, const char* const* argv,
                           const char* const* envp, uint32_t uid, uint32_t gid);
    static bool MapUserPage(Process* proc, uint64_t virt_addr, const void* data,
                            size_t len, uint64_t flags);

    static bool initialized;
    // one user-execution context per cpu; cpu() returns the caller's. (satoru)
    static UserspaceCpuState cpu_state[SMP_MAX_CPUS];
    static UserspaceCpuState& cpu();
};