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

}
