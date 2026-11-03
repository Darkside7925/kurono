#pragma once
//  minimal dhcp client (rfc 2131) for eth0. the kernel had none: eth0 always
//  used a hard-coded 10.0.2.15 (qemu slirp), which is useless on a real lan.
//  non-blocking state machine driven from the network process: it waits for
//  link, broadcasts discover/request, applies the lease to TCPStack + eth0,
//  and restarts whenever the link comes back (cable replug, late autoneg).
//  until a lease arrives the static defaults stay in force. (satoru)

#include "../kernel/types.h"

class Dhcp {
public:
    static void Tick();                                   // call every ~10 ms (satoru)
    // returns true if the frame was a dhcp reply for us (consumed) (satoru)
    static bool OnFrame(const uint8_t* data, uint16_t length);
    static bool IsBound();
};
// end (satoru)
