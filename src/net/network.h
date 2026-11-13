#pragma once
//  kurono os - network stack & wifi driver
//  bare-metal nic abstraction + basic tcp/ip

#include "../kernel/types.h"

#define NET_MAX_INTERFACES  4
#define NET_ARP_TABLE_SIZE 32
#define NET_PACKET_SIZE  1518
#define NET_RX_RING_SIZE   32
#define NET_TX_RING_SIZE   16
#define NET_MAX_SOCKETS    16
#define NET_MAX_SSID       32
#define NET_MAX_WIFI_NETS  16

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

struct NetIPv4Header {
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

struct NetUDPHeader {
    unsigned short src_port;
    unsigned short dst_port;
    unsigned short length;
    unsigned short checksum;
};

struct NetTCPHeader {
    unsigned short src_port;
    unsigned short dst_port;
    unsigned int   seq_num;
    unsigned int   ack_num;
    unsigned short flags;
    unsigned short window;
    unsigned short checksum;
    unsigned short urgent;
};

struct NetARPEntry {
    IPv4Address ip;
    MACAddress  mac;
    bool        valid;
    unsigned int timestamp;
};

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
    int  signal_strength;     // dbm (-100 to 0)
    int  channel;
    WiFiSecurity security;
    MACAddress   bssid;
    bool connected;
};

enum NICType {
    NIC_NONE = 0,
    NIC_ETHERNET,
    NIC_WIFI,
    NIC_LOOPBACK,
    NIC_TUN,            // L3 (IP) tunnel - userspace reads/writes IP packets
    NIC_TAP,            // L2 (Ethernet) tunnel - userspace reads/writes frames
    NIC_BRIDGE,         // software L2 bridge across multiple NICs
    NIC_VLAN            // 802.1Q tagged subinterface
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

class Network {
public:
    static void Init();

    // interface management
    static NetworkInterface* GetInterface(const char* name);
    static NetworkInterface* GetInterfaces();
    static int GetInterfaceCount();
    static bool SetIP(const char* ifname, IPv4Address ip, IPv4Address mask, IPv4Address gw);

    // packet i/o
    static bool SendPacket(const char* ifname, const unsigned char* data, int len);
    static int  RecvPacket(const char* ifname, unsigned char* data, int max_len);
    // re-read the physical link of eth0 and update its up/down state, so a link
    // that comes up late (autoneg takes seconds on real hardware) is noticed. (satoru)
    static void RefreshLink();

    // ip helpers
    static IPv4Address MakeIP(unsigned char a, unsigned char b, unsigned char c, unsigned char d);
    static bool IPEquals(IPv4Address a, IPv4Address b);
    static void IPToString(IPv4Address ip, char* buf, int max);
    static IPv4Address ParseIP(const char* str);

    // arp
    static MACAddress* ARPLookup(IPv4Address ip);
    static void ARPAdd(IPv4Address ip, MACAddress mac);

    // /proc-style dumps (used by RuntimeLayout::RefreshProc).
    // Both write a NUL-terminated, human-readable table into buf and
    // return the number of bytes written (excluding the NUL).
    static int DumpARPTable(char* buf, int max);
    static int DumpRoutes(char* buf, int max);

    // sockets
    static int  SocketCreate(SocketType type);
    static bool SocketBind(int fd, unsigned short port);
    static bool SocketConnect(int fd, IPv4Address ip, unsigned short port);
    static bool SocketListen(int fd);
    static int  SocketAccept(int fd);
    static int  SocketSend(int fd, const unsigned char* data, int len);
    static int  SocketRecv(int fd, unsigned char* data, int max_len);
    static void SocketClose(int fd);

    // dns
    static bool Resolve(const char* hostname, IPv4Address* out);
    // lease-provided resolver pair, tried primary then secondary. (satoru)
    static void SetDNSServers(uint32_t primary, uint32_t secondary);
    static void GetDNSServers(uint32_t* primary, uint32_t* secondary);

private:
    static NetworkInterface interfaces[NET_MAX_INTERFACES];
    static int interface_count;
    static NetARPEntry arp_table[NET_ARP_TABLE_SIZE];
    static Socket sockets[NET_MAX_SOCKETS];
    static unsigned short next_port;
};

class WiFi {
public:
    enum LinkKind {
        LINK_NONE = 0,      // no NIC detected
        LINK_ETHERNET = 1,  // wired (e1000, virtio-net, etc.)
        LINK_WIFI = 2       // wireless NIC found on PCI bus
    };

    static void Init();
    // the KLD iwlwifi container owns the radio: skip the native probe (satoru)
    static void SetKldOwned(bool on);

    static bool Enable();
    static bool Disable();
    static bool Scan();
    static bool Connect(const char* ssid, const char* password);
    static bool Disconnect();
    // auto-rejoin after a lost/deauthenticated link, with capped backoff. called
    // from the network process loop. (satoru)
    static void Tick();
    static void SetAutoReconnect(bool on);
    static bool AutoReconnect();

    // saved (known) networks, kept encrypted by net/wifi_store. the settings
    // page and the control center read them through here, so the ring-3 build
    // can serve the same calls over the settings service. (satoru)
    static int  SavedCount();
    static bool SavedAt(int i, char* ssid, int cap, int* security, bool* autojoin);
    static bool Forget(const char* ssid);
    static bool SetAutoJoin(const char* ssid, bool on);
    // true while the KLD linux driver container owns the radio. (satoru)
    static bool KldBridge();

    static WiFiState GetState();
    static WiFiNetwork* GetNetworks();
    static int GetNetworkCount();
    static WiFiNetwork* GetConnectedNetwork();

    static int GetSignalStrength();  // current connected signal
    static const char* StateString();

    // Real link inspection (no stubs).  Walks PCI bus once and caches
    // result.  Used by the taskbar indicator and Settings panel.
    static LinkKind DetectedLink();
    static const char* LinkKindString();   // "ethernet", "wifi", "offline"
    static const char* WirelessChipName(); // e.g. "Intel iwlwifi 8260" or ""
    static bool        IsLinkUp();         // true if either NIC has carrier
    static int         SignalBars();       // 0..4 bars for the icon

    // shell integration
    static void RegisterCommands(void* shell);
    static int cmd_wifi(void* sh, int argc, const char** argv, char* out, int mx);
    static int cmd_iwconfig(void* sh, int argc, const char** argv, char* out, int mx);
    static int cmd_nmcli(void* sh, int argc, const char** argv, char* out, int mx);

private:
    static WiFiState state;
    static WiFiNetwork networks[NET_MAX_WIFI_NETS];
    static int network_count;
    static int connected_index;
    static LinkKind   detected_link;
    static char       wireless_chip[64];
    static bool       link_probed;
    static void SimulateNetworks();
    static void ProbePCIWireless();
    static bool ConnectInternal(const char* ssid, const char* password, bool notify);
    static void DropLink(const char* why);
    static bool auto_reconnect;
    static char saved_ssid[NET_MAX_SSID];
    static char saved_pass[64];
    static bool have_saved;
    static uint32_t reconnect_at_ms;
    static uint32_t reconnect_backoff_ms;
    static uint8_t  reconnect_tries;
};
