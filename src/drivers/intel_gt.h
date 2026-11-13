#pragma once
#include "../kernel/types.h"

// intel gt bring-up and 2d blits on xe-lpg (meteor lake / arrow lake,
// graphics 12.70) without guc firmware: forcewake, pat and mocs, a logical
// ring context with its own ppgtt, execlist submission on the blitter engine
// (bcs0), and XY_FAST_COLOR_BLT / XY_FAST_COPY_BLT batches. the design, what
// is verified and the laptop test plan are in scratchpad/agent_gpu2d_results.md.
//
// the contract: (satoru)
//  - everything is off by default. kurono.igpu.gt=1 brings the engine up and
//    proves a no-op batch ran; kurono.igpu.blt=1 adds the fill, copy,
//    coherency, resubmit and throughput tests. nothing else uses the blitter
//    yet; a later stage may, and only after both passed.
//    kurono.igpu.gttest=1 runs the same code in qemu against the software
//    model of the command streamer in intel_gt_calc.cpp.
//  - it needs the kurono.igpu.accel=1 probe (the ggtt access) and never
//    touches a display register. gt registers are written through one
//    allowlisted helper. the 8 ggtt entries it uses come from the flip
//    driver's reserved region, are refused if anything references them, and
//    are saved and put back on off.
//  - every wait is bounded by the tsc of the cpu that waits. a job that does
//    not finish marks the gt dead for the session: the engine is stopped and
//    reset, the registers are dumped, forcewake is released, and every caller
//    falls back to the cpu path. the display is not involved at any point.
//  - lines are "[igpu-gt]" in the serial log (KURONO.LOG on bare metal). (satoru)
namespace IntelGt {

// boot entry, after IntelAccel::Boot. gt = kurono.igpu.gt=1, blt =
// kurono.igpu.blt=1 (implies gt), sim = kurono.igpu.gttest=1 (qemu stand-in,
// implies both). silent and inert when all are false. (satoru)
int Boot(bool gt, bool blt, bool sim);
// kurono.igpu.selftest=N: N random fills and copies checked against a cpu model after the
// blitter ladder (default off). (satoru)
void SetSelftest(int n, int jobops, int mode, int flush);   // flush: 0 none, 1 on overlap (default), 2 every blit (satoru)

bool Ready();        // the engine ran the no-op job (satoru)
bool BlitReady();    // the fill and copy tests passed too (satoru)
bool Simulated();    // the qemu software model stands in for the engine (satoru)
// the copy test with a cpu-cached, unflushed source passed: the engine reads
// what the cpu wrote without a clflush (pat index 3, one way coherent). (satoru)
bool CoherentRead();

// make a physically contiguous ram buffer addressable by the engine, at the
// address of its physical pages. scanout = the display reads it: uncached, so
// what the engine writes is in ram when the plane fetches it. (satoru)
bool MapBuffer(uint64_t phys, uint64_t bytes, bool scanout);

// one job at a time: Begin, any number of Add*, Submit. Submit returns the
// job's sequence number, 0 when nothing could be submitted. (satoru)
bool Busy();
bool Begin();
bool AddCopy(uint64_t dst_phys, uint32_t dst_pitch, int dx, int dy,
             uint64_t src_phys, uint32_t src_pitch, int sx, int sy, int w, int h);
bool AddFill(uint64_t dst_phys, uint32_t dst_pitch, int x, int y, int w, int h, uint32_t color);
uint32_t Submit();
// 1 = finished, 0 = still running, -1 = failed (timed out: the gt is now off). (satoru)
int Poll(uint32_t seq);
// spin until the job finishes or timeout_us passes on this cpu. (satoru)
bool Wait(uint32_t seq, uint32_t timeout_us);

void Off(const char* why);
int Status(char* out, int maxo);
int SelfTest(char* out, int maxo);   // `gpuaccel gttest`: runs the blit ladder again (satoru)
// [gui] report: jobs, kilobytes blitted and engine microseconds since the last call. (satoru)
void TakeStats(uint32_t* jobs, uint64_t* kb, uint64_t* job_us, uint32_t* fails);

}
// end (satoru)
