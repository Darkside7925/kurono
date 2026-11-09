//  kurono os - virtio-net host device with a nat into kurono's network stack
//
//  the guest sits alone on a private 10.0.3.0/24 link: kurono is the gateway
//  10.0.3.1 (arp, a tiny dhcp server, dns forwarding) and the guest gets
//  10.0.3.15. outgoing tcp/udp/icmp-echo is masqueraded: source rewritten to
//  kurono's own ip and a host port from a reserved range (32768-48999, below
//  the stack's ephemeral range), then sent through TCPStack. replies are caught
//  by the tcpip receive hook before local delivery and translated back. this
//  works over any link kurono has (ethernet or wifi), unlike bridging. (satoru)
#pragma once
#include <stdint.h>

namespace VNet {
    bool Init(uint8_t irq);              // vm creation (vm thread)
    void Shutdown();                     // vm teardown: drop the nat table
    void Poll();                         // vm thread: deliver queued frames to the guest
    bool HasPending();                   // frames waiting for the guest
    int  Status(char* out, int max);     // human readable counters

    uint32_t GuestIP();                  // host order
    uint32_t GatewayIP();
}

// end (satoru)
