# UI input routing

How mouse and keyboard input gets from the drivers to something visibly happening.

## The route in one sentence

The kernel polls the devices, the input manager normalises state, the desktop environment decides
which surface owns the event, the window manager handles chrome and focus, and the app callback
gets what's left.

## Low level sources

`src/drivers/keyboard.cpp` and `src/drivers/mouse.cpp`. The keyboard driver tracks scancodes, key
state and a character queue. The mouse driver tracks packet assembly, position, buttons and a ring
buffer of higher-level events.

## The input manager

`src/system/input_manager.cpp`, small but important. Two jobs: register the visible input devices
and keep keyboard callbacks wired into the rest of the system, then poll the low-level drivers in a
controlled order.

The order is keyboard first, mouse second. That's not cosmetic. Some laptop controllers will starve
or wedge the keyboard when the touchpad floods the shared controller path.

## Handoff from the main loop

The handoff happens in the main loop in `src/kernel/kurono_kernel.cpp`. The loop polls input,
drains mouse events, reads keyboard characters, and feeds them into
`DesktopEnvironment::HandleInput()`.

Deliberately direct. There is no giant event bus between the kernel loop and the desktop shell.

## DesktopEnvironment is the first UI router

`DesktopEnvironment::HandleInput()` in `src/ui/desktop.cpp` applies a strict priority order.

Mouse side:

1. right click checks the focused window's content area first
2. desktop right click opens the desktop context route if no window owns the click
3. left click starts double-click detection
4. the taskbar gets first refusal
5. the window manager gets second refusal
6. desktop icons only see clicks the taskbar and window manager declined

That ordering explains a lot of surface interactions that otherwise look inconsistent. If the
taskbar takes the click, the windows never see it. If the window manager takes it, the desktop never
sees it.

Keyboard side:

1. if taskbar search is active, search owns the keys
2. otherwise the focused window gets the keypress callback

## The taskbar is not passive

It owns several special interactions: start button toggling, search activation and search text
editing, launching search results, system tray clicks, and volume popup dragging. It behaves more
like a shell service than a decorative bar.

## Window manager routing

The WM owns chrome, focus, stacking order, drag, resize and window buttons. Entry points in
`src/ui/window_manager.cpp`:

- `HandleMouseDown()` finds the topmost window under the cursor, focuses it, checks the
  close, minimize and maximize controls, then hit-tests the drag and resize regions. If the click
  lands in the client area and the window has an input callback, the callback runs.
- `HandleMouseMove()` updates geometry while a drag or resize is active.
- `HandleMouseUp()` ends the active WM action.

## App callbacks

Every app window can expose a render callback and an input callback. Once the desktop and the WM
decide a window owns the event, the final dispatch is `win->input(...)`. The event codes in the
desktop path are simple: mouse-down handoff, keypress, scroll, right click. Lightweight, but it
works.

## Search, as an example

When taskbar search is active, characters stop flowing to the focused app. The desktop environment
intercepts them, edits `Taskbar::search_buf`, and on Enter launches the best match. A clean example
of the desktop shell overriding the normal focused-window route.

## Right click, as an example

1. the desktop environment asks whether the focused window's content rectangle owns the click
2. if yes, the window callback gets event `4`
3. if not, the desktop gets the right click and can open its context menu

So right click is window first, desktop second, not globally owned by the desktop.

## Why input bugs cross files

They usually cross at least three levels: driver capture, input manager polling and callback
wiring, then desktop or window routing policy. That's why a dead keyboard can come from boot order,
controller commands, polling order, or focused-surface routing.

## Fast debug order

1. `src/kernel/kurono_kernel.cpp`
2. `src/system/input_manager.cpp`
3. `src/drivers/keyboard.cpp`
4. `src/drivers/mouse.cpp`
5. `src/ui/desktop.cpp`
6. `src/ui/window_manager.cpp`
7. the app window callback

## Mental model

A cascade. Drivers observe hardware, the input manager stabilises polling, the desktop environment
decides which surface has first claim, the WM owns frame interactions, and the focused window owns
the final behaviour. That's the whole route.
