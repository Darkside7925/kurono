# Panic and Crash Screen

`src/kernel/panic.cpp` and `panic.h` implement the crash handler that runs when the kernel detects an unrecoverable error.

## 1. Purpose

When the kernel hits a state it cannot continue from  -  a CPU exception with no handler, a null pointer in a critical path, an assert failure  -  it calls the panic path. The panic path:

1. Stops the current execution context.
2. Draws a crash screen to the framebuffer.
3. Optionally logs the fault to the serial port.
4. Halts the CPU or triggers a timed reboot.

## 2. Early initialization

The panic system is initialized *before* the main display stack is fully online. This is deliberate. If the graphics driver fails later during boot, the panic code already has the framebuffer address from the bootloader's GOP or linear framebuffer handoff. It can still render a crash screen even when the driver is broken.

This is one of the more important design decisions in the boot order. Do not move panic initialization later without understanding this dependency.

## 3. Panic screen content

The crash screen typically shows:

- A colored header bar with a message ("KURONO PANIC" or similar).
- The fault type or assertion message.
- Register state if the fault came from a CPU exception handler.
- A suggestion to check the serial log for more detail.

## 4. VGA fallback

If even the framebuffer path is unavailable, the panic path has a VGA text-mode fallback. This handles the case where boot fails before any graphics is established.

## 5. Hard reboot path

After displaying the crash screen, the panic path waits briefly and then either halts or triggers a hardware reset through the 8042 or the ACPI reset register. The behavior can be adjusted in the panic source.

## 6. Common problems

| Problem | Likely cause |
| --- | --- |
| Crash screen is black or garbled | Framebuffer address wrong at panic init time |
| No crash screen at all | Panic init ran before display or VGA not available |
| Reboot loop after crash | Hard reboot triggered before the user can read the screen |

## 7. Related files

- `src/kernel/kurono_kernel.cpp`  -  early panic init placement
- `src/hal/hal.cpp`  -  CPU exception handlers that feed into panic
- `src/drivers/display.cpp`  -  framebuffer setup that panic depends on
