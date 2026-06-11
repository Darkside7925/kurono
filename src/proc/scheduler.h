#pragma once
#include "../kernel/types.h"

// Process Scheduler
// Manages execution of tasks, priority queues, and context switching.

enum ProcessState {
    Process_Ready,
    Process_Running,
    Process_Blocked,
    Process_Terminated
};

struct Process {
    uint32_t pid;
    char name[32];
    ProcessState state;
    uint32_t priority; // 0 = High, 255 = Low
    uintptr_t rsp;     // Stack pointer (64-bit in long mode)
    uintptr_t rbp;     // Base pointer
    uintptr_t rip;     // Instruction pointer (for resume)
    uint32_t sleep_ticks;
    
    Process* next;
};

class Scheduler {
public:
    static Process* current_process;
    static Process* ready_queue;
    static uint32_t next_pid;
    
    static void Init();
    static Process* CreateProcess(const char* name, void (*entry_point)(), uint32_t priority);
    static void Schedule();
    static void Yield();
    static void Sleep(uint32_t ticks);
    static void Exit();
    static void Tick(); // Called by timer interrupt
    
    // Performance monitoring
    static uint32_t GetProcessCount();
    static const char* GetCurrentProcessName();
};
