// kui_internal.h: what the shared kui code needs from a backend (kernel or ring 3) (satoru)
#pragma once
#include "kui.h"
#include "kui_raster.h"

namespace kui {

struct Canvas::Impl {
    raster::Target t;                 // target pixels + the current clip box (satoru)
    int ox, oy;                       // content origin inside the target (satoru)
    int cw, ch;                       // content size (satoru)
    Rect damage;                      // content-local repaint rect (satoru)
    struct Box { int x0, y0, x1, y1; };
    Box stack[16];                    // saved clip boxes (satoru)
    int sp;
};

// set up a canvas impl: target, origin, content size, and a base clip in absolute target coords (satoru)
void CanvasInit(Canvas::Impl& ci, uint32_t* px, int pitch, int tw, int th,
                int ox, int oy, int cw, int ch, int bx0, int by0, int bx1, int by1);

// double click tracking shared by both backends: returns 1 or 2 (satoru)
uint8_t ClickCount(int x, int y, uint32_t now);

namespace bk {
    // text in absolute target coordinates, clipped to ci's current clip box (satoru)
    int  TextDraw(Canvas::Impl& ci, int ax, int atop, Face f, float px, const char* s, Color c);
    int  TextWidth(Face f, float px, const char* s);
    int  TextAscent(Face f, float px);
    bool FontsOk();
    // point the kernel font code at a target for callers that draw text directly (satoru)
    void SetTextTarget(raster::Target* t);
    // the modern icon blob ("KICN" + count + size + premultiplied squares), nullptr if absent (satoru)
    const uint8_t* IconBlob(size_t* n);
}

}  // namespace kui

// end (satoru)
