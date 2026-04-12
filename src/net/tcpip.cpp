//  kurono os  -  tcp/ip network stack implementation
//  lightweight tcp/ip over e1000 ethernet
#include "tcpip.h"
#include "../drivers/e1000.h"
#include "../kernel/heap.h"

bool TCPStack::initialized = false;
uint32_t TCPStack::local_ip = 0;
uint32_t TCPStack::subnet_mask = 0;
uint32_t TCPStack::gateway = 0;
uint32_t TCPStack::dns_server = 0;
uint8_t TCPStack::mac[6] = {};

ARPEntry TCPStack::arp_cache[ARP_CACHE_SIZE] = {};
NetSocket TCPStack::sockets[MAX_SOCKETS] = {};
NetStats TCPStack::stats = {};

uint16_t TCPStack::next_ephemeral_port = 49152;
uint16_t TCPStack::ip_ident = 1;

uint16_t TCPStack::htons(uint16_t val) {
    return ((val & 0xFF) << 8) | ((val >> 8) & 0xFF);
}

uint32_t TCPStack::htonl(uint32_t val) {
    return ((val & 0xFF) << 24) | (((val >> 8) & 0xFF) << 16) |
           (((val >> 16) & 0xFF) << 8) | ((val >> 24) & 0xFF);
}

uint16_t TCPStack::ntohs(uint16_t val) { return htons(val); }
uint32_t TCPStack::ntohl(uint32_t val) { return htonl(val); }

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
    const uint16_t* ptr = (const uint16_t*)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }

    if (len == 1)
        sum += *(const uint8_t*)ptr;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)(~sum);
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

    int tcp_total = sizeof(TCPHeader) + payload_len;
    pseudo.src_ip = ip->src_ip;
    pseudo.dst_ip = ip->dst_ip;
    pseudo.zero = 0;
    pseudo.protocol = IP_PROTO_TCP;
    pseudo.tcp_length = htons(tcp_total);

    uint32_t sum = 0;
    const uint16_t* p;

    // sum pseudo header
    p = (const uint16_t*)&pseudo;
    for (int i = 0; i < 6; i++) sum += p[i];

    // sum tcp header
    p = (const uint16_t*)tcp;
    for (int i = 0; i < (int)(sizeof(TCPHeader) / 2); i++) sum += p[i];

    // sum payload
    p = (const uint16_t*)payload;
    int plen = payload_len;
    while (plen > 1) { sum += *p++; plen -= 2; }
    if (plen == 1) sum += *(const uint8_t*)p;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

static void NetworkPacketHandler(const uint8_t* data, uint16_t length) {
    TCPStack::ProcessRxPacket(data, (int)length);
}

bool TCPStack::Init() {
    initialized = false;

    for (int i = 0; i < MAX_SOCKETS; i++) sockets[i].active = false;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) arp_cache[i].valid = false;

    stats = {};

    // check if e1000 is available
    if (!E1000::IsDetected()) return false;

    // get mac address from e1000
    E1000::GetMAC(mac);

    // register packet handler so e1000::poll() delivers packets to our stack
    E1000::SetPacketHandler(NetworkPacketHandler);

    // default ip config (can be changed later via dhcp or static)
    local_ip = MakeIP(10, 0, 2, 15);
    subnet_mask = MakeIP(255, 255, 255, 0);
    gateway = MakeIP(10, 0, 2, 2);
    dns_server = MakeIP(10, 0, 2, 3);

    initialized = true;
    return true;
}

bool TCPStack::IsUp() { return initialized && E1000::IsDetected(); }

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
    if (len < (int)sizeof(ARPHeader)) return;
    const ARPHeader* arp = (const ARPHeader*)data;

    uint32_t sender_ip = ((uint32_t)arp->sender_ip[0] << 24) |
                         ((uint32_t)arp->sender_ip[1] << 16) |
                         ((uint32_t)arp->sender_ip[2] << 8) |
                         arp->sender_ip[3];
    uint32_t target_ip = ((uint32_t)arp->target_ip[0] << 24) |
                         ((uint32_t)arp->target_ip[1] << 16) |
                         ((uint32_t)arp->target_ip[2] << 8) |
                         arp->target_ip[3];

    // update arp cache with sender info
    bool found = false;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == sender_ip) {
            for (int j = 0; j < 6; j++) arp_cache[i].mac[j] = arp->sender_mac[j];
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
                break;
            }
        }
    }

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
    if (len > ETH_MTU) return false;

    uint8_t frame[ETH_FRAME_MAX];
    EthernetHeader* eth = (EthernetHeader*)frame;

    for (int i = 0; i < 6; i++) eth->dst_mac[i] = dst_mac[i];
    for (int i = 0; i < 6; i++) eth->src_mac[i] = mac[i];
    eth->ethertype = htons(ethertype);

    const uint8_t* src = (const uint8_t*)payload;
    for (int i = 0; i < len; i++) frame[ETH_HLEN + i] = src[i];

    E1000::Send(frame, ETH_HLEN + len);
    stats.packets_tx++;
    stats.bytes_tx += ETH_HLEN + len;
    return true;
}

bool TCPStack::SendIPv4(uint32_t dst_ip, uint8_t proto, const void* payload, int len) {
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

    ip->checksum = Checksum(ip, sizeof(IPv4Header));

    // copy payload
    const uint8_t* src = (const uint8_t*)payload;
    for (int i = 0; i < len && i < (int)(ETH_MTU - sizeof(IPv4Header)); i++)
        pkt[sizeof(IPv4Header) + i] = src[i];

    // determine next-hop ip
    uint32_t next_hop = dst_ip;
    if ((dst_ip & subnet_mask) != (local_ip & subnet_mask))
        next_hop = gateway;

    // arp lookup
    uint8_t dst_mac[6];
    if (!ARPLookup(next_hop, dst_mac)) {
        ARPRequest(next_hop);
        // wait briefly for arp response
        for (int i = 0; i < 100; i++) {
            // process pending packets
            for (volatile int d = 0; d < 10000; d++);
            if (ARPLookup(next_hop, dst_mac)) break;
        }
        if (!ARPLookup(next_hop, dst_mac)) {
            stats.errors_tx++;
            return false;
        }
    }

    return SendEthernet(dst_mac, ETH_TYPE_IPV4, pkt, sizeof(IPv4Header) + len);
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
        default:
            stats.dropped++;
            break;
    }
}

void TCPStack::ProcessIPv4(const void* data, int len) {
    if (len < (int)sizeof(IPv4Header)) return;

    const IPv4Header* ip = (const IPv4Header*)data;
    int ihl = (ip->ver_ihl & 0x0F) * 4;
    if (ihl < 20 || len < ihl) return;

    // verify checksum
    // uint16_t cksum = checksum(ip, ihl);
    // if (cksum != 0) { stats.errors_rx++; return; }

    const uint8_t* payload = (const uint8_t*)data + ihl;
    int payload_len = ntohs(ip->total_length) - ihl;
    if (payload_len < 0 || payload_len > len - ihl) payload_len = len - ihl;

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
            stats.dropped++;
            break;
    }
}

void TCPStack::ProcessICMP(const IPv4Header* ip_hdr, const void* data, int len) {
    if (len < (int)sizeof(ICMPHeader)) return;
    stats.icmp_rx++;

    const ICMPHeader* icmp = (const ICMPHeader*)data;

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
        reply_icmp->checksum = Checksum(reply_buf, copy_len);

        uint32_t sender_ip = ntohl(ip_hdr->src_ip);
        SendIPv4(sender_ip, IP_PROTO_ICMP, reply_buf, copy_len);
        stats.icmp_tx++;
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
    const uint8_t* tcp_data = (const uint8_t*)data + data_offset;
    int tcp_data_len = len - data_offset;
    if (tcp_data_len < 0) tcp_data_len = 0;

    // find matching socket
    NetSocket* sock = nullptr;
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].active || sockets[i].type != SOCK_STREAM) continue;

        if (sockets[i].local_port == dst_port) {
            if (sockets[i].tcp_state == TCP_LISTEN ||
                (sockets[i].remote_ip == src_ip && sockets[i].remote_port == src_port)) {
                sock = &sockets[i];
                break;
            }
        }
    }

    if (!sock) {
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
                SendTCP(sock, TCP_FLAG_SYN | TCP_FLAG_ACK, nullptr, 0);
                sock->tcp_seq++;
            }
            break;

        case TCP_SYN_SENT:
            if ((tcp->flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
                sock->tcp_ack = their_seq + 1;
                sock->tcp_seq = their_ack;
                sock->tcp_state = TCP_ESTABLISHED;
                SendTCP(sock, TCP_FLAG_ACK, nullptr, 0);
            }
            break;

        case TCP_SYN_RECEIVED:
            if (tcp->flags & TCP_FLAG_ACK) {
                sock->tcp_state = TCP_ESTABLISHED;
            }
            break;

        case TCP_ESTABLISHED:
            if (tcp->flags & TCP_FLAG_FIN) {
                sock->tcp_ack = their_seq + 1;
                sock->tcp_state = TCP_CLOSE_WAIT;
                SendTCP(sock, TCP_FLAG_ACK, nullptr, 0);
            } else {
                // receive data
                if (tcp_data_len > 0) {
                    for (int i = 0; i < tcp_data_len && sock->rx_count < TCP_RX_BUFSIZE; i++) {
                        sock->rx_buf[sock->rx_tail] = tcp_data[i];
                        sock->rx_tail = (sock->rx_tail + 1) % TCP_RX_BUFSIZE;
                        sock->rx_count++;
                    }
                    sock->tcp_ack = their_seq + tcp_data_len;
                    SendTCP(sock, TCP_FLAG_ACK, nullptr, 0);
                }
                if (tcp->flags & TCP_FLAG_ACK) {
                    sock->tx_unacked = their_ack;
                }
            }
            break;

        case TCP_FIN_WAIT_1:
            if ((tcp->flags & TCP_FLAG_ACK) && (tcp->flags & TCP_FLAG_FIN)) {
                sock->tcp_ack = their_seq + 1;
                sock->tcp_state = TCP_TIME_WAIT;
                SendTCP(sock, TCP_FLAG_ACK, nullptr, 0);
            } else if (tcp->flags & TCP_FLAG_ACK) {
                sock->tcp_state = TCP_FIN_WAIT_2;
            } else if (tcp->flags & TCP_FLAG_FIN) {
                sock->tcp_ack = their_seq + 1;
                sock->tcp_state = TCP_CLOSING;
                SendTCP(sock, TCP_FLAG_ACK, nullptr, 0);
            }
            break;

        case TCP_FIN_WAIT_2:
            if (tcp->flags & TCP_FLAG_FIN) {
                sock->tcp_ack = their_seq + 1;
                sock->tcp_state = TCP_TIME_WAIT;
                SendTCP(sock, TCP_FLAG_ACK, nullptr, 0);
            }
            break;

        case TCP_CLOSE_WAIT:
            // waiting for application to close
            break;

        case TCP_CLOSING:
            if (tcp->flags & TCP_FLAG_ACK) {
                sock->tcp_state = TCP_TIME_WAIT;
            }
            break;

        case TCP_LAST_ACK:
            if (tcp->flags & TCP_FLAG_ACK) {
                sock->tcp_state = TCP_CLOSED;
                sock->active = false;
            }
            break;

        default:
            break;
    }
}

bool TCPStack::SendTCP(NetSocket* sock, uint8_t flags, const void* data, int len) {
    uint8_t buf[sizeof(TCPHeader) + TCP_MSS];
    TCPHeader* tcp = (TCPHeader*)buf;

    tcp->src_port = htons(sock->local_port);
    tcp->dst_port = htons(sock->remote_port);
    tcp->seq_num = htonl(sock->tcp_seq);
    tcp->ack_num = htonl(sock->tcp_ack);
    tcp->data_offset = (5 << 4); // 20 bytes, no options
    tcp->flags = flags;
    tcp->window = htons(sock->tcp_window ? sock->tcp_window : TCP_RX_BUFSIZE);
    tcp->checksum = 0;
    tcp->urgent = 0;

    if (data && len > 0) {
        if (len > TCP_MSS) len = TCP_MSS;
        const uint8_t* src = (const uint8_t*)data;
        for (int i = 0; i < len; i++) buf[sizeof(TCPHeader) + i] = src[i];
    }

    // compute tcp checksum (need pseudo-header with ip info)
    IPv4Header pseudo_ip;
    pseudo_ip.src_ip = htonl(local_ip);
    pseudo_ip.dst_ip = htonl(sock->remote_ip);
    tcp->checksum = TCPChecksum(&pseudo_ip, tcp, data, len);

    stats.tcp_tx++;
    return SendIPv4(sock->remote_ip, IP_PROTO_TCP, buf, sizeof(TCPHeader) + len);
}

void TCPStack::ProcessUDP(const IPv4Header* ip_hdr, const void* data, int len) {
    if (len < (int)sizeof(UDPHeader)) return;
    stats.udp_rx++;

    const UDPHeader* udp = (const UDPHeader*)data;
    uint16_t dst_port = ntohs(udp->dst_port);
    uint16_t src_port = ntohs(udp->src_port);
    uint32_t src_ip = ntohl(ip_hdr->src_ip);

    const uint8_t* payload = (const uint8_t*)data + sizeof(UDPHeader);
    int payload_len = ntohs(udp->length) - sizeof(UDPHeader);
    if (payload_len < 0) return;
    if (payload_len > len - (int)sizeof(UDPHeader)) payload_len = len - (int)sizeof(UDPHeader);

    // find matching socket
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].active || sockets[i].type != SOCK_DGRAM) continue;
        if (sockets[i].local_port != dst_port) continue;

        // buffer the data
        NetSocket* sock = &sockets[i];
        sock->remote_ip = src_ip;
        sock->remote_port = src_port;

        for (int j = 0; j < payload_len && sock->rx_count < TCP_RX_BUFSIZE; j++) {
            sock->rx_buf[sock->rx_tail] = payload[j];
            sock->rx_tail = (sock->rx_tail + 1) % TCP_RX_BUFSIZE;
            sock->rx_count++;
        }
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
            sockets[i].tcp_window = TCP_RX_BUFSIZE;
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
    if (s->local_port == 0) s->local_port = AllocatePort();

    if (s->type == SOCK_STREAM) {
        // tcp: send syn
        s->tcp_seq = 1000; // initial sequence number (should be random)
        s->tcp_ack = 0;
        s->tcp_state = TCP_SYN_SENT;
        SendTCP(s, TCP_FLAG_SYN, nullptr, 0);
        s->tcp_seq++;

        // wait for connection (polling)
        for (int i = 0; i < 300000; i++) {
            Tick();
            if (s->tcp_state == TCP_ESTABLISHED) return true;
            if (s->tcp_state == TCP_CLOSED) return false;
            for (volatile int d = 0; d < 100; d++);
        }
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

    // poll for incoming syn
    for (int i = 0; i < 1000000; i++) {
        Tick();
        if (sockets[sock].tcp_state == TCP_ESTABLISHED) {
            // create a new socket for this connection
            int new_sock = Socket(SOCK_STREAM);
            if (new_sock < 0) return -1;

            sockets[new_sock] = sockets[sock];
            sockets[sock].tcp_state = TCP_LISTEN; // reset listener
            sockets[sock].remote_ip = 0;
            sockets[sock].remote_port = 0;
            return new_sock;
        }
        for (volatile int d = 0; d < 100; d++);
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
            SendTCP(s, TCP_FLAG_ACK | TCP_FLAG_PSH, ptr + sent, chunk);
            s->tcp_seq += chunk;
            sent += chunk;
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
    if (s->rx_count == 0) return 0;

    uint8_t* dst = (uint8_t*)buf;
    int count = 0;
    while (count < max_len && s->rx_count > 0) {
        dst[count++] = s->rx_buf[s->rx_head];
        s->rx_head = (s->rx_head + 1) % TCP_RX_BUFSIZE;
        s->rx_count--;
    }
    return count;
}

bool TCPStack::Close(int sock) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return false;

    NetSocket* s = &sockets[sock];
    if (s->type == SOCK_STREAM && s->tcp_state == TCP_ESTABLISHED) {
        s->tcp_state = TCP_FIN_WAIT_1;
        SendTCP(s, TCP_FLAG_FIN | TCP_FLAG_ACK, nullptr, 0);
        s->tcp_seq++;
    } else if (s->type == SOCK_STREAM && s->tcp_state == TCP_CLOSE_WAIT) {
        s->tcp_state = TCP_LAST_ACK;
        SendTCP(s, TCP_FLAG_FIN | TCP_FLAG_ACK, nullptr, 0);
        s->tcp_seq++;
    }

    s->active = false;
    return true;
}

int TCPStack::SendTo(int sock, const void* data, int len, uint32_t ip, uint16_t port) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return -1;
    if (sockets[sock].type != SOCK_DGRAM) return -1;

    uint8_t buf[sizeof(UDPHeader) + ETH_MTU];
    UDPHeader* udp = (UDPHeader*)buf;

    if (sockets[sock].local_port == 0)
        sockets[sock].local_port = AllocatePort();

    udp->src_port = htons(sockets[sock].local_port);
    udp->dst_port = htons(port);
    udp->length = htons(sizeof(UDPHeader) + len);
    udp->checksum = 0; // optional for udp over ipv4

    const uint8_t* src = (const uint8_t*)data;
    for (int i = 0; i < len && i < (int)ETH_MTU; i++)
        buf[sizeof(UDPHeader) + i] = src[i];

    stats.udp_tx++;
    if (SendIPv4(ip, IP_PROTO_UDP, buf, sizeof(UDPHeader) + len))
        return len;
    return -1;
}

int TCPStack::RecvFrom(int sock, void* buf, int max_len, uint32_t* from_ip, uint16_t* from_port) {
    if (sock < 0 || sock >= MAX_SOCKETS || !sockets[sock].active) return -1;

    NetSocket* s = &sockets[sock];
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

    echo.hdr.type = ICMP_TYPE_ECHO_REQUEST;
    echo.hdr.code = 0;
    echo.hdr.identifier = htons(0x1234);
    echo.hdr.sequence = htons(1);
    echo.hdr.checksum = 0;
    for (int i = 0; i < 32; i++) echo.data[i] = (uint8_t)(i + 'A');
    echo.hdr.checksum = Checksum(&echo, sizeof(echo));

    stats.icmp_tx++;
    if (!SendIPv4(ip, IP_PROTO_ICMP, &echo, sizeof(echo)))
        return false;

    // wait for reply
    for (int i = 0; i < timeout_ms * 100; i++) {
        Tick();
        for (volatile int d = 0; d < 100; d++);
    }

    if (rtt_ms) *rtt_ms = timeout_ms; // rough approximation
    return true; // in a real impl, we'd track the reply properly
}

void TCPStack::Tick() {
    if (!initialized) return;

    // poll e1000 for received packets (callback-based)
    E1000::Poll();

    // tcp timer processing
    TCPTick();
}

void TCPStack::TCPTick() {
    // handle retransmissions, time_wait cleanup, etc.
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].active || sockets[i].type != SOCK_STREAM) continue;
        if (sockets[i].tcp_state == TCP_TIME_WAIT) {
            // eventually clean up
            sockets[i].active = false;
            sockets[i].tcp_state = TCP_CLOSED;
        }
    }
}

uint16_t TCPStack::AllocatePort() {
    uint16_t port = next_ephemeral_port++;
    if (next_ephemeral_port >= 65535) next_ephemeral_port = 49152;
    return port;
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
