#include "notification.h"
#include "ui_elements.h"          // animation easing helpers (satoru)
#include "../drivers/graphics.h"
#include "../drivers/timer.h"

// fixed storage  -  no heap, lives in bss. (satoru)
NotificationManager::Toast NotificationManager::toasts[NotificationManager::MAX_TOASTS];
bool NotificationManager::initialized = false;

namespace {

// freestanding string copy with hard cap; always nul-terminates. (satoru)
void copy_str(char* dst, const char* src, int cap) {
    if (cap <= 0) return;
    int i = 0;
    if (src) {
        for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    }
    dst[i] = 0;
}

// card geometry / animation tuning (satoru)
constexpr int      CARD_W      = 320;
constexpr int      CARD_H      = 72;
constexpr int      MARGIN_X    = 16;   // gap from right edge
constexpr int      MARGIN_TOP  = 16;   // gap from top edge
constexpr int      GAP_Y       = 12;   // vertical gap between cards
constexpr int      CORNER      = 12;
constexpr uint32_t SLIDE_MS    = 260;  // in/out animation length
constexpr uint32_t BAR_W       = 5;    // accent bar width

constexpr uint32_t CARD_BG     = 0xFF1E1E24;  // dark card body (satoru)
constexpr uint32_t CARD_BG2    = 0xFF26262E;  // subtle gradient bottom
constexpr uint32_t TITLE_COL   = 0xFFFFFFFF;
constexpr uint32_t BODY_COL    = 0xFFB8B8C0;

} // namespace

void NotificationManager::EnsureInit() {
    if (initialized) return;
    for (int i = 0; i < MAX_TOASTS; i++) {
        toasts[i].used  = false;
        toasts[i].phase = PHASE_FREE;
    }
    initialized = true;
}

uint32_t NotificationManager::AccentFor(IconType t) {
    switch (t) {
        case ICON_SUCCESS: return 0xFF34C759;  // green
        case ICON_WARNING: return 0xFFFFCC00;  // amber
        case ICON_ERROR:   return 0xFFFF3B30;  // red
        case ICON_INFO:    return 0xFF0A84FF;  // blue
        default:           return 0xFF0A84FF;
    }
}

const char* NotificationManager::GlyphFor(IconType t) {
    switch (t) {
        case ICON_SUCCESS: return "OK";
        case ICON_WARNING: return "! ";
        case ICON_ERROR:   return "X ";
        case ICON_INFO:    return "i ";
        default:           return "i ";
    }
}

void NotificationManager::Post(const char* title, const char* body, int icon_type, uint32_t duration_ms) {
    EnsureInit();

    IconType icon = ICON_INFO;
    if (icon_type >= ICON_INFO && icon_type <= ICON_ERROR) icon = (IconType)icon_type;
    if (duration_ms == 0) duration_ms = 3000;  // sane default (satoru)

    // find a free slot; if full, evict the oldest holding/incoming one. (satoru)
    int slot = -1;
    for (int i = 0; i < MAX_TOASTS; i++) {
        if (!toasts[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < MAX_TOASTS; i++) {
            if (toasts[i].born_ms <= oldest) { oldest = toasts[i].born_ms; slot = i; }
        }
        if (slot < 0) slot = 0;
    }

    Toast& tt = toasts[slot];
    tt.used        = true;
    tt.phase       = PHASE_IN;
    tt.icon        = icon;
    tt.duration_ms = duration_ms;
    tt.born_ms     = Timer::GetRealMs();
    copy_str(tt.title, title, TITLE_CAP);
    copy_str(tt.body,  body,  BODY_CAP);
}

void NotificationManager::Tick() {
    EnsureInit();
    uint32_t now = Timer::GetRealMs();
    for (int i = 0; i < MAX_TOASTS; i++) {
        Toast& tt = toasts[i];
        if (!tt.used) continue;
        uint32_t age = now - tt.born_ms;  // wrap-safe unsigned delta (satoru)
        switch (tt.phase) {
            case PHASE_IN:
                if (age >= SLIDE_MS) { tt.phase = PHASE_HOLD; tt.born_ms = now; }
                break;
            case PHASE_HOLD:
                if (age >= tt.duration_ms) { tt.phase = PHASE_OUT; tt.born_ms = now; }
                break;
            case PHASE_OUT:
                if (age >= SLIDE_MS) { tt.used = false; tt.phase = PHASE_FREE; }
                break;
            default:
                tt.used = false; tt.phase = PHASE_FREE;
                break;
        }
    }
}

void NotificationManager::Clear() {
    EnsureInit();
    for (int i = 0; i < MAX_TOASTS; i++) {
        toasts[i].used  = false;
        toasts[i].phase = PHASE_FREE;
    }
}

int NotificationManager::ActiveCount() {
    EnsureInit();
    int n = 0;
    for (int i = 0; i < MAX_TOASTS; i++) if (toasts[i].used) n++;
    return n;
}

void NotificationManager::Render() {
    EnsureInit();
    Tick();

    int screen_w = Graphics::GetWidth();
    if (screen_w <= 0) return;
    const int rest_x = screen_w - CARD_W - MARGIN_X;   // settled left edge (satoru)
    const int off_x  = screen_w + 8;                   // fully off-screen right
    uint32_t now = Timer::GetRealMs();

    // stack live cards top-down in slot order. (satoru)
    int row = 0;
    for (int i = 0; i < MAX_TOASTS; i++) {
        Toast& tt = toasts[i];
        if (!tt.used) continue;

        uint32_t age = now - tt.born_ms;
        int   y = MARGIN_TOP + row * (CARD_H + GAP_Y);
        row++;

        // slide progress + fade alpha from the easing system. (satoru)
        int     x     = rest_x;
        uint8_t alpha = 255;
        if (tt.phase == PHASE_IN) {
            float p = Animation::EaseMs(age, SLIDE_MS, Animation::EaseOutCubic);
            x     = (int)Animation::Lerp((float)off_x, (float)rest_x, p);
            alpha = (uint8_t)(255.0f * Animation::Clamp01(p));
        } else if (tt.phase == PHASE_OUT) {
            float p = Animation::EaseMs(age, SLIDE_MS, Animation::EaseInOutQuint);
            x     = (int)Animation::Lerp((float)rest_x, (float)off_x, p);
            alpha = (uint8_t)(255.0f * Animation::Clamp01(1.0f - p));
        }

        // crossfade the card colours from transparent->solid against a
        // neutral backdrop so the fade reads even without true alpha. (satoru)
        uint32_t accent = AccentFor(tt.icon);
        uint32_t bg_top = Animation::LerpColor(0xFF101014, CARD_BG,  alpha);
        uint32_t bg_bot = Animation::LerpColor(0xFF101014, CARD_BG2, alpha);
        uint32_t acc    = Animation::LerpColor(0xFF101014, accent,   alpha);
        uint32_t tcol   = Animation::LerpColor(CARD_BG,    TITLE_COL, alpha);
        uint32_t bcol   = Animation::LerpColor(CARD_BG,    BODY_COL,  alpha);

        // soft drop shadow, then body, then accent bar. (satoru)
        Graphics::ApplyShadow(x + 3, y + 4, CARD_W, CARD_H, 0, 2, (uint8_t)(alpha / 3));
        Graphics::FillGradientV(x, y, CARD_W, CARD_H, bg_top, bg_bot);
        Graphics::FillRoundedRect(x, y, CARD_W, CARD_H, CORNER, bg_top);
        Graphics::FillRect(x + 1, y + 8, BAR_W, CARD_H - 16, acc);

        // icon glyph in the accent colour, title + body text. (satoru)
        int text_x = x + BAR_W + 14;
        Graphics::DrawString(x + BAR_W + 6, y + 10, GlyphFor(tt.icon), acc, bg_top);
        Graphics::DrawString(text_x + 18, y + 10, tt.title, tcol, bg_top);
        if (tt.body[0]) {
            Graphics::DrawString(text_x, y + 38, tt.body, bcol, bg_top);
        }
    }
}

// end (satoru)
