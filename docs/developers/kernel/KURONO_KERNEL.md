# Kernel Core

`src/kernel/kurono_kernel.cpp` is the boot coordinator and main loop for the entire OS. It is the most important single file in the project.

## 1. What this file is

This is not a minimal stub. It is the live C++ equivalent of a kernel boot script. Everything that Kurono initializes, registers, and runs flows through here. If you are hunting down a boot-time problem, start here.

## 2. Boot sequence overview

The kernel performs initialization in a deliberate fixed order. Changing that order without understanding the dependencies will likely produce a crash.

1. Serial logging is brought up so all subsequent debug output has a path out.
2. Multiboot2 info is normalized to a compatible structure when needed.
3. Panic is initialized early so crash rendering has a framebuffer path even if the graphics driver fails later.
4. HAL (IDT, PIC, interrupts) is initialized.
5. Physical memory manager (PMM) is set up from the Multiboot memory map.
6. Heap allocator is initialized on top of PMM.
7. Scheduler is started.
8. KVFS virtual filesystem is initialized.
9. UIConfig is loaded from `/etc/kurono/ui.conf`.
10. Timer and RTC are started.
11. Display path is chosen (GOP > VBE > BGA fallback).
12. Input path (keyboard, mouse) is initialized.
13. Shell and all subsystem commands are registered.
14. Desktop environment is initialized and icons are placed.
15. Main loop begins.

## 3. Main loop

The main loop runs indefinitely after boot. Each iteration:

1. Advances time via PIT measurements.
2. Polls deferred settings work.
3. Ticks audio output.
4. Polls the E1000 NIC if present.
5. Polls the input queue.
6. Ticks the scheduler.
7. Drains mouse and keyboard events into the desktop.
8. Updates and renders the desktop.
9. Refreshes periodic views like Task Manager.

The loop is intentionally polling-based. This is not a limitation - it is a deliberate architectural choice that matches the project's hardware compatibility requirements.

## 4. Emergency mode

If the system detects that the normal boot path is unsafe, it falls into an emergency profile. This profile uses a VGA text shell, registers only the minimum services needed for diagnosis, and skips the desktop entirely. Useful for diagnosing hardware failures before the GPU is ready.

## 5. Common failure points

| Symptom | Most likely cause |
| --- | --- |
| Black screen before shell appears | HAL or IDT setup failure |
| Crash before desktop | PMM, heap, or display path |
| No keyboard response | 8042/PS2 init order |
| Fonts wrong or missing | Font init before display ready |
| Freezes in main loop | Timer or scheduler not ticking |

## 6. Related files

- `src/kernel/panic.cpp` - crash screen
- `src/hal/hal.cpp` - IDT and PIC
- `src/kernel/pmm.cpp` - physical memory
- `src/kernel/heap.cpp` - heap allocator
- `src/drivers/display.cpp` - display path selection
