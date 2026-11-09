#pragma once
#include "../kernel/types.h"

// intel display stage 3, first acceleration step: a hardware cursor plane on
// the pipes the firmware (pipe a) and stage 2 (pipe b) already light, on
// display version 14 (meteor lake / arrow lake). see
// scratchpad/oc_gpuaccel_design.md for the full analysis.
//
// the contract, all of it gated behind kurono.igpu.accel=1 (default off): (satoru)
//  - no new ggtt window, no ggtt pte write, no tlb invalidation. the cursor
//    image lives in the unused tail of the framebuffer the pipe already scans,
//    and every tail pte is validated (present, lm/physical relation, and
//    contiguous) against the known cpu framebuffer address before a byte is
//    written. (satoru)
//  - no guc, no huc, no forcewake, no power well change. only cursor plane
//    registers (plus the primary plane ddb/wm of the same pipe when a cursor
//    ddb has to be carved out of it) are ever written, all through one
//    allowlisted helper. pipe a timing/transcoder/pps/cdclk stay read only
//    and are diffed before and after. (satoru)
//  - every wait is bounded by the tsc; any failure restores the saved
//    registers, disables the hardware cursor and leaves the software cursor
//    path in charge. runtime position updates verify the pos readback and
//    fall back permanently for that pipe on a mismatch. (satoru)
//  - lines are "[igpu-accel]" in the serial log (KURONO.LOG on bare metal)
//    and in the shell output buffer. nothing is logged and nothing runs when
//    the flag is off. (satoru)
namespace IntelAccel {

// boot entry: gated by kurono.igpu.accel=1 (false = off, byte for byte the
// old path). fb_phys/w/h/pitch describe the scanout grub left lit, which is
// the one pipe a shows. runs the self-test, logs PASS/FAIL per step, and
// leaves both pipes ready but hidden; the first cursor update shows the
// hardware cursor on the output under the pointer. pte_fallback
// (on by default) allows a 5 page pte window in the padding of the same ggtt
// window when the firmware did not map the fb tail, with every old pte saved
// and restored; kurono.igpu.accel.noptefb=1 turns that fallback off. (satoru)
int Boot(bool enabled, bool pte_fallback, uint64_t fb_phys, uint32_t fb_w, uint32_t fb_h, uint32_t fb_pitch);

// runtime control for the `gpuaccel` shell command. on re-runs the enable
// and self-test, off restores every register this module changed. (satoru)
int Enable(char* out, int maxo);
int Off(char* out, int maxo);
int Status(char* out, int maxo);
int SelfTest(char* out, int maxo);

// ---- stage 2 zero-copy present (kurono.igpu.zerocopy=1, requires accel) ----
// call once after the compositor's render mode and output layout are final.
// stores the boot flag and, when requested, enters zero-copy: the compositor
// renders into a write-back ram buffer, the damage is clflushed and the present
// copy disappears. multi output maps the virtual desktop into a free ggtt
// window and gives every pipe its PLANE_OFFSET/PLANE_SIZE slice; single output
// uses its own ram buffer. every window pte is checked live-free before the
// write and read back after, old ptes and plane registers are saved, all waits
// are bounded, and any compositor relayout (or a self-check crc mismatch)
// restores the original surfaces first. (satoru)
int ZeroCopyBoot(bool requested);
int ZeroCopyEnable(char* out, int maxo);
void ZeroCopyDisable();
bool ZeroCopyActive();
// [gui] report: commits and commit failures since the last call. (satoru)
void TakeZeroCopyStats(uint32_t* commits, uint32_t* fails);

// true when at least one lit pipe has a working hardware cursor plane. (satoru)
bool Enabled();

// show the hardware cursor at global desktop (x, y). returns true when the
// hardware cursor consumed the update (the caller must not draw or damage
// for a pure move), false when the software cursor has to draw instead.
// x < 0 hides it. safe to call from the gui loop and the shell pump. (satoru)
bool CursorUpdate(int x, int y);
// show/hide all hardware cursors (Mouse::Show/Hide). (satoru)
void CursorVisible(bool on);

// gui report support: resets the window counters and returns the tsc cycles
// spent in CursorUpdate. only meaningful while Enabled(). (satoru)
uint64_t TakeGuiStats(uint32_t* hw, uint32_t* sw, uint32_t* fail);

const char* LastError();

}

// end (satoru)
