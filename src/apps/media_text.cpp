// kurono os - text helper for the media player (see media_text.h)
#include "media_text.h"

namespace MediaText {

int Width(const char* s, int px, bool bold) {
    if (!s) return 0;
    return kui::MeasureWidthI(bold ? kui::Face::SansBold : kui::Face::Sans, px, s);
}

void Draw(kui::Canvas& c, int x, int y, const char* s, uint32_t color, int px, bool bold) {
    if (!s) return;
    c.TextI(x, y, bold ? kui::Face::SansBold : kui::Face::Sans, px, s, color);
}

int Height(int px) { return kui::FontsReady() ? px + 4 : 16; }

} // namespace MediaText

// end (satoru)
