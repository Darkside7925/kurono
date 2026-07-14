#include "tcpip.h"
#include "ipv6.h"
#include "../drivers/e1000.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../kernel/heap.h"
#include "../kernel/time.h"
#include "../shell/shell.h"
#include "../proc/scheduler.h"    // cooperative yield in network wait loops (satoru)
#include "../system/logging.h"   // notable net events -> /kurono/var/log/network.log (satoru)

// tiny helpers for serial diag
static void slog(const char* s){ SerialLogger::Log(s); }

/* Cooperative ~1ms throttle inside network wait loops. The previous
   `for (volatile d=0;d<100;d++)` and `pause` spins called Tick + PumpUI
   thousands of times per millisecond, starving the kernel main loop and
   the E1000 poll path. Now we YIELD for ~1ms instead of busy-spinning
   `pause`, so other cooperative processes actually run during the wait.
   Scheduler::SleepMs(1) gives up the cpu to the next runnable kernel
   process when the preemptive scheduler is live, and otherwise HLTs until
   the next IRQ (still no busy spin) - both keep the ~1ms cadence and stop
   burning a core. The callers re-poll the NIC via Tick() each pass, so
   correctness holds. Matches the recv-loop pacing in linux_cmds.cpp. (satoru) */
static inline void net_wait_one_ms() {
    Scheduler::SleepMs(1);
}

// copy `n` bytes from `src` into the socket rx ring at rx_tail, advancing
// rx_tail (mod TCP_RX_BUFSIZE) and rx_count. uses at most two bulk memcpy spans
// (the region before the wrap and the region after) instead of the old per-byte
// loop, so a full-mss segment lands at memory bandwidth. caller has already
// clamped n to the free space. (satoru)
static inline void tcp_ring_push(NetSocket* s, const uint8_t* src, int n) {
    if (n <= 0) return;
    int tail = s->rx_tail;
    int first = TCP_RX_BUFSIZE - tail;        // bytes until the ring wraps (satoru)
    if (first > n) first = n;
    memcpy(s->rx_buf + tail, src, (size_t)first);
    int rem = n - first;
    if (rem > 0) memcpy(s->rx_buf, src + first, (size_t)rem);
    s->rx_tail = (tail + n) % TCP_RX_BUFSIZE;
    s->rx_count += n;
}

// pop n bytes from the rx ring into dst (or discard when dst is null),
// wrap-aware, advancing rx_head. caller has verified rx_count >= n. the udp
// datagram-framing path uses this to pop one record at a time. (satoru)
static inline void tcp_ring_pop(NetSocket* s, uint8_t* dst, int n) {
    if (n <= 0) return;
    int head = s->rx_head;
    int first = TCP_RX_BUFSIZE - head;
    if (first > n) first = n;
    if (dst) memcpy(dst, s->rx_buf + head, (size_t)first);
    int rem = n - first;
    if (rem > 0 && dst) memcpy(dst + first, s->rx_buf, (size_t)rem);
    s->rx_head = (head + n) % TCP_RX_BUFSIZE;
    s->rx_count -= n;
}

// udp datagram record header pushed ahead of each payload in the rx ring so
// RecvFrom returns DISTINCT datagrams with their true source. without framing
// the ring concatenated datagrams and remote_ip/port were clobbered per packet
// - musl's resolver (parallel A+AAAA on one socket, memcmp on the reply
// source) breaks on both. (satoru)
struct UdpRecHdr {
    uint16_t len;        // payload bytes following this header (satoru)
    uint16_t src_port;   // datagram source port (host order) (satoru)
    uint32_t src_ip;     // datagram source ip (host order) (satoru)
} __attribute__((packed));
static void sloghex(uint32_t v){
    char b[12]; b[0]='0'; b[1]='x';
    const char* h = "0123456789ABCDEF";
    for (int i=0;i<8;i++) b[2+i]=h[(v>>((7-i)*4))&0xF];
    b[10]='\r'; b[11]='\n';
    char b2[13]; for(int i=0;i<12;i++) b2[i]=b[i]; b2[12]=0;
    SerialLogger::Log(b2);
}
static void slognum(int v){
    char b[16]; int n=0; if(v<0){ b[n++]='-'; v=-v; }
    char t[12]; int ti=0; if(v==0) t[ti++]='0';
    while(v){ t[ti++]=(char)('0'+(v%10)); v/=10; }
    while(ti) b[n++]=t[--ti];
    b[n++]='\r'; b[n++]='\n'; b[n]=0;
    SerialLogger::Log(b);
}
bool TCPStack::initialized = false;
uint32_t TCPStack::local_ip = 0;
uint32_t TCPStack::subnet_mask = 0;
uint32_t TCPStack::gateway = 0;
uint32_t TCPStack::dns_server = 0;
uint8_t TCPStack::mac[6] = {};

ARPEntry TCPStack::arp_cache[ARP_CACHE_SIZE] = {};
NetSocket TCPStack::sockets[MAX_SOCKETS] = {};
NetStats TCPStack::stats = {};
PendingIPv4Frame TCPStack::pending_ipv4[PENDING_IPV4_TX] = {};

uint16_t TCPStack::next_ephemeral_port = 49152;
uint16_t TCPStack::ip_ident = 1;

namespace {
bool     g_ping_waiting = false;
bool     g_ping_reply_ready = false;
uint16_t g_ping_expect_id = 0;
uint16_t g_ping_expect_seq = 0;
uint16_t g_ping_next_id = 0x1234;
uint16_t g_ping_next_seq = 1;
uint32_t g_ping_expect_ip = 0;
uint32_t g_ping_sent_ms = 0;
uint32_t g_ping_rtt_ms = 0;

bool SeqBefore(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

bool SeqAfter(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

bool BytesEqual(const uint8_t* a, const uint8_t* b, int len) {
    for (int i = 0; i < len; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

void ApplyAck(NetSocket* sock, uint32_t ack_seq) {
    if (!sock) return;
    if (SeqBefore(ack_seq, sock->tx_unacked) || SeqAfter(ack_seq, sock->tcp_seq)) {
        return;
    }

    // fast-retransmit (rfc 5681): a pure dup-ack is one that does NOT advance
    // tx_unacked while we still have unacked data outstanding. count them; on the
    // 3rd, resend the oldest still-in_use scoreboard segment immediately instead
    // of waiting for its rto. a NEW ack (advancing) resets the counter. (satoru)
    if (ack_seq == sock->tx_unacked && sock->tx_seg_inflight > 0) {
        if (ack_seq != sock->last_ack_seen) { sock->last_ack_seen = ack_seq; sock->dup_ack_cnt = 1; }
        else if (sock->dup_ack_cnt < 255)   { sock->dup_ack_cnt++; }
        if (sock->dup_ack_cnt == 3) {
            // resend the oldest unacked segment on the NEXT TCPTick (same
            // NetworkProcess loop, ~10ms) rather than its 500ms-4s rto: age its
            // last_tx_ms past any rto so TCPTick's per-segment check fires it.
            // (SendTCPPacket is a private TCPStack member; ApplyAck is a free
            // helper, so we signal via the scoreboard the tick already scans.) (satoru)
            TxDataSeg* oldest = nullptr;
            for (int i = 0; i < TCP_SND_WND_SEGS; i++) {
                TxDataSeg* s = &sock->tx_segs[i];
                if (!s->in_use) continue;
                if (!oldest || SeqBefore(s->seq, oldest->seq)) oldest = s;
            }
            if (oldest) oldest->last_tx_ms = Timer::GetTicks() - 5000u;   // > max rto -> fires next tick (satoru)
        }
        return;   // a dup-ack acks no new data - nothing below to free (satoru)
    }
    sock->last_ack_seen = ack_seq;
    sock->dup_ack_cnt   = 0;

    sock->tx_unacked = ack_seq;
    if (sock->tx_pending && !SeqBefore(ack_seq, sock->tx_seq_end)) {
        sock->tx_pending = false;
        sock->tx_retries = 0;
    }

    // free every send-window data segment the peer has cumulatively acked. a
    // segment is fully acked once ack_seq has advanced to or past its last
    // byte (seq + len). this opens slots for Send to keep the pipe full and
    // stops TCPTick retransmitting acked data. (satoru)
    for (int i = 0; i < TCP_SND_WND_SEGS; i++) {
        TxDataSeg* seg = &sock->tx_segs[i];
        if (!seg->in_use) continue;
        if (!SeqBefore(ack_seq, seg->seq + (uint32_t)seg->len)) {
            seg->in_use = false;
            if (sock->tx_seg_inflight > 0) sock->tx_seg_inflight--;
        }
    }
}
}

uint16_t TCPStack::htons(uint16_t val) {
    return ((val & 0xFF) << 8) | ((val >> 8) & 0xFF);
}

uint32_t TCPStack::htonl(uint32_t val) {
    return ((val & 0xFF) << 24) | (((val >> 8) & 0xFF) << 16) |
           (((val >> 16) & 0xFF) << 8) | ((val >> 24) & 0xFF);
}

uint16_t TCPStack::ntohs(uint16_t val) { return htons(val); }
uint32_t TCPStack::ntohl(uint32_t val) { return htonl(val); }

static uint32_t ChecksumAccumulate(const void* data, int len, uint32_t sum = 0) {
    const uint8_t* bytes = (const uint8_t*)data;
    while (len > 1) {
        sum += ((uint32_t)bytes[0] << 8) | (uint32_t)bytes[1];
        bytes += 2;
        len -= 2;
    }
    if (len == 1)
        sum += (uint32_t)bytes[0] << 8;
    return sum;
}

static uint16_t ChecksumFinish(uint32_t sum) {
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

uint32_t TCPStack::MakeIP(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
}

void TCPStack::FormatIP(uint32_t ip, char* out) {
    auto write_num = [&](uint8_t val) {
        if (val >= 100) { *out++ = '0' + (val / 100); val %= 100; *out++ = '0' + (val / 10); val %= 10; }
        else if (val >= 10) { *out++ = '0' + (val / 10); val %= 10; }
        *out++ = '0' + val;
    };
    write_num((ip >> 24) & 0xFF); *out++ = '.';
    write_num((ip >> 16) & 0xFF); *out++ = '.';
    write_num((ip >> 8) & 0xFF);  *out++ = '.';
    write_num(ip & 0xFF);
    *out = 0;
}

uint16_t TCPStack::Checksum(const void* data, int len) {
    return ChecksumFinish(ChecksumAccumulate(data, len));
}

uint16_t TCPStack::TCPChecksum(const IPv4Header* ip, const TCPHeader* tcp,
                               const void* payload, int payload_len) {
    // pseudo header: src_ip(4) + dst_ip(4) + zero(1) + proto(1) + tcp_len(2)
    struct {
        uint32_t src_ip;
        uint32_t dst_ip;
        uint8_t  zero;
        uint8_t  protocol;
        uint16_t tcp_length;
    } __attribute__((packed)) pseudo;

    int tcp_header_len = ((tcp->data_offset >> 4) & 0xF) * 4;
    if (tcp_header_len < (int)sizeof(TCPHeader))
        tcp_header_len = sizeof(TCPHeader);
    int tcp_total = tcp_header_len + payload_len;
    pseudo.src_ip = ip->src_ip;
    pseudo.dst_ip = ip->dst_ip;
    pseudo.zero = 0;
    pseudo.protocol = IP_PROTO_TCP;
    pseudo.tcp_length = htons(tcp_total);

    uint32_t sum = 0;
    sum = ChecksumAccumulate(&pseudo, sizeof(pseudo), sum);
    sum = ChecksumAccumulate(tcp, tcp_header_len, sum);
    sum = ChecksumAccumulate(payload, payload_len, sum);
    return ChecksumFinish(sum);
}

bool TCPStack::Init() {
    initialized = false;

    for (int i = 0; i < MAX_SOCKETS; i++) sockets[i].active = false;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) arp_cache[i].valid = false;
    for (int i = 0; i < PENDING_IPV4_TX; i++) pending_ipv4[i].active = false;

    stats = {};

    // check if e1000 is available
    if (!E1000::IsDetected()) return false;

    // get mac address from e1000
    E1000::GetMAC(mac);

    // default ip config (can be changed later via dhcp or static)
    local_ip = MakeIP(10, 0, 2, 15);
    subnet_mask = MakeIP(255, 255, 255, 0);
    gateway = MakeIP(10, 0, 2, 2);
    dns_server = MakeIP(10, 0, 2, 3);

    // seed the ephemeral source port from rdtsc so successive boots (and
    // reconnects to the same server) don't reuse the identical 4-tuple - a
    // server still holding TIME_WAIT state for the old tuple answers a fresh
    // syn with a bare challenge-ack instead of syn-ack (rfc 5961), so the
    // handshake never completes. (satoru)
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    next_ephemeral_port = (uint16_t)(49152u + ((lo ^ hi) % (65535u - 49152u)));

    initialized = true;
    return true;
}

bool TCPStack::IsUp() { return initialized && E1000::IsDetected() && E1000::IsLinkUp(); }

void TCPStack::SetIP(uint32_t ip) { local_ip = ip; }
void TCPStack::SetSubnetMask(uint32_t mask) { subnet_mask = mask; }
void TCPStack::SetGateway(uint32_t gw) { gateway = gw; }
void TCPStack::SetDNS(uint32_t dns) { dns_server = dns; }
uint32_t TCPStack::GetIP() { return local_ip; }
uint32_t TCPStack::GetSubnetMask() { return subnet_mask; }
uint32_t TCPStack::GetGateway() { return gateway; }
const uint8_t* TCPStack::GetMAC() { return mac; }

bool TCPStack::ARPLookup(uint32_t ip, uint8_t* mac_out) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            for (int j = 0; j < 6; j++) mac_out[j] = arp_cache[i].mac[j];
            return true;
        }
    }
    return false;
}

void TCPStack::ARPRequest(uint32_t ip) {
    uint8_t frame[ETH_HLEN + sizeof(ARPHeader)];
    EthernetHeader* eth = (EthernetHeader*)frame;
    ARPHeader* arp = (ARPHeader*)(frame + ETH_HLEN);

    // broadcast mac
    for (int i = 0; i < 6; i++) eth->dst_mac[i] = 0xFF;
    for (int i = 0; i < 6; i++) eth->src_mac[i] = mac[i];
    eth->ethertype = htons(ETH_TYPE_ARP);

    arp->hw_type = htons(1);
    arp->proto_type = htons(0x0800);
    arp->hw_len = 6;
    arp->proto_len = 4;
    arp->opcode = htons(ARP_OP_REQUEST);
    for (int i = 0; i < 6; i++) arp->sender_mac[i] = mac[i];
    arp->sender_ip[0] = (local_ip >> 24) & 0xFF;
    arp->sender_ip[1] = (local_ip >> 16) & 0xFF;
    arp->sender_ip[2] = (local_ip >> 8) & 0xFF;
    arp->sender_ip[3] = local_ip & 0xFF;
    for (int i = 0; i < 6; i++) arp->target_mac[i] = 0;
    arp->target_ip[0] = (ip >> 24) & 0xFF;
    arp->target_ip[1] = (ip >> 16) & 0xFF;
    arp->target_ip[2] = (ip >> 8) & 0xFF;
    arp->target_ip[3] = ip & 0xFF;

    E1000::Send(frame, sizeof(frame));
    stats.arp_requests++;
    stats.packets_tx++;
}

void TCPStack::ProcessARP(const void* data, int len) {
    if (len < (int)sizeof(ARPHeader)) {
        slog("[ARP] short frame, dropping\r\n");
        return;
    }
    const ARPHeader* arp = (const ARPHeader*)data;

    uint32_t sender_ip = ((uint32_t)arp->sender_ip[0] << 24) |
                         ((uint32_t)arp->sender_ip[1] << 16) |
                         ((uint32_t)arp->sender_ip[2] << 8) |
                         arp->sender_ip[3];
    uint32_t target_ip = ((uint32_t)arp->target_ip[0] << 24) |
                         ((uint32_t)arp->target_ip[1] << 16) |
                         ((uint32_t)arp->target_ip[2] << 8) |
                         arp->target_ip[3];

    uint16_t op = ntohs(arp->opcode);
    slog(op == ARP_OP_REQUEST ? "[ARP] REQUEST sender="
                              : (op == ARP_OP_REPLY ? "[ARP] REPLY   sender="
                                                    : "[ARP] op=?    sender="));
    sloghex(sender_ip);
    slog("[ARP]   target=");
    sloghex(target_ip);

    // update arp cache with sender info
    bool found = false;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == sender_ip) {
            for (int j = 0; j < 6; j++) arp_cache[i].mac[j] = arp->sender_mac[j];
            arp_cache[i].timestamp = Time::GetTicks();
            found = true;
            break;
        }
    }
    if (!found) {
        for (int i = 0; i < ARP_CACHE_SIZE; i++) {
            if (!arp_cache[i].valid) {
                arp_cache[i].ip = sender_ip;
                for (int j = 0; j < 6; j++) arp_cache[i].mac[j] = arp->sender_mac[j];
                arp_cache[i].valid = true;
                arp_cache[i].timestamp = Time::GetTicks();
                break;
            }
        }
    }

    // we just learned sender_ip's MAC: flush every packet queued on it AT ONCE,
    // exactly when the resolution lands (linux neigh_update drains arp_queue on
    // the reply) rather than waiting for the next 10ms poll. this is what makes a
    // burst of parallel connects go out the instant the gateway answers. (satoru)
    ServicePendingIPv4();

    // reply to requests for our ip
    if (ntohs(arp->opcode) == ARP_OP_REQUEST && target_ip == local_ip) {
        uint8_t frame[ETH_HLEN + sizeof(ARPHeader)];
        EthernetHeader* eth = (EthernetHeader*)frame;
        ARPHeader* reply = (ARPHeader*)(frame + ETH_HLEN);

        for (int i = 0; i < 6; i++) eth->dst_mac[i] = arp->sender_mac[i];
        for (int i = 0; i < 6; i++) eth->src_mac[i] = mac[i];
        eth->ethertype = htons(ETH_TYPE_ARP);

        reply->hw_type = htons(1);
        reply->proto_type = htons(0x0800);
        reply->hw_len = 6;
        reply->proto_len = 4;
        reply->opcode = htons(ARP_OP_REPLY);
        for (int i = 0; i < 6; i++) reply->sender_mac[i] = mac[i];
        reply->sender_ip[0] = (local_ip >> 24) & 0xFF;
        reply->sender_ip[1] = (local_ip >> 16) & 0xFF;
        reply->sender_ip[2] = (local_ip >> 8) & 0xFF;
        reply->sender_ip[3] = local_ip & 0xFF;
        for (int i = 0; i < 6; i++) reply->target_mac[i] = arp->sender_mac[i];
        for (int i = 0; i < 4; i++) reply->target_ip[i] = arp->sender_ip[i];

        E1000::Send(frame, sizeof(frame));
        stats.arp_replies++;
        stats.packets_tx++;
    }
}

bool TCPStack::SendEthernet(const uint8_t* dst_mac, uint16_t ethertype,
                           const void* payload, int len) {
    if (len > ETH_MTU) {
        slog("[ETH:TX] reject: payload > ETH_MTU\r\n");
        return false;
    }
    if (!E1000::IsDetected() || !E1000::IsLinkUp()) {
        slog("[ETH:TX] reject: link down\r\n");
        stats.errors_tx++;
        return false;
    }

    uint8_t frame[ETH_FRAME_MAX];
    EthernetHeader* eth = (EthernetHeader*)frame;

    for (int i = 0; i < 6; i++) eth->dst_mac[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) eth->src_mac[i] = mac[i];
    eth->ethertype = htons(ethertype);

    const uint8_t* src = (const uint8_t*)payload;
    for (int i = 0; i < len; i++) frame[ETH_HLEN + i] = src[i];

    bool ok = E1000::Send(frame, ETH_HLEN + len);
    if (!ok) {
        slog("[ETH:TX] E1000::Send returned FALSE\r\n");
        stats.errors_tx++;
        return false;
    }
    stats.packets_tx++;
    stats.bytes_tx += ETH_HLEN + len;
    return true;
}

bool TCPStack::SendIPv4(uint32_t dst_ip, uint8_t proto, const void* payload, int len) {
    if (len < 0 || len > (int)(ETH_MTU - sizeof(IPv4Header))) {
        slog("[IPv4:TX] reject: payload too large\r\n");
        stats.errors_tx++;
        return false;
    }
    static int s_send_log = 0;
    if (s_send_log++ < 10) {
        slog("[IPv4:TX] dst=");
        sloghex(dst_ip);
        slog("[IPv4:TX]   proto=");
        slognum((int)proto);
        slog("[IPv4:TX]   plen=");
        slognum(len);
    }
    uint8_t pkt[ETH_MTU];
    IPv4Header* ip = (IPv4Header*)pkt;

    ip->ver_ihl = 0x45; // ipv4, 5 words header
    ip->tos = 0;
    ip->total_length = htons(sizeof(IPv4Header) + len);
    ip->identification = htons(ip_ident++);
    ip->flags_fragment = 0;
    ip->ttl = 64;
    ip->protocol = proto;
    ip->checksum = 0;
    ip->src_ip = htonl(local_ip);
    ip->dst_ip = htonl(dst_ip);

    ip->checksum = htons(Checksum(ip, sizeof(IPv4Header)));

    // copy payload
    const uint8_t* src = (const uint8_t*)payload;
    for (int i = 0; i < len && i < (int)(ETH_MTU - sizeof(IPv4Header)); i++)
        pkt[sizeof(IPv4Header) + i] = src[i];

    // determine next-hop ip
    uint32_t next_hop = dst_ip;
    if ((dst_ip & subnet_mask) != (local_ip & subnet_mask))
        next_hop = gateway;

    // arp lookup (with proper RX polling so the reply actually gets processed)
    uint8_t dst_mac[6];
    if (!ARPLookup(next_hop, dst_mac)) {
        return QueuePendingIPv4(next_hop, pkt, sizeof(IPv4Header) + len);
    }

    return SendEthernet(dst_mac, ETH_TYPE_IPV4, pkt, sizeof(IPv4Header) + len);
}

bool TCPStack::QueuePendingIPv4(uint32_t next_hop, const void* packet, int len) {
    if (!packet || len <= 0 || len > ETH_MTU) {
        stats.errors_tx++;
        return false;
    }

    const uint8_t* bytes = (const uint8_t*)packet;
    for (int i = 0; i < PENDING_IPV4_TX; i++) {
        if (!pending_ipv4[i].active) continue;
        if (pending_ipv4[i].next_hop != next_hop || pending_ipv4[i].len != (uint16_t)len) continue;
        if (!BytesEqual(pending_ipv4[i].packet, bytes, len)) continue;
        return true;
    }

    // one probe per next-hop (linux neighbour model): if another packet is
    // already waiting on THIS next_hop's arp, don't fire a second request - just
    // queue behind it. only the first packet to an unresolved next-hop probes;
    // the reply (ProcessARP) flushes the whole backlog at once. (satoru)
    bool probe_in_flight = false;
    for (int i = 0; i < PENDING_IPV4_TX; i++) {
        if (pending_ipv4[i].active && pending_ipv4[i].next_hop == next_hop) { probe_in_flight = true; break; }
    }

    for (int i = 0; i < PENDING_IPV4_TX; i++) {
        if (pending_ipv4[i].active) continue;
        pending_ipv4[i].active = true;
        pending_ipv4[i].next_hop = next_hop;
        pending_ipv4[i].len = (uint16_t)len;
        pending_ipv4[i].last_arp_ms = Timer::GetTicks();
        pending_ipv4[i].arp_retries = 1;
        for (int j = 0; j < len; j++) pending_ipv4[i].packet[j] = bytes[j];
        if (!probe_in_flight) ARPRequest(next_hop);   // coalesce: one arp per next-hop (satoru)
        return true;
    }

    slog("[IPv4:TX] pending ARP queue full\r\n");
    stats.errors_tx++;
    return false;
}

void TCPStack::ServicePendingIPv4() {
    uint8_t dst_mac[6];
    uint32_t now_ms = Timer::GetTicks();
    for (int i = 0; i < PENDING_IPV4_TX; i++) {
        PendingIPv4Frame* entry = &pending_ipv4[i];
        if (!entry->active) continue;

        if (ARPLookup(entry->next_hop, dst_mac)) {
            SendEthernet(dst_mac, ETH_TYPE_IPV4, entry->packet, entry->len);
            entry->active = false;
            continue;
        }

        if ((uint32_t)(now_ms - entry->last_arp_ms) < 250u) {
            continue;
        }

        if (entry->arp_retries >= 8) {
            slog("[IPv4:TX] dropping packet after ARP retries\r\n");
            entry->active = false;
            stats.errors_tx++;
            continue;
        }

        entry->arp_retries++;
        entry->last_arp_ms = now_ms;
        ARPRequest(entry->next_hop);
    }
}

void TCPStack::ProcessRxPacket(const void* data, int length) {
    if (length < ETH_HLEN) return;

    stats.packets_rx++;
    stats.bytes_rx += length;

    const EthernetHeader* eth = (const EthernetHeader*)data;
    const uint8_t* payload = (const uint8_t*)data + ETH_HLEN;
    int payload_len = length - ETH_HLEN;

    uint16_t ethertype = ntohs(eth->ethertype);

    switch (ethertype) {
        case ETH_TYPE_ARP:
            ProcessARP(payload, payload_len);
            break;
        case ETH_TYPE_IPV4:
            ProcessIPv4(payload, payload_len);
            break;
        case ETH_TYPE_IPV6:
            // Hand off to the IPv6 stack - it expects the full ethernet
            // frame so it can echo MACs back on replies.
            IPv6::ProcessRx((const unsigned char*)data, length,
                            eth->dst_mac, eth->src_mac);
            break;
        default: {
            static int s_unk_log = 0;
            if (s_unk_log++ < 4) {
                slog("[ETH:RX] unknown ethertype=");
                sloghex((uint32_t)ethertype);
            }
            stats.dropped++;
            break;
        }
    }
}

void TCPStack::ProcessIPv4(const void* data, int len) {
    if (len < (int)sizeof(IPv4Header)) {
        slog("[IPv4] short frame, dropping\r\n");
        return;
    }

    const IPv4Header* ip = (const IPv4Header*)data;
    int ihl = (ip->ver_ihl & 0x0F) * 4;
    if (ihl < 20 || len < ihl) {
        slog("[IPv4] bad ihl, dropping\r\n");
        return;
    }

    if (Checksum(ip, ihl) != 0) {
        slog("[IPv4] bad checksum, dropping\r\n");
        stats.errors_rx++;
        return;
    }

    // Fragmentation: drop fragments cleanly (no reassembly window). The
    // raw bits we care about are the MF flag and any non-zero fragment
    // offset; the upper 3 flag bits live above the offset.
    uint16_t flags_frag = ntohs(ip->flags_fragment);
    uint16_t frag_off   = flags_frag & 0x1FFFu;
    bool mf             = (flags_frag & 0x2000u) != 0;
    if (mf || frag_off != 0) {
        stats.dropped++;
        return;
    }

    const uint8_t* payload = (const uint8_t*)data + ihl;
    int total_len = (int)ntohs(ip->total_length);
    if (total_len < ihl || total_len > len) total_len = len;
    int payload_len = total_len - ihl;
    if (payload_len < 0) payload_len = 0;

    // verbose: first ~10 IPv4 packets
    static int s_ipv4_log = 0;
    if (s_ipv4_log++ < 10) {
        slog("[IPv4] RX src=");
        sloghex(ntohl(ip->src_ip));
        slog("[IPv4]    dst=");
        sloghex(ntohl(ip->dst_ip));
        slog("[IPv4]    proto=");
        slognum((int)ip->protocol);
        slog("[IPv4]    plen=");
        slognum(payload_len);
    }

    switch (ip->protocol) {
        case IP_PROTO_ICMP:
            ProcessICMP(ip, payload, payload_len);
            break;
        case IP_PROTO_TCP:
            ProcessTCP(ip, payload, payload_len);
            break;
        case IP_PROTO_UDP:
            ProcessUDP(ip, payload, payload_len);
            break;
        default:
            slog("[IPv4] unknown proto, dropping\r\n");
            stats.dropped++;
            break;
    }
}

void TCPStack::ProcessICMP(const IPv4Header* ip_hdr, const void* data, int len) {
    if (len < (int)sizeof(ICMPHeader)) return;
    stats.icmp_rx++;

    const ICMPHeader* icmp = (const ICMPHeader*)data;
    uint32_t sender_ip = ntohl(ip_hdr->src_ip);

    if (icmp->type == ICMP_TYPE_ECHO_REQUEST) {
        // send echo reply
        uint8_t reply_buf[ETH_MTU];
        int copy_len = len;
        if (copy_len > (int)(ETH_MTU - sizeof(IPv4Header))) copy_len = ETH_MTU - sizeof(IPv4Header);

        // copy original icmp data
        const uint8_t* src = (const uint8_t*)data;
        for (int i = 0; i < copy_len; i++) reply_buf[i] = src[i];

        // change type to reply
        ICMPHeader* reply_icmp = (ICMPHeader*)reply_buf;
        reply_icmp->type = ICMP_TYPE_ECHO_REPLY;
        reply_icmp->code = 0;
        reply_icmp->checksum = 0;
        reply_icmp->checksum = htons(Checksum(reply_buf, copy_len));

        SendIPv4(sender_ip, IP_PROTO_ICMP, reply_buf, copy_len);
        stats.icmp_tx++;
    } else if (icmp->type == ICMP_TYPE_ECHO_REPLY) {
        uint16_t reply_id = ntohs(icmp->identifier);
        uint16_t reply_seq = ntohs(icmp->sequence);
        if (g_ping_waiting && sender_ip == g_ping_expect_ip &&
            reply_id == g_ping_expect_id && reply_seq == g_ping_expect_seq) {
            uint32_t now_ms = Timer::GetTicks();
            g_ping_rtt_ms = now_ms - g_ping_sent_ms;
            g_ping_reply_ready = true;
            g_ping_waiting = false;
        }
    }
}

// buffer in-order tcp payload into the socket rx ring and advance our ack.
// returns the number of bytes accepted; sets *ack_needed when any data arrived
// (even out-of-order/duplicate, which still needs a duplicate ack). shared by
// the ESTABLISHED and FIN_WAIT half-close states so a peer's data is never
// dropped after we have sent our own FIN (the http "Connection: close" path).
// (satoru)
// push in-order bytes into the rx ring, advancing tcp_ack by what fits.
// returns bytes accepted. (satoru)
static int tcp_push_inorder(NetSocket* s, uint32_t seq, const uint8_t* payload, int len) {
    if (len <= 0) return 0;
    int space = TCP_RX_BUFSIZE - s->rx_count;
    int copy = len < space ? len : space;
    if (copy <= 0) return 0;
    tcp_ring_push(s, payload, copy);
    s->tcp_ack = seq + (uint32_t)copy;
    return copy;
}

// park an out-of-order segment (seq is AHEAD of tcp_ack) so it can be spliced
// in later. dedup by exact seq; drop if the pen is full or the segment sits
// beyond our advertised receive window (a peer that ignores the window). the
// stored bytes are capped to one segment (TCP_MSS). (satoru)
static void tcp_park_ooo(NetSocket* s, uint32_t seq, const uint8_t* payload, int len) {
    if (len <= 0) return;
    if (len > TCP_MSS) len = TCP_MSS;
    // must stay within the receive window ahead of tcp_ack, else drop. (satoru)
    uint32_t ahead = seq - s->tcp_ack;
    if (ahead >= (uint32_t)TCP_RX_BUFSIZE) return;
    for (int i = 0; i < TCP_OOO_SEGS; i++) {
        if (s->ooo_segs[i].in_use && s->ooo_segs[i].seq == seq) return;  // already held (satoru)
    }
    for (int i = 0; i < TCP_OOO_SEGS; i++) {
        if (s->ooo_segs[i].in_use) continue;
        s->ooo_segs[i].in_use = true;
        s->ooo_segs[i].seq    = seq;
        s->ooo_segs[i].len    = len;
        for (int j = 0; j < len; j++) s->ooo_segs[i].data[j] = payload[j];
        s->ooo_count++;
        return;
    }
    // pen full: drop (the peer will retransmit; better than clobbering). (satoru)
}

// after tcp_ack advances, splice any parked segments that now start at (or
// straddle) tcp_ack, looping until nothing contiguous remains. (satoru)
static void tcp_drain_ooo(NetSocket* s) {
    bool progress = true;
    while (progress && s->ooo_count > 0) {
        progress = false;
        for (int i = 0; i < TCP_OOO_SEGS; i++) {
            TcpOooSeg* o = &s->ooo_segs[i];
            if (!o->in_use) continue;
            uint32_t end = o->seq + (uint32_t)o->len;
            // wholly-old (already consumed): free it. (satoru)
            if (!SeqBefore(s->tcp_ack, end)) {
                o->in_use = false; s->ooo_count--; progress = true; continue;
            }
            // contiguous or overlapping the front of the gap: splice the new
            // tail (skip bytes at/behind tcp_ack). (satoru)
            if (!SeqAfter(o->seq, s->tcp_ack)) {
                uint32_t skip = s->tcp_ack - o->seq;   // bytes already consumed (satoru)
                if ((int)skip < o->len) {
                    tcp_push_inorder(s, s->tcp_ack, o->data + skip, o->len - (int)skip);
                }
                o->in_use = false; s->ooo_count--; progress = true;
            }
        }
    }
}

// accept a received tcp payload with FULL out-of-order reassembly. in-order
// bytes go straight to the rx ring (then any parked segments that now fit are
// spliced in); future bytes are parked; past bytes (duplicates) just ack.
// returns bytes delivered in-order; sets *ack_needed whenever data arrived
// (an out-of-order/dup segment still needs an immediate ack so the peer fast-
// retransmits the gap). shared by ESTABLISHED + the FIN_WAIT half-close so a
// peer's data is never dropped. (satoru)
static int tcp_accept_rx_data(NetSocket* s, uint32_t their_seq,
                              const uint8_t* payload, int payload_len,
                              bool* ack_needed) {
    if (payload_len <= 0) return 0;
    *ack_needed = true;
    if (their_seq == s->tcp_ack) {
        int copy = tcp_push_inorder(s, their_seq, payload, payload_len);
        tcp_drain_ooo(s);
        return copy;
    }
    if (SeqAfter(their_seq, s->tcp_ack)) {
        // future data (gap before it): park for reassembly, ack the gap. (satoru)
        tcp_park_ooo(s, their_seq, payload, payload_len);
        return 0;
    }
    // wholly-old duplicate (their_seq before tcp_ack): ack only. (satoru)
    return 0;
}

// parse the tcp options area of a received syn/syn-ack for the rfc 1323 window
// scale option. options live between the fixed 20-byte header and data_offset.
// on finding a wscale option, set *out_shift (clamped to TCP_WSCALE_MAX) and
// *out_found. unknown options are skipped by their length byte; nop/end are
// handled explicitly; a malformed length bails out. called only during the
// handshake, so the cost is negligible. (satoru)
static void tcp_parse_wscale(const uint8_t* tcp_hdr, int data_offset,
                             bool* out_found, uint8_t* out_shift) {
    *out_found = false;
    *out_shift = 0;
    int opt_start = (int)sizeof(TCPHeader);
    int i = opt_start;
    while (i < data_offset) {
        uint8_t kind = tcp_hdr[i];
        if (kind == TCP_OPT_END) break;
        if (kind == TCP_OPT_NOP) { i++; continue; }
        // every other option carries a length byte; need at least 2 bytes and a
        // length that stays within the options area. (satoru)
        if (i + 1 >= data_offset) break;
        uint8_t optlen = tcp_hdr[i + 1];
        if (optlen < 2 || i + optlen > data_offset) break;
        if (kind == TCP_OPT_WSCALE && optlen == 3) {
            uint8_t shift = tcp_hdr[i + 2];
            if (shift > TCP_WSCALE_MAX) shift = TCP_WSCALE_MAX;
            *out_shift = shift;
            *out_found = true;
        }
        i += optlen;
    }
}

void TCPStack::ProcessTCP(const IPv4Header* ip_hdr, const void* data, int len) {
    if (len < (int)sizeof(TCPHeader)) return;
    stats.tcp_rx++;

    const TCPHeader* tcp = (const TCPHeader*)data;
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint16_t src_port = ntohs(tcp->src_port);
    uint32_t src_ip = ntohl(ip_hdr->src_ip);

    int data_offset = ((tcp->data_offset >> 4) & 0xF) * 4;
    if (data_offset < (int)sizeof(TCPHeader) || data_offset > len) {
        slog("[TCP] bad header length, dropping\r\n");
        stats.errors_rx++;
        return;
    }
    const uint8_t* tcp_data = (const uint8_t*)data + data_offset;
    int tcp_data_len = len - data_offset;
    if (tcp_data_len < 0) tcp_data_len = 0;

    if (TCPChecksum(ip_hdr, tcp, tcp_data, tcp_data_len) != 0) {
        slog("[TCP] bad checksum, dropping\r\n");
        stats.errors_rx++;
        return;
    }

    // Prefer an exact 4-tuple match. Only fall back to a listening socket
    // for a new SYN; otherwise an accepted connection can get misrouted back
    // into the listener that shares the same local port.
    NetSocket* sock = nullptr;
    NetSocket* listener = nullptr;
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].active || sockets[i].type != SOCK_STREAM) continue;
        if (sockets[i].local_port != dst_port) continue;

        if (sockets[i].tcp_state == TCP_LISTEN) {
            if (!listener) listener = &sockets[i];
            continue;
        }

        if (sockets[i].remote_ip == src_ip && sockets[i].remote_port == src_port) {
            sock = &sockets[i];
            break;
        }
    }

    if (!sock && listener && (tcp->flags & TCP_FLAG_SYN)) {
        sock = listener;
    }

    if (!sock) {
        slog("[TCP] RX no matching socket: src=");
        sloghex(src_ip);
        slog("[TCP]   sport=");
        slognum((int)src_port);
        slog("[TCP]   dport=");
        slognum((int)dst_port);
        slog("[TCP]   flags=");
        sloghex((uint32_t)tcp->flags);
        // send rst for unknown connection
        return;
    }

    uint32_t their_seq = ntohl(tcp->seq_num);
    uint32_t their_ack = ntohl(tcp->ack_num);

    switch (sock->tcp_state) {
        case TCP_LISTEN:
            if (tcp->flags & TCP_FLAG_SYN) {
                sock->remote_ip = src_ip;
                sock->remote_port = src_port;
                sock->tcp_ack = their_seq + 1;
                sock->tcp_state = TCP_SYN_RECEIVED;
                sock->last_activity_ms = Timer::GetTicks();
                sock->keepalive_last_ms = sock->last_activity_ms;
                sock->keepalive_probes = 0;
                // learn the peer's window scale from their syn. we (server side)
                // only enable scaling, and only then echo the option in our
                // syn-ack, when the client offered it (rfc 1323 mutual rule).
                // wscale_ok gates the option echo inside SendTCPPacket. (satoru)
                {
                    bool ws_found = false; uint8_t ws_shift = 0;
                    tcp_parse_wscale((const uint8_t*)tcp, data_offset, &ws_found, &ws_shift);
                    sock->wscale_ok = ws_found;
                    sock->snd_wscale = ws_found ? ws_shift : 0;
                    if (!ws_found) sock->rcv_wscale = 0;
                }
                SendTCP(sock, TCP_FLAG_SYN | TCP_FLAG_ACK, nullptr, 0);
                sock->tcp_seq++;
            }
            break;

        case TCP_SYN_SENT:
            slog("[TCP] RX in SYN_SENT flags=");
            sloghex((uint32_t)tcp->flags);
            if ((tcp->flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
                ApplyAck(sock, their_ack);
                sock->tcp_ack = their_seq + 1;
                sock->tcp_seq = their_ack;
                sock->tcp_state = TCP_ESTABLISHED;
                // we sent the window-scale option in our syn (Connect, so
                // rcv_wscale is already TCP_RCV_WSCALE). scaling is mutual: only
                // keep ours active if the syn-ack echoed a window scale option.
                // if the peer didn't, drop back to a plain 16-bit window so our
                // advertised value isn't silently shifted down for a peer that
                // won't shift it back up. (satoru)
                {
                    bool ws_found = false; uint8_t ws_shift = 0;
                    tcp_parse_wscale((const uint8_t*)tcp, data_offset, &ws_found, &ws_shift);
                    sock->wscale_ok = ws_found;
                    sock->snd_wscale = ws_found ? ws_shift : 0;
                    if (!ws_found) sock->rcv_wscale = 0;
                }
                // background handshake finished: clear EINPROGRESS and arm
                // the keepalive/activity clocks fresh (satoru)
                sock->sock_errno = 0;
                sock->last_activity_ms = Timer::GetTicks();
                sock->keepalive_last_ms = sock->last_activity_ms;
                sock->keepalive_probes = 0;
                SendTCP(sock, TCP_FLAG_ACK, nullptr, 0);
                slog("[TCP] -> ESTABLISHED\r\n");
                // report the negotiated rfc 1323 window scaling so a headless
                // run can confirm the option round-tripped: wscale_ok=1 means
                // the peer echoed it; rcv/snd shifts are the agreed exponents.
                // (satoru)
                slog(sock->wscale_ok ? "[TCP] wscale negotiated ok rcv_shift="
                                     : "[TCP] wscale NOT negotiated rcv_shift=");
                slognum((int)sock->rcv_wscale);
                slog("[TCP]   snd_shift=");
                slognum((int)sock->snd_wscale);
                RuntimeLog::LogNetwork("tcp connection established", nullptr);
            } else if (tcp->flags & TCP_FLAG_RST) {
                sock->tcp_state = TCP_CLOSED;
                // rst during the handshake IS connection-refused; timed-out was
                // a mislabel that made every closed port read as a dead host in
                // getsockopt(SO_ERROR). (satoru)
                sock->sock_errno = TCP_ECONNREFUSED;
                slog("[TCP] got RST -> CLOSED\r\n");
                RuntimeLog::LogNetwork("tcp connection reset by peer", nullptr);
            }
            break;

        case TCP_SYN_RECEIVED:
            if (tcp->flags & TCP_FLAG_RST) {
                sock->tcp_state = TCP_LISTEN;
                sock->tx_pending = false;
                break;
            }
            if (tcp->flags & TCP_FLAG_ACK) {
                ApplyAck(sock, their_ack);
                sock->tcp_state = TCP_ESTABLISHED;
                // server-side handshake complete: arm keepalive clocks (satoru)
                sock->last_activity_ms = Timer::GetTicks();
                sock->keepalive_last_ms = sock->last_activity_ms;
                sock->keepalive_probes = 0;
            }
            break;

        case TCP_ESTABLISHED:
            if (tcp->flags & TCP_FLAG_RST) {
                sock->tcp_state = TCP_CLOSED;
                sock->tx_pending = false;
                sock->active = false;
                slog("[TCP] got RST in ESTABLISHED -> CLOSED\r\n");
                RuntimeLog::LogNetwork("tcp connection reset", "peer RST in established");
            } else {
                // any segment from the peer is proof the link is alive: reset
                // the keepalive idle timer and clear pending probes (satoru)
                sock->last_activity_ms = Timer::GetTicks();
                sock->keepalive_probes = 0;
                sock->keepalive_last_ms = sock->last_activity_ms;
                bool ack_needed = false;
                int accepted_data = 0;
                if (tcp_data_len > 0) {
                    // full reassembly: in-order data is buffered + drains any
                    // parked out-of-order segments; future data is parked;
                    // duplicates just ack. (satoru)
                    accepted_data = tcp_accept_rx_data(sock, their_seq, tcp_data,
                                                       tcp_data_len, &ack_needed);
                }
                if (tcp->flags & TCP_FLAG_ACK) {
                    ApplyAck(sock, their_ack);
                }
                if (tcp->flags & TCP_FLAG_FIN) {
                    // FIN consumes one sequence number after the data.
                    uint32_t fin_seq = their_seq + (uint32_t)tcp_data_len;
                    if (tcp_data_len == 0 || accepted_data == tcp_data_len) {
                        // Use seq arithmetic so wraparound is safe.
                        if (!SeqBefore(fin_seq + 1u, sock->tcp_ack))
                            sock->tcp_ack = fin_seq + 1u;
                        sock->tcp_state = TCP_CLOSE_WAIT;
                        ack_needed = true;
                        RuntimeLog::LogNetwork("tcp connection closed", "peer FIN (graceful)");
                    }
                }
                if (ack_needed) {
                    SendTCPPacket(sock, TCP_FLAG_ACK, nullptr, 0, sock->tcp_seq, false);
                }
            }
            break;

        case TCP_FIN_WAIT_1: {
            if (tcp->flags & TCP_FLAG_RST) {
                sock->tcp_state = TCP_CLOSED;
                sock->tx_pending = false;
                sock->active = false;
                break;
            }
            // a half-close still receives the peer's response: buffer+ack any
            // in-order data before acting on the ack/fin flags. (satoru)
            bool ack_needed = false;
            int accepted = tcp_accept_rx_data(sock, their_seq, tcp_data, tcp_data_len, &ack_needed);
            if (tcp->flags & TCP_FLAG_ACK) ApplyAck(sock, their_ack);
            // our fin is acked once the peer's ack reaches snd.nxt (tcp_seq was
            // bumped past the fin in Close). (satoru)
            bool our_fin_acked = (tcp->flags & TCP_FLAG_ACK) && !SeqBefore(their_ack, sock->tcp_seq);
            bool their_fin = false;
            if (tcp->flags & TCP_FLAG_FIN) {
                uint32_t fin_seq = their_seq + (uint32_t)tcp_data_len;
                if (tcp_data_len == 0 || accepted == tcp_data_len) {
                    if (!SeqBefore(fin_seq + 1u, sock->tcp_ack))
                        sock->tcp_ack = fin_seq + 1u;
                    their_fin = true;
                    ack_needed = true;
                }
            }
            if (our_fin_acked && their_fin)      sock->tcp_state = TCP_TIME_WAIT;
            else if (their_fin)                  sock->tcp_state = TCP_CLOSING;
            else if (our_fin_acked)              sock->tcp_state = TCP_FIN_WAIT_2;
            if (ack_needed)
                SendTCPPacket(sock, TCP_FLAG_ACK, nullptr, 0, sock->tcp_seq, false);
            break;
        }

        case TCP_FIN_WAIT_2: {
            if (tcp->flags & TCP_FLAG_RST) {
                sock->tcp_state = TCP_CLOSED;
                sock->tx_pending = false;
                sock->active = false;
                break;
            }
            // our fin is already acked here; keep receiving the peer's data
            // until it sends its own fin. (satoru)
            bool ack_needed = false;
            int accepted = tcp_accept_rx_data(sock, their_seq, tcp_data, tcp_data_len, &ack_needed);
            if (tcp->flags & TCP_FLAG_ACK) ApplyAck(sock, their_ack);
            if (tcp->flags & TCP_FLAG_FIN) {
                uint32_t fin_seq = their_seq + (uint32_t)tcp_data_len;
                if (tcp_data_len == 0 || accepted == tcp_data_len) {
                    if (!SeqBefore(fin_seq + 1u, sock->tcp_ack))
                        sock->tcp_ack = fin_seq + 1u;
                    sock->tcp_state = TCP_TIME_WAIT;
                    ack_needed = true;
                }
            }
            if (ack_needed)
                SendTCPPacket(sock, TCP_FLAG_ACK, nullptr, 0, sock->tcp_seq, false);
            break;
        }

        case TCP_CLOSE_WAIT:
            if (tcp->flags & TCP_FLAG_RST) {
                sock->tcp_state = TCP_CLOSED;
                sock->tx_pending = false;
                sock->active = false;
            }
            break;

        case TCP_CLOSING:
            if (tcp->flags & TCP_FLAG_RST) {
                sock->tcp_state = TCP_CLOSED;
                sock->tx_pending = false;
                sock->active = false;
                break;
            }
            if (tcp->flags & TCP_FLAG_ACK) {
                ApplyAck(sock, their_ack);
                sock->tcp_state = TCP_TIME_WAIT;
            }
            break;

        case TCP_LAST_ACK:
            if (tcp->flags & TCP_FLAG_ACK) {
                ApplyAck(sock, their_ack);
                sock->tcp_state = TCP_CLOSED;
                sock->tx_pending = false;
                sock->active = false;
            }
            break;

        default:
            break;
    }
}

bool TCPStack::SendTCP(NetSocket* sock, uint8_t flags, const void* data, int len) {
    if (!sock) return false;
    return SendTCPPacket(sock, flags, data, len, sock->tcp_seq, true);
}

uint32_t TCPStack::TCPSeqAdvance(uint8_t flags, int len) {
    uint32_t advance = (len > 0) ? (uint32_t)len : 0u;
    if (flags & TCP_FLAG_SYN) advance++;
    if (flags & TCP_FLAG_FIN) advance++;
    return advance;
}

bool TCPStack::SendTCPPacket(NetSocket* sock, uint8_t flags, const void* data, int len,
                             uint32_t seq, bool track_pending) {
    if (!sock) return false;
    // room for the fixed header + a 4-byte window-scale option (only on syn) +
    // a full mss of payload. (satoru)
    uint8_t buf[sizeof(TCPHeader) + 4 + TCP_MSS];
    TCPHeader* tcp = (TCPHeader*)buf;

    tcp->src_port = htons(sock->local_port);
    tcp->dst_port = htons(sock->remote_port);
    tcp->seq_num = htonl(seq);
    tcp->ack_num = htonl(sock->tcp_ack);

    // window-scale option goes ONLY on syn / syn-ack segments (rfc 1323). it is
    // 3 bytes (kind=3,len=3,shift); pad with a leading nop to keep the header a
    // 4-byte multiple, giving a 24-byte header (data offset 6). for a client
    // syn we always offer it; for a syn-ack we echo it only if the peer's syn
    // offered scaling (wscale_ok), so a non-scaling peer gets a plain header
    // back. (satoru)
    int opt_len = 0;
    bool emit_wscale = (flags & TCP_FLAG_SYN) &&
                       (sock->tcp_state == TCP_SYN_SENT || sock->wscale_ok);
    if (emit_wscale) {
        uint8_t* opt = buf + sizeof(TCPHeader);
        opt[0] = TCP_OPT_NOP;
        opt[1] = TCP_OPT_WSCALE;
        opt[2] = 3;
        opt[3] = TCP_RCV_WSCALE;
        opt_len = 4;
        sock->rcv_wscale = TCP_RCV_WSCALE;   // we have committed to scaling our rx window (satoru)
    }
    int hdr_words = (int)(sizeof(TCPHeader) + opt_len) / 4;
    tcp->data_offset = (uint8_t)(hdr_words << 4);
    tcp->flags = flags;
    // advertise the ACTUAL free space in our rx ring, recomputed at every
    // emission, so the peer never overruns the buffer (clamp to >=0). once
    // scaling is in effect (rcv_wscale>0, only after a syn that carried the
    // option), shift the byte count down into the 16-bit window field; the peer
    // shifts it back up by the same amount. the syn segment itself is NEVER
    // scaled (rfc 1323), so on a syn we send the raw (clamped) value. (satoru)
    int rx_free = TCP_RX_BUFSIZE - sock->rx_count;
    if (rx_free < 0) rx_free = 0;
    uint32_t adv = (uint32_t)rx_free;
    if (!(flags & TCP_FLAG_SYN) && sock->rcv_wscale)
        adv >>= sock->rcv_wscale;
    if (adv > 0xFFFFu) adv = 0xFFFFu;
    sock->tcp_window = (uint16_t)adv;
    tcp->window = htons((uint16_t)adv);
    tcp->checksum = 0;
    tcp->urgent = 0;

    if (data && len > 0) {
        if (len > TCP_MSS) len = TCP_MSS;
        const uint8_t* src = (const uint8_t*)data;
        for (int i = 0; i < len; i++) buf[sizeof(TCPHeader) + opt_len + i] = src[i];
    }

    // compute tcp checksum (need pseudo-header with ip info). TCPChecksum reads
    // the data-offset field to learn the header length, so the option bytes
    // (contiguous after the fixed header in buf) are covered automatically;
    // `data`/`len` is just the application payload. (satoru)
    IPv4Header pseudo_ip;
    pseudo_ip.src_ip = htonl(local_ip);
    pseudo_ip.dst_ip = htonl(sock->remote_ip);
    // Guard against null data pointer - only pass valid payload to checksum
    uint8_t null_buf[1] = {0};
    if (!data) { data = null_buf; len = 0; }
    tcp->checksum = htons(TCPChecksum(&pseudo_ip, tcp, data, len));

    stats.tcp_tx++;
    bool ok = SendIPv4(sock->remote_ip, IP_PROTO_TCP, buf, sizeof(TCPHeader) + opt_len + len);
    if (!ok) return false;

    if (track_pending) {
        uint32_t seq_advance = TCPSeqAdvance(flags, len);
        if (seq_advance > 0) {
            sock->tx_pending = true;
            sock->tx_flags = flags;
            sock->tx_retries = 0;
            sock->tx_len = len;
            sock->tx_seq_base = seq;
            sock->tx_seq_end = seq + seq_advance;
            sock->tx_last_tx_ms = Timer::GetTicks();
            if (len > 0) {
                const uint8_t* src = (const uint8_t*)data;
                for (int i = 0; i < len; i++) sock->tx_buf[i] = src[i];
            }
        }
    }
    return true;
}

void TCPStack::ProcessUDP(const IPv4Header* ip_hdr, const void* data, int len) {
    if (len < (int)sizeof(UDPHeader)) return;
    stats.udp_rx++;

    const UDPHeader* udp = (const UDPHeader*)data;
    uint16_t dst_port = ntohs(udp->dst_port);
    uint16_t src_port = ntohs(udp->src_port);
    uint32_t src_ip = ntohl(ip_hdr->src_ip);
    uint16_t udp_len_field = ntohs(udp->length);
    if (udp_len_field < sizeof(UDPHeader) || (int)udp_len_field > len) {
        stats.errors_rx++;
        return;
    }

    // Optional UDP-over-IPv4 checksum: 0 means "not computed", anything
    // else must verify or we drop. We reuse TCPChecksum which builds the
    // same pseudo-header layout - but the protocol byte differs, so
    // compute manually here.
    if (udp->checksum != 0) {
        struct {
            uint32_t src_ip;
            uint32_t dst_ip;
            uint8_t  zero;
            uint8_t  protocol;
            uint16_t udp_length;
        } __attribute__((packed)) pseudo;
        pseudo.src_ip = ip_hdr->src_ip;
        pseudo.dst_ip = ip_hdr->dst_ip;
        pseudo.zero = 0;
        pseudo.protocol = IP_PROTO_UDP;
        pseudo.udp_length = udp->length;
        uint32_t sum = 0;
        sum = ChecksumAccumulate(&pseudo, sizeof(pseudo), sum);
        sum = ChecksumAccumulate(udp, udp_len_field, sum);
        if (ChecksumFinish(sum) != 0) {
            stats.errors_rx++;
            return;
        }
    }

    const uint8_t* payload = (const uint8_t*)data + sizeof(UDPHeader);
    int payload_len = (int)udp_len_field - (int)sizeof(UDPHeader);

    // find matching socket
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].active || sockets[i].type != SOCK_DGRAM) continue;
        if (sockets[i].local_port != dst_port) continue;

        NetSocket* sock = &sockets[i];
        sock->remote_ip = src_ip;
        sock->remote_port = src_port;

        // framed push: an 8-byte record header + the payload, dropped WHOLE if
        // the ring can't hold both - a truncated datagram record would desync
        // every later pop. RecvFrom pops one record per call so datagram
        // boundaries + true source survive (musl's dns resolver needs both).
        // (satoru)
        int space = TCP_RX_BUFSIZE - sock->rx_count;
        if (payload_len + (int)sizeof(UdpRecHdr) > space) { stats.dropped++; return; }
        UdpRecHdr rh;
        rh.len      = (uint16_t)payload_len;
        rh.src_port = src_port;
        rh.src_ip   = src_ip;
        tcp_ring_push(sock, (const uint8_t*)&rh, (int)sizeof(rh));
        tcp_ring_push(sock, payload, payload_len);
        return;
    }

    stats.dropped++;
}

int TCPStack::Socket(int type) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].active) {
            sockets[i] = {};
            sockets[i].active = true;
            sockets[i].type = type;
            sockets[i].local_ip = local_ip;
            sockets[i].tcp_state = TCP_CLOSED;
            // tcp window is a 16-bit field; clamp (TCP_RX_BUFSIZE is now 64 kb,
            // which would wrap to 0 in a uint16_t). recomputed per-send. (satoru)
            sockets[i].tcp_window = (TCP_RX_BUFSIZE > 0xFFFF) ? 0xFFFF : (uint16_t)TCP_RX_BUFSIZE;
            sockets[i].rx_head = 0;
            sockets[i].rx_tail = 0;
            sockets[i].rx_count = 0;
            return i;
        }
    }
    return -1;
}

bool TCPStack::Bind(int sock, uint16_t port) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return false;
    sockets[sock].local_port = port;
    return true;
}

bool TCPStack::Connect(int sock, uint32_t ip, uint16_t port) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return false;

    NetSocket* s = &sockets[sock];
    s->remote_ip = ip;
    s->remote_port = port;
    s->sock_errno = 0;
    if (s->local_port == 0) s->local_port = AllocatePort();

    if (s->type == SOCK_STREAM) {
        slog("[TCP] Connect dst=");
        sloghex(ip);
        slog("[TCP] Connect dport=");
        slognum(port);
        // tcp: send syn (ISN derived from RDTSC for entropy)
        uint32_t lo, hi;
        __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
        s->tcp_seq = (lo ^ (hi << 1)) | 1u;
        s->tcp_ack = 0;
        s->tx_unacked = s->tcp_seq;
        s->tcp_state = TCP_SYN_SENT;
        // seed keepalive/activity clocks so an idle ESTABLISHED socket has a
        // sane baseline the moment the handshake finishes (satoru)
        s->last_activity_ms = Timer::GetTicks();
        s->keepalive_last_ms = s->last_activity_ms;
        s->keepalive_probes = 0;
        if (!SendTCP(s, TCP_FLAG_SYN, nullptr, 0)) {
            slog("[TCP] SendTCP(SYN) returned FALSE (route/ARP)\r\n");
            s->tcp_state = TCP_CLOSED;
            return false;
        }
        slog("[TCP] SYN sent, waiting for SYN-ACK\r\n");
        s->tcp_seq++;

        // Non-blocking connect: return immediately with EINPROGRESS. The
        // handshake finishes in the background (incoming SYN-ACK in
        // ProcessTCP, retransmits in TCPTick); callers poll IsWritable to
        // see the socket become ESTABLISHED. (satoru)
        if (s->nonblocking) {
            s->sock_errno = TCP_EINPROGRESS;
            slog("[TCP] non-blocking connect -> EINPROGRESS\r\n");
            return false;
        }

        // Wait using real elapsed time. The old spin-count loop could burn
        // through the entire timeout window before QEMU/SLIRP delivered a
        // SYN-ACK, so valid replies arrived only after the socket was closed.
        uint32_t connect_start_ms = Timer::GetTicks();
        while ((uint32_t)(Timer::GetTicks() - connect_start_ms) < 10000u) {
            if (KuronoShell::IsCommandCancelRequested()) {
                s->tcp_state = TCP_CLOSED;
                return false;
            }
            Tick();
            KuronoShell::PumpUI();
            if (s->tcp_state == TCP_ESTABLISHED) return true;
            if (s->tcp_state == TCP_CLOSED) return false;
            net_wait_one_ms();
        }
        s->tcp_state = TCP_CLOSED;
        slog("[TCP] Connect TIMEOUT (no SYN-ACK in 10s) tcp_rx=");
        slognum((int)stats.tcp_rx);
        slog("[TCP]   total packets_rx=");
        slognum((int)stats.packets_rx);
        slog("[TCP]   nic tx_pkts=");
        slognum((int)E1000::GetTxCount());
        slog("[TCP]   nic rx_pkts=");
        slognum((int)E1000::GetRxCount());
        return false;
    }

    return true; // udp is connectionless
}

bool TCPStack::Listen(int sock, int backlog) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return false;
    (void)backlog;
    sockets[sock].tcp_state = TCP_LISTEN;
    return true;
}

int TCPStack::Accept(int sock) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return -1;
    if (sockets[sock].tcp_state != TCP_LISTEN) return -1;

    // Poll for incoming SYN with a real-time bound (~10s) plus user
    // cancel, so the shell stays responsive instead of busy-spinning
    // on a CPU-cycle counter that varies wildly with build / host.
    uint32_t start_ms = Timer::GetTicks();
    while ((uint32_t)(Timer::GetTicks() - start_ms) < 10000u) {
        if (KuronoShell::IsCommandCancelRequested()) return -1;
        Tick();
        KuronoShell::PumpUI();
        if (sockets[sock].tcp_state == TCP_ESTABLISHED) {
            int new_sock = Socket(SOCK_STREAM);
            if (new_sock < 0) return -1;
            sockets[new_sock] = sockets[sock];
            sockets[new_sock].tx_pending = false;
            // the accepted socket starts with an empty send window - a listener
            // never queued data, but make the invariant explicit. (satoru)
            for (int k = 0; k < TCP_SND_WND_SEGS; k++) sockets[new_sock].tx_segs[k].in_use = false;
            sockets[new_sock].tx_seg_inflight = 0;
            sockets[sock].tcp_state = TCP_LISTEN;
            sockets[sock].remote_ip = 0;
            sockets[sock].remote_port = 0;
            sockets[sock].tx_pending = false;
            return new_sock;
        }
        net_wait_one_ms();
    }
    return -1;
}

int TCPStack::Send(int sock, const void* data, int len) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return -1;

    NetSocket* s = &sockets[sock];

    if (s->type == SOCK_STREAM) {
        if (s->tcp_state != TCP_ESTABLISHED) return -1;
        int sent = 0;
        const uint8_t* ptr = (const uint8_t*)data;
        while (sent < len) {
            int chunk = len - sent;
            if (chunk > TCP_MSS) chunk = TCP_MSS;

            // sliding send window: find a free scoreboard slot. up to
            // TCP_SND_WND_SEGS data segments may be outstanding at once, so we
            // only block here when the window is FULL (vs the old stop-and-wait
            // that blocked after every single segment). while we wait for an
            // ack to free a slot, Tick() runs ProcessTCP (which frees acked
            // segments via ApplyAck) and TCPTick (which retransmits on rto).
            // cap the wait at 2s so a stalled peer surfaces promptly. (satoru)
            int slot = -1;
            uint32_t wait_start_ms = Timer::GetTicks();
            for (;;) {
                for (int i = 0; i < TCP_SND_WND_SEGS; i++) {
                    if (!s->tx_segs[i].in_use) { slot = i; break; }
                }
                if (slot >= 0) break;
                if ((uint32_t)(Timer::GetTicks() - wait_start_ms) >= 2000u) break;
                if (KuronoShell::IsCommandCancelRequested())
                    return sent > 0 ? sent : -1;
                Tick();
                KuronoShell::PumpUI();
                if (s->tcp_state == TCP_CLOSED)
                    return sent > 0 ? sent : -1;
                net_wait_one_ms();
            }
            if (slot < 0) {
                slog("[TCP] Send window-full ACK timeout\r\n");
                return sent > 0 ? sent : -1;
            }
            if (s->tcp_state != TCP_ESTABLISHED)
                return sent > 0 ? sent : -1;

            // when the window is empty, snd.una starts at snd.nxt; ApplyAck
            // advances it as cumulative acks arrive. don't reset it mid-window
            // or in-flight segments would look un-acked. (satoru)
            if (s->tx_seg_inflight == 0) s->tx_unacked = s->tcp_seq;

            // buffer the payload + bookkeeping BEFORE emitting so a retransmit
            // that races in from TCPTick (same tick path) sees a complete slot.
            TxDataSeg* seg = &s->tx_segs[slot];
            for (int i = 0; i < chunk; i++) seg->data[i] = ptr[sent + i];
            seg->seq = s->tcp_seq;
            seg->len = chunk;
            seg->retries = 0;
            seg->last_tx_ms = Timer::GetTicks();
            seg->in_use = true;
            s->tx_seg_inflight++;

            // emit without touching the single-slot control machinery
            // (track_pending=false): the scoreboard owns this segment's
            // retransmit, and SYN/FIN must keep their own tx_pending slot. (satoru)
            if (!SendTCPPacket(s, TCP_FLAG_ACK | TCP_FLAG_PSH, seg->data, chunk,
                               seg->seq, false)) {
                slog("[TCP] Send failed in established state\r\n");
                seg->in_use = false;
                if (s->tx_seg_inflight > 0) s->tx_seg_inflight--;
                return sent > 0 ? sent : -1;
            }
            s->tcp_seq += (uint32_t)chunk;
            sent += chunk;
            // outbound data is activity: hold off keepalive probing (satoru)
            s->last_activity_ms = Timer::GetTicks();
            s->keepalive_probes = 0;
            s->keepalive_last_ms = s->last_activity_ms;
        }
        return sent;
    } else if (s->type == SOCK_DGRAM) {
        return SendTo(sock, data, len, s->remote_ip, s->remote_port);
    }
    return -1;
}

int TCPStack::Recv(int sock, void* buf, int max_len) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return -1;

    NetSocket* s = &sockets[sock];

    // throttled rx diagnostics (~every 2s): nic missed-packets (ring overflow),
    // total rx bytes, and the current ring occupancy. lets a headless bulk
    // download show whether the nic is dropping frames (the retransmit-stall
    // throughput killer) vs flowing cleanly. (satoru)
    {
        static uint32_t last_diag_ms = 0;
        uint32_t now = Timer::GetTicks();
        if ((uint32_t)(now - last_diag_ms) >= 2000u) {
            last_diag_ms = now;
            slog("[NETDIAG] rx_missed=");
            slognum((int)E1000::GetRxMissed());
            slog("[NETDIAG]   rx_kb=");
            slognum((int)(E1000::GetRxBytes() / 1024));
            slog("[NETDIAG]   ring_used=");
            slognum(s->rx_count);
        }
    }

    if (s->rx_count == 0) return 0;

    // free space BEFORE we drain - this is roughly what the peer last saw us
    // advertise. (satoru)
    int free_before = TCP_RX_BUFSIZE - s->rx_count;

    // drain the ring into the caller's buffer with at most two bulk memcpy
    // spans (the bytes up to the wrap, then the rest), instead of the old
    // per-byte loop. at a 256 kb window a single Recv can move a quarter
    // megabyte, so the per-byte loop was pure cpu overhead on the hot download
    // path. (satoru)
    uint8_t* dst = (uint8_t*)buf;
    int count = max_len < s->rx_count ? max_len : s->rx_count;
    if (count > 0) {
        int head = s->rx_head;
        int first = TCP_RX_BUFSIZE - head;     // bytes until the ring wraps (satoru)
        if (first > count) first = count;
        memcpy(dst, s->rx_buf + head, (size_t)first);
        int rem = count - first;
        if (rem > 0) memcpy(dst + first, s->rx_buf, (size_t)rem);
        s->rx_head = (head + count) % TCP_RX_BUFSIZE;
        s->rx_count -= count;
    }

    // window-update ack: on a bulk download the rx ring fills and our
    // advertised window collapses toward 0, so the peer stops sending. once the
    // app drains the ring the window reopens - but tcp only learns that if we
    // emit a segment. without this, slirp deadlocks waiting for a window update
    // that never comes (the 235 mb firefox tar stalled after ~4 kb). emit a
    // pure ack (which recomputes + carries the fresh, scaled window) whenever
    // the drain meaningfully reopens the window: either the ring was at least
    // half full before draining, or we just freed >= 2 mss. SendTCP recomputes
    // the window from rx_count at emission time. (satoru)
    if (count > 0 && s->type == SOCK_STREAM && s->tcp_state == TCP_ESTABLISHED) {
        bool was_pressured = free_before < (TCP_RX_BUFSIZE / 2);
        bool freed_a_lot   = count >= (TCP_MSS * 2);
        if (was_pressured && freed_a_lot) {
            SendTCP(s, TCP_FLAG_ACK, nullptr, 0);
        }
    }
    return count;
}

bool TCPStack::IsPeerClosed(int sock) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return true;
    TCPState state = sockets[sock].tcp_state;
    return state == TCP_CLOSE_WAIT || state == TCP_LAST_ACK ||
           state == TCP_TIME_WAIT || state == TCP_CLOSED;
}

void TCPStack::SetNonblocking(int sock, bool nonblocking) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return;
    sockets[sock].nonblocking = nonblocking;
}

bool TCPStack::IsNonblocking(int sock) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return false;
    return sockets[sock].nonblocking;
}

bool TCPStack::IsWritable(int sock) {
    // A stream socket is writable for poll/select once the handshake
    // completed (ESTABLISHED) - this is how a background non-blocking
    // connect signals success. UDP sockets are always writable. (satoru)
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return false;
    NetSocket* s = &sockets[sock];
    if (s->type != SOCK_STREAM) return true;
    return s->tcp_state == TCP_ESTABLISHED;
}

int TCPStack::GetSockError(int sock) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return 0;
    return sockets[sock].sock_errno;
}

// lock-free single-word getters for the linux af_inet bridge (see tcpip.h);
// staleness is bounded by the caller's next poll pass. (satoru)
int TCPStack::RxAvailable(int sock) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return 0;
    return sockets[sock].rx_count;
}

int TCPStack::TxFreeSegs(int sock) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return 0;
    NetSocket* s = &sockets[sock];
    if (s->type != SOCK_STREAM) return TCP_SND_WND_SEGS;   // udp never queues (satoru)
    if (s->tcp_state != TCP_ESTABLISHED) return 0;
    int free_slots = TCP_SND_WND_SEGS - s->tx_seg_inflight;
    return free_slots > 0 ? free_slots : 0;
}

bool TCPStack::GetAddrInfo(int sock, uint32_t* lip, uint16_t* lport,
                           uint32_t* rip, uint16_t* rport) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return false;
    NetSocket* s = &sockets[sock];
    if (lip)   *lip   = s->local_ip ? s->local_ip : local_ip;
    if (lport) *lport = s->local_port;
    if (rip)   *rip   = s->remote_ip;
    if (rport) *rport = s->remote_port;
    return true;
}

bool TCPStack::Close(int sock) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return false;

    NetSocket* s = &sockets[sock];
    if (s->type == SOCK_STREAM && s->tcp_state == TCP_ESTABLISHED) {
        s->tcp_state = TCP_FIN_WAIT_1;
        if (!SendTCP(s, TCP_FLAG_FIN | TCP_FLAG_ACK, nullptr, 0)) {
            s->tcp_state = TCP_ESTABLISHED;
            return false;
        }
        s->tcp_seq++;
        return true;
    } else if (s->type == SOCK_STREAM && s->tcp_state == TCP_CLOSE_WAIT) {
        s->tcp_state = TCP_LAST_ACK;
        if (!SendTCP(s, TCP_FLAG_FIN | TCP_FLAG_ACK, nullptr, 0)) {
            s->tcp_state = TCP_CLOSE_WAIT;
            return false;
        }
        s->tcp_seq++;
        return true;
    } else if (s->type == SOCK_STREAM &&
               (s->tcp_state == TCP_FIN_WAIT_1 || s->tcp_state == TCP_FIN_WAIT_2 ||
                s->tcp_state == TCP_CLOSING || s->tcp_state == TCP_LAST_ACK ||
                s->tcp_state == TCP_TIME_WAIT)) {
        return true;
    }

    s->active = false;
    s->tcp_state = TCP_CLOSED;
    return true;
}

int TCPStack::SendTo(int sock, const void* data, int len, uint32_t ip, uint16_t port) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return -1;
    if (sockets[sock].type != SOCK_DGRAM) return -1;
    if (len < 0) return -1;
    const int max_payload = (int)ETH_MTU - (int)sizeof(IPv4Header) - (int)sizeof(UDPHeader);
    if (len > max_payload) return -1;

    uint8_t buf[sizeof(UDPHeader) + ETH_MTU];
    UDPHeader* udp = (UDPHeader*)buf;

    if (sockets[sock].local_port == 0)
        sockets[sock].local_port = AllocatePort();

    udp->src_port = htons(sockets[sock].local_port);
    udp->dst_port = htons(port);
    udp->length = htons((uint16_t)(sizeof(UDPHeader) + len));
    udp->checksum = 0;

    const uint8_t* src = (const uint8_t*)data;
    for (int i = 0; i < len; i++)
        buf[sizeof(UDPHeader) + i] = src[i];

    stats.udp_tx++;
    if (SendIPv4(ip, IP_PROTO_UDP, buf, sizeof(UDPHeader) + len))
        return len;
    return -1;
}

int TCPStack::RecvFrom(int sock, void* buf, int max_len, uint32_t* from_ip, uint16_t* from_port) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return -1;

    NetSocket* s = &sockets[sock];

    // udp: pop exactly ONE framed datagram record (see UdpRecHdr) so each call
    // returns one distinct datagram with its true source, not a concatenation
    // with a clobbered remote. oversize payload is truncated to max_len and the
    // tail discarded - standard udp semantics. (satoru)
    if (s->type == SOCK_DGRAM) {
        if (s->rx_count < (int)sizeof(UdpRecHdr)) return 0;   // nothing queued (satoru)
        UdpRecHdr rh;
        tcp_ring_pop(s, (uint8_t*)&rh, (int)sizeof(rh));
        int give = (int)rh.len < max_len ? (int)rh.len : max_len;
        tcp_ring_pop(s, (uint8_t*)buf, give);
        int discard = (int)rh.len - give;
        if (discard > 0) tcp_ring_pop(s, nullptr, discard);
        if (from_ip)   *from_ip   = rh.src_ip;
        if (from_port) *from_port = rh.src_port;
        return give;
    }

    if (from_ip) *from_ip = s->remote_ip;
    if (from_port) *from_port = s->remote_port;

    return Recv(sock, buf, max_len);
}

bool TCPStack::Ping(uint32_t ip, int timeout_ms, int* rtt_ms) {
    // send icmp echo request
    struct {
        ICMPHeader hdr;
        uint8_t data[32];
    } __attribute__((packed)) echo;

    uint16_t ping_id = g_ping_next_id++;
    uint16_t ping_seq = g_ping_next_seq++;

    echo.hdr.type = ICMP_TYPE_ECHO_REQUEST;
    echo.hdr.code = 0;
    echo.hdr.identifier = htons(ping_id);
    echo.hdr.sequence = htons(ping_seq);
    echo.hdr.checksum = 0;
    for (int i = 0; i < 32; i++) echo.data[i] = (uint8_t)(i + 'A');
    echo.hdr.checksum = htons(Checksum(&echo, sizeof(echo)));

    g_ping_waiting = true;
    g_ping_reply_ready = false;
    g_ping_expect_id = ping_id;
    g_ping_expect_seq = ping_seq;
    g_ping_expect_ip = ip;
    /* Use Timer::GetTicks (PIT-polled real ms). Time::GetTicks reads
       TimeManager::monotonic_ms which only advances from the kernel main
       loop, so it freezes during synchronous shell commands and the wait
       below would never time out. */
    g_ping_sent_ms = Timer::GetTicks();
    g_ping_rtt_ms = 0;

    stats.icmp_tx++;
    if (!SendIPv4(ip, IP_PROTO_ICMP, &echo, sizeof(echo))) {
        g_ping_waiting = false;
        return false;
    }

    // wait for a matching echo reply (PIT-polled time so we cannot wedge)
    uint32_t start_ms = g_ping_sent_ms;
    while ((uint32_t)(Timer::GetTicks() - start_ms) < (uint32_t)timeout_ms) {
        if (KuronoShell::IsCommandCancelRequested()) {
            g_ping_waiting = false;
            g_ping_reply_ready = false;
            return false;
        }
        Tick();
        KuronoShell::PumpUI();
        if (g_ping_reply_ready) {
            g_ping_reply_ready = false;
            if (rtt_ms) *rtt_ms = (int)g_ping_rtt_ms;
            return true;
        }
        net_wait_one_ms();
    }

    g_ping_waiting = false;
    g_ping_reply_ready = false;
    return false;
}

void TCPStack::Tick() {
    if (!initialized) return;

    // poll e1000 for received packets (callback-based)
    E1000::Poll();
    ServicePendingIPv4();

    // tcp timer processing
    TCPTick();
}

void TCPStack::TCPTick() {
    // handle retransmissions, time_wait cleanup, etc.
    uint32_t now_ms = Timer::GetTicks();
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].active || sockets[i].type != SOCK_STREAM) continue;
        if (sockets[i].tx_pending) {
            // Exponential backoff: 500ms * 2^retries, capped at 4s.
            uint32_t rto_ms = 500u << (sockets[i].tx_retries < 4 ? sockets[i].tx_retries : 4);
            if (rto_ms > 4000u) rto_ms = 4000u;
            if ((uint32_t)(now_ms - sockets[i].tx_last_tx_ms) >= rto_ms) {
                if (sockets[i].tx_retries >= 6) {
                    slog("[TCP] retransmit budget exhausted\r\n");
                    sockets[i].tx_pending = false;
                    sockets[i].tcp_state = TCP_CLOSED;
                    sockets[i].active = false;
                    continue;
                }
                SendTCPPacket(&sockets[i], sockets[i].tx_flags,
                              sockets[i].tx_len > 0 ? sockets[i].tx_buf : nullptr,
                              sockets[i].tx_len, sockets[i].tx_seq_base, false);
                sockets[i].tx_retries++;
                sockets[i].tx_last_tx_ms = now_ms;
            }
        }

        // send-window retransmit: each outstanding DATA segment carries its own
        // rto + retry count and is retransmitted independently. acks free
        // segments in ApplyAck; anything still in_use past its rto goes back on
        // the wire. if any one segment blows its retransmit budget the peer is
        // gone - tear the connection down (same policy as the control slot).
        // payload was buffered in the scoreboard so we resend without the
        // caller. (satoru)
        bool conn_dead = false;
        for (int k = 0; k < TCP_SND_WND_SEGS; k++) {
            TxDataSeg* seg = &sockets[i].tx_segs[k];
            if (!seg->in_use) continue;
            uint32_t rto_ms = 500u << (seg->retries < 4 ? seg->retries : 4);
            if (rto_ms > 4000u) rto_ms = 4000u;
            if ((uint32_t)(now_ms - seg->last_tx_ms) < rto_ms) continue;
            if (seg->retries >= 6) {
                slog("[TCP] data retransmit budget exhausted\r\n");
                conn_dead = true;
                break;
            }
            SendTCPPacket(&sockets[i], TCP_FLAG_ACK | TCP_FLAG_PSH,
                          seg->data, seg->len, seg->seq, false);
            seg->retries++;
            seg->last_tx_ms = now_ms;
        }
        if (conn_dead) {
            sockets[i].tx_pending = false;
            for (int k = 0; k < TCP_SND_WND_SEGS; k++) sockets[i].tx_segs[k].in_use = false;
            sockets[i].tx_seg_inflight = 0;
            sockets[i].tcp_state = TCP_CLOSED;
            sockets[i].active = false;
            continue;
        }

        // keepalive: probe an idle ESTABLISHED socket, then give up after a
        // bounded number of unanswered probes. Any RX/TX activity resets these
        // clocks (see ProcessTCP / Send), so this only fires on true idle.
        // A keepalive probe is a zero-length segment carrying seq = snd.nxt-1,
        // which forces the peer to emit an ACK without delivering data. don't
        // probe while data is still in flight - that's not idle. (satoru)
        if (sockets[i].tcp_state == TCP_ESTABLISHED && !sockets[i].tx_pending &&
            sockets[i].tx_seg_inflight == 0) {
            uint32_t idle_ms = (uint32_t)(now_ms - sockets[i].last_activity_ms);
            if (sockets[i].keepalive_probes == 0) {
                if (idle_ms >= TCP_KEEPALIVE_IDLE_MS) {
                    slog("[TCP] keepalive: idle, probing\r\n");
                    SendTCPPacket(&sockets[i], TCP_FLAG_ACK, nullptr, 0,
                                  sockets[i].tcp_seq - 1u, false);
                    sockets[i].keepalive_probes = 1;
                    sockets[i].keepalive_last_ms = now_ms;
                }
            } else if ((uint32_t)(now_ms - sockets[i].keepalive_last_ms) >= TCP_KEEPALIVE_INTVL_MS) {
                if (sockets[i].keepalive_probes >= TCP_KEEPALIVE_MAX_PROBES) {
                    slog("[TCP] keepalive: no response -> ETIMEDOUT, closing\r\n");
                    sockets[i].sock_errno = TCP_ETIMEDOUT;
                    sockets[i].tx_pending = false;
                    sockets[i].tcp_state = TCP_CLOSED;
                    sockets[i].active = false;
                    continue;
                }
                SendTCPPacket(&sockets[i], TCP_FLAG_ACK, nullptr, 0,
                              sockets[i].tcp_seq - 1u, false);
                sockets[i].keepalive_probes++;
                sockets[i].keepalive_last_ms = now_ms;
            }
        }
        if (sockets[i].tcp_state == TCP_TIME_WAIT) {
            // eventually clean up
            sockets[i].active = false;
            sockets[i].tcp_state = TCP_CLOSED;
        }
    }
}

uint16_t TCPStack::AllocatePort() {
    // Walk the ephemeral range and skip any port already in use.
    for (int tries = 0; tries < (65535 - 49152); tries++) {
        if (next_ephemeral_port < 49152 || next_ephemeral_port >= 65535)
            next_ephemeral_port = 49152;
        uint16_t p = next_ephemeral_port++;
        bool used = false;
        for (int i = 0; i < MAX_SOCKETS; i++) {
            if (sockets[i].active && sockets[i].local_port == p) { used = true; break; }
        }
        if (!used) return p;
    }
    return next_ephemeral_port++;
}

const NetStats& TCPStack::GetStats() { return stats; }

void TCPStack::DumpInfo(char* out, int max_len) {
    int pos = 0;
    auto append = [&](const char* s) {
        while (*s && pos < max_len - 1) out[pos++] = *s++;
    };
    auto append_num = [&](uint32_t val) {
        char buf[12]; int i = 0;
        if (val == 0) { buf[i++] = '0'; }
        else { char rev[12]; int ri = 0; uint32_t tmp = val;
            while (tmp) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; }
            while (ri--) buf[i++] = rev[ri]; }
        buf[i] = 0; append(buf);
    };
    auto append_hex = [&](uint8_t val) {
        const char* hex = "0123456789abcdef";
        if (pos < max_len - 1) out[pos++] = hex[(val >> 4) & 0xF];
        if (pos < max_len - 1) out[pos++] = hex[val & 0xF];
    };

    if (!initialized) {
        append("Network: Not initialized\n");
        out[pos] = 0; return;
    }

    char ipbuf[16];

    append("TCP/IP Network Stack\n");
    append("  MAC: ");
    for (int i = 0; i < 6; i++) {
        if (i > 0) append(":");
        append_hex(mac[i]);
    }
    append("\n");

    append("  IP:      "); FormatIP(local_ip, ipbuf); append(ipbuf); append("\n");
    append("  Mask:    "); FormatIP(subnet_mask, ipbuf); append(ipbuf); append("\n");
    append("  Gateway: "); FormatIP(gateway, ipbuf); append(ipbuf); append("\n");
    append("  DNS:     "); FormatIP(dns_server, ipbuf); append(ipbuf); append("\n");

    append("  Stats:\n");
    append("    RX Packets: "); append_num(stats.packets_rx); append("\n");
    append("    TX Packets: "); append_num(stats.packets_tx); append("\n");
    append("    RX Bytes:   "); append_num(stats.bytes_rx); append("\n");
    append("    TX Bytes:   "); append_num(stats.bytes_tx); append("\n");
    append("    ARP Req:    "); append_num(stats.arp_requests); append("\n");
    append("    ARP Rep:    "); append_num(stats.arp_replies); append("\n");
    append("    ICMP RX:    "); append_num(stats.icmp_rx); append("\n");
    append("    TCP RX:     "); append_num(stats.tcp_rx); append("\n");
    append("    TCP TX:     "); append_num(stats.tcp_tx); append("\n");
    append("    UDP RX:     "); append_num(stats.udp_rx); append("\n");
    append("    UDP TX:     "); append_num(stats.udp_tx); append("\n");
    append("    Dropped:    "); append_num(stats.dropped); append("\n");

    // active sockets
    int active_count = 0;
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].active) active_count++;
    }
    append("  Active Sockets: "); append_num(active_count); append("\n");

    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].active) continue;
        append("    ["); append_num(i); append("] ");
        append(sockets[i].type == SOCK_STREAM ? "TCP" : "UDP");
        append(" :"); append_num(sockets[i].local_port);
        if (sockets[i].remote_ip) {
            append(" -> ");
            FormatIP(sockets[i].remote_ip, ipbuf); append(ipbuf);
            append(":"); append_num(sockets[i].remote_port);
        }
        if (sockets[i].type == SOCK_STREAM) {
            append(" [");
            const char* state_names[] = {"CLOSED","LISTEN","SYN_SENT","SYN_RCVD",
                "ESTABLISHED","FIN_WAIT1","FIN_WAIT2","CLOSE_WAIT",
                "CLOSING","LAST_ACK","TIME_WAIT"};
            if (sockets[i].tcp_state <= TCP_TIME_WAIT)
                append(state_names[sockets[i].tcp_state]);
            append("]");
        }
        append("\n");
    }

    out[pos] = 0;
}
