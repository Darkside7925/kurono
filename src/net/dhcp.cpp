#include "dhcp.h"
#include "network.h"
#include "tcpip.h"
#include "../drivers/e1000.h"
#include "../drivers/serial.h"
#include "../proc/scheduler.h"

enum DhcpState : uint8_t { DS_IDLE = 0, DS_DISCOVER, DS_REQUEST, DS_BOUND };

static DhcpState g_state = DS_IDLE;
static uint32_t  g_xid = 0;
static uint64_t  g_t_last = 0;
static uint32_t  g_delay_ms = 2000;
static uint8_t   g_tries = 0;
static uint64_t  g_t_renew = 0;
static bool      g_was_down = true;
static uint8_t   g_mac[6];

// parsed reply, filled by OnFrame (rx path) and consumed by Tick (satoru)
static volatile bool g_have_offer = false, g_have_ack = false, g_have_nak = false;
static uint32_t g_offer_ip, g_offer_server;     // host order a<<24.. (satoru)
static uint32_t g_mask, g_router, g_dns, g_lease_s;

static void Log(const char* s) { SerialLogger::Log(s); }
static void LogIp(uint32_t ip) {
    SerialLogger::LogDec((int)(ip >> 24)); Log(".");
    SerialLogger::LogDec((int)((ip >> 16) & 255)); Log(".");
    SerialLogger::LogDec((int)((ip >> 8) & 255)); Log(".");
    SerialLogger::LogDec((int)(ip & 255));
}
static uint32_t Be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void Put32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }

static uint16_t IpCsum(const uint8_t* p, int len) {
    uint32_t s = 0;
    for (int i = 0; i + 1 < len; i += 2) s += ((uint32_t)p[i] << 8) | p[i + 1];
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

// build + send a client message (type 1 discover / 3 request) as a broadcast
// ethernet frame from 0.0.0.0:68 to 255.255.255.255:67. (satoru)
static void SendMsg(uint8_t type, uint32_t req_ip, uint32_t server) {
    static uint8_t f[400];
    for (int i = 0; i < (int)sizeof(f); i++) f[i] = 0;
    for (int i = 0; i < 6; i++) { f[i] = 0xFF; f[6 + i] = g_mac[i]; }
    f[12] = 0x08; f[13] = 0x00;
    uint8_t* ip = f + 14;
    uint8_t* udp = ip + 20;
    uint8_t* b = udp + 8;                 // bootp (satoru)
    b[0] = 1; b[1] = 1; b[2] = 6; b[3] = 0;
    Put32(b + 4, g_xid);
    b[10] = 0x80; b[11] = 0x00;           // broadcast flag: server replies to ff:ff (satoru)
    for (int i = 0; i < 6; i++) b[28 + i] = g_mac[i];
    uint8_t* o = b + 236;
    o[0] = 0x63; o[1] = 0x82; o[2] = 0x53; o[3] = 0x63; o += 4;
    *o++ = 53; *o++ = 1; *o++ = type;
    *o++ = 61; *o++ = 7; *o++ = 1; for (int i = 0; i < 6; i++) *o++ = g_mac[i];
    if (type == 3) {
        *o++ = 50; *o++ = 4; Put32(o, req_ip); o += 4;
        *o++ = 54; *o++ = 4; Put32(o, server); o += 4;
    }
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;   // mask router dns lease (satoru)
    *o++ = 255;
    int bootp_len = (int)(o - b);
    if (bootp_len < 300 - 14 - 28) bootp_len = 300 - 14 - 28;       // pad to a classic 300-byte frame (satoru)
    int udp_len = 8 + bootp_len, ip_len = 20 + udp_len;
    udp[0] = 0; udp[1] = 68; udp[2] = 0; udp[3] = 67;
    udp[4] = udp_len >> 8; udp[5] = (uint8_t)udp_len;                // checksum 0 = none (satoru)
    ip[0] = 0x45; ip[2] = ip_len >> 8; ip[3] = (uint8_t)ip_len;
    ip[8] = 64; ip[9] = 17;
    ip[16] = ip[17] = ip[18] = ip[19] = 0xFF;                        // dst 255.255.255.255 (src stays 0) (satoru)
    uint16_t cs = IpCsum(ip, 20);
    ip[10] = cs >> 8; ip[11] = (uint8_t)cs;
    E1000::Send(f, (uint16_t)(14 + ip_len));
}

bool Dhcp::OnFrame(const uint8_t* d, uint16_t len) {
    if (len < 14 + 20 + 8 + 240) return false;
    if (d[12] != 0x08 || d[13] != 0x00) return false;
    const uint8_t* ip = d + 14;
    if ((ip[0] >> 4) != 4 || ip[9] != 17) return false;
    int ihl = (ip[0] & 15) * 4;
    if (14 + ihl + 8 + 240 > len) return false;
    const uint8_t* udp = ip + ihl;
    if (udp[2] != 0 || udp[3] != 68) return false;
    const uint8_t* b = udp + 8;
    if (b[0] != 2 || Be32(b + 4) != g_xid) return false;
    for (int i = 0; i < 6; i++) if (b[28 + i] != g_mac[i]) return false;
    if (Be32(b + 236) != 0x63825363u) return false;
    uint8_t type = 0;
    uint32_t mask = 0, router = 0, dns = 0, lease = 0, server = 0;
    const uint8_t* o = b + 240;
    const uint8_t* end = d + len;
    while (o < end && *o != 255) {
        if (*o == 0) { o++; continue; }
        if (o + 2 > end) break;
        uint8_t c = o[0], l = o[1];
        if (o + 2 + l > end) break;
        const uint8_t* v = o + 2;
        if (c == 53 && l >= 1) type = v[0];
        else if (c == 1 && l >= 4) mask = Be32(v);
        else if (c == 3 && l >= 4) router = Be32(v);
        else if (c == 6 && l >= 4) dns = Be32(v);
        else if (c == 51 && l >= 4) lease = Be32(v);
        else if (c == 54 && l >= 4) server = Be32(v);
        o += 2 + l;
    }
    uint32_t yi = Be32(b + 16);
    if (type == 2 && g_state == DS_DISCOVER) {
        g_offer_ip = yi; g_offer_server = server;
        g_mask = mask; g_router = router; g_dns = dns; g_lease_s = lease;
        g_have_offer = true;
    } else if (type == 5 && g_state == DS_REQUEST) {
        g_offer_ip = yi;
        if (mask) g_mask = mask;
        if (router) g_router = router;
        if (dns) g_dns = dns;
        if (lease) g_lease_s = lease;
        g_have_ack = true;
    } else if (type == 6 && g_state == DS_REQUEST) {
        g_have_nak = true;
    } else {
        return false;
    }
    return true;
}

static void Apply() {
    uint32_t ip = g_offer_ip;
    TCPStack::SetIP(ip);
    if (g_mask) TCPStack::SetSubnetMask(g_mask);
    if (g_router) TCPStack::SetGateway(g_router);
    if (g_dns) TCPStack::SetDNS(g_dns);
    NetworkInterface* eth = Network::GetInterface("eth0");
    if (eth) {
        eth->ip = Network::MakeIP((uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip);
        if (g_mask) eth->netmask = Network::MakeIP((uint8_t)(g_mask >> 24), (uint8_t)(g_mask >> 16), (uint8_t)(g_mask >> 8), (uint8_t)g_mask);
        if (g_router) eth->gateway = Network::MakeIP((uint8_t)(g_router >> 24), (uint8_t)(g_router >> 16), (uint8_t)(g_router >> 8), (uint8_t)g_router);
        if (g_dns) eth->dns = Network::MakeIP((uint8_t)(g_dns >> 24), (uint8_t)(g_dns >> 16), (uint8_t)(g_dns >> 8), (uint8_t)g_dns);
    }
    Log("[dhcp] lease bound: ip "); LogIp(ip);
    Log(" mask "); LogIp(g_mask); Log(" gw "); LogIp(g_router);
    Log(" dns "); LogIp(g_dns); Log(" lease "); SerialLogger::LogDec((int)g_lease_s); Log("s\r\n");
}

static void StartDiscover(uint64_t now) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    g_xid = lo ^ (hi * 2654435761u) ^ 0x4B52u;
    E1000::GetMAC(g_mac);
    g_have_offer = g_have_ack = g_have_nak = false;
    g_state = DS_DISCOVER;
    g_t_last = now;
    SendMsg(1, 0, 0);
    Log("[dhcp] discover sent\r\n");
}

void Dhcp::Tick() {
    if (!E1000::IsDetected()) return;
    uint64_t now = Scheduler::NowMs();
    if (!E1000::IsLinkUp()) {
        // no carrier: forget progress so a later link-up starts a fresh exchange (satoru)
        if (!g_was_down) Log("[dhcp] link down, will restart when it returns\r\n");
        g_was_down = true;
        if (g_state != DS_BOUND) g_state = DS_IDLE;
        return;
    }
    if (g_was_down) {
        g_was_down = false;
        g_state = DS_IDLE;
        g_delay_ms = 2000; g_tries = 0;
        Log("[dhcp] link up, starting\r\n");
    }
    switch (g_state) {
    case DS_IDLE:
        StartDiscover(now);
        break;
    case DS_DISCOVER:
        if (g_have_offer) {
            g_have_offer = false;
            Log("[dhcp] offer "); LogIp(g_offer_ip); Log(" from "); LogIp(g_offer_server); Log("\r\n");
            g_state = DS_REQUEST;
            g_t_last = now;
            SendMsg(3, g_offer_ip, g_offer_server);
        } else if (now - g_t_last > g_delay_ms) {
            g_t_last = now;
            if (g_delay_ms < 16000) g_delay_ms *= 2;
            if (++g_tries <= 3 || (g_tries & 7) == 0) Log("[dhcp] no offer yet, retrying discover\r\n");
            SendMsg(1, 0, 0);
        }
        break;
    case DS_REQUEST:
        if (g_have_ack) {
            g_have_ack = false;
            Apply();
            g_state = DS_BOUND;
            uint32_t half = (g_lease_s ? g_lease_s : 3600) / 2;
            g_t_renew = now + (uint64_t)half * 1000;
            g_delay_ms = 2000; g_tries = 0;
        } else if (g_have_nak || now - g_t_last > 4000) {
            g_have_nak = false;
            Log("[dhcp] request failed (nak/timeout), restarting\r\n");
            g_state = DS_IDLE;
        }
        break;
    case DS_BOUND:
        if (now >= g_t_renew) {
            Log("[dhcp] renewing lease\r\n");
            StartDiscover(now);
        }
        break;
    }
}

bool Dhcp::IsBound() { return g_state == DS_BOUND; }
// end (satoru)
