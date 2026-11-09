#pragma once
#include "../kernel/types.h"

// intel display stage 1: displayport aux reads on display version 14
// (meteor lake / arrow lake). it reads the sink's dpcd receiver caps, sink
// count and edid (i2c-over-aux at 0x50) on every port that already has a
// powered, owned phy and a live hotplug, and logs raw bytes plus decoded
// fields as "[igpu-aux]" lines. (satoru)
//
// it lights nothing. the only mmio it writes are the aux channel ctl/data
// registers of the port it talks to, plus, on a type-c port whose aux power
// is off, the aux power request bit (set before, cleared after, the way
// xelpdp_aux_power_well_enable/disable in linux do it). it never writes a
// pipe, plane, transcoder, ddi_buf, pll, pps, power well or tc ownership
// register, and it never sends a native aux write to the sink. (satoru)
//
// boot runs it once right after the stage 0 probe (kurono.igpu.aux=0 turns
// that off); the `gpuaux` shell command reruns it. harmless without an
// intel gpu: it logs one line and returns. (satoru)
namespace IntelDpAux {

// lines go to the serial log and are appended to out (when non-null).
// returns the number of bytes written to out. (satoru)
int Run(char* out, int maxo);

// ---- stage 2 api, used only by intel_modeset.cpp. Run itself still never
// sends a native aux write; these calls do (dpcd 0x100.., 0x600, 0xf0000..
// during link training), on type-c ports 1..4 only. (satoru)

// the type-c sink the last Run read: effective dpcd caps (0x2200 when the
// extended field was present), sink count, raw edid (0 len if none).
// false when the last Run did not get the port's dpcd. (satoru)
struct SinkInfo {
    uint8_t dpcd[16];
    uint8_t sink_count;
    int edid_len;
    const uint8_t* edid;
};
bool LastSink(int tc, SinkInfo* out);

// start a bounded aux session on a type-c port: resets the transfer count
// and sets a wall-time budget, like the per-port budget of Run. (satoru)
bool BeginSession(int tc, uint32_t budget_us, int max_xfers);
// native dpcd access, drm_dp_dpcd_access retry rules. return n or < 0. (satoru)
int DpcdRead(int tc, uint32_t addr, uint8_t* buf, int n);
int DpcdWrite(int tc, uint32_t addr, const uint8_t* buf, int n);
// set or clear the port's aux power request (xelpdp_aux_power_well_*);
// on type-c this also blocks tc-cold while set. was_on reports the bit
// before the call. false when the channel stays busy. (satoru)
bool AuxPower(int tc, bool on, bool* was_on);
const char* ErrName(int e);

}

// end (satoru)
