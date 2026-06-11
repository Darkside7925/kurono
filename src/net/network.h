#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Network Stack & WiFi Driver
//  Bare-metal NIC abstraction + basic TCP/IP
// ═══════════════════════════════════════════════════════════════════════════

#define NET_MAX_INTERFACES  4
#define NET_ARP_TABLE_SIZE 32
#define NET_PACKET_SIZE  1518
#define NET_RX_RING_SIZE   32
#define NET_TX_RING_SIZE   16
#define NET_MAX_SOCKETS    16
#define NET_MAX_SSID       32
#define NET_MAX_WIFI_NETS  16

// ── Ethernet ─────────────────────────────────────────────────────────────

struct MACAddress {
    unsigned char bytes[6];
};

struct IPv4Address {
    unsigned char bytes[4];
};

struct EthernetFrame {
    MACAddress dst;
    MACAddress src;
    unsigned short ethertype;
    unsigned char payload[NET_PACKET_SIZE - 14];
};

// ── IP / UDP / TCP headers ───────────────────────────────────────────────

struct IPv4Header {
    unsigned char  version_ihl;
    unsigned char  tos;
    unsigned short total_length;
    unsigned short identification;
    unsigned short flags_frag;
    unsigned char  ttl;
    unsigned char  protocol;
    unsigned short checksum;
    IPv4Address    src;
    IPv4Address    dst;
};

struct UDPHeader {
    unsigned short src_port;
    unsigned short dst_port;
    unsigned short length;
    unsigned short checksum;
};

struct TCPHeader {
    unsigned short src_port;
    unsigned short dst_port;
    unsigned int   seq_num;
    unsigned int   ack_num;
    unsigned short flags;
    unsigned short window;
    unsigned short checksum;
    unsigned short urgent;
};

// ── ARP ──────────────────────────────────────────────────────────────────

struct ARPEntry {
    IPv4Address ip;
    MACAddress  mac;
    bool        valid;
    unsigned int timestamp;
};

// ── WiFi ─────────────────────────────────────────────────────────────────

enum WiFiState {
    WIFI_OFF = 0,
    WIFI_SCANNING,
    WIFI_CONNECTING,
    WIFI_CONNECTED,
    WIFI_DISCONNECTED,
    WIFI_ERROR
};

enum WiFiSecurity {
    WIFI_OPEN = 0,
    WIFI_WEP,
    WIFI_WPA,
    WIFI_WPA2,
    WIFI_WPA3
};

struct WiFiNetwork {
    char ssid[NET_MAX_SSID];
    int  signal_strength;     // dBm (-100 to 0)
    int  channel;
    WiFiSecurity security;
    MACAddress   bssid;
    bool connected;
};

// ── Network Interface ────────────────────────────────────────────────────

enum NICType {
    NIC_NONE = 0,
    NIC_ETHERNET,
    NIC_WIFI,
    NIC_LOOPBACK
};

enum NICState {
    NIC_DOWN = 0,
    NIC_UP
};

struct NetworkInterface {
    char name[16];
    NICType type;
    NICState state;
    MACAddress mac;
    IPv4Address ip;
    IPv4Address netmask;
    IPv4Address gateway;
    IPv4Address dns;
    unsigned int rx_packets;
    unsigned int tx_packets;
    unsigned int rx_bytes;
    unsigned int tx_bytes;
    unsigned int rx_errors;
    unsigned int tx_errors;
};

// ── Socket ───────────────────────────────────────────────────────────────

enum SocketType {
    SOCK_TCP = 0,
    SOCK_UDP
};

enum SocketState {
    SOCK_CLOSED = 0,
    SOCK_LISTEN,
    SOCK_SYN_SENT,
    SOCK_ESTABLISHED,
    SOCK_CLOSE_WAIT,
    SOCK_TIME_WAIT
};

struct Socket {
    int           fd;
    SocketType    type;
    SocketState   state;
    IPv4Address   local_ip;
    unsigned short local_port;
    IPv4Address   remote_ip;
    unsigned short remote_port;
    unsigned char rx_buffer[2048];
    int           rx_len;
};

// ═══════════════════════════════════════════════════════════════════════════

class Network {
public:
    static void Init();

    // Interface management
    static NetworkInterface* GetInterface(const char* name);
    static NetworkInterface* GetInterfaces();
    static int GetInterfaceCount();
    static bool SetIP(const char* ifname, IPv4Address ip, IPv4Address mask, IPv4Address gw);

    // Packet I/O
    static bool SendPacket(const char* ifname, const unsigned char* data, int len);
    static int  RecvPacket(const char* ifname, unsigned char* data, int max_len);

    // IP helpers
    static IPv4Address MakeIP(unsigned char a, unsigned char b, unsigned char c, unsigned char d);
    static bool IPEquals(IPv4Address a, IPv4Address b);
    static void IPToString(IPv4Address ip, char* buf, int max);
    static IPv4Address ParseIP(const char* str);

    // ARP
    static MACAddress* ARPLookup(IPv4Address ip);
    static void ARPAdd(IPv4Address ip, MACAddress mac);

    // Sockets
    static int  SocketCreate(SocketType type);
    static bool SocketBind(int fd, unsigned short port);
    static bool SocketConnect(int fd, IPv4Address ip, unsigned short port);
    static bool SocketListen(int fd);
    static int  SocketAccept(int fd);
    static int  SocketSend(int fd, const unsigned char* data, int len);
    static int  SocketRecv(int fd, unsigned char* data, int max_len);
    static void SocketClose(int fd);

    // DNS
    static bool Resolve(const char* hostname, IPv4Address* out);

private:
    static NetworkInterface interfaces[NET_MAX_INTERFACES];
    static int interface_count;
    static ARPEntry arp_table[NET_ARP_TABLE_SIZE];
    static Socket sockets[NET_MAX_SOCKETS];
    static unsigned short next_port;
};

// ═══════════════════════════════════════════════════════════════════════════

class WiFi {
public:
    static void Init();

    static bool Enable();
    static bool Disable();
    static bool Scan();
    static bool Connect(const char* ssid, const char* password);
    static bool Disconnect();

    static WiFiState GetState();
    static WiFiNetwork* GetNetworks();
    static int GetNetworkCount();
    static WiFiNetwork* GetConnectedNetwork();

    static int GetSignalStrength();  // Current connected signal
    static const char* StateString();

    // Shell integration
    static void RegisterCommands(void* shell);
    static int cmd_wifi(void* sh, int argc, const char** argv, char* out, int mx);
    static int cmd_iwconfig(void* sh, int argc, const char** argv, char* out, int mx);
    static int cmd_nmcli(void* sh, int argc, const char** argv, char* out, int mx);

private:
    static WiFiState state;
    static WiFiNetwork networks[NET_MAX_WIFI_NETS];
    static int network_count;
    static int connected_index;
    static void SimulateNetworks();
};
