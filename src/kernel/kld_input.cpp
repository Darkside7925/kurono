// KldInput: the kernel half of the touchpad pointer bridge.
//
// the container has already applied the touchpad semantics (one finger move,
// tap click, two-finger scroll, palm ignore). this file only turns the result
// into a boot-protocol mouse report and calls the public USB-HID injection
// path, so the desktop sees exactly the same Mouse::Event/RingPush stream as a
// real PS/2 or USB mouse. (satoru)

#include "kld_input.h"
#include "../drivers/mouse.h"
#include "../drivers/serial.h"
#include "../proc/scheduler.h"

namespace KldInput {

static bool g_verbose;
static uint32_t g_events, g_sum_dx, g_sum_dy;
static uint64_t g_sec;

void SetVerbose(bool on) { g_verbose = on; }

static int clamp8(int v)
{
    if (v < -127) return -127;
    if (v > 127) return 127;
    return v;
}

void Pointer(int dx, int dy, uint32_t buttons, int wheel)
{
    // untrusted producer: at most 1000 events/s (one per ms), no per-event
    // serial line. excess is dropped and counted once per second. (satoru)
    uint64_t now = Scheduler::NowMs() / 1000;   // the window is one SECOND (satoru)
    static uint64_t win_ms;
    static uint32_t win_n, win_drop;
    if (now != win_ms) {
        if (win_ms && (win_n || win_drop)) {
            SerialLogger::Log("[kld-input] summary events=");
            SerialLogger::LogDec((int)win_n);
            SerialLogger::Log(" dropped=");
            SerialLogger::LogDec((int)win_drop);
            SerialLogger::Log("\r\n");
        }
        win_ms = now;
        win_n = 0;
        win_drop = 0;
    }
    if (++win_n > 1000) {
        win_drop++;
        return;
    }
    static bool announced;
    if (!announced) {
        announced = true;
        // one line at most, never per event: proves the bridge reached the
        // kernel without letting an untrusted producer flood the log. (satoru)
        SerialLogger::Log("[kld-input] pointer bridge active\r\n");
    }
    uint8_t rep[4];
    rep[0] = (uint8_t)(buttons & 0x1f);   // left, right, middle, x1, x2
    rep[1] = (uint8_t)(int8_t)clamp8(dx);
    rep[2] = (uint8_t)(int8_t)clamp8(dy); // positive down
    rep[3] = (uint8_t)(int8_t)(wheel < -1 ? -1 : wheel > 1 ? 1 : wheel);
    g_events++;
    g_sum_dx += dx;
    g_sum_dy += dy;
    Mouse::ProcessInjectedReport(rep, 4);
}

}  // namespace KldInput
// end (satoru)
