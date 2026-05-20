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
- `SwapBuffers()`  -  swap back buffer to framebuffer with NT-store path
- `BeginFrame()` / `EndFrame()`  -  frame pacing and FPS tracking

## 2. Color format

Colors are `uint32_t` in `0xAARRGGBB` format throughout the codebase. The same format is used in `UIConfig` color keys.

## 3. Framebuffer setup

The graphics driver receives the framebuffer base address, width, height, and pitch from the display setup path during boot. The framebuffer pages are remapped to write-combining (WC) via PAT to ensure CPU stores reach GPU VRAM on real hardware. After setup, the driver operates independently. The display manager (`display_mgr.cpp`) handles mode selection and backend routing; the graphics driver handles all subsequent drawing.

## 4. Buffering and swap path

The graphics driver supports single, double, and triple buffering modes. When double or triple buffering is enabled, `SwapBuffers()` uses SSE2 non-temporal stores (`movntdq`) to blast the back buffer to the framebuffer without polluting CPU caches, followed by an `sfence` instruction. This is critical for real hardware where cached writes to WC memory may never become visible.

Dirty region tracking (up to 16 rectangles) allows partial updates when the full screen has not changed.

## 5. Performance notes

The framebuffer is write-combining memory. Random-access patterns (many small `PutPixel` calls) are slow. Batch operations like `FillRect` and `BlitRect` are significantly faster because they access memory linearly.

For large backgrounds (wallpaper, desktop gradient), the desktop code caches a pre-rendered pixel buffer and blits the whole thing at once rather than redrawing each frame.

Blend modes (alpha, additive, multiply) add overhead. The desktop uses opaque rendering where possible to avoid per-pixel alpha-blend overhead on the framebuffer hot path.

## 6. Common problems

| Problem | Likely cause |
| --- | --- |
| Rendering appears in wrong location | x/y origin wrong; check pitch vs width |
| Colors look inverted or wrong | Framebuffer is BGR not RGB |
| Tearing | No double-buffering; drawing while scanout is active |
| Black screen after drawing | Framebuffer pointer stale after mode switch or WC remap failed |
| Pixels never appear on real hardware | Missing WC remap; cached writes invisible to GPU |

## 7. Related files

- `src/drivers/display_mgr.cpp`  -  mode selection and framebuffer pointer setup
- `src/drivers/gpu_probe.cpp`  -  GPU detection for backend selection
- `src/ui/gui.cpp`  -  uses graphics primitives exclusively
- `src/ui/desktop.cpp`  -  uses blit path for wallpaper
