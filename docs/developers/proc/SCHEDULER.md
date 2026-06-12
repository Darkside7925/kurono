# Scheduler

`src/proc/scheduler.cpp` and `scheduler.h` implement the Kurono process scheduler.

## 1. What it does

The scheduler maintains the list of runnable tasks and decides which one runs each time it is ticked. It is called from the main loop and from the timer interrupt callback.

## 2. Current model

The current scheduler is a cooperative/time-slice hybrid. Processes yield explicitly or are preempted after a timer quantum. Priority values (stored in the `TMProcess` struct) influence the time quantum allocation  -  higher priority processes run more often.

Because Kurono does not use hardware context switching (no separate user-space rings with full register save/restore), the "processes" are actually OS-managed tasks that share the kernel stack. Full preemptive user-space processes would require TSS setup and syscall infrastructure.

## 3. Process table

The scheduler owns the canonical process table. The Task Manager app reads from this table for its display. Each entry includes:

- PID (unique integer)
- Name (32-char string)
- CPU usage percentage
- Memory usage (KB)
- Thread count
- State (RUNNING, SLEEPING, STOPPED, ZOMBIE)
- Priority
- I/O counters

## 4. Tick

`Scheduler::Tick()` is called from the main loop each iteration. It advances time-slice counters and runs context switches.

## 5. Multi-core (SMP)

All logical CPUs are brought up at boot (`src/proc/smp.{h,cpp}` + `src/boot/ap_trampoline.asm`): the local APIC is enabled, the ACPI MADT enumerates the cores, and each application processor (AP) is started with INIT-SIPI-SIPI through a real-mode→long-mode trampoline onto the kernel's shared page tables. Every core then loads its own GDT/TSS, the shared IDT, and per-core SYSCALL MSRs (`HAL::SetupAPCpuState`), and the 64-bit SYSCALL fast-path uses `swapgs` + a per-CPU kernel stack (`PerCpu.kernel_rsp` at `gs:8`) so two cores can syscall at once without sharing one stack.

`Scheduler::GetCurrentProcess()` / `SetCurrentForThisCpu()` track the running task **per CPU** (`PerCpu.current` on an AP, the global on the boot core), so the shared Linux syscall handler operates on the right task whichever core it runs on.

The boot core runs the desktop, drivers, and kernel processes as before. The remaining work is the AP dispatch loop that actually places **user** threads onto the secondary cores  -  it needs ready-queue synchronization (a spinlock around every queue scan/mutation, released before any context switch) so the boot core and an AP can't run the same task. Until that lands, user threads run on the boot core while the kernel runs on all cores.

## 6. Related files

- `src/proc/smp.cpp` / `src/boot/ap_trampoline.asm`  -  multi-core bring-up
- `src/apps/task_manager.cpp`  -  reads process table for display
- `src/drivers/timer.cpp`  -  timer interrupt triggers tick
- `src/kernel/kurono_kernel.cpp`  -  calls `Scheduler::Tick()` in main loop
