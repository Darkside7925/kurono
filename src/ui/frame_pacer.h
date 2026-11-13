#pragma once
#include "../kernel/types.h"

//  frame pacer: decides the rate the gui loop composites at and hands it a
//  sub-millisecond frame period. software pacing only - it never touches a
//  display register or sets a mode; gop already scans the panel out at its
//  native rate. (satoru)
//
//  the rate is min(cap, what this machine sustains, highest lit output):
//    cap  = kurono.hz=N if given, else Settings > Display "Refresh rate"
//           (display.refresh_mode: 0 = auto), where auto = the detected panel
//           rate (intel pipe timings, else a measured vga vsync, else 60)
//           snapped down to a step, clamped to the highest refresh among the
//           lit outputs so no pipe is asked for frames it cannot show. vsync
//           off keeps the old 360 cap before the output clamp.
//    rate = the cap, stepped down 240 > 165 > 144 > 120 > 90 > 60 while frames
//           that had real work keep missing their budget, and back up after a
//           clean stretch with hysteresis. idle frames and boot animation
//           frames never count as load; an all-idle stretch counts as spare
//           capacity and recovers. never below 60. (satoru)
namespace FramePacer {

// the detected panel refresh in millihertz and where it came from ("igpu",
// "vga", "default"). called once at boot; 0 means unknown. (satoru)
void SetPanelMilliHz(uint32_t mhz, const char* source);
uint32_t PanelHz();
const char* PanelSource();

// kurono.hz=N (0 = none). overrides the persisted setting. (satoru)
void SetCmdlineHz(uint32_t hz);
uint32_t CmdlineHz();

// re-read display.refresh_mode / display.vsync and recompute the cap. (satoru)
void ReloadConfig();

// the lit outputs (or their refresh) changed: re-clamp the cap to the highest
// refresh a pipe can actually show. (satoru)
void OutputsChanged();

// Settings: 0 = auto, else a fixed cap. persists display.refresh_mode. (satoru)
void SetMode(uint32_t hz);
uint32_t Mode();

// the ceiling the policy allows and the rate actually paced now. (satoru)
uint32_t CapHz();
uint32_t CurrentHz();

// frame period of CurrentHz() in nanoseconds (exact, no 1 ms rounding). (satoru)
uint64_t PeriodNs();

// monotonic tsc clock in ns for the gui loop's pacing. (satoru)
uint64_t NowNs();

// called by the gui loop after each composited frame. cost_ns = render +
// present, late_ns = how far past its deadline the frame started, continuous =
// the previous iteration also rendered (an idle gap is not a miss), real =
// a real desktop frame (boot animation / hand-off frames are not), work = this
// frame had damage or an active animation (an idle fallback frame did not).
// only real continuous frames with work count as load; an all-idle window
// counts as spare capacity so the rate can recover. (satoru)
void OnFrame(uint64_t cost_ns, uint64_t late_ns, bool continuous, bool real, bool work);

}  // namespace FramePacer

// end (satoru)
