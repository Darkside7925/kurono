#include "sntp.h"
#include "network.h"
#include "tcpip.h"
#include "dhcp.h"
#include "../kernel/time.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../proc/scheduler.h"
#include "../proc/spinlock.h"
#include "../proc/kernel_locks.h"

namespace Sntp {

bool g_enabled = true;

namespace {

int64_t  g_last_offset_ms = 0;
uint64_t g_last_sync_ms = 0;

// seconds between 1900-01-01 (ntp era 0) and 1970-01-01. (satoru)
const uint64_t NTP_UNIX_DELTA = 2208988800ull;

uint32_t rd_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// the realtime clock the linux layer and the vdso serve, in ms. (satoru)
uint64_t realtime_ms() {
    return TimeManager::RealtimeBaseSeconds() * 1000ull + Timer::GetRealMs64();
}

void log_ip(uint32_t ip) {
    char b[20];
    TCPStack::FormatIP(ip, b);
    SerialLogger::Log(b);
}

void log_i64(int64_t v) {
    if (v < 0) { SerialLogger::Log("-"); v = -v; } else SerialLogger::Log("+");
    if (v > 2000000000ll) v = 2000000000ll;
    SerialLogger::LogDec((int)v);
}

// one request/response against one server ip. (satoru)
bool query(uint32_t ip, int64_t* offset_ms, int* rtt_ms) {
    int sock;
    {
        SpinLockCpuGuard g(g_net_lock);
        sock = TCPStack::Socket(SOCK_DGRAM);
    }
    if (sock < 0) return false;
    uint8_t pkt[48];
    for (int i = 0; i < 48; i++) pkt[i] = 0;
    pkt[0] = 0x23;   // li 0, version 4, mode 3 (client) (satoru)
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        uint64_t t1 = Timer::GetRealMs64();
        uint64_t r1 = realtime_ms();
        int sent;
        {
            SpinLockCpuGuard g(g_net_lock);
            sent = TCPStack::SendTo(sock, pkt, 48, ip, 123);
        }
        if (sent != 48) { Scheduler::SleepMs(200); continue; }
        while (Timer::GetRealMs64() - t1 < 1500ull) {
            uint8_t resp[96];
            uint32_t from_ip = 0;
            uint16_t from_port = 0;
            int got;
            {
                SpinLockCpuGuard g(g_net_lock);
                got = TCPStack::RecvFrom(sock, resp, sizeof(resp), &from_ip, &from_port);
            }
            if (got <= 0) { Scheduler::SleepMs(10); continue; }
            if (from_port != 123 || got < 48) continue;
            int mode = resp[0] & 7;
            int stratum = resp[1];
            if (mode != 4 || stratum == 0 || stratum > 15) continue;   // not a server reply / kiss-o-death (satoru)
            uint64_t t4 = Timer::GetRealMs64();
            uint32_t tx_s = rd_be32(resp + 40);
            uint32_t tx_f = rd_be32(resp + 44);
            if (tx_s == 0) continue;
            // server transmit time in unix ms. 2036+ era wrap: an era-0 value
            // below the 1970 offset belongs to era 1. (satoru)
            uint64_t s = (uint64_t)tx_s;
            if (s < NTP_UNIX_DELTA) s += 0x100000000ull;
            uint64_t server_ms = (s - NTP_UNIX_DELTA) * 1000ull + (((uint64_t)tx_f * 1000ull) >> 32);
            uint64_t rtt = t4 - t1;
            // the reply left the server about half a round trip ago. (satoru)
            uint64_t local_at_reply = r1 + rtt;
            *offset_ms = (int64_t)(server_ms + rtt / 2) - (int64_t)local_at_reply;
            *rtt_ms = (int)rtt;
            ok = true;
            break;
        }
    }
    {
        SpinLockCpuGuard g(g_net_lock);
        TCPStack::Close(sock);
    }
    return ok;
}

// a tiny dns a-record lookup on our own socket, under g_net_lock like every
// other socket call here. Network::Resolve pumps the ui and ticks the stack
// without the lock from whatever process calls it, which is fine for the shell
// but not for a background service. (satoru)
bool dns_a(const char* host, uint32_t server, uint32_t* out) {
    uint8_t q[300];
    int n = 0;
    uint16_t id = (uint16_t)(Timer::GetRealMs64() * 2654435761ull >> 7);
    q[n++] = (uint8_t)(id >> 8); q[n++] = (uint8_t)id;
    q[n++] = 0x01; q[n++] = 0x00;   // recursion desired (satoru)
    q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0; q[n++] = 0;
    const char* p = host;
    while (*p) {
        const char* dot = p;
        while (*dot && *dot != '.') dot++;
        int len = (int)(dot - p);
        if (len <= 0 || len > 63 || n + len + 6 > (int)sizeof(q)) return false;
        q[n++] = (uint8_t)len;
        for (int i = 0; i < len; i++) q[n++] = (uint8_t)p[i];
        p = *dot ? dot + 1 : dot;
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1;   // type a (satoru)
    q[n++] = 0; q[n++] = 1;   // class in (satoru)
    int sock;
    {
        SpinLockCpuGuard g(g_net_lock);
        sock = TCPStack::Socket(SOCK_DGRAM);
    }
    if (sock < 0) return false;
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        int sent;
        {
            SpinLockCpuGuard g(g_net_lock);
            sent = TCPStack::SendTo(sock, q, n, server, 53);
        }
        if (sent != n) { Scheduler::SleepMs(200); continue; }
        uint64_t t0 = Timer::GetRealMs64();
        while (!ok && Timer::GetRealMs64() - t0 < 1500ull) {
            uint8_t r[512];
            uint32_t fip = 0;
            uint16_t fport = 0;
            int got;
            {
                SpinLockCpuGuard g(g_net_lock);
                got = TCPStack::RecvFrom(sock, r, sizeof(r), &fip, &fport);
            }
            if (got <= 0) { Scheduler::SleepMs(10); continue; }
            if (fport != 53 || got < 12 || r[0] != (uint8_t)(id >> 8) || r[1] != (uint8_t)id) continue;
            if (!(r[2] & 0x80) || (r[3] & 0x0F)) break;   // not a reply, or an error rcode (satoru)
            int an = (r[6] << 8) | r[7];
            int i = n;   // the question is echoed back unchanged (satoru)
            for (int k = 0; k < an && i + 12 <= got; k++) {
                // owner name: a pointer (2 bytes) or labels (satoru)
                while (i < got && r[i] != 0 && (r[i] & 0xC0) != 0xC0) i += r[i] + 1;
                i += (i < got && (r[i] & 0xC0) == 0xC0) ? 2 : 1;
                if (i + 10 > got) break;
                int type = (r[i] << 8) | r[i + 1];
                int rdlen = (r[i + 8] << 8) | r[i + 9];
                i += 10;
                if (type == 1 && rdlen == 4 && i + 4 <= got) {
                    *out = TCPStack::MakeIP(r[i], r[i + 1], r[i + 2], r[i + 3]);
                    ok = true;
                    break;
                }
                i += rdlen;   // a cname and friends (satoru)
            }
            break;
        }
    }
    {
        SpinLockCpuGuard g(g_net_lock);
        TCPStack::Close(sock);
    }
    return ok;
}

bool resolve(const char* host, uint32_t* out) {
    uint32_t servers[3] = { TCPStack::GetDNS(), TCPStack::MakeIP(1, 1, 1, 1), TCPStack::MakeIP(8, 8, 8, 8) };
    for (int i = 0; i < 3; i++) {
        if (!servers[i]) continue;
        if (dns_a(host, servers[i], out)) return true;
    }
    return false;
}

}  // namespace

int64_t LastOffsetMs() { return g_last_offset_ms; }
uint64_t LastSyncUptimeMs() { return g_last_sync_ms; }

bool SyncOnce(int64_t* offset_ms, int* rtt_ms) {
    static const char* const kServers[] = { "pool.ntp.org", "time.google.com", "time.cloudflare.com" };
    for (unsigned i = 0; i < sizeof(kServers) / sizeof(kServers[0]); i++) {
        uint32_t ip = 0;
        if (!resolve(kServers[i], &ip)) {
            SerialLogger::Log("[sntp] cannot resolve ");
            SerialLogger::Log(kServers[i]);
            SerialLogger::Log("\r\n");
            continue;
        }
        int64_t off = 0;
        int rtt = 0;
        if (!query(ip, &off, &rtt)) {
            SerialLogger::Log("[sntp] no reply from ");
            SerialLogger::Log(kServers[i]);
            SerialLogger::Log(" (");
            log_ip(ip);
            SerialLogger::Log(")\r\n");
            continue;
        }
        // the realtime base has whole-second resolution: step it by the rounded
        // offset when we are at least half a second off. (satoru)
        int64_t step_s = (off >= 0 ? off + 500 : off - 500) / 1000;
        if (step_s != 0) TimeManager::StepRealtimeSeconds((int32_t)step_s);
        SerialLogger::Log("[sntp] ");
        SerialLogger::Log(kServers[i]);
        SerialLogger::Log(" (");
        log_ip(ip);
        SerialLogger::Log(") offset=");
        log_i64(off);
        SerialLogger::Log(" ms rtt=");
        SerialLogger::LogDec(rtt);
        SerialLogger::Log(" ms stepped=");
        log_i64(step_s);
        SerialLogger::Log(" s\r\n");
        g_last_offset_ms = off;
        g_last_sync_ms = Timer::GetRealMs64();
        if (offset_ms) *offset_ms = off;
        if (rtt_ms) *rtt_ms = rtt;
        return true;
    }
    return false;
}

void ProcessEntry() {
    SerialLogger::Log(g_enabled ? "[sntp] online\r\n" : "[sntp] off (kurono.ntp=0)\r\n");
    for (;;) {
        if (!g_enabled) { Scheduler::SleepMs(60000); continue; }
        // wait for an address (dhcp lease or a static ip) before asking. (satoru)
        if (!TCPStack::IsUp() || TCPStack::GetIP() == 0) { Scheduler::SleepMs(1000); continue; }
        int64_t off = 0;
        int rtt = 0;
        bool ok = SyncOnce(&off, &rtt);
        // every 4 hours once synced; a failed round retries after 10 s, then
        // backs off to 2 minutes (dns is often not ready right at link-up). (satoru)
        static uint32_t s_fail_wait = 10u * 1000u;
        uint32_t wait_ms = ok ? 4u * 3600u * 1000u : s_fail_wait;
        if (ok) s_fail_wait = 10u * 1000u;
        else if (s_fail_wait < 120u * 1000u) s_fail_wait *= 2;
        for (uint32_t slept = 0; slept < wait_ms; slept += 10000u) Scheduler::SleepMs(10000);
    }
}

}  // namespace Sntp

// end (satoru)
