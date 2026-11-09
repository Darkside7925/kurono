#pragma once
#include "../kernel/types.h"

//  boot animation: the kurono emblem is traced, filled, then breaks into its
//  two pieces, which fall to the bottom edge and smash together; an upward
//  burst and a rising light wave reveal the desktop through a blur that pulls
//  into focus. (satoru)
//
//  threading: the formation and hold loop run on one application processor
//  while the bsp keeps initialising (it never waits on the animation). the gui
//  process takes over at its first frame and plays the tail as an overlay on
//  top of the real, already live desktop. every frame is a pure function of
//  the real clock (Timer::GetRealMs64), so the motion is identical at any
//  refresh rate. (satoru)

namespace BootAnim {

// bsp, where the old splash drew. mode: 0 = off (caller draws the old splash),
// 1 = full, 2 = fast tail. returns true when the animation owns the screen. (satoru)
bool Start(int mode);

// test hooks (kurono.bootanim.hold=MS / kurono.bootanim.early=MS): show the
// hold loop of a slow boot or the formation warp of a fast one. (satoru)
void SetTest(int hold_ms, int early_ms);

// bsp: a real boot stage was reached (0..95 %); drives the loading bar. pause,
// resume and the gui takeover add their own steps, and the bar only completes
// when the gui is up. (satoru)
void Progress(int pct);

// ap dispatch loop hook. cheap when there is nothing to do; when a job is
// posted one ap claims it and renders until the gui takes over. (satoru)
bool ApPending();
void ApService();

// bsp: stop the ap from touching the back buffer around display
// reconfiguration (ui scale, desktop layout), and let it continue after. (satoru)
void Pause();
void Resume();

// gui process, once at entry: take the screen over from the ap. (satoru)
void GuiTakeover();

// true while the overlay still has frames to draw (keeps the gui loop busy). (satoru)
bool Active();

// true while the boot scene hides the whole desktop, so the gui loop can skip
// composing a desktop nobody sees (it is needed again before the reveal). (satoru)
bool CoversScreen();

// shutdown / restart: play the boot film backwards (blur in, the light falls,
// sparks converge, the pieces fly back up and rejoin, the outline un-draws,
// black). drawn by the gui loop; a request during the boot film waits for it.
// action 0: the caller waits on ShutdownDone() and powers off itself;
// action 1: the gui loop halts the cpu once the film is black. returns false
// when animations are off, so the caller just proceeds. (satoru)
bool StartShutdown(int action);
bool ShutdownDone();

// gui frame: composite the overlay into the back buffer, after the desktop
// and cursor were drawn and right before the present. no-op when inactive. (satoru)
void Composite();

}

// end (satoru)
