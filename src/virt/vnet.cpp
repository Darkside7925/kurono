//  kurono os - virtio-net host device + nat (see vnet.h)
#include "vnet.h"
#include "virtio_dev.h"
#include "guest_mem.h"
#include "../net/tcpip.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../proc/spinlock.h"
#include "../kernel/types.h"

namespace {
    // the private guest link (host order) (satoru)
    const uint32_t GW_IP    = (10u << 24) | (0u << 16) | (3u << 8) | 1u;
    const uint32_t GUEST_IP = (10u << 24) | (0u << 16) | (3u << 8) | 15u;
    const uint32_t NETMASK  = 0xFFFFFF00u;
    const uint8_t  GW_MAC[6]    = { 0x52, 0x54, 0x00, 0x4B, 0x52, 0x01 };
    const uint8_t  GUEST_MAC[6] = { 0x52, 0x54, 0x00, 0x4B, 0x52, 0x0F };

    const uint16_t PORT_LO = 32768, PORT_HI = 48999;
    const int      NAT_MAX = 1024;
    const int      RXQ = 256;            // frames waiting for guest rx buffers
    const int      FRAME_MAX = 1514;
    const int      VHDR = 12;            // virtio_net_hdr_mrg_rxbuf (version 1)

    struct Nat {
        bool     used;
        uint8_t  proto;
        uint16_t gport;                  // guest port (icmp: echo id)
        uint32_t rip;                    // remote ip the guest talks to (after redirect)
        uint16_t rport;
        uint16_t hport;                  // our port on the host ip (icmp: echo id)
        uint32_t gdst;                   // destination the guest used (dns redirect: the gateway)
        uint64_t last_ms;
        uint8_t  fin;                    // bit0 guest fin, bit1 remote fin, bit2 rst
    };

    struct Frame { uint16_t len; uint8_t data[FRAME_MAX]; };

    VirtioDev g_dev;
    bool      g_up = false;
    volatile bool g_active = false;
    Spinlock  g_lock;                    // nat table + rx ring
    Nat       g_nat[NAT_MAX];
    Frame     g_rx[RXQ];
    volatile uint32_t g_rx_w = 0, g_rx_r = 0;
    uint16_t  g_next_port = PORT_LO;
    uint8_t   g_guest_mac[6];
    uint64_t  g_last_sweep = 0;

    uint64_t  st_tx, st_rx, st_drop_tx, st_drop_rx, st_nat_out, st_nat_in, st_dhcp, st_arp, st_full;

    inline uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
    inline uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
    inline void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
    inline void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

    uint32_t sum16(const uint8_t* d, int n, uint32_t s) {
        int i = 0;
        for (; i + 1 < n; i += 2) s += (uint32_t)((d[i] << 8) | d[i + 1]);
        if (i < n) s += (uint32_t)(d[i] << 8);
        return s;
    }
    uint16_t fold(uint32_t s) { while (s >> 16) s = (s & 0xFFFF) + (s >> 16); return (uint16_t)~s; }

    void fix_ip_csum(uint8_t* ip) {
        int ihl = (ip[0] & 15) * 4;
        ip[10] = ip[11] = 0;
        wr16(ip + 10, fold(sum16(ip, ihl, 0)));
    }

    // tcp / udp checksum over the pseudo header; a udp checksum of 0 means
    // "none" and stays 0 (satoru)
    void fix_l4_csum(uint8_t* ip, int total) {
        int ihl = (ip[0] & 15) * 4;
        uint8_t proto = ip[9];
        uint8_t* l4 = ip + ihl;
        int l4len = total - ihl;
        int off;
        if (proto == 6) { if (l4len < 20) return; off = 16; }
        else if (proto == 17) { if (l4len < 8) return; off = 6; if (rd16(l4 + 6) == 0) return; }
        else return;
        l4[off] = l4[off + 1] = 0;
        uint32_t s = 0;
        s = sum16(ip + 12, 8, s);
        s += proto;
        s += (uint32_t)l4len;
        s = sum16(l4, l4len, s);
        uint16_t c = fold(s);
        if (proto == 17 && c == 0) c = 0xFFFF;
        wr16(l4 + off, c);
    }

    // true when a tcp/udp checksum on the wire is valid (or udp sent none) (satoru)
    bool l4_csum_ok(const uint8_t* ip, int total) {
        int ihl = (ip[0] & 15) * 4;
        uint8_t proto = ip[9];
        const uint8_t* l4 = ip + ihl;
        int l4len = total - ihl;
        if (proto == 6 && l4len < 20) return false;
        if (proto == 17 && (l4len < 8 || rd16(l4 + 6) == 0)) return l4len >= 8;
        uint32_t s = sum16(ip + 12, 8, 0);
        s += proto;
        s += (uint32_t)l4len;
        s = sum16(l4, l4len, s);
        return fold(s) == 0;
    }

    void fix_icmp_csum(uint8_t* icmp, int len) {
        icmp[2] = icmp[3] = 0;
        wr16(icmp + 2, fold(sum16(icmp, len, 0)));
    }

    uint64_t now_ms() { return Timer::GetRealMs64(); }

    // frame for the guest (eth header included); lock held or not, both paths
    // take it here (satoru)
    bool to_guest_locked(const uint8_t* eth, int len) {
        if (len > FRAME_MAX || len < 14) return false;
        if (g_rx_w - g_rx_r >= (uint32_t)RXQ) { st_full++; return false; }
        Frame& f = g_rx[g_rx_w % RXQ];
        memcpy(f.data, eth, len);
        f.len = (uint16_t)len;
        __atomic_store_n(&g_rx_w, g_rx_w + 1, __ATOMIC_RELEASE);
        return true;
    }
    bool to_guest(const uint8_t* eth, int len) {
        SpinLockGuard g(g_lock);
        return to_guest_locked(eth, len);
    }

    void eth_hdr(uint8_t* f, const uint8_t* dst, const uint8_t* src) {
        for (int i = 0; i < 6; i++) { f[i] = dst[i]; f[6 + i] = src[i]; }
        f[12] = 0x08; f[13] = 0x00;
    }

    bool port_in_use(uint8_t proto, uint16_t p) {
        for (int i = 0; i < NAT_MAX; i++)
            if (g_nat[i].used && g_nat[i].proto == proto && g_nat[i].hport == p) return true;
        return false;
    }

    // find or create the translation for a guest flow (lock held) (satoru)
    Nat* nat_out(uint8_t proto, uint16_t gport, uint32_t rip, uint16_t rport, uint32_t gdst) {
        Nat* freee = nullptr;
        Nat* oldest = nullptr;
        for (int i = 0; i < NAT_MAX; i++) {
            Nat& e = g_nat[i];
            if (!e.used) { if (!freee) freee = &e; continue; }
            if (e.proto == proto && e.gport == gport && e.rip == rip && e.rport == rport) return &e;
            if (!oldest || e.last_ms < oldest->last_ms) oldest = &e;
        }
        Nat* e = freee ? freee : oldest;
        if (!e) return nullptr;
        uint16_t hp = 0;
        for (int t = 0; t < (PORT_HI - PORT_LO + 1); t++) {
            uint16_t p = g_next_port++;
            if (g_next_port > PORT_HI) g_next_port = PORT_LO;
            if (!port_in_use(proto, p)) { hp = p; break; }
        }
        if (!hp) return nullptr;
        e->used = true; e->proto = proto; e->gport = gport; e->rip = rip; e->rport = rport;
        e->hport = hp; e->gdst = gdst; e->fin = 0; e->last_ms = now_ms();
        return e;
    }

    Nat* nat_in(uint8_t proto, uint16_t hport, uint32_t rip, uint16_t rport) {
        if (proto != 1 && (hport < PORT_LO || hport > PORT_HI)) return nullptr;
        for (int i = 0; i < NAT_MAX; i++) {
            Nat& e = g_nat[i];
            if (e.used && e.proto == proto && e.hport == hport && e.rip == rip && (proto == 1 || e.rport == rport))
                return &e;
        }
        return nullptr;
    }

    void sweep() {
        uint64_t t = now_ms();
        SpinLockGuard g(g_lock);
        for (int i = 0; i < NAT_MAX; i++) {
            Nat& e = g_nat[i];
            if (!e.used) continue;
            uint64_t idle = t - e.last_ms, lim;
            if (e.proto == 6) lim = (e.fin & 4) ? 2000 : ((e.fin & 3) == 3 ? 30000 : 1800000);
            else if (e.proto == 17) lim = 120000;
            else lim = 30000;
            if (idle > lim) e.used = false;
        }
    }

    // ---- guest -> world ----------------------------------------------------

    void reply_arp(const uint8_t* f, int len) {
        if (len < 42) return;
        const uint8_t* a = f + 14;
        if (rd16(a) != 1 || rd16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4 || rd16(a + 6) != 1) return;
        uint32_t tpa = rd32(a + 24);
        uint32_t spa = rd32(a + 14);
        if (tpa == spa || tpa == GUEST_IP) return;        // gratuitous / duplicate address probe
        uint8_t r[42];
        for (int i = 0; i < 6; i++) { r[i] = a[8 + i]; r[6 + i] = GW_MAC[i]; }
        r[12] = 0x08; r[13] = 0x06;
        uint8_t* p = r + 14;
        wr16(p, 1); wr16(p + 2, 0x0800); p[4] = 6; p[5] = 4; wr16(p + 6, 2);
        for (int i = 0; i < 6; i++) p[8 + i] = GW_MAC[i];
        wr32(p + 14, tpa);                                // proxy: everything is the gateway
        for (int i = 0; i < 6; i++) p[18 + i] = a[8 + i];
        wr32(p + 24, spa);
        st_arp++;
        to_guest(r, 42);
    }

    void dhcp(const uint8_t* f, int len) {
        const uint8_t* ip = f + 14;
        int ihl = (ip[0] & 15) * 4;
        const uint8_t* b = ip + ihl + 8;
        int blen = len - 14 - ihl - 8;
        if (blen < 240 || b[0] != 1 || rd32(b + 236) != 0x63825363) return;
        int type = 0;
        for (int i = 240; i + 1 < blen;) {
            uint8_t o = b[i];
            if (o == 255) break;
            if (o == 0) { i++; continue; }
            uint8_t l = b[i + 1];
            if (o == 53 && l >= 1) type = b[i + 2];
            i += 2 + l;
        }
        int rtype = (type == 1) ? 2 : (type == 3) ? 5 : 0;  // discover -> offer, request -> ack
        if (!rtype) return;
        static uint8_t r[14 + 20 + 8 + 300];
        memset(r, 0, sizeof(r));
        static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        eth_hdr(r, bcast, GW_MAC);
        uint8_t* rip = r + 14;
        uint8_t* udp = rip + 20;
        uint8_t* bo = udp + 8;
        bo[0] = 2; bo[1] = 1; bo[2] = 6;
        for (int i = 4; i < 8; i++) bo[i] = b[i];          // xid
        bo[10] = b[10]; bo[11] = b[11];                    // flags
        wr32(bo + 16, GUEST_IP);                           // yiaddr
        wr32(bo + 20, GW_IP);                              // siaddr
        for (int i = 0; i < 16; i++) bo[28 + i] = b[28 + i];
        wr32(bo + 236, 0x63825363);
        uint8_t* o = bo + 240;
        *o++ = 53; *o++ = 1; *o++ = (uint8_t)rtype;
        *o++ = 54; *o++ = 4; wr32(o, GW_IP); o += 4;
        *o++ = 51; *o++ = 4; wr32(o, 86400); o += 4;
        *o++ = 1;  *o++ = 4; wr32(o, NETMASK); o += 4;
        *o++ = 3;  *o++ = 4; wr32(o, GW_IP); o += 4;
        *o++ = 6;  *o++ = 4; wr32(o, GW_IP); o += 4;
        *o++ = 26; *o++ = 2; wr16(o, 1500); o += 2;
        *o++ = 255;
        int blen_out = 300;
        wr16(udp, 67); wr16(udp + 2, 68); wr16(udp + 4, (uint16_t)(8 + blen_out));
        rip[0] = 0x45; wr16(rip + 2, (uint16_t)(20 + 8 + blen_out)); rip[8] = 64; rip[9] = 17;
        wr32(rip + 12, GW_IP); wr32(rip + 16, 0xFFFFFFFFu);
        fix_ip_csum(rip);
        fix_l4_csum(rip, 20 + 8 + blen_out);
        st_dhcp++;
        to_guest(r, 14 + 20 + 8 + blen_out);
    }

    void icmp_echo_local(const uint8_t* f, int len) {
        // ping the gateway: answer here (satoru)
        static uint8_t r[FRAME_MAX];
        if (len > FRAME_MAX) return;
        memcpy(r, f, len);
        eth_hdr(r, g_guest_mac, GW_MAC);
        uint8_t* ip = r + 14;
        int ihl = (ip[0] & 15) * 4;
        int total = rd16(ip + 2);
        if (14 + total > len) return;
        wr32(ip + 16, rd32(ip + 12)); wr32(ip + 12, GW_IP);
        ip[8] = 64;
        uint8_t* ic = ip + ihl;
        ic[0] = 0;
        fix_icmp_csum(ic, total - ihl);
        fix_ip_csum(ip);
        to_guest(r, 14 + total);
    }

    void guest_ipv4(const uint8_t* f, int len) {
        static uint8_t pkt[FRAME_MAX];
        const uint8_t* ip = f + 14;
        if (len < 34 || (ip[0] >> 4) != 4) { st_drop_tx++; return; }
        int ihl = (ip[0] & 15) * 4;
        int total = rd16(ip + 2);
        if (ihl < 20 || total < ihl || 14 + total > len || total > 1500) { st_drop_tx++; return; }
        uint8_t proto = ip[9];
        uint32_t src = rd32(ip + 12), dst = rd32(ip + 16);
        const uint8_t* l4 = ip + ihl;
        int l4len = total - ihl;
        if (proto == 17 && l4len >= 8 && rd16(l4 + 2) == 67) { dhcp(f, len); return; }
        if (src != GUEST_IP) { st_drop_tx++; return; }
        if (rd16(ip + 6) & 0x3FFF) { st_drop_tx++; return; }       // fragments
        if ((dst >> 28) >= 14 || dst == 0xFFFFFFFFu) return;        // multicast / broadcast
        uint32_t real_dst = dst;
        if ((dst & NETMASK) == (GW_IP & NETMASK)) {
            if (dst != GW_IP) { st_drop_tx++; return; }
            if (proto == 1 && l4len >= 8 && l4[0] == 8) { icmp_echo_local(f, len); return; }
            // dns to the gateway goes to kurono's own resolver (satoru)
            if ((proto == 17 || proto == 6) && l4len >= 4 && rd16(l4 + 2) == 53) {
                real_dst = TCPStack::GetDNS();
                if (!real_dst) real_dst = TCPStack::GetGateway();
                if (!real_dst) { st_drop_tx++; return; }
            } else { st_drop_tx++; return; }
        }
        if (!TCPStack::IsUp() || !TCPStack::GetIP()) { st_drop_tx++; return; }
        memcpy(pkt, ip, total);
        uint8_t* l = pkt + ihl;
        uint32_t hip = TCPStack::GetIP();
        bool ok = false;
        {
            SpinLockGuard g(g_lock);
            if ((proto == 6 && l4len >= 20) || (proto == 17 && l4len >= 8)) {
                Nat* e = nat_out(proto, rd16(l), real_dst, rd16(l + 2), dst);
                if (e) {
                    e->last_ms = now_ms();
                    if (proto == 6) {
                        uint8_t fl = l[13];
                        if ((fl & 2) && !(fl & 16)) e->fin = 0;   // a new syn on a reused port (satoru)
                        if (fl & 1) e->fin |= 1;
                        if (fl & 4) e->fin |= 4;
                    }
                    wr16(l, e->hport);
                    ok = true;
                }
            } else if (proto == 1 && l4len >= 8 && l[0] == 8) {
                Nat* e = nat_out(1, rd16(l + 4), real_dst, 0, dst);
                if (e) { e->last_ms = now_ms(); wr16(l + 4, e->hport); ok = true; }
            }
        }
        if (!ok) { st_drop_tx++; return; }
        wr32(pkt + 12, hip);
        wr32(pkt + 16, real_dst);
        if (proto == 1) fix_icmp_csum(l, l4len); else fix_l4_csum(pkt, total);
        fix_ip_csum(pkt);
        if (TCPStack::SendRawIPv4(pkt, total)) st_nat_out++; else st_drop_tx++;
    }

    void guest_frame(const uint8_t* f, int len) {
        if (len < 14) return;
        for (int i = 0; i < 6; i++) g_guest_mac[i] = f[6 + i];
        uint16_t et = rd16(f + 12);
        st_tx++;
        if (et == 0x0806) reply_arp(f, len);
        else if (et == 0x0800) guest_ipv4(f, len);
    }

    // ---- world -> guest (rx hook, any cpu) ----------------------------------

    bool rx_hook(const void* packet, int len) {
        if (!g_active || len < 20) return false;
        const uint8_t* ip = (const uint8_t*)packet;
        int ihl = (ip[0] & 15) * 4;
        if (ihl < 20 || len < ihl + 8) return false;
        uint8_t proto = ip[9];
        if (proto != 6 && proto != 17 && proto != 1) return false;
        if (rd32(ip + 16) != TCPStack::GetIP()) return false;
        uint32_t src = rd32(ip + 12);
        const uint8_t* l4 = ip + ihl;
        uint8_t f[FRAME_MAX];
        if (14 + len > FRAME_MAX) return false;
        SpinLockGuard g(g_lock);
        Nat* e = nullptr;
        int inner = 0;
        if (proto == 6 || proto == 17) {
            uint16_t dp = rd16(l4 + 2);
            if (dp < PORT_LO || dp > PORT_HI) return false;
            e = nat_in(proto, dp, src, rd16(l4));
        } else if (l4[0] == 0) {
            e = nat_in(1, rd16(l4 + 4), src, 0);
        } else if ((l4[0] == 3 || l4[0] == 11 || l4[0] == 12) && len >= ihl + 8 + 28) {
            // an icmp error about one of our flows: the quoted header names it (satoru)
            const uint8_t* qi = l4 + 8;
            int qihl = (qi[0] & 15) * 4;
            if (len >= ihl + 8 + qihl + 8 && (qi[9] == 6 || qi[9] == 17)) {
                const uint8_t* ql = qi + qihl;
                e = nat_in(qi[9], rd16(ql), rd32(qi + 16), rd16(ql + 2));
                inner = qihl;
            }
        }
        if (!e) return false;
        // ours, but corrupted on the wire: drop it rather than re-checksum it (satoru)
        if ((proto == 6 || proto == 17) && !l4_csum_ok(ip, len)) { st_drop_rx++; return true; }
        e->last_ms = now_ms();
        eth_hdr(f, g_guest_mac, GW_MAC);
        uint8_t* p = f + 14;
        memcpy(p, ip, len);
        uint8_t* l = p + ihl;
        wr32(p + 16, GUEST_IP);
        if (proto == 6 || proto == 17) {
            wr16(l + 2, e->gport);
            if (e->gdst != e->rip) wr32(p + 12, e->gdst);        // dns answers come from the gateway
            if (proto == 6) {
                if (l[13] & 1) e->fin |= 2;
                if (l[13] & 4) e->fin |= 4;
            }
            fix_l4_csum(p, len);
        } else if (!inner) {
            wr16(l + 4, e->gport);
            if (e->gdst != e->rip) wr32(p + 12, e->gdst);
            fix_icmp_csum(l, len - ihl);
        } else {
            uint8_t* qi = l + 8;
            uint8_t* ql = qi + inner;
            wr32(qi + 12, GUEST_IP);
            wr16(ql, e->gport);
            if (e->gdst != e->rip) wr32(qi + 16, e->gdst);
            fix_ip_csum(qi);
            fix_icmp_csum(l, len - ihl);
        }
        fix_ip_csum(p);
        st_nat_in++;
        to_guest_locked(f, 14 + len);   // consumed even when the queue is full (dropped) (satoru)
        return true;
    }

    // ---- virtqueues --------------------------------------------------------

    void drain_tx() {
        static uint8_t buf[VHDR + FRAME_MAX + 64];
        VirtioChain c;
        int n = 0;
        while (VirtioPop(&g_dev, 1, &c)) {
            uint32_t got = VirtioReadChain(&c, 0, buf, sizeof(buf));
            if (got > (uint32_t)VHDR) guest_frame(buf + VHDR, (int)(got - VHDR));
            VirtioPush(&g_dev, 1, c.head, 0);
            n++;
        }
        if (n) VirtioRaise(&g_dev);
    }

    void fill_rx() {
        if (!VirtioDriverOk(&g_dev)) return;
        static uint8_t buf[VHDR + FRAME_MAX];
        int n = 0;
        while (g_rx_r != g_rx_w && VirtioHasAvail(&g_dev, 0)) {
            Frame& fr = g_rx[g_rx_r % RXQ];
            VirtioChain c;
            if (!VirtioPop(&g_dev, 0, &c)) break;
            memset(buf, 0, VHDR);
            buf[10] = 1;                                   // num_buffers
            memcpy(buf + VHDR, fr.data, fr.len);
            uint32_t want = VHDR + fr.len;
            uint32_t w = (VirtioChainLen(&c, true) >= want) ? VirtioWriteChain(&c, 0, buf, want) : 0;
            if (w) st_rx++; else st_drop_rx++;
            VirtioPush(&g_dev, 0, c.head, w);
            __atomic_store_n(&g_rx_r, g_rx_r + 1, __ATOMIC_RELEASE);
            n++;
        }
        if (n) VirtioRaise(&g_dev);
    }

    void on_notify(VirtioDev* d, int qid) {
        (void)d;
        if (qid == 1) drain_tx();
        else if (qid == 0) fill_rx();
    }
}

bool VNet::Init(uint8_t irq) {
    {
        SpinLockGuard g(g_lock);
        g_active = false;
        for (int i = 0; i < NAT_MAX; i++) g_nat[i].used = false;
        g_rx_r = g_rx_w = 0;
    }
    for (int i = 0; i < 6; i++) g_guest_mac[i] = GUEST_MAC[i];
    st_tx = st_rx = st_drop_tx = st_drop_rx = st_nat_out = st_nat_in = st_dhcp = st_arp = st_full = 0;
    g_dev = {};
    g_dev.name = "virtio-net";
    g_dev.devtype = 1;
    g_dev.pci_class = 0x02; g_dev.pci_subclass = 0x00;
    g_dev.irq = irq;
    g_dev.nqueues = 2;
    g_dev.queue_max = 256;
    g_dev.features = (1ULL << 5) | (1ULL << 16);       // MAC, STATUS
    for (int i = 0; i < 6; i++) g_dev.devcfg[i] = GUEST_MAC[i];
    g_dev.devcfg[6] = 1;                               // VIRTIO_NET_S_LINK_UP
    g_dev.devcfg_len = 12;
    g_dev.notify = on_notify;
    g_up = VirtioRegister(&g_dev);
    if (g_up) {
        g_tcpip_ipv4_rx_hook = rx_hook;
        g_active = true;
    }
    return g_up;
}

void VNet::Shutdown() {
    g_active = false;
    SpinLockGuard g(g_lock);
    for (int i = 0; i < NAT_MAX; i++) g_nat[i].used = false;
    g_rx_r = g_rx_w = 0;
}

void VNet::Poll() {
    if (!g_up) return;
    drain_tx();
    fill_rx();
    uint64_t t = now_ms();
    if (t - g_last_sweep > 1000) { g_last_sweep = t; sweep(); }
}

bool VNet::HasPending() { return g_up && g_rx_r != g_rx_w && VirtioHasAvail(&g_dev, 0); }
uint32_t VNet::GuestIP() { return GUEST_IP; }
uint32_t VNet::GatewayIP() { return GW_IP; }

static int vn_app(char* o, int p, int mx, const char* s) { while (*s && p < mx - 1) o[p++] = *s++; o[p] = 0; return p; }
static int vn_appu(char* o, int p, int mx, uint64_t v) { char t[24]; int k = 0; if (!v) t[k++] = '0'; while (v) { t[k++] = (char)('0' + v % 10); v /= 10; } while (k) { if (p < mx - 1) o[p++] = t[--k]; else --k; } o[p] = 0; return p; }

int VNet::Status(char* out, int mx) {
    int nat = 0;
    for (int i = 0; i < NAT_MAX; i++) if (g_nat[i].used) nat++;
    int p = 0;
    p = vn_app(out, p, mx, "  net: guest 10.0.3.15 gw 10.0.3.1, frames out "); p = vn_appu(out, p, mx, st_tx);
    p = vn_app(out, p, mx, " in "); p = vn_appu(out, p, mx, st_rx);
    p = vn_app(out, p, mx, ", nat out "); p = vn_appu(out, p, mx, st_nat_out);
    p = vn_app(out, p, mx, " in "); p = vn_appu(out, p, mx, st_nat_in);
    p = vn_app(out, p, mx, ", flows "); p = vn_appu(out, p, mx, (uint64_t)nat);
    p = vn_app(out, p, mx, ", dhcp "); p = vn_appu(out, p, mx, st_dhcp);
    p = vn_app(out, p, mx, ", drops tx "); p = vn_appu(out, p, mx, st_drop_tx);
    p = vn_app(out, p, mx, " rx "); p = vn_appu(out, p, mx, st_drop_rx + st_full);
    p = vn_app(out, p, mx, TCPStack::IsUp() ? "\n" : " (kurono has no network)\n");
    return p;
}

// end (satoru)
