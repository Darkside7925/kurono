#pragma once
// kurono os - display output list (multi-monitor) (satoru)
// one entry per physical screen the kernel can draw to directly (a gop
// framebuffer). the compositor renders into one virtual desktop and the graphics
// layer scatters each damaged rect to every output it overlaps. with exactly one
// output nothing here is active and the single-framebuffer path is untouched.
// output 0 is the primary: it always sits at the virtual origin and hosts the
// menu bar and dock; other outputs get their own full-size work area.
#include "../kernel/types.h"

#define OUTPUT_MAX 4

struct Output {
    bool     present;     // slot in use
    bool     connected;   // false = switched off (windows moved away, not drawn)
    bool     swap_rb;     // framebuffer is rgb-ordered, not bgr (gop pixel format 0)
    int      x, y, w, h;  // placement + size in the virtual desktop (logical px: physical * 100 / ui scale)
    int      pw, ph;      // physical pixels of the hardware framebuffer
    int      ax, ay;      // placement in physical px (strip or free arrangement); slot 0 at 0,0 (satoru)
    uint8_t* fb;          // linear framebuffer (identity mapped)
    uint64_t phys;
    uint32_t pitch;       // bytes per scanline of the hardware framebuffer
    uint8_t  bpp;
    char     name[16];
};

class Outputs {
public:
    static void Reset();
    // register an output; returns its index or -1 when the list is full
    static int  Add(uint64_t phys, int w, int h, uint32_t pitch, uint8_t bpp,
                    bool swap_rb, const char* name);
    static int  Total();                 // slots in use (connected or not)
    static int  Connected();             // outputs currently lit
    static bool Multi();                 // more than one connected output
    static const Output* Get(int i);     // nullptr when out of range
    static bool IsLive(int i);           // present and connected

    // lay every connected output out: horizontal = left to right in index order,
    // vertical = top to bottom. output 0 stays at the origin. (satoru)
    static void Layout(bool vertical);
    static bool Vertical();
    // free arrangement (settings > displays): per-slot ax/ay in physical px,
    // slot 0 at 0,0 and nothing above or left of it. Layout then keeps it
    // (scaled to logical px, touching edges kept touching) until a strip layout,
    // MakePrimary or Swap replaces it. nullptr drops back to the strip. (satoru)
    static void Arrange(const int* ax, const int* ay);
    static bool Custom();
    // ui scale (percent): logical size of every output = physical * 100 / pct. the
    // caller re-runs Layout afterwards. (satoru)
    static void SetScale(int pct);
    static void Bounds(int* w, int* h);  // size of the virtual desktop
    static void CapacityBytes(size_t* bytes); // worst case backing size for both layouts

    static bool SetConnected(int i, bool on);   // never turns off output 0
    static bool MakePrimary(int i);             // swap slot i with slot 0
    static bool Swap(int a, int b);             // exchange two slots (arrangement order) (satoru)

    // which output holds the point; -1 when it falls in no live output
    static int  At(int px, int py);
    static int  Nearest(int px, int py);        // always a live index
    // pull a point into the union of live outputs (cursor travel). no-op with
    // one output. (satoru)
    static void Constrain(int* px, int* py);
    // fills a work-area rect for output i: output 0 reserves the menu bar and
    // dock, others are the full output. falls back to the whole desktop.
    static void WorkArea(int i, int* x, int* y, int* w, int* h);
    // primary reservation set by the taskbar (top inset, bottom height)
    static void SetPrimaryInsets(int top, int bottom);

private:
    static Output list[OUTPUT_MAX];
    static int total;
    static bool vertical;
    static bool custom;      // free arrangement active (satoru)
    static int  scale_pct;   // last SetScale percent (satoru)
    static int inset_top, inset_bottom;
    static void LayoutCustom();
};
// end (satoru)
