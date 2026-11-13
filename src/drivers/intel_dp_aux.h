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
// kurono.igpu.tclegacy=1: also connect, read and release a type-c port whose
// only live hotplug is the legacy one (fix 6 diagnostic, writes that port's
// port_buf_ctl1 for the duration of the read). (satoru)
void SetTcLegacyProbe(bool on);

// ---- kurono.igpu.legacytc=1 (default off): type-c ports wired in legacy
// mode (the hx laptop's hdmi / usb-c dp-in paths). Run connects such a port
// the way xelpdp_tc_phy_connect does for tc_port_legacy (tc-cold block via
// the aux power request, tcss power request + ready + power ack, phy
// ownership, phy ready), reads dpcd + edid, and when a dp sink with an edid
// answers keeps the port connected for stage 2. anything else is released
// again. if aux stays silent the edid is read over the port's gmbus (ddc)
// pin to tell an hdmi sink apart. lines are "[igpu-tc]". (satoru)
void SetLegacyTc(bool on);
bool LegacyTcEnabled();
// connect tc (1..4) in legacy mode now; true when this module holds it
// connected (also when it already did). false leaves the port as found. (satoru)
bool LegacyTcConnect(int tc);
// true while this module holds tc connected (satoru)
bool LegacyTcHeld(int tc);
// undo LegacyTcConnect (xelpdp_tc_phy_disconnect for legacy): ownership off,
// tcss power off, then the tc-cold block. no-op when not held. why goes in
// the log line. (satoru)
void LegacyTcRelease(int tc, const char* why);
void LegacyTcReleaseAll(const char* why);

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
    bool legacy;            // read on a type-c port in legacy mode (no dp-alt / tbt hotplug) (satoru)
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

// the internal panel (edp, port a): native dpcd read / write of up to 16
// bytes, for the aux brightness interface (system/backlight.cpp). refused
// (-1) unless port a's aux is already powered, the panel power sequencer is
// on and psr / panel replay are off. (satoru)
int EdpDpcdRead(uint32_t addr, uint8_t* buf, int n);
int EdpDpcdWrite(uint32_t addr, const uint8_t* buf, int n);

}

// end (satoru)
