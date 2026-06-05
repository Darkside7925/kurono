#pragma once
//  kurono os  -  performance hud (satoru)
//  compact top-left overlay: fps, frame time, heap usage, window count,
//  uptime. self-contained; uses only graphics:: primitives. (satoru)

#include "../kernel/types.h"

namespace PerfHUD {
    // toggle visibility on/off (bound to a hotkey by the orchestrator). (satoru)
    void Toggle();
    // whether the hud is currently shown. (satoru)
    bool IsVisible();
    // draw the hud panel; returns immediately when not visible. call this
    // late in the gui render loop so it sits above windows. (satoru)
    void Render(uint32_t fps, uint32_t frame_ms);
}

// end (satoru)
