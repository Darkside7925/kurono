# Graphics

`src/drivers/graphics.cpp` and `graphics.h`. The framebuffer drawing primitives every
part of the UI uses.

It owns the framebuffer pointer. Nothing in the UI writes to the framebuffer directly, it
all goes through here.

The operations, roughly:

- `PutPixel(x, y, color)`
- `FillRect(x, y, w, h, color)`
- `DrawRect(x, y, w, h, color)`
- `DrawLine(x0, y0, x1, y1, color)`
- `BlitRect(dst_x, dst_y, src, src_x, src_y, w, h)`
- `CopyToScreen(buf, x, y, w, h)`
- `SwapBuffers()`, the NT-store swap
- `BeginFrame()` and `EndFrame()` for pacing and FPS

## Colours

`uint32_t` in `0xAARRGGBB` everywhere, same format the `UIConfig` colour keys use.

## Setup

The display path hands over the framebuffer base, width, height and pitch during boot, and
the pages get remapped write-combining through PAT so CPU stores actually reach GPU VRAM
on real hardware. After that the driver runs on its own. Mode selection and backend
routing belong to the display manager, drawing belongs here.

## Buffering and the swap

Single, double or triple buffering. With double or triple on, `SwapBuffers()` uses SSE2
non-temporal stores (`movntdq`) plus an `sfence` to push the back buffer out without
polluting CPU caches. On real hardware that isn't an optimisation, it's required: cached
writes to WC memory may never become visible.

Dirty region tracking, up to 16 rectangles, for partial updates.

## Performance

The framebuffer is write-combining memory, so random access is slow. Lots of small
`PutPixel` calls hurt. `FillRect` and `BlitRect` are much faster because they walk memory
linearly.

Big backgrounds (wallpaper, the desktop gradient) get prerendered into a cached pixel
buffer and blitted in one go instead of being redrawn every frame.

Blend modes (alpha, additive, multiply) cost real time. The desktop renders opaque where
it can, to keep per-pixel alpha blending off the hot path.

## When it breaks

| Symptom | Cause |
| --- | --- |
| rendering lands in the wrong place | x/y origin wrong, check pitch against width |
| colours inverted or wrong | framebuffer is BGR, not RGB |
| tearing | no double buffering, drawing while scanout is live |
| black screen after drawing | framebuffer pointer stale after a mode switch, or the WC remap failed |
| pixels never show on real hardware | WC remap missing, cached writes are invisible to the GPU |

## Files

- `src/drivers/display_mgr.cpp` mode selection and the framebuffer pointer
- `src/drivers/gpu_probe.cpp` backend selection
- `src/ui/gui.cpp` uses these primitives and nothing lower
- `src/ui/desktop.cpp` uses the blit path for the wallpaper
