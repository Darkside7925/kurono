# Scheduler

`src/proc/scheduler.cpp` and `scheduler.h`.

It holds the runnable task list and decides who runs on each tick. Called from the main loop
and from the timer interrupt callback.

## Model

Cooperative and time-sliced at the same time: a CFS-style vruntime pick plus FIFO/RR preempt
tiers. Kernel processes on the boot core are still mostly cooperative (loop `work();
SleepMs(n)` and yield). Priority values move each task's time quantum.

Ring-3 user processes are real: a per-CPU TSS (`TSS64` in `src/hal/hal.cpp`), `int 0x80`
plus the per-CPU `swapgs` SYSCALL fast path, and saved user interrupt frames. Linux user
threads get preemptively switched, on the boot core from the PIT IRQ and opt-in on the
application processors from a per-CPU LAPIC timer (see [SMP.md](SMP.md)). The old claim that
full preemptive user processes would need TSS/syscall infrastructure is dead, that
infrastructure is in.

## Process table

The scheduler owns the canonical table and the task manager renders it. Each entry has PID,
name (32 chars), CPU percent, memory in KB, thread count, state (RUNNING, SLEEPING, STOPPED,
ZOMBIE), priority and I/O counters.

## Tick

`Scheduler::Tick()` runs from the main loop each iteration. It advances the time-slice
counters and does the context switches.

## Multi-core

Every logical CPU comes up at boot (`src/proc/smp.{h,cpp}` plus
`src/boot/ap_trampoline.asm`): local APIC enabled, ACPI MADT enumerates the cores, and each
application processor is started with INIT-SIPI-SIPI through a real-mode to long-mode
trampoline onto the kernel's shared page tables. Each core then loads its own GDT/TSS, the
shared IDT and per-core SYSCALL MSRs (`HAL::SetupAPCpuState`). The 64-bit SYSCALL fast path
uses `swapgs` plus a per-CPU kernel stack (`PerCpu.kernel_rsp` at `gs:8`), so two cores can
syscall at once without sharing one stack.

`Scheduler::GetCurrentProcess()` and `SetCurrentForThisCpu()` track the running task per CPU
(`PerCpu.current` on an AP, the global on the boot core), so the shared Linux syscall handler
acts on the right task whichever core it runs on.

The boot core still runs the desktop, drivers and kernel processes. The AP dispatch loop that
puts user threads on the secondary cores has landed: a cross-core ready-queue spinlock
(`g_sched_lock`) with an atomic per-CPU claim, a per-CPU user-execution context, and per-CPU
syscall state, all verified non-regressing under `-smp 4`. With the opt-in gate on
(`./start.sh --apsched`), an application processor was shown launching and running a ring-3
Linux program to exit 0 in parallel with the desktop, and each AP arms a per-CPU LAPIC timer
so those threads are time-sliced rather than purely cooperative. Default boot still parks the
APs. Left to do: `clone`-sibling placement across cores, and load balancing. Full phased state
in [SMP.md](SMP.md).

## Files

- `src/proc/smp.cpp` and `src/boot/ap_trampoline.asm` multi-core bring-up
- `src/apps/task_manager.cpp` renders the process table
- `src/drivers/timer.cpp` the interrupt that triggers the tick
- `src/kernel/kurono_kernel.cpp` calls `Scheduler::Tick()` in the main loop
