# Panic and the crash screen

`src/kernel/panic.cpp` and `panic.h`. What runs when the kernel hits a state it can't
continue from: an exception with no handler, a null pointer in a critical path, a
failed assert.

The panic path stops the current context, draws a crash screen, optionally puts the
fault on serial, then halts or triggers a timed reboot.

## It comes up early

Panic init runs before the full display stack. Deliberate. If the graphics driver dies
later in boot, panic already has the framebuffer address from the bootloader's GOP or
linear framebuffer handoff, so it can still draw a screen with the driver broken.

That ordering is one of the more load-bearing decisions in the boot sequence. Don't
move panic init later without knowing why it sits there.

## What goes on the screen

- a coloured header with "KURONO PANIC" or similar
- the fault type or assert message
- register state, if the fault came out of an exception handler
- a pointer to the serial log for detail

## VGA fallback

If even the framebuffer path is unavailable, panic falls back to VGA text mode. That
covers boot dying before any graphics existed.

## Reset

After the screen, panic waits a moment then either halts or resets through the 8042 or
the ACPI reset register. Which one is adjustable in the source.

## When it breaks

| Symptom | Cause |
| --- | --- |
| crash screen black or garbled | framebuffer address was wrong at panic init |
| no crash screen at all | panic init ran before display, or there's no VGA |
| reboot loop | the reset fired before anyone could read the screen |

## Files

- `src/kernel/kurono_kernel.cpp` where panic init sits in the boot order
- `src/hal/hal.cpp` exception handlers that feed into panic
- `src/drivers/display.cpp` the framebuffer setup panic depends on
