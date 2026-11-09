// kurono os - text helper for the media player
// =====================================================================
// the player is built without sse (it decodes jpeg from the gui process), and the float font sizes
// kui::Canvas::Text takes cross that boundary in xmm0 and arrive as zero. these thin wrappers take an
// int size and use kui's whole-pixel twins. (satoru)
#pragma once
#include <stdint.h>
#include "../kui/kui.h"

namespace MediaText {

// pixel width of s at px (smooth face when the modern style is active). (satoru)
int Width(const char* s, int px, bool bold = false);

// draw s with its line box top at y. (satoru)
void Draw(kui::Canvas& c, int x, int y, const char* s, uint32_t color, int px, bool bold = false);

// height of one line at px. (satoru)
int Height(int px);

} // namespace MediaText

// end (satoru)
