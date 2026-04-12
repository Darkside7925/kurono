#pragma once
#include "../kernel/types.h"
#include "font.h"

class TextLayout {
public:
    struct Line {
        const char* start;
        int length;
        int width;
    };

    static const int MAX_LINES = 64;
    Line lines[MAX_LINES];
    int line_count = 0;
    
    enum Alignment { Left, Center, Right };
    
    // layout text into lines based on max_width
    void Layout(const char* text, float font_size, int max_width);
    
    // draw the laid out text
    void Draw(int x, int y, float font_size, uint32_t color, Alignment align = Left);
    
    int GetHeight(float font_size);
    int GetWidth();
};
