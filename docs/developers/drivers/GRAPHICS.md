# Graphics Driver

`src/drivers/graphics.cpp` and `graphics.h` provide the framebuffer drawing primitives used by every part of the UI.

## 1. What it does

The graphics driver owns the framebuffer pointer and exposes a set of drawing operations that the entire UI layer calls. No UI code writes directly to the framebuffer  -  it goes through the graphics driver.

Provided operations (representative list):

- `PutPixel(x, y, color)`  -  write a single pixel
- `FillRect(x, y, w, h, color)`  -  solid rectangle
- `DrawRect(x, y, w, h, color)`  -  rectangle outline
- `DrawLine(x0, y0, x1, y1, color)`  -  line
- `BlitRect(dst_x, dst_y, src, src_x, src_y, w, h)`  -  blit from a pixel buffer
- `CopyToScreen(buf, x, y, w, h)`  -  copy a region to the live framebuffer

## 2. Color format

Colors are `uint32_t` in `0xAARRGGBB` format throughout the codebase. The same format is used in `UIConfig` color keys.

## 3. Framebuffer setup

The graphics driver receives the framebuffer base address, width, height, and pitch from the display setup path during boot. After that it operates independently. The display driver (`display.cpp`) handles mode selection; the graphics driver handles all subsequent drawing.

## 4. Performance notes

The framebuffer is typically uncached memory. Random-access patterns (many small `PutPixel` calls) are slow. Batch operations like `FillRect` and `BlitRect` are significantly faster because they access memory linearly.

For large backgrounds (wallpaper, desktop gradient), the desktop code caches a pre-rendered pixel buffer and blits the whole thing at once rather than redrawing each frame.

## 5. Common problems

| Problem | Likely cause |
| --- | --- |
| Rendering appears in wrong location | x/y origin wrong; check pitch vs width |
| Colors look inverted or wrong | Framebuffer is BGR not RGB |
| Tearing | No double-buffering; drawing while scanout is active |
| Black screen after drawing | Framebuffer pointer stale after mode switch |

## 6. Related files

- `src/drivers/display.cpp`  -  mode selection and framebuffer pointer setup
- `src/ui/gui.cpp`  -  uses graphics primitives exclusively
- `src/ui/desktop.cpp`  -  uses blit path for wallpaper
