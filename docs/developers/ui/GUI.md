# GUI helpers

`src/ui/gui.cpp` and `gui.h`. Widget-level drawing, built on top of the graphics driver.

The graphics driver gives you pixels, lines and rectangles. This layer gives you:

- rounded rectangles and panels, with optional shadows
- buttons, in normal, hover and pressed states
- text labels with left, centre or right alignment
- progress bars
- dividers and separators
- scroll indicators
- input field backgrounds

## Where it sits

| File | Level |
| --- | --- |
| `graphics.cpp` | pixel and geometric primitives |
| `font.cpp` | glyph rendering |
| `gui.cpp` | widget-level components |
| `ui_elements.cpp` | reusable higher-level pieces: sliders, checkboxes and so on |

Apps call `gui.cpp` or `ui_elements.cpp` for their rendering, not `graphics.cpp` directly.

## Colours

`uint32_t` in `0xAARRGGBB`. General UI colours come from `UIConfig` where they're
configurable, and themed code should read from there so `kurono reload` updates it live.

## Fonts

`Font::DrawString(x, y, text, color)` with a built-in bitmap font. No Unicode beyond basic
ASCII.

## Files

- `src/drivers/graphics.cpp` the pixel backend
- `src/ui/font.cpp` text rendering
- `src/ui/ui_elements.cpp` slider, checkbox and list components
- `src/system/ui_config.cpp` the colour source for themed widgets
