//  kurono os  -  flat vector app icons drawn with graphics primitives. (satoru)
#include "app_icons.h"
#include "kss.h"
#include "../drivers/graphics.h"

namespace AppIcons {

// ── small local helpers ──────────────────────────────────────────────────────
// integer rounding scale: map a fraction num/den of `size` to pixels. keeps the
// geometry expressed as ratios of the box so one path scales to any size. (satoru)
static inline int sc(int size, int num, int den){
    return (size * num + den / 2) / den;
}
// clamp a stroke/length to at least 1px so thin features survive at ~30px. (satoru)
static inline int atleast1(int v){ return v < 1 ? 1 : v; }

// a thick line drawn as a filled run of parallel 1px lines, so strokes read at
// small sizes regardless of DrawLine's own thickness arg behavior. (satoru)
static void thick_line(int x0,int y0,int x1,int y1,int t,uint32_t c){
    if (t < 1) t = 1;
    Graphics::DrawLine(x0, y0, x1, y1, c, t);
}

// ── palette ───────────────────────────────────────────────────────────────
// the tiles behind these icons are already colored by the caller, so the glyph
// itself uses near-white ink + the theme accent for one or two highlights to
// stay crisp and on-brand against the dark/colored tiles. (satoru)
static const uint32_t INK      = 0xFFF2F4FA;  // primary glyph ink (satoru)
static const uint32_t INK_DIM  = 0xFFC4C8D4;  // secondary lines (satoru)
static const uint32_t INK_DARK = 0xFF14161E;  // dark fills (e.g. terminal screen) (satoru)
static const uint32_t SHADE    = 0xFF8C92A4;  // subtle shading / folder tab (satoru)

// terminal: dark rounded screen + a ">" prompt and a blinking-style cursor. (satoru)
static void draw_terminal(int x,int y,int s){
    int pad = sc(s,5,28);
    int sx = x + pad, sy = y + sc(s,7,28);
    int sw = s - pad*2, sh = s - sc(s,12,28);
    int r  = atleast1(sc(s,3,28));
    Graphics::FillRoundedRect(sx, sy, sw, sh, r, INK_DARK);
    // title strip with three dots (satoru)
    int strip_h = atleast1(sc(s,4,28));
    Graphics::FillRoundedRect(sx, sy, sw, strip_h, r, 0xFF2A2E3A);
    int dot = atleast1(sc(s,1,28));
    for (int i=0;i<3;i++)
        Graphics::FillCircle(sx + sc(s,3,28) + i*atleast1(sc(s,3,28)), sy + strip_h/2, dot, SHADE);
    // ">" prompt chevron drawn as two strokes (satoru)
    int t  = atleast1(sc(s,2,28));
    int px = sx + sc(s,4,28);
    int py = sy + strip_h + sc(s,4,28);
    int ph = sc(s,5,28);
    thick_line(px, py, px + ph, py + ph, t, KSS::Accent());
    thick_line(px + ph, py + ph, px, py + ph*2, t, KSS::Accent());
    // underscore cursor (satoru)
    int cw = sc(s,5,28);
    Graphics::FillRect(px + ph + sc(s,2,28), py + ph*2 - t, cw, t, INK);
}

// files: a folder with a raised tab and a slightly lighter front face. (satoru)
static void draw_files(int x,int y,int s){
    int fx = x + sc(s,5,28);
    int fw = s - sc(s,10,28);
    int top = y + sc(s,9,28);
    int bot = y + sc(s,21,28);
    int r   = atleast1(sc(s,2,28));
    // back tab (satoru)
    int tab_w = sc(s,9,28);
    Graphics::FillRoundedRect(fx, top - sc(s,3,28), tab_w, sc(s,5,28), r, SHADE);
    // folder body (satoru)
    Graphics::FillRoundedRect(fx, top, fw, bot - top, r, KSS::Accent());
    // front flap, a touch lighter, to give it depth (satoru)
    int flap_y = top + sc(s,3,28);
    Graphics::FillRoundedRect(fx + sc(s,1,28), flap_y, fw - sc(s,2,28), bot - flap_y, r, 0x66FFFFFF);
}

// calculator: a screen on top + a tidy grid of rounded buttons. (satoru)
static void draw_calculator(int x,int y,int s){
    int bx = x + sc(s,6,28);
    int bw = s - sc(s,12,28);
    int by = y + sc(s,5,28);
    int bh = s - sc(s,10,28);
    int r  = atleast1(sc(s,3,28));
    Graphics::FillRoundedRect(bx, by, bw, bh, r, INK);
    // display (satoru)
    int dpad = atleast1(sc(s,2,28));
    int dh   = sc(s,5,28);
    Graphics::FillRoundedRect(bx+dpad, by+dpad, bw-dpad*2, dh, atleast1(sc(s,1,28)), INK_DARK);
    // 3x3 keypad (satoru)
    int gx = bx + dpad;
    int gy = by + dpad*2 + dh;
    int gw = bw - dpad*2;
    int cell = gw / 3;
    int kb = atleast1(cell - atleast1(sc(s,2,28)));
    uint32_t acc = KSS::Accent();
    for (int row=0; row<3; row++){
        for (int col=0; col<3; col++){
            uint32_t kc = (col==2) ? acc : 0xFF6B7180;
            Graphics::FillRoundedRect(gx + col*cell, gy + row*cell, kb, kb,
                                      atleast1(sc(s,1,28)), kc);
        }
    }
}

// editor: a document sheet with a folded corner + a few text lines. (satoru)
static void draw_editor(int x,int y,int s){
    int dx = x + sc(s,7,28);
    int dw = s - sc(s,14,28);
    int dy = y + sc(s,4,28);
    int dh = s - sc(s,8,28);
    int r  = atleast1(sc(s,2,28));
    Graphics::FillRoundedRect(dx, dy, dw, dh, r, INK);
    // folded top-right corner (satoru)
    int fold = sc(s,6,28);
    Graphics::FillRect(dx + dw - fold, dy, fold, fold, SHADE);
    Graphics::FillRect(dx + dw - fold, dy, fold, atleast1(sc(s,1,28)), INK);
    // text lines (satoru)
    int lx = dx + sc(s,3,28);
    int lw = dw - sc(s,6,28);
    int t  = atleast1(sc(s,1,28));
    int ly = dy + sc(s,9,28);
    int step = sc(s,4,28); if (step < t+1) step = t+1;
    int widths[4] = { lw, (lw*7)/10, (lw*9)/10, (lw*5)/10 };
    for (int i=0;i<4;i++){
        if (ly + t > dy + dh - sc(s,2,28)) break;
        Graphics::FillRect(lx, ly, widths[i], t, INK_DIM);
        ly += step;
    }
}

// settings: a gear approximated by a ring with eight radial teeth + a hub. (satoru)
static void draw_settings(int x,int y,int s){
    int cx = x + s/2;
    int cy = y + s/2;
    int outer = sc(s,11,28);
    int inner = sc(s,8,28);
    int hub   = sc(s,4,28);
    uint32_t acc = KSS::Accent();
    // teeth as small rects at 8 compass points (satoru)
    int tw = atleast1(sc(s,3,28));
    int tl = sc(s,3,28);
    // cardinal teeth (satoru)
    Graphics::FillRect(cx - tw/2, cy - outer - tl + sc(s,2,28), tw, tl, INK);
    Graphics::FillRect(cx - tw/2, cy + outer - sc(s,2,28),      tw, tl, INK);
    Graphics::FillRect(cx - outer - tl + sc(s,2,28), cy - tw/2, tl, tw, INK);
    Graphics::FillRect(cx + outer - sc(s,2,28),      cy - tw/2, tl, tw, INK);
    // diagonal teeth as small squares (satoru)
    int d = sc(s,7,28);
    int sq = atleast1(sc(s,3,28));
    Graphics::FillRect(cx - d - sq/2, cy - d - sq/2, sq, sq, INK);
    Graphics::FillRect(cx + d - sq/2, cy - d - sq/2, sq, sq, INK);
    Graphics::FillRect(cx - d - sq/2, cy + d - sq/2, sq, sq, INK);
    Graphics::FillRect(cx + d - sq/2, cy + d - sq/2, sq, sq, INK);
    // ring (satoru)
    Graphics::FillCircle(cx, cy, outer, INK);
    Graphics::FillCircle(cx, cy, inner, INK_DARK);
    // accent hub (satoru)
    Graphics::FillCircle(cx, cy, hub, acc);
}

// browser: a globe  -  a filled circle with meridian + latitude lines. (satoru)
static void draw_browser(int x,int y,int s){
    int cx = x + s/2;
    int cy = y + s/2;
    int rad = sc(s,11,28);
    uint32_t acc = KSS::Accent();
    Graphics::FillCircle(cx, cy, rad, acc);
    // outline ring for crispness (satoru)
    Graphics::DrawCircle(cx, cy, rad, INK);
    // equator + two latitude lines (satoru)
    int t = atleast1(sc(s,1,28));
    Graphics::FillRect(cx - rad, cy - t/2, rad*2, t, INK);
    int ly = sc(s,5,28);
    Graphics::FillRect(cx - sc(s,9,28), cy - ly, sc(s,9,28)*2, t, INK);
    Graphics::FillRect(cx - sc(s,9,28), cy + ly, sc(s,9,28)*2, t, INK);
    // central vertical meridian + two curved side meridians drawn as half-width
    // vertical bands clipped to the disc (a circle-shaped mask via the row
    // half-width). gives the globe its longitude lines without DrawEllipse. (satoru)
    Graphics::FillRect(cx - t/2, cy - rad, t, rad*2, INK);
    int merid = sc(s,5,28);             // horizontal offset of side meridians (satoru)
    for (int dy = -rad; dy <= rad; dy++){
        // half-width of the disc at this row (circle equation) (satoru)
        int hw2 = rad*rad - dy*dy;
        if (hw2 <= 0) continue;
        int hw = 0; while ((hw+1)*(hw+1) <= hw2) hw++;
        // the side meridian sits at x-offset = merid * (hw/rad), tracing an arc (satoru)
        int off = (rad > 0) ? (merid * hw) / rad : 0;
        Graphics::FillRect(cx + off - t/2, cy + dy, t, 1, INK);
        Graphics::FillRect(cx - off - t/2, cy + dy, t, 1, INK);
    }
}

// tasks: an ascending bar chart (three rising bars). (satoru)
static void draw_tasks(int x,int y,int s){
    int base = y + s - sc(s,7,28);
    int bw   = sc(s,4,28);
    int gap  = sc(s,2,28);
    int x0   = x + sc(s,7,28);
    int heights[3] = { sc(s,7,28), sc(s,12,28), sc(s,16,28) };
    uint32_t cols[3] = { INK_DIM, INK, KSS::Accent() };
    int r = atleast1(sc(s,1,28));
    for (int i=0;i<3;i++){
        int bx = x0 + i*(bw+gap);
        Graphics::FillRoundedRect(bx, base - heights[i], bw, heights[i], r, cols[i]);
    }
    // baseline (satoru)
    Graphics::FillRect(x0 - sc(s,1,28), base, (bw+gap)*3, atleast1(sc(s,1,28)), INK_DIM);
}

// media: a play triangle inside a rounded square. (satoru)
static void draw_media(int x,int y,int s){
    int bx = x + sc(s,5,28);
    int by = y + sc(s,5,28);
    int bw = s - sc(s,10,28);
    int r  = atleast1(sc(s,4,28));
    Graphics::FillRoundedRect(bx, by, bw, bw, r, KSS::Accent());
    // play triangle (right-pointing) built from horizontal scanlines (satoru)
    int cx = bx + bw/2 - sc(s,1,28);
    int cy = by + bw/2;
    int half = sc(s,6,28);          // triangle half-height (satoru)
    int len  = sc(s,9,28);          // triangle width (satoru)
    int tx   = cx - len/2;
    for (int dy = -half; dy <= half; dy++){
        // width shrinks linearly toward the tip as |dy| grows (satoru)
        int run = len - (len * (dy<0?-dy:dy)) / (half>0?half:1);
        if (run <= 0) continue;
        Graphics::FillRect(tx, cy + dy, run, 1, INK);
    }
}

// home: a house  -  a roof triangle over a square body with a door. (satoru)
static void draw_home(int x,int y,int s){
    int cx = x + s/2;
    int roof_top = y + sc(s,5,28);
    int eaves_y  = y + sc(s,13,28);
    int half_w   = sc(s,11,28);
    uint32_t acc = KSS::Accent();
    // roof as stacked scanlines forming a triangle (satoru)
    for (int yy = roof_top; yy <= eaves_y; yy++){
        float f = (float)(yy - roof_top) / (float)(eaves_y - roof_top > 0 ? eaves_y - roof_top : 1);
        int hw = (int)(half_w * f + 0.5f);
        Graphics::FillRect(cx - hw, yy, hw*2, 1, acc);
    }
    // body (satoru)
    int body_w = sc(s,16,28);
    int body_x = cx - body_w/2;
    int body_y = eaves_y;
    int body_h = y + s - sc(s,5,28) - body_y;
    if (body_h < 1) body_h = 1;
    Graphics::FillRoundedRect(body_x, body_y, body_w, body_h, atleast1(sc(s,1,28)), INK);
    // door (satoru)
    int door_w = sc(s,5,28);
    int door_h = sc(s,8,28);
    int door_x = cx - door_w/2;
    int door_y = body_y + body_h - door_h;
    if (door_h > body_h - sc(s,1,28)) { door_h = body_h - sc(s,1,28); door_y = body_y + sc(s,1,28); }
    Graphics::FillRoundedRect(door_x, door_y, door_w, door_h, atleast1(sc(s,1,28)), acc);
}

// generic: a stylized app glyph  -  a rounded square with a smaller inset. (satoru)
static void draw_generic(int x,int y,int s){
    int bx = x + sc(s,6,28);
    int bw = s - sc(s,12,28);
    int r  = atleast1(sc(s,4,28));
    Graphics::FillRoundedRect(bx, y + sc(s,6,28), bw, bw, r, INK);
    int ipad = sc(s,4,28);
    Graphics::FillRoundedRect(bx + ipad, y + sc(s,6,28) + ipad, bw - ipad*2, bw - ipad*2,
                              atleast1(sc(s,2,28)), KSS::Accent());
}

void Draw(int id, int x, int y, int size){
    if (size <= 0) return;
    switch (id){
        case TERMINAL:   draw_terminal(x,y,size);   break;
        case FILES:      draw_files(x,y,size);       break;
        case CALCULATOR: draw_calculator(x,y,size);  break;
        case EDITOR:     draw_editor(x,y,size);      break;
        case SETTINGS:   draw_settings(x,y,size);    break;
        case BROWSER:    draw_browser(x,y,size);     break;
        case TASKS:      draw_tasks(x,y,size);       break;
        case MEDIA:      draw_media(x,y,size);       break;
        case HOME:       draw_home(x,y,size);        break;
        default:         draw_generic(x,y,size);     break;
    }
}

// case-sensitive char compare on the leading bytes  -  names come from our own
// fixed app/desktop tables so a short prefix test is enough. order matters:
// more-specific prefixes (e.g. "Co" for control center, "Te"/"Ta"/"Tex") are
// tested before broad single-letter ones. (satoru)
static bool pfx(const char* s, const char* p){
    if (!s || !p) return false;
    int i = 0;
    while (p[i]){ if (s[i] != p[i]) return false; i++; }
    return true;
}

int IdForName(const char* n){
    if (!n || !n[0]) return GENERIC;
    // terminal vs task manager vs text editor all start with 'T'. (satoru)
    if (pfx(n,"Ter")) return TERMINAL;
    if (pfx(n,"Tas")) return TASKS;
    if (pfx(n,"Tex")) return EDITOR;
    // "Control" (control panel/center window title) maps to settings. (satoru)
    if (pfx(n,"Co"))  return SETTINGS;
    if (pfx(n,"Cal")) return CALCULATOR;   // calculator (satoru)
    if (pfx(n,"Ca"))  return CALCULATOR;   // generic 'Ca' still a calculator (satoru)
    if (pfx(n,"Fi"))  return FILES;        // files / file browser (satoru)
    if (pfx(n,"Ed"))  return EDITOR;       // editor (satoru)
    if (pfx(n,"Se"))  return SETTINGS;     // settings (satoru)
    if (pfx(n,"Br"))  return BROWSER;      // browser (satoru)
    if (pfx(n,"Me"))  return MEDIA;        // media player (satoru)
    if (pfx(n,"Ho"))  return HOME;         // home (satoru)
    // single-letter fallbacks for window titles that carry only an initial. (satoru)
    switch (n[0]){
        case 'T': return TERMINAL;
        case 'F': return FILES;
        case 'C': return CALCULATOR;
        case 'E': return EDITOR;
        case 'S': return SETTINGS;
        case 'B': return BROWSER;
        case 'M': return MEDIA;
        case 'H': return HOME;
        default:  return GENERIC;
    }
}

} // namespace AppIcons
// end (satoru)
