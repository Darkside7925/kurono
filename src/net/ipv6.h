#ifndef KURONO_NET_IPV6_H
#define KURONO_NET_IPV6_H

#include <stdint.h>

// Minimal IPv6 + ICMPv6 + Neighbor Discovery support.
//
// We implement the wire format, checksum, parsing of incoming IPv6
// packets, and replies for the two ICMPv6 types every host MUST handle:
//
//   - 128 Echo Request          → reply with 129 Echo Reply (ping6)
//   - 135 Neighbor Solicitation → reply with 136 Neighbor Advertisement
//
// Plus a fully-formed IPv6 link-local address derivation from MAC (the
// modified EUI-64 method used on every Linux box).  Higher layers (UDP6,
// TCP6, DHCPv6, RA processing) build on top of these primitives.

#define ETH_TYPE_IPV6     0x86DD
#define IPPROTO_ICMPV6    58
#define IPPROTO_TCP6      6
#define IPPROTO_UDP6      17
#define IPPROTO_FRAGMENT  44

#define ICMP6_ECHO_REQUEST            128
#define ICMP6_ECHO_REPLY              129
#define ICMP6_ROUTER_SOLICITATION     133
#define ICMP6_ROUTER_ADVERTISEMENT    134
#define ICMP6_NEIGHBOR_SOLICITATION   135
#define ICMP6_NEIGHBOR_ADVERTISEMENT  136

#pragma pack(push, 1)
struct IPv6Address {
    uint8_t b[16];
};

struct IPv6Header {
    uint32_t ver_tc_fl;     // version(4)|traffic_class(8)|flow_label(20), big-endian
    uint16_t payload_len;   // bytes after this 40-byte header
    uint8_t  next_header;   // upper-layer proto
    uint8_t  hop_limit;
    IPv6Address src;
    IPv6Address dst;
};

struct ICMPv6Header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    // body follows, format depends on type
};

struct ICMPv6Echo {
    ICMPv6Header hdr;
    uint16_t id;
    uint16_t seq;
    // payload follows
};

struct ICMPv6NS {              // Neighbor Solicitation (135)
    ICMPv6Header hdr;
    uint32_t reserved;
    IPv6Address target;
    // option: source link-layer address (TLV: type=1, len=1 (×8B), 6 MAC bytes)
};

struct ICMPv6NA {              // Neighbor Advertisement (136)
    ICMPv6Header hdr;
    uint32_t flags;            // R(31) S(30) O(29) followed by 29 reserved bits
    IPv6Address target;
    // option: target link-layer address (TLV: type=2, len=1, 6 MAC bytes)
};
#pragma pack(pop)

namespace IPv6 {

    // one-time init: build link-local address(es) for each NIC.
    void Init();

    // Parse an inbound ethernet frame whose ethertype was 0x86DD.  The
    // pointer is to the start of the IPv6 header (14 bytes after the
    // ethernet header).  If we recognise + handle the packet (echo,
    // neighbor solicit, etc.) we may craft and queue a reply.
    void ProcessRx(const unsigned char* eth_frame,
                   int eth_len,
                   const unsigned char* dst_mac,
                   const unsigned char* src_mac);

    // ICMPv6 checksum spans the IPv6 pseudo-header + the ICMPv6 message.
    uint16_t Checksum(const IPv6Header* hdr, const void* icmp, int icmp_len);

    // Build the standard EUI-64 link-local address fe80::xxxx:xxff:fexx:xxxx
    // from a 48-bit MAC and write into out.
    void MacToLinkLocal(const unsigned char mac[6], IPv6Address* out);

    // Compare two addresses for equality.
    bool Equals(const IPv6Address& a, const IPv6Address& b);

    // Construct the solicited-node multicast address ff02::1:ff00:0/104
    // for a target unicast address.
    void SolicitedNodeMulticast(const IPv6Address& tgt, IPv6Address* out);

    // Send an ICMPv6 echo reply built from a captured echo request.
    bool SendEchoReply(const IPv6Header* req_ip,
                       const ICMPv6Echo* req_icmp,
                       int payload_len,
                       const unsigned char src_mac[6],
                       const unsigned char dst_mac[6]);

    // Send a neighbor advertisement in response to a solicitation.
    bool SendNeighborAdvert(const IPv6Address& target_ip,
                            const IPv6Address& dst_ip,
                            const unsigned char src_mac[6],
                            const unsigned char dst_mac[6]);
}

#endif
