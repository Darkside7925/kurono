# SMP  -  Symmetric Multi-Processing

`src/proc/smp.{h,cpp}` + `src/boot/ap_trampoline.asm` bring up the secondary CPU
cores (application processors / APs) and, with the scheduler, are growing toward
running real user threads on them in parallel. This is the gate for genuinely
multithreaded user workloads (e.g. a multithreaded browser) using more than one
core.

The work is phased; every phase is committed and verified to still boot the
desktop under `-smp 4`. See also [SCHEDULER.md](SCHEDULER.md).

## 1. What runs where

- **The BSP (boot CPU, core 0)** runs the kernel processes  -  the compositor,
  window manager, drivers, daemons  -  exactly as it always has. The GUI is single-
  threaded and stays on the BSP.
- **The APs (cores 1..N)** boot, set up their own CPU state, and are being wired
  to pull **user threads** off a shared ready queue and run them in ring-3. They
  never run kernel processes.

This split is deliberate: it keeps the single-threaded GUI/driver code off the
APs (no locking retrofit needed there) while letting CPU-bound user threads use
the extra cores.

## 2. Bring-up (phases 1 - 3c, done)

| Phase | What |
| --- | --- |
| 1 | LAPIC enable; ACPI **MADT** parse (scan BIOS/EBDA for `RSD PTR ` → RSDT/XSDT → `APIC` table) to enumerate cores + APIC IDs (CPUID fallback). `PerCpu` blocks + an APIC-id→dense-index map. |
| 2 | **AP trampoline** (`ap_trampoline.asm`): a flat blob copied to physical `0x8000` (SIPI vector `0x08`); real → 32-bit → 64-bit on the BSP's *shared* CR3+GDT; patch area at `0x9000` (CR3, GDT ptr, `&ap_entry`, per-AP stack). `SMP::StartAPs()` issues **INIT-SIPI-SIPI** serially. Reports `4/4 cpus online`. |
| 3a | **Per-CPU SYSCALL path**: `syscall_entry.asm` does `swapgs` at entry (gs = `PerCpu`), stashes the user RSP at `gs:0`, loads this CPU's kernel stack from `gs:8`, `swapgs` back before `iretq`  -  so two cores can `syscall` at once without sharing a kernel stack. |
| 3b | **Per-CPU GDT/TSS/IDT + SYSCALL MSRs** per AP (`HAL::SetupAPCpuState`): each AP `lgdt`s its own GDT (its own TSS descriptor + IST stack), `ltr`s its TSS, `lidt`s the shared IDT. |
| 3c | **Per-CPU current task**: `Scheduler::GetCurrentProcess()` returns `PerCpu.current` on an AP / the global on the BSP; `SetCurrentForThisCpu()` routes the per-CPU write. |

## 3. The per-CPU rewrite (phase 3d)

Running ring-3 Linux user threads on the APs turned out to need more than a
scheduler loop, because the user-execution layer was built around **one active
process at a time, driven from the BSP**. Doing it naïvely would corrupt the
Linux runtime. Two designs were on the table  -  a big-kernel-lock (ring-3 parallel,
syscalls serialized) or a full per-CPU rewrite (syscalls parallel too). The
project chose the **full per-CPU rewrite**.

### Foundation  -  done, committed, verified `-smp 4`

This is the conceptually-hard, race-prone core, and it's in:

- **Cross-core scheduler lock (`g_sched_lock`)** with a strict `_nolock`
  discipline. The `ready_queue` was mutated under only an `IrqGuard` (cli/sti)  - 
  single-core mutual exclusion that does nothing against another core. Now the
  queue helpers, `Schedule()`, and the user pick all coordinate on one lock.
  `ScheduleNextUser()` does an **atomic release-current + claim-next entirely
  under the lock**, so there's never a window where a thread is Ready-but-unowned
  that two cores could double-claim. A CPU-**affinity** gate is in the pick, and
  `ClaimNextUserForCpu()` is the atomic bootstrap claim for an AP.
- **Per-CPU Userspace context.** `active_process` / `return_context` were single
  globals (a second process was literally rejected; an exit `longjmp`ed to one
  shared frame). They're now a `UserspaceCpuState[SMP_MAX_CPUS]` indexed per CPU,
  so each core runs its own active process and longjmps back to **its own**
  `RunProcessWithArgs` frame on exit/fault.
- **Per-CPU syscall-entry scratch.** `current_syscall_frame` and the
  rewrite/resume flags were globals that two cores in syscalls would corrupt  - 
  now per-CPU.

On the BSP all of this runs as slot 0, so behavior is **identical** to before  - 
verified: boots `-smp 4`, 4/4 CPUs online, the desktop renders, Wayland + every
kernel process up, zero faults.

### APs run user processes  -  done (proven)

The secondary cores now execute real ring-3 Linux user **processes** in parallel
with the BSP. **Proven**: with the gate enabled, an application processor (cpu3)
claimed `/usr/bin/mhello` (a static-musl program), launched it via the now-per-CPU
`RunProcessWithArgs`, ran its code + SYSCALLs on the per-CPU syscall path, printed
its output, and **exited 0**  -  while the desktop rendered untouched (verified by
screenshot).

- `SMP::SetApUserSched` / `ApUserSched` is the gate (default **off**). The
  `ap_entry()` dispatch loop, when the gate is on, calls `ClaimFreshUserForCpu`,
  runs the claimed process with `RunProcessWithArgs`, drains its console output,
  and loops; gate off → the AP parks (`hlt`) exactly as before.
- `ClaimFreshUserForCpu` claims only a process **explicitly pinned** to that CPU
  (an affinity bit set), atomically under the scheduler lock. The explicit pin
  closes a load race  -  a process is enqueued `Ready` *before* its caller finishes
  mapping the ELF segments, so a spinning AP must not grab a default-affinity
  process mid-load; the launcher pins it to an AP only **after** it is fully
  loaded.
- Opt-in: the `kurono.apsched` cmdline token (or `./start.sh --apsched`) sets the
  gate before AP bring-up. Without it the default boot is unchanged (APs park).

### Phase 4  -  preemption (part 1 done)

AP execution started **cooperative** (a thread only yielded at a syscall/exit, so
a compute-bound thread monopolized its core). Now each AP arms a **periodic LAPIC
timer** (vector 0x40, ~100 Hz, calibrated against the TSC at bring-up) that drives
`Scheduler::ApTimerPreempt` → `SaveUserFrame` + `ScheduleNextUser`  -  the *same*
per-CPU switch the BSP's PIT-IRQ preempt uses (the threads of one process share
cr3, so no address-space switch). So a user thread on an AP is now time-sliced,
not just cooperative. A side benefit: with interrupts enabled, a parked AP wakes
on each tick and re-checks the dispatch gate, so toggling the gate at runtime
works without an IPI. Verified: boots `-smp 4` with the timer armed on every AP,
desktop renders, no faults, and an AP ran a user program to exit 0 with the timer
live. (The "resume an existing thread's frame" primitive turned out to be
unnecessary  -  `ScheduleNextUser`'s IRQ-frame rewrite *is* the resume.)

Still ahead: live-verifying multithreaded time-slicing on an AP (`pthread_test`),
deciding `clone` **sibling-thread placement** (pin to the parent's core vs spread
across cores), and **load balancing**.

## 4. Constraints that shape the design

- The kernel scheduler for *kernel processes* is preemptive on the BSP (PIT IRQ0);
  the APs currently have no timer, so AP user scheduling starts cooperative.
- Spinlocks are real `lock cmpxchg` atomics, so the existing global subsystem
  locks are already SMP-safe.
- `HAL::SetKernelStack` updates the per-CPU syscall stack (`gs:8`); each AP's
  IRQ/fault stack is its own `ap_tss[cpu].rsp0`.

## 5. Verifying

```bash
./start.sh --headless --no-build          # boot -smp 4 headless, QMP at /tmp/kurono.qmp
python3 qmp_shot.py /tmp/kurono.qmp out    # screendump to out.png  -  confirm the desktop renders
```

The `pthread_test` user program (`clone` + `futex`, embedded in the kernel) is the
regression gate for the per-CPU syscall path.

## 6. Related files

- `src/proc/smp.{h,cpp}`  -  LAPIC, MADT, per-CPU blocks, AP bring-up
- `src/boot/ap_trampoline.asm`  -  real→long-mode AP trampoline
- `src/proc/scheduler.{h,cpp}`  -  the cross-core lock, atomic claim, per-CPU pick
- `src/kernel/userspace.{h,cpp}`  -  per-CPU user-execution context
- `src/hal/hal.cpp`  -  `SetupAPCpuState`, per-CPU TSS/GDT, `EnterUserMode`
