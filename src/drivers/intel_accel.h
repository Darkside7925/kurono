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

// ---- stage 2b double-buffered page flip (kurono.igpu.flip=1, default off) ----
// the compositor keeps its own back buffer; per pipe two scanout buffers with
// the firmware's exact layout are mapped into reserved ggtt windows (1-3 gb),
// the damage is copied into the idle one with non-temporal stores and
// PLANE_SURF flips at vblank. PLANE_SURF is the only register written per
// frame. needs the kurono.igpu.accel=1 pipe probe (the cursor itself may
// fail). qemu_test (kurono.igpu.fliptest=1) runs the same bookkeeping against a
// stand-in scanout that copies the latched buffer onto the real framebuffer
// after a simulated 240 hz / 60 hz vblank. call after the final layout. (satoru)
int FlipBoot(bool requested, bool qemu_test);
int FlipEnable(char* out, int maxo);
void FlipDisable(const char* why);
bool FlipActive();
// a deferred flip or a re-arm is waiting: the gui loop should call
// FlipService (with the framebuffer lock) instead of sleeping long. (satoru)
bool FlipBusy();
void FlipService();
// cumulative flip counters, never reset: bytes copied into the flip buffers,
// flips, flips that copied the whole slice, deferred flips, flip list merges.
// read by the gfx oracle. (satoru)
struct FlipTotals { uint64_t bytes, flips, full_flips, defers, merges; };
void GetFlipTotals(FlipTotals* out);
// [gui] report counters since the last call. (satoru)
void TakeFlipStats(uint32_t* flips, uint32_t* defers, uint32_t* fails, uint64_t* copy_kb,
                   uint64_t* copy_cycles, uint32_t* latch_max_us);

// ggtt entries for the gt driver out of the flip driver's reserved region,
// under the same rules as a flip window (nobody owns it, no enabled plane or
// cursor references it, old ptes saved, new ptes read back). saved needs npte
// entries. (satoru)
bool GgttReserve(uint64_t phys, uint32_t npte, int pat_index, uint32_t* ggtt_out, uint64_t* saved);
void GgttRelease(uint32_t ggtt, uint32_t npte, const uint64_t* saved);

// kurono.igpu.accel.wmcalc=1: the cursor ddb carve writes computed primary
// watermarks (all levels) instead of keeping the firmware's levels. call
// before Boot. (satoru)
void SetWmCalc(bool on);

// true when at least one lit pipe has a working hardware cursor plane. (satoru)
bool Enabled();

// show the hardware cursor at global desktop (x, y). returns true when the
// hardware cursor consumed the update (the caller must not draw or damage
// for a pure move), false when the software cursor has to draw instead.
// x < 0 hides it. safe to call from the gui loop and the shell pump. (satoru)
bool CursorUpdate(int x, int y);
// show/hide all hardware cursors (Mouse::Show/Hide). (satoru)
void CursorVisible(bool on);
// turn every pipe's cursor plane off now (raw mmio, never waits for a lock):
// the boot animation hand-off, reboot, shutdown and panic paths. the next
// pointer update shows it again on the pipe under the pointer. (satoru)
void CursorHideAll();

// gui report support: resets the window counters and returns the tsc cycles
// spent in CursorUpdate. only meaningful while Enabled(). (satoru)
uint64_t TakeGuiStats(uint32_t* hw, uint32_t* sw, uint32_t* fail);

const char* LastError();

}

// end (satoru)
