#include "outputs.h"

// display output list. pure bookkeeping: no hardware access here. (satoru)

Output Outputs::list[OUTPUT_MAX] = {};
int    Outputs::total = 0;
bool   Outputs::vertical = false;
bool   Outputs::custom = false;
int    Outputs::scale_pct = 100;
int    Outputs::inset_top = 0;
int    Outputs::inset_bottom = 0;

void Outputs::Reset() {
    for (int i = 0; i < OUTPUT_MAX; i++) list[i] = {};
    total = 0;
    vertical = false;
    custom = false;
    scale_pct = 100;
}

int Outputs::Add(uint64_t phys, int w, int h, uint32_t pitch, uint8_t bpp,
                 bool swap_rb, const char* name) {
    if (total >= OUTPUT_MAX || !phys || w <= 0 || h <= 0) return -1;
    Output& o = list[total];
    o = {};
    o.present = true;
    o.connected = true;
    o.swap_rb = swap_rb;
    o.w = w; o.h = h;
    o.pw = w; o.ph = h;
    o.fb = (uint8_t*)(uintptr_t)phys;
    o.phys = phys;
    o.pitch = pitch;
    o.bpp = bpp;
    int n = 0;
    if (name) while (name[n] && n < 15) { o.name[n] = name[n]; n++; }
    o.name[n] = 0;
    return total++;
}

int Outputs::Total() { return total; }

int Outputs::Connected() {
    int n = 0;
    for (int i = 0; i < total; i++) if (list[i].connected) n++;
    return n;
}

bool Outputs::Multi() { return Connected() > 1; }

const Output* Outputs::Get(int i) {
    if (i < 0 || i >= total) return nullptr;
    return &list[i];
}

bool Outputs::IsLive(int i) {
    return i >= 0 && i < total && list[i].present && list[i].connected;
}

void Outputs::SetAuxPresent(int i, void (*fn)(int, int, int, int)) {
    if (i < 0 || i >= total) return;
    list[i].aux_present = fn;
}

void Outputs::SetRam(int i, bool on) {
    if (i < 0 || i >= total) return;
    list[i].ram = on;
}

void Outputs::SetList(const Output* src, int n) {
    for (int i = 0; i < OUTPUT_MAX; i++) list[i] = {};
    total = 0;
    if (!src) return;
    if (n > OUTPUT_MAX) n = OUTPUT_MAX;
    for (int i = 0; i < n; i++)
        list[i] = src[i];
    total = n;
}

bool Outputs::Remove(int i) {
    if (i < 0 || i >= total) return false;
    for (int j = i; j < total - 1; j++) list[j] = list[j + 1];
    list[total - 1] = {};
    total--;
    Layout(vertical);
    return true;
}

void Outputs::PlaceCustom(int i, int ax, int ay) {
    if (i < 0 || i >= total) return;
    // the other slots already have ax/ay from the last layout; enable custom
    // and move only this one. (satoru)
    custom = true;
    list[i].ax = ax;
    list[i].ay = ay;
    LayoutCustom();
}

void Outputs::SetGeometry(int i, int w, int h, uint32_t pitch) {
    if (i < 0 || i >= total || w <= 0 || h <= 0) return;
    int pct = scale_pct < 100 ? 100 : scale_pct;
    list[i].pw = w;
    list[i].ph = h;
    list[i].w = (int)((long long)w * 100 / pct);
    list[i].h = (int)((long long)h * 100 / pct);
    list[i].pitch = pitch;
}

void Outputs::SetRefreshMilliHz(int i, uint32_t mhz) {
    if (i < 0 || i >= total) return;
    list[i].refresh_mhz = mhz;
}

uint32_t Outputs::RefreshMilliHz(int i) {
    if (i < 0 || i >= total) return 0;
    return list[i].refresh_mhz;
}

uint32_t Outputs::MaxRefreshMilliHz() {
    uint32_t m = 0;
    for (int i = 0; i < total; i++)
        if (list[i].present && list[i].connected && list[i].refresh_mhz > m)
            m = list[i].refresh_mhz;
    return m;
}

bool Outputs::Vertical() { return vertical; }

void Outputs::SetScale(int pct) {
    if (pct < 100) pct = 100;
    scale_pct = pct;
    for (int i = 0; i < total; i++) {
        list[i].w = (int)((long long)list[i].pw * 100 / pct);
        list[i].h = (int)((long long)list[i].ph * 100 / pct);
    }
}

void Outputs::Layout(bool vert) {
    vertical = vert;
    if (custom) { LayoutCustom(); return; }
    int cx = 0, cy = 0, px = 0, py = 0;
    for (int i = 0; i < total; i++) {
        if (!list[i].connected) continue;
        list[i].x = cx;
        list[i].y = cy;
        list[i].ax = px;
        list[i].ay = py;
        if (vert) { cy += list[i].h; py += list[i].ph; } else { cx += list[i].w; px += list[i].pw; }
    }
}

// free arrangement: physical placement -> logical px. rounding at a ui scale
// could open a 1 px seam (or overlap) between outputs that touch, so edges
// that touch in physical px are re-joined in logical px, then the union is
// moved back to the origin. (satoru)
void Outputs::LayoutCustom() {
    int pct = scale_pct < 100 ? 100 : scale_pct;
    for (int i = 0; i < total; i++) {
        if (!list[i].connected) continue;
        list[i].x = (int)((long long)list[i].ax * 100 / pct);
        list[i].y = (int)((long long)list[i].ay * 100 / pct);
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < total; i++) {
            Output& a = list[i];
            if (!a.connected) continue;
            for (int j = 0; j < total; j++) {
                const Output& b = list[j];
                if (j == i || !b.connected) continue;
                if (a.ax == b.ax + b.pw) a.x = b.x + b.w;
                if (a.ay == b.ay + b.ph) a.y = b.y + b.h;
                if (a.ax == b.ax) a.x = b.x;
                if (a.ay == b.ay) a.y = b.y;
            }
        }
    }
    int mx = 0, my = 0;
    bool first = true;
    for (int i = 0; i < total; i++) {
        if (!list[i].connected) continue;
        if (first || list[i].x < mx) mx = list[i].x;
        if (first || list[i].y < my) my = list[i].y;
        first = false;
    }
    for (int i = 0; i < total; i++) {
        if (!list[i].connected) continue;
        list[i].x -= mx;
        list[i].y -= my;
    }
}

void Outputs::Arrange(const int* ax, const int* ay) {
    if (!ax || !ay) { custom = false; return; }
    for (int i = 0; i < total; i++) { list[i].ax = ax[i]; list[i].ay = ay[i]; }
    custom = true;
    Layout(vertical);
}

bool Outputs::Custom() { return custom; }

void Outputs::Bounds(int* w, int* h) {
    int mw = 0, mh = 0;
    for (int i = 0; i < total; i++) {
        if (!list[i].connected) continue;
        if (list[i].x + list[i].w > mw) mw = list[i].x + list[i].w;
        if (list[i].y + list[i].h > mh) mh = list[i].y + list[i].h;
    }
    if (w) *w = mw;
    if (h) *h = mh;
}

// the largest buffer either layout (or any on/off subset) can need, so the
// virtual framebuffer can be allocated once and re-pitched later. (satoru)
void Outputs::CapacityBytes(size_t* bytes) {
    long long sw = 0, sh = 0, mw = 0, mh = 0;
    for (int i = 0; i < total; i++) {
        sw += list[i].w; sh += list[i].h;
        if (list[i].w > mw) mw = list[i].w;
        if (list[i].h > mh) mh = list[i].h;
    }
    long long a = sw * mh, b = mw * sh;
    if (a < b) a = b;
    *bytes = (size_t)(a * 4);
}

bool Outputs::SetConnected(int i, bool on) {
    if (i <= 0 || i >= total) return false;
    if (list[i].connected == on) return true;
    list[i].connected = on;
    Layout(vertical);
    return true;
}

bool Outputs::MakePrimary(int i) {
    if (i <= 0 || i >= total || !list[i].connected) return false;
    Output t = list[0];
    list[0] = list[i];
    list[i] = t;
    custom = false;   // a swapped slot 0 is no longer at its arranged origin (satoru)
    Layout(vertical);
    return true;
}

bool Outputs::Swap(int a, int b) {
    if (a < 0 || b < 0 || a >= total || b >= total || a == b) return false;
    if (!list[a].connected || !list[b].connected) return false;
    Output t = list[a];
    list[a] = list[b];
    list[b] = t;
    custom = false;   // swap means strip order (satoru)
    Layout(vertical);
    return true;
}

int Outputs::At(int px, int py) {
    for (int i = 0; i < total; i++) {
        const Output& o = list[i];
        if (!o.connected) continue;
        if (px >= o.x && px < o.x + o.w && py >= o.y && py < o.y + o.h) return i;
    }
    return -1;
}

static long long dist2_to_rect(const Output& o, int px, int py) {
    long long dx = 0, dy = 0;
    if (px < o.x) dx = o.x - px; else if (px >= o.x + o.w) dx = px - (o.x + o.w - 1);
    if (py < o.y) dy = o.y - py; else if (py >= o.y + o.h) dy = py - (o.y + o.h - 1);
    return dx * dx + dy * dy;
}

int Outputs::Nearest(int px, int py) {
    int best = 0;
    long long bd = -1;
    for (int i = 0; i < total; i++) {
        if (!list[i].connected) continue;
        long long d = dist2_to_rect(list[i], px, py);
        if (bd < 0 || d < bd) { bd = d; best = i; }
    }
    return best;
}

void Outputs::Constrain(int* px, int* py) {
    if (Connected() < 2) return;
    if (At(*px, *py) >= 0) return;
    const Output& o = list[Nearest(*px, *py)];
    if (*px < o.x) *px = o.x; else if (*px >= o.x + o.w) *px = o.x + o.w - 1;
    if (*py < o.y) *py = o.y; else if (*py >= o.y + o.h) *py = o.y + o.h - 1;
}

void Outputs::SetPrimaryInsets(int top, int bottom) {
    inset_top = top;
    inset_bottom = bottom;
}

void Outputs::WorkArea(int i, int* x, int* y, int* w, int* h) {
    if (!IsLive(i)) i = 0;
    if (i < 0 || i >= total) { *x = 0; *y = 0; *w = 0; *h = 0; return; }
    const Output& o = list[i];
    *x = o.x; *y = o.y; *w = o.w; *h = o.h;
    if (i == 0) {
        *y += inset_top;
        *h -= inset_top + inset_bottom;
        if (*h < 1) *h = o.h;
    }
}
// end (satoru)
