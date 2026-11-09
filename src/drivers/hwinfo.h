#pragma once
#include "../kernel/types.h"

// hardware inventory + input heartbeat for bare-metal bring-up. (satoru)
//
// DumpAll() writes one complete picture of the machine into the kernel log
// (and so into KURONO.LOG): cpu, every pci function with class and bars, the
// ps/2 controller, usb controllers + devices, display, block devices, network.
// Heartbeat() is called every few seconds by the disklog worker: uptime plus
// keyboard/mouse event counters, so a log can tell "frozen" from "running but
// the input device is dead". (satoru)
namespace HwInfo {
    void NoteScancode(uint8_t sc);     // ps/2 keyboard byte (satoru)
    void NoteUsbKeyboard();
    void NoteMouseEvent();             // any mouse event reaching the ring (satoru)
    void NoteUsbMouse();
    void NotePoll();                   // desktop input loop polled the keyboard (satoru)
    void DumpAll();
    // read-only dump of the Intel display engine (pipes, planes, transcoder DDI
    // function, DDI buffers, power wells) for 8086:7D67. maps BAR0 uncached through
    // kdf and only ever READS; nothing is written. grep "[idisp]". (satoru)
    void DumpIntelDisplay();
    void Heartbeat();
    // one-shot log of every keyboard candidate: i8042 state + what the ps/2 keyboard
    // answered at init, usb keyboards bound, interrupt transfers completed per device.
    // `sample` is 1 or 2 (a second look later in the boot). (satoru)
    void KbDiag(int sample);
}

// end (satoru)
