#include "text_layout.h"
#include "font.h"

void TextLayout::Invalidate() {
    cached_text = nullptr;
    cached_font_size = 0.0f;
    cached_max_width = -1;
    line_count = 0;
}

void TextLayout::Layout(const char* text, float font_size, int max_width) {
    if (text == cached_text
        && font_size == cached_font_size
        && max_width == cached_max_width) {
        return;
    }
    cached_text = text;
    cached_font_size = font_size;
    cached_max_width = max_width;

    line_count = 0;
    if (!text || !FontTTF::ok || max_width <= 0) return;

    const char* ptr = text;

    while (*ptr && line_count < MAX_LINES) {
        const char* line_start = ptr;
        int current_width = 0;
        const char* last_space = nullptr;
        int width_at_last_space = 0;
        bool line_done = false;

        while (*ptr) {
            char c = *ptr;
            if (c == '\n') {
                lines[line_count++] = {line_start, (int)(ptr - line_start), current_width};
                ptr++;
                line_done = true;
                break;
            }

            char tmp[2] = {c, 0};
            int char_w = FontTTF::Measure(font_size, tmp);

            if (current_width + char_w > max_width && ptr != line_start) {
                if (last_space) {
                    lines[line_count++] = {line_start, (int)(last_space - line_start), width_at_last_space};
                    ptr = last_space + 1;
                } else {
                    lines[line_count++] = {line_start, (int)(ptr - line_start), current_width};
                }
                line_done = true;
                break;
            }

            current_width += char_w;
            if (c == ' ') {
                last_space = ptr;
                width_at_last_space = current_width - char_w;
            }
            ptr++;
        }

        if (!line_done) {
            lines[line_count++] = {line_start, (int)(ptr - line_start), current_width};
            break;
        }
    }
}

void TextLayout::Draw(int x, int y, float font_size, uint32_t color, Alignment align) {
    if (!FontTTF::ok) return;

    int line_h = (int)(font_size * 1.2f);
    if (line_h < 1) line_h = 1;

    for (int i = 0; i < line_count; i++) {
        int lx = x;
        if (align == Center) {
            lx = x - lines[i].width / 2;
        } else if (align == Right) {
            lx = x - lines[i].width;
        }

        char buf[256];
        int len = lines[i].length;
        if (len < 0) len = 0;
        if (len > 255) len = 255;
        const char* src = lines[i].start;
        if (!src) continue;
        for (int k = 0; k < len; k++) buf[k] = src[k];
        buf[len] = 0;

        FontTTF::DrawString(lx, y + i * line_h, font_size, buf, color);
    }
}

int TextLayout::GetHeight(float font_size) {
    int lh = (int)(font_size * 1.2f);
    if (lh < 1) lh = 1;
    return line_count * lh;
}

int TextLayout::GetWidth() {
    int max_w = 0;
    for (int i = 0; i < line_count; i++) {
        if (lines[i].width > max_w) max_w = lines[i].width;
    }
    return max_w;
}
