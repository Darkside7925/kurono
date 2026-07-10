//  kurono os - control center implementation
#include "control_center.h"
#include "../drivers/graphics.h"
#include "../drivers/audio.h"
#include "../drivers/mouse.h"
#include "font.h"
#include "kss.h"
#include "../net/network.h"
#include "../system/user_mgmt.h"
#include "desktop.h"
#include "ui_elements.h"
#include "../kernel/time.h"

// state -------------------------------------------------------------------
bool ControlCenter::open = false;
int  ControlCenter::screen_w = 0;
int  ControlCenter::screen_h = 0;
int  ControlCenter::panel_x = 0;
int  ControlCenter::panel_y = 0;
int  ControlCenter::panel_w = 360;
int  ControlCenter::panel_h = 460;
bool ControlCenter::dragging_volume = false;
bool ControlCenter::dragging_brightness = false;
int  ControlCenter::brightness_pct = 80;
bool ControlCenter::airplane_mode = false;
bool ControlCenter::night_light   = false;
bool ControlCenter::focus_mode    = false;
bool ControlCenter::do_not_disturb = false;

int ControlCenter::wifi_x=0, ControlCenter::wifi_y=0, ControlCenter::wifi_w=0, ControlCenter::wifi_h=0;
int ControlCenter::bt_x=0, ControlCenter::bt_y=0, ControlCenter::bt_w=0, ControlCenter::bt_h=0;
int ControlCenter::air_x=0, ControlCenter::air_y=0, ControlCenter::air_w=0, ControlCenter::air_h=0;
int ControlCenter::night_x=0, ControlCenter::night_y=0, ControlCenter::night_w=0, ControlCenter::night_h=0;
int ControlCenter::focus_x=0, ControlCenter::focus_y=0, ControlCenter::focus_w=0, ControlCenter::focus_h=0;
int ControlCenter::dnd_x=0, ControlCenter::dnd_y=0, ControlCenter::dnd_w=0, ControlCenter::dnd_h=0;
int ControlCenter::bright_track_x=0, ControlCenter::bright_track_y=0, ControlCenter::bright_track_w=0;
int ControlCenter::vol_track_x=0, ControlCenter::vol_track_y=0, ControlCenter::vol_track_w=0;
int ControlCenter::signout_x=0, ControlCenter::signout_y=0, ControlCenter::signout_w=0, ControlCenter::signout_h=0;
int ControlCenter::settings_x=0, ControlCenter::settings_y=0, ControlCenter::settings_w=0, ControlCenter::settings_h=0;
int ControlCenter::lock_x=0, ControlCenter::lock_y=0, ControlCenter::lock_w=0, ControlCenter::lock_h=0;

// Per-tile animation state.  Indexed in the same order as kTileSlot below.
namespace {
    enum TileSlot {
        SLOT_WIFI = 0, SLOT_BT, SLOT_AIRPLANE, SLOT_NIGHT, SLOT_FOCUS, SLOT_DND,
        SLOT_LOCK, SLOT_SETTINGS, SLOT_SIGNOUT,
        SLOT_COUNT
    };

    // per-tile press timing only - the on/off colour state now lives in the kss
    // animation engine (keyed by a stable id), so the hand-rolled crossfade
    // bookkeeping is no longer needed here. (satoru)
    struct TileAnim {
        uint32_t tap_start_ms;   // 0 when no tap underway
    };
    TileAnim g_tiles[SLOT_COUNT] = {};

    // panel open/close animation
    uint32_t g_panel_anim_start = 0;
    bool     g_panel_anim_opening = false;
    bool     g_panel_animating = false;

    constexpr uint32_t TAP_DUR_MS    = 280;    // press window length on tile press
    constexpr uint32_t COLOR_DUR_MS  = 180;    // active/inactive colour crossfade
    constexpr uint32_t PANEL_DUR_MS  = 220;    // open/close timing

    // stable base id for kss::anim slots; per-tile colour id is base + slot so
    // each tile/slider/button eases independently. press ids are offset further
    // so the colour and scale tweens never share a slot. (satoru)
    constexpr uint32_t ANIM_ID_BASE  = 0xCC000000u;

    inline uint32_t NowMs() { return Time::GetTicks(); }

    inline void StartTap(int slot) {
        if (slot < 0 || slot >= SLOT_COUNT) return;
        g_tiles[slot].tap_start_ms = NowMs();
    }

    // smooth, engine-driven press scale for a tile/button: a subtle dip toward
    // 0.95 right after a tap that eases back to 1.0, with no snap on release - 
    // the scale value itself rides kss::anim so it never jumps. (satoru)
    inline float PressScale(int slot) {
        if (slot < 0 || slot >= SLOT_COUNT) return 1.0f;
        float target = 1.0f;
        if (g_tiles[slot].tap_start_ms) {
            uint32_t dt = NowMs() - g_tiles[slot].tap_start_ms;
            if (dt >= TAP_DUR_MS) {
                g_tiles[slot].tap_start_ms = 0;   // press window elapsed
            } else if (dt < TAP_DUR_MS / 2) {
                target = 0.95f;                   // pressed-in for the first half
            }
        }
        // distinct id per slot, offset from the colour id so they never collide. (satoru)
        return KSS::Anim::Float(ANIM_ID_BASE + 0x100u + (uint32_t)slot,
                                target, 140, KSS::Anim::OutCubic);
    }
}

// helpers -----------------------------------------------------------------
static int slen(const char* s){ int n=0; if(s) while(s[n]) n++; return n; }
static void scpy(char* d, const char* s, int mx){
    if (!d || mx <= 0) return;
    int i=0;
    if (s) while (s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static void int_to_str(int v, char* b, int mx){
    if (!b || mx < 2) { if (b && mx > 0) b[0] = 0; return; }
    if (v < 0) { b[0] = '-'; int_to_str(-v, b + 1, mx - 1); return; }
    char t[16]; int n = 0;
    do { t[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 15);
    int i = 0;
    while (n > 0 && i < mx - 1) b[i++] = t[--n];
    b[i] = 0;
}

// ---- cached quarter-circle alpha mask (8-bit), 1 LUT per radius (≤16) ----
// We support a small fixed set of radii used by the panel; everything goes
// through the lookup.  Mask[(dy*size)+dx] = 0..255 coverage of the
// rounded-corner edge for the top-left quadrant; other quadrants are mirrors.
static const int kMaskRadii[] = { 4, 8, 12, 16 };
static const int kNumMasks = 4;
static uint8_t g_corner_mask[kNumMasks][16 * 16]; // up to r=16
static bool    g_corner_mask_ready = false;

static int RadiusToIndex(int r) {
    int best = 0; int best_d = 0x7fffffff;
    for (int i = 0; i < kNumMasks; i++) {
        int d = r - kMaskRadii[i]; if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static void BuildCornerMasks() {
    if (g_corner_mask_ready) return;
    for (int m = 0; m < kNumMasks; m++) {
        int r = kMaskRadii[m];
        // Anti-aliased mask: for each pixel, sample 4 sub-pixel offsets and
        // count how many are inside the circle r.  This produces a 0..255
        // coverage map vs the previous binary edge.
        int r2 = r * r * 4; // compared against (2*dx)^2 + (2*dy)^2
        for (int dy = 0; dy < r; dy++) {
            for (int dx = 0; dx < r; dx++) {
                int cov = 0;
                // 2x2 sub-sample at +/- 0.25
                int sx[2] = { dx*2 + 0, dx*2 + 1 };
                int sy[2] = { dy*2 + 0, dy*2 + 1 };
                for (int i = 0; i < 2; i++)
                    for (int j = 0; j < 2; j++) {
                        int xx = sx[i] - 0; // 0..2r-1
                        int yy = sy[j] - 0;
                        // distance from corner centre (r,r) measured in half-pixels
                        int rx = (r * 2 - 1) - xx; // mirror so (0,0) is the corner cell
                        int ry = (r * 2 - 1) - yy;
                        if (rx * rx + ry * ry <= r2) cov++;
                    }
                g_corner_mask[m][dy * 16 + dx] = (uint8_t)((cov * 255) / 4);
            }
        }
    }
    g_corner_mask_ready = true;
}

// ---- cached vertical gradient ramp (small table) -------------------------
static uint32_t g_grad_ramp[64];
static uint32_t g_grad_top  = 0;
static uint32_t g_grad_bot  = 0;
static bool     g_grad_ready = false;

static void BuildGradient(uint32_t top, uint32_t bot) {
    if (g_grad_ready && top == g_grad_top && bot == g_grad_bot) return;
    g_grad_top = top; g_grad_bot = bot; g_grad_ready = true;
    int tr = (top >> 16) & 0xFF, tg = (top >> 8) & 0xFF, tb = top & 0xFF;
    int br = (bot >> 16) & 0xFF, bg = (bot >> 8) & 0xFF, bb = bot & 0xFF;
    for (int i = 0; i < 64; i++) {
        int s = (i * 256) / 63;
        int r = tr + ((br - tr) * s) / 256;
        int g = tg + ((bg - tg) * s) / 256;
        int b = tb + ((bb - tb) * s) / 256;
        g_grad_ramp[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
}

// Returns false if rect is fully outside the current viewport (caller can
// short-circuit further draw work for that primitive).
static bool RectVisible(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    int sw = Graphics::GetWidth();
    int sh = Graphics::GetHeight();
    if (x + w <= 0 || y + h <= 0) return false;
    if (x >= sw || y >= sh) return false;
    return true;
}

// Anti-aliased rounded rect using the cached corner mask.
// transform_scale: multiplies the rect inset (1.0 = original, 0.95 = press).
static void DrawRoundedTile(int x, int y, int w, int h, int r, uint32_t fill, float scale) {
    if (!g_corner_mask_ready) BuildCornerMasks();
    if (scale < 0.5f) scale = 0.5f;
    if (scale > 1.0f) scale = 1.0f;
    // Inset by (1 - scale) * w/2 horizontally, same vertically - keeps it
    // centered.  Round to nearest integer.
    int dx = (int)((1.0f - scale) * (float)w * 0.5f + 0.5f);
    int dy = (int)((1.0f - scale) * (float)h * 0.5f + 0.5f);
    x += dx; y += dy; w -= 2 * dx; h -= 2 * dy;
    if (!RectVisible(x, y, w, h)) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0) { Graphics::FillRect(x, y, w, h, fill); return; }

    int mi = RadiusToIndex(r);
    int mr = kMaskRadii[mi];
    if (mr != r) r = mr; // snap radius to the LUT we have

    uint32_t opaque = fill | 0xFF000000u;
    Graphics::FillRect(x + r, y,       w - 2 * r, r,         opaque);  // top strip (non-corner cols)
    Graphics::FillRect(x,     y + r,   w,         h - 2 * r, opaque);  // mid
    Graphics::FillRect(x + r, y + h - r, w - 2 * r, r,       opaque);  // bot strip

    uint8_t fr = (fill >> 16) & 0xFF;
    uint8_t fg = (fill >>  8) & 0xFF;
    uint8_t fb =  fill        & 0xFF;
    const uint8_t* mask = g_corner_mask[mi];
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            uint8_t cov = mask[j * 16 + i];
            if (cov == 0) continue;
            int tl_x = x + r - 1 - i, tl_y = y + r - 1 - j;
            int tr_x = x + w - r + i, tr_y = tl_y;
            int bl_x = tl_x, bl_y = y + h - r + j;
            int br_x = tr_x, br_y = bl_y;
            if (cov == 255) {
                Graphics::DrawPixel(tl_x, tl_y, opaque);
                Graphics::DrawPixel(tr_x, tr_y, opaque);
                Graphics::DrawPixel(bl_x, bl_y, opaque);
                Graphics::DrawPixel(br_x, br_y, opaque);
            } else {
                Graphics::BlendPixel(tl_x, tl_y, fr, fg, fb, cov);
                Graphics::BlendPixel(tr_x, tr_y, fr, fg, fb, cov);
                Graphics::BlendPixel(bl_x, bl_y, fr, fg, fb, cov);
                Graphics::BlendPixel(br_x, br_y, fr, fg, fb, cov);
            }
        }
    }
}

void ControlCenter::Init(int sw, int sh){
    screen_w = sw; screen_h = sh;
    open = false;
    BuildCornerMasks();
    BuildGradient(0xFF1B1B2E, 0xFF12121E);
    Layout();
    if (Audio::IsAvailable()) brightness_pct = 80;
    for (int i = 0; i < SLOT_COUNT; i++) g_tiles[i] = TileAnim{};
}

void ControlCenter::OnScreenResize(int sw, int sh){
    screen_w = sw; screen_h = sh;
    Layout();
}

void ControlCenter::Layout(){
    panel_w = 360;
    panel_h = 460;
    panel_x = screen_w - panel_w - 12;
    if (panel_x < 12) panel_x = 12;
    panel_y = screen_h - panel_h - 56;
    if (panel_y < 12) panel_y = 12;
}

void ControlCenter::Open(){
    Layout();
    if (!open) { g_panel_anim_start = NowMs(); g_panel_anim_opening = true; g_panel_animating = true; }
    open = true;
}
void ControlCenter::OpenAt(int anchor_x, int anchor_y){
    panel_w = 360; panel_h = 460;
    panel_x = anchor_x - panel_w / 2;
    panel_y = anchor_y - panel_h - 8;
    if (panel_x < 10) panel_x = 10;
    if (panel_x > screen_w - panel_w - 10) panel_x = screen_w - panel_w - 10;
    if (panel_y < 10) panel_y = 10;
    if (panel_y > screen_h - panel_h - 10) panel_y = screen_h - panel_h - 10;
    if (!open) { g_panel_anim_start = NowMs(); g_panel_anim_opening = true; g_panel_animating = true; }
    open = true;
}
void ControlCenter::ToggleAt(int ax, int ay){ if (open) Close(); else OpenAt(ax, ay); }
void ControlCenter::Close(){ open = false; dragging_volume = false; dragging_brightness = false; }
void ControlCenter::Toggle(){ if (open) Close(); else Open(); }
bool ControlCenter::IsOpen(){ return open; }
bool ControlCenter::IsAnimating(){ return open || g_panel_animating; }
int  ControlCenter::GetX(){ return panel_x; }
int  ControlCenter::GetY(){ return panel_y; }
int  ControlCenter::GetW(){ return panel_w; }
int  ControlCenter::GetH(){ return panel_h; }

// rendering helpers -------------------------------------------------------
// slot index passed in so we can track per-tile animation.
static void DrawTileAnimated(int slot, int x, int y, int w, int h,
                             const char* label, const char* sub,
                             bool active, uint32_t accent) {
    // themed: inactive tiles are the raised surface, active tiles fill with the
    // user accent for a cohesive black/grey + accent look. (satoru)
    uint32_t surf = KSS::T().surface_hi;
    uint32_t acc  = KSS::Accent();
    (void)accent;
    // colour crossfade now rides the kss anim engine: it seeds at the target on
    // first sight (no jump) and eases from the live blended colour whenever the
    // on/off state flips. distinct stable id per tile so they tween apart. (satoru)
    uint32_t bg = KSS::Anim::Color(ANIM_ID_BASE + (uint32_t)slot,
                                   active ? acc : surf,
                                   COLOR_DUR_MS, KSS::Anim::OutCubic);

    // tactile press - engine-driven scale, eases in and back out with no snap. (satoru)
    float scale = PressScale(slot);

    DrawRoundedTile(x, y, w, h, 12, bg, scale);

    // Compute the visible (scaled) rect to position the inner content.
    int dxp = (int)((1.0f - scale) * (float)w * 0.5f + 0.5f);
    int dyp = (int)((1.0f - scale) * (float)h * 0.5f + 0.5f);
    int rx = x + dxp, ry = y + dyp, rw = w - 2 * dxp, rh = h - 2 * dyp;

    // top highlight band, also animated
    Graphics::FillRectRounded(rx + 1, ry + 1, rw - 2, 2, 1,
                              active ? 0x40FFFFFF : 0x10FFFFFF);

    if (!active) Graphics::FillCircle(rx + 14, ry + rh / 2, 5, acc);

    uint32_t txt = active ? KSS::T().white : KSS::T().text;
    uint32_t sub_col = active ? 0xCCFFFFFF : KSS::T().text_dim;
    int tx = rx + 28;
    if (label) Graphics::DrawString(tx, ry + 8, label, txt, 0xFF000000);
    if (sub)   Graphics::DrawString(tx, ry + rh - 16, sub, sub_col, 0xFF000000);
}

void ControlCenter::DrawTile(int x, int y, int w, int h, const char* label,
                             const char* sub, bool active, uint32_t accent){
    // legacy entry - fall through with no slot (no animation)
    uint32_t bg = active ? accent : 0xFF1E1E2E;
    if (!RectVisible(x, y, w, h)) return;
    Graphics::FillRoundedRect(x, y, w, h, 12, bg);
    Graphics::FillRoundedRect(x+1, y+1, w-2, 2, 1, active ? 0x40FFFFFF : 0x10FFFFFF);
    if (!active) Graphics::FillCircle(x + 14, y + h/2, 5, accent);
    uint32_t txt = active ? 0xFFFFFFFF : 0xFFE0E0F0;
    uint32_t sub_col = active ? 0xCCFFFFFF : 0xFF7878A0;
    int tx = x + 28;
    if (label) Graphics::DrawString(tx, y + 8, label, txt, 0xFF000000);
    if (sub)   Graphics::DrawString(tx, y + h - 16, sub, sub_col, 0xFF000000);
}

void ControlCenter::DrawSlider(int x, int y, int w, const char* label, int pct, uint32_t fill){
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    if (w < 16) return;
    if (label) Graphics::DrawString(x, y, label, KSS::T().text_dim, 0xFF000000);
    int track_y = y + 18;
    int track_h = 8;
    Graphics::FillRoundedRect(x, track_y, w, track_h, 4, KSS::T().track);
    int fw = (pct * w) / 100;
    if (fw > 0) Graphics::FillRoundedRect(x, track_y, fw, track_h, 4, fill);
    int knob_x = x + fw - 8;
    if (knob_x < x) knob_x = x;
    if (knob_x > x + w - 16) knob_x = x + w - 16;
    Graphics::FillCircle(knob_x + 8, track_y + track_h/2, 8, KSS::T().white);

    char pct_buf[8] = {0};
    int_to_str(pct, pct_buf, 6);
    int p = slen(pct_buf);
    if (p < 6) { pct_buf[p] = '%'; pct_buf[p+1] = 0; }
    // measure proper proportional width instead of assuming 8px/char. (satoru)
    int pw = FontTTF::Measure(KSS::BodyPx(), pct_buf);
    Graphics::DrawString(x + w - pw, y, pct_buf, KSS::T().text, 0xFF000000);
}

void ControlCenter::DrawUserCard(int x, int y, int w){
    if (w <= 0) return;
    Graphics::FillRoundedRect(x, y, w, 64, 12, KSS::T().surface_hi);

    int user_count = UserManager::GetUserCount();
    User* u = nullptr;
    if (user_count > 0) {
        int active_idx = UserManager::GetCurrentUserIndex();
        if (active_idx < 0 || active_idx >= user_count) active_idx = 0;
        u = &UserManager::users[active_idx];
    }

    uint32_t accent = (u && u->accent_color) ? u->accent_color : 0xFF5C8AFF;
    Graphics::FillCircle(x + 32, y + 32, 22, accent);

    char init[2] = { 'U', 0 };
    if (u) {
        if (u->display_name[0]) init[0] = u->display_name[0];
        else if (u->username[0]) init[0] = u->username[0];
    }
    if (init[0] >= 'a' && init[0] <= 'z') init[0] = (char)(init[0] - 32);
    float fs = 22.0f;
    int tw = FontTTF::Measure(fs, init);
    FontTTF::DrawString(x + 32 - tw/2, y + 32 + (int)(fs * 0.27f), fs, init, 0xFFFFFFFF);

    const char* name = "User";
    if (u) {
        if (u->display_name[0]) name = u->display_name;
        else if (u->username[0]) name = u->username;
    }
    Graphics::DrawString(x + 64, y + 14, name, KSS::T().text, 0xFF000000);

    char uline[40] = {0};
    if (u){
        scpy(uline, "@", sizeof(uline));
        int n = slen(uline);
        for (int i = 0; u->username[i] && n < (int)sizeof(uline) - 1; i++) uline[n++] = u->username[i];
        uline[n] = 0;
    } else {
        scpy(uline, "Guest session", sizeof(uline));
    }
    Graphics::DrawString(x + 64, y + 32, uline, KSS::T().text_dim, 0xFF000000);

    int btn_y = y + 76;
    int bw = (w - 24) / 3;
    if (bw < 1) bw = 1;
    lock_x = x; lock_y = btn_y; lock_w = bw; lock_h = 30;
    settings_x = x + bw + 12; settings_y = btn_y; settings_w = bw; settings_h = 30;
    signout_x = x + 2*(bw + 12); signout_y = btn_y; signout_w = bw; signout_h = 30;

    // action buttons share the engine-driven press scale so they ease smoothly
    // in and back out with no snap, matching the toggle tiles. (satoru)
    DrawRoundedTile(lock_x,     lock_y,     lock_w,     lock_h, 8, KSS::T().surface_hi, PressScale(SLOT_LOCK));
    DrawRoundedTile(settings_x, settings_y, settings_w, settings_h, 8, KSS::T().surface_hi, PressScale(SLOT_SETTINGS));
    DrawRoundedTile(signout_x,  signout_y,  signout_w,  signout_h, 8, 0xFFE0584E, PressScale(SLOT_SIGNOUT));

    // centered labels measured via fontttf (no fixed 8px/char assumption). (satoru)
    float bp = KSS::BodyPx();
    FontTTF::DrawStringCenter(lock_x + lock_w/2,         lock_y + 9,     bp, "Lock",     KSS::T().text);
    FontTTF::DrawStringCenter(settings_x + settings_w/2, settings_y + 9, bp, "Settings", KSS::T().text);
    FontTTF::DrawStringCenter(signout_x + signout_w/2,   signout_y + 9,  bp, "Sign Out", KSS::T().white);
}

void ControlCenter::DrawHeader(int x, int y, int w) {
    (void)w;
    // larger heading in the theme heading color for a modern panel title. (satoru)
    FontTTF::DrawString(x, y - 2, 20.0f, "Control Center", KSS::T().heading);
}

// main render -------------------------------------------------------------
void ControlCenter::Render(){
    if (!open) return;
    Layout();
    if (!RectVisible(panel_x, panel_y, panel_w, panel_h)) return;

    // panel open animation: slide-up + fade
    int draw_x = panel_x;
    int draw_y = panel_y;
    if (g_panel_animating) {
        uint32_t dt = NowMs() - g_panel_anim_start;
        if (dt >= PANEL_DUR_MS) {
            g_panel_animating = false;
        } else {
            float t = Animation::EaseMs(dt, PANEL_DUR_MS, Animation::EaseInOutQuint);
            if (!g_panel_anim_opening) t = 1.0f - t;
            // start 24 px lower for a slide-up
            int offset = (int)((1.0f - t) * 24.0f + 0.5f);
            draw_y += offset;
        }
    }

    // soft drop shadow
    Graphics::ApplyShadow(draw_x, draw_y, panel_w, panel_h, 0, 8, 110);
    // body (themed card) + hairline border
    Graphics::FillRoundedRect(draw_x, draw_y, panel_w, panel_h, 16, KSS::T().surface);
    Graphics::DrawRect(draw_x, draw_y, panel_w, panel_h, KSS::T().border);
    // top accent bar
    Graphics::FillRect(draw_x + 16, draw_y + 1, panel_w - 32, 2, KSS::Accent());

    int x = draw_x + 16;
    int y = draw_y + 18;
    int inner_w = panel_w - 32;
    if (inner_w < 16) return;

    DrawHeader(x, y, inner_w);
    y += 24;

    int tile_w = (inner_w - 12) / 2;
    int tile_h = 64;
    int gap = 12;
    if (tile_w < 16) tile_w = 16;

    bool wifi_on = WiFi::IsLinkUp();
    const char* wifi_sub = wifi_on ? "Connected" : "Off";
    wifi_x = x;                wifi_y = y; wifi_w = tile_w; wifi_h = tile_h;
    bt_x   = x + tile_w + gap; bt_y   = y; bt_w   = tile_w; bt_h   = tile_h;
    DrawTileAnimated(SLOT_WIFI, wifi_x, wifi_y, wifi_w, wifi_h, "Wi-Fi", wifi_sub, wifi_on, 0xFF5C8AFF);
    DrawTileAnimated(SLOT_BT,   bt_x,   bt_y,   bt_w,   bt_h,   "Bluetooth", "Off", false,    0xFF3498DB);
    y += tile_h + gap;

    air_x   = x;                air_y   = y; air_w   = tile_w; air_h   = tile_h;
    night_x = x + tile_w + gap; night_y = y; night_w = tile_w; night_h = tile_h;
    DrawTileAnimated(SLOT_AIRPLANE, air_x,   air_y,   air_w,   air_h,   "Airplane",    airplane_mode  ? "On" : "Off", airplane_mode,  0xFFE67E22);
    DrawTileAnimated(SLOT_NIGHT,    night_x, night_y, night_w, night_h, "Night Light", night_light    ? "On" : "Off", night_light,    0xFFF39C12);
    y += tile_h + gap;

    focus_x = x;                focus_y = y; focus_w = tile_w; focus_h = tile_h;
    dnd_x   = x + tile_w + gap; dnd_y   = y; dnd_w   = tile_w; dnd_h   = tile_h;
    DrawTileAnimated(SLOT_FOCUS, focus_x, focus_y, focus_w, focus_h, "Focus",          focus_mode     ? "On" : "Off", focus_mode,     0xFF9B59B6);
    DrawTileAnimated(SLOT_DND,   dnd_x,   dnd_y,   dnd_w,   dnd_h,   "Do Not Disturb", do_not_disturb ? "On" : "Off", do_not_disturb, 0xFFE74C3C);
    y += tile_h + 18;

    bright_track_x = x; bright_track_y = y + 18; bright_track_w = inner_w;
    DrawSlider(x, y, inner_w, "Brightness", brightness_pct, KSS::Accent());
    y += 38;

    int vol_pct = Audio::IsAvailable() ? Audio::GetMasterVolume() : 80;
    if (vol_pct < 0) vol_pct = 0;
    if (vol_pct > 100) vol_pct = 100;
    if (Audio::IsAvailable() && Audio::IsMuted()) vol_pct = 0;
    vol_track_x = x; vol_track_y = y + 18; vol_track_w = inner_w;
    DrawSlider(x, y, inner_w, "Volume", vol_pct, KSS::Accent());
    y += 42;

    DrawUserCard(x, y, inner_w);
}

// click handling ----------------------------------------------------------
static bool point_in(int mx, int my, int x, int y, int w, int h){
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

bool ControlCenter::HandleClick(int mx, int my){
    if (!open) return false;

    if (!point_in(mx, my, panel_x, panel_y, panel_w, panel_h)){
        Close();
        return false;
    }

    if (point_in(mx, my, wifi_x, wifi_y, wifi_w, wifi_h)){
        StartTap(SLOT_WIFI);
        if (WiFi::IsLinkUp()) WiFi::Disable(); else WiFi::Enable();
        return true;
    }
    if (point_in(mx, my, bt_x, bt_y, bt_w, bt_h)){
        StartTap(SLOT_BT);
        return true;
    }
    if (point_in(mx, my, air_x, air_y, air_w, air_h)){
        StartTap(SLOT_AIRPLANE);
        airplane_mode = !airplane_mode;
        if (airplane_mode){ WiFi::Disable(); }
        return true;
    }
    if (point_in(mx, my, night_x, night_y, night_w, night_h)){
        StartTap(SLOT_NIGHT);
        night_light = !night_light;
        return true;
    }
    if (point_in(mx, my, focus_x, focus_y, focus_w, focus_h)){
        StartTap(SLOT_FOCUS);
        focus_mode = !focus_mode;
        return true;
    }
    if (point_in(mx, my, dnd_x, dnd_y, dnd_w, dnd_h)){
        StartTap(SLOT_DND);
        do_not_disturb = !do_not_disturb;
        return true;
    }

    if (bright_track_w > 0
        && my >= bright_track_y - 8 && my <= bright_track_y + 16
        && mx >= bright_track_x && mx <= bright_track_x + bright_track_w){
        int rel = mx - bright_track_x;
        int pct = (rel * 100) / bright_track_w;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        brightness_pct = pct;
        return true;
    }

    if (vol_track_w > 0
        && my >= vol_track_y - 8 && my <= vol_track_y + 16
        && mx >= vol_track_x && mx <= vol_track_x + vol_track_w){
        int rel = mx - vol_track_x;
        int v = (rel * 100) / vol_track_w;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        if (Audio::IsAvailable()){
            Audio::SetMasterVolume(v);
            Audio::SetMuted(false);
        }
        return true;
    }

    if (point_in(mx, my, signout_x, signout_y, signout_w, signout_h)){
        StartTap(SLOT_SIGNOUT);
        Close();
        DesktopEnvironment::RequestLogout();
        return true;
    }
    if (point_in(mx, my, settings_x, settings_y, settings_w, settings_h)){
        StartTap(SLOT_SETTINGS);
        Close();
        DesktopEnvironment::LaunchSettings();
        return true;
    }
    if (point_in(mx, my, lock_x, lock_y, lock_w, lock_h)){
        StartTap(SLOT_LOCK);
        Close();
        DesktopEnvironment::RequestLogout();
        return true;
    }

    return true;
}
