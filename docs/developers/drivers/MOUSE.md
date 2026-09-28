# Mouse

`src/drivers/mouse.cpp` and `mouse.h`. PS/2 mouse, plus whatever a laptop touchpad
emulates.

Sets up the PS/2 auxiliary port (IRQ 12), reads movement and button packets, turns raw
deltas into an absolute cursor position clamped to the display bounds.

## Init

PS/2 mouse init is one of the nastier bits of real laptop bring-up. Enabling the 8042
aux port can reset the keyboard controller's state, which is why the keyboard driver
re-arms itself once this finishes.

The driver enables the aux device, sets the sample rate, enables data reporting, then
installs IRQ 12.

## Packets

Standard 3-byte packets: a status byte (button bits, overflow flags, sign bits) then X
and Y delta bytes. The driver assembles them and converts deltas to an absolute cursor
position.

## Touchpads

Laptop touchpads emulate a PS/2 mouse at the protocol level, so the same path works.
Multi-touch, gestures and palm rejection are not implemented. You get basic pointer
movement and left/right click.

## Cursor

The driver keeps a global cursor position and the desktop renderer draws the sprite
from it. The main loop reads mouse events and feeds them into
`DesktopEnvironment::HandleInput()`.

## When it breaks

| Symptom | Cause |
| --- | --- |
| cursor doesn't move | IRQ 12 not registered, or the aux port is disabled |
| cursor sticks at the edge | delta accumulation overflow, look at the clamp |
| pointer jumps around | packet alignment lost, re-init the device |
| keyboard stops after mouse init | 8042 state got clobbered, re-arm the keyboard |

## Files

- `src/hal/hal.cpp` IRQ 12 registration
- `src/drivers/keyboard.cpp` re-arms the keyboard after aux init
- `src/system/input_manager.cpp` consumes mouse events
