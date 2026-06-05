#pragma once
#include "../kernel/types.h"

// toast notification manager  -  top-right stacked cards that slide+fade
// in/out via the animation easing system and auto-dismiss after a timeout.
// fully static / fixed-array backed: no dynamic allocation. (satoru)

class NotificationManager {
public:
    // accent style for the left bar + icon glyph color. (satoru)
    enum IconType {
        ICON_INFO    = 0,
        ICON_SUCCESS = 1,
        ICON_WARNING = 2,
        ICON_ERROR   = 3,
    };

    // queue a toast. title/body are copied into a fixed buffer (bounded),
    // so callers may pass transient strings. duration_ms is the time the
    // card stays fully visible (excludes slide animations). (satoru)
    static void Post(const char* title, const char* body, int icon_type, uint32_t duration_ms);

    // draw all live toasts; call last each frame so they sit topmost.
    // folds timing in (advances lifecycle from Timer::GetRealMs). (satoru)
    static void Render();

    // optional explicit lifecycle advance; Render() already does this, but
    // the orchestrator may call it separately if it likes. (satoru)
    static void Tick();

    // drop every active toast immediately. (satoru)
    static void Clear();

    // number of currently-live toasts (for tests/diagnostics). (satoru)
    static int ActiveCount();

private:
    static const int MAX_TOASTS = 5;
    static const int TITLE_CAP  = 48;
    static const int BODY_CAP   = 96;

    // lifecycle phase of a single toast. (satoru)
    enum Phase {
        PHASE_FREE = 0,   // slot unused
        PHASE_IN,         // sliding in
        PHASE_HOLD,       // fully visible, counting down
        PHASE_OUT,        // sliding out, then freed
    };

    struct Toast {
        bool     used;
        Phase    phase;
        IconType icon;
        char     title[TITLE_CAP];
        char     body[BODY_CAP];
        uint32_t born_ms;       // when current phase began (real ms)
        uint32_t duration_ms;   // hold duration requested by caller
    };

    static Toast  toasts[MAX_TOASTS];
    static bool   initialized;

    static void   EnsureInit();
    static uint32_t AccentFor(IconType t);
    static const char* GlyphFor(IconType t);
};

// end (satoru)
