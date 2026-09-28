# kurono_kernel.cpp

The boot coordinator and main loop for the whole OS. Most important single file in the
project.

Not a stub. The live C++ equivalent of a kernel boot script. Everything Kurono
initialises, registers and runs goes through it. Hunting a boot-time problem? Start
here.

## Boot order

The order is deliberate. Change it without understanding the dependencies and it will
crash.

1. serial logging, so everything after it has somewhere to go
2. Multiboot2 info normalised when needed
3. panic, early, so crash rendering has a framebuffer even if graphics dies later
4. HAL: IDT, PIC, interrupts
5. PMM, built from the Multiboot memory map
6. heap, on top of the PMM
7. scheduler
8. KVFS
9. UIConfig, from `/etc/kurono/ui.conf`
10. timer and RTC
11. the display path: GOP, then VBE, falling back to BGA
12. keyboard and mouse
13. shell plus every subsystem command registration
14. desktop, then icons
15. main loop

## Main loop

Runs forever after boot. Every pass:

1. advance time from the PIT
2. drain deferred settings work
3. tick audio output
4. poll the E1000 if there is one
5. poll the input queue
6. tick the scheduler
7. push mouse and keyboard events into the desktop
8. update and render the desktop
9. refresh periodic views like the task manager

Polling on purpose. Not a limitation, it's what the hardware compatibility story needs.

## Emergency mode

If the normal boot path looks unsafe, it drops into an emergency profile: VGA text
shell, minimum services, no desktop. Useful for diagnosing hardware failures before the
GPU is ready.

## Where it falls over

| Symptom | Most likely cause |
| --- | --- |
| black screen before the shell | HAL or IDT setup failure |
| crash before the desktop | PMM, heap, or the display path |
| no keyboard response | 8042/PS2 init order |
| fonts wrong or missing | font init ran before display was ready |
| freeze in the main loop | timer or scheduler stopped ticking |

## Files

- `src/kernel/panic.cpp` crash screen
- `src/hal/hal.cpp` IDT and PIC
- `src/kernel/pmm.cpp` physical memory
- `src/kernel/heap.cpp` heap allocator
- `src/drivers/display.cpp` display path selection
