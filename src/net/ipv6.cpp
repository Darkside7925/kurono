#include "ipv6.h"
#include "tcpip.h"
#include "network.h"
#include "../drivers/serial.h"

namespace {
    inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
    inline uint32_t bswap32(uint32_t v) {
        return ((v & 0x000000FFu) << 24) |
               ((v & 0x0000FF00u) << 8)  |
               ((v & 0x00FF0000u) >> 8)  |
               ((v & 0xFF000000u) >> 24);
    }

    inline void mzero(void* p, unsigned n) {
        unsigned char* b = (unsigned char*)p;
        for (unsigned i = 0; i < n; i++) b[i] = 0;
    }
    inline void mcopy(void* d, const void* s, unsigned n) {
        unsigned char* dd = (unsigned char*)d;
        const unsigned char* ss = (const unsigned char*)s;
        for (unsigned i = 0; i < n; i++) dd[i] = ss[i];
    }
}

namespace IPv6 {

bool Equals(const IPv6Address& a, const IPv6Address& b) {
    for (int i = 0; i < 16; i++) if (a.b[i] != b.b[i]) return false;
    return true;
}

void MacToLinkLocal(const unsigned char mac[6], IPv6Address* out) {
    // fe80::/64 + modified EUI-64
    mzero(out, sizeof(*out));
    out->b[0] = 0xFE; out->b[1] = 0x80;
    // bytes 2..7 zero, bytes 8..15 = EUI-64
    out->b[8]  = mac[0] ^ 0x02;     // flip universal/local bit
    out->b[9]  = mac[1];
    out->b[10] = mac[2];
    out->b[11] = 0xFF;
    out->b[12] = 0xFE;
    out->b[13] = mac[3];
    out->b[14] = mac[4];
    out->b[15] = mac[5];
}

void SolicitedNodeMulticast(const IPv6Address& tgt, IPv6Address* out) {
    // ff02::1:ffXX:XXXX where XX:XXXX is the low 24 bits of target
    mzero(out, sizeof(*out));
    out->b[0]  = 0xFF; out->b[1]  = 0x02;
    out->b[11] = 0x01;
    out->b[12] = 0xFF;
    out->b[13] = tgt.b[13];
    out->b[14] = tgt.b[14];
    out->b[15] = tgt.b[15];
}

uint16_t Checksum(const IPv6Header* hdr, const void* icmp, int icmp_len) {
    // Standard RFC 2460 pseudo-header sum:
    //   src(16) + dst(16) + upper_layer_len(4) + zero(3) + next_header(1)
    // then the message itself (zero-padded to even).
    uint32_t sum = 0;

    // src + dst
    const uint16_t* p = (const uint16_t*)&hdr->src;
    for (int i = 0; i < 16; i++) sum += bswap16(p[i]);

    // upper-layer length (32-bit)
    uint32_t ulen = (uint32_t)icmp_len;
    sum += (ulen >> 16) & 0xFFFF;
    sum += (ulen) & 0xFFFF;

    // zeros (3 bytes) + next header (1 byte)
    sum += (uint16_t)hdr->next_header;

    // ICMPv6 message
    const uint16_t* m = (const uint16_t*)icmp;
    int words = icmp_len / 2;
    for (int i = 0; i < words; i++) sum += bswap16(m[i]);
    if (icmp_len & 1) {
        sum += ((uint16_t)((const unsigned char*)icmp)[icmp_len - 1]) << 8;
    }

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

bool SendEchoReply(const IPv6Header* req_ip,
                   const ICMPv6Echo* req_icmp,
                   int payload_len,
                   const unsigned char src_mac[6],
                   const unsigned char dst_mac[6])
{
    // payload_len is the body after the 8-byte echo header.
    int icmp_total = (int)sizeof(ICMPv6Echo) + payload_len;
    int total = (int)sizeof(EthernetHeader) + (int)sizeof(IPv6Header) + icmp_total;
    if (total > 1518) return false;

    unsigned char frame[1518];
    mzero(frame, total);

    // Ethernet header
    EthernetHeader* eth = (EthernetHeader*)frame;
    mcopy(eth->dst_mac, dst_mac, 6);
    mcopy(eth->src_mac, src_mac, 6);
    eth->ethertype = bswap16(ETH_TYPE_IPV6);

    // IPv6 header
    IPv6Header* ip = (IPv6Header*)(frame + sizeof(EthernetHeader));
    ip->ver_tc_fl   = bswap32(0x60000000u);   // version 6
    ip->payload_len = bswap16((uint16_t)icmp_total);
    ip->next_header = IPPROTO_ICMPV6;
    ip->hop_limit   = 64;
    // swap src/dst from request
    mcopy(&ip->src, &req_ip->dst, 16);
    mcopy(&ip->dst, &req_ip->src, 16);

    // ICMPv6 echo reply
    ICMPv6Echo* echo = (ICMPv6Echo*)((unsigned char*)ip + sizeof(IPv6Header));
    echo->hdr.type     = ICMP6_ECHO_REPLY;
    echo->hdr.code     = 0;
    echo->hdr.checksum = 0;
    echo->id  = req_icmp->id;
    echo->seq = req_icmp->seq;
    if (payload_len > 0) {
        mcopy((unsigned char*)echo + sizeof(ICMPv6Echo),
              (const unsigned char*)req_icmp + sizeof(ICMPv6Echo),
              (unsigned)payload_len);
    }
    echo->hdr.checksum = bswap16(Checksum(ip, echo, icmp_total));

    return Network::SendPacket("eth0", frame, total);
}

bool SendNeighborAdvert(const IPv6Address& target_ip,
                        const IPv6Address& dst_ip,
                        const unsigned char src_mac[6],
                        const unsigned char dst_mac[6])
{
    int icmp_total = (int)sizeof(ICMPv6NA) + 2 + 6;     // + TLV opt
    int total = (int)sizeof(EthernetHeader) + (int)sizeof(IPv6Header) + icmp_total;

    unsigned char frame[1518];
    mzero(frame, total);

    EthernetHeader* eth = (EthernetHeader*)frame;
    mcopy(eth->dst_mac, dst_mac, 6);
    mcopy(eth->src_mac, src_mac, 6);
    eth->ethertype = bswap16(ETH_TYPE_IPV6);

    IPv6Header* ip = (IPv6Header*)(frame + sizeof(EthernetHeader));
    ip->ver_tc_fl   = bswap32(0x60000000u);
    ip->payload_len = bswap16((uint16_t)icmp_total);
    ip->next_header = IPPROTO_ICMPV6;
    ip->hop_limit   = 255;                  // RFC 4861 requires 255
    mcopy(&ip->src, &target_ip, 16);
    mcopy(&ip->dst, &dst_ip, 16);

    ICMPv6NA* na = (ICMPv6NA*)((unsigned char*)ip + sizeof(IPv6Header));
    na->hdr.type     = ICMP6_NEIGHBOR_ADVERTISEMENT;
    na->hdr.code     = 0;
    na->hdr.checksum = 0;
    // R=0, S=1 (solicited), O=1 (override) → top 3 bits set in big-endian
    na->flags = bswap32(0x60000000u);
    mcopy(&na->target, &target_ip, 16);

    // Option: target link-layer address (type=2, len=1 ×8B, then 6B MAC)
    unsigned char* opt = (unsigned char*)na + sizeof(ICMPv6NA);
    opt[0] = 2;
    opt[1] = 1;
    mcopy(opt + 2, src_mac, 6);

    na->hdr.checksum = bswap16(Checksum(ip, na, icmp_total));

    return Network::SendPacket("eth0", frame, total);
}

void ProcessRx(const unsigned char* eth_frame,
               int eth_len,
               const unsigned char* dst_mac_unused,
               const unsigned char* src_mac)
{
    (void)dst_mac_unused;
    if (eth_len < (int)(sizeof(EthernetHeader) + sizeof(IPv6Header))) return;

    const EthernetHeader* eth = (const EthernetHeader*)eth_frame;
    const IPv6Header* ip = (const IPv6Header*)(eth_frame + sizeof(EthernetHeader));

    // Sanity: version field must be 6.
    uint32_t vtcfl = bswap32(ip->ver_tc_fl);
    if ((vtcfl >> 28) != 6) return;

    int payload_len = (int)bswap16(ip->payload_len);
    int avail = eth_len - (int)sizeof(EthernetHeader) - (int)sizeof(IPv6Header);
    if (payload_len > avail) payload_len = avail;
    if (payload_len < (int)sizeof(ICMPv6Header)) return;

    if (ip->next_header != IPPROTO_ICMPV6) return;

    const ICMPv6Header* icmp = (const ICMPv6Header*)
        (eth_frame + sizeof(EthernetHeader) + sizeof(IPv6Header));

    NetworkInterface* nif = Network::GetInterface("eth0");
    if (!nif) return;

    // Reuse our own MAC as source on replies.
    unsigned char our_mac[6];
    for (int i = 0; i < 6; i++) our_mac[i] = nif->mac.bytes[i];

    // Reply unicast back to whoever sent it.
    unsigned char reply_dst_mac[6];
    for (int i = 0; i < 6; i++) reply_dst_mac[i] = src_mac ? src_mac[i] : eth->src_mac[i];

    switch (icmp->type) {
    case ICMP6_ECHO_REQUEST: {
        if (payload_len < (int)sizeof(ICMPv6Echo)) return;
        const ICMPv6Echo* echo = (const ICMPv6Echo*)icmp;
        int body = payload_len - (int)sizeof(ICMPv6Echo);
        SendEchoReply(ip, echo, body, our_mac, reply_dst_mac);
        SerialLogger::Log("IPv6: replied to echo request\r\n");
        break;
    }
    case ICMP6_NEIGHBOR_SOLICITATION: {
        if (payload_len < (int)sizeof(ICMPv6NS)) return;
        const ICMPv6NS* ns = (const ICMPv6NS*)icmp;
        SendNeighborAdvert(ns->target, ip->src, our_mac, reply_dst_mac);
        SerialLogger::Log("IPv6: replied to neighbor solicit\r\n");
        break;
    }
    default:
        // unhandled ICMPv6 type  -  silently drop
        break;
    }
}

void Init() {
    SerialLogger::Log("IPv6: stack initialized (echo + ND)\r\n");
}

}  // namespace IPv6
