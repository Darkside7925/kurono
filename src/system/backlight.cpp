// backlight: see backlight.h. (satoru)
#include "backlight.h"
#include "ui_config.h"
#include "../drivers/intel_gpu.h"
#include "../drivers/serial.h"
#include "../drivers/graphics.h"
#include "../drivers/intel_dp_aux.h"   // edp aux brightness (fix 7) (satoru)

namespace Backlight {
namespace {
bool g_writes = false;
int  g_ctrl = -2;     // -2 not probed, -1 none, else the lit bxt controller (satoru)
int  g_pct = -1;

uint32_t ctl(int c)  { return 0xC8250u + (uint32_t)c * 0x100u; }
uint32_t freq(int c) { return 0xC8254u + (uint32_t)c * 0x100u; }
uint32_t duty(int c) { return 0xC8258u + (uint32_t)c * 0x100u; }

void find() {
    if (g_ctrl != -2) return;
    g_ctrl = -1;
    if (!IntelGPU::IsDetected()) return;
    for (int c = 0; c < 2; c++) {
        uint32_t cv = IntelGPU::ReadReg(ctl(c)), fv = IntelGPU::ReadReg(freq(c));
        if (cv == 0xFFFFFFFFu || fv == 0xFFFFFFFFu) continue;
        if ((cv & 0x80000000u) && fv > 0 && fv < 0x7FFFFFFFu) { g_ctrl = c; return; }
    }
}

void loghex(const char* n, uint32_t v) { SerialLogger::Log(n); SerialLogger::LogHex(v); }

// fix 7: an oled panel has no pwm backlight (every pwm register reads 0 on the
// laptop's SDC ATNA60HU01); its brightness is set over eDP aux, the vesa dpcd
// interface linux drives in drm_edp_backlight_*: 0x701 bit 0 tcon adjustable,
// 0x702 bit 1 brightness through aux (bit 2: 16-bit value), 0x721 mode 2 =
// dpcd, 0x722/0x723 brightness msb/lsb, 0x720 bit 0 backlight enable when
// 0x701 bit 2 says aux enable. reads only at probe; writes need
// kurono.backlight=1. (satoru)
int      g_aux = -2;        // -2 not probed, -1 none, 1 vesa aux brightness (satoru)
uint8_t  g_edp[4];          // dpcd 0x700..0x703 (satoru)
uint32_t g_aux_max = 0xFF;
bool     g_aux_mode = false;   // 0x721 set to dpcd by us (satoru)
bool     g_edp_ok = false;     // the 0x700 read answered (satoru)

void aux_probe() {
    if (!g_writes) return;   // no dpcd access at all without kurono.backlight=1 (satoru)
    if (g_aux != -2) return;
    g_aux = -1;
    if (!IntelGPU::IsDetected()) return;
    if (IntelDpAux::EdpDpcdRead(0x700, g_edp, 4) != 4) return;
    g_edp_ok = true;
    if (!(g_edp[1] & 0x01) || !(g_edp[2] & 0x02)) return;
    g_aux_max = (g_edp[2] & 0x04) ? 0xFFFFu : 0xFFu;
    g_aux = 1;
}

int aux_get() {
    aux_probe();
    if (g_aux < 0) return -1;
    uint8_t b[2] = { 0, 0 };
    int n = g_aux_max > 0xFF ? 2 : 1;
    if (IntelDpAux::EdpDpcdRead(0x722, b, n) != n) return -1;
    uint32_t v = n == 2 ? ((uint32_t)b[0] << 8 | b[1]) : b[0];
    uint64_t pct = ((uint64_t)v * 100u + g_aux_max / 2) / g_aux_max;
    return pct > 100 ? 100 : (int)pct;
}

bool aux_set(int pct) {
    aux_probe();
    if (g_aux < 0 || !g_writes) return false;
    if (!g_aux_mode) {
        uint8_t m = 0;
        if (IntelDpAux::EdpDpcdRead(0x721, &m, 1) != 1) return false;
        m = (uint8_t)((m & ~0x03u) | 0x02u);
        if (IntelDpAux::EdpDpcdWrite(0x721, &m, 1) != 1) return false;
        if (g_edp[1] & 0x04) {
            uint8_t c = 0;
            if (IntelDpAux::EdpDpcdRead(0x720, &c, 1) == 1 && !(c & 1)) {
                c |= 1;
                IntelDpAux::EdpDpcdWrite(0x720, &c, 1);
            }
        }
        g_aux_mode = true;
        SerialLogger::Log("[backlight] edp aux brightness: dpcd mode set\r\n");
    }
    uint32_t v = (uint32_t)(((uint64_t)g_aux_max * (uint32_t)pct) / 100u);
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    if (g_aux_max <= 0xFF) { b[0] = (uint8_t)v; return IntelDpAux::EdpDpcdWrite(0x722, b, 1) == 1; }
    return IntelDpAux::EdpDpcdWrite(0x722, b, 2) == 2;
}
}  // namespace

void Probe() {
    if (!IntelGPU::IsDetected()) { SerialLogger::Log("[backlight] no intel display\r\n"); return; }
    SerialLogger::Log("[backlight] probe (read only):");
    for (int c = 0; c < 2; c++) {
        loghex(c ? " ctl1=" : " ctl0=", IntelGPU::ReadReg(ctl(c)));
        loghex(" freq=", IntelGPU::ReadReg(freq(c)));
        loghex(" duty=", IntelGPU::ReadReg(duty(c)));
    }
    loghex(" legacy_ctl=", IntelGPU::ReadReg(0x48250));
    loghex(" legacy_duty=", IntelGPU::ReadReg(0x48254));
    loghex(" pch_ctl1=", IntelGPU::ReadReg(0xC8250));
    SerialLogger::Log("\r\n");
    find();
    SerialLogger::Log(g_ctrl >= 0 ? "[backlight] bxt pwm controller lit\r\n" : "[backlight] no lit bxt pwm controller\r\n");
    if (g_ctrl >= 0) return;
    if (!g_writes) { SerialLogger::Log("[backlight] edp aux brightness not probed (kurono.backlight=1)\r\n"); return; }
    // the edp aux interface: dpcd reads only. (satoru)
    uint8_t ctl[4] = { 0, 0, 0, 0 }, hdr[4] = { 0, 0, 0, 0 };
    aux_probe();
    if (!g_edp_ok) {
        SerialLogger::Log("[backlight] edp aux: no dpcd (port a aux off, psr on, or no edp)\r\n");
        return;
    }
    int rc = IntelDpAux::EdpDpcdRead(0x720, ctl, 4);
    int rh = IntelDpAux::EdpDpcdRead(0x340, hdr, 4);   // intel hdr backlight caps, logged only (satoru)
    SerialLogger::Log("[backlight] edp dpcd 0x700:");
    for (int i = 0; i < 4; i++) loghex(" ", g_edp[i]);
    SerialLogger::Log(" 0x720:");
    if (rc == 4) for (int i = 0; i < 4; i++) loghex(" ", ctl[i]); else SerialLogger::Log(" (read failed)");
    SerialLogger::Log(" 0x340:");
    if (rh == 4) for (int i = 0; i < 4; i++) loghex(" ", hdr[i]); else SerialLogger::Log(" (read failed)");
    SerialLogger::Log("\r\n");
    if (g_aux > 0) {
        SerialLogger::Log("[backlight] edp aux brightness capable, ");
        SerialLogger::Log(g_aux_max > 0xFF ? "16-bit" : "8-bit");
        SerialLogger::Log(", writes on (kurono.backlight=1)\r\n");
    } else {
        SerialLogger::Log("[backlight] edp aux brightness not offered by the panel (0x701 bit 0 / 0x702 bit 1)\r\n");
    }
}

void SetWritesAllowed(bool on) { g_writes = on; }

bool Available() {
    find();
    if (IntelGPU::GetBacklightPercent() >= 0 || g_ctrl >= 0) return true;
    if (!g_writes) return false;
    aux_probe();
    return g_aux > 0;   // (satoru)
}

int Get() {
    int p = IntelGPU::GetBacklightPercent();
    if (p >= 0) return p;
    find();
    if (g_ctrl < 0) {
        int a = (g_writes && g_aux_mode) ? aux_get() : -1;   // our own level once we drive it (satoru)
        return a >= 0 ? a : g_pct;
    }
    uint32_t f = IntelGPU::ReadReg(freq(g_ctrl)), d = IntelGPU::ReadReg(duty(g_ctrl));
    if (!f) return -1;
    uint64_t v = (uint64_t)d * 100u / f;
    return v > 100 ? 100 : (int)v;
}

bool Set(int pct) {
    if (pct < 5) pct = 5;          // never fully dark from a slider or key (satoru)
    if (pct > 100) pct = 100;
    g_pct = pct;
    if (IntelGPU::IsDetected() && IntelGPU::SetBacklightPercent(pct)) return true;   // legacy pair (satoru)
    find();
    if (g_ctrl < 0) return aux_set(pct);   // edp aux brightness, kurono.backlight=1 (satoru)
    if (!g_writes) return false;
    uint32_t f = IntelGPU::ReadReg(freq(g_ctrl));
    if (!f || f >= 0x7FFFFFFFu) return false;
    IntelGPU::WriteReg(duty(g_ctrl), (uint32_t)(((uint64_t)f * (uint32_t)pct) / 100u));
    return true;
}

bool Blank(bool off) {
    static int saved = -1;
    if (off) {
        saved = Get();
        if (IntelGPU::IsDetected() && IntelGPU::SetBacklightPercent(0)) return true;
        find();
        if (g_ctrl < 0) {
            // restore target: our own aux level, else the saved setting, never
            // a near-black value that Blank(false) would bring back. (satoru)
            if (saved < 5) saved = UIConfig::Int("display.brightness", 100);
            if (saved < 5) saved = 100;
            bool ok = aux_set(0);
            // a failed write after the mode switch may have left it dark: put
            // the level back, since the caller will not call Blank(false). (satoru)
            if (!ok && g_aux_mode) aux_set(saved);
            return ok;
        }
        if (!g_writes) return false;
        IntelGPU::WriteReg(duty(g_ctrl), 0);
        return true;
    }
    if (saved < 0) return false;
    bool ok = Set(saved);
    saved = -1;
    return ok;
}

void Step(int dir) {
    int cur = Get();
    if (cur < 0) cur = UIConfig::Int("display.brightness", 100);
    int next = cur + dir * 6;
    if (next < 5) next = 5;
    if (next > 100) next = 100;
    bool hw = Set(next);
    // without a driven backlight the compositor's own dim is the fallback. (satoru)
    if (!hw) Graphics::SetOutputBrightness(0, next < 10 ? 10 : next);
    UIConfig::SetInt("display.brightness", next, true);
    Graphics::MarkUIDirty();
}

}  // namespace Backlight

// end (satoru)
