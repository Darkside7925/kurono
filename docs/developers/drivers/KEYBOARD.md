# Keyboard

`src/drivers/keyboard.cpp` and `keyboard.h`. PS/2 keyboard.

Talks to the 8042 controller for scan codes, translates them to logical keys, posts them
to a software queue the input manager drains every frame.

## Init

Registers IRQ 1 with the HAL during bring-up, enables the keyboard port on the 8042,
sets the LED state. On real laptops the 8042 is fragile: the driver re-arms the keyboard
after the PS/2 aux (mouse) port comes up, because some controllers drop keyboard state
during auxiliary bring-up.

## Scan codes

Raw codes map through a logical key table. Extended codes, the 0xE0 prefix, are handled.
That covers arrows, Home, End, Delete and the numpad extras.

The table is US QWERTY. Other layouts mean editing the table in `keyboard.cpp`.

## Queue

Translated characters go into a small circular buffer. The main loop drains it into the
focused window or the desktop's input handler.

## When it breaks

| Symptom | Cause |
| --- | --- |
| no keyboard input | IRQ 1 not registered, or 8042 init failed |
| garbled keys | scan code set mismatch, Set 1 vs Set 2 |
| missed keystrokes | queue overflow, it isn't being drained fast enough |
| laptop touchpad fights the keyboard | PS/2 aux init order, the keyboard needs re-arming |

## Files

- `src/hal/hal.cpp` IRQ registration
- `src/drivers/mouse.cpp` the aux path that can disturb the keyboard
- `src/system/input_manager.cpp` drains the keyboard queue
