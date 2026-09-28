# Input manager

`src/system/input_manager.cpp` and `input_manager.h`. Collects keyboard and mouse events
from the drivers and routes them to the focused window or the shell.

It's the bridge between hardware drivers and the UI. The drivers post raw events, this reads
them, does any preprocessing, and calls the right handler.

## Keyboard

```
HAL IRQ 1 -> keyboard.cpp (translate scan code) -> key queue
main loop -> InputManager::PollKeyboard() -> KuronoShell or DesktopEnvironment
```

The main loop calls `PollKeyboard()` each iteration. It drains the queue into
`DesktopEnvironment::HandleInput()`, which routes to the focused window's input callback.

## Mouse

```
HAL IRQ 12 -> mouse.cpp (assemble packet, update position) -> delta/button event
main loop -> InputManager::PollMouse() -> DesktopEnvironment
```

Split-trigger: IRQs queue, the main loop drains. That keeps IRQ handlers minimal and stops
UI code being re-entered.

## Cursor

The input manager owns the global cursor position. The desktop renderer reads it to draw the
sprite each frame.

## Files

- `src/drivers/keyboard.cpp` keyboard event source
- `src/drivers/mouse.cpp` mouse event source
- `src/ui/desktop.cpp` the `HandleInput()` consumer
