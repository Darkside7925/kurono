//  kurono os - settings control kit: theme, anti-aliased clipped primitives,
//  vector icons, time based springs, and the row controls every settings page
//  is built from. one code path lays out, draws, hit-tests and searches, so the
//  hit boxes always match the pixels. (satoru)
#include "settings_kit.h"
#include "../drivers/graphics.h"
#include "../drivers/timer.h"
#include "../ui/font.h"
#include "../kernel/heap.h"
#include "../system/ui_config.h"

namespace SKit {

Ctx C;

// ════════════════════════════════════════════════════════════════════════════
//  small helpers
// ════════════════════════════════════════════════════════════════════════════
static int  k_len(const char* s){ int n = 0; if(s) while(s[n]) n++; return n; }
static int  k_min(int a, int b){ return a < b ? a : b; }
static int  k_max(int a, int b){ return a > b ? a : b; }
static int  k_clamp(int v, int lo, int hi){ return v < lo ? lo : (v > hi ? hi : v); }
static float f_clamp(float v, float lo, float hi){ return v < lo ? lo : (v > hi ? hi : v); }
static float f_abs(float v){ return v < 0 ? -v : v; }
static float f_sqrt(float v){ return v > 0.0f ? __builtin_sqrtf(v) : 0.0f; }
static float f_min(float a, float b){ return a < b ? a : b; }
static float f_max(float a, float b){ return a > b ? a : b; }

static char lower(char c){ return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

bool Match(const char* hay, const char* needle){
    if(!needle || !needle[0]) return true;
    if(!hay) return false;
    for(int i = 0; hay[i]; i++){
        int k = 0;
        while(needle[k] && hay[i + k] && lower(hay[i + k]) == lower(needle[k])) k++;
        if(!needle[k]) return true;
    }
    return false;
}

static inline uint32_t mix(uint32_t a, uint32_t b, int t){
    if(t <= 0) return a | 0xFF000000u;
    if(t >= 255) return b | 0xFF000000u;
    uint32_t t256 = (uint32_t)t + (t >> 7);
    uint32_t rb = (((a & 0xFF00FFu) * (256 - t256) + (b & 0xFF00FFu) * t256) >> 8) & 0xFF00FFu;
    uint32_t g  = (((a & 0x00FF00u) * (256 - t256) + (b & 0x00FF00u) * t256) >> 8) & 0x00FF00u;
    return 0xFF000000u | rb | g;
}
static int luma(uint32_t c){
    return (int)((((c >> 16) & 0xFF) * 299 + ((c >> 8) & 0xFF) * 587 + (c & 0xFF) * 114) / 1000);
}

uint32_t Fade(uint32_t c){
    if(C.alpha >= 255) return c;
    return mix(C.under, c, C.alpha);
}

// ════════════════════════════════════════════════════════════════════════════
//  theme
// ════════════════════════════════════════════════════════════════════════════
static SettingsUI::Theme g_theme;
static const SettingsUI::Theme kDark = {
    0xFF18181B, 0xFF1F1F23, 0xFF232328, 0xFF2E2E35, 0xFF2D2D33, 0xFF2B2B31, 0xFF35353C,
    0xFFF2F2F4, 0xFFA1A1AA, 0xFF6E6E78,
    0xFF404048, 0xFFFFFFFF, 0xFF2C2C32, 0xFF3A3A42,
    0, 0, 0,
    0xFFFF5F57, 0xFF32D74B, 0xFFFFB340, true
};
static const SettingsUI::Theme kLight = {
    0xFFF6F6F8, 0xFFECECF0, 0xFFFFFFFF, 0xFFE3E3E8, 0xFFEBEBEF, 0xFFF3F3F6, 0xFFE8E8ED,
    0xFF1C1C1E, 0xFF6B6B73, 0xFF9D9DA5,
    0xFFD9D9DF, 0xFFFFFFFF, 0xFFF0F0F3, 0xFFD9D9DF,
    0, 0, 0,
    0xFFE5484D, 0xFF2DA44E, 0xFFD97706, false
};
static bool g_theme_anim = false;

void ThemeUpdate(){
    const char* ap = UIConfig::Str("ui.appearance", "dark");
    bool light = ap && (ap[0] == 'l' || ap[0] == 'L');
    float t = SettingsUI::Spring(0x7E3E0001u, light ? 1.0f : 0.0f, 2);
    int q = (int)(f_clamp(t, 0.0f, 1.0f) * 255.0f + 0.5f);
    g_theme_anim = (q != (light ? 255 : 0));
    const uint32_t* a = (const uint32_t*)&kDark;
    const uint32_t* b = (const uint32_t*)&kLight;
    uint32_t* o = (uint32_t*)&g_theme;
    int nc = (int)((sizeof(SettingsUI::Theme) - sizeof(bool)) / sizeof(uint32_t));
    for(int i = 0; i < nc; i++) o[i] = mix(a[i], b[i], q);
    g_theme.dark = q < 128;
    uint32_t acc = UIConfig::Color("theme.accent", 0xFF0A84FF) | 0xFF000000u;
    g_theme.accent = acc;
    g_theme.on_accent = luma(acc) > 186 ? 0xFF1C1C1E : 0xFFFFFFFF;
    g_theme.accent_soft = mix(g_theme.sidebar, acc, g_theme.dark ? 70 : 46);
    // legacy palette mirrors the live theme (satoru)
    SettingsUI::COL_BG      = g_theme.bg;
    SettingsUI::COL_PANEL   = g_theme.card;
    SettingsUI::COL_TEXT    = g_theme.text;
    SettingsUI::COL_DIM     = g_theme.text2;
    SettingsUI::COL_HEADING = g_theme.text;
    SettingsUI::COL_BORDER  = g_theme.sep;
    SettingsUI::COL_ON      = acc;
    SettingsUI::COL_OFF     = g_theme.track;
    SettingsUI::COL_WHITE   = g_theme.text;
}
bool ThemeAnimating(){ return g_theme_anim; }

// ════════════════════════════════════════════════════════════════════════════
//  clip + raw pixel access
// ════════════════════════════════════════════════════════════════════════════
struct ClipR { int x0, y0, x1, y1; };
static ClipR g_clip = {0, 0, 0, 0};
static ClipR g_clip_stack[16];
static int   g_clip_sp = 0;

void ResetClip(int x, int y, int w, int h){
    g_clip.x0 = x; g_clip.y0 = y; g_clip.x1 = x + w; g_clip.y1 = y + h;
    g_clip_sp = 0;
}
void PushClip(int x, int y, int w, int h){
    if(g_clip_sp < 16) g_clip_stack[g_clip_sp++] = g_clip;
    ClipR n = { x, y, x + w, y + h };
    if(n.x0 < g_clip.x0) n.x0 = g_clip.x0;
    if(n.y0 < g_clip.y0) n.y0 = g_clip.y0;
    if(n.x1 > g_clip.x1) n.x1 = g_clip.x1;
    if(n.y1 > g_clip.y1) n.y1 = g_clip.y1;
    if(n.x1 < n.x0) n.x1 = n.x0;
    if(n.y1 < n.y0) n.y1 = n.y0;
    g_clip = n;
    Graphics::PushClipRect(n.x0, n.y0, n.x1 - n.x0, n.y1 - n.y0);
}
void PopClip(){
    if(g_clip_sp > 0) g_clip = g_clip_stack[--g_clip_sp];
    Graphics::PopClipRect();
}

static bool target(uint32_t*& buf, int& pitch){
    if(Graphics::GetBpp() != 32 || !Graphics::GetBuffer()) return false;
    buf = (uint32_t*)Graphics::GetBuffer();
    pitch = (int)(Graphics::GetPitch() / 4);
    return true;
}
static ClipR eff_clip(){
    ClipR c = g_clip;
    // the screen, or the damage scissor while a compositor pass runs (satoru)
    int bx0, by0, sw, sh;
    Graphics::ScissorBounds(bx0, by0, sw, sh);
    if(c.x0 < bx0) c.x0 = bx0;
    if(c.y0 < by0) c.y0 = by0;
    if(c.x1 > sw) c.x1 = sw;
    if(c.y1 > sh) c.y1 = sh;
    return c;
}
static inline uint32_t blend(uint32_t src, uint32_t dst, int a256){
    if(a256 >= 256) return src | 0xFF000000u;
    uint32_t rb = (((src & 0xFF00FFu) * a256 + (dst & 0xFF00FFu) * (256 - a256)) >> 8) & 0xFF00FFu;
    uint32_t g  = (((src & 0x00FF00u) * a256 + (dst & 0x00FF00u) * (256 - a256)) >> 8) & 0x00FF00u;
    return 0xFF000000u | rb | g;
}

// ── rounded corner coverage masks (4x4 supersampled, cached per radius) ─────
static const int MASK_MAX = 48;
static uint8_t* g_mask[MASK_MAX + 1];
static const uint8_t* corner_mask(int r){
    if(r <= 0 || r > MASK_MAX) return nullptr;
    if(g_mask[r]) return g_mask[r];
    uint8_t* m = (uint8_t*)KernelHeap::Alloc((size_t)r * r);
    if(!m) return nullptr;
    float r2 = (float)r * r;
    for(int j = 0; j < r; j++)
        for(int i = 0; i < r; i++){
            int n = 0;
            for(int b = 0; b < 4; b++){
                float dy = j + (b + 0.5f) * 0.25f - r;
                for(int a = 0; a < 4; a++){
                    float dx = i + (a + 0.5f) * 0.25f - r;
                    if(dx * dx + dy * dy <= r2) n++;
                }
            }
            m[j * r + i] = (uint8_t)n;
        }
    g_mask[r] = m;
    return m;
}
static int corner_cov(int i, int j, int r){
    if(r <= 0) return 16;
    if(i < 0 || j < 0) return 0;
    if(i >= r || j >= r) return 16;
    if(const uint8_t* m = corner_mask(r)) return m[j * r + i];
    return 16;
}

static void fill_round(int x, int y, int w, int h, int r, uint32_t c, int alpha){
    if(w <= 0 || h <= 0) return;
    alpha = alpha * C.alpha / 255;
    if(alpha <= 0) return;
    uint32_t* buf; int pitch;
    if(!target(buf, pitch)) return;
    ClipR cl = eff_clip();
    if(r * 2 > w) r = w / 2;
    if(r * 2 > h) r = h / 2;
    if(r < 0) r = 0;
    if(r > MASK_MAX) r = MASK_MAX;
    int a256 = alpha >= 255 ? 256 : alpha;
    int jy0 = k_max(0, cl.y0 - y), jy1 = k_min(h, cl.y1 - y);
    for(int j = jy0; j < jy1; j++){
        int py = y + j;
        uint32_t* row = buf + (size_t)py * pitch;
        bool band = (j < r) || (j >= h - r);
        int x0 = x, x1 = x + w;
        if(band){
            int cj = j < r ? j : h - 1 - j;
            for(int i = 0; i < r; i++){
                int cv = corner_cov(i, cj, r);
                if(!cv) continue;
                int a = (a256 * cv) >> 4;
                int px = x + i;
                if(px >= cl.x0 && px < cl.x1) row[px] = blend(c, row[px], a);
                px = x + w - 1 - i;
                if(px >= cl.x0 && px < cl.x1) row[px] = blend(c, row[px], a);
            }
            x0 = x + r; x1 = x + w - r;
        }
        if(x0 < cl.x0) x0 = cl.x0;
        if(x1 > cl.x1) x1 = cl.x1;
        if(a256 >= 256){ uint32_t cc = c | 0xFF000000u; for(int px = x0; px < x1; px++) row[px] = cc; }
        else for(int px = x0; px < x1; px++) row[px] = blend(c, row[px], a256);
    }
}

static void stroke_round(int x, int y, int w, int h, int r, uint32_t c, int alpha){
    if(w <= 2 || h <= 2) return;
    alpha = alpha * C.alpha / 255;
    if(alpha <= 0) return;
    uint32_t* buf; int pitch;
    if(!target(buf, pitch)) return;
    ClipR cl = eff_clip();
    if(r * 2 > w) r = w / 2;
    if(r * 2 > h) r = h / 2;
    if(r > MASK_MAX) r = MASK_MAX;
    int a256 = alpha >= 255 ? 256 : alpha;
    auto put = [&](int px, int py, int a){
        if(px < cl.x0 || px >= cl.x1 || py < cl.y0 || py >= cl.y1 || a <= 0) return;
        uint32_t* d = buf + (size_t)py * pitch + px;
        *d = blend(c, *d, a);
    };
    for(int px = x + r; px < x + w - r; px++){ put(px, y, a256); put(px, y + h - 1, a256); }
    for(int py = y + r; py < y + h - r; py++){ put(x, py, a256); put(x + w - 1, py, a256); }
    for(int j = 0; j < r; j++)
        for(int i = 0; i < r; i++){
            int outer = corner_cov(i, j, r);
            int inner = (r > 1) ? ((i >= 1 && j >= 1) ? corner_cov(i - 1, j - 1, r - 1) : 0) : 0;
            int ring = outer - inner;
            if(ring <= 0) continue;
            int a = (a256 * ring) >> 4;
            put(x + i, y + j, a); put(x + w - 1 - i, y + j, a);
            put(x + i, y + h - 1 - j, a); put(x + w - 1 - i, y + h - 1 - j, a);
        }
}

// paint `c` over the outside of a rounded rect's corners, so a square picture
// drawn into (x,y,w,h) ends up with rounded corners on a known background. (satoru)
static void mask_corners(int x, int y, int w, int h, int r, uint32_t c){
    uint32_t* buf; int pitch;
    if(!target(buf, pitch)) return;
    ClipR cl = eff_clip();
    if(r > MASK_MAX) r = MASK_MAX;
    for(int j = 0; j < r; j++)
        for(int i = 0; i < r; i++){
            int cv = corner_cov(i, j, r);
            if(cv >= 16) continue;
            int a = ((16 - cv) * 256) >> 4;
            int xs[2] = { x + i, x + w - 1 - i }, ys[2] = { y + j, y + h - 1 - j };
            for(int u = 0; u < 2; u++) for(int v = 0; v < 2; v++){
                int px = xs[u], py = ys[v];
                if(px < cl.x0 || px >= cl.x1 || py < cl.y0 || py >= cl.y1) continue;
                uint32_t* d = buf + (size_t)py * pitch + px;
                *d = blend(c, *d, a);
            }
        }
}

static void disc(float cx, float cy, float rad, uint32_t c, int alpha){
    alpha = alpha * C.alpha / 255;
    if(alpha <= 0 || rad <= 0) return;
    uint32_t* buf; int pitch;
    if(!target(buf, pitch)) return;
    ClipR cl = eff_clip();
    int x0 = k_max(cl.x0, (int)(cx - rad - 1)), x1 = k_min(cl.x1, (int)(cx + rad + 2));
    int y0 = k_max(cl.y0, (int)(cy - rad - 1)), y1 = k_min(cl.y1, (int)(cy + rad + 2));
    float inner = rad - 0.75f; float inner2 = inner > 0 ? inner * inner : 0;
    for(int py = y0; py < y1; py++){
        uint32_t* row = buf + (size_t)py * pitch;
        float dy = (float)py + 0.5f - cy;
        for(int px = x0; px < x1; px++){
            float dx = (float)px + 0.5f - cx;
            float d2 = dx * dx + dy * dy;
            float cov;
            if(d2 <= inner2) cov = 1.0f;
            else { cov = rad - f_sqrt(d2) + 0.5f; if(cov <= 0) continue; if(cov > 1) cov = 1; }
            int a = (int)(cov * alpha + 0.5f);
            row[px] = blend(c, row[px], a >= 255 ? 256 : a);
        }
    }
}

void Shadow(int x, int y, int w, int h, int r, int spread, int dy, int alpha){
    if(spread <= 0 || alpha <= 0) return;
    for(int k = spread; k >= 1; k--){
        int a = alpha * (spread - k + 1) * (spread - k + 1) / (spread * spread * 2);
        if(a < 1) continue;
        stroke_round(x - k, y - k + dy, w + 2 * k, h + 2 * k, r + k, 0xFF000000u, a);
    }
}

int TextFit(int x, int y, float px, const char* s, uint32_t c, int maxw, bool bold){
    if(!s) return 0;
    if(maxw <= 0) return 0;
    int w = FontTTF::MeasureText(px, s, bold);
    if(w <= maxw) return FontTTF::DrawText(x, y, px, s, Fade(c), bold);
    char buf[128];
    int n = k_len(s); if(n > 120) n = 120;
    int ew = FontTTF::MeasureText(px, "...", bold);
    while(n > 0){
        for(int i = 0; i < n; i++) buf[i] = s[i];
        buf[n] = 0;
        if(FontTTF::MeasureText(px, buf, bold) + ew <= maxw) break;
        n--;
    }
    buf[n] = '.'; buf[n + 1] = '.'; buf[n + 2] = '.'; buf[n + 3] = 0;
    return FontTTF::DrawText(x, y, px, buf, Fade(c), bold);
}

// ════════════════════════════════════════════════════════════════════════════
//  vector icons: signed distance shapes on a 24 unit grid, cached as alpha
//  masks per (icon, size). (satoru)
// ════════════════════════════════════════════════════════════════════════════
static float sd_seg(float px, float py, float ax, float ay, float bx, float by, float hw){
    float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    float d = bax * bax + bay * bay;
    float h = d > 0 ? f_clamp((pax * bax + pay * bay) / d, 0.0f, 1.0f) : 0.0f;
    float dx = pax - bax * h, dy = pay - bay * h;
    return f_sqrt(dx * dx + dy * dy) - hw;
}
static float sd_disc(float px, float py, float cx, float cy, float r){
    float dx = px - cx, dy = py - cy; return f_sqrt(dx * dx + dy * dy) - r;
}
static float sd_ring(float px, float py, float cx, float cy, float r, float hw){
    return f_abs(sd_disc(px, py, cx, cy, r)) - hw;
}
static float sd_box(float px, float py, float cx, float cy, float hx, float hy, float r){
    float qx = f_abs(px - cx) - hx + r, qy = f_abs(py - cy) - hy + r;
    float ox = f_max(qx, 0.0f), oy = f_max(qy, 0.0f);
    return f_sqrt(ox * ox + oy * oy) + f_min(f_max(qx, qy), 0.0f) - r;
}
static float sd_box_stroke(float px, float py, float cx, float cy, float hx, float hy, float r, float hw){
    return f_abs(sd_box(px, py, cx, cy, hx, hy, r)) - hw;
}
// upward cone of 45 degrees half angle with apex at (cx,cy) (satoru)
static float sd_cone_up(float px, float py, float cx, float cy){
    float x = px - cx, y = py - cy;
    return f_max((x + y) * 0.7071f, (-x + y) * 0.7071f);
}
static float sd_cone_right(float px, float py, float cx, float cy){
    float x = px - cx, y = py - cy;
    return (f_abs(y) - x) * 0.7071f;
}

static const float kCos8[8] = { 1.0f, 0.7071f, 0.0f, -0.7071f, -1.0f, -0.7071f, 0.0f, 0.7071f };
static const float kSin8[8] = { 0.0f, 0.7071f, 1.0f, 0.7071f, 0.0f, -0.7071f, -1.0f, -0.7071f };

static float icon_sd(int id, float x, float y){
    const float S = 1.1f;   // stroke half width (satoru)
    switch(id){
    case IC_NETWORK: case IC_WIFI_SMALL: {
        float d = sd_disc(x, y, 12, 18.6f, 1.9f);
        d = f_min(d, f_max(sd_ring(x, y, 12, 19.0f, 6.6f, S), sd_cone_up(x, y, 12, 19.0f)));
        d = f_min(d, f_max(sd_ring(x, y, 12, 19.0f, 11.4f, S), sd_cone_up(x, y, 12, 19.0f)));
        return d;
    }
    case IC_DISPLAY: {
        float d = sd_box_stroke(x, y, 12, 10.5f, 9.0f, 6.5f, 2.0f, S);
        d = f_min(d, sd_seg(x, y, 12, 17.5f, 12, 20.0f, S));
        d = f_min(d, sd_seg(x, y, 8, 20.6f, 16, 20.6f, S));
        return d;
    }
    case IC_SOUND: {
        float body = sd_box(x, y, 5.6f, 12, 2.1f, 3.2f, 0.8f);
        float cone = f_max(f_max(7.0f - x, x - 12.0f), (f_abs(y - 12) - 3.0f - (x - 7.0f)) * 0.7071f);
        float d = f_min(body, cone);
        d = f_min(d, f_max(sd_ring(x, y, 12.5f, 12, 4.2f, S), sd_cone_right(x, y, 12.5f, 12)));
        d = f_min(d, f_max(sd_ring(x, y, 12.5f, 12, 8.2f, S), sd_cone_right(x, y, 12.5f, 12)));
        return d;
    }
    case IC_POWER: {
        float d = sd_box_stroke(x, y, 11, 12, 7.6f, 4.6f, 1.8f, S);
        d = f_min(d, sd_box(x, y, 20.3f, 12, 1.0f, 2.0f, 0.6f));
        d = f_min(d, sd_box(x, y, 8.2f, 12, 3.4f, 2.0f, 0.6f));
        return d;
    }
    case IC_APPEARANCE: {
        float d = sd_ring(x, y, 12, 12, 8.4f, S);
        d = f_min(d, f_max(sd_disc(x, y, 12, 12, 8.4f), x - 12.0f));
        return d;
    }
    case IC_A11Y: {
        float d = sd_disc(x, y, 12, 5.4f, 2.1f);
        d = f_min(d, sd_seg(x, y, 5.5f, 9.6f, 18.5f, 9.6f, S));
        d = f_min(d, sd_seg(x, y, 12, 9.6f, 12, 14.6f, 1.3f));
        d = f_min(d, sd_seg(x, y, 12, 14.6f, 8.6f, 20.6f, S));
        d = f_min(d, sd_seg(x, y, 12, 14.6f, 15.4f, 20.6f, S));
        return d;
    }
    case IC_SECURITY: case IC_LOCK: {
        float bodyd = sd_box(x, y, 12, 15.2f, 6.6f, 5.0f, 1.6f);
        float hole = f_min(sd_disc(x, y, 12, 14.4f, 1.4f), sd_seg(x, y, 12, 14.4f, 12, 17.2f, 0.65f));
        float d = f_max(bodyd, -hole);
        float sh = f_max(sd_ring(x, y, 12, 8.6f, 4.0f, S), y - 8.6f);
        sh = f_min(sh, sd_seg(x, y, 8.0f, 8.6f, 8.0f, 10.6f, S));
        sh = f_min(sh, sd_seg(x, y, 16.0f, 8.6f, 16.0f, 10.6f, S));
        return f_min(d, sh);
    }
    case IC_STORAGE: {
        float d = sd_box_stroke(x, y, 12, 12, 8.6f, 6.2f, 2.0f, S);
        d = f_min(d, sd_seg(x, y, 4.0f, 13.6f, 20.0f, 13.6f, 0.9f));
        d = f_min(d, sd_disc(x, y, 16.2f, 15.9f, 1.15f));
        return d;
    }
    case IC_DEVICES: {
        float d = sd_box_stroke(x, y, 12, 12, 5.6f, 5.6f, 1.3f, S);
        d = f_min(d, sd_box(x, y, 12, 12, 2.2f, 2.2f, 0.5f));
        for(int k = 0; k < 2; k++){
            float o = k ? 14.4f : 9.6f;
            d = f_min(d, sd_seg(x, y, o, 3.4f, o, 6.0f, 0.9f));
            d = f_min(d, sd_seg(x, y, o, 18.0f, o, 20.6f, 0.9f));
            d = f_min(d, sd_seg(x, y, 3.4f, o, 6.0f, o, 0.9f));
            d = f_min(d, sd_seg(x, y, 18.0f, o, 20.6f, o, 0.9f));
        }
        return d;
    }
    case IC_SYSTEM: {
        float d = sd_disc(x, y, 12, 12, 6.6f);
        for(int k = 0; k < 8; k++){
            float dx = x - 12, dy = y - 12;
            float u = dx * kCos8[k] + dy * kSin8[k];
            float v = -dx * kSin8[k] + dy * kCos8[k];
            d = f_min(d, sd_box(u, v, 7.9f, 0, 1.9f, 1.55f, 0.55f));
        }
        d = f_max(d, -sd_disc(x, y, 12, 12, 2.7f));
        return d;
    }
    case IC_GLOBE: {
        // a ring, the equator and one meridian (a squeezed ring). (satoru)
        float d = sd_ring(x, y, 12, 12, 8.6f, S);
        d = f_min(d, sd_seg(x, y, 3.6f, 12, 20.4f, 12, S * 0.9f));
        d = f_min(d, sd_ring(12 + (x - 12) * 2.2f, y, 12, 12, 8.6f, S * 2.0f) / 2.2f);
        return d;
    }
    case IC_ABOUT: {
        float d = sd_ring(x, y, 12, 12, 8.6f, S);
        d = f_min(d, sd_disc(x, y, 12, 7.9f, 1.25f));
        d = f_min(d, sd_seg(x, y, 12, 11.0f, 12, 16.6f, 1.15f));
        return d;
    }
    case IC_SEARCH: {
        float d = sd_ring(x, y, 10.4f, 10.4f, 5.6f, 1.25f);
        d = f_min(d, sd_seg(x, y, 14.6f, 14.6f, 19.4f, 19.4f, 1.45f));
        return d;
    }
    case IC_CHEV_R:
        return f_min(sd_seg(x, y, 9.6f, 6.4f, 15.0f, 12.0f, 1.25f), sd_seg(x, y, 15.0f, 12.0f, 9.6f, 17.6f, 1.25f));
    case IC_CHEV_L:
        return f_min(sd_seg(x, y, 14.4f, 6.4f, 9.0f, 12.0f, 1.25f), sd_seg(x, y, 9.0f, 12.0f, 14.4f, 17.6f, 1.25f));
    case IC_CHEV_UD: {
        float d = f_min(sd_seg(x, y, 8.4f, 9.6f, 12, 6.2f, 1.15f), sd_seg(x, y, 12, 6.2f, 15.6f, 9.6f, 1.15f));
        d = f_min(d, f_min(sd_seg(x, y, 8.4f, 14.4f, 12, 17.8f, 1.15f), sd_seg(x, y, 12, 17.8f, 15.6f, 14.4f, 1.15f)));
        return d;
    }
    case IC_CHECK:
        return f_min(sd_seg(x, y, 5.8f, 12.6f, 9.9f, 16.6f, 1.5f), sd_seg(x, y, 9.9f, 16.6f, 18.2f, 7.8f, 1.5f));
    case IC_CLEAR: {
        float cross = f_min(sd_seg(x, y, 9, 9, 15, 15, 1.0f), sd_seg(x, y, 15, 9, 9, 15, 1.0f));
        return f_max(sd_disc(x, y, 12, 12, 8.2f), -cross);
    }
    case IC_ETHERNET: {
        float d = sd_box_stroke(x, y, 12, 5.8f, 3.4f, 2.6f, 0.8f, 1.0f);
        d = f_min(d, sd_seg(x, y, 12, 8.4f, 12, 12, 1.0f));
        d = f_min(d, sd_seg(x, y, 5, 12, 19, 12, 1.0f));
        d = f_min(d, sd_seg(x, y, 5, 12, 5, 15, 1.0f));
        d = f_min(d, sd_seg(x, y, 19, 12, 19, 15, 1.0f));
        d = f_min(d, sd_box_stroke(x, y, 5, 17.8f, 3.0f, 2.6f, 0.8f, 1.0f));
        d = f_min(d, sd_box_stroke(x, y, 19, 17.8f, 3.0f, 2.6f, 0.8f, 1.0f));
        return d;
    }
    case IC_LINUX: {
        // a console window with a prompt: the linux vm (satoru)
        float d = sd_box_stroke(x, y, 12, 12, 8.6f, 7.0f, 2.0f, S);
        d = f_min(d, sd_seg(x, y, 7.6f, 9.4f, 10.6f, 12.0f, S));
        d = f_min(d, sd_seg(x, y, 10.6f, 12.0f, 7.6f, 14.6f, S));
        d = f_min(d, sd_seg(x, y, 12.4f, 15.0f, 16.4f, 15.0f, S));
        return d;
    }
    case IC_USER: {
        float d = sd_disc(x, y, 12, 8.4f, 3.7f);
        d = f_min(d, f_max(sd_disc(x, y, 12, 21.6f, 7.6f), y - 19.6f));
        return d;
    }
    default: {
        float d = sd_disc(x, y, 8, 8, 2.2f);
        d = f_min(d, sd_disc(x, y, 16, 8, 2.2f));
        d = f_min(d, sd_disc(x, y, 8, 16, 2.2f));
        return f_min(d, sd_disc(x, y, 16, 16, 2.2f));
    }
    }
}

struct IconMask { int id, size; uint32_t used; uint8_t px[40 * 40]; };
static IconMask g_icons[40];
static uint32_t g_icon_clock = 0;

static const uint8_t* icon_mask(int id, int size){
    if(size < 6 || size > 40) return nullptr;
    for(int i = 0; i < 40; i++)
        if(g_icons[i].size == size && g_icons[i].id == id){ g_icons[i].used = ++g_icon_clock; return g_icons[i].px; }
    int slot = 0;
    for(int i = 0; i < 40; i++){
        if(g_icons[i].size == 0){ slot = i; break; }
        if(g_icons[i].used < g_icons[slot].used) slot = i;
    }
    IconMask& m = g_icons[slot];
    float sc = 24.0f / (float)size;
    for(int j = 0; j < size; j++)
        for(int i = 0; i < size; i++){
            float d = icon_sd(id, ((float)i + 0.5f) * sc, ((float)j + 0.5f) * sc) / sc;
            float cov = f_clamp(0.5f - d, 0.0f, 1.0f);
            m.px[j * size + i] = (uint8_t)(cov * 255.0f + 0.5f);
        }
    m.id = id; m.size = size; m.used = ++g_icon_clock;
    return m.px;
}

// ════════════════════════════════════════════════════════════════════════════
//  springs: semi-implicit euler in fixed 2 ms substeps of real time, so the
//  motion is identical at 60, 144 or 240 hz. (satoru)
// ════════════════════════════════════════════════════════════════════════════
struct Spr { uint32_t id; float x, v; uint32_t t_last, t_touch; bool used; };
static Spr g_spr[384];

static const float kK[4] = { 900.0f, 520.0f, 240.0f, 380.0f };
static const float kZ[4] = { 1.0f, 0.62f, 1.0f, 0.82f };

static Spr* spr_find(uint32_t id, bool& fresh){
    fresh = false;
    uint32_t h = (id * 2654435761u) % 384u;
    int free_i = -1, old_i = (int)h;
    for(int n = 0; n < 384; n++){
        int i = (int)((h + (uint32_t)n) % 384u);
        if(g_spr[i].used && g_spr[i].id == id) return &g_spr[i];
        if(!g_spr[i].used){ if(free_i < 0) free_i = i; break; }
        if(g_spr[i].t_touch < g_spr[old_i].t_touch) old_i = i;
    }
    int i = free_i >= 0 ? free_i : old_i;
    g_spr[i].used = true; g_spr[i].id = id; g_spr[i].v = 0; g_spr[i].t_last = C.now; g_spr[i].t_touch = C.now;
    fresh = true;
    return &g_spr[i];
}

float SpringSeed(uint32_t id, float target, int kind, float seed){
    if(kind < 0 || kind > 3) kind = 0;
    bool fresh;
    Spr* s = spr_find(id, fresh);
    if(fresh){ s->x = seed; s->v = 0; }
    s->t_touch = C.now;
    if(C.reduced){ s->x = target; s->v = 0; s->t_last = C.now; return target; }
    uint32_t dt = C.now - s->t_last;
    if(dt > 600) dt = 600;
    s->t_last = C.now;
    float k = kK[kind], c = 2.0f * kZ[kind] * f_sqrt(k);
    int steps = (int)(dt / 2);
    s->t_last -= dt % 2;    // carry the remainder so time is not lost (satoru)
    for(int i = 0; i < steps; i++){
        float a = k * (target - s->x) - c * s->v;
        s->v += a * 0.002f;
        s->x += s->v * 0.002f;
    }
    float err = target - s->x;
    float scale = f_abs(target) > 1.0f ? f_abs(target) : 1.0f;
    if(f_abs(err) < 0.0015f * scale && f_abs(s->v) < 0.02f * scale){ s->x = target; s->v = 0; }
    else C.animating = true;
    return s->x;
}

float Ease(uint32_t t0, int delay_ms, int dur_ms){
    if(C.reduced) return 1.0f;
    int el = (int)(C.now - t0) - delay_ms;
    if(el <= 0){ C.animating = true; return 0.0f; }
    if(el >= dur_ms) return 1.0f;
    C.animating = true;
    float t = (float)el / (float)dur_ms;
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

void AnimSweep(){
    for(int i = 0; i < 384; i++)
        if(g_spr[i].used && (C.now - g_spr[i].t_touch) > 4000u) g_spr[i].used = false;
}

// ════════════════════════════════════════════════════════════════════════════
//  pass control + layout state
// ════════════════════════════════════════════════════════════════════════════
void BeginPass(Pass p){
    C.pass = p;
    C.y = 0;
    C.in_group = false;
    C.group_seq = 0;
    C.row_in_group = 0;
    C.focus_count = 0;
    C.id_sp = 0;
    C.group_id = 0;
    C.dy = 0;
    C.search_hit = false;
    C.first_hit_y = -1;
    if(p == P_DRAW || p == P_LAYOUT){ C.focus_id = 0; C.focus_y0 = C.focus_y1 = -1; }
    if(p == P_EVENT){ C.consumed = false; C.changed = false; C.committed = false; C.submitted = false; }
}
void EndPass(){
    if(C.in_group) SettingsUI::EndGroup();
    C.y += 28;
    C.content_h = C.y;
}

static uint32_t cur_seed(){
    uint32_t s = C.group_id ^ ((uint32_t)C.page * 0x9E3779B1u);
    if(C.id_sp > 0) s ^= C.id_stack[C.id_sp - 1] * 0x85EBCA6Bu;
    return s;
}

// ── row model ───────────────────────────────────────────────────────────────
struct KRow {
    uint32_t id;
    int y;            // content y (satoru)
    int sy;           // screen y used for hit tests (no stagger offset) (satoru)
    int dyy;          // screen y used for drawing (satoru)
    int h;
    int x, w;         // card x / width (satoru)
    bool hover, focus, hit_search;
    int fidx;
};

static bool pt_in(int px, int py, int x, int y, int w, int h){
    return px >= x && px < x + w && py >= y && py < y + h;
}

static const float FS_LABEL = 14.5f, FS_SUB = 12.5f, FS_VALUE = 14.0f;

static KRow row_begin(const char* label, const char* sub, bool focusable, int h){
    KRow r;
    r.id = SettingsUI::Hash(label ? label : "", cur_seed());
    if(h <= 0) h = sub ? 60 : 48;
    r.h = h;
    r.y = C.y;
    r.x = C.col_x; r.w = C.col_w;
    r.sy = C.origin_y + C.y;
    r.dyy = r.sy + C.dy;
    C.y += h;
    r.fidx = -1;
    r.focus = false;
    if(focusable){
        r.fidx = C.focus_count++;
        if(C.pass == P_EVENT && C.ev == EV_CLICK && pt_in(C.ev_x, C.ev_y, r.x, r.sy, r.w, h)) C.focus = r.fidx;
        if(r.fidx == C.focus){ r.focus = true; C.focus_id = r.id; C.focus_y0 = r.y; C.focus_y1 = r.y + h; }
    }
    r.hover = C.pointer_ok && pt_in(C.mx, C.my, r.x, r.sy, r.w, r.h);
    r.hit_search = C.query && C.query[0] && label && Match(label, C.query);
    if(r.hit_search || (sub && C.query && C.query[0] && Match(sub, C.query))){
        C.search_hit = true;
        if(C.first_hit_y < 0) C.first_hit_y = r.y;
    }
    if(C.pass == P_DRAW){
        if(C.row_in_group > 0)
            fill_round(r.x + 16, r.dyy, r.w - 16, 1, 0, SettingsUI::T().sep, 255);
    }
    C.row_in_group++;
    return r;
}

static bool visible(const KRow& r){
    return r.dyy + r.h >= C.view_y && r.dyy < C.view_y + C.view_h;
}

// row background clipped to the card shape (hover, press, search tint). (satoru)
static void row_fill(const KRow& r, uint32_t c, int alpha){
    if(alpha <= 0) return;
    int gy = C.origin_y + C.group_y0 + C.dy;
    int gh = C.group_h[C.group_seq & 63];
    PushClip(r.x, r.dyy, r.w, r.h);
    fill_round(r.x, gy, r.w, gh, 12, c, alpha);
    PopClip();
}

static void row_label(const KRow& r, const char* label, const char* sub, int maxw){
    const SettingsUI::Theme& t = SettingsUI::T();
    uint32_t lc = r.hit_search ? t.accent : t.text;
    if(sub){
        TextFit(r.x + 16, r.dyy + 11, FS_LABEL, label, lc, maxw);
        TextFit(r.x + 16, r.dyy + 32, FS_SUB, sub, t.text2, maxw);
    } else {
        TextFit(r.x + 16, r.dyy + (r.h - 18) / 2, FS_LABEL, label, lc, maxw);
    }
}

// shared row chrome: hover / search tint, focus ring around a control rect. (satoru)
static void row_chrome(const KRow& r, bool interactive, int fx, int fy, int fw, int fh, int fr){
    const SettingsUI::Theme& t = SettingsUI::T();
    if(r.hit_search){
        float s = SettingsUI::Spring(r.id ^ 0x5EA4C401u, 1.0f, 3);
        row_fill(r, t.accent, (int)(s * 34.0f));
    }
    if(interactive){
        float hv = SettingsUI::Spring(r.id ^ 0x40E70001u, r.hover ? 1.0f : 0.0f, 0);
        bool pressed = C.active == r.id && C.down;
        float pr = SettingsUI::Spring(r.id ^ 0x40E70002u, pressed ? 1.0f : 0.0f, 0);
        if(hv > 0.01f) row_fill(r, t.hover, (int)(hv * 255.0f));
        if(pr > 0.01f) row_fill(r, t.press, (int)(pr * 255.0f));
    }
    float fv = SettingsUI::Spring(r.id ^ 0xF0C50001u, (r.focus && C.focus_visible) ? 1.0f : 0.0f, 0);
    if(fv > 0.01f && fw > 0){
        int a = (int)(fv * 200.0f);
        int g = (int)((1.0f - fv) * 4.0f);
        stroke_round(fx - 3 - g, fy - 3 - g, fw + 6 + 2 * g, fh + 6 + 2 * g, fr + 3 + g, t.accent, a);
        stroke_round(fx - 2 - g, fy - 2 - g, fw + 4 + 2 * g, fh + 4 + 2 * g, fr + 2 + g, t.accent, a);
    }
}

static bool ev_click_in(int x, int y, int w, int h){
    return C.pass == P_EVENT && C.ev == EV_CLICK && !C.consumed && pt_in(C.ev_x, C.ev_y, x, y, w, h);
}
static bool ev_key_for(const KRow& r){
    return C.pass == P_EVENT && C.ev == EV_KEY && !C.consumed && r.focus;
}
static void mark(bool commit){ C.consumed = true; C.changed = true; if(commit) C.committed = true; }

// popover open helper (satoru)
static void pop_open(uint32_t id, int ax, int ay, int aw, int ah, const char* const* items, int n, int sel){
    C.pop.id = id; C.pop.open = true;
    C.pop.ax = ax; C.pop.ay = ay; C.pop.aw = aw; C.pop.ah = ah;
    C.pop.n = n > 16 ? 16 : n; C.pop.sel = sel; C.pop.hi = sel;
    for(int i = 0; i < C.pop.n; i++){
        const char* s = items[i] ? items[i] : "";
        int k = 0; while(s[k] && k < 43){ C.pop.items[i][k] = s[k]; k++; }
        C.pop.items[i][k] = 0;
    }
    C.pop.t_open = C.now;
}
void ClosePopover(){ C.pop.open = false; }

}  // namespace SKit

// ════════════════════════════════════════════════════════════════════════════
//  public kit
// ════════════════════════════════════════════════════════════════════════════
using namespace SKit;

namespace SettingsUI {

unsigned int COL_BG      = 0xFF18181B;
unsigned int COL_PANEL   = 0xFF232328;
unsigned int COL_TEXT    = 0xFFF2F2F4;
unsigned int COL_DIM     = 0xFFA1A1AA;
unsigned int COL_HEADING = 0xFFF2F2F4;
unsigned int COL_BORDER  = 0xFF2D2D33;
unsigned int COL_ON      = 0xFF0A84FF;
unsigned int COL_OFF     = 0xFF404048;
unsigned int COL_WHITE   = 0xFFF2F2F4;

const Theme& T(){ return g_theme; }
unsigned int Accent(){ return g_theme.accent ? g_theme.accent : UIConfig::Color("theme.accent", 0xFF0A84FF); }

uint32_t Hash(const char* s, uint32_t seed){
    uint32_t h = seed;
    if(s) while(*s){ h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}
uint32_t Mix(uint32_t a, uint32_t b, int t255){ return mix(a, b, t255); }
float Spring(uint32_t id, float target, int kind){
    bool fresh; Spr* s = spr_find(id, fresh);
    float seed = fresh ? target : s->x;
    return SpringSeed(id, target, kind, seed);
}

void FillRound(int x, int y, int w, int h, int r, uint32_t c, int alpha){ fill_round(x, y, w, h, r, c, alpha); }
void StrokeRound(int x, int y, int w, int h, int r, uint32_t c, int alpha){ stroke_round(x, y, w, h, r, c, alpha); }
void Disc(float cx, float cy, float r, uint32_t c, int alpha){ disc(cx, cy, r, c, alpha); }
int  Text(int x, int y, float px, const char* s, uint32_t c, bool bold){ return s ? FontTTF::DrawText(x, y, px, s, Fade(c), bold) : 0; }
int  TextWidth(float px, const char* s, bool bold){ return s ? FontTTF::MeasureText(px, s, bold) : 0; }

void Icon(int icon, int x, int y, int size, uint32_t c, int alpha){
    alpha = alpha * C.alpha / 255;
    if(alpha <= 0) return;
    const uint8_t* m = icon_mask(icon, size);
    if(!m) return;
    uint32_t* buf; int pitch;
    if(!target(buf, pitch)) return;
    ClipR cl = eff_clip();
    for(int j = 0; j < size; j++){
        int py = y + j;
        if(py < cl.y0 || py >= cl.y1) continue;
        uint32_t* row = buf + (size_t)py * pitch;
        for(int i = 0; i < size; i++){
            int px = x + i;
            if(px < cl.x0 || px >= cl.x1) continue;
            int a = m[j * size + i];
            if(!a) continue;
            a = a * alpha / 255;
            row[px] = blend(c, row[px], a >= 255 ? 256 : a);
        }
    }
}

void Image(const uint32_t* px, int w, int h, int x, int y, int alpha){
    alpha = alpha * C.alpha / 255;
    if(!px || alpha <= 0) return;
    uint32_t* buf; int pitch;
    if(!target(buf, pitch)) return;
    ClipR cl = eff_clip();
    int a256 = alpha >= 255 ? 256 : alpha;
    for(int j = 0; j < h; j++){
        int py = y + j;
        if(py < cl.y0 || py >= cl.y1) continue;
        uint32_t* row = buf + (size_t)py * pitch;
        const uint32_t* src = px + (size_t)j * w;
        for(int i = 0; i < w; i++){
            int qx = x + i;
            if(qx < cl.x0 || qx >= cl.x1) continue;
            row[qx] = a256 >= 256 ? (src[i] | 0xFF000000u) : blend(src[i], row[qx], a256);
        }
    }
}

void PushId(int n){
    if(C.id_sp >= 8) return;
    uint32_t base = C.id_sp ? C.id_stack[C.id_sp - 1] : 0;
    C.id_stack[C.id_sp] = (uint32_t)n * 0x27D4EB2Du + 0x165667B1u + base;
    C.id_sp++;
}
void PopId(){ if(C.id_sp > 0) C.id_sp--; }
bool Committed(){ return C.committed; }
bool Submitted(){ return C.submitted; }
bool Drawing(){ return C.pass == P_DRAW; }
void GoTo(const char* id){
    int k = 0; if(id) while(id[k] && k < 23){ C.goto_id[k] = id[k]; k++; }
    C.goto_id[k] = 0;
}

// ── groups ──────────────────────────────────────────────────────────────────
void Group(const char* title, const char* footer){
    (void)footer;
    if(C.in_group) EndGroup();
    C.y += (C.group_seq > 0) ? 26 : 6;
    C.group_id = Hash(title ? title : "~", 0x6A09E667u ^ (uint32_t)C.group_seq);
    // page enter: cards fade and rise in with a small stagger (satoru)
    if(C.pass == P_DRAW){
        float e = Ease(C.page_ms, 30 + C.group_seq * 45, 380);
        C.dy = (int)((1.0f - e) * 22.0f + 0.5f);
        C.alpha = (int)(e * 255.0f + 0.5f);
        C.under = g_theme.bg;
    }
    if(title && title[0]){
        bool hit = C.query && C.query[0] && Match(title, C.query);
        if(C.pass == P_DRAW){
            int ty = C.origin_y + C.y + C.dy;
            if(ty + 20 >= C.view_y && ty < C.view_y + C.view_h)
                TextFit(C.col_x + 4, ty, 13.0f, title, hit ? g_theme.accent : g_theme.text2, C.col_w - 8, true);
        }
        if(hit){ C.search_hit = true; if(C.first_hit_y < 0) C.first_hit_y = C.y; }
        C.y += 26;
    }
    C.in_group = true;
    C.group_y0 = C.y;
    C.row_in_group = 0;
    if(C.pass == P_DRAW){
        int gy = C.origin_y + C.group_y0 + C.dy;
        int gh = C.group_h[C.group_seq & 63];
        if(gh > 0 && gy + gh >= C.view_y && gy < C.view_y + C.view_h){
            if(!g_theme.dark) Shadow(C.col_x, gy, C.col_w, gh, 12, 3, 1, 26);
            fill_round(C.col_x, gy, C.col_w, gh, 12, g_theme.card, 255);
            stroke_round(C.col_x, gy, C.col_w, gh, 12, g_theme.card_line, g_theme.dark ? 255 : 180);
        }
        C.under = g_theme.card;
    }
}

void EndGroup(){
    if(!C.in_group) return;
    if(C.row_in_group == 0) C.y += 0;
    if(C.pass == P_LAYOUT) C.group_h[C.group_seq & 63] = C.y - C.group_y0;
    C.in_group = false;
    C.group_seq++;
    if(C.pass == P_DRAW) C.under = g_theme.bg;
}

void Note(const char* text){
    if(!text) return;
    if(C.in_group) EndGroup();
    // wrap into lines that fit the column (satoru)
    const float px = 12.5f;
    int maxw = C.col_w - 8;
    const char* p = text;
    C.y += 8;
    while(*p){
        char line[160]; int n = 0, last_sp = -1;
        while(p[n] && n < 159){
            line[n] = p[n]; line[n + 1] = 0;
            if(p[n] == ' ') last_sp = n;
            if(FontTTF::MeasureText(px, line, false) > maxw && last_sp > 0){ n = last_sp; break; }
            n++;
        }
        line[n] = 0;
        if(C.pass == P_DRAW){
            int ty = C.origin_y + C.y + C.dy;
            if(ty + 18 >= C.view_y && ty < C.view_y + C.view_h)
                FontTTF::DrawText(C.col_x + 4, ty, px, line, Fade(g_theme.text2), false);
        }
        if(C.query && C.query[0] && Match(line, C.query)) C.search_hit = true;
        C.y += 18;
        p += n;
        while(*p == ' ') p++;
    }
}

// ── info ────────────────────────────────────────────────────────────────────
void Info(const char* label, const char* value, const char* sub, uint32_t dot){
    KRow r = row_begin(label, sub, false, 0);
    if(value && C.query && C.query[0] && Match(value, C.query)) C.search_hit = true;
    if(C.pass != P_DRAW || !visible(r)) return;
    row_chrome(r, false, 0, 0, 0, 0, 0);
    int vw = value ? TextWidth(FS_VALUE, value) : 0;
    int lw = TextWidth(FS_LABEL, label);
    int avail = r.w - 32 - lw - 24 - (dot ? 14 : 0);
    if(avail < r.w / 3) avail = r.w / 3;
    if(vw > avail) vw = avail;
    int vx = r.x + r.w - 16 - vw;
    row_label(r, label, sub, r.w - 32 - vw - 24 - (dot ? 14 : 0));
    if(value) TextFit(vx, r.dyy + (r.h - 18) / 2, FS_VALUE, value, g_theme.text2, avail);
    if(dot) disc((float)(vx - 10), (float)r.dyy + r.h * 0.5f, 3.6f, dot, 255);
}

// ── toggle ──────────────────────────────────────────────────────────────────
bool Toggle(const char* label, bool* v, const char* sub){
    KRow r = row_begin(label, sub, true, 0);
    const int TW = 44, TH = 26;
    int tx = r.x + r.w - 16 - TW, ty = r.sy + (r.h - TH) / 2;
    if(C.pass == P_EVENT){
        if(C.ev == EV_DOWN && pt_in(C.ev_x, C.ev_y, r.x, r.sy, r.w, r.h)) C.active = r.id;
        if(ev_click_in(r.x, r.sy, r.w, r.h)){ *v = !*v; mark(true); return true; }
        if(ev_key_for(r)){
            int k = C.ev_key;
            if(k == K_ENTER || k == K_SPACE){ *v = !*v; mark(true); return true; }
            if(k == K_LEFT && *v){ *v = false; mark(true); return true; }
            if(k == K_RIGHT && !*v){ *v = true; mark(true); return true; }
        }
        return false;
    }
    if(C.pass != P_DRAW || !visible(r)) { if(C.pass == P_DRAW){ Spring(r.id ^ 1u, *v ? 1.0f : 0.0f, 1); } return false; }
    ty = r.dyy + (r.h - TH) / 2;
    row_chrome(r, true, tx, ty, TW, TH, TH / 2);
    row_label(r, label, sub, r.w - 32 - TW - 16);
    float pos = Spring(r.id ^ 1u, *v ? 1.0f : 0.0f, 1);
    float col = Spring(r.id ^ 2u, *v ? 1.0f : 0.0f, 0);
    bool pressed = C.active == r.id && C.down && r.hover;
    float st = Spring(r.id ^ 3u, pressed ? 1.0f : 0.0f, 0);
    float hv = Spring(r.id ^ 4u, r.hover ? 1.0f : 0.0f, 0);
    uint32_t off = mix(g_theme.track, g_theme.text3, (int)(hv * 60.0f));
    uint32_t tc = mix(off, g_theme.accent, (int)(f_clamp(col, 0, 1) * 255.0f));
    fill_round(tx, ty, TW, TH, TH / 2, tc, 255);
    // knob: a pill that stretches toward its travel while pressed (satoru)
    float kr = 11.0f, kw = 22.0f + st * 6.0f;
    float travel = (float)(TW - 4) - kw;
    float kx = (float)tx + 2.0f + f_clamp(pos, -0.12f, 1.12f) * travel;
    int ki = (int)(kx + 0.5f);
    Shadow(ki, ty + 2, (int)kw, 22, 11, 2, 1, 60);
    if(st < 0.02f) disc(kx + kr, (float)ty + 13.0f, kr, g_theme.knob, 255);
    else fill_round(ki, ty + 2, (int)(kw + 0.5f), 22, 11, g_theme.knob, 255);
    return false;
}

// ── slider ──────────────────────────────────────────────────────────────────
bool Slider(const char* label, int* v, int lo, int hi, int step, const char* value_text, const char* sub){
    KRow r = row_begin(label, sub, true, 0);
    if(hi <= lo) hi = lo + 1;
    if(step < 1) step = 1;
    int vw = 52;
    int sw = r.w * 42 / 100; sw = k_clamp(sw, 120, 260);
    int lw = TextWidth(FS_LABEL, label);
    if(r.w - 32 - vw - 12 - sw < lw + 12) sw = k_max(90, r.w - 32 - vw - 12 - lw - 12);
    int sx = r.x + r.w - 16 - vw - 8 - sw;
    int cy = r.sy + r.h / 2;
    auto val_at = [&](int px) -> int {
        int rel = px - sx; rel = k_clamp(rel, 0, sw);
        int raw = lo + (int)(((long long)rel * (hi - lo) + sw / 2) / sw);
        int s = lo + ((raw - lo + step / 2) / step) * step;
        return k_clamp(s, lo, hi);
    };
    if(C.pass == P_EVENT){
        bool in_track = pt_in(C.ev_x, C.ev_y, sx - 12, r.sy, sw + 24, r.h);
        if(C.ev == EV_DOWN && !C.consumed && in_track){
            C.active = r.id; C.down_id = r.id; C.down_ms = C.now;
            int nv = val_at(C.ev_x);
            C.consumed = true;
            if(nv != *v){ *v = nv; C.changed = true; return true; }
            return false;
        }
        if(C.ev == EV_MOVE && C.active == r.id){
            int nv = val_at(C.ev_x);
            if(nv != *v){ *v = nv; C.changed = true; C.consumed = true; return true; }
            return false;
        }
        if(C.ev == EV_UP && C.active == r.id){
            C.active = 0; mark(true); return true;
        }
        if(ev_click_in(sx - 12, r.sy, sw + 24, r.h)){
            if(C.down_id == r.id && (C.now - C.down_ms) < 600u){ C.consumed = true; return false; }
            *v = val_at(C.ev_x); mark(true); return true;
        }
        if(ev_key_for(r)){
            int big = (hi - lo) / 20; if(big < step) big = step;
            big = ((big + step - 1) / step) * step;
            int k = C.ev_key, nv = *v;
            if(k == K_LEFT) nv -= big; else if(k == K_RIGHT) nv += big; else return false;
            nv = k_clamp(nv, lo, hi);
            C.consumed = true;
            if(nv != *v){ *v = nv; C.changed = true; C.committed = true; return true; }
        }
        return false;
    }
    float frac = (float)(*v - lo) / (float)(hi - lo);
    bool dragging = C.active == r.id && C.down;
    if(C.pass != P_DRAW || !visible(r)){ if(C.pass == P_DRAW) SpringSeed(r.id ^ 1u, frac, 3, 0.0f); return false; }
    int dcy = r.dyy + r.h / 2;
    (void)cy;
    row_chrome(r, false, sx - 6, dcy - 12, sw + 12, 24, 12);
    row_label(r, label, sub, sx - r.x - 32);
    float pos = SpringSeed(r.id ^ 1u, frac, dragging ? 0 : 3, 0.0f);
    pos = f_clamp(pos, 0.0f, 1.0f);
    bool hov = r.hover && pt_in(C.mx, C.my, sx - 12, r.sy, sw + 24, r.h);
    float grow = Spring(r.id ^ 2u, dragging ? 1.0f : (hov ? 0.5f : 0.0f), 1);
    fill_round(sx, dcy - 2, sw, 4, 2, g_theme.track, 255);
    int fw = (int)(pos * sw + 0.5f);
    if(fw > 0) fill_round(sx, dcy - 2, fw, 4, 2, g_theme.accent, 255);
    float kx = (float)sx + pos * (float)sw, kr = 10.0f + grow * 2.0f;
    if(dragging) disc(kx, (float)dcy, kr + 6.0f, g_theme.accent, 46);
    Shadow((int)(kx - kr), (int)(dcy - kr), (int)(kr * 2), (int)(kr * 2), (int)kr, 3, 1, 70);
    disc(kx, (float)dcy, kr, g_theme.knob, 255);
    if(!g_theme.dark) stroke_round((int)(kx - kr), (int)(dcy - kr), (int)(kr * 2 + 0.5f), (int)(kr * 2 + 0.5f), (int)kr, 0xFF000000u, 22);
    char nb[16];
    const char* vt = value_text;
    if(!vt){ IntToStr(*v, nb, 16); vt = nb; }
    int tw = TextWidth(FS_VALUE, vt);
    TextFit(r.x + r.w - 16 - k_min(tw, vw), r.dyy + (r.h - 18) / 2, FS_VALUE, vt, g_theme.text2, vw);
    return false;
}

// ── segmented ───────────────────────────────────────────────────────────────
bool Segmented(const char* label, const char* const* items, int n, int* sel, const char* sub){
    KRow r = row_begin(label, sub, true, 0);
    if(n < 1) return false;
    int segw = 0;
    for(int i = 0; i < n; i++) segw = k_max(segw, TextWidth(13.5f, items[i], true) + 28);
    int lw = label && label[0] ? TextWidth(FS_LABEL, label) + 40 : 32;
    int maxseg = (r.w - lw) / n;
    if(segw > maxseg) segw = k_max(40, maxseg);
    int H = 30, W = segw * n + 4;
    int sx = r.x + r.w - 16 - W;
    if(!label || !label[0]) sx = r.x + (r.w - W) / 2;
    int sy = r.sy + (r.h - H) / 2;
    if(C.pass == P_EVENT){
        for(int i = 0; i < n; i++)
            if(ev_click_in(sx + 2 + i * segw, sy, segw, H)){
                C.consumed = true;
                if(*sel != i){ *sel = i; mark(true); return true; }
                return false;
            }
        if(ev_key_for(r)){
            int k = C.ev_key, nv = *sel;
            if(k == K_LEFT) nv--; else if(k == K_RIGHT || k == K_SPACE || k == K_ENTER) nv++; else return false;
            if(k == K_SPACE || k == K_ENTER){ if(nv >= n) nv = 0; }
            nv = k_clamp(nv, 0, n - 1);
            C.consumed = true;
            if(nv != *sel){ *sel = nv; mark(true); return true; }
        }
        return false;
    }
    if(C.pass != P_DRAW || !visible(r)){ if(C.pass == P_DRAW) Spring(r.id ^ 1u, (float)*sel, 1); return false; }
    sy = r.dyy + (r.h - H) / 2;
    row_chrome(r, false, sx, sy, W, H, 9);
    if(label && label[0]) row_label(r, label, sub, sx - r.x - 32);
    fill_round(sx, sy, W, H, 9, g_theme.field, 255);
    float p = Spring(r.id ^ 1u, (float)k_clamp(*sel, 0, n - 1), 1);
    int px = sx + 2 + (int)(p * segw + 0.5f);
    uint32_t pill = g_theme.dark ? mix(g_theme.field, 0xFFFFFFFFu, 38) : 0xFFFFFFFFu;
    Shadow(px, sy + 2, segw, H - 4, 7, 2, 1, g_theme.dark ? 70 : 34);
    fill_round(px, sy + 2, segw, H - 4, 7, pill, 255);
    for(int i = 0; i < n; i++){
        int cx = sx + 2 + i * segw;
        bool hov = r.hover && pt_in(C.mx, C.my, cx, r.sy + (r.h - H) / 2, segw, H);
        float near = 1.0f - f_clamp(f_abs(p - (float)i), 0.0f, 1.0f);
        uint32_t tc = mix(hov ? g_theme.text : g_theme.text2, g_theme.text, (int)(near * 255.0f));
        int tw = TextWidth(13.5f, items[i], true);
        int maxw = segw - 12;
        TextFit(cx + (segw - k_min(tw, maxw)) / 2, sy + 6, 13.5f, items[i], tc, maxw, true);
    }
    return false;
}

// ── dropdown ────────────────────────────────────────────────────────────────
bool Dropdown(const char* label, const char* const* items, int n, int* sel, const char* sub){
    KRow r = row_begin(label, sub, true, 0);
    int cur = (*sel >= 0 && *sel < n) ? *sel : 0;
    int maxt = 0;
    for(int i = 0; i < n; i++) maxt = k_max(maxt, TextWidth(FS_VALUE, items[i]));
    int lw = TextWidth(FS_LABEL, label);
    int bw = k_clamp(maxt + 46, 110, k_max(110, r.w - lw - 64));
    int H = 32, bx = r.x + r.w - 16 - bw, by = r.sy + (r.h - H) / 2;
    if(C.pass == P_EVENT){
        if(C.ev == EV_PICK && C.pick_id == r.id){
            C.consumed = true;
            if(C.pick_index >= 0 && C.pick_index < n && C.pick_index != *sel){ *sel = C.pick_index; mark(true); return true; }
            return false;
        }
        if(C.ev == EV_DOWN && pt_in(C.ev_x, C.ev_y, bx, by, bw, H)) C.active = r.id;
        if(ev_click_in(bx, by, bw, H) || (ev_key_for(r) && (C.ev_key == K_ENTER || C.ev_key == K_SPACE))){
            C.consumed = true;
            pop_open(r.id, bx, by, bw, H, items, n, cur);
        }
        return false;
    }
    if(C.pass != P_DRAW) return false;
    if(C.pop.open && C.pop.id == r.id){
        C.pop.ax = bx; C.pop.ay = by; C.pop.aw = bw; C.pop.ah = H;
    }
    if(!visible(r)) return false;
    by = r.dyy + (r.h - H) / 2;
    row_chrome(r, false, bx, by, bw, H, 8);
    row_label(r, label, sub, bx - r.x - 32);
    bool hov = r.hover && pt_in(C.mx, C.my, bx, r.sy + (r.h - H) / 2, bw, H);
    bool open = C.pop.open && C.pop.id == r.id;
    float hv = Spring(r.id ^ 1u, (hov || open) ? 1.0f : 0.0f, 0);
    uint32_t bg = mix(g_theme.field, g_theme.press, (int)(hv * 255.0f));
    fill_round(bx, by, bw, H, 8, bg, 255);
    stroke_round(bx, by, bw, H, 8, g_theme.field_line, (int)(120 + hv * 100));
    TextFit(bx + 12, by + 7, FS_VALUE, n > 0 ? items[cur] : "", g_theme.text, bw - 40);
    Icon(IC_CHEV_UD, bx + bw - 26, by + 7, 18, g_theme.text2, 255);
    return false;
}

// ── button ──────────────────────────────────────────────────────────────────
bool Button(const char* label, const char* button_text, int kind, const char* sub){
    KRow r = row_begin(label, sub, true, 0);
    int bw = TextWidth(14.0f, button_text, true) + 30, H = 32;
    int bx = r.x + r.w - 16 - bw, by = r.sy + (r.h - H) / 2;
    if(C.pass == P_EVENT){
        if(C.ev == EV_DOWN && pt_in(C.ev_x, C.ev_y, bx, by, bw, H)) C.active = r.id;
        if(ev_click_in(bx, by, bw, H) || (ev_key_for(r) && (C.ev_key == K_ENTER || C.ev_key == K_SPACE))){
            mark(true); return true;
        }
        return false;
    }
    if(C.pass != P_DRAW || !visible(r)) return false;
    by = r.dyy + (r.h - H) / 2;
    row_chrome(r, false, bx, by, bw, H, 8);
    row_label(r, label, sub, bx - r.x - 32);
    bool hov = r.hover && pt_in(C.mx, C.my, bx, r.sy + (r.h - H) / 2, bw, H);
    bool pr = hov && C.down && C.active == r.id;
    float hv = Spring(r.id ^ 1u, hov ? 1.0f : 0.0f, 0);
    float pv = Spring(r.id ^ 2u, pr ? 1.0f : 0.0f, 1);
    uint32_t bg, fg;
    if(kind == BTN_PRIMARY){ bg = g_theme.accent; fg = g_theme.on_accent; bg = mix(bg, 0xFFFFFFFFu, (int)(hv * 28.0f)); }
    else if(kind == BTN_DESTRUCTIVE){ bg = mix(g_theme.card, g_theme.danger, (int)(36 + hv * 40.0f)); fg = g_theme.danger; }
    else { bg = mix(g_theme.field, g_theme.press, (int)(hv * 255.0f)); fg = g_theme.text; }
    bg = mix(bg, 0xFF000000u, (int)(pv * 40.0f));
    int in = (int)(pv * 1.5f + 0.5f);
    fill_round(bx + in, by + in, bw - 2 * in, H - 2 * in, 8, bg, 255);
    if(kind == BTN_SECONDARY) stroke_round(bx + in, by + in, bw - 2 * in, H - 2 * in, 8, g_theme.field_line, 160);
    Text(bx + 15, by + 7, 14.0f, button_text, fg, true);
    return false;
}

// ── nav row ─────────────────────────────────────────────────────────────────
bool Nav(const char* label, const char* value, const char* sub){
    KRow r = row_begin(label, sub, true, 0);
    if(C.pass == P_EVENT){
        if(C.ev == EV_DOWN && pt_in(C.ev_x, C.ev_y, r.x, r.sy, r.w, r.h)) C.active = r.id;
        if(ev_click_in(r.x, r.sy, r.w, r.h) || (ev_key_for(r) && (C.ev_key == K_ENTER || C.ev_key == K_SPACE || C.ev_key == K_RIGHT))){
            mark(true); return true;
        }
        return false;
    }
    if(C.pass != P_DRAW || !visible(r)) return false;
    row_chrome(r, true, r.x + 4, r.dyy + 4, r.w - 8, r.h - 8, 9);
    float hv = Spring(r.id ^ 9u, r.hover ? 1.0f : 0.0f, 1);
    int cx = r.x + r.w - 30 + (int)(hv * 3.0f);
    Icon(IC_CHEV_R, cx, r.dyy + (r.h - 16) / 2, 16, g_theme.text3, 255);
    int vw = value ? k_min(TextWidth(FS_VALUE, value), r.w / 2) : 0;
    if(value) TextFit(r.x + r.w - 38 - vw, r.dyy + (r.h - 18) / 2, FS_VALUE, value, g_theme.text2, vw);
    row_label(r, label, sub, r.w - 70 - vw);
    return false;
}

// ── network row: a nav row with signal bars, a lock and a check ────────────
// bars 0..4. clicking anywhere on the row returns true. (satoru)
bool NetRow(const char* label, int bars, bool locked, bool connected, const char* sub){
    KRow r = row_begin(label, sub, true, 0);
    if(C.pass == P_EVENT){
        if(C.ev == EV_DOWN && pt_in(C.ev_x, C.ev_y, r.x, r.sy, r.w, r.h)) C.active = r.id;
        if(ev_click_in(r.x, r.sy, r.w, r.h) || (ev_key_for(r) && (C.ev_key == K_ENTER || C.ev_key == K_SPACE || C.ev_key == K_RIGHT))){
            mark(true); return true;
        }
        return false;
    }
    if(C.pass != P_DRAW || !visible(r)) return false;
    row_chrome(r, true, r.x + 4, r.dyy + 4, r.w - 8, r.h - 8, 9);
    float hv = Spring(r.id ^ 9u, r.hover ? 1.0f : 0.0f, 1);
    int cy = r.dyy + r.h / 2;
    int cx = r.x + r.w - 30 + (int)(hv * 3.0f);
    Icon(IC_CHEV_R, cx, cy - 8, 16, g_theme.text3, 255);
    // four rising bars, bottom aligned; the unlit ones stay faint. (satoru)
    int bx = r.x + r.w - 62;
    if(bars < 0) bars = 0;
    if(bars > 4) bars = 4;
    for(int i = 0; i < 4; i++){
        int bh = 5 + i * 3;
        bool lit = i < bars;
        fill_round(bx + i * 5, cy + 7 - bh, 3, bh, 1, lit ? g_theme.text : g_theme.text3, lit ? 255 : 90);
    }
    int x = bx - 22;
    if(locked){ Icon(IC_LOCK, x, cy - 8, 16, g_theme.text2, 255); x -= 22; }
    if(connected) Icon(IC_CHECK, x, cy - 8, 16, g_theme.accent, 255);
    row_label(r, label, sub, (x - r.x) - 30);
    return false;
}

// ── text field ──────────────────────────────────────────────────────────────
bool TextField(const char* label, char* buf, int cap, const char* placeholder, int flags){
    KRow r = row_begin(label, nullptr, true, 0);
    int fw = k_clamp(r.w * 46 / 100, 170, 320), H = 32;
    int fx = r.x + r.w - 16 - fw, fy = r.sy + (r.h - H) / 2;
    int len = k_len(buf);
    if(C.pass == P_EVENT){
        if(ev_click_in(r.x, r.sy, r.w, r.h)){
            C.edit_id = r.id; C.edit_ms = C.now; C.consumed = true;
            return false;
        }
        bool editing = C.edit_id == r.id;
        if(C.ev == EV_KEY && !C.consumed && (editing || r.focus)){
            int k = C.ev_key;
            if(!editing){
                if(k == K_ENTER || k == K_SPACE){ C.edit_id = r.id; C.edit_ms = C.now; C.consumed = true; return false; }
                if(k < 32 || k > 126) return false;
                C.edit_id = r.id;
            }
            C.edit_ms = C.now;
            if(k == K_ESC){ C.edit_id = 0; C.consumed = true; return false; }
            if(k == K_ENTER){ C.consumed = true; C.changed = true; C.committed = true; C.submitted = true; return true; }
            if(k == K_BS){ C.consumed = true; if(len > 0){ buf[len - 1] = 0; C.changed = true; return true; } return false; }
            if(k >= 32 && k <= 126){
                C.consumed = true;
                if(len < cap - 1){ buf[len] = (char)k; buf[len + 1] = 0; C.changed = true; return true; }
            }
        }
        return false;
    }
    if(C.pass != P_DRAW || !visible(r)) return false;
    fy = r.dyy + (r.h - H) / 2;
    bool editing = C.edit_id == r.id;
    row_chrome(r, false, fx, fy, fw, H, 8);
    row_label(r, label, nullptr, fx - r.x - 32);
    float ev = Spring(r.id ^ 1u, editing ? 1.0f : 0.0f, 0);
    fill_round(fx, fy, fw, H, 8, mix(g_theme.field, g_theme.card, (int)(ev * 255.0f)), 255);
    stroke_round(fx, fy, fw, H, 8, mix(g_theme.field_line, g_theme.accent, (int)(ev * 255.0f)), 255);
    if(ev > 0.02f) stroke_round(fx - 1, fy - 1, fw + 2, H + 2, 9, g_theme.accent, (int)(ev * 90.0f));
    char shown[96];
    int n = k_min(len, 95);
    for(int i = 0; i < n; i++) shown[i] = (flags & FIELD_PASSWORD) ? '*' : buf[i];
    shown[n] = 0;
    PushClip(fx + 8, fy, fw - 16, H);
    int tw = TextWidth(FS_VALUE, shown);
    int tx = fx + 10;
    if(tw > fw - 24) tx = fx + fw - 14 - tw;     // keep the caret end visible (satoru)
    if(n == 0 && !editing && placeholder) Text(fx + 10, fy + 7, FS_VALUE, placeholder, g_theme.text3);
    else Text(tx, fy + 7, FS_VALUE, shown, g_theme.text);
    if(editing){
        uint32_t el = C.now - C.edit_ms;
        bool on = el > 10000u || ((el / 530u) % 2u) == 0;
        if(el <= 10000u) Graphics::RequestFrameAt(C.now + 530u - (el % 530u));
        if(on) fill_round(tx + tw + 1, fy + 7, 2, 18, 1, g_theme.accent, 255);
    }
    PopClip();
    return false;
}

// ── swatches ────────────────────────────────────────────────────────────────
bool Swatches(const char* label, const uint32_t* colors, int n, int* sel){
    KRow r = row_begin(label, nullptr, true, 0);
    const int SP = 32, RAD = 11;
    int W = n * SP;
    int sx = r.x + r.w - 16 - W + (SP - 2 * RAD) / 2;
    if(C.pass == P_EVENT){
        for(int i = 0; i < n; i++)
            if(ev_click_in(sx + i * SP - 4, r.sy + 4, 2 * RAD + 8, r.h - 8)){
                C.consumed = true;
                if(*sel != i){ *sel = i; mark(true); return true; }
                return false;
            }
        if(ev_key_for(r)){
            int k = C.ev_key, nv = *sel < 0 ? 0 : *sel;
            if(k == K_LEFT) nv--; else if(k == K_RIGHT) nv++; else return false;
            nv = k_clamp(nv, 0, n - 1); C.consumed = true;
            if(nv != *sel){ *sel = nv; mark(true); return true; }
        }
        return false;
    }
    if(C.pass != P_DRAW || !visible(r)) return false;
    row_chrome(r, false, sx - 6, r.dyy + 8, W - (SP - 2 * RAD) + 12, r.h - 16, 14);
    row_label(r, label, nullptr, sx - r.x - 40);
    float cy = (float)r.dyy + r.h * 0.5f;
    for(int i = 0; i < n; i++){
        float cx = (float)(sx + i * SP + RAD);
        bool hov = r.hover && pt_in(C.mx, C.my, sx + i * SP - 4, r.sy + 4, 2 * RAD + 8, r.h - 8);
        float s = Spring(r.id ^ (0x100u + (uint32_t)i), i == *sel ? 1.0f : 0.0f, 1);
        float h = Spring(r.id ^ (0x200u + (uint32_t)i), hov ? 1.0f : 0.0f, 0);
        float rr = (float)RAD + h * 1.5f - s * 2.0f;
        if(s > 0.02f){
            disc(cx, cy, (float)RAD + 3.5f * s, colors[i], (int)(255 * f_clamp(s, 0, 1)));
            disc(cx, cy, (float)RAD + 1.5f * s, g_theme.card, (int)(255 * f_clamp(s, 0, 1)));
        }
        disc(cx, cy, rr, colors[i], 255);
        if(s > 0.3f){
            uint32_t ck = luma(colors[i]) > 186 ? 0xFF1C1C1E : 0xFFFFFFFF;
            Icon(IC_CHECK, (int)(cx - 8), (int)(cy - 8), 16, ck, (int)(255 * f_clamp((s - 0.3f) / 0.7f, 0, 1)));
        }
    }
    return false;
}

// ── meter ───────────────────────────────────────────────────────────────────
void Meter(const char* label, const char* value, int pct, const char* sub){
    KRow r = row_begin(label, sub, false, sub ? 78 : 64);
    if(C.pass != P_DRAW || !visible(r)) return;
    pct = k_clamp(pct, 0, 100);
    int vw = value ? TextWidth(FS_VALUE, value) : 0;
    TextFit(r.x + 16, r.dyy + 12, FS_LABEL, label, r.hit_search ? g_theme.accent : g_theme.text, r.w - 48 - vw);
    if(value) Text(r.x + r.w - 16 - vw, r.dyy + 12, FS_VALUE, value, g_theme.text2);
    int by = r.dyy + 40, bw = r.w - 32;
    fill_round(r.x + 16, by, bw, 6, 3, g_theme.track, 255);
    float f = SpringSeed(r.id ^ 1u, (float)pct / 100.0f, 2, 0.0f);
    int fw = (int)(f_clamp(f, 0, 1) * bw + 0.5f);
    uint32_t c = pct >= 90 ? g_theme.danger : (pct >= 75 ? g_theme.warn : g_theme.accent);
    if(fw > 1) fill_round(r.x + 16, by, fw, 6, 3, c, 255);
    if(sub) TextFit(r.x + 16, r.dyy + 54, FS_SUB, sub, g_theme.text2, bw);
}

// ── tiles ───────────────────────────────────────────────────────────────────
bool Tiles(const char* const* names, int n, int* sel, int tw, int th, TilePaint paint){
    int gap = 16;
    int avail = C.col_w - 32;
    int per = k_max(1, (avail + gap) / (tw + gap));
    if(per > n) per = n;
    int lines = (n + per - 1) / per;
    int cell_h = th + 34;
    int h = 16 + lines * cell_h + 4;
    KRow r = row_begin(names && n ? names[0] : "tiles", nullptr, true, h);
    int total_w = per * tw + (per - 1) * gap;
    int x0 = r.x + (r.w - total_w) / 2;
    if(C.pass == P_EVENT){
        for(int i = 0; i < n; i++){
            int tx = x0 + (i % per) * (tw + gap), ty = r.sy + 16 + (i / per) * cell_h;
            if(ev_click_in(tx - 4, ty - 4, tw + 8, th + 30)){
                C.consumed = true;
                if(*sel != i){ *sel = i; mark(true); return true; }
                return false;
            }
        }
        if(ev_key_for(r)){
            int k = C.ev_key, nv = *sel < 0 ? 0 : *sel;
            if(k == K_LEFT) nv--; else if(k == K_RIGHT) nv++; else return false;
            nv = k_clamp(nv, 0, n - 1); C.consumed = true;
            if(nv != *sel){ *sel = nv; mark(true); return true; }
        }
        return false;
    }
    if(C.pass != P_DRAW || !visible(r)) return false;
    for(int i = 0; i < n; i++){
        int tx = x0 + (i % per) * (tw + gap);
        int hty = r.sy + 16 + (i / per) * cell_h;
        bool hov = r.hover && pt_in(C.mx, C.my, tx - 4, hty - 4, tw + 8, th + 30);
        float hv = Spring(r.id ^ (0x300u + (uint32_t)i), hov ? 1.0f : 0.0f, 1);
        float s = Spring(r.id ^ (0x400u + (uint32_t)i), i == *sel ? 1.0f : 0.0f, 1);
        int ty = r.dyy + 16 + (i / per) * cell_h - (int)(hv * 3.0f + 0.5f);
        Shadow(tx, ty, tw, th, 10, 4, 2, (int)(40 + hv * 50));
        PushClip(tx, ty, tw, th);
        if(paint) paint(i, tx, ty, tw, th);
        mask_corners(tx, ty, tw, th, 10, g_theme.card);
        PopClip();
        stroke_round(tx, ty, tw, th, 10, g_theme.dark ? 0xFF000000u : g_theme.card_line, g_theme.dark ? 90 : 220);
        if(s > 0.02f){
            int a = (int)(f_clamp(s, 0, 1) * 255.0f);
            int g = (int)((1.0f - f_clamp(s, 0, 1)) * 5.0f);
            for(int k = 0; k < 3; k++)
                stroke_round(tx - 3 - k - g, ty - 3 - k - g, tw + 6 + 2 * (k + g), th + 6 + 2 * (k + g), 12 + k + g, g_theme.accent, k == 2 ? a / 2 : a);
        }
        if(r.focus && C.focus_visible)
            stroke_round(tx - 7, ty - 7, tw + 14, th + 14, 16, g_theme.accent, i == *sel ? 140 : 0);
        const char* nm = names[i];
        int nw = k_min(TextWidth(13.0f, nm, i == *sel), tw);
        TextFit(tx + (tw - nw) / 2, ty + th + 9, 13.0f, nm, i == *sel ? g_theme.text : g_theme.text2, tw, i == *sel);
    }
    return false;
}

Rect Custom(int h, bool separator){
    int save = C.row_in_group;
    if(!separator) C.row_in_group = 0;
    KRow r = row_begin("~custom", nullptr, false, h);
    if(!separator) C.row_in_group = save + 1;
    Rect out = { 0, 0, 0, 0 };
    if(C.pass == P_DRAW && visible(r)){ out.x = r.x; out.y = r.dyy; out.w = r.w; out.h = r.h; }
    return out;
}

// ════════════════════════════════════════════════════════════════════════════
//  legacy helpers (display page): old geometry, new look (satoru)
// ════════════════════════════════════════════════════════════════════════════
void Toggle(int x, int y, bool on){
    uint32_t id = 0x1E6A0000u ^ ((uint32_t)(x & 0xFFF) << 12) ^ (uint32_t)(y & 0xFFF);
    float p = Spring(id, on ? 1.0f : 0.0f, 1);
    float c = Spring(id ^ 0x55u, on ? 1.0f : 0.0f, 0);
    fill_round(x, y, TOGGLE_W, TOGGLE_H, TOGGLE_H / 2, mix(g_theme.track, g_theme.accent, (int)(f_clamp(c, 0, 1) * 255)), 255);
    float kx = (float)x + 10.0f + f_clamp(p, -0.1f, 1.1f) * (float)(TOGGLE_W - 20);
    disc(kx, (float)y + TOGGLE_H * 0.5f, 8.0f, g_theme.knob, 255);
}
bool ToggleHit(int x, int y, int mx, int my){
    return mx >= x && mx < x + TOGGLE_W && my >= y && my < y + TOGGLE_H;
}
void Slider(int x, int y, int w, int pct){
    pct = k_clamp(pct, 0, 100);
    uint32_t id = 0x51DE0000u ^ ((uint32_t)(x & 0xFFF) << 12) ^ (uint32_t)(y & 0xFFF);
    float p = Spring(id, (float)pct / 100.0f, 3);
    int cy = y + 4 + SLIDER_H / 2;
    fill_round(x, cy - 2, w, 4, 2, g_theme.track, 255);
    int fw = (int)(f_clamp(p, 0, 1) * w + 0.5f);
    if(fw > 0) fill_round(x, cy - 2, fw, 4, 2, g_theme.accent, 255);
    disc((float)(x + fw), (float)cy, 8.5f, g_theme.knob, 255);
    stroke_round(x + fw - 9, cy - 9, 18, 18, 9, 0xFF000000u, 30);
}
int SliderHit(int x, int y, int w, int mx, int my){
    if(my < y - 4 || my > y + SLIDER_H + 8) return -1;
    if(mx < x || mx > x + w || w <= 0) return -1;
    return k_clamp(((mx - x) * 100) / w, 0, 100);
}
void Dropdown(int x, int y, int w, const char* value){
    fill_round(x, y, w, DROPDOWN_H, 7, g_theme.field, 255);
    stroke_round(x, y, w, DROPDOWN_H, 7, g_theme.field_line, 200);
    // left/right step zones are the arrows; draw them as chevrons (satoru)
    int cy = y + DROPDOWN_H / 2;
    Icon(IC_CHEV_L, x + 2, cy - 8, 16, g_theme.text2, 255);
    Icon(IC_CHEV_R, x + w - 18, cy - 8, 16, g_theme.text2, 255);
    if(value){
        int tw = TextWidth(13.5f, value);
        int tx = x + (w - tw) / 2;
        if(tx < x + 22) tx = x + 22;
        PushClip(x + 20, y, w - 40, DROPDOWN_H);
        Text(tx, y + 2, 13.5f, value, g_theme.text);
        PopClip();
    }
}
int DropdownHit(int x, int y, int w, int mx, int my){
    if(my < y || my >= y + DROPDOWN_H) return -1;
    if(mx >= x && mx < x + 20) return 0;
    if(mx >= x + w - 20 && mx < x + w) return 1;
    return -1;
}
void SectionHeader(int x, int y, const char* text){
    Text(x, y, 13.0f, text, g_theme.text2, true);
    fill_round(x, y + 19, 560, 1, 0, g_theme.sep, 255);
}
void Row(int x, int y, const char* label, const char* value){
    Text(x, y, 14.0f, label, g_theme.text);
    if(value) Text(x + 150, y, 14.0f, value, g_theme.text2);
}

static int  s_len(const char* s){ int n=0; if(s) while(s[n]) n++; return n; }
void IntToStr(int v, char* b, int mx){
    if(mx < 2){ if(mx > 0) b[0] = 0; return; }
    if(v < 0){ b[0] = '-'; IntToStr(-v, b + 1, mx - 1); return; }
    char t[16]; int n = 0; do { t[n++] = (char)('0' + (v % 10)); v /= 10; } while(v && n < 15);
    int i = 0; while(n > 0 && i < mx - 1) b[i++] = t[--n]; b[i] = 0;
}
void StrCpy(char* d, const char* s, int mx){ int i = 0; if(s) while(s[i] && i < mx - 1){ d[i] = s[i]; i++; } if(mx > 0) d[i] = 0; }
void StrApp(char* d, const char* s, int mx){ int n = s_len(d), i = 0; if(s) while(s[i] && n < mx - 1){ d[n++] = s[i++]; } if(mx > 0) d[n] = 0; }
int  StrLen(const char* s){ return s_len(s); }

}  // namespace SettingsUI

// ════════════════════════════════════════════════════════════════════════════
//  popover (dropdown menu)
// ════════════════════════════════════════════════════════════════════════════
namespace SKit {

static int g_pop_x, g_pop_y, g_pop_w, g_pop_h;
static const int POP_ITEM = 32, POP_PAD = 6;

static void pop_geom(int win_x, int win_y, int win_w, int win_h){
    int maxt = 0;
    for(int i = 0; i < C.pop.n; i++) maxt = k_max(maxt, SettingsUI::TextWidth(14.0f, C.pop.items[i]));
    g_pop_w = k_max(C.pop.aw, maxt + 64);
    g_pop_h = C.pop.n * POP_ITEM + 2 * POP_PAD;
    g_pop_x = C.pop.ax + C.pop.aw - g_pop_w;
    if(g_pop_x < win_x + 8) g_pop_x = win_x + 8;
    if(g_pop_x + g_pop_w > win_x + win_w - 8) g_pop_x = win_x + win_w - 8 - g_pop_w;
    g_pop_y = C.pop.ay + C.pop.ah + 6;
    if(g_pop_y + g_pop_h > win_y + win_h - 8) g_pop_y = C.pop.ay - 6 - g_pop_h;
    if(g_pop_y < win_y + 8) g_pop_y = win_y + 8;
}

void DrawPopover(int win_x, int win_y, int win_w, int win_h){
    float p = SettingsUI::Spring(0xB0B0B0B1u, C.pop.open ? 1.0f : 0.0f, C.pop.open ? 1 : 0);
    if(p < 0.01f && !C.pop.open) return;
    pop_geom(win_x, win_y, win_w, win_h);
    float pc = f_clamp(p, 0.0f, 1.2f);
    bool above = g_pop_y < C.pop.ay;
    int vis_h = (int)(g_pop_h * (0.82f + 0.18f * f_clamp(pc, 0, 1.1f)));
    if(vis_h > g_pop_h + 4) vis_h = g_pop_h + 4;
    int oy = (int)((1.0f - f_clamp(pc, 0, 1)) * 8.0f) * (above ? 1 : -1);
    int y = above ? g_pop_y + (g_pop_h - vis_h) + oy : g_pop_y + oy;
    int saved_alpha = C.alpha;
    C.alpha = (int)(f_clamp(pc, 0, 1) * 255.0f);
    const SettingsUI::Theme& t = SettingsUI::T();
    uint32_t bg = t.dark ? 0xFF2A2A30 : 0xFFFFFFFF;
    Shadow(g_pop_x, y, g_pop_w, vis_h, 12, 10, 4, t.dark ? 120 : 60);
    fill_round(g_pop_x, y, g_pop_w, vis_h, 12, bg, 255);
    stroke_round(g_pop_x, y, g_pop_w, vis_h, 12, t.dark ? 0xFF3A3A42 : 0xFFDCDCE2, 255);
    PushClip(g_pop_x, y, g_pop_w, vis_h);
    C.under = bg;
    float hp = SettingsUI::Spring(0xB0B0B0B2u, (float)(C.pop.hi < 0 ? 0 : C.pop.hi), 1);
    int base = above ? y - (g_pop_h - vis_h) : y;
    if(C.pop.hi >= 0)
        fill_round(g_pop_x + 5, base + POP_PAD + (int)(hp * POP_ITEM + 0.5f) + 1, g_pop_w - 10, POP_ITEM - 2, 7, t.accent, 255);
    for(int i = 0; i < C.pop.n; i++){
        int iy = base + POP_PAD + i * POP_ITEM;
        float near = 1.0f - f_clamp(f_abs(hp - (float)i), 0, 1);
        uint32_t fg = mix(t.text, t.on_accent, C.pop.hi >= 0 ? (int)(near * 255.0f) : 0);
        if(i == C.pop.sel) SettingsUI::Icon(IC_CHECK, g_pop_x + 12, iy + 8, 16, fg, 255);
        C.under = mix(bg, t.accent, C.pop.hi >= 0 ? (int)(near * 255.0f) : 0);
        TextFit(g_pop_x + 36, iy + 7, 14.0f, C.pop.items[i], fg, g_pop_w - 48);
    }
    PopClip();
    C.alpha = saved_alpha;
}

bool PopoverEvent(Ev ev, int x, int y, int key){
    if(!C.pop.open) return false;
    bool inside = pt_in(x, y, g_pop_x, g_pop_y, g_pop_w, g_pop_h);
    int item = inside ? (y - g_pop_y - POP_PAD) / POP_ITEM : -1;
    if(item >= C.pop.n) item = -1;
    switch(ev){
    case EV_MOVE:
        if(item >= 0) C.pop.hi = item;
        return inside;
    case EV_DOWN: return true;
    case EV_UP: return true;
    case EV_CLICK:
        if(item >= 0){ C.pick_id = C.pop.id; C.pick_index = item; C.pop.open = false; return true; }
        // a click on the anchor button closes without reopening (satoru)
        C.pop.open = false;
        C.pick_id = 0;
        return true;
    case EV_KEY:
        if(key == K_UP){ C.pop.hi = C.pop.hi <= 0 ? C.pop.n - 1 : C.pop.hi - 1; return true; }
        if(key == K_DOWN || key == K_TAB){ C.pop.hi = (C.pop.hi + 1) % (C.pop.n ? C.pop.n : 1); return true; }
        if(key == K_ENTER || key == K_SPACE){
            if(C.pop.hi >= 0){ C.pick_id = C.pop.id; C.pick_index = C.pop.hi; }
            C.pop.open = false; return true;
        }
        if(key == K_ESC){ C.pop.open = false; C.pick_id = 0; return true; }
        return true;
    default: return false;
    }
}

}  // namespace SKit
// end (satoru)
