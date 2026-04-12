#include "network.h"
#include "../kernel/time.h"
#include "../shell/shell.h"
#include "../drivers/serial.h"
#include "../drivers/e1000.h"
#include "../hal/hal.h"

//  network stack implementation

NetworkInterface Network::interfaces[NET_MAX_INTERFACES];
int Network::interface_count = 0;
ARPEntry Network::arp_table[NET_ARP_TABLE_SIZE];
Socket Network::sockets[NET_MAX_SOCKETS];
unsigned short Network::next_port = 49152;

static int nlen(const char* s) { int n=0; while (s[n]) n++; return n; }
static void ncpy(char* d, const char* s, int m) { int i=0; while (s[i]&&i<m-1) { d[i]=s[i]; i++; } d[i]=0; }
static bool neq(const char* a, const char* b) { while (*a&&*b) { if(*a!=*b) return false; a++; b++; } return *a==*b; }
static int na(char* b, int p, int m, const char* s) { while (*s&&p<m-1) b[p++]=*s++; b[p]=0; return p; }
static int nac(char* b, int p, int m, char c) { if (p<m-1) {b[p++]=c; b[p]=0;} return p; }
static int nai(char* b, int p, int m, int v) {
    if (v<0) { p=nac(b,p,m,'-'); v=-v; }
    if (v==0) return nac(b,p,m,'0');
    char t[12]; int ti=0; while (v>0) { t[ti++]='0'+(v%10); v/=10; } while (ti>0) p=nac(b,p,m,t[--ti]); return p;
}
static int nau(char* b, int p, int m, unsigned int v) {
    if (v==0) return nac(b,p,m,'0');
    char t[12]; int ti=0; while (v>0) { t[ti++]='0'+(v%10); v/=10; } while (ti>0) p=nac(b,p,m,t[--ti]); return p;
}
static void nmemset(void* p, int v, int n) {
    unsigned char* b = (unsigned char*)p;
    for (int i = 0; i < n; i++) b[i] = (unsigned char)v;
}

static void e1000_rx_handler(const uint8_t* data, uint16_t length) {
    // simple packet processing  -  update interface stats
    NetworkInterface* eth = Network::GetInterface("eth0");
    if (eth) {
        eth->rx_packets++;
        eth->rx_bytes += length;
    }

    // todo: parse ethernet frame, dispatch to ip/arp/etc.
    // for now, arp responses and dhcp would be processed here
    if (length >= 14) {
        uint16_t ethertype = ((uint16_t)data[12] << 8) | data[13];
        if (ethertype == 0x0806) {
            // arp response  -  extract sender ip and mac
            if (length >= 42) {
                MACAddress sender_mac;
                for (int i = 0; i < 6; i++) sender_mac.bytes[i] = data[22 + i];
                IPv4Address sender_ip = Network::MakeIP(data[28], data[29], data[30], data[31]);
                Network::ARPAdd(sender_ip, sender_mac);
            }
        }
    }
}

void Network::Init() {
    interface_count = 0;
    nmemset(arp_table, 0, sizeof(arp_table));
    nmemset(sockets, 0, sizeof(sockets));
    next_port = 49152;

    // create loopback interface
    NetworkInterface& lo = interfaces[interface_count++];
    ncpy(lo.name, "lo", 16);
    lo.type = NIC_LOOPBACK;
    lo.state = NIC_UP;
    nmemset(&lo.mac, 0, 6);
    lo.ip = MakeIP(127, 0, 0, 1);
    lo.netmask = MakeIP(255, 0, 0, 0);
    lo.gateway = MakeIP(0, 0, 0, 0);
    lo.dns = MakeIP(0, 0, 0, 0);
    lo.rx_packets = lo.tx_packets = 0;
    lo.rx_bytes = lo.tx_bytes = 0;
    lo.rx_errors = lo.tx_errors = 0;

    bool have_real_nic = E1000::Init();

    // create eth0  -  backed by real e1000 if detected
    NetworkInterface& eth = interfaces[interface_count++];
    ncpy(eth.name, "eth0", 16);
    eth.type = NIC_ETHERNET;
    eth.state = NIC_UP;
    if (have_real_nic) {
        E1000::GetMAC(eth.mac.bytes);
        E1000::SetPacketHandler(e1000_rx_handler);
        // qemu user-mode networking: guest gets 10.0.2.x by default
        eth.ip = MakeIP(10, 0, 2, 15);
        eth.netmask = MakeIP(255, 255, 255, 0);
        eth.gateway = MakeIP(10, 0, 2, 2);
        eth.dns = MakeIP(10, 0, 2, 3);
        SerialLogger::Log("Network: eth0 backed by real E1000 NIC\r\n");
    } else {
        // fallback: simulated
        eth.mac.bytes[0] = 0x00; eth.mac.bytes[1] = 0x1A;
        eth.mac.bytes[2] = 0x2B; eth.mac.bytes[3] = 0x3C;
        eth.mac.bytes[4] = 0x4D; eth.mac.bytes[5] = 0x5E;
        eth.ip = MakeIP(192, 168, 1, 100);
        eth.netmask = MakeIP(255, 255, 255, 0);
        eth.gateway = MakeIP(192, 168, 1, 1);
        eth.dns = MakeIP(8, 8, 8, 8);
        SerialLogger::Log("Network: eth0 simulated (no E1000 found)\r\n");
    }
    eth.rx_packets = eth.tx_packets = 0;
    eth.rx_bytes = eth.tx_bytes = 0;
    eth.rx_errors = eth.tx_errors = 0;

    // create wlan0  -  wifi interface (bridged through eth0 in qemu)
    NetworkInterface& wlan = interfaces[interface_count++];
    ncpy(wlan.name, "wlan0", 16);
    wlan.type = NIC_WIFI;
    wlan.state = NIC_UP;
    // wifi shares the nic mac with locally-administered bit set
    for (int i = 0; i < 6; i++) wlan.mac.bytes[i] = eth.mac.bytes[i];
    wlan.mac.bytes[0] |= 0x02;  // locally administered
    wlan.mac.bytes[5] ^= 0x01;  // different from eth0
    wlan.ip = eth.ip;            // same ip as eth0 (bridged)
    wlan.netmask = eth.netmask;
    wlan.gateway = eth.gateway;
    wlan.dns = eth.dns;
    wlan.rx_packets = wlan.tx_packets = 0;
    wlan.rx_bytes = wlan.tx_bytes = 0;
    wlan.rx_errors = wlan.tx_errors = 0;

    // add gateway to arp
    ARPAdd(eth.gateway, {{0x52, 0x55, 0x0A, 0x00, 0x02, 0x02}});

    SerialLogger::Log("Network: Stack initialized (lo, eth0, wlan0)\r\n");
}

NetworkInterface* Network::GetInterface(const char* name) {
    for (int i = 0; i < interface_count; i++) {
        if (neq(interfaces[i].name, name)) return &interfaces[i];
    }
    return nullptr;
}

NetworkInterface* Network::GetInterfaces() { return interfaces; }
int Network::GetInterfaceCount() { return interface_count; }

bool Network::SetIP(const char* ifname, IPv4Address ip, IPv4Address mask, IPv4Address gw) {
    NetworkInterface* iface = GetInterface(ifname);
    if (!iface) return false;
    iface->ip = ip;
    iface->netmask = mask;
    iface->gateway = gw;
    return true;
}

IPv4Address Network::MakeIP(unsigned char a, unsigned char b, unsigned char c, unsigned char d) {
    IPv4Address ip;
    ip.bytes[0] = a; ip.bytes[1] = b; ip.bytes[2] = c; ip.bytes[3] = d;
    return ip;
}

bool Network::IPEquals(IPv4Address a, IPv4Address b) {
    return a.bytes[0] == b.bytes[0] && a.bytes[1] == b.bytes[1] &&
           a.bytes[2] == b.bytes[2] && a.bytes[3] == b.bytes[3];
}

void Network::IPToString(IPv4Address ip, char* buf, int max) {
    int p = 0;
    for (int i = 0; i < 4; i++) {
        if (i > 0) p = nac(buf, p, max, '.');
        p = nau(buf, p, max, ip.bytes[i]);
    }
}

IPv4Address Network::ParseIP(const char* str) {
    IPv4Address ip = {{0,0,0,0}};
    int octet = 0, val = 0;
    for (int i = 0; str[i] && octet < 4; i++) {
        if (str[i] == '.') {
            ip.bytes[octet++] = (unsigned char)val;
            val = 0;
        } else if (str[i] >= '0' && str[i] <= '9') {
            val = val * 10 + (str[i] - '0');
        }
    }
    if (octet < 4) ip.bytes[octet] = (unsigned char)val;
    return ip;
}

MACAddress* Network::ARPLookup(IPv4Address ip) {
    for (int i = 0; i < NET_ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && IPEquals(arp_table[i].ip, ip))
            return &arp_table[i].mac;
    }
    return nullptr;
}

void Network::ARPAdd(IPv4Address ip, MACAddress mac) {
    // update existing
    for (int i = 0; i < NET_ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && IPEquals(arp_table[i].ip, ip)) {
            arp_table[i].mac = mac;
            arp_table[i].timestamp = Time::GetTicks();
            return;
        }
    }
    // add new
    for (int i = 0; i < NET_ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid) {
            arp_table[i].ip = ip;
            arp_table[i].mac = mac;
            arp_table[i].valid = true;
            arp_table[i].timestamp = Time::GetTicks();
            return;
        }
    }
}

bool Network::SendPacket(const char* ifname, const unsigned char* data, int len) {
    NetworkInterface* iface = GetInterface(ifname);
    if (!iface || iface->state != NIC_UP) return false;

    bool sent = false;
    if (iface->type == NIC_ETHERNET || iface->type == NIC_WIFI) {
        // use real e1000 nic if available
        if (E1000::IsDetected()) {
            sent = E1000::Send(data, (uint16_t)len);
        }
    }

    if (sent || iface->type == NIC_LOOPBACK) {
        iface->tx_packets++;
        iface->tx_bytes += (unsigned int)len;
    }
    return sent || (iface->type == NIC_LOOPBACK);
}

int Network::RecvPacket(const char* ifname, unsigned char* data, int max_len) {
    NetworkInterface* iface = GetInterface(ifname);
    if (!iface || iface->state != NIC_UP) return -1;

    // e1000 packets are delivered via callback (e1000_rx_handler)
    // poll the nic to process any pending packets
    if (E1000::IsDetected()) {
        E1000::Poll();
    }

    (void)data; (void)max_len;
    return 0;
}

int Network::SocketCreate(SocketType type) {
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (sockets[i].state == SOCK_CLOSED) {
            sockets[i].fd = i;
            sockets[i].type = type;
            sockets[i].state = SOCK_CLOSED;
            sockets[i].local_port = next_port++;
            sockets[i].rx_len = 0;
            return i;
        }
    }
    return -1;
}

bool Network::SocketBind(int fd, unsigned short port) {
    if (fd < 0 || fd >= NET_MAX_SOCKETS) return false;
    sockets[fd].local_port = port;
    return true;
}

bool Network::SocketConnect(int fd, IPv4Address ip, unsigned short port) {
    if (fd < 0 || fd >= NET_MAX_SOCKETS) return false;
    sockets[fd].remote_ip = ip;
    sockets[fd].remote_port = port;
    sockets[fd].state = SOCK_ESTABLISHED;
    return true;
}

bool Network::SocketListen(int fd) {
    if (fd < 0 || fd >= NET_MAX_SOCKETS) return false;
    sockets[fd].state = SOCK_LISTEN;
    return true;
}

int Network::SocketAccept(int fd) {
    (void)fd;
    return -1; // no incoming connections in simulation
}

int Network::SocketSend(int fd, const unsigned char* data, int len) {
    if (fd < 0 || fd >= NET_MAX_SOCKETS) return -1;
    if (sockets[fd].state != SOCK_ESTABLISHED) return -1;
    (void)data;
    return len; // simulated send
}

int Network::SocketRecv(int fd, unsigned char* data, int max_len) {
    if (fd < 0 || fd >= NET_MAX_SOCKETS) return -1;
    (void)data; (void)max_len;
    return 0;
}

void Network::SocketClose(int fd) {
    if (fd < 0 || fd >= NET_MAX_SOCKETS) return;
    sockets[fd].state = SOCK_CLOSED;
}

// simulates dns resolution with a built-in host table + fallback.
// in a real os with nic driver, this would construct udp packets to 8.8.8.8:53.

// built-in dns cache for common hosts
struct DNSEntry {
    const char* hostname;
    uint8_t a, b, c, d;
};

static const DNSEntry dns_cache[] = {
    {"localhost",          127, 0, 0, 1},
    {"gateway",            192, 168, 1, 1},
    {"router",             192, 168, 1, 1},
    {"google.com",         142, 250, 80, 46},
    {"www.google.com",     142, 250, 80, 46},
    {"example.com",        93, 184, 216, 34},
    {"www.example.com",    93, 184, 216, 34},
    {"github.com",         140, 82, 121, 3},
    {"www.github.com",     140, 82, 121, 3},
    {"cloudflare.com",     104, 16, 132, 229},
    {"dns.google",         8, 8, 8, 8},
    {"one.one.one.one",    1, 1, 1, 1},
    {"satorut.com",        185, 199, 110, 20},
    {"server.satorut.com", 185, 199, 110, 21},
    {"kurono.local",       192, 168, 1, 100},
    {nullptr, 0, 0, 0, 0}
};

bool Network::Resolve(const char* hostname, IPv4Address* out) {
    if (!hostname || !out) return false;

    // check built-in cache
    for (int i = 0; dns_cache[i].hostname; i++) {
        if (neq(hostname, dns_cache[i].hostname)) {
            *out = MakeIP(dns_cache[i].a, dns_cache[i].b, dns_cache[i].c, dns_cache[i].d);
            return true;
        }
    }

    // check if input is already an ip address (simple check for digits and dots)
    bool is_ip = true;
    int dots = 0;
    for (const char* p = hostname; *p; p++) {
        if (*p == '.') { dots++; }
        else if (*p < '0' || *p > '9') { is_ip = false; break; }
    }
    if (is_ip && dots == 3) {
        // parse ip directly
        uint8_t octets[4] = {0};
        int oi = 0;
        int val = 0;
        for (const char* p = hostname; ; p++) {
            if (*p >= '0' && *p <= '9') {
                val = val * 10 + (*p - '0');
            }
            if (*p == '.' || *p == 0) {
                if (oi < 4) octets[oi++] = (uint8_t)val;
                val = 0;
                if (*p == 0) break;
            }
        }
        *out = MakeIP(octets[0], octets[1], octets[2], octets[3]);
        return true;
    }

    // unknown host  -  return a deterministic hash-based ip
    // this simulates "resolving" any hostname to a consistent address
    uint32_t hash = 0;
    for (const char* p = hostname; *p; p++) {
        hash = hash * 31 + (uint8_t)*p;
    }
    *out = MakeIP(10, (uint8_t)((hash >> 16) & 0xFF),
                      (uint8_t)((hash >> 8) & 0xFF),
                      (uint8_t)(hash & 0xFF));
    return true;
}

//  wifi driver implementation

WiFiState WiFi::state = WIFI_OFF;
WiFiNetwork WiFi::networks[NET_MAX_WIFI_NETS];
int WiFi::network_count = 0;
int WiFi::connected_index = -1;

void WiFi::SimulateNetworks() {
    network_count = 0;
    connected_index = -1;

    // only show the real e1000 nic as a wired bridge  -  no fake wifi networks
    if (E1000::IsDetected() && E1000::IsLinkUp()) {
        WiFiNetwork& n = networks[network_count++];
        ncpy(n.ssid, "eth0-bridge", NET_MAX_SSID);
        n.signal_strength = -20;
        n.channel = 0;
        n.security = WIFI_OPEN;
        n.connected = true;
        // use the real e1000 mac address
        uint8_t mac[6];
        E1000::GetMAC(mac);
        for (int i = 0; i < 6; i++) n.bssid.bytes[i] = mac[i];
        connected_index = 0;
    }
    // no simulated wifi networks  -  qemu does not emulate wifi hardware
}

void WiFi::Init() {
    state = WIFI_DISCONNECTED;
    network_count = 0;
    connected_index = -1;

    // if e1000 is available and link is up, auto-connect wifi state
    if (E1000::IsDetected() && E1000::IsLinkUp()) {
        state = WIFI_CONNECTED;
        Scan();  // populate network list
        SerialLogger::Log("WiFi: Driver initialized (E1000 bridge active)\r\n");
    } else {
        SerialLogger::Log("WiFi: Driver initialized (virtual mode)\r\n");
    }
}

bool WiFi::Enable() {
    if (state == WIFI_OFF) state = WIFI_DISCONNECTED;
    return true;
}

bool WiFi::Disable() {
    if (connected_index >= 0) Disconnect();
    state = WIFI_OFF;
    return true;
}

bool WiFi::Scan() {
    if (state == WIFI_OFF) return false;
    state = WIFI_SCANNING;
    SimulateNetworks();
    state = connected_index >= 0 ? WIFI_CONNECTED : WIFI_DISCONNECTED;
    return true;
}

bool WiFi::Connect(const char* ssid, const char* password) {
    (void)password;
    if (state == WIFI_OFF) return false;

    // find network
    for (int i = 0; i < network_count; i++) {
        if (neq(networks[i].ssid, ssid)) {
            if (connected_index >= 0) networks[connected_index].connected = false;
            networks[i].connected = true;
            connected_index = i;
            state = WIFI_CONNECTED;
            return true;
        }
    }
    return false;
}

bool WiFi::Disconnect() {
    if (connected_index >= 0) {
        networks[connected_index].connected = false;
        connected_index = -1;
    }
    state = WIFI_DISCONNECTED;
    return true;
}

WiFiState WiFi::GetState() { return state; }
WiFiNetwork* WiFi::GetNetworks() { return networks; }
int WiFi::GetNetworkCount() { return network_count; }

WiFiNetwork* WiFi::GetConnectedNetwork() {
    return connected_index >= 0 ? &networks[connected_index] : nullptr;
}

int WiFi::GetSignalStrength() {
    return connected_index >= 0 ? networks[connected_index].signal_strength : -100;
}

const char* WiFi::StateString() {
    switch (state) {
        case WIFI_OFF: return "OFF";
        case WIFI_SCANNING: return "Scanning...";
        case WIFI_CONNECTING: return "Connecting...";
        case WIFI_CONNECTED: return "Connected";
        case WIFI_DISCONNECTED: return "Disconnected";
        case WIFI_ERROR: return "Error";
    }
    return "Unknown";
}

void WiFi::RegisterCommands(void* shell_ptr) {
    KuronoShell* sh = (KuronoShell*)shell_ptr;
    sh->RegisterCommand("wifi",     "WiFi management",         ENV_KURONO, "network", (ShellCmdHandler)cmd_wifi);
    sh->RegisterCommand("iwconfig", "Wireless config (Linux)", ENV_LINUX,  "network", (ShellCmdHandler)cmd_iwconfig);
    sh->RegisterCommand("nmcli",    "NetworkManager CLI",      ENV_LINUX,  "network", (ShellCmdHandler)cmd_nmcli);
}

int WiFi::cmd_wifi(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    int p = 0;

    if (argc < 2) {
        p = na(out, p, mx, "WiFi: ");
        p = na(out, p, mx, StateString());
        p = nac(out, p, mx, '\n');
        if (connected_index >= 0) {
            p = na(out, p, mx, "SSID: ");
            p = na(out, p, mx, networks[connected_index].ssid);
            p = na(out, p, mx, "  Signal: ");
            p = nai(out, p, mx, networks[connected_index].signal_strength);
            p = na(out, p, mx, " dBm\n");
        }
        p = na(out, p, mx, "\nUsage: wifi scan|connect|disconnect|on|off\n");
        return p;
    }

    if (neq(argv[1], "on")) {
        Enable();
        return na(out, 0, mx, "WiFi enabled.\n");
    }
    if (neq(argv[1], "off")) {
        Disable();
        return na(out, 0, mx, "WiFi disabled.\n");
    }

    if (neq(argv[1], "scan")) {
        if (!Scan()) return na(out, 0, mx, "WiFi is off. Use: wifi on\n");
        p = na(out, p, mx, "╔═══════════════════════════════════════════════════╗\n");
        p = na(out, p, mx, "║              Available WiFi Networks              ║\n");
        p = na(out, p, mx, "╠═══════════════════════════════════════════════════╣\n");
        for (int i = 0; i < network_count; i++) {
            WiFiNetwork& n = networks[i];
            // signal bars
            int bars = (n.signal_strength > -40) ? 4 : (n.signal_strength > -55) ? 3 :
                       (n.signal_strength > -70) ? 2 : (n.signal_strength > -85) ? 1 : 0;
            p = na(out, p, mx, "║ ");
            p = na(out, p, mx, n.connected ? "● " : "  ");

            // signal bars visual
            for (int b = 0; b < 4; b++) p = na(out, p, mx, b < bars ? "▂" : " ");
            p = na(out, p, mx, " ");
            p = na(out, p, mx, n.ssid);
            int sl = nlen(n.ssid);
            for (int j = sl; j < 22; j++) p = nac(out, p, mx, ' ');

            // security
            const char* sec = "OPEN";
            if (n.security == WIFI_WEP)  sec = "WEP ";
            if (n.security == WIFI_WPA)  sec = "WPA ";
            if (n.security == WIFI_WPA2) sec = "WPA2";
            if (n.security == WIFI_WPA3) sec = "WPA3";
            p = na(out, p, mx, sec);
            p = na(out, p, mx, "  Ch:");
            p = nai(out, p, mx, n.channel);
            if (n.channel < 10) p = nac(out, p, mx, ' ');
            p = na(out, p, mx, "  ");
            p = nai(out, p, mx, n.signal_strength);
            p = na(out, p, mx, "dBm");
            p = na(out, p, mx, " ║\n");
        }
        p = na(out, p, mx, "╚═══════════════════════════════════════════════════╝\n");
        return p;
    }

    if (neq(argv[1], "connect")) {
        if (argc < 3) return na(out, 0, mx, "Usage: wifi connect <SSID> [password]\n");
        const char* pwd = (argc >= 4) ? argv[3] : "";
        if (Connect(argv[2], pwd)) {
            p = na(out, p, mx, "✓ Connected to ");
            p = na(out, p, mx, argv[2]);
            p = nac(out, p, mx, '\n');
        } else {
            p = na(out, p, mx, "✗ Failed to connect to ");
            p = na(out, p, mx, argv[2]);
            p = nac(out, p, mx, '\n');
        }
        return p;
    }

    if (neq(argv[1], "disconnect")) {
        Disconnect();
        return na(out, 0, mx, "WiFi disconnected.\n");
    }

    return na(out, 0, mx, "Unknown wifi command. Use: scan|connect|disconnect|on|off\n");
}

int WiFi::cmd_iwconfig(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = na(out, p, mx, "wlan0     IEEE 802.11  ");
    if (connected_index >= 0) {
        p = na(out, p, mx, "ESSID:\"");
        p = na(out, p, mx, networks[connected_index].ssid);
        p = na(out, p, mx, "\"\n");
        p = na(out, p, mx, "          Mode:Managed  Frequency:2.437 GHz  Access Point: AA:BB:CC:DD:EE:FF\n");
        p = na(out, p, mx, "          Bit Rate=54 Mb/s   Tx-Power=20 dBm\n");
        p = na(out, p, mx, "          Signal level=");
        p = nai(out, p, mx, networks[connected_index].signal_strength);
        p = na(out, p, mx, " dBm\n");
    } else {
        p = na(out, p, mx, "ESSID:off/any\n");
        p = na(out, p, mx, "          Mode:Managed  Access Point: Not-Associated\n");
    }
    return p;
}

int WiFi::cmd_nmcli(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    int p = 0;

    if (argc < 2 || neq(argv[1], "device")) {
        p = na(out, p, mx, "DEVICE  TYPE      STATE        CONNECTION\n");
        p = na(out, p, mx, "eth0    ethernet  connected    Wired\n");
        p = na(out, p, mx, "wlan0   wifi      ");
        p = na(out, p, mx, connected_index >= 0 ? "connected    " : "disconnected ");
        if (connected_index >= 0) p = na(out, p, mx, networks[connected_index].ssid);
        p = nac(out, p, mx, '\n');
        p = na(out, p, mx, "lo      loopback  unmanaged    --\n");
    }
    return p;
}
