# Scheduler

`src/proc/scheduler.cpp` and `scheduler.h` implement the Kurono process scheduler.

## 1. What it does

The scheduler maintains the list of runnable tasks and decides which one runs each time it is ticked. It is called from the main loop and from the timer interrupt callback.

## 2. Current model

The scheduler is a cooperative/time-slice hybrid with a CFS-style vruntime pick
plus FIFO/RR preempt tiers. **Kernel processes** on the boot core are still
largely cooperative (they loop `work(); SleepMs(n)` and yield), while priority
values influence each task's time quantum.

The OS **does** have full ring-3 user processes: a per-CPU TSS (`TSS64` in
`src/hal/hal.cpp`), `int 0x80` + the per-CPU `swapgs` SYSCALL fast path, and
saved user interrupt frames. Linux user threads are preemptively switched - on
the boot core via the PIT IRQ, and (opt-in) on the application processors via a
per-CPU LAPIC timer (see §5 and [SMP.md](SMP.md)). The earlier claim that "full
preemptive user-space processes would require TSS/syscall infrastructure" is no
longer true - that infrastructure is in.

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

The boot core runs the desktop, drivers, and kernel processes as before. The
AP dispatch loop that places **user** threads onto the secondary cores has since
landed: a cross-core ready-queue spinlock (`g_sched_lock`) with an atomic per-CPU
claim, a per-CPU user-execution context, and per-CPU syscall state, all verified
non-regressing under `-smp 4`. With the opt-in gate on (`./start.sh --apsched`)
an application processor was shown launching and running a ring-3 Linux program
to exit 0 in parallel with the desktop, and each AP arms a per-CPU LAPIC timer so
those threads are time-sliced rather than purely cooperative. The default boot
still parks the APs. Remaining: `clone`-sibling placement across cores and load
balancing. See [SMP.md](SMP.md) for the full phased state.

## 6. Related files

- `src/proc/smp.cpp` / `src/boot/ap_trampoline.asm` - multi-core bring-up
- `src/apps/task_manager.cpp` - reads process table for display
- `src/drivers/timer.cpp` - timer interrupt triggers tick
- `src/kernel/kurono_kernel.cpp` - calls `Scheduler::Tick()` in main loop
