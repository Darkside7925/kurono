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
    uint32_t refresh_mhz; // what this output scans out, millihertz (0 unknown) (satoru)
    char     name[16];
    // optional per-output damage sink (the KLD display bridge). called with
    // the output-local rectangle after each scatter copy. (satoru)
    void (*aux_present)(int lx, int ly, int lw, int lh);
    bool     ram;         // PMM ram output: never WC-remap (the KLD scanout) (satoru)
    bool     kld;         // this slot is the display bridge output (removed on restore) (satoru)
    bool     bridge_fw;   // the firmware slot setup had to add (removed on restore) (satoru)
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
    static bool IsLive(int i);       // present and connected
    // what this output really scans out, for the frame pacer's ceiling. 0 when
    // the hardware did not report one. (satoru)
    static void SetRefreshMilliHz(int i, uint32_t mhz);
    static void SetAuxPresent(int i, void (*fn)(int, int, int, int));
    // the KLD output's container changed mode: new physical size + pitch. (satoru)
    static void SetGeometry(int i, int w, int h, uint32_t pitch);
    static void SetRam(int i, bool on);
    // restore a saved output list exactly (KLD setup/restore). (satoru)
    static void SetList(const Output* src, int n);
    // remove one output (the KLD bridge on restore); keeps the others and
    // re-runs the current layout. (satoru)
    static bool Remove(int i);
    // place one output in the physical arrangement, preserving the others
    // (mirror puts the bridge at 0,0; extend to the right of the desktop).
    static void PlaceCustom(int i, int ax, int ay);
    static uint32_t RefreshMilliHz(int i);
    static uint32_t MaxRefreshMilliHz();   // highest among the lit outputs

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
