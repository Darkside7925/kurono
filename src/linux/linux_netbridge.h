#pragma once
//  kurono os  -  linux network stack bridge
//  shares the network stack between kurono and linux subsystem.
//  linux programs see standard socket apis; the bridge translates
//  to kurono's native network stack.
//
//  provides:
//  - socket(), bind(), listen(), accept(), connect()
//  - send(), recv(), sendto(), recvfrom()
//  - /etc/resolv.conf integration
//  - ifconfig/ip addr output
//  - shared dns resolution

#include "../kernel/types.h"

#define LNET_MAX_SOCKETS   16
#define LNET_MAX_BACKLOG    8
#define LNET_BUF_SIZE     4096

// address families
#define LAF_UNSPEC   0
#define LAF_UNIX     1     // unix domain socket
#define LAF_INET     2     // ipv4
#define LAF_INET6    10    // ipv6

// socket types
#define LSOCK_STREAM  1    // tcp
#define LSOCK_DGRAM   2    // udp
#define LSOCK_RAW     3    // raw

// protocol
#define LIPPROTO_TCP  6
#define LIPPROTO_UDP  17

// socket options
#define LSOL_SOCKET   1
#define LSO_REUSEADDR 2
#define LSO_KEEPALIVE 9
#define LSO_RCVTIMEO  20
#define LSO_SNDTIMEO  21

// shutdown how
#define LSHUT_RD     0
#define LSHUT_WR     1
#define LSHUT_RDWR   2

// sockaddr_in
struct LinuxSockaddrIn {
    uint16_t sin_family;
    uint16_t sin_port;       // network byte order
    uint32_t sin_addr;       // network byte order
    uint8_t  sin_zero[8];
} __attribute__((packed));

// sockaddr_un (unix domain)
struct LinuxSockaddrUn {
    uint16_t sun_family;
    char     sun_path[108];
} __attribute__((packed));

// generic sockaddr
struct LinuxSockaddr {
    uint16_t sa_family;
    char     sa_data[14];
} __attribute__((packed));

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

    // local address
    uint32_t         local_addr;
    uint16_t         local_port;

    // remote address (for connected sockets)
    uint32_t         remote_addr;
    uint16_t         remote_port;

    // options
    bool             reuse_addr;
    bool             keepalive;
    int              backlog;

    // buffers
    char             recv_buf[LNET_BUF_SIZE];
    int              recv_len;
    char             send_buf[LNET_BUF_SIZE];
    int              send_len;

    // underlying kurono socket (if mapped)
    int              kurono_socket;

    // owner
    int              owner_pid;
};

#define LNET_MAX_INTERFACES  4

struct LinuxNetInterface {
    char     name[16];       // e.g., "eth0", "lo"
    uint32_t ip_addr;        // ipv4
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

//  linuxnetbridge  -  socket and network bridge

class LinuxNetBridge {
public:
    static void Init();

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

    static void InitInterfaces();
    static LinuxNetInterface* GetInterface(const char* name);
    static LinuxNetInterface* GetInterfaces();
    static int  GetInterfaceCount();
    static void SetInterfaceIP(const char* name, uint32_t ip, uint32_t mask);
    static void SetInterfaceUp(const char* name, bool up);

    static int  Resolve(const char* hostname, uint32_t* ip_out);
    static void SetDNS(uint32_t primary, uint32_t secondary);
    static void GetDNS(uint32_t* primary, uint32_t* secondary);

    static void Ifconfig(char* out, int max_out);
    static void IpAddr(char* out, int max_out);
    static void Netstat(char* out, int max_out);

    static uint16_t Htons(uint16_t v);
    static uint16_t Ntohs(uint16_t v);
    static uint32_t Htonl(uint32_t v);
    static uint32_t Ntohl(uint32_t v);
    static void     IpToStr(uint32_t ip, char* buf, int max);

    static void RegisterShellCommands(void* shell);

private:
    static LinuxSocket       sockets[LNET_MAX_SOCKETS];
    static int               socket_count;
    static LinuxNetInterface interfaces[LNET_MAX_INTERFACES];
    static int               iface_count;
    static uint32_t          dns_primary;
    static uint32_t          dns_secondary;

    // find free socket
    static int AllocSocket();
    static LinuxSocket* GetSocket(int fd);

    // shell handlers
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

    // helpers
    static int pa(char* out, int pos, int mx, const char* s);
    static int pd(char* out, int pos, int mx, int val);
};
