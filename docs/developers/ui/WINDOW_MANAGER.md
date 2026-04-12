# Window Manager

`src/ui/window_manager.cpp` and `window_manager.h` implement the floating window system.

## 1. What it provides

The window manager (WM) maintains a list of open windows. Each window has:

- A title bar with close, minimize, and maximize buttons.
- A content area that the owning application renders into.
- Drag-to-move and resize handles.
- Z-order (focus stack).

## 2. Window lifecycle

1. An application calls a launch function in `DesktopEnvironment` (e.g., `LaunchTerminal()`).
2. The launch function creates a new window via the WM with a title, initial position, and size.
3. The WM assigns a window ID and adds it to the stack.
4. Each frame the WM calls the application's `Render(win, x, y, w, h)` callback with the content region.
5. When the close button is clicked, the WM removes the window and calls the application's cleanup path.

## 3. Rendering order

Windows are rendered in Z-order from back to front. The focused window is always on top. Clicking a window that is not on top brings it to the front.

## 4. Title bar aesthetics

Title bar colors come from UIConfig:

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

## 5. Keyboard and mouse routing

All mouse events pass through the WM before reaching desktop icons. The WM checks hit areas in the following order:

1. Title bar drag
2. Close / minimize / maximize buttons
3. Resize handles
4. Content area (forwarded to application)

## 6. Common problems

| Problem | Likely cause |
| --- | --- |
| Windows can't be moved | Drag state not set; check `HandleClick` in WM |
| Wrong app gets clicks | Z-order not updated on focus click |
| Close button does nothing | Application callback not returning clean state |
| Rendering artifacts | Clip region not applied before application render |

## 7. Related files

- `src/ui/desktop.cpp`  -  calls WM render and input from within DesktopEnvironment
- `src/ui/gui.cpp`  -  drawing primitives available to all applications inside their content region
