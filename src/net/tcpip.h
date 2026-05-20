#pragma once
//  kurono os  -  tcp/ip network stack
//  lightweight implementation over e1000 ethernet driver
//  supports: ethernet ii, arp, ipv4, icmp, udp, tcp
#include "../kernel/types.h"

#define ETH_TYPE_ARP   0x0806
#define ETH_TYPE_IPV4  0x0800
#define ETH_TYPE_IPV6  0x86DD
#define ETH_HLEN       14
#define ETH_MTU        1500
#define ETH_FRAME_MAX  (ETH_MTU + ETH_HLEN)

struct EthernetHeader {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;     // big-endian
} __attribute__((packed));

#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

struct ARPHeader {
    uint16_t hw_type;       // 1 = ethernet
    uint16_t proto_type;    // 0x0800 = ipv4
    uint8_t  hw_len;        // 6 for mac
    uint8_t  proto_len;     // 4 for ipv4
    uint16_t opcode;
    uint8_t  sender_mac[6];
    uint8_t  sender_ip[4];
    uint8_t  target_mac[6];
    uint8_t  target_ip[4];
} __attribute__((packed));

#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17

struct IPv4Header {
    uint8_t  ver_ihl;       // version (4) | ihl (5)
    uint8_t  tos;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed));

#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8

struct ICMPHeader {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
} __attribute__((packed));

struct UDPHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

#define TCP_FLAG_FIN  0x01
#define TCP_FLAG_SYN  0x02
#define TCP_FLAG_RST  0x04
#define TCP_FLAG_PSH  0x08
#define TCP_FLAG_ACK  0x10
#define TCP_FLAG_URG  0x20

struct TCPHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;   // (offset/4) << 4
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

#define ARP_CACHE_SIZE 32

struct ARPEntry {
    uint32_t ip;
    uint8_t  mac[6];
    bool     valid;
    uint32_t timestamp;
};

enum TCPState {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
};

#define SOCK_STREAM    1   // tcp
#define SOCK_DGRAM     2   // udp
#define SOCK_RAW       3   // raw ip

#define MAX_SOCKETS    16
#define TCP_RX_BUFSIZE 8192
#define TCP_TX_BUFSIZE 8192
#define TCP_MSS        1460
#define PENDING_IPV4_TX 8

struct PendingIPv4Frame {
    bool     active;
    uint32_t next_hop;
    uint16_t len;
    uint32_t last_arp_ms;
    uint8_t  arp_retries;
    uint8_t  packet[ETH_MTU];
};

struct NetSocket {
    bool     active;
    int      type;          // sock_stream, sock_dgram, sock_raw
    uint32_t local_ip;
    uint16_t local_port;
    uint32_t remote_ip;
    uint16_t remote_port;

    // tcp state
    TCPState tcp_state;
    uint32_t tcp_seq;       // our sequence number
    uint32_t tcp_ack;       // their sequence number
    uint16_t tcp_window;

    // receive buffer
    uint8_t  rx_buf[TCP_RX_BUFSIZE];
    int      rx_head;
    int      rx_tail;
    int      rx_count;

    // transmit state
    uint32_t tx_unacked;
    uint32_t retransmit_timer;
    bool     tx_pending;
    uint8_t  tx_flags;
    uint8_t  tx_retries;
    int      tx_len;
    uint32_t tx_seq_base;
    uint32_t tx_seq_end;
    uint32_t tx_last_tx_ms;
    uint8_t  tx_buf[TCP_MSS];
};

struct NetStats {
    uint32_t packets_rx;
    uint32_t packets_tx;
    uint32_t bytes_rx;
    uint32_t bytes_tx;
    uint32_t errors_rx;
    uint32_t errors_tx;
    uint32_t arp_requests;
    uint32_t arp_replies;
    uint32_t icmp_rx;
    uint32_t icmp_tx;
    uint32_t tcp_rx;
    uint32_t tcp_tx;
    uint32_t udp_rx;
    uint32_t udp_tx;
    uint32_t dropped;
};

class TCPStack {
public:
    static bool Init();
    static bool IsUp();

    // configuration
    static void SetIP(uint32_t ip);
    static void SetSubnetMask(uint32_t mask);
    static void SetGateway(uint32_t gw);
    static void SetDNS(uint32_t dns);
    static uint32_t GetIP();
    static uint32_t GetSubnetMask();
    static uint32_t GetGateway();
    static const uint8_t* GetMAC();

    // ip helpers
    static uint32_t MakeIP(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
    static void FormatIP(uint32_t ip, char* out);

    // socket api
    static int  Socket(int type);
    static bool Bind(int sock, uint16_t port);
    static bool Connect(int sock, uint32_t ip, uint16_t port);
    static bool Listen(int sock, int backlog);
    static int  Accept(int sock);
    static int  Send(int sock, const void* data, int len);
    static int  Recv(int sock, void* buf, int max_len);
    static bool IsPeerClosed(int sock);
    static bool Close(int sock);

    // udp convenience
    static int  SendTo(int sock, const void* data, int len, uint32_t ip, uint16_t port);
    static int  RecvFrom(int sock, void* buf, int max_len, uint32_t* from_ip, uint16_t* from_port);

    // icmp
    static bool Ping(uint32_t ip, int timeout_ms, int* rtt_ms);

    // packet processing (called from driver/interrupt handler)
    static void ProcessRxPacket(const void* data, int length);
    static void Tick();

    // statistics
    static const NetStats& GetStats();

    static void DumpInfo(char* out, int max_len);

private:
    static bool initialized;
    static uint32_t local_ip;
    static uint32_t subnet_mask;
    static uint32_t gateway;
    static uint32_t dns_server;
    static uint8_t  mac[6];

    static ARPEntry arp_cache[ARP_CACHE_SIZE];
    static NetSocket sockets[MAX_SOCKETS];
    static NetStats stats;
    static PendingIPv4Frame pending_ipv4[PENDING_IPV4_TX];

    static uint16_t next_ephemeral_port;
    static uint16_t ip_ident;

    // byte-order helpers
    static uint16_t htons(uint16_t val);
    static uint32_t htonl(uint32_t val);
    static uint16_t ntohs(uint16_t val);
    static uint32_t ntohl(uint32_t val);

    // checksum
    static uint16_t Checksum(const void* data, int len);
    static uint16_t TCPChecksum(const IPv4Header* ip, const TCPHeader* tcp, const void* payload, int payload_len);

    // arp
    static bool ARPLookup(uint32_t ip, uint8_t* mac_out);
    static void ARPRequest(uint32_t ip);
    static void ProcessARP(const void* data, int len);

    // ip
    static void ProcessIPv4(const void* data, int len);
    static bool SendIPv4(uint32_t dst_ip, uint8_t proto, const void* payload, int len);
    static bool QueuePendingIPv4(uint32_t next_hop, const void* packet, int len);
    static void ServicePendingIPv4();

    // icmp
    static void ProcessICMP(const IPv4Header* ip_hdr, const void* data, int len);

    // tcp
    static void ProcessTCP(const IPv4Header* ip_hdr, const void* data, int len);
    static bool SendTCP(NetSocket* sock, uint8_t flags, const void* data, int len);
    static bool SendTCPPacket(NetSocket* sock, uint8_t flags, const void* data, int len,
                              uint32_t seq, bool track_pending);
    static uint32_t TCPSeqAdvance(uint8_t flags, int len);
    static void TCPTick();

    // udp
    static void ProcessUDP(const IPv4Header* ip_hdr, const void* data, int len);

    // raw send
    static bool SendEthernet(const uint8_t* dst_mac, uint16_t ethertype, const void* payload, int len);

    // port allocation
    static uint16_t AllocatePort();
};
