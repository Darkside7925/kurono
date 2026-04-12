# GUI Drawing Helpers

`src/ui/gui.cpp` and `gui.h` provide higher-level UI drawing primitives built on top of the graphics driver.

## 1. What is here

Where the graphics driver provides raw geometric primitives (pixels, lines, rectangles), the GUI layer provides widget-level components:

- Rounded rectangles and panels with optional shadows
- Buttons (normal, hover, pressed states)
- Text labels with alignment (left, center, right)
- Progress bars
- Dividers and separators
- Scroll indicators
- Input field backgrounds

## 2. Relationship with other UI files

| File | Level |
| --- | --- |
| `graphics.cpp` | pixel/geometric primitives |
| `font.cpp` | glyph rendering |
| `gui.cpp` | widget-level components |
| `ui_elements.cpp` | reusable higher-level components (sliders, checkboxes, etc.) |

Every application calls into `gui.cpp` or `ui_elements.cpp` for its UI rendering rather than calling `graphics.cpp` directly.

## 3. Color conventions

All colors are passed as `uint32_t` in `0xAARRGGBB` format. Color values for general UI come from `UIConfig` where configurable. Applications that need themed colors should read from `UIConfig` so that `kurono reload` updates them live.

## 4. Font integration

The GUI layer calls `Font::DrawString(x, y, text, color)` for text output. The font is a built-in bitmap font. Unicode beyond basic ASCII is not currently supported.

## 5. Related files

- `src/drivers/graphics.cpp`  -  pixel-level backend
- `src/ui/font.cpp`  -  text rendering
- `src/ui/ui_elements.cpp`  -  slider, checkbox, list components
- `src/system/ui_config.cpp`  -  color source for themed widgets
