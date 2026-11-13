#include "night_light.h"
#include "../drivers/graphics.h"
#include "../drivers/timer.h"
#include "../drivers/serial.h"
#include "../kernel/time.h"
#include "../system/ui_config.h"

namespace NightLight {

namespace {
bool g_on = false;
int  g_strength = 70;
bool g_sched = false;
int  g_from = 20 * 60;
int  g_to = 7 * 60;
int  g_last_in = -1;          // schedule window state seen by the last tick (satoru)
uint32_t g_last_check = 0;
bool g_inited = false;

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void apply() { Graphics::SetNightLight(g_on ? g_strength : 0); }

bool in_window() {
    DateTime dt = TimeManager::NowLocalDateTime();
    int m = (int)dt.h * 60 + (int)dt.m;
    if (g_from == g_to) return false;
    if (g_from < g_to) return m >= g_from && m < g_to;
    return m >= g_from || m < g_to;   // across midnight (satoru)
}

void save_on() {
    UIConfig::SetInt("display.night_light", g_on ? 1 : 0, true);
    UIConfig::Save();
}
}  // namespace

void Init() {
    g_inited = true;
    g_on = UIConfig::Int("display.night_light", 0) != 0;
    g_strength = clampi(UIConfig::Int("display.night_strength", 70), 10, 100);
    g_sched = UIConfig::Int("display.night_schedule", 0) != 0;
    g_from = clampi(UIConfig::Int("display.night_from", 20 * 60), 0, 24 * 60 - 1);
    g_to = clampi(UIConfig::Int("display.night_to", 7 * 60), 0, 24 * 60 - 1);
    g_last_in = -1;
    if (g_sched) {
        g_last_in = in_window() ? 1 : 0;
        g_on = g_last_in == 1;
    }
    apply();
}

bool IsOn() { return g_on; }

void Set(bool on) {
    if (!g_inited) Init();
    g_on = on;
    apply();
    save_on();
}

void Toggle() { Set(!g_on); }

int Strength() { return g_strength; }

void SetStrength(int pct) {
    g_strength = clampi(pct, 10, 100);
    UIConfig::SetInt("display.night_strength", g_strength, true);
    UIConfig::Save();
    apply();
}

bool ScheduleOn() { return g_sched; }

void SetSchedule(bool on) {
    g_sched = on;
    UIConfig::SetInt("display.night_schedule", on ? 1 : 0, true);
    g_last_in = -1;
    if (on) {
        g_last_in = in_window() ? 1 : 0;
        g_on = g_last_in == 1;
        apply();
        UIConfig::SetInt("display.night_light", g_on ? 1 : 0, true);
    }
    UIConfig::Save();
}

int FromMinutes() { return g_from; }
int ToMinutes() { return g_to; }

void Tick() {
    if (!g_inited || !g_sched) return;
    uint32_t now = Timer::GetRealMs();
    if (g_last_check && (uint32_t)(now - g_last_check) < 5000u) return;
    g_last_check = now;
    int in = in_window() ? 1 : 0;
    if (in != g_last_in) {
        g_last_in = in;
        if (g_on != (in == 1)) {
            g_on = in == 1;
            apply();
            save_on();
            SerialLogger::Log(g_on ? "[nightlight] schedule: on\r\n" : "[nightlight] schedule: off\r\n");
        }
    }
    // an idle desktop may sleep its loop; come back for the next check. (satoru)
    Graphics::RequestFrameAt(now + 30000u);
}

}  // namespace NightLight

// end (satoru)
