#pragma once

#include "types.h"
#include "../proc/scheduler.h"

struct UserspaceReturnContext {
    uint64_t kernel_rsp;
    uint64_t resume_rip;
    int32_t exit_code;
    uint32_t reserved;
} __attribute__((packed));

extern "C" int UserspaceEnter(uint64_t rip, uint64_t rsp, UserspaceReturnContext* ctx);
extern "C" void UserspaceResume(UserspaceReturnContext* ctx, int exit_code);

class Userspace {
public:
    static void Init();
    static bool IsReady();
    static bool IsActive();

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
    static uint64_t kernel_address_space;
    static Process* previous_process;
    static int previous_linux_process;
    static Process* active_process;
    static int active_linux_process;
    static UserspaceReturnContext return_context;
};