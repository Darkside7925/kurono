# Mouse Driver

`src/drivers/mouse.cpp` and `mouse.h` implement PS/2 mouse and basic laptop touchpad support.

## 1. What it does

The mouse driver initializes the PS/2 auxiliary port (IRQ 12) and reads movement and button packets. It translates raw delta values into absolute screen coordinates clamped to the display bounds.

## 2. Initialization

PS/2 mouse initialization is one of the trickiest parts of the hardware bring-up on real laptops. The 8042 auxiliary port enable sequence can reset the keyboard controller state, which is why the keyboard driver re-arms the keyboard after mouse init completes.

The driver enables the auxiliary device, sets the sample rate, and enables data reporting before installing the IRQ 12 handler.

## 3. Packet format

Standard PS/2 mice send 3-byte packets: a status byte (button bits, overflow flags, sign bits) followed by X and Y delta bytes. The driver assembles packets and converts deltas to an absolute cursor position.

## 4. Touchpad notes

Laptop touchpads typically emulate a PS/2 mouse at the protocol level. The driver works with them through the same path. Extended touchpad features (multi-touch, gestures, palm rejection) are not implemented  -  the driver sees basic pointer movement and left/right button clicks only.

## 5. Cursor position

The mouse driver maintains a global cursor position that the desktop renderer uses to draw the cursor sprite. The main loop reads mouse events and feeds them into `DesktopEnvironment::HandleInput()`.

## 6. Common problems

| Problem | Likely cause |
| --- | --- |
| Mouse does not move | IRQ 12 not registered or aux port disabled |
| Cursor stuck at edge | Delta accumulation overflow; clamp logic |
| Pointer jumps erratically | Packet alignment lost; re-init the device |
| Keyboard stops working after mouse init | 8042 state corrupted; re-arm keyboard in init sequence |

## 7. Related files

- `src/hal/hal.cpp`  -  IRQ 12 registration
- `src/drivers/keyboard.cpp`  -  keyboard re-arm after aux init
- `src/system/input_manager.cpp`  -  consumer of mouse events
