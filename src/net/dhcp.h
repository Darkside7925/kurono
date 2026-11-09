#pragma once
//  minimal dhcp client (rfc 2131) for eth0. the kernel had none: eth0 always
//  used a hard-coded 10.0.2.15 (qemu slirp), which is useless on a real lan.
//  non-blocking state machine driven from the network process: it waits for
//  link, broadcasts discover/request, applies the lease to TCPStack + eth0,
//  and restarts whenever the link comes back (cable replug, late autoneg).
//  until a lease arrives the static defaults stay in force. (satoru)
//
//  lease lifecycle follows rfc 2131: bound -> renewing at t1 (unicast request
//  to the server) -> rebinding at t2 (broadcast request) -> discover again when
//  the lease expires. a link flap while bound revalidates the old address
//  instead of throwing it away. (satoru)

#include "../kernel/types.h"

class Dhcp {
public:
    static void Tick();                                   // call every ~10 ms (satoru)
    // returns true if the frame was a dhcp reply for us (consumed) (satoru)
    static bool OnFrame(const uint8_t* data, uint16_t length);
    static bool IsBound();

    // status for the settings page + diagnostics (satoru)
    static const char* StateString();
    static uint32_t LeaseSeconds();
    static uint32_t LeaseRemainingSeconds();
    static uint32_t GetServer();
    static uint32_t GetPrimaryDNS();
    static uint32_t GetSecondaryDNS();
};
// end (satoru)
