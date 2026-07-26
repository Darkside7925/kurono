#pragma once
//  kurono os - flat vector app icons drawn in code (no external image assets,
//  since the freestanding stb decoder is unreliable). a single Draw() renders a
//  tasteful flat glyph into a size x size box using graphics primitives, scaled
//  by `size` so it stays crisp at both ~30px (taskbar) and ~56px (desktop). the
//  rounded-tile background, shadow, hover and labels stay with the caller; this
//  only paints the inner symbol. (satoru)
#include "../kernel/types.h"

namespace AppIcons {

// stable id per app/desktop entry. GENERIC is the catch-all fallback. (satoru)
enum Id {
    TERMINAL = 0,
    FILES,
    CALCULATOR,
    EDITOR,
    SETTINGS,
    BROWSER,
    TASKS,
    MEDIA,
    HOME,
    FIREFOX,
    GENERIC
};

// paint the icon for `id` centered inside the box at (x,y) of side `size`. all
// internal geometry is derived from `size` so a single source draws at any
// scale. uses graphics primitives only (no fonts), so it never depends on the
// glyph cache. (satoru)
void Draw(int id, int x, int y, int size);

// map an app/desktop entry name (e.g. "Terminal", "File Browser", "Media
// Player") to an Id by a short prefix test. returns GENERIC when nothing
// matches. (satoru)
int IdForName(const char* n);

} // namespace AppIcons
// end (satoru)
