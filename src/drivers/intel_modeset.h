#pragma once
#include "../kernel/types.h"

// intel display stage 2: light an external displayport sink on a type-c
// port (dp-alt, c20 phy) through pipe b / transcoder b / plane 1 on display
// version 14 (meteor lake / arrow lake), scanning out a framebuffer kurono
// owns. the sequence is a transcription of linux i915 (hsw_crtc_enable,
// mtl_ddi_pre_enable_dp, intel_cx0pll_enable, intel_dp_start_link_train,
// icl_plane_update_*), each step cited in intel_modeset.cpp. (satoru)
//
// safety contract (satoru):
//  - every mmio write goes through one helper with an allowlist: pipe b,
//    transcoder b, plane 1 of pipe b, the chosen type-c port's ddi/phy/aux
//    registers, trans_clk_sel b, wm_linetime b, gamma/csc mode b, dss_ctl1
//    b, the pmdemand request pair and dbuf slices 3/4. anything else (pipe
//    a, transcoder a, port a, pps, cdclk, power wells other than pw_b) is
//    refused and logged. pipe a's registers are read before and after and
//    any difference is logged.
//  - every wait is bounded by the tsc and an iteration cap; interrupts are
//    never disabled; the whole run has a wall-time budget.
//  - any failure rolls back what this run enabled, in reverse order, and
//    restores the ggtt ptes it replaced. power wells, dbuf slices and the
//    pmdemand request are only ever raised, never lowered.
//  - lines are "[igpu-mode]" in the serial log (KURONO.LOG on bare metal)
//    and in the shell output buffer. harmless without an intel gpu. (satoru)
namespace IntelModeset {

// boot entry: runs only when the last stage 1 aux probe found an external
// dp sink on a type-c port. skipped when disabled is true
// (kurono.igpu.modeset=0) or when a root file KUROMODE.OFF exists on the
// KURONO fat partition. (satoru)
int Boot(bool disabled);

// `gpumode light [hz]`: light now (hz picks the edid timing at the native
// resolution closest to it, default 60). (satoru)
int Light(char* out, int maxo, uint32_t target_hz);
// `gpumode off`: undo everything Light enabled (the framebuffer memory is
// kept, a second screen may still point at it). (satoru)
int Off(char* out, int maxo);
// `gpumode status` (satoru)
int Status(char* out, int maxo);

// ---- live refresh switching on the lit pipe b sink (satoru) ----
// the integer refresh rates the sink's edid offers at the current resolution,
// ascending. 0 while nothing is lit or no edid was decoded. (satoru)
int Rates(uint32_t* hz, int max);
// the rate pipe b scans out now (0 while off). (satoru)
uint32_t CurrentHz();
uint32_t CurrentMilliHz();
// `gpumode rate HZ` / Settings > Display: turn pipe b off and light it again
// at the edid timing closest to hz at the current resolution. the accelerated
// present paths are suspended first and re-arm on the new mode. on failure the
// previous timing is restored and 0 is returned. (satoru)
int SetRate(char* out, int maxo, uint32_t hz);
// boot: re-light pipe b at UIConfig display.dp_hz after the config store is
// up (the early Boot() call runs before KVFS). 0 when nothing changed. (satoru)
int ApplySavedRate();
// push the lit pipe's current rate into the registered compositor output so
// the pacer ceiling and the settings page see it. no-op before registration.
// (satoru)
void PublishOutputRate();

// the lit output for the outputs layer: framebuffer physical address
// (identity mapped, 2 mb aligned, size a 2 mb multiple), mode and pitch.
// false while nothing is lit. (satoru)
bool GetOutput(uint64_t* phys, int* w, int* h, uint32_t* pitch);

// the ggtt offset plane 1 of pipe b scans while lit (the window ggtt_setup
// programmed), for stage 3 acceleration which reuses that mapping. false
// while nothing is lit. (satoru)
bool GetOutputGgtt(uint32_t* ggtt);

// kurono.igpu.legacytc=1: true when pipe b is lit on a legacy type-c port
// and the sink reported lock after enable, i.e. the external screen should
// carry the menu bar and dock (the boot code makes it the primary). (satoru)
bool LegacyTcLit();

}

// end (satoru)
