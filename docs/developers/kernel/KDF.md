# KDF  -  Kernel Driver Framework

Kurono's answer to Windows KMDF: the **ring-0+** tier of a Windows-NT-style hybrid
kernel. The performance-critical core (VMM, PMM, scheduler, compositor, the NVMe
data path, the TCP stack) stays in plain ring 0. A KMDF-equivalent *kernel driver*
also runs in ring 0 (same privilege, same CR3), but every DMA buffer and MMIO
window it obtains through KDF is fenced by **unmapped guard pages**, so the common
driver bug  -  running off the end of a ring / descriptor / DMA buffer  -  is caught
and isolated instead of silently corrupting an adjacent kernel allocation or
panicking the whole OS.

Source: `src/kernel/kdf.{h,cpp}`, the fault hook in `src/hal/hal.cpp`, the kinit
bridge in `src/system/kinit.cpp`, and the crash-recovery gate in
`src/kernel/kdf_test.cpp`.

## What it gives you

- **Guard-fenced allocations.** `KDF::AllocDMA(size)`, `KDF::AllocContiguous(size)`,
  `KDF::MapMMIO(paddr, size)`. Each region is laid out `[guard][payload...][guard]`
  in a dedicated higher-half virtual window; the guard pages are unmapped, so an
  out-of-bounds access raises a `#PF`. `KDF::FreeDMA(va)` reclaims a region (and
  its slot, so a driver that cycles scratch buffers does not exhaust the table).
- **Physical translation for DMA.** `KDF::PhysOf(va)` maps a fenced higher-half VA
  back to its backing physical address, because the hardware DMAs physical
  addresses while the CPU touches the fenced alias.
- **Crash-isolated execution.** `KDF::RunGuarded(id, op, arg)` runs a driver
  operation with crash isolation armed (built on `__builtin_setjmp`/`longjmp`,
  freestanding, no libc). A guard-page `#PF` anywhere inside the call unwinds back
  to the call site and returns `false` instead of faulting the kernel. Nesting is
  supported.
- **Crash-isolated IRQs.** `KDF::RegisterIRQ(id, line, handler)` wraps an ISR so a
  fault inside it quarantines + reports rather than panicking.
- **Crash reporting + restart.** `KDF::ReportCrash` logs to
  `/kurono/var/log/drivers.log`, dumps registers, and notifies kinit, which runs
  its standard backoff + 5-in-60s restart policy and re-inits the driver.

## How isolation works

Each registered driver gets a 1 GiB slice of a higher-half VA window at
`0xFFFFC000_00000000` (PML4 entry 384, far above the identity map and the user
canonical lower half, so a fault there is unambiguously a KDF guard hit). KDF maps
the driver's DMA frames (allocated contiguous from the PMM, which also identity-
maps them for the device's benefit) into that slice with one unmapped page before
and after each region. The whole KDF sub-tree is reserved at init, before any user
address space is cloned, so the window is globally coherent.

When the CPU walks off a buffer into a guard page:

1. `#PF` → `isr_common_handler` (hal.cpp). For a **kernel-mode** fault, before the
   panic path, it calls `KDF::HandleGuardFault(cr2, rip)`.
2. If `cr2` is in the KDF window, KDF **quarantines** the region (unmaps its pages,
   frees its frames), dumps a regdump to serial, logs it, and calls
   `KDF::ReportCrash` → `KInit::NotifyDriverCrash`.
3. If a `RunGuarded` sandbox is armed on this CPU, KDF `__builtin_longjmp`s back to
   it (the op returns `false`); otherwise it returns `true` to the fault handler so
   the kernel still does not panic.
4. kinit schedules a backoff restart; its monitor re-runs the driver's init entry
   via `KDF::Start`, re-initializing the hardware.

## Using it from a driver

```cpp
// register once; init() is your (re-)init entry, run inside the crash sandbox.
int id = KDF::RegisterDriver("mydrv", &MyDriver::Init);
KDF::Start(id);                       // brings it up via RunGuarded

// inside Init(): guard-fenced resources, programmed by physical address.
void* ring   = KDF::AllocDMA(4096);
uint64_t pa  = KDF::PhysOf(ring);     // give the device pa, touch the va
void* regs   = KDF::MapMMIO(bar_paddr, 0x10000);
```

To get kinit restart supervision, register through kinit instead, which calls
`KDF::RegisterDriver` for you and supervises the unit:

```cpp
KInit::RegisterKdfDriver("mydrv", &MyDriver::Init, KInit::KTGT_KERNEL,
                         /*critical=*/false, /*already_running=*/false);
```

## Migrated drivers

- **NVMe** (`src/drivers/nvme.cpp`)  -  first migration. The admin SQ/CQ, the identify
  scratch, and the per-core I/O SQ/CQ pairs are `KDF::AllocDMA` (guard-fenced); the
  controller is programmed with `KDF::PhysOf` of each. BAR0 is `KDF::MapMMIO`. The
  hot Read/Write path is unchanged (data buffers are ordinary identity-mapped
  kernel memory, the queues are set up once), so steady-state throughput is
  unaffected  -  measured **879 MB/s seq write, 1082 MB/s seq read, 35.5k 4K IOPS**
  under KDF, with `verify=OK`.

## The `kurono.kdf.test` gate

Boot with `kurono.kdf.test` (add `kurono.kdf.poweroff=1` for a bounded CI run) to
run three headless crash-recovery scenarios (`src/kernel/kdf_test.cpp`):

- **A. sandbox unwind**  -  a guarded op writes one byte past its `AllocDMA` buffer
  into the trailing guard page; the kernel survives, the op returns failure, the
  region is quarantined.
- **B. in-bounds still works**  -  a normal in-bounds op completes after recovery.
- **C. kinit restart**  -  the reported crash drives kinit's backoff restart; the
  driver re-inits and its unit returns to RUNNING.

Verified result on `-smp 4` with an NVMe data disk: `KDF-SELFTEST: 3/3 OVERALL
PASS`, with the serial log showing the guard fault caught, the region quarantined,
the kinit crash + restart, the driver back to running, and the kernel executing
after the fault.

## Honest scope / limits

- KDF isolation is **VMM guard pages, NOT full address-space separation.** A KDF
  driver shares the kernel page tables; a wild write to an arbitrary *in-range*
  kernel address is not caught  -  only the fenced guard pages around its own
  DMA/MMIO are. It catches the common real bug (buffer overrun) and keeps the OS
  alive. For full isolation, a driver belongs in **UDF** (ring 3, see UDF.md).
- A driver restart means a **brief window of hardware unavailability** while it
  re-inits.
- The crash unwind re-enables interrupts on return (the `#PF` entered with IF
  cleared and `__builtin_longjmp` does not restore RFLAGS); the IRQ-from-fault case
  is handled but drivers that fault inside an ISR skip that IRQ's completion.
- Drivers other than NVMe are not yet migrated (audio / USB / GPU-detect are the
  next candidates, same pattern).
