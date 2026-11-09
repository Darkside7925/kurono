#pragma once
#include "../kernel/types.h"

//  anti-aliased drawing for the modern ui style: rounded rects with vertical
//  gradients and alpha, soft shadows, circles, and gradient app tiles. corners
//  are 4x4 supersampled, so edges stay smooth where the classic primitives
//  stair-step. everything writes straight to the frame being composed,
//  clipped to the screen only, so callers keep it to shell chrome (top bar,
//  dock, menus, desktop) rather than window content. (satoru)

namespace MDraw {

// rounded rect filled with a vertical gradient (top -> bottom), blended at
// alpha 0..255 over what is already there.
void RoundRect(int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom, int alpha);

static inline void RoundRect(int x, int y, int w, int h, int r, uint32_t c, int alpha) {
    RoundRect(x, y, w, h, r, c, c, alpha);
}

// a one-pixel rounded outline, blended at alpha.
void RoundOutline(int x, int y, int w, int h, int r, uint32_t c, int alpha);

// soft drop shadow for a rounded rect: layered falloff `spread` px wide,
// offset down by dy, darkest at `alpha`.
void Shadow(int x, int y, int w, int h, int r, int spread, int dy, int alpha);

void Circle(int cx, int cy, int radius, uint32_t c, int alpha);

// opaque mix of a and b, t 0..255 (0 = a).
uint32_t Mix(uint32_t a, uint32_t b, int t);

// a modern app tile: gradient squircle, top sheen, hairline edge, soft shadow,
// with the AppIcons glyph on top. `size` is the tile side.
void AppTile(int id, int x, int y, int size);

// the tile's base colour (for colour handoffs such as the window open motion).
uint32_t AppTileColor(int id);

// ── shared shell surfaces (satoru) ───────────────────────────────────────
// one look for every popup in the shell (desktop menu, window menu, toasts),
// matching the settings app: light or dark from ui.appearance, glass panel,
// soft shadow, hairline edge, accent hover pill. (satoru)
struct Palette {
    uint32_t panel, panel2, line, text, text2, hover, danger, accent;
    bool dark;
};
const Palette& Pal();          // refreshed at most once per 250 ms (satoru)

// a context menu. items: "-" is a separator, a leading '!' marks a
// destructive row (red), a leading '~' a disabled one. rows start at y + top
// and are item_h tall (the caller keeps its own hit test). hover is the row
// under the pointer or -1. t: 0..1 open progress (scale + fade in). (satoru)
void Menu(int x, int y, int w, int top, int item_h, const char* const* items, int n, int hover, float t);

// a notification card at (x, y), w x h. kind: 0 info, 1 success, 2 warning,
// 3 error. alpha 0..255. (satoru)
void Toast(int x, int y, int w, int h, int kind, const char* title, const char* body, int alpha);

}
