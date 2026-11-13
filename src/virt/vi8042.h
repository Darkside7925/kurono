//  kurono os - minimal virtual i8042 (ps/2 keyboard controller) (satoru)
//
//  enough of an 8042 for a guest that probes one (a Kurono guest does): a
//  config byte, the self tests, keyboard commands answered with ACK, no aux
//  device (aux bytes are answered with RESEND so a mouse probe fails fast
//  instead of spinning on 0xFF). the host can type into the guest: queued
//  set-1 scancodes come out of port 0x60 with IRQ1. without this, every
//  unhandled read of 0x64 returned 0xFF ("output full, aux") and the guest's
//  mouse driver logged "Expected ACK, got 0xFF" a few hundred thousand
//  times. (satoru)
#pragma once
#include "../kernel/types.h"

namespace VI8042 {
    void Reset();
    // ports 0x60 / 0x64; false for any other port (satoru)
    bool HandlePort(uint16_t port, bool is_out, uint32_t& value);
    // vm thread: raise IRQ1 while keyboard bytes wait (call from the device tick) (satoru)
    void Tick();
    // any thread: queue set-1 scancodes / type ascii text (satoru)
    void InjectScancode(uint8_t sc);
    int  InjectText(const char* s);           // returns characters queued (satoru)
    bool InjectKeyName(const char* name);     // enter, esc, tab, backspace, up, down, left, right (satoru)
}

// end (satoru)
