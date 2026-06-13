#include "network.h"
#include "../kernel/time.h"
#include "../drivers/timer.h"
#include "../shell/shell.h"
#include "../drivers/serial.h"
#include "../drivers/e1000.h"
#include "../hal/hal.h"
#include "../system/logging.h"
#include "../ui/notification.h"
#include "tcpip.h"

//  network stack implementation

NetworkInterface Network::interfaces[NET_MAX_INTERFACES];
int Network::interface_count = 0;
NetARPEntry Network::arp_table[NET_ARP_TABLE_SIZE];
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

    if (TCPStack::IsUp()) {
        TCPStack::ProcessRxPacket(data, (int)length);
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
    eth.state = NIC_DOWN;
    if (have_real_nic) {
        E1000::GetMAC(eth.mac.bytes);
        E1000::SetPacketHandler(e1000_rx_handler);
        // qemu user-mode networking: guest gets 10.0.2.x by default
        eth.ip = MakeIP(10, 0, 2, 15);
        eth.netmask = MakeIP(255, 255, 255, 0);
        eth.gateway = MakeIP(10, 0, 2, 2);
        eth.dns = MakeIP(10, 0, 2, 3);
        eth.state = E1000::IsLinkUp() ? NIC_UP : NIC_DOWN;
        SerialLogger::Log("Network: eth0 backed by real E1000 NIC\r\n");
        RuntimeLog::LogNetwork(eth.state == NIC_UP ? "link up" : "link down",
                               "eth0 (e1000)");
    } else {
        // keep a default config for diagnostics, but do not advertise carrier
        // unless a real NIC exists. Linux keeps carrier state separate from
        // address configuration; mirror that so higher layers don't think a
        // fake interface is actually usable.
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
    wlan.state = NIC_DOWN;
    nmemset(&wlan.mac, 0, 6);
    wlan.ip = MakeIP(0, 0, 0, 0);
    wlan.netmask = MakeIP(0, 0, 0, 0);
    wlan.gateway = MakeIP(0, 0, 0, 0);
    wlan.dns = MakeIP(0, 0, 0, 0);
    if (WiFi::DetectedLink() == WiFi::LINK_WIFI) {
        for (int i = 0; i < 6; i++) wlan.mac.bytes[i] = eth.mac.bytes[i];
        wlan.mac.bytes[0] |= 0x02;  // locally administered
        wlan.mac.bytes[5] ^= 0x01;  // distinct from eth0
    }
    wlan.rx_packets = wlan.tx_packets = 0;
    wlan.rx_bytes = wlan.tx_bytes = 0;
    wlan.rx_errors = wlan.tx_errors = 0;

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

// ────────────────────────────────────────────────────────────────────
//  /proc helpers  -  small, dependency-free string formatting so the
//  runtime layout can publish live ARP / route tables every second.
// ────────────────────────────────────────────────────────────────────
namespace {
    int np_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }
    int np_append(char* buf, int max, int pos, const char* s) {
        while (*s && pos < max - 1) buf[pos++] = *s++;
        buf[pos] = 0;
        return pos;
    }
    int np_append_u(char* buf, int max, int pos, unsigned int v) {
        char tmp[12]; int ti = 0;
        if (v == 0) tmp[ti++] = '0';
        else while (v) { tmp[ti++] = '0' + (v % 10); v /= 10; }
        while (ti && pos < max - 1) buf[pos++] = tmp[--ti];
        buf[pos] = 0;
        return pos;
    }
    int np_append_hex2(char* buf, int max, int pos, unsigned char v) {
        const char* hex = "0123456789abcdef";
        if (pos < max - 2) {
            buf[pos++] = hex[(v >> 4) & 0xF];
            buf[pos++] = hex[v & 0xF];
            buf[pos] = 0;
        }
        return pos;
    }
    int np_append_ip(char* buf, int max, int pos, IPv4Address ip) {
        for (int i = 0; i < 4; i++) {
            pos = np_append_u(buf, max, pos, ip.bytes[i]);
            if (i < 3) pos = np_append(buf, max, pos, ".");
        }
        return pos;
    }
    int np_append_mac(char* buf, int max, int pos, const MACAddress& mac) {
        for (int i = 0; i < 6; i++) {
            pos = np_append_hex2(buf, max, pos, mac.bytes[i]);
            if (i < 5) pos = np_append(buf, max, pos, ":");
        }
        return pos;
    }
    // little-endian hex32 (Linux /proc/net/route convention)
    int np_append_hex_le(char* buf, int max, int pos, IPv4Address ip) {
        for (int i = 0; i < 4; i++)
            pos = np_append_hex2(buf, max, pos, ip.bytes[i]);
        return pos;
    }
}

int Network::DumpARPTable(char* buf, int max) {
    if (!buf || max <= 0) return 0;
    int pos = 0;
    pos = np_append(buf, max, pos,
        "IP address       HW type     Flags       HW address            Mask     Device\n");
    for (int i = 0; i < NET_ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid) continue;
        int ip_start = pos;
        pos = np_append_ip(buf, max, pos, arp_table[i].ip);
        // pad ip column to 17
        int ip_len = pos - ip_start;
        while (ip_len < 17 && pos < max - 1) { buf[pos++] = ' '; ip_len++; }
        buf[pos] = 0;
        pos = np_append(buf, max, pos, "0x1         0x2         ");
        pos = np_append_mac(buf, max, pos, arp_table[i].mac);
        pos = np_append(buf, max, pos, "     *        eth0\n");
    }
    return pos;
}

int Network::DumpRoutes(char* buf, int max) {
    if (!buf || max <= 0) return 0;
    int pos = 0;
    pos = np_append(buf, max, pos,
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
    for (int i = 0; i < interface_count; i++) {
        const NetworkInterface& iface = interfaces[i];
        if (iface.state != NIC_UP) continue;
        // default route via gateway (if non-zero)
        bool has_gw = (iface.gateway.bytes[0] | iface.gateway.bytes[1] |
                       iface.gateway.bytes[2] | iface.gateway.bytes[3]) != 0;
        if (has_gw) {
            pos = np_append(buf, max, pos, iface.name);
            pos = np_append(buf, max, pos, "\t00000000\t");
            pos = np_append_hex_le(buf, max, pos, iface.gateway);
            pos = np_append(buf, max, pos, "\t0003\t0\t0\t100\t00000000\t0\t0\t0\n");
        }
        // on-link subnet route
        IPv4Address subnet;
        for (int j = 0; j < 4; j++)
            subnet.bytes[j] = iface.ip.bytes[j] & iface.netmask.bytes[j];
        pos = np_append(buf, max, pos, iface.name);
        pos = np_append(buf, max, pos, "\t");
        pos = np_append_hex_le(buf, max, pos, subnet);
        pos = np_append(buf, max, pos, "\t00000000\t0001\t0\t0\t0\t");
        pos = np_append_hex_le(buf, max, pos, iface.netmask);
        pos = np_append(buf, max, pos, "\t0\t0\t0\n");
    }
    return pos;
}

bool Network::SendPacket(const char* ifname, const unsigned char* data, int len) {
    NetworkInterface* iface = GetInterface(ifname);
    if (!iface || iface->state != NIC_UP) return false;

    bool sent = false;
    if (iface->type == NIC_ETHERNET || iface->type == NIC_WIFI) {
        // use real e1000 nic if available
        if (E1000::IsDetected() && E1000::IsLinkUp()) {
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
    if (E1000::IsDetected() && E1000::IsLinkUp()) {
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
            if (next_port < 49152 || next_port >= 65535) next_port = 49152;
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

struct DNSEntry {
    const char* hostname;
    uint8_t a, b, c, d;
};

struct DNSHeader {
    uint16_t id;
    uint16_t flags;
    uint16_t question_count;
    uint16_t answer_count;
    uint16_t authority_count;
    uint16_t additional_count;
} __attribute__((packed));

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
    {"satorut.com",        104, 21, 44, 39},
    {"kurono.satorut.com", 104, 21, 44, 39},
    {"server.satorut.com", 104, 21, 44, 39},
    {"kurono.local",       192, 168, 1, 100},
    {nullptr, 0, 0, 0, 0}
};

static void ndns_write_u16(uint8_t* out, uint16_t value) {
    out[0] = (uint8_t)((value >> 8) & 0xFF);
    out[1] = (uint8_t)(value & 0xFF);
}

static uint16_t ndns_read_u16(const uint8_t* in) {
    return (uint16_t)(((uint16_t)in[0] << 8) | (uint16_t)in[1]);
}

static uint32_t ndns_ip_to_u32(IPv4Address ip) {
    return TCPStack::MakeIP(ip.bytes[0], ip.bytes[1], ip.bytes[2], ip.bytes[3]);
}

static bool ndns_append_name(const char* hostname, uint8_t* out, int max_len, int& pos) {
    if (!hostname || !*hostname) return false;

    const char* label = hostname;
    int label_len = 0;
    for (int i = 0;; i++) {
        char ch = hostname[i];
        if (ch == '.' || ch == 0) {
            if (label_len <= 0 || label_len > 63) return false;
            if (pos + 1 + label_len >= max_len) return false;
            out[pos++] = (uint8_t)label_len;
            for (int j = 0; j < label_len; j++) out[pos++] = (uint8_t)label[j];
            if (ch == 0) break;
            label = hostname + i + 1;
            label_len = 0;
            continue;
        }
        label_len++;
    }

    if (pos >= max_len) return false;
    out[pos++] = 0;
    return true;
}

static int ndns_skip_name(const uint8_t* data, int len, int pos) {
    while (pos < len) {
        uint8_t label_len = data[pos];
        if (label_len == 0) return pos + 1;
        if ((label_len & 0xC0) == 0xC0) {
            return (pos + 1 < len) ? pos + 2 : -1;
        }
        if (label_len & 0xC0) return -1;
        pos++;
        if (pos + label_len > len) return -1;
        pos += label_len;
    }
    return -1;
}

// Decompress a (possibly pointer-compressed) DNS name at `pos` into a dotted
// hostname string. Follows compression pointers with a bounded jump budget so
// a malicious self-referential packet can't loop forever. Returns true on
// success; the parsed name lands in `out` (lowercased not required). (satoru)
static bool ndns_read_name(const uint8_t* data, int len, int pos, char* out, int out_max) {
    if (!out || out_max < 1) return false;
    int op = 0;
    int jumps = 0;
    out[0] = 0;
    while (pos >= 0 && pos < len) {
        uint8_t label_len = data[pos];
        if (label_len == 0) {
            out[op] = 0;
            return true;
        }
        if ((label_len & 0xC0) == 0xC0) {
            if (pos + 1 >= len) return false;
            if (++jumps > 16) return false;            // pointer loop guard (satoru)
            int target = ((label_len & 0x3F) << 8) | data[pos + 1];
            pos = target;
            continue;
        }
        if (label_len & 0xC0) return false;
        pos++;
        if (pos + label_len > len) return false;
        if (op > 0 && op < out_max - 1) out[op++] = '.';
        for (int i = 0; i < label_len; i++) {
            if (op < out_max - 1) out[op++] = (char)data[pos + i];
        }
        pos += label_len;
    }
    return false;
}

static void ndns_log(const char* action, const char* hostname, const IPv4Address* ip) {
    char line[192];
    int p = 0;
    line[0] = 0;
    p = na(line, p, sizeof(line), action);
    if (hostname && *hostname) {
        p = na(line, p, sizeof(line), ": ");
        p = na(line, p, sizeof(line), hostname);
    }
    if (ip) {
        char ip_text[16];
        Network::IPToString(*ip, ip_text, sizeof(ip_text));
        p = na(line, p, sizeof(line), " -> ");
        p = na(line, p, sizeof(line), ip_text);
    }
    RuntimeLog::LogSystem("network", line);
}

// Single DNS A-query for `hostname`. Tri-state result:
//   1  -> an A record was found; *out holds the address
//   0  -> no A record, but a CNAME was present; its (decompressed) target is
//         written to cname_out so the caller can follow the chain
//  -1  -> hard failure (timeout / malformed / server rcode error)
// The A-record scan still takes the FIRST A in the answer section, so a
// response that already carries the CNAME->A pair resolves in one shot; the
// CNAME hand-back only happens when the answer contains NO A record. (satoru)
static int ndns_query_once(const char* hostname, uint32_t dns_ip,
                           IPv4Address* out, char* cname_out, int cname_max) {
    if (cname_out && cname_max > 0) cname_out[0] = 0;

    int sock = TCPStack::Socket(SOCK_DGRAM);
    if (sock < 0) return -1;

    uint8_t query[512];
    uint8_t response[512];
    nmemset(query, 0, sizeof(query));
    nmemset(response, 0, sizeof(response));

    uint16_t request_id = (uint16_t)(Time::GetTicks() & 0xFFFFu);
    if (request_id == 0) request_id = 1;

    DNSHeader* header = (DNSHeader*)query;
    ndns_write_u16((uint8_t*)&header->id, request_id);
    ndns_write_u16((uint8_t*)&header->flags, 0x0100);
    ndns_write_u16((uint8_t*)&header->question_count, 1);
    ndns_write_u16((uint8_t*)&header->answer_count, 0);
    ndns_write_u16((uint8_t*)&header->authority_count, 0);
    ndns_write_u16((uint8_t*)&header->additional_count, 0);

    int query_len = (int)sizeof(DNSHeader);
    if (!ndns_append_name(hostname, query, (int)sizeof(query), query_len)) {
        TCPStack::Close(sock);
        return -1;
    }
    if (query_len + 4 > (int)sizeof(query)) {
        TCPStack::Close(sock);
        return -1;
    }

    ndns_write_u16(query + query_len, 1);
    query_len += 2;
    ndns_write_u16(query + query_len, 1);
    query_len += 2;

    for (int attempt = 0; attempt < 3; attempt++) {
        if (TCPStack::SendTo(sock, query, query_len, dns_ip, 53) != query_len) {
            continue;
        }

        /* Real-ms deadline (~1s per attempt). The previous spin counter
           depended on CPU speed and could spin forever if PumpUI rendering
           was busy enough. */
        uint32_t attempt_start_ms = Timer::GetTicks();
        const uint32_t attempt_timeout_ms = 1000u;
        while ((uint32_t)(Timer::GetTicks() - attempt_start_ms) < attempt_timeout_ms) {
            if (KuronoShell::IsCommandCancelRequested()) {
                TCPStack::Close(sock);
                return -1;
            }
            TCPStack::Tick();
            KuronoShell::PumpUI();

            uint32_t from_ip = 0;
            uint16_t from_port = 0;
            int got = TCPStack::RecvFrom(sock, response, sizeof(response), &from_ip, &from_port);
            if (got <= 0) {
                /* ~1ms PIT-poll between recv attempts so we don't starve
                   the kernel main loop / E1000 path. */
                uint32_t iter_start = Timer::GetTicks();
                while ((uint32_t)(Timer::GetTicks() - iter_start) < 1u) {
                    __asm__ __volatile__("pause");
                }
                continue;
            }
            if (from_port != 53 || got < (int)sizeof(DNSHeader)) continue;
            if (ndns_read_u16(response) != request_id) continue;

            uint16_t flags = ndns_read_u16(response + 2);
            if ((flags & 0x8000u) == 0) continue;
            if ((flags & 0x000Fu) != 0) {
                TCPStack::Close(sock);
                return -1;
            }

            int questions = ndns_read_u16(response + 4);
            int answers = ndns_read_u16(response + 6);
            int pos = (int)sizeof(DNSHeader);

            for (int q = 0; q < questions; q++) {
                pos = ndns_skip_name(response, got, pos);
                if (pos < 0 || pos + 4 > got) {
                    TCPStack::Close(sock);
                    return -1;
                }
                pos += 4;
            }

            // remember the first CNAME target as a fallback if no A appears (satoru)
            bool have_cname = false;
            char cname_tmp[256];
            cname_tmp[0] = 0;

            for (int a = 0; a < answers; a++) {
                pos = ndns_skip_name(response, got, pos);
                if (pos < 0 || pos + 10 > got) {
                    TCPStack::Close(sock);
                    return -1;
                }

                uint16_t type = ndns_read_u16(response + pos); pos += 2;
                uint16_t klass = ndns_read_u16(response + pos); pos += 2;
                pos += 4; // ttl
                uint16_t rdlen = ndns_read_u16(response + pos); pos += 2;
                if (pos + rdlen > got) {
                    TCPStack::Close(sock);
                    return -1;
                }

                if (type == 1 && klass == 1 && rdlen == 4) {
                    *out = Network::MakeIP(response[pos], response[pos + 1], response[pos + 2], response[pos + 3]);
                    TCPStack::Close(sock);
                    return 1;
                }
                // CNAME (type 5): decompress its rdata target name and hold it
                // in case the answer section carries no A record (satoru)
                if (type == 5 && klass == 1 && !have_cname) {
                    if (ndns_read_name(response, got, pos, cname_tmp, (int)sizeof(cname_tmp)) &&
                        cname_tmp[0]) {
                        have_cname = true;
                    }
                }
                pos += rdlen;
            }

            // got a valid response but no A record. If a CNAME was present,
            // hand its target back so the caller can chase it. (satoru)
            if (have_cname && cname_out && cname_max > 0) {
                ncpy(cname_out, cname_tmp, cname_max);
                TCPStack::Close(sock);
                return 0;
            }
            TCPStack::Close(sock);
            return -1;
        }
    }

    TCPStack::Close(sock);
    return -1;
}

static bool ndns_resolve_live(const char* hostname, IPv4Address* out) {
    if (!TCPStack::IsUp() || !hostname || !*hostname || !out) return false;

    NetworkInterface* iface = Network::GetInterface("eth0");
    if (!iface || iface->state != NIC_UP) {
        NetworkInterface* wlan = Network::GetInterface("wlan0");
        if (wlan && wlan->state == NIC_UP) iface = wlan;
    }

    uint32_t dns_ip = iface ? ndns_ip_to_u32(iface->dns) : 0;
    if (dns_ip == 0) dns_ip = TCPStack::MakeIP(10, 0, 2, 3);

    // Follow a CNAME chain, issuing a fresh A query for each canonical name.
    // Bounded to 5 hops so a misconfigured/looping zone can't spin us. (satoru)
    char name[256];
    ncpy(name, hostname, (int)sizeof(name));
    const int MAX_CNAME_HOPS = 5;
    for (int hop = 0; hop < MAX_CNAME_HOPS; hop++) {
        char cname[256];
        int r = ndns_query_once(name, dns_ip, out, cname, (int)sizeof(cname));
        if (r == 1) return true;            // resolved to an A record
        if (r < 0) return false;            // timeout / error / NXDOMAIN
        if (!cname[0]) return false;        // CNAME with no usable target
        if (neq(name, cname)) return false; // self-referential CNAME loop (satoru)
        ndns_log("dns cname", name, nullptr);
        ncpy(name, cname, (int)sizeof(name)); // follow the canonical name
    }
    return false;                            // exceeded hop budget (satoru)
}

bool Network::Resolve(const char* hostname, IPv4Address* out) {
    if (!hostname || !out) return false;

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

    if (ndns_resolve_live(hostname, out)) {
        ndns_log("dns resolved", hostname, out);
        return true;
    }

    for (int i = 0; dns_cache[i].hostname; i++) {
        if (neq(hostname, dns_cache[i].hostname)) {
            *out = MakeIP(dns_cache[i].a, dns_cache[i].b, dns_cache[i].c, dns_cache[i].d);
            ndns_log("dns fallback", hostname, out);
            return true;
        }
    }

    ndns_log("dns failed", hostname, nullptr);
    return false;
}

//  wifi driver implementation

WiFiState WiFi::state = WIFI_OFF;
WiFiNetwork WiFi::networks[NET_MAX_WIFI_NETS];
int WiFi::network_count = 0;
int WiFi::connected_index = -1;
WiFi::LinkKind WiFi::detected_link = WiFi::LINK_NONE;
char WiFi::wireless_chip[64] = {0};
bool WiFi::link_probed = false;

// Real PCI bus walk to identify wireless network controllers.
// Subclass 0x80 of class 0x02 = "Other network controller", which is
// what every IEEE 802.11 NIC reports.  Some Intel cards also use 0x02:0x80
// with prog_if 0x00.  We additionally check known wireless vendor/device
// IDs to be safe.
void WiFi::ProbePCIWireless() {
    if (link_probed) return;
    link_probed = true;
    detected_link = LINK_NONE;
    wireless_chip[0] = 0;
    bool wired_found = false;
    for (uint32_t bus = 0; bus < 256; bus++) {
        for (uint32_t slot = 0; slot < 32; slot++) {
            uint32_t addr = (1u << 31) | (bus << 16) | (slot << 11) | 0x00;
            __asm__ __volatile__("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
            uint32_t id;
            __asm__ __volatile__("inl %1, %0" : "=a"(id) : "Nd"((uint16_t)0xCFC));
            uint16_t vendor = id & 0xFFFF;
            uint16_t device = (id >> 16) & 0xFFFF;
            if (vendor == 0xFFFF || vendor == 0) continue;
            addr = (1u << 31) | (bus << 16) | (slot << 11) | 0x08;
            __asm__ __volatile__("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
            uint32_t cls;
            __asm__ __volatile__("inl %1, %0" : "=a"(cls) : "Nd"((uint16_t)0xCFC));
            uint8_t class_code = (cls >> 24) & 0xFF;
            uint8_t subclass   = (cls >> 16) & 0xFF;
            if (class_code != 0x02) continue;
            // 0x00 = ethernet, 0x80 = other (wireless), 0x01 = token ring
            bool is_wireless = (subclass == 0x80);
            // Also flag known WiFi vendor/device combos even if subclass==0
            // Intel iwlwifi: 0x8086 + (0x0084..0x24fd range)
            // Atheros: 0x168c
            // Broadcom: 0x14e4 wifi devices
            // Realtek RTL8821/8723: 0x10ec 0x8821 etc.
            if (!is_wireless) {
                if (vendor == 0x168c) is_wireless = true; // Atheros
                else if (vendor == 0x14e4 && (device == 0x4359 || device == 0x4727 || device == 0x43a0)) is_wireless = true;
                else if (vendor == 0x10ec && (device == 0x8821 || device == 0x8723 || device == 0xb822)) is_wireless = true;
                else if (vendor == 0x8086 && (device == 0x24fd || device == 0x095a || device == 0x08b1 || device == 0x2723)) is_wireless = true;
            }
            if (is_wireless) {
                detected_link = LINK_WIFI;
                const char* vname = "Unknown";
                if (vendor == 0x8086) vname = "Intel iwlwifi";
                else if (vendor == 0x168c) vname = "Atheros ath";
                else if (vendor == 0x14e4) vname = "Broadcom brcmfmac";
                else if (vendor == 0x10ec) vname = "Realtek rtw";
                int wp = 0;
                while (vname[wp] && wp < 60) { wireless_chip[wp] = vname[wp]; wp++; }
                wireless_chip[wp] = 0;
                return;
            }
            if (subclass == 0x00) wired_found = true;
        }
    }
    if (wired_found || E1000::IsDetected()) detected_link = LINK_ETHERNET;
}

WiFi::LinkKind WiFi::DetectedLink() {
    if (!link_probed) ProbePCIWireless();
    return detected_link;
}

const char* WiFi::LinkKindString() {
    switch (DetectedLink()) {
        case LINK_WIFI:     return "wifi";
        case LINK_ETHERNET: return "ethernet";
        default:            return "offline";
    }
}

const char* WiFi::WirelessChipName() {
    if (!link_probed) ProbePCIWireless();
    return wireless_chip;
}

bool WiFi::IsLinkUp() {
    if (!link_probed) ProbePCIWireless();
    if (detected_link == LINK_WIFI)     return state == WIFI_CONNECTED;
    return false;
}

int WiFi::SignalBars() {
    if (!IsLinkUp()) return 0;
    if (detected_link == LINK_ETHERNET) return 4; // wired = full signal
    int dbm = GetSignalStrength();
    if (dbm >= -50) return 4;
    if (dbm >= -65) return 3;
    if (dbm >= -75) return 2;
    if (dbm >= -85) return 1;
    return 0;
}

void WiFi::SimulateNetworks() {
    network_count = 0;
    connected_index = -1;
}

void WiFi::Init() {
    state = WIFI_DISCONNECTED;
    network_count = 0;
    connected_index = -1;
    ProbePCIWireless();

    if (detected_link == LINK_WIFI) {
        SerialLogger::Log("WiFi: Controller detected, native 802.11 stack not implemented yet\r\n");
    } else if (detected_link == LINK_ETHERNET) {
        SerialLogger::Log("WiFi: No native WiFi radio in current environment (wired link only)\r\n");
    } else {
        SerialLogger::Log("WiFi: No wireless controller detected\r\n");
    }
}

bool WiFi::Enable() {
    if (DetectedLink() != LINK_WIFI) return false;
    if (state == WIFI_OFF) state = WIFI_DISCONNECTED;
    return true;
}

bool WiFi::Disable() {
    if (connected_index >= 0) Disconnect();
    state = WIFI_OFF;
    return true;
}

bool WiFi::Scan() {
    if (state == WIFI_OFF || DetectedLink() != LINK_WIFI) return false;
    state = WIFI_SCANNING;
    SimulateNetworks();
    state = connected_index >= 0 ? WIFI_CONNECTED : WIFI_DISCONNECTED;
    return true;
}

bool WiFi::Connect(const char* ssid, const char* password) {
    (void)password;
    if (state == WIFI_OFF || DetectedLink() != LINK_WIFI) return false;

    // find network
    for (int i = 0; i < network_count; i++) {
        if (neq(networks[i].ssid, ssid)) {
            if (connected_index >= 0) networks[connected_index].connected = false;
            networks[i].connected = true;
            connected_index = i;
            state = WIFI_CONNECTED;
            // wifi link came up  -  toast the connected ssid. (satoru)
            NotificationManager::Post("Network", networks[i].ssid,
                                      NotificationManager::ICON_SUCCESS, 3000);
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
    // wifi link went down. (satoru)
    NotificationManager::Post("Network", "wifi disconnected",
                              NotificationManager::ICON_WARNING, 3000);
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
        if (!Enable()) return na(out, 0, mx, "No native WiFi radio is available in this environment.\n");
        return na(out, 0, mx, "WiFi enabled.\n");
    }
    if (neq(argv[1], "off")) {
        Disable();
        return na(out, 0, mx, "WiFi disabled.\n");
    }

    if (neq(argv[1], "scan")) {
        if (!Scan()) {
            if (DetectedLink() != LINK_WIFI) {
                return na(out, 0, mx, "No native WiFi radio is available in this environment.\n");
            }
            return na(out, 0, mx, "WiFi is off. Use: wifi on\n");
        }
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
        if (DetectedLink() != LINK_WIFI) {
            return na(out, 0, mx, "No native WiFi radio is available in this environment.\n");
        }
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
