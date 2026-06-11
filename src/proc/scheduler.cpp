#include "scheduler.h"
#include "../kernel/heap.h"
#include "../hal/hal.h"

Process* Scheduler::current_process = nullptr;
Process* Scheduler::ready_queue = nullptr;
uint32_t Scheduler::next_pid = 1;

void Scheduler::Init() {
    // Create "Idle" process (PID 0)
    // In a real OS, we would set up the stack for the current execution flow
    // as the first process.
}

Process* Scheduler::CreateProcess(const char* name, void (*entry_point)(), uint32_t priority) {
    (void)entry_point;
    if (next_pid >= 32) return nullptr; // Max processes cap for now
    Process* proc = (Process*)KernelHeap::Alloc(sizeof(Process));
    proc->pid = next_pid++;
    
    int i = 0; while(name[i] && i<31) { proc->name[i] = name[i]; i++; } proc->name[i] = 0;
    
    proc->state = Process_Ready;
    proc->priority = priority;
    proc->sleep_ticks = 0;
    
    // Stack setup would happen here (allocating stack page, pushing initial context)
    // proc->esp = ...
    
    // Add to queue
    proc->next = ready_queue;
    ready_queue = proc;
    
    return proc;
}

void Scheduler::Schedule() {
    if (!ready_queue) return;
    
    // Process* next = ready_queue; // Very simple FIFO
    
    // Round Robin
    if (current_process) {
        if (current_process->state == Process_Running) {
            current_process->state = Process_Ready;
        }
        // Move current to end of list? For now just pick next
        current_process = current_process->next;
        if (!current_process) current_process = ready_queue;
    } else {
        current_process = ready_queue;
    }
    
    if (current_process) {
        current_process->state = Process_Running;
        // Context Switch would happen here
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
    // Decrement sleep counters
    Process* p = ready_queue;
    while (p) {
        if (p->state == Process_Blocked && p->sleep_ticks > 0) {
            p->sleep_ticks--;
            if (p->sleep_ticks == 0) p->state = Process_Ready;
        }
        p = p->next;
    }
    
    // Preemption logic
    if (current_process) {
        // if (timeslice_expired) Schedule();
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

void Scheduler::Exit() {
    if (current_process) {
        current_process->state = Process_Terminated;
        Schedule();
    }
}
