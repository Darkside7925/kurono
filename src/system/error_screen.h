// error_screen: recovery R4/R5, the driver error flow. (satoru)
//
// a driver supervisor (the KLD supervisor, a KDF watchdog, the display
// pacer) calls Handle() when a driver died, hung or misbehaved. the owner's
// tiers (recovery_PLAN.md 1.1):
//   notify: wi-fi, bluetooth, audio, touchpad, camera, extra usb devices, a
//           second monitor. a notification only; two quiet retries.
//   blue:   the only display, the system disk, the only keyboard, a device
//           that keeps faulting the iommu, a core service crash loop, or a
//           notify-tier device whose failure endangers the system.
// blue: freeze every app (ProcessFreeze), mute audio, hold the compositor's
// presents, draw the blue screen (kscreen) straight on the scanout, retry the
// driver after 1 s and 4 s with a spinner, and either resume everything or
// offer the choices: continue without it, a basic driver, a terminal,
// restart into recovery, restart. the crash record is the caller's job
// (KCrash::Report) and is written before Handle runs. (satoru)
#pragma once
#include "../kernel/types.h"

namespace ErrorScreen {

enum DevClass : uint8_t {
    DC_GENERIC = 0x00, DC_DISPLAY = 0x01, DC_STORAGE = 0x02, DC_NETWORK = 0x03,
    DC_WIFI = 0x04, DC_KEYBOARD = 0x05, DC_AUDIO = 0x06, DC_TOUCHPAD = 0x07,
    DC_BLUETOOTH = 0x08, DC_CAMERA = 0x09, DC_USB = 0x0A, DC_SERVICE = 0x0B,
};
enum Kind : uint8_t { K_CRASH = 0x01, K_HANG = 0x02, K_IOMMU = 0x03, K_INIT = 0x04 };

enum Outcome : uint8_t {
    O_NOTIFIED = 0,      // notify tier: handled with notifications (satoru)
    O_RESTARTED,         // a retry worked, everything resumed (satoru)
    O_CONTINUE,          // the person chose to go on without the device (satoru)
    O_BASIC,             // the basic driver took over (satoru)
    O_REBOOTING,         // never returns in practice (satoru)
};

struct Failure {
    const char* family;          // "iwlwifi", "i915", ... (satoru)
    DevClass    dev_class;
    Kind        kind;
    uint32_t    bdf;             // bus << 8 | dev << 3 | fn, 0 = unknown (satoru)
    uint16_t    vendor, device;
    const char* detail;          // "exit=SIGSEGV", "no heartbeat for 5 s" (satoru)
    bool        only_device;     // the only display / keyboard, or the system disk (satoru)
    bool        endangers;       // iommu storm, holds a cpu or the system lock (satoru)
    const char* basic_label;     // "Use basic display" when a fallback exists (satoru)
    // reload the driver (attempt 1 or 2). true when it works again. (satoru)
    bool (*retry)(void* ctx, int attempt);
    bool (*use_basic)(void* ctx);
    void*       ctx;
};

Outcome Handle(const Failure& f);

// true while the blue screen owns the display and the keyboard. graphics
// holds its presents and the keyboard routes chars here meanwhile. (satoru)
bool Active();
// keyboard.cpp hands every typed char here while Active(). (satoru)
void OnChar(char c);
// another full-screen owner (idle sleep): hold presents and take the keys
// the same way, then give both back. PopChar is -1 when nothing was typed. (satoru)
void BeginExclusive();
void EndExclusive();
int  PopChar();

// shell `errscreen <class> [ok|fail|ok2]`: fault injection without a real
// driver; the retry callback succeeds on the given attempt. (satoru)
int  TestCommand(int argc, const char** argv, char* out, int mx);

}  // namespace ErrorScreen

// for the two hooks outside this module (graphics, keyboard). (satoru)
extern volatile bool g_kscreen_hold;

// end (satoru)
