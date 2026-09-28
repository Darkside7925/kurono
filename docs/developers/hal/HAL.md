# HAL

`src/hal/hal.cpp` and `hal.h`. The lowest x86 hardware interface in Kurono.

It does everything that needs direct CPU and chipset access:

- IDT with handlers for all 256 vectors
- PIC remap so IRQs sit above the CPU exception range
- the default exception handlers (divide by zero, page fault, GP fault, double fault)
  that feed into panic
- hardware reboot through the 8042 reset line
- `sti()` and `cli()` wrappers

## Init

First major subsystem in `kurono_kernel.cpp`, right after serial logging. If HAL
doesn't come up right, the CPU triple faults on the first interrupt that fires,
including the timer tick.

## IDT

64-bit interrupt gate descriptors. All 256 entries start on a default stub that logs the
vector and calls panic. Keyboard (IRQ 1), mouse (IRQ 12), timer (IRQ 0) and the NIC
install their real handlers later, once HAL has the base table up.

## PIC remap

The legacy PIC's default mapping, IRQ 0 to 7 on vectors 8 to 15, collides with CPU
exception vectors. HAL moves it:

- master PIC, IRQ 0 to 7, onto vectors 0x20 to 0x27
- slave PIC, IRQ 8 to 15, onto vectors 0x28 to 0x2F

Standard, and it has to happen before any hardware interrupt is safe to use.

## Reboot

`Reboot()` pulses the 8042 reset line. Both the panic path and the `reboot` shell
command use it.

## When it breaks

| Symptom | Cause |
| --- | --- |
| triple fault on boot | HAL init incomplete, or the IDT never got loaded |
| IRQ hits the wrong handler | PIC remap skipped before device init |
| keyboard interrupt lost | IRQ 1 handler not registered after HAL |
| reboot hangs | 8042 not responding, try the ACPI reset path |

## Files

- `src/kernel/kurono_kernel.cpp` calls `HAL::Init()` first in the boot sequence
- `src/kernel/panic.cpp` takes CPU fault callbacks from HAL's exception handlers
- `src/drivers/keyboard.cpp` registers IRQ 1 through HAL
- `src/drivers/timer.cpp` registers IRQ 0 through HAL
