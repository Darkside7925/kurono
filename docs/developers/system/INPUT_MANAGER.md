# Input Manager

`src/system/input_manager.cpp` and `input_manager.h` aggregate keyboard and mouse events from their hardware drivers and route them to the focused window or shell.

## 1. Role

The input manager is the bridge between the hardware drivers (keyboard, mouse) and the UI. The hardware drivers post raw event data; the input manager reads it, applies any preprocessing, and calls the right handler.

## 2. Keyboard flow

```
HAL IRQ 1 → keyboard.cpp (translate scan code) → post to key queue
Main loop → InputManager::PollKeyboard() → KuronoShell or DesktopEnvironment
```

The main loop calls `InputManager::PollKeyboard()` each iteration. That function drains the key queue and passes each character to `DesktopEnvironment::HandleInput()`. The desktop environment routes it to the focused window's input callback.

## 3. Mouse flow

```
HAL IRQ 12 → mouse.cpp (assemble packet, update position) → post delta/button event
Main loop → InputManager::PollMouse() → DesktopEnvironment
```

The split-trigger model means: IRQs queue events, the main loop drains them. This keeps IRQ handlers minimal and avoids re-entrant UI code.

## 4. Cursor position

The input manager maintains the global cursor position. The desktop renderer queries this to draw the cursor sprite at the right location each frame.

## 5. Related files

- `src/drivers/keyboard.cpp`  -  keyboard event source
- `src/drivers/mouse.cpp`  -  mouse event source
- `src/ui/desktop.cpp`  -  `HandleInput()` consumer
