#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Linux Network Stack Bridge
//  Shares the network stack between Kurono and Linux subsystem.
//  Linux programs see standard socket APIs; the bridge translates
//  to Kurono's native network stack.
//
//  Provides:
//  - socket(), bind(), listen(), accept(), connect()
//  - send(), recv(), sendto(), recvfrom()
//  - /etc/resolv.conf integration
//  - ifconfig/ip addr output
//  - Shared DNS resolution
// ═══════════════════════════════════════════════════════════════════════════

#include "../kernel/types.h"

// ─── Socket types ───────────────────────────────────────────────────────

#define LNET_MAX_SOCKETS   16
#define LNET_MAX_BACKLOG    8
#define LNET_BUF_SIZE     4096

// Address families
#define LAF_UNSPEC   0
#define LAF_UNIX     1     // Unix domain socket
#define LAF_INET     2     // IPv4
#define LAF_INET6    10    // IPv6

// Socket types
#define LSOCK_STREAM  1    // TCP
#define LSOCK_DGRAM   2    // UDP
#define LSOCK_RAW     3    // Raw

// Protocol
#define LIPPROTO_TCP  6
#define LIPPROTO_UDP  17

// Socket options
#define LSOL_SOCKET   1
#define LSO_REUSEADDR 2
#define LSO_KEEPALIVE 9
#define LSO_RCVTIMEO  20
#define LSO_SNDTIMEO  21

// Shutdown how
#define LSHUT_RD     0
#define LSHUT_WR     1
#define LSHUT_RDWR   2

// sockaddr_in
struct LinuxSockaddrIn {
    uint16_t sin_family;
    uint16_t sin_port;       // Network byte order
    uint32_t sin_addr;       // Network byte order
    uint8_t  sin_zero[8];
} __attribute__((packed));

// sockaddr_un (Unix domain)
struct LinuxSockaddrUn {
    uint16_t sun_family;
    char     sun_path[108];
} __attribute__((packed));

// Generic sockaddr
struct LinuxSockaddr {
    uint16_t sa_family;
    char     sa_data[14];
} __attribute__((packed));

// ─── Socket state ───────────────────────────────────────────────────────

enum LinuxSocketState {
    LSOCK_CLOSED = 0,
    LSOCK_CREATED,
    LSOCK_BOUND,
    LSOCK_LISTENING,
    LSOCK_CONNECTED,
    LSOCK_CONNECTING
};

struct LinuxSocket {
    int              fd;
    int              family;
    int              type;
    int              protocol;
    LinuxSocketState state;
    bool             active;

    // Local address
    uint32_t         local_addr;
    uint16_t         local_port;

    // Remote address (for connected sockets)
    uint32_t         remote_addr;
    uint16_t         remote_port;

    // Options
    bool             reuse_addr;
    bool             keepalive;
    int              backlog;

    // Buffers
    char             recv_buf[LNET_BUF_SIZE];
    int              recv_len;
    char             send_buf[LNET_BUF_SIZE];
    int              send_len;

    // Underlying Kurono socket (if mapped)
    int              kurono_socket;

    // Owner
    int              owner_pid;
};

// ─── Network interface info ─────────────────────────────────────────────

#define LNET_MAX_INTERFACES  4

struct LinuxNetInterface {
    char     name[16];       // e.g., "eth0", "lo"
    uint32_t ip_addr;        // IPv4
    uint32_t netmask;
    uint32_t gateway;
    uint32_t broadcast;
    uint8_t  mac[6];
    bool     up;
    bool     loopback;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t tx_packets;
    int      mtu;
};

// ═══════════════════════════════════════════════════════════════════════════
//  LinuxNetBridge — Socket and network bridge
// ═══════════════════════════════════════════════════════════════════════════

class LinuxNetBridge {
public:
    static void Init();

    // ── BSD Socket API ────────────────────────────────────────────────
    static int  Socket(int family, int type, int protocol, int owner_pid);
    static int  Bind(int sockfd, const LinuxSockaddrIn* addr);
    static int  Listen(int sockfd, int backlog);
    static int  Accept(int sockfd, LinuxSockaddrIn* addr);
    static int  Connect(int sockfd, const LinuxSockaddrIn* addr);
    static int  Send(int sockfd, const void* buf, int len, int flags);
    static int  Recv(int sockfd, void* buf, int len, int flags);
    static int  Sendto(int sockfd, const void* buf, int len, int flags,
                        const LinuxSockaddrIn* dest_addr);
    static int  Recvfrom(int sockfd, void* buf, int len, int flags,
                          LinuxSockaddrIn* src_addr);
    static int  Shutdown(int sockfd, int how);
    static int  Close(int sockfd);
    static int  Setsockopt(int sockfd, int level, int optname,
                            const void* optval, int optlen);
    static int  Getsockopt(int sockfd, int level, int optname,
                            void* optval, int* optlen);
    static int  Getpeername(int sockfd, LinuxSockaddrIn* addr);
    static int  Getsockname(int sockfd, LinuxSockaddrIn* addr);

    // ── Network interface management ──────────────────────────────────
    static void InitInterfaces();
    static LinuxNetInterface* GetInterface(const char* name);
    static LinuxNetInterface* GetInterfaces();
    static int  GetInterfaceCount();
    static void SetInterfaceIP(const char* name, uint32_t ip, uint32_t mask);
    static void SetInterfaceUp(const char* name, bool up);

    // ── DNS resolution ────────────────────────────────────────────────
    static int  Resolve(const char* hostname, uint32_t* ip_out);
    static void SetDNS(uint32_t primary, uint32_t secondary);
    static void GetDNS(uint32_t* primary, uint32_t* secondary);

    // ── ifconfig / ip addr output ─────────────────────────────────────
    static void Ifconfig(char* out, int max_out);
    static void IpAddr(char* out, int max_out);
    static void Netstat(char* out, int max_out);

    // ── Byte-order helpers ────────────────────────────────────────────
    static uint16_t Htons(uint16_t v);
    static uint16_t Ntohs(uint16_t v);
    static uint32_t Htonl(uint32_t v);
    static uint32_t Ntohl(uint32_t v);
    static void     IpToStr(uint32_t ip, char* buf, int max);

    // ── Shell commands ────────────────────────────────────────────────
    static void RegisterShellCommands(void* shell);

private:
    static LinuxSocket       sockets[LNET_MAX_SOCKETS];
    static int               socket_count;
    static LinuxNetInterface interfaces[LNET_MAX_INTERFACES];
    static int               iface_count;
    static uint32_t          dns_primary;
    static uint32_t          dns_secondary;

    // Find free socket
    static int AllocSocket();
    static LinuxSocket* GetSocket(int fd);

    // Shell handlers
    static int cmd_ifconfig(void* sh, int argc, const char** argv,
                             char* out, int mx);
    static int cmd_ip(void* sh, int argc, const char** argv,
                       char* out, int mx);
    static int cmd_netstat(void* sh, int argc, const char** argv,
                            char* out, int mx);
    static int cmd_ping(void* sh, int argc, const char** argv,
                         char* out, int mx);
    static int cmd_nslookup(void* sh, int argc, const char** argv,
                             char* out, int mx);

    // Helpers
    static int pa(char* out, int pos, int mx, const char* s);
    static int pd(char* out, int pos, int mx, int val);
};
