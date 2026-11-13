#pragma once
#include "../kernel/types.h"

//  frame statistics and a self-driving pointer / window-drag benchmark for
//  the compositor. everything here is off unless asked for:
//    kurono.gfxstats=1  one [frame] line per second while frames are drawn:
//                       frames, fps, render / present / work time (avg, p99,
//                       max), frames over the period budget, late frames.
//    kurono.gfxbench=1  after the desktop settles, run the benchmark once:
//                       a cursor phase (the pointer circles the primary
//                       output), a drag phase (a real titlebar drag through
//                       the normal input path) and an idle phase. one
//                       [gfxbench] result line per phase.
//    shell gfxbench     the same benchmark at runtime.
//  the benchmark only drives Mouse::mx/my and a synthetic left button; it
//  never touches a display register. (satoru)
namespace GfxBench {

// boot flags. (satoru)
void Configure(bool stats_line, bool run_at_boot);

// shell: start the benchmark (false if one is already running). (satoru)
bool Start();
bool Running();
// one line for the shell after a run (empty while running or before). (satoru)
const char* LastSummary();

// gui loop, top of each iteration: moves the synthetic pointer. (satoru)
void Tick();

// gui loop, after each composited frame: the frame's render and present
// time, how many render passes it took (damage scissor), the paced period
// and whether the previous iteration also rendered. (satoru)
void FrameDone(uint64_t render_ns, uint64_t present_ns, int passes,
               uint64_t period_ns, bool continuous, uint64_t late_ns);

}  // namespace GfxBench

// end (satoru)
