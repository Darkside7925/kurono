# UI Input Routing

This document explains how mouse and keyboard input travel from low level drivers to visible desktop behavior.

## 1. The route in one sentence

The kernel polls devices, the input manager normalizes state, the desktop environment decides what surface owns the event, the window manager handles chrome and focus, and application callbacks receive the final event.

## 2. Low level sources

The low level sources are in `src/drivers/keyboard.cpp` and `src/drivers/mouse.cpp`.

The keyboard driver tracks scancodes, key state, and a character queue.

The mouse driver tracks packet assembly, position, buttons, and a ring buffer of higher level mouse events.

## 3. The input manager

`src/system/input_manager.cpp` is the small but important bridge layer.

Its two main jobs are these.

1. Register visible input devices and keep keyboard callbacks connected to the rest of the system.
2. Poll the low level keyboard and mouse drivers in a controlled order.

The current code polls keyboard first and mouse second. That detail is not cosmetic. It exists because some laptop controllers can starve or wedge the keyboard when the touchpad floods the shared controller path.

## 4. Kernel main loop handoff

The actual handoff into the desktop happens inside the main loop in `src/kernel/kurono_kernel.cpp`.

The loop polls input, drains mouse events, reads keyboard characters, and feeds them into `DesktopEnvironment::HandleInput()`.

This is an intentionally direct route. There is no giant event bus between the kernel loop and the desktop shell.

## 5. Desktop environment as the first UI router

`DesktopEnvironment::HandleInput()` in `src/ui/desktop.cpp` is the first high level router.

It applies a strict priority order.

### Mouse side priority

1. Right click handling checks the focused window content area first.
2. Desktop right click opens the desktop context route if no window owns the click.
3. Left click begins double click detection.
4. The taskbar gets first refusal.
5. The window manager gets second refusal.
6. The desktop icon layer receives clicks only after the taskbar and window manager decline them.

This explains many surface interactions that might otherwise seem inconsistent.

If the taskbar accepts the click, the click never reaches the windows.

If the window manager accepts the click, the desktop never sees it.

### Keyboard side priority

1. If taskbar search is active, search owns the keys.
2. Otherwise the focused window receives the keypress callback.

That is the core keyboard route in Kurono.

## 6. Taskbar routing

The taskbar is not passive. It owns several special interactions.

1. Start button toggling
2. Search activation and search text editing
3. Search result launching
4. System tray clicks
5. Volume popup dragging

The taskbar acts more like a shell service than a decorative bar.

## 7. Window manager routing

The window manager owns window chrome, focus, stacking order, drag, resize, and window button behavior.

The main entry points are in `src/ui/window_manager.cpp`.

### `HandleMouseDown()`

This function finds the topmost window under the cursor, focuses it, checks close, minimize, and maximize controls, then performs hit testing for drag or resize regions.

If the click lands inside the client area and the window has an input callback, the callback is invoked.

### `HandleMouseMove()`

This function updates geometry while a drag or resize action is active.

### `HandleMouseUp()`

This function ends the active window manager action.

## 8. Application callback route

Every application window can expose a render callback and an input callback.

Once the desktop and window manager decide a window should own the event, the final dispatch goes to `win->input(...)`.

In practice, the event codes used in the desktop path are simple.

1. Mouse down style handoff
2. Keypress
3. Scroll
4. Right click

This is lightweight, but effective.

## 9. Search routing example

A useful example is the taskbar search box.

When search is active, character input no longer flows to the focused application. The desktop environment intercepts characters, edits `Taskbar::search_buf`, and on Enter launches the best matching application.

This is a clean example of the desktop shell overriding the normal focused window route.

## 10. Right click routing example

Right click behavior also shows the layered design.

1. The desktop environment asks whether the focused window content rectangle owns the click.
2. If yes, the window callback receives event `4`.
3. If not, the desktop gets the right click and can open a desktop context menu.

So right click is not globally owned by the desktop. It is window first, desktop second.

## 11. Why input bugs often cross files

Input bugs in Kurono usually cross at least three levels.

1. Driver level capture
2. Input manager polling and callback wiring
3. Desktop or window routing policy

That is why a dead keyboard can come from boot order, controller commands, polling order, or focused surface routing.

## 12. Fast debug order

When UI input breaks, inspect code in this order.

1. `src/kernel/kurono_kernel.cpp`
2. `src/system/input_manager.cpp`
3. `src/drivers/keyboard.cpp`
4. `src/drivers/mouse.cpp`
5. `src/ui/desktop.cpp`
6. `src/ui/window_manager.cpp`
7. The app window callback

## 13. Mental model

The UI stack is easiest to understand if treated as a cascade.

The device drivers observe hardware.

The input manager stabilizes polling.

The desktop environment decides which surface has first claim.

The window manager owns frame interactions.

The focused window owns the final application behavior.

That is the whole route.
