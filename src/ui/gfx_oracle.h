#pragma once
#include "../kernel/types.h"

//  the damage oracle (kurono.gfx.oracle=1, default off). a scripted desktop
//  scenario (menus, right-click menus, a window with changing content, a drag
//  over it, the control center, a toast, a ui scale change) is played through
//  the real input path. at checkpoints, once the scene is still, the whole
//  frame is rendered again with the default renderer and compared pixel for
//  pixel with
//    back   the compositor back buffer as the partial (scissor) frames left it
//    front  what the present path put on the screen (cpu copy, or the page
//           flip stand-in of kurono.igpu.fliptest=1)
//  the back buffer is put back afterwards, so the check does not clean up the
//  state it is looking at. also counts the bytes copied to the front buffer,
//  the damage rects and the full-frame fallbacks per phase (idle desktop,
//  moving cursor, dragged window). it never touches a display register. (satoru)
namespace GfxOracle {

void Configure(bool on, bool shots, int soak_secs);
bool Running();
// gui loop, top of each iteration: scripted pointer, clicks and ui actions. (satoru)
void Tick();
// gui loop, after the frame was presented, framebuffer lock held. (satoru)
void AfterPresent();

}  // namespace GfxOracle

// end (satoru)
