# Window manager

`src/ui/window_manager.cpp` and `window_manager.h`. The floating window system.

Every open window has a title bar with close, minimize and maximize buttons, a content area
the owning app renders into, drag-to-move and resize handles, and a place in the focus stack.

## Lifecycle

1. an app calls a launch function on `DesktopEnvironment`, e.g. `LaunchTerminal()`
2. that creates a window through the WM with a title, initial position and size
3. the WM assigns a window ID and puts it on the stack
4. each frame the WM calls the app's `Render(win, x, y, w, h)` with the content region
5. close gets clicked, the WM removes the window and calls the app's cleanup path

## Order

Rendered back to front in Z-order, focused window on top. Clicking a window that isn't on top
brings it forward.

## Title bar colours

From UIConfig:

| Key | Default |
| --- | --- |
| `window.titlebar_height` | 36 |
| `window.title_bg` | 0xFF1C1C2E |
| `window.title_focused` | 0xFF22223A |
| `window.title_text` | 0xFFF0F0F5 |
| `window.border_focus` | 0xFF6C8CFF |
| `window.close_btn` | 0xFFFF5F57 |
| `window.min_btn` | 0xFFFFBD2E |
| `window.max_btn` | 0xFF28C840 |

## Input

Every mouse event passes through the WM before desktop icons see anything. Hit areas get
checked in this order:

1. title bar drag
2. close / minimize / maximize buttons
3. resize handles
4. content area, forwarded to the app

## When it breaks

| Symptom | Cause |
| --- | --- |
| windows won't move | drag state never set, look at `HandleClick` in the WM |
| the wrong app gets clicks | Z-order not updated on the focus click |
| close does nothing | the app callback didn't return clean state |
| rendering artifacts | no clip region applied before the app render |

## Files

- `src/ui/desktop.cpp` calls WM render and input from inside DesktopEnvironment
- `src/ui/gui.cpp` what apps have available inside their content region
