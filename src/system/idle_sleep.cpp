// idle_sleep: see idle_sleep.h. (satoru)
#include "idle_sleep.h"
#include "freeze.h"
#include "backlight.h"
#include "error_screen.h"
#include "../kernel/panic.h"
#include "../kernel/kscreen.h"
#include "../drivers/mouse.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../hal/hal.h"
#include "../proc/scheduler.h"

namespace IdleSleep {
namespace {
bool g_on = false;

void black() {
    uint64_t a; uint32_t pitch, w, h; uint8_t bpp;
    if (!KernelPanic::GetFramebuffer(&a, &pitch, &w, &h, &bpp) || bpp != 32) return;
    for (uint32_t y = 0; y < h; y++) {
        volatile uint32_t* row = (volatile uint32_t*)(uintptr_t)(a + (uint64_t)y * pitch);
        for (uint32_t x = 0; x < w; x++) row[x] = 0xFF000000u;
    }
}
}  // namespace

void SetEnabled(bool on) { g_on = on; }
bool Enabled() { return g_on; }

void Run() {
    SerialLogger::Log("[idlesleep] screen off, apps paused\r\n");
    ErrorScreen::BeginExclusive();
    ProcessFreeze::FreezeAll();
    bool hw = Backlight::Blank(true);
    black();
    int mx = Mouse::mx, my = Mouse::my;
    uint32_t t0 = Timer::GetRealMs();
    for (;;) {
        Scheduler::SleepMs(50);
        if (ErrorScreen::PopChar() >= 0) break;
        if (Mouse::mx != mx || Mouse::my != my) break;
        if (Timer::GetRealMs() - t0 > 1000u && HAL::AcpiPowerButtonPressed()) break;
    }
    if (hw) Backlight::Blank(false);
    ProcessFreeze::ThawAll();
    ErrorScreen::EndExclusive();
    SerialLogger::Log("[idlesleep] awake\r\n");
}

}  // namespace IdleSleep

// end (satoru)
