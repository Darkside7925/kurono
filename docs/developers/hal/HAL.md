# Hardware Abstraction Layer

`src/hal/hal.cpp` and `hal.h` are the lowest-level x86 hardware interface in Kurono.

## 1. What HAL does

The HAL handles everything that requires direct CPU and chipset access at the early hardware level.

- Sets up the IDT (Interrupt Descriptor Table) with handlers for all 256 vectors.
- Programs the PIC (Programmable Interrupt Controller) to remap IRQs above the CPU exception range.
- Implements the default CPU exception handlers (divide by zero, page fault, general protection fault, double fault, etc.) that feed into the panic system.
- Provides hardware reboot via the 8042 reset line.
- Exposes `sti()`/`cli()` wrappers for enabling and disabling interrupts.

## 2. Initialization

HAL is the first major subsystem initialized in `kurono_kernel.cpp`, right after serial logging. If HAL does not initialize correctly, the CPU will triple fault the moment any interrupt fires. This includes the timer tick.

## 3. IDT layout

The IDT uses 64-bit interrupt gate descriptors. The HAL sets all 256 entries to a default stub that logs the vector number and calls panic. Specific handlers for the keyboard (IRQ 1), mouse (IRQ 12), timer (IRQ 0), and NIC are installed later by their respective drivers after HAL sets up the base table.

## 4. PIC remapping

The x86 legacy PIC's default IRQ assignment (IRQ 0 - 7 mapped to vectors 8 - 15) collides with CPU exception vectors. HAL remaps:

- Master PIC (IRQ 0 - 7) to vectors 0x20 - 0x27
- Slave PIC (IRQ 8 - 15) to vectors 0x28 - 0x2F

This is standard and must happen before any hardware interrupt can be used safely.

## 5. Reboot

HAL provides a `Reboot()` function that pulses the 8042 reset line. This is the reboot path used by the panic system and by the `reboot` shell command.

## 6. Common problems

| Problem | Likely cause |
| --- | --- |
| Triple fault on boot | HAL init incomplete or IDT not loaded |
| IRQ fires wrong handler | PIC remap not done before device init |
| Keyboard interrupt lost | IRQ 1 handler not registered after HAL init |
| Reboot hangs | 8042 not responding; try ACPI reset path |

## 7. Related files

- `src/kernel/kurono_kernel.cpp` - calls `HAL::Init()` first in boot sequence
- `src/kernel/panic.cpp` - receives CPU fault callbacks from HAL exception handlers
- `src/drivers/keyboard.cpp` - registers IRQ 1 handler through HAL
- `src/drivers/timer.cpp` - registers IRQ 0 handler through HAL
