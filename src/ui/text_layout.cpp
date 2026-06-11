#include "text_layout.h"
#include "font.h"

void TextLayout::Layout(const char* text, float font_size, int max_width) {
    line_count = 0;
    if (!text || !FontTTF::ok) return;
    
    const char* ptr = text;
    
    while (*ptr && line_count < MAX_LINES) {
        const char* line_start = ptr;
        int current_width = 0;
        const char* last_space = nullptr;
        int width_at_last_space = 0;
        
        while (*ptr) {
            char c = *ptr;
            if (c == '\n') {
                lines[line_count++] = {line_start, (int)(ptr - line_start), current_width};
                ptr++; // Skip newline
                goto next_line;
            }
            
            // Measure char (simplified, ideally use FontTTF::Measure for substrings)
            char tmp[2] = {c, 0};
            int char_w = FontTTF::Measure(font_size, tmp);
            
            if (current_width + char_w > max_width) {
                // Wrap
                if (last_space) {
                    // Wrap at last space
                    lines[line_count++] = {line_start, (int)(last_space - line_start), width_at_last_space};
                    ptr = last_space + 1; // Skip space
                } else {
                    // Force break
                    lines[line_count++] = {line_start, (int)(ptr - line_start), current_width};
                }
                goto next_line;
            }
            
            current_width += char_w;
            if (c == ' ') {
                last_space = ptr;
                width_at_last_space = current_width - char_w; // Width before space
            }
            ptr++;
        }
        
        // End of string
        lines[line_count++] = {line_start, (int)(ptr - line_start), current_width};
        break;
        
        next_line:;
    }
}

void TextLayout::Draw(int x, int y, float font_size, uint32_t color, Alignment align) {
    if (!FontTTF::ok) return;
    
    int line_h = (int)(font_size * 1.2f); // 1.2 line height
    
    for (int i = 0; i < line_count; i++) {
        int lx = x;
        if (align == Center) {
            // Recalculate width precisely or use stored width
            // Stored width might include trailing space? 
            // Let's remeasure for exact center if needed, or trust Layout.
            // Layout width includes visible chars.
            lx = x - lines[i].width / 2;
        } else if (align == Right) {
            lx = x - lines[i].width;
        }
        
        // Draw substring
        // FontTTF::DrawString expects null-terminated.
        // We need to copy to temp buffer or modify DrawString to take length.
        // Let's modify DrawString in future. For now, temp buffer.
        char buf[256];
        int len = lines[i].length;
        if (len > 255) len = 255;
        for(int k=0; k<len; k++) buf[k] = lines[i].start[k];
        buf[len] = 0;
        
        FontTTF::DrawString(lx, y + i * line_h, font_size, buf, color);
    }
}

int TextLayout::GetHeight(float font_size) {
    return line_count * (int)(font_size * 1.2f);
}

int TextLayout::GetWidth() {
    int max_w = 0;
    for(int i=0; i<line_count; i++) {
        if(lines[i].width > max_w) max_w = lines[i].width;
    }
    return max_w;
}
