#include "dhcp.h"
#include "network.h"
#include "tcpip.h"
#include "../drivers/e1000.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../proc/scheduler.h"
#include "../proc/spinlock.h"

// lease states. bound is the steady state; renew/rebind are the rfc 2131
// lifecycle timers that keep an address alive without a full exchange. (satoru)
enum DhcpState : uint8_t {
    DS_IDLE = 0,
    DS_DISCOVER,
    DS_REQUEST,
    DS_BOUND,
    DS_RENEW,
    DS_REBIND
};

static DhcpState g_state = DS_IDLE;
static uint32_t  g_xid = 0;
static uint32_t  g_retry_ms = 2000;      // current retransmit backoff (satoru)
static uint8_t   g_tries = 0;
static uint64_t  g_t_next = 0;           // next retransmit / renew action (satoru)
static uint64_t  g_t1_ms = 0;            // renew (unicast) deadline (satoru)
static uint64_t  g_t2_ms = 0;            // rebind (broadcast) deadline (satoru)
static uint64_t  g_lease_end_ms = 0;     // absolute lease expiry (satoru)
static bool      g_was_down = true;
static uint8_t   g_mac[6];
// serializes the lease state machine: Tick and the RX path (OnFrame). (satoru)
static Spinlock  g_tick_lock;

// parsed reply, filled by OnFrame (rx path) and consumed by Tick (satoru)
static volatile bool g_have_offer = false, g_have_ack = false, g_have_nak = false;
static uint32_t g_offer_ip, g_offer_server;     // host order a<<24.. (satoru)
static uint32_t g_mask, g_router, g_dns, g_dns2, g_lease_s;
static uint32_t g_t1_opt, g_t2_opt;             // rfc 2131 options 58/59 (satoru)
static uint32_t g_server;                       // selected server identifier (satoru)

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

// TSC-based real ms, not Scheduler::NowMs(): the latter is the PIT-IRQ count,
// which coalesces and barely advances under load, so discover retries, renew
// and rebind deadlines never fired. (satoru)
static uint64_t NowMs() { return Timer::GetRealMs64(); }

// one mac from the kernel arp cache, so a renew can be unicast when the
// neighbour is known and broadcast otherwise. (satoru)
static bool ArpMac(uint32_t ip, uint8_t out[6]) {
    IPv4Address a = Network::MakeIP((uint8_t)(ip >> 24), (uint8_t)(ip >> 16),
                                    (uint8_t)(ip >> 8), (uint8_t)ip);
    MACAddress* m = Network::ARPLookup(a);
    if (!m) return false;
    for (int i = 0; i < 6; i++) out[i] = m->bytes[i];
    return true;
}

// build + send a client message (type 1 discover / 3 request). discover and
// rebind are broadcast; renew is unicast to the server when arp knows it.
// the source stays 0.0.0.0:68 and the broadcast flag asks the server to reply
// to ff:ff, so a unicast request still gets an answer we can receive. (satoru)
static void SendMsg(uint8_t type, uint32_t req_ip, uint32_t server, bool unicast) {
    static uint8_t f[400];
    for (int i = 0; i < (int)sizeof(f); i++) f[i] = 0;

    uint32_t dst_ip = 0xFFFFFFFFu;
    uint8_t dst_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    if (unicast) {
        uint32_t target = server ? server : g_router;
        if (target && ArpMac(target, dst_mac)) dst_ip = target;
    }
    for (int i = 0; i < 6; i++) { f[i] = dst_mac[i]; f[6 + i] = g_mac[i]; }
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
    // parameter request list: mask router dns lease t1 t2 (satoru)
    *o++ = 55; *o++ = 6; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51; *o++ = 58; *o++ = 59;
    *o++ = 255;
    int bootp_len = (int)(o - b);
    if (bootp_len < 300 - 14 - 28) bootp_len = 300 - 14 - 28;       // pad to a classic 300-byte frame (satoru)
    int udp_len = 8 + bootp_len, ip_len = 20 + udp_len;
    udp[0] = 0; udp[1] = 68; udp[2] = 0; udp[3] = 67;
    udp[4] = udp_len >> 8; udp[5] = (uint8_t)udp_len;                // checksum 0 = none (satoru)
    ip[0] = 0x45; ip[2] = ip_len >> 8; ip[3] = (uint8_t)ip_len;
    ip[8] = 64; ip[9] = 17;
    Put32(ip + 16, dst_ip);
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
    // the reply publishes offer/ack state that Tick consumes, so it runs under
    // the same lock as the state machine. (satoru)
    SpinLockCpuGuard frame_guard(g_tick_lock);
    if (b[0] != 2 || Be32(b + 4) != g_xid) return false;
    for (int i = 0; i < 6; i++) if (b[28 + i] != g_mac[i]) return false;
    if (Be32(b + 236) != 0x63825363u) return false;
    uint8_t type = 0;
    uint32_t mask = 0, router = 0, dns = 0, dns2 = 0, lease = 0, server = 0;
    uint32_t t1 = 0, t2 = 0;
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
        else if (c == 6 && l >= 4) { dns = Be32(v); if (l >= 8) dns2 = Be32(v + 4); }
        else if (c == 51 && l >= 4) lease = Be32(v);
        else if (c == 54 && l >= 4) server = Be32(v);
        else if (c == 58 && l >= 4) t1 = Be32(v);
        else if (c == 59 && l >= 4) t2 = Be32(v);
        o += 2 + l;
    }
    uint32_t yi = Be32(b + 16);
    if (type == 2 && g_state == DS_DISCOVER) {
        g_offer_ip = yi; g_offer_server = server;
        g_mask = mask; g_router = router; g_dns = dns; g_dns2 = dns2;
        g_lease_s = lease; g_t1_opt = t1; g_t2_opt = t2;
        g_have_offer = true;
    } else if (type == 5 &&
               (g_state == DS_REQUEST || g_state == DS_RENEW || g_state == DS_REBIND)) {
        if (yi) g_offer_ip = yi;
        if (mask) g_mask = mask;
        if (router) g_router = router;
        if (dns) { g_dns = dns; g_dns2 = dns2; }
        if (lease) g_lease_s = lease;
        if (t1) g_t1_opt = t1;
        if (t2) g_t2_opt = t2;
        if (server) g_server = server;
        g_have_ack = true;
    } else if (type == 6 &&
               (g_state == DS_REQUEST || g_state == DS_BOUND ||
                g_state == DS_RENEW || g_state == DS_REBIND)) {
        g_have_nak = true;
    } else {
        return false;
    }
    return true;
}

static void Apply(uint64_t now) {
    uint32_t ip = g_offer_ip;
    TCPStack::SetIP(ip);
    if (g_mask) TCPStack::SetSubnetMask(g_mask);
    if (g_router) TCPStack::SetGateway(g_router);
    if (g_dns) TCPStack::SetDNS(g_dns);
    Network::SetDNSServers(g_dns, g_dns2);
    if (g_offer_server) g_server = g_offer_server;
    NetworkInterface* eth = Network::GetInterface("eth0");
    if (eth) {
        eth->ip = Network::MakeIP((uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip);
        if (g_mask) eth->netmask = Network::MakeIP((uint8_t)(g_mask >> 24), (uint8_t)(g_mask >> 16), (uint8_t)(g_mask >> 8), (uint8_t)g_mask);
        if (g_router) eth->gateway = Network::MakeIP((uint8_t)(g_router >> 24), (uint8_t)(g_router >> 16), (uint8_t)(g_router >> 8), (uint8_t)g_router);
        if (g_dns) eth->dns = Network::MakeIP((uint8_t)(g_dns >> 24), (uint8_t)(g_dns >> 16), (uint8_t)(g_dns >> 8), (uint8_t)g_dns);
    }

    // rfc 2131 derived timers: t1 = renew, t2 = rebind, then expiry. clamp so
    // a broken server cannot make us renew instantly or never. (satoru)
    uint32_t lease = g_lease_s ? g_lease_s : 3600;
    if (lease < 2) lease = 2;
    uint32_t t1 = g_t1_opt ? g_t1_opt : lease / 2;
    uint32_t t2 = g_t2_opt ? g_t2_opt : (lease - lease / 8);
    if (t1 < 1) t1 = 1;
    if (t1 >= lease) t1 = lease / 2;
    if (t2 <= t1) t2 = t1 + (lease - t1) / 2;
    if (t2 > lease) t2 = lease;
    g_t1_ms = now + (uint64_t)t1 * 1000u;
    g_t2_ms = now + (uint64_t)t2 * 1000u;
    g_lease_end_ms = now + (uint64_t)lease * 1000u;
    g_state = DS_BOUND;
    g_retry_ms = 2000; g_tries = 0;

    Log("[dhcp] lease bound: ip "); LogIp(ip);
    Log(" mask "); LogIp(g_mask); Log(" gw "); LogIp(g_router);
    Log(" dns "); LogIp(g_dns);
    if (g_dns2) { Log(","); LogIp(g_dns2); }
    Log(" lease "); SerialLogger::LogDec((int)g_lease_s); Log("s\r\n");
    Log("[dhcp] timers renew "); SerialLogger::LogDec((int)t1);
    Log("s rebind "); SerialLogger::LogDec((int)t2); Log("s\r\n");
}

// drop the address (expiry or server nak) and go back to discovery. (satoru)
static void Release(const char* why) {
    Log("[dhcp] lease released: "); Log(why); Log("\r\n");
    TCPStack::SetIP(0);
    TCPStack::SetSubnetMask(0);
    TCPStack::SetGateway(0);
    TCPStack::SetDNS(0);
    Network::SetDNSServers(0, 0);
    NetworkInterface* eth = Network::GetInterface("eth0");
    if (eth) {
        eth->ip = Network::MakeIP(0, 0, 0, 0);
        eth->netmask = Network::MakeIP(0, 0, 0, 0);
        eth->gateway = Network::MakeIP(0, 0, 0, 0);
        eth->dns = Network::MakeIP(0, 0, 0, 0);
    }
    g_dns = g_dns2 = 0;
    g_server = 0;
    g_t1_opt = g_t2_opt = 0;
    g_state = DS_IDLE;
    g_retry_ms = 2000; g_tries = 0;
    g_have_offer = g_have_ack = g_have_nak = false;
}

static void StartDiscover(uint64_t now) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    g_xid = lo ^ (hi * 2654435761u) ^ 0x4B52u ^ (uint32_t)now;
    if (g_xid == 0) g_xid = 1;
    E1000::GetMAC(g_mac);
    g_have_offer = g_have_ack = g_have_nak = false;
    g_server = 0;
    g_t1_opt = g_t2_opt = 0;
    g_state = DS_DISCOVER;
    g_retry_ms = 2000; g_tries = 0;
    g_t_next = now + g_retry_ms;
    SendMsg(1, 0, 0, false);
    Log("[dhcp] discover sent\r\n");
}

// send a request for the address we already hold. renew is unicast when arp
// can resolve the server, rebind is always broadcast. (satoru)
static void SendRequest(uint64_t now, bool rebind) {
    uint32_t ip = g_offer_ip;
    uint32_t sid = g_server ? g_server : g_offer_server;
    g_have_ack = g_have_nak = false;
    SendMsg(3, ip, sid, !rebind);
    g_t_next = now + g_retry_ms;
    if (g_state == DS_REQUEST) {
        Log("[dhcp] request sent\r\n");
    } else if (rebind) {
        Log("[dhcp] rebinding lease (broadcast)\r\n");
    } else {
        Log("[dhcp] renewing lease\r\n");
    }
}

void Dhcp::Tick() {
    // called from the network process, the gui watchdog and the cli loop, so
    // two cpus must never step the state machine at once. OnFrame takes the
    // same lock, so a reply cannot update the lease while Tick reads it. (satoru)
    SpinLockCpuGuard tick_guard(g_tick_lock);
    if (!E1000::IsDetected()) return;
    uint64_t now = NowMs();
    if (!E1000::IsLinkUp()) {
        // no carrier: keep the lease and its timers; if the link returns
        // before expiry just revalidate it instead of starting over. (satoru)
        if (!g_was_down) Log("[dhcp] link down, lease kept until it returns\r\n");
        g_was_down = true;
        return;
    }
    if (g_was_down) {
        g_was_down = false;
        g_retry_ms = 2000; g_tries = 0;
        if ((g_state == DS_BOUND || g_state == DS_RENEW || g_state == DS_REBIND) &&
            now < g_lease_end_ms) {
            Log("[dhcp] link up, revalidating lease\r\n");
            g_state = DS_RENEW;
            SendRequest(now, false);
        } else {
            g_state = DS_IDLE;
            Log("[dhcp] link up, starting\r\n");
        }
    }
    switch (g_state) {
    case DS_IDLE:
        StartDiscover(now);
        break;
    case DS_DISCOVER:
        if (g_have_offer) {
            g_have_offer = false;
            g_server = g_offer_server;
            Log("[dhcp] offer "); LogIp(g_offer_ip); Log(" from "); LogIp(g_offer_server); Log("\r\n");
            g_state = DS_REQUEST;
            g_retry_ms = 2000; g_tries = 0;
            SendRequest(now, false);
        } else if (now >= g_t_next) {
            if (++g_tries <= 3 || (g_tries & 7) == 0) Log("[dhcp] no offer yet, retrying discover\r\n");
            if (g_retry_ms < 16000) g_retry_ms *= 2;
            if (g_retry_ms > 16000) g_retry_ms = 16000;
            g_t_next = now + g_retry_ms;
            SendMsg(1, 0, 0, false);
        }
        break;
    case DS_REQUEST:
        if (g_have_ack) {
            g_have_ack = false;
            Apply(now);
        } else if (g_have_nak) {
            g_have_nak = false;
            Log("[dhcp] request nak, restarting\r\n");
            Release("server nak");
            StartDiscover(now);
        } else if (now >= g_t_next) {
            if (++g_tries > 3) {
                Log("[dhcp] request failed (nak/timeout), restarting\r\n");
                g_state = DS_IDLE;
            } else {
                SendMsg(3, g_offer_ip, g_server, false);
                g_t_next = now + g_retry_ms;
            }
        }
        break;
    case DS_BOUND:
        if (g_have_nak) {
            g_have_nak = false;
            Release("server nak");
            StartDiscover(now);
        } else if (now >= g_lease_end_ms) {
            Release("expired");
            StartDiscover(now);
        } else if (now >= g_t1_ms) {
            g_state = DS_RENEW;
            g_retry_ms = 2000; g_tries = 0;
            SendRequest(now, false);
        }
        break;
    case DS_RENEW:
        if (g_have_ack) {
            g_have_ack = false;
            Apply(now);
        } else if (g_have_nak) {
            g_have_nak = false;
            Release("server nak during renew");
            StartDiscover(now);
        } else if (now >= g_lease_end_ms) {
            Release("expired");
            StartDiscover(now);
        } else if (now >= g_t2_ms) {
            g_state = DS_REBIND;
            g_retry_ms = 2000; g_tries = 0;
            SendRequest(now, true);
        } else if (now >= g_t_next) {
            if (g_retry_ms < 16000) g_retry_ms *= 2;
            if (g_retry_ms > 16000) g_retry_ms = 16000;
            SendRequest(now, false);
        }
        break;
    case DS_REBIND:
        if (g_have_ack) {
            g_have_ack = false;
            Apply(now);
        } else if (g_have_nak) {
            g_have_nak = false;
            Release("server nak during rebind");
            StartDiscover(now);
        } else if (now >= g_lease_end_ms) {
            Release("expired");
            StartDiscover(now);
        } else if (now >= g_t_next) {
            if (g_retry_ms < 16000) g_retry_ms *= 2;
            if (g_retry_ms > 16000) g_retry_ms = 16000;
            SendRequest(now, true);
        }
        break;
    }
}

bool Dhcp::IsBound() {
    return g_state == DS_BOUND || g_state == DS_RENEW || g_state == DS_REBIND;
}

const char* Dhcp::StateString() {
    switch (g_state) {
        case DS_IDLE:     return "idle";
        case DS_DISCOVER: return "discover";
        case DS_REQUEST:  return "request";
        case DS_BOUND:    return "bound";
        case DS_RENEW:    return "renewing";
        case DS_REBIND:   return "rebinding";
    }
    return "unknown";
}

uint32_t Dhcp::LeaseSeconds() { return g_lease_s; }

uint32_t Dhcp::LeaseRemainingSeconds() {
    if (!IsBound()) return 0;
    uint64_t now = NowMs();
    if (now >= g_lease_end_ms) return 0;
    return (uint32_t)((g_lease_end_ms - now) / 1000u);
}

uint32_t Dhcp::GetServer() { return g_server ? g_server : g_offer_server; }
uint32_t Dhcp::GetPrimaryDNS() { return g_dns; }
uint32_t Dhcp::GetSecondaryDNS() { return g_dns2; }
// end (satoru)
