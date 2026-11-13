//  kurono os - single definition point for the embedded wallpaper bitmaps
//
// the auto-generated wallpaper headers used to hold `static const` arrays,
// so every source file that included one got its own multi-megabyte copy in
// rodata (desktop.cpp and kurono_kernel.cpp both did). the headers now only
// declare the arrays and this file defines each bitmap once. (satoru)
#define KURONO_WP_IMPL
#include "wallpaper.h"
#include "wallpaper2.h"

// end (satoru)
