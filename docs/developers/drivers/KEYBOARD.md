# Keyboard Driver

`src/drivers/keyboard.cpp` and `keyboard.h` implement PS/2 keyboard support.

## 1. What it does

The keyboard driver communicates with the 8042 PS/2 controller to receive scan codes from an attached keyboard. It translates scan codes to logical key values and posts them to a software queue that the input manager drains each frame.

## 2. Initialization

The driver registers an IRQ 1 handler with the HAL during kernel bring-up. It programs the 8042 to enable the keyboard port and sets an LED state. On real laptop hardware, the 8042 initialization is fragile - the driver re-arms the keyboard after the PS/2 auxiliary (mouse) port is initialized because some controllers drop keyboard state during auxiliary bring-up.

## 3. Scan code translation

Raw scan codes are converted to a logical key table. The driver handles extended scan codes (0xE0 prefix) for keys like arrow keys, Home, End, Delete, and the numpad extras.

The current translation table covers a standard US QWERTY layout. Extending it for other layouts requires modifying the table in `keyboard.cpp`.

## 4. Key queue

Translated key characters are placed in a small circular buffer. The main loop drains this buffer and passes characters to the currently focused window or the desktop's input handler.

## 5. Common problems

| Problem | Likely cause |
| --- | --- |
| No keyboard input | IRQ 1 not registered or 8042 init failure |
| Garbled keys | Scan code set mismatch (Set 1 vs Set 2) |
| Missed keystrokes | Buffer overflow if queue not drained fast enough |
| Laptop touchpad interfering | PS/2 aux init order; keyboard needs re-arm |

## 6. Related files

- `src/hal/hal.cpp` - IRQ registration
- `src/drivers/mouse.cpp` - PS/2 auxiliary path that can affect keyboard
- `src/system/input_manager.cpp` - consumer of the keyboard queue
