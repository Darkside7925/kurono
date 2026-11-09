//  kurono os - settings module: network (satoru)
//  wi-fi (on/off, scan, join open networks, join secured networks with an
//  inline password field) when a wireless adapter exists, the ethernet link
//  (status, addresses, traffic), and the bluetooth adapter state. every control
//  drives the real WiFi / Network api. (satoru)
#include "system_settings.h"
#include "../net/network.h"
#include "../net/dhcp.h"
#include "../drivers/e1000.h"
#include "../linux/linux_drivers.h"
#include "../system/ui_config.h"

using namespace SettingsUI;

static char s_pass[64];
static int  s_expand = -1;       // secured network row showing the password field (satoru)
static char s_last_error[64];

// ── helpers ─────────────────────────────────────────────────────────────────
static bool starts_with(const char* s, const char* p){
    if(!s || !p) return false;
    int i = 0; while(p[i]){ if(s[i] != p[i]) return false; i++; }
    return true;
}
static const LinuxDriver* find_driver(const char* const* prefixes, int count){
    LinuxDriver* d = LinuxDriverFramework::GetDrivers();
    int n = LinuxDriverFramework::GetDriverCount();
    for(int i = 0; i < n; i++){
        if(!(d[i].bound || d[i].state == LDRV_ACTIVE)) continue;
        for(int p = 0; p < count; p++) if(starts_with(d[i].name, prefixes[p])) return &d[i];
    }
    return nullptr;
}
static const LinuxDriver* bt_driver(){
    static const char* pfx[] = { "bluetooth_", "bluetooth", "bt", "hci" };
    return find_driver(pfx, 4);
}
static void mac_str(const MACAddress& m, char* b, int mx){
    static const char hex[] = "0123456789ABCDEF";
    int p = 0;
    for(int i = 0; i < 6 && p < mx - 3; i++){
        b[p++] = hex[(m.bytes[i] >> 4) & 0xF];
        b[p++] = hex[m.bytes[i] & 0xF];
        if(i < 5) b[p++] = ':';
    }
    b[p] = 0;
}
static void bytes_str(uint64_t v, char* b, int mx){
    const char* unit = " B";
    uint64_t whole = v, frac = 0;
    if(v >= 1024ull * 1024 * 1024){ whole = v / (1024ull * 1024 * 1024); frac = (v % (1024ull * 1024 * 1024)) * 10 / (1024ull * 1024 * 1024); unit = " GB"; }
    else if(v >= 1024ull * 1024){ whole = v / (1024ull * 1024); frac = (v % (1024ull * 1024)) * 10 / (1024ull * 1024); unit = " MB"; }
    else if(v >= 1024){ whole = v / 1024; frac = (v % 1024) * 10 / 1024; unit = " KB"; }
    IntToStr((int)whole, b, mx);
    if(unit[1] != 'B'){ char f[4]; StrApp(b, ".", mx); IntToStr((int)frac, f, 4); StrApp(b, f, mx); }
    StrApp(b, unit, mx);
}
static const char* sec_str(WiFiSecurity s){
    switch(s){
        case WIFI_OPEN: return "Open";
        case WIFI_WEP:  return "WEP";
        case WIFI_WPA:  return "WPA";
        case WIFI_WPA2: return "WPA2";
        case WIFI_WPA3: return "WPA3";
        default:        return "Secured";
    }
}
static const char* signal_word(int dbm){
    if(dbm >= -55) return "Excellent";
    if(dbm >= -65) return "Good";
    if(dbm >= -75) return "Fair";
    return "Weak";
}

static uint32_t ip_u32(const IPv4Address& a){
    return ((uint32_t)a.bytes[0] << 24) | ((uint32_t)a.bytes[1] << 16) |
           ((uint32_t)a.bytes[2] << 8) | (uint32_t)a.bytes[3];
}
static void ip_u32_str(uint32_t v, char* b, int mx){
    IPv4Address a = Network::MakeIP((uint8_t)(v >> 24), (uint8_t)(v >> 16),
                                    (uint8_t)(v >> 8), (uint8_t)v);
    Network::IPToString(a, b, mx);
}

// one glance at the live link: carrier, signal, address, resolver and traffic.
// reads the driver counters (not the interface bookkeeping, which never saw the
// tcp tx path) and the dhcp lease state. (satoru)
static void status_section(){
    Group("Status");
    NetworkInterface* eth = Network::GetInterface("eth0");
    NetworkInterface* wlan = Network::GetInterface("wlan0");
    bool wifi_up = (WiFi::DetectedLink() == WiFi::LINK_WIFI &&
                    WiFi::GetState() == WIFI_CONNECTED);
    bool eth_up = (eth && eth->state == NIC_UP);
    const char* link = "Offline";
    const char* sub = "no active link";
    if(wifi_up){ link = "Wi-Fi"; sub = "associated"; }
    else if(eth_up){ link = "Ethernet"; sub = "carrier up"; }
    Info("Link", link, sub, (wifi_up || eth_up) ? T().success : T().text3);

    NetworkInterface* active = wifi_up ? wlan : eth;
    if(wifi_up){
        WiFiNetwork* w = WiFi::GetConnectedNetwork();
        Info("Network", (w && w->ssid[0]) ? w->ssid : "joined");
        int dbm = WiFi::GetSignalStrength();
        char sig[64];
        StrCpy(sig, signal_word(dbm), (int)sizeof(sig));
        StrApp(sig, " (", (int)sizeof(sig));
        char num[16]; IntToStr(dbm, num, (int)sizeof(num)); StrApp(sig, num, (int)sizeof(sig));
        StrApp(sig, " dBm)", (int)sizeof(sig));
        Info("Signal", sig,
             WiFi::AutoReconnect() ? "auto-reconnect on" : "auto-reconnect off",
             WiFi::SignalBars() > 0 ? T().success : T().warn);
    } else {
        Info("Signal", eth_up ? "Wired" : "None",
             eth_up ? "full, no radio needed" : "no carrier",
             eth_up ? T().success : T().text3);
    }

    char ip[32] = "0.0.0.0";
    if(active) Network::IPToString(active->ip, ip, (int)sizeof(ip));
    Info("IP address", ip);

    uint32_t dp = 0, ds = 0;
    Network::GetDNSServers(&dp, &ds);
    if(!dp && active) dp = ip_u32(active->dns);
    char dns[80];
    if(dp){
        ip_u32_str(dp, dns, (int)sizeof(dns));
        if(ds){
            StrApp(dns, ", ", (int)sizeof(dns));
            char t[24]; ip_u32_str(ds, t, (int)sizeof(t));
            StrApp(dns, t, (int)sizeof(dns));
        }
    } else {
        StrCpy(dns, "none", (int)sizeof(dns));
    }
    Info("DNS server", dns);

    uint64_t rxb = E1000::IsDetected() ? E1000::GetRxBytes() : (active ? active->rx_bytes : 0);
    uint64_t txb = E1000::IsDetected() ? E1000::GetTxBytes() : (active ? active->tx_bytes : 0);
    uint32_t rxp = E1000::IsDetected() ? E1000::GetRxCount() : (active ? active->rx_packets : 0);
    uint32_t txp = E1000::IsDetected() ? E1000::GetTxCount() : (active ? active->tx_packets : 0);
    char val[80], pk[24];
    bytes_str(rxb, val, (int)sizeof(val));
    StrApp(val, " (", (int)sizeof(val)); IntToStr((int)rxp, pk, (int)sizeof(pk));
    StrApp(val, pk, (int)sizeof(val)); StrApp(val, " packets)", (int)sizeof(val));
    Info("Received", val);
    bytes_str(txb, val, (int)sizeof(val));
    StrApp(val, " (", (int)sizeof(val)); IntToStr((int)txp, pk, (int)sizeof(pk));
    StrApp(val, pk, (int)sizeof(val)); StrApp(val, " packets)", (int)sizeof(val));
    Info("Sent", val);

    if(Dhcp::IsBound()){
        char lease[64];
        StrCpy(lease, Dhcp::StateString(), (int)sizeof(lease));
        StrApp(lease, " ", (int)sizeof(lease));
        char sec[16]; IntToStr((int)Dhcp::LeaseRemainingSeconds(), sec, (int)sizeof(sec));
        StrApp(lease, sec, (int)sizeof(lease)); StrApp(lease, "s left", (int)sizeof(lease));
        char srv[24]; ip_u32_str(Dhcp::GetServer(), srv, (int)sizeof(srv));
        Info("DHCP lease", lease, srv);
    } else {
        Info("DHCP", Dhcp::StateString(), "no lease");
    }
    EndGroup();
}

static void network_on_show(){
    s_expand = -1;
    s_pass[0] = 0;
    s_last_error[0] = 0;
}

static void wifi_section(){
    Group("Wi-Fi");
    if(WiFi::DetectedLink() != WiFi::LINK_WIFI){
        Info("Wi-Fi", "Not available", "No wireless adapter was found");
        EndGroup();
        return;
    }
    bool on = WiFi::GetState() != WIFI_OFF;
    const char* chip = WiFi::WirelessChipName();
    if(Toggle("Wi-Fi", &on, (chip && chip[0]) ? chip : WiFi::StateString())){
        UIConfig::SetInt("net.wifi_enabled", on ? 1 : 0, true);
        UIConfig::Save();
        if(on){ WiFi::Enable(); WiFi::Scan(); } else { WiFi::Disable(); s_expand = -1; }
    }
    if(!on){ EndGroup(); return; }

    int n = WiFi::GetNetworkCount();
    WiFiNetwork* nets = WiFi::GetNetworks();
    if(n > 12) n = 12;
    if(n <= 0 || !nets){
        Info("Networks", WiFi::GetState() == WIFI_SCANNING ? "Searching..." : "None found");
    }
    for(int i = 0; i < n && nets; i++){
        const WiFiNetwork& w = nets[i];
        PushId(i);
        char val[48];
        if(w.connected) StrCpy(val, "Connected", 48);
        else { StrCpy(val, sec_str(w.security), 48); StrApp(val, ", ", 48); StrApp(val, signal_word(w.signal_strength), 48); }
        if(Nav(w.ssid[0] ? w.ssid : "Hidden network", val)){
            if(w.connected){ WiFi::Disconnect(); s_expand = -1; }
            else if(w.security == WIFI_OPEN){
                s_last_error[0] = 0;
                if(!WiFi::Connect(w.ssid, "")) StrCpy(s_last_error, "Could not join this network", 64);
            } else {
                s_expand = (s_expand == i) ? -1 : i;
                s_pass[0] = 0;
            }
        }
        if(s_expand == i && !w.connected){
            bool go = false;
            if(TextField("Password", s_pass, 64, "Network password", FIELD_PASSWORD) && Submitted()) go = true;
            if(Button("", "Join", BTN_PRIMARY)) go = true;
            if(go){
                s_last_error[0] = 0;
                if(WiFi::Connect(w.ssid, s_pass)){ s_expand = -1; s_pass[0] = 0; }
                else StrCpy(s_last_error, "Could not join: check the password", 64);
            }
        }
        PopId();
    }
    if(s_last_error[0]) Info("Status", s_last_error, nullptr, T().danger);
    if(Button("Find networks", "Scan", BTN_SECONDARY)) WiFi::Scan();
    EndGroup();
}

static void network_build(){
    status_section();
    wifi_section();

    NetworkInterface* eth = Network::GetInterface("eth0");
    char buf[64];
    Group("Ethernet");
    if(!eth){
        Info("Status", "No adapter", "No wired network adapter was found");
    } else {
        bool up = eth->state == NIC_UP;
        Info("Status", up ? "Connected" : "Disconnected", nullptr, up ? T().success : T().text3);
        Network::IPToString(eth->ip, buf, 64);      Info("IP address", buf);
        Network::IPToString(eth->gateway, buf, 64); Info("Router", buf);
        Network::IPToString(eth->dns, buf, 64);     Info("DNS server", buf);
        mac_str(eth->mac, buf, 64);                  Info("Hardware address", buf);
    }
    EndGroup();

    if(eth){
        Group("Activity");
        char pk[24];
        bytes_str(eth->rx_bytes, buf, 64); StrApp(buf, "  (", 64); IntToStr((int)eth->rx_packets, pk, 24); StrApp(buf, pk, 64); StrApp(buf, " packets)", 64);
        Info("Received", buf);
        bytes_str(eth->tx_bytes, buf, 64); StrApp(buf, "  (", 64); IntToStr((int)eth->tx_packets, pk, 24); StrApp(buf, pk, 64); StrApp(buf, " packets)", 64);
        Info("Sent", buf);
        EndGroup();
    }

    Group("Bluetooth");
    const LinuxDriver* bt = bt_driver();
    if(bt) Info("Adapter", bt->description[0] ? bt->description : bt->name, "Pairing is not supported yet");
    else   Info("Bluetooth", "Not available", "No Bluetooth adapter was found");
    EndGroup();
}

extern const SettingsModule g_network_module = {
    "network", "Network", "", network_on_show, nullptr, nullptr, nullptr, network_build
};
// end (satoru)
