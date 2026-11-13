#pragma once
#include "../kernel/types.h"

// night light - the warm tint the present pass applies (Graphics::SetNightLight),
// with a saved strength and an optional daily schedule (local time, default
// 20:00 to 07:00). the schedule acts on its edges only: turning the tint on or
// off by hand holds until the next start or end time, as on other desktops.
// settings: display.night_light, display.night_strength (10..100, default 70),
// display.night_schedule (0/1), display.night_from / display.night_to
// (minutes after midnight). (satoru)

namespace NightLight {

void Init();                 // read the saved settings and apply them (satoru)
bool IsOn();
void Set(bool on);           // manual on/off, saved (satoru)
void Toggle();
int  Strength();
void SetStrength(int pct);   // 10..100, saved, applied live when on (satoru)
bool ScheduleOn();
void SetSchedule(bool on);   // saved; turning it on applies the current window (satoru)
int  FromMinutes();
int  ToMinutes();
// called every desktop frame; cheap, checks the clock once a few seconds. (satoru)
void Tick();

}  // namespace NightLight

// end (satoru)
