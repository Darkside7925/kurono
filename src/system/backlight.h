// backlight: the internal panel's brightness (4a). (satoru)
//
// two pwm register layouts are known:
//   - the legacy / pch pair (BLC_PWM_CTL2/CTL, BLC_PWM_PCH_CTL1/CTL2), which
//     IntelGPU::SetBacklightPercent already drives (pre-existing, default on);
//   - the per-controller layout every display since broxton / cannon lake
//     uses, Arrow Lake included: BXT_BLC_PWM_CTL(c) 0xC8250 + c*0x100 (bit 31
//     enable), BXT_BLC_PWM_FREQ(c) 0xC8254, BXT_BLC_PWM_DUTY(c) 0xC8258
//     (linux i915 intel_backlight.c, bxt/cnp setup). Probe() only reads and
//     logs them at boot; writing the duty register needs kurono.backlight=1
//     until it is proven on the laptop. only the duty register is ever
//     written, never the frequency, enable or polarity.
// panels driven through eDP AUX (the vesa dpcd interface: 0x701 / 0x702 caps,
// 0x721 mode, 0x722/0x723 brightness) are handled when no pwm controller is
// lit: Probe() reads and logs the caps, Set() / Blank() write them only with
// kurono.backlight=1 (fix 7, oled panels). (satoru)
#pragma once
#include "../kernel/types.h"

namespace Backlight {
void Probe();                 // read only: logs what was found (satoru)
void SetWritesAllowed(bool on);   // kurono.backlight=1 (satoru)
bool Available();
int  Get();                   // 0..100, -1 unknown (satoru)
bool Set(int pct);
void Step(int dir);           // the brightness keys: 6 % steps (satoru)
// idle sleep: the panel dark (duty 0) and back to the level before. false
// when no backlight is driven (the screen is then only painted black). (satoru)
bool Blank(bool off);
}

// end (satoru)
