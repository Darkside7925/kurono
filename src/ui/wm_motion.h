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

// ── physical springs (satoru) ─────────────────────────────────────────────
// a damped harmonic oscillator pulled toward t. SpringStep solves it in closed form
// for the elapsed time, so 60, 144 and 240 hz all land on the same curve and a
// retarget mid-flight keeps position and velocity (interruptible motion).
// response: seconds per undamped oscillation; damping: 1 = critical, below
// overshoots once a little. (satoru)
struct Spring1 { float x, v, t; };

static inline void SpringSet(Spring1& s, float v) { s.x = v; s.v = 0.0f; s.t = v; }
void SpringStep(Spring1& s, float response, float damping, float dt_s);
// at rest within eps of the target (velocity scaled by the same eps per second). (satoru)
bool SpringRest(const Spring1& s, float eps);

// small float helpers (no libm). (satoru)
float Exp(float x);      // accurate for x <= 0 (satoru)
float Sin(float x);
float Cos(float x);
float Sqrt(float x);

// ── premultiplied snapshots (satoru) ──────────────────────────────────────
// a capture whose pixels carry real coverage in the alpha byte (colour
// premultiplied), so rounded corners stay anti-aliased when the snapshot is
// scaled or moved. CapturePremul draws nothing itself: the caller has already
// drawn the window over black inside the rect. (satoru)
bool CapturePremul(Snapshot& s, int x, int y, int w, int h, int radius);

// save / restore what is under a rect of the frame being composed, so a window
// can be drawn, captured and taken away again within one frame. one shared
// buffer, so calls must not nest. (satoru)
bool SaveUnder(int x, int y, int w, int h);
void RestoreUnder();

// bilinear scaled composite of a premultiplied snapshot into the float rect
// (dx, dy, dw, dh), at opacity 0..256. subpixel positions are honoured, so
// slow motion does not step. (satoru)
void BlitSmooth(const Snapshot& s, float dx, float dy, float dw, float dh, int alpha256);

// the genie: the snapshot of a window at (wx, wy, ww, wh) drains into the
// target rect (tx, ty, tw, th) below it along a curved tube. p 0 = window,
// 1 = inside the target. (satoru)
void Genie(const Snapshot& s, int wx, int wy, int ww, int wh,
           float tx, float ty, float tw, float th, float p);

// soft drop shadow of a rounded rect: falloff `blur` px wide, offset down by
// dy, peak darkness alpha 0..255. hole skips the inside of the caster (only
// valid when the caster is opaque). (satoru)
void SoftShadow(float x, float y, float w, float h, float r, float blur, float dy, int alpha, bool hole);

// cycles spent since the last take: 0 shadows, 1 scaled blits, 2 genie. (satoru)
unsigned long long ProfTake(int i);

// bounds of what Genie touched last call (for damage). (satoru)
void LastGenieBounds(int& x0, int& y0, int& x1, int& y1);

}

// end (satoru)
