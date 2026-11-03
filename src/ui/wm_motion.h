#pragma once
#include "../kernel/types.h"

//  window motion toolkit: spring/bezier progress curves and the snapshot
//  blitter the window manager uses for open/close/minimize/restore. the
//  springs are precomputed damped-spring step responses (no libm in the
//  kernel), so motion is physical rather than a stock ease curve. progress is
//  Q12: 4096 = at rest, and springs may briefly exceed it (overshoot). (satoru)

namespace WMMotion {

enum SpringId { SPRING_OPEN_X, SPRING_OPEN_Y, SPRING_OPEN_W, SPRING_OPEN_H, SPRING_ICON };

// spring progress at t_ms of a response sampled over dur_ms. t clamps.
int Spring(SpringId id, int t_ms, int dur_ms);

// css-style cubic-bezier(x1, y1, x2, y2) progress at t_ms of dur_ms, 0..4096.
int Bezier(float x1, float y1, float x2, float y2, int t_ms, int dur_ms);

static inline int Lerp(int a, int b, int q12) {
    return a + (int)(((long long)(b - a) * q12) / 4096);
}

// per-channel ARGB mix, q12 = 0 gives a, 4096 gives b.
uint32_t MixColor(uint32_t a, uint32_t b, int q12);

// a window's pixels captured once from the frame being composed. pixels whose
// alpha byte is 0 are transparent (the area outside the rounded corners), so a
// moving snapshot never drags stale desktop pixels along with it.
struct Snapshot {
    uint32_t* px;
    int w, h;
};

bool Capture(Snapshot& s, int x, int y, int w, int h, int radius);
void Release(Snapshot& s);

// nearest-neighbour scaled copy of src rect (sx,sy,sw,sh) to dest rect
// (dx,dy,dw,dh), blended at alpha 0..255. clipped to the screen.
void Blit(const Snapshot& s, int sx, int sy, int sw, int sh,
          int dx, int dy, int dw, int dh, int alpha);

// rounded-rect fill blended at alpha 0..255. clipped to the screen.
void FillRoundAlpha(int x, int y, int w, int h, int r, uint32_t color, int alpha);

}
