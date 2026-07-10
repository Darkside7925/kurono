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

    // layout text into lines based on max_width. results are cached and
    // re-computed only when the text pointer, font_size or width change.
    void Layout(const char* text, float font_size, int max_width);

    // force re-layout on next Layout() call regardless of cache state.
    void Invalidate();

    void Draw(int x, int y, float font_size, uint32_t color, Alignment align = Left);

    int GetHeight(float font_size);
    int GetWidth();

private:
    // cache key - re-layout when any of these change.
    const char* cached_text = nullptr;
    float       cached_font_size = 0.0f;
    int         cached_max_width = -1;
};
