#pragma once
#include "../kernel/types.h"
#include "../drivers/graphics.h"
#include "../drivers/mouse.h"
#include "../drivers/keyboard.h"
#include "../ui/font.h"
#include "../ui/text_layout.h"

// Easing / motion helpers.  All time-driven, milliseconds.
//   t_ms   = elapsed milliseconds since the animation started
//   dur_ms = total animation duration in milliseconds
// Sample value at t = clamp(t_ms / dur_ms, 0..1) then run through the curve.
// Returns a 0..1 progress factor; use Lerp(a, b, progress) to interpolate.
class Animation {
public:
    enum Type {
        Linear,
        EaseIn,        // quadratic ease-in (legacy)
        EaseOut,       // quadratic ease-out (legacy)
        EaseInOut,     // quadratic ease-in-out (legacy)
        EaseOutCubic,  // 1 - (1-t)^3  -  best for "lands softly" reveals
        EaseInOutQuint // strong acceleration + deceleration for panel moves
    };

    static inline float Clamp01(float t) {
        if (t < 0.0f) return 0.0f;
        if (t > 1.0f) return 1.0f;
        return t;
    }

    static inline float Lerp(float a, float b, float t) {
        return a + (b - a) * t;
    }

    // 32-bit channel lerp.  alpha 0..255.  Crossfade two ARGB colours.
    static inline uint32_t LerpColor(uint32_t c0, uint32_t c1, uint8_t alpha) {
        uint32_t inv = 255u - alpha;
        uint32_t a = (((c0 >> 24) & 0xFF) * inv + ((c1 >> 24) & 0xFF) * alpha) / 255u;
        uint32_t r = (((c0 >> 16) & 0xFF) * inv + ((c1 >> 16) & 0xFF) * alpha) / 255u;
        uint32_t g = (((c0 >>  8) & 0xFF) * inv + ((c1 >>  8) & 0xFF) * alpha) / 255u;
        uint32_t b = (( c0        & 0xFF) * inv + ( c1        & 0xFF) * alpha) / 255u;
        return (a << 24) | (r << 16) | (g << 8) | b;
    }

    static float Ease(float t, Type type) {
        t = Clamp01(t);
        switch (type) {
            case EaseIn:        return t * t;
            case EaseOut:       return t * (2.0f - t);
            case EaseInOut:     return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
            case EaseOutCubic: {
                float u = 1.0f - t;
                return 1.0f - u * u * u;
            }
            case EaseInOutQuint:
                if (t < 0.5f) {
                    float u = 2.0f * t;
                    return 0.5f * u * u * u * u * u;
                } else {
                    float u = 2.0f * t - 2.0f;
                    return 1.0f + 0.5f * u * u * u * u * u;
                }
            default: return t;
        }
    }

    // ms-driven convenience.  Returns the curve value at t_ms/dur_ms.
    static inline float EaseMs(uint32_t t_ms, uint32_t dur_ms, Type type) {
        if (dur_ms == 0) return 1.0f;
        return Ease((float)t_ms / (float)dur_ms, type);
    }

    // libm-free exp(-x) for x >= 0.  Range-reduces by halving so we always
    // evaluate the Taylor series at a small argument, then squares back up.
    static inline float ExpNeg(float x) {
        if (x <= 0.0f) return 1.0f;
        int halves = 0;
        while (x > 0.25f) { x *= 0.5f; halves++; if (halves > 24) break; }
        // 6-term Taylor for e^-x at small x (|x|<=0.25): err < 1e-7
        float x2 = x * x;
        float x3 = x2 * x;
        float x4 = x2 * x2;
        float x5 = x4 * x;
        float y = 1.0f - x + x2 * 0.5f - x3 * (1.0f/6.0f)
                + x4 * (1.0f/24.0f) - x5 * (1.0f/120.0f);
        // square `halves` times to undo the halvings: (e^-(x/2^n))^(2^n)
        for (int i = 0; i < halves; i++) y = y * y;
        if (y < 0.0f) y = 0.0f;
        if (y > 1.0f) y = 1.0f;
        return y;
    }

    // Underdamped spring response, normalised so spring(0)=0 and the
    // value settles near 1.  Produces a bouncy "tap" feel.
    //   stiffness ≈ 12 (default) → ~250 ms settle
    //   damping   ≈ 0.55         → 1-2 visible overshoots
    static float Spring(float t, float stiffness = 12.0f, float damping = 0.55f) {
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        float w = stiffness;
        float d = damping;
        // y = 1 - e^(-d*w*t) * cos(w*sqrt(1-d^2)*t)
        float ex = ExpNeg(d * w * t);
        // sqrt(1 - d^2) via two Newton iterations
        float s = 1.0f - d * d;
        if (s < 0.0f) s = 0.0f;
        float sq = 0.5f + 0.5f * s;
        sq = 0.5f * (sq + s / (sq + 1e-6f));
        sq = 0.5f * (sq + s / (sq + 1e-6f));
        // wrap arg to (-pi, pi] for accurate cos via Taylor
        const float TWO_PI = 6.28318530718f;
        const float PI     = 3.14159265359f;
        float a = w * sq * t;
        while (a >  PI) a -= TWO_PI;
        while (a < -PI) a += TWO_PI;
        float a2 = a * a;
        float a4 = a2 * a2;
        float a6 = a4 * a2;
        float cs = 1.0f - a2 * 0.5f + a4 * (1.0f/24.0f) - a6 * (1.0f/720.0f);
        return 1.0f - ex * cs;
    }

    static inline float SpringMs(uint32_t t_ms, uint32_t dur_ms) {
        if (dur_ms == 0) return 1.0f;
        return Spring((float)t_ms / (float)dur_ms);
    }
};

#include "../drivers/serial.h"

class Widget {
public:
    static bool high_contrast_mode;
    int x = 0, y = 0, w = 0, h = 0;
    bool visible = true;
    bool focused = false;

    virtual ~Widget() {}
    virtual void Draw() = 0;
    virtual void OnClick(int mx, int my) { (void)mx; (void)my; }
    virtual void OnKey(char c) { (void)c; }

    bool Contains(int mx, int my) {
        return mx >= x && mx < x + w && my >= y && my < y + h;
    }
};

class Button : public Widget {
public:
    const char* text = nullptr;
    uint32_t bg_color = 0xFF444444;
    uint32_t text_color = 0xFFFFFFFF;
    uint32_t hover_color = 0xFF666666;
    float font_size = 16.0f;
    void (*callback)() = nullptr;

    Button(int x_, int y_, int w_, int h_, const char* t, void (*cb)()) {
        x = x_; y = y_; w = w_; h = h_;
        text = t; callback = cb;
    }

    void Draw() override {
        if (!visible) return;
        int mx, my; Mouse::GetPosition(mx, my);
        bool hover = Contains(mx, my);
        Graphics::FillRectRounded(x, y, w, h, 8, hover ? hover_color : bg_color);
        if (!text) return;
        float fs = font_size;
        if (fs > (float)(h - 4)) fs = (float)(h - 4);
        if (fs < 8.0f) fs = 8.0f;
        int tw = FontTTF::Measure(fs, text);
        while (tw > w - 8 && fs > 8.0f) {
            fs -= 1.0f;
            tw = FontTTF::Measure(fs, text);
        }
        int text_y = y + (h - (int)fs) / 2;
        FontTTF::DrawStringCenter(x + w/2, text_y, fs, text, text_color);
    }

    void OnClick(int mx, int my) override {
        if (visible && Contains(mx, my) && callback) callback();
    }
};

class InputField : public Widget {
public:
    char buffer[64];
    int cursor_pos = 0;
    const char* placeholder = nullptr;
    bool is_password = false;
    float font_size = 16.0f;

    InputField(int x_, int y_, int w_, int h_, const char* ph) {
        x = x_; y = y_; w = w_; h = h_;
        placeholder = ph;
        buffer[0] = 0;
    }

    void Draw() override {
        if (!visible) return;

        if (Widget::high_contrast_mode) {
            Graphics::FillRect(x - 2, y - 2, w + 4, h + 4, 0xFFFFFFFF);
            Graphics::FillRect(x, y, w, h, 0xFF000000);
        } else {
            Graphics::FillRectRounded(x, y, w, h, 4, 0xFF222222);
            Graphics::FillRectRounded(x, y, w, h, 4, focused ? 0xFF0088FF : 0xFF666666);
            Graphics::FillRectRounded(x+2, y+2, w-4, h-4, 4, 0xFF000000);
        }

        const char* display = buffer;
        char pwd_buf[64];
        if (is_password) {
            int len = 0; while (buffer[len] && len < 63) len++;
            for (int i = 0; i < len; i++) pwd_buf[i] = '*';
            pwd_buf[len] = 0;
            display = pwd_buf;
        }

        float fs = font_size;
        if (fs > (float)(h - 4)) fs = (float)(h - 4);
        if (fs < 8.0f) fs = 8.0f;
        int text_y = y + (h - (int)fs) / 2;

        if (display[0] == 0 && !focused) {
            if (placeholder) FontTTF::DrawString(x + 10, text_y, fs, placeholder, 0xFF888888);
        } else {
            FontTTF::DrawString(x + 10, text_y, fs, display, 0xFFFFFFFF);
        }

        if (focused) {
            int tw = FontTTF::Measure(fs, display);
            Graphics::FillRect(x + 10 + tw, y + 6, 2, h - 12, 0xFFFFFFFF);
        }
    }

    void OnKey(char c) override {
        if (!visible || !focused) return;
        int len = 0; while (buffer[len] && len < 63) len++;
        if (cursor_pos < 0) cursor_pos = 0;
        if (cursor_pos > len) cursor_pos = len;
        if (c == '\b') {
            if (cursor_pos > 0) {
                for (int i = cursor_pos - 1; i < len; i++) buffer[i] = buffer[i + 1];
                cursor_pos--;
            }
        } else if (c == '\t' || c == '\n') {
            // handled by parent
        } else if (c >= 32 && c <= 126) {
            if (len < 63) {
                for (int i = len; i > cursor_pos; i--) buffer[i] = buffer[i - 1];
                buffer[cursor_pos++] = c;
                buffer[len + 1] = 0;
            }
        }
        len = 0; while (buffer[len] && len < 63) len++;
        if (cursor_pos > len) cursor_pos = len;
    }
};

class TextBox : public Widget {
public:
    const char* text = nullptr;
    float font_size = 16.0f;
    uint32_t color = 0xFFFFFFFF;
    TextLayout::Alignment align = TextLayout::Left;
    TextLayout layout;

    TextBox(int x_, int y_, int w_, int h_, const char* t) {
        x = x_; y = y_; w = w_; h = h_;
        text = t;
    }

    void Refresh() {
        layout.Invalidate();
        layout.Layout(text, font_size, w);
    }

    void Draw() override {
        if (!visible) return;
        // cached on the layout side  -  only re-runs when text/size/width change.
        layout.Layout(text, font_size, w);
        layout.Draw(x, y, font_size, color, align);
    }

    void SetText(const char* t) {
        text = t;
        layout.Invalidate();
    }
};
