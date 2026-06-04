#include "tuntap.h"
#include "../drivers/serial.h"

namespace TunTap {

    static Device  g_devs[TUNTAP_MAX_DEVS];
    static int     g_count = 0;

    // ---- inline helpers (no libc) ----
    static void mzero(void* p, unsigned int n) {
        unsigned char* b = (unsigned char*)p;
        for (unsigned int i = 0; i < n; i++) b[i] = 0;
    }
    static void mcopy(void* d, const void* s, unsigned int n) {
        unsigned char* dd = (unsigned char*)d;
        const unsigned char* ss = (const unsigned char*)s;
        for (unsigned int i = 0; i < n; i++) dd[i] = ss[i];
    }
    static unsigned int slen(const char* s) {
        unsigned int n = 0; while (s && s[n]) n++; return n;
    }
    static bool seq(const char* a, const char* b) {
        if (!a || !b) return false;
        while (*a && *b && *a == *b) { a++; b++; }
        return *a == *b;
    }
    static void scopy(char* d, const char* s, int max) {
        int i = 0;
        while (s && s[i] && i < max - 1) { d[i] = s[i]; i++; }
        d[i] = 0;
    }
    static void itoa10(unsigned int v, char* out) {
        char tmp[12]; int n = 0;
        if (v == 0) { out[0] = '0'; out[1] = 0; return; }
        while (v && n < 11) { tmp[n++] = '0' + (v % 10); v /= 10; }
        for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
        out[n] = 0;
    }

    void Init() {
        mzero(g_devs, sizeof(g_devs));
        g_count = 0;
        SerialLogger::Log("TunTap: subsystem initialized\r\n");
    }

    int Count() { return g_count; }

    Device* Get(int dev_id) {
        if (dev_id < 0 || dev_id >= TUNTAP_MAX_DEVS) return nullptr;
        if (!g_devs[dev_id].in_use) return nullptr;
        return &g_devs[dev_id];
    }

    static int find_by_name(const char* name) {
        if (!name || !name[0]) return -1;
        for (int i = 0; i < TUNTAP_MAX_DEVS; i++) {
            if (g_devs[i].in_use && seq(g_devs[i].name, name)) return i;
        }
        return -1;
    }

    static int alloc_slot() {
        for (int i = 0; i < TUNTAP_MAX_DEVS; i++) {
            if (!g_devs[i].in_use) return i;
        }
        return -1;
    }

    int Open(const char* req_name, bool tap, bool persist, unsigned int flags) {
        // Reuse persistent device if name matches.
        int existing = find_by_name(req_name);
        if (existing >= 0) {
            g_devs[existing].refcount++;
            return existing;
        }

        int slot = alloc_slot();
        if (slot < 0) return -1;
        Device& d = g_devs[slot];
        mzero(&d, sizeof(d));
        d.in_use = true;
        d.is_tap = tap;
        d.persist = persist;
        d.up = false;
        d.refcount = 1;
        d.flags = flags;

        // Auto-name if caller passed empty: tunN / tapN.
        if (!req_name || !req_name[0]) {
            int n = 0;
            for (int i = 0; i < TUNTAP_MAX_DEVS; i++) {
                if (g_devs[i].in_use && i != slot && g_devs[i].is_tap == tap) n++;
            }
            char num[12]; itoa10((unsigned)n, num);
            const char* prefix = tap ? "tap" : "tun";
            int p = 0; while (prefix[p]) { d.name[p] = prefix[p]; p++; }
            int q = 0; while (num[q] && p < 15) { d.name[p++] = num[q++]; }
            d.name[p] = 0;
        } else {
            scopy(d.name, req_name, 16);
        }

        // Default MAC for TAP: locally-administered, derived from slot index.
        if (tap) {
            d.mac.bytes[0] = 0x02;
            d.mac.bytes[1] = 0x00;
            d.mac.bytes[2] = 0x00;
            d.mac.bytes[3] = 0x00;
            d.mac.bytes[4] = 0x00;
            d.mac.bytes[5] = (unsigned char)slot;
        }

        if (slot >= g_count) g_count = slot + 1;

        char log[80]; int li = 0;
        const char* hdr = "TunTap: opened ";
        while (hdr[li - 0]) { log[li] = hdr[li]; li++; }
        int j = 0; while (d.name[j] && li < 70) { log[li++] = d.name[j++]; }
        log[li++] = '\r'; log[li++] = '\n'; log[li] = 0;
        SerialLogger::Log(log);
        return slot;
    }

    void Close(int dev_id) {
        Device* d = Get(dev_id);
        if (!d) return;
        if (--d->refcount > 0) return;
        // Best-effort drain of pending TX so we don't lose in-flight
        // packets on the last close. Skipped if the device is down or
        // there's no link below us.
        if (d->up) {
            while (d->tx_tail != d->tx_head) {
                Packet& p = d->tx_ring[d->tx_tail % TUNTAP_RING_SLOTS];
                if (!Network::SendPacket("eth0", p.data, p.len)) break;
                d->tx_tail++;
            }
        }
        if (d->persist) {
            // keep allocation, but reset volatile per-fd state so the next
            // open starts cleanly.
            d->refcount = 0;
            d->rx_head = d->rx_tail = 0;
            d->tx_head = d->tx_tail = 0;
            return;
        }
        mzero(d, sizeof(*d));
    }

    int Read(int dev_id, unsigned char* buf, int max) {
        Device* d = Get(dev_id);
        if (!d || !buf || max <= 0) return -1;
        if (d->rx_head == d->rx_tail) return 0;          // empty
        Packet& p = d->rx_ring[d->rx_tail % TUNTAP_RING_SLOTS];
        int n = p.len;
        if (n > max) n = max;
        mcopy(buf, p.data, (unsigned int)n);
        d->rx_tail++;
        d->rx_packets++;
        d->rx_bytes += (unsigned)n;
        return n;
    }

    int Write(int dev_id, const unsigned char* buf, int len) {
        Device* d = Get(dev_id);
        if (!d || !buf || len <= 0 || len > TUNTAP_FRAME_MAX) return -1;
        unsigned int slots_used = d->tx_head - d->tx_tail;
        if (slots_used >= TUNTAP_RING_SLOTS) {
            d->tx_drops++;
            return -1;                                    // would block
        }
        Packet& p = d->tx_ring[d->tx_head % TUNTAP_RING_SLOTS];
        p.len = (unsigned short)len;
        mcopy(p.data, buf, (unsigned int)len);
        d->tx_head++;
        d->tx_packets++;
        d->tx_bytes += (unsigned)len;
        return len;
    }

    bool Inject(int dev_id, const unsigned char* buf, int len) {
        Device* d = Get(dev_id);
        if (!d || !buf || len <= 0 || len > TUNTAP_FRAME_MAX) return false;
        unsigned int slots_used = d->rx_head - d->rx_tail;
        if (slots_used >= TUNTAP_RING_SLOTS) {
            d->rx_drops++;
            return false;
        }
        Packet& p = d->rx_ring[d->rx_head % TUNTAP_RING_SLOTS];
        p.len = (unsigned short)len;
        mcopy(p.data, buf, (unsigned int)len);
        d->rx_head++;
        return true;
    }

    void Tick() {
        // Drain each device's tx_ring with a per-tick budget so a busy
        // VPN daemon can't starve the rest of the kernel main loop. If
        // the downstream NIC refuses a packet we leave it queued (head
        // not advanced) so the next Tick retries  -  this is what gives
        // Write() real backpressure: tx_ring fills up and Write() then
        // returns -1 instead of silently dropping.
        const int kTickBudget = TUNTAP_RING_SLOTS;
        for (int i = 0; i < TUNTAP_MAX_DEVS; i++) {
            Device& d = g_devs[i];
            if (!d.in_use || !d.up) continue;
            int sent_this_tick = 0;
            while (d.tx_tail != d.tx_head && sent_this_tick < kTickBudget) {
                Packet& p = d.tx_ring[d.tx_tail % TUNTAP_RING_SLOTS];
                if (!Network::SendPacket("eth0", p.data, p.len)) {
                    // Underlying NIC is congested. Stop draining; the
                    // queued packet stays in the ring and Write() will
                    // start failing as the ring fills  -  true backpressure.
                    break;
                }
                d.tx_tail++;
                sent_this_tick++;
            }

            NetworkInterface* nif = Network::GetInterface(d.name);
            if (nif) {
                nif->rx_packets = d.rx_packets;
                nif->tx_packets = d.tx_packets;
                nif->rx_bytes   = d.rx_bytes;
                nif->tx_bytes   = d.tx_bytes;
                nif->rx_errors  = d.rx_drops;
                nif->tx_errors  = d.tx_drops;
            }
        }
    }

    bool SetUp(int dev_id, bool up) {
        Device* d = Get(dev_id);
        if (!d) return false;
        d->up = up;
        return true;
    }

    bool SetIP(int dev_id, IPv4Address ip, IPv4Address mask) {
        Device* d = Get(dev_id);
        if (!d) return false;
        d->ip = ip;
        d->netmask = mask;
        return true;
    }

    bool SetMAC(int dev_id, const MACAddress& mac) {
        Device* d = Get(dev_id);
        if (!d) return false;
        for (int i = 0; i < 6; i++) d->mac.bytes[i] = mac.bytes[i];
        return true;
    }
}
