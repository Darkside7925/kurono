#pragma once
#include "../kernel/types.h"
#include "../drivers/graphics.h"
#include "../drivers/mouse.h"
#include "../drivers/keyboard.h"
#include "../ui/font.h"
#include "../ui/text_layout.h"

// Animation tweening helper
class Animation {
public:
    enum Type { Linear, EaseIn, EaseOut, EaseInOut };
    static float Lerp(float a, float b, float t) { return a + (b - a) * t; }
    static float Ease(float t, Type type) {
        switch (type) {
            case EaseIn: return t * t;
            case EaseOut: return t * (2 - t);
            case EaseInOut: return t < 0.5f ? 2 * t * t : -1 + (4 - 2 * t) * t;
            default: return t;
        }
    }
};

#include "../drivers/serial.h"

class Widget {
public:
    static bool high_contrast_mode;
    int x, y, w, h;
    bool visible = true;
    bool focused = false;
    
    virtual void Draw() = 0;
    virtual void OnClick(int mx, int my) { (void)mx; (void)my; }
    virtual void OnKey(char c) { (void)c; }
    
    bool Contains(int mx, int my) {
        return mx >= x && mx < x + w && my >= y && my < y + h;
    }
};

class Button : public Widget {
public:
    const char* text;
    uint32_t bg_color = 0xFF444444;
    uint32_t text_color = 0xFFFFFFFF;
    uint32_t hover_color = 0xFF666666;
    float font_size = 16.0f;
    void (*callback)() = nullptr;
    
    Button(int x, int y, int w, int h, const char* text, void (*cb)()) {
        this->x = x; this->y = y; this->w = w; this->h = h;
        this->text = text; this->callback = cb;
    }
    
    void Draw() override {
        if (!visible) return;
        int mx, my; Mouse::GetPosition(mx, my);
        bool hover = Contains(mx, my);
        Graphics::FillRectRounded(x, y, w, h, 8, hover ? hover_color : bg_color);
        // Center text vertically: y + (h - font_size)/2 is top-left, but DrawString takes top-left.
        // Actually DrawString y is usually baseline or top-left depending on implementation.
        // STB Truetype usually treats y as baseline? No, usually top-left in simple implementations, 
        // but let's check FontTTF::DrawString. 
        // Looking at font.cpp, it calculates offsets. 
        // Let's assume top-left for now or approximate centering.
        int text_y = y + (h - (int)font_size) / 2;
        FontTTF::DrawStringCenter(x + w/2, text_y, font_size, text, text_color);
    }
    
    void OnClick(int mx, int my) override {
        if (visible && Contains(mx, my) && callback) callback();
    }
};

class InputField : public Widget {
public:
    char buffer[64];
    int cursor_pos = 0;
    const char* placeholder;
    bool is_password = false;
    float font_size = 16.0f;
    
    InputField(int x, int y, int w, int h, const char* ph) {
        this->x = x; this->y = y; this->w = w; this->h = h;
        this->placeholder = ph;
        buffer[0] = 0;
    }
    
    void Draw() override {
        if (!visible) return;
        
        if (Widget::high_contrast_mode) {
            Graphics::FillRect(x - 2, y - 2, w + 4, h + 4, 0xFFFFFFFF); // White Border
            Graphics::FillRect(x, y, w, h, 0xFF000000); // Black Inner
        } else {
            Graphics::FillRectRounded(x, y, w, h, 4, 0xFF222222);
            Graphics::FillRectRounded(x, y, w, h, 4, focused ? 0xFF0088FF : 0xFF666666); // Border
            Graphics::FillRectRounded(x+2, y+2, w-4, h-4, 4, 0xFF000000); // Inner
        }
        
        const char* display = buffer;
        char pwd_buf[64];
        if (is_password) {
            int len = 0; while(buffer[len]) len++;
            for(int i=0;i<len;i++) pwd_buf[i] = '*';
            pwd_buf[len] = 0;
            display = pwd_buf;
        }
        
        int text_y = y + (h - (int)font_size) / 2;
        
        if (display[0] == 0 && !focused) {
            FontTTF::DrawString(x + 10, text_y, font_size, placeholder, 0xFF888888);
        } else {
            FontTTF::DrawString(x + 10, text_y, font_size, display, 0xFFFFFFFF);
        }
        
        if (focused) {
            // Draw cursor
            int tw = FontTTF::Measure(font_size, display);
            Graphics::FillRect(x + 10 + tw, y + 6, 2, h - 12, 0xFFFFFFFF);
        }
    }
    
    void OnKey(char c) override {
        if (!visible || !focused) return;
        
        SerialLogger::Log("InputField: OnKey '");
        char tmp[2] = {c, 0};
        SerialLogger::Log(tmp);
        SerialLogger::Log("'\r\n");
        
        int len = 0; while(buffer[len]) len++;
        if (c == '\b') {
            if (len > 0) buffer[--len] = 0;
        } else if (c >= 32 && c <= 126) {
            if (len < 63) {
                buffer[len++] = c;
                buffer[len] = 0;
            }
        }
    }
};

class TextBox : public Widget {
public:
    const char* text;
    float font_size = 16.0f;
    uint32_t color = 0xFFFFFFFF;
    TextLayout::Alignment align = TextLayout::Left;
    TextLayout layout;
    
    TextBox(int x, int y, int w, int h, const char* text) {
        this->x = x; this->y = y; this->w = w; this->h = h;
        this->text = text;
        Refresh();
    }
    
    void Refresh() {
        layout.Layout(text, font_size, w);
    }
    
    void Draw() override {
        if (!visible) return;
        layout.Draw(x, y, font_size, color, align);
    }
    
    void SetText(const char* t) {
        text = t;
        Refresh();
    }
};
