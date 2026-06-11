//  kurono os  -  settings module: network (satoru)
//  detailed network & bluetooth page: live ethernet interface stats, a wi-fi
//  enable toggle + scan affordance + scanned ssid list, bluetooth controller
//  binding state + enable toggle, and a small lo/eth0 interface table. radio
//  toggles persist to net.wifi_enabled / net.bt_enabled via uiconfig. (satoru)
#include "system_settings.h"
#include "../drivers/graphics.h"
#include "../net/network.h"
#include "../linux/linux_drivers.h"
#include "../system/ui_config.h"

// ── module state (constant-initialised statics  -  ctor-free) ─────────────────
static bool s_wifi_enabled = false;   // persisted to net.wifi_enabled (satoru)
static bool s_bt_enabled   = false;   // persisted to net.bt_enabled (satoru)

// ── tiny libc-free local string helpers (mirror the shell's SettingsUI) ─────
static void net_strcpy(char* d, const char* s, int mx){ SettingsUI::StrCpy(d, s, mx); }
static void net_strapp(char* d, const char* s, int mx){ SettingsUI::StrApp(d, s, mx); }
static void net_intstr(int v, char* b, int mx){ SettingsUI::IntToStr(v, b, mx); }

// ── driver-binding probes (same prefix sets the legacy settings.cpp used) ───
static bool net_starts_with(const char* s, const char* prefix){
    if(!s || !prefix) return false;
    int i = 0; while(prefix[i]){ if(s[i] != prefix[i]) return false; i++; }
    return true;
}
static const LinuxDriver* net_find_driver(const char* const* prefixes, int count){
    LinuxDriver* drivers = LinuxDriverFramework::GetDrivers();
    int driver_count = LinuxDriverFramework::GetDriverCount();
    for(int i = 0; i < driver_count; i++){
        if(!(drivers[i].bound || drivers[i].state == LDRV_ACTIVE)) continue;
        for(int p = 0; p < count; p++){
            if(net_starts_with(drivers[i].name, prefixes[p])) return &drivers[i];
        }
    }
    return nullptr;
}
static const LinuxDriver* net_find_wifi_driver(){
    static const char* prefixes[] = {
        "wifi_", "iwl", "ath", "rtw", "rtl", "brcm", "mt76", "cfg80211", "mac80211"
    };
    return net_find_driver(prefixes, (int)(sizeof(prefixes) / sizeof(prefixes[0])));
}
static const LinuxDriver* net_find_bt_driver(){
    static const char* prefixes[] = {
        "bluetooth_", "bluetooth", "bt", "hci"
    };
    return net_find_driver(prefixes, (int)(sizeof(prefixes) / sizeof(prefixes[0])));
}

// ── formatting helpers ──────────────────────────────────────────────────────
// "a.b.c.d" into buf via the network stack's own formatter. (satoru)
static void net_ip_str(IPv4Address ip, char* buf, int mx){
    Network::IPToString(ip, buf, mx);
}
// canonical xx:xx:xx:xx:xx:xx mac. (satoru)
static void net_mac_str(const MACAddress& m, char* buf, int mx){
    static const char hex[] = "0123456789ABCDEF";
    int p = 0;
    for(int i = 0; i < 6 && p < mx - 1; i++){
        if(p < mx - 1) buf[p++] = hex[(m.bytes[i] >> 4) & 0xF];
        if(p < mx - 1) buf[p++] = hex[m.bytes[i] & 0xF];
        if(i < 5 && p < mx - 1) buf[p++] = ':';
    }
    buf[p] = 0;
}
// short label for a wi-fi security mode. (satoru)
static const char* net_sec_str(WiFiSecurity s){
    switch(s){
        case WIFI_OPEN: return "Open";
        case WIFI_WEP:  return "WEP";
        case WIFI_WPA:  return "WPA";
        case WIFI_WPA2: return "WPA2";
        case WIFI_WPA3: return "WPA3";
        default:        return "?";
    }
}
// rough 0..4 bar count from a dbm reading (-100..0). (satoru)
static int net_bars_from_dbm(int dbm){
    if(dbm >= -55) return 4;
    if(dbm >= -65) return 3;
    if(dbm >= -75) return 2;
    if(dbm >= -85) return 1;
    return 0;
}

// ── on_show: load persisted radio toggles + detect live state ───────────────
static void network_on_show(){
    s_wifi_enabled = UIConfig::Int("net.wifi_enabled", (WiFi::GetState() != WIFI_OFF) ? 1 : 0) != 0;
    s_bt_enabled   = UIConfig::Int("net.bt_enabled", net_find_bt_driver() ? 1 : 0) != 0;
}

// ── layout constants shared by render + input so hit-testing matches exactly.
//    the input() signature carries no pane width, so we fix the controls column
//    here (pane-local x). (satoru)
static const int CTRL_X_OFF  = 200;   // toggle column relative to pane left (satoru)
static const int SCAN_W      = 90;    // "scan" pill width in px (satoru)
static const int WIFI_LIST_MAX = 6;   // ssid rows we render at most (satoru)

// how many scanned networks we will actually draw. (satoru)
static int net_wifi_rows(){
    int n = WiFi::GetNetworkCount();
    if(n < 0) n = 0;
    if(n > WIFI_LIST_MAX) n = WIFI_LIST_MAX;
    return n;
}

static void network_render(int x, int y, int w, int h, int scroll){
    (void)h;
    int ctrl_x = x + CTRL_X_OFF;
    int ly = y - scroll + 8;
    char buf[64];

    const LinuxDriver* wifi_drv = net_find_wifi_driver();
    const LinuxDriver* bt_drv   = net_find_bt_driver();
    NetworkInterface*  eth      = Network::GetInterface("eth0");

    // ── ethernet ─────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Ethernet");
    ly += 26;

    bool eth_up = eth && eth->state == NIC_UP;
    SettingsUI::Row(x, ly, "Link:", eth ? (eth_up ? "Connected" : "Down") : "No adapter");
    ly += 22;

    if(eth){
        net_ip_str(eth->ip, buf, 64);
        SettingsUI::Row(x, ly, "IPv4:", buf);
        ly += 22;

        net_mac_str(eth->mac, buf, 64);
        SettingsUI::Row(x, ly, "MAC:", buf);
        ly += 22;

        net_ip_str(eth->gateway, buf, 64);
        SettingsUI::Row(x, ly, "Gateway:", buf);
        ly += 22;

        net_ip_str(eth->dns, buf, 64);
        SettingsUI::Row(x, ly, "DNS:", buf);
        ly += 22;

        // rx packets / bytes. (satoru)
        net_intstr((int)eth->rx_packets, buf, 64);
        net_strapp(buf, " pkts / ", 64);
        { char nb[24]; net_intstr((int)eth->rx_bytes, nb, 24); net_strapp(buf, nb, 64); }
        net_strapp(buf, " B", 64);
        SettingsUI::Row(x, ly, "RX:", buf);
        ly += 22;

        // tx packets / bytes. (satoru)
        net_intstr((int)eth->tx_packets, buf, 64);
        net_strapp(buf, " pkts / ", 64);
        { char nb[24]; net_intstr((int)eth->tx_bytes, nb, 24); net_strapp(buf, nb, 64); }
        net_strapp(buf, " B", 64);
        SettingsUI::Row(x, ly, "TX:", buf);
        ly += 22;

        SettingsUI::Row(x, ly, "Driver:", "E1000 (Intel 82540EM)");
        ly += 30;
    } else {
        SettingsUI::Row(x, ly, "Detected:", "No Ethernet adapter");
        ly += 30;
    }

    // ── wi-fi ────────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Wi-Fi");
    ly += 26;

    // enable toggle + a scan pill alongside it. (satoru)
    Graphics::DrawString(x, ly + 2, "Wi-Fi:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Toggle(ctrl_x, ly, s_wifi_enabled);
    SettingsUI::Dropdown(ctrl_x + SettingsUI::TOGGLE_W + 12, ly - 1, SCAN_W, "Scan");
    ly += 30;

    SettingsUI::Row(x, ly, "Status:", WiFi::StateString());
    ly += 22;

    SettingsUI::Row(x, ly, "Adapter:",
        wifi_drv ? (wifi_drv->description[0] ? wifi_drv->description : wifi_drv->name)
                 : "No native Wi-Fi binding");
    ly += 22;

    // connected network summary, if any. (satoru)
    {
        WiFiNetwork* cw = WiFi::GetConnectedNetwork();
        if(WiFi::GetState() == WIFI_CONNECTED && cw){
            SettingsUI::Row(x, ly, "Connected:", cw->ssid[0] ? cw->ssid : "(hidden)");
            ly += 22;
            net_intstr(WiFi::SignalBars(), buf, 64);
            net_strapp(buf, " / 4 bars", 64);
            SettingsUI::Row(x, ly, "Signal:", buf);
            ly += 22;
        }
    }

    // scanned network list  -  each row is tappable to connect (open networks) or
    // disconnect (the connected one). (satoru)
    Graphics::DrawString(x, ly, "Available networks:", SettingsUI::COL_HEADING, 0xFF000000);
    ly += 22;
    {
        int rows = net_wifi_rows();
        WiFiNetwork* nets = WiFi::GetNetworks();
        if(rows > 0 && nets){
            for(int i = 0; i < rows; i++){
                const WiFiNetwork& n = nets[i];
                const char* ssid = n.ssid[0] ? n.ssid : "(hidden)";
                // ssid on the left. (satoru)
                Graphics::DrawString(x + 12, ly,
                                     ssid,
                                     n.connected ? SettingsUI::Accent() : SettingsUI::COL_TEXT,
                                     0xFF000000);
                // security label in the middle. (satoru)
                Graphics::DrawString(ctrl_x, ly, net_sec_str(n.security),
                                     SettingsUI::COL_DIM, 0xFF000000);
                // signal bars on the right. (satoru)
                net_intstr(net_bars_from_dbm(n.signal_strength), buf, 64);
                net_strapp(buf, "/4", 64);
                Graphics::DrawString(ctrl_x + 70, ly, buf, SettingsUI::COL_DIM, 0xFF000000);
                // per-row action hint at the far right. (satoru)
                const char* act = n.connected ? "Disconnect"
                                : (n.security == WIFI_OPEN ? "Connect" : "Locked");
                Graphics::DrawString(ctrl_x + 110, ly, act,
                                     n.connected ? SettingsUI::Accent()
                                                 : (n.security == WIFI_OPEN ? SettingsUI::COL_ON
                                                                            : SettingsUI::COL_DIM),
                                     0xFF000000);
                ly += 20;
            }
        } else {
            Graphics::DrawString(x + 12, ly,
                                 s_wifi_enabled ? "No networks found  -  tap Scan."
                                                : "Enable Wi-Fi to scan.",
                                 SettingsUI::COL_DIM, 0xFF000000);
            ly += 20;
        }
    }
    // honest guidance: open networks connect on tap; secured ones need a
    // passphrase, which this panel has no text-entry field for. (satoru)
    Graphics::DrawString(x + 12, ly, "Tap an open network to connect. WPA needs a password.",
                         SettingsUI::COL_DIM, 0xFF000000);
    ly += 28;

    // ── bluetooth ────────────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Bluetooth");
    ly += 26;

    Graphics::DrawString(x, ly + 2, "Bluetooth:", SettingsUI::COL_TEXT, 0xFF000000);
    SettingsUI::Toggle(ctrl_x, ly, s_bt_enabled);
    ly += 30;

    SettingsUI::Row(x, ly, "Controller:", bt_drv ? "Bound" : "No adapter");
    ly += 22;

    SettingsUI::Row(x, ly, "Driver:",
        bt_drv ? (bt_drv->description[0] ? bt_drv->description : bt_drv->name)
               : "No controller metadata");
    ly += 22;

    if(bt_drv){
        SettingsUI::Row(x, ly, "Binding:", bt_drv->name);
        ly += 22;
    }
    // paired/discoverable state  -  no pairing api in this kernel yet. (satoru)
    SettingsUI::Row(x, ly, "Paired:", "0 devices");
    ly += 22;
    SettingsUI::Row(x, ly, "Discoverable:", s_bt_enabled ? "While panel open" : "Off");
    ly += 30;

    // ── interfaces table ─────────────────────────────────────────────────
    SettingsUI::SectionHeader(x, ly, "Interfaces");
    ly += 26;

    // header row. (satoru)
    Graphics::DrawString(x,       ly, "Iface",  SettingsUI::COL_DIM, 0xFF000000);
    Graphics::DrawString(x + 70,  ly, "IPv4",   SettingsUI::COL_DIM, 0xFF000000);
    Graphics::DrawString(x + 220, ly, "Status", SettingsUI::COL_DIM, 0xFF000000);
    ly += 20;

    // loopback is always up. (satoru)
    Graphics::DrawString(x,       ly, "lo",        SettingsUI::COL_TEXT, 0xFF000000);
    Graphics::DrawString(x + 70,  ly, "127.0.0.1", SettingsUI::COL_TEXT, 0xFF000000);
    Graphics::DrawString(x + 220, ly, "UP",        SettingsUI::COL_ON,   0xFF000000);
    ly += 20;

    // eth0 reflects the live interface. (satoru)
    {
        const char* ipv = " - ";
        if(eth){ net_ip_str(eth->ip, buf, 64); ipv = buf; }
        Graphics::DrawString(x,       ly, "eth0", SettingsUI::COL_TEXT, 0xFF000000);
        Graphics::DrawString(x + 70,  ly, ipv,    SettingsUI::COL_TEXT, 0xFF000000);
        Graphics::DrawString(x + 220, ly, eth_up ? "UP" : "DOWN",
                             eth_up ? SettingsUI::COL_ON : SettingsUI::COL_OFF, 0xFF000000);
        ly += 20;
    }
}

// ── input: pane-local mx,my. we replay the SAME running-y layout used by
//    render (already offset by -scroll) so the hit rects line up. only the
//    interactive rows (wi-fi toggle/scan, bluetooth toggle) need tracking;
//    everything else just advances the cursor. (satoru)
static bool network_input(int mx, int my, bool click, char key, int scroll){
    (void)key;
    if(!click) return false;

    int ctrl_x = CTRL_X_OFF;
    int ly = -scroll + 8;

    NetworkInterface* eth = Network::GetInterface("eth0");
    const LinuxDriver* bt_drv = net_find_bt_driver();

    // ── ethernet block (read-only): mirror render's advances exactly. ────
    ly += 26;                               // "Ethernet" header (satoru)
    ly += 22;                               // link row (satoru)
    if(eth){
        ly += 22;   // ipv4 (satoru)
        ly += 22;   // mac (satoru)
        ly += 22;   // gateway (satoru)
        ly += 22;   // dns (satoru)
        ly += 22;   // rx (satoru)
        ly += 22;   // tx (satoru)
        ly += 30;   // driver (satoru)
    } else {
        ly += 30;   // detected row (satoru)
    }

    // ── wi-fi block ──────────────────────────────────────────────────────
    ly += 26;                               // "Wi-Fi" header (satoru)

    // wi-fi enable toggle. (satoru)
    if(SettingsUI::ToggleHit(ctrl_x, ly, mx, my)){
        s_wifi_enabled = !s_wifi_enabled;
        UIConfig::SetInt("net.wifi_enabled", s_wifi_enabled ? 1 : 0, true);
        UIConfig::Save();
        if(s_wifi_enabled){ WiFi::Enable(); WiFi::Scan(); }
        else               WiFi::Disable();
        return true;
    }
    // scan pill (a dropdown widget reused as a button  -  either arrow rescans). (satoru)
    {
        int hit = SettingsUI::DropdownHit(ctrl_x + SettingsUI::TOGGLE_W + 12, ly - 1, SCAN_W, mx, my);
        if(hit >= 0){
            if(s_wifi_enabled) WiFi::Scan();
            return true;
        }
    }
    ly += 30;                               // toggle/scan row (satoru)
    ly += 22;                               // status row (satoru)
    ly += 22;                               // adapter row (satoru)

    // connected summary occupies two rows when present (matches render). (satoru)
    {
        WiFiNetwork* cw = WiFi::GetConnectedNetwork();
        if(WiFi::GetState() == WIFI_CONNECTED && cw){
            ly += 22;   // connected ssid (satoru)
            ly += 22;   // signal (satoru)
        }
    }

    ly += 22;                               // "Available networks:" label (satoru)
    {
        int rows = net_wifi_rows();
        if(rows > 0){
            WiFiNetwork* nets = WiFi::GetNetworks();
            // each scanned row is a 20px-tall click target spanning the pane. tapping
            // toggles connect/disconnect; open networks join with an empty key, the
            // connected one disconnects, secured ones can't join without a password
            // (no text field here) so we just report the click. (satoru)
            for(int i = 0; i < rows && nets; i++){
                if(my >= ly && my < ly + 20){
                    const WiFiNetwork& n = nets[i];
                    if(n.connected){
                        WiFi::Disconnect();
                    } else if(n.security == WIFI_OPEN && n.ssid[0]){
                        WiFi::Connect(n.ssid, "");
                    }
                    // secured + no-ssid rows: nothing to do but repaint. (satoru)
                    return true;
                }
                ly += 20;
            }
        } else {
            ly += 20;                       // the empty-state line (satoru)
        }
    }
    ly += 28;                               // connect-guidance line (satoru)

    // ── bluetooth block ──────────────────────────────────────────────────
    ly += 26;                               // "Bluetooth" header (satoru)

    // bluetooth enable toggle. (satoru)
    if(SettingsUI::ToggleHit(ctrl_x, ly, mx, my)){
        s_bt_enabled = !s_bt_enabled;
        UIConfig::SetInt("net.bt_enabled", s_bt_enabled ? 1 : 0, true);
        UIConfig::Save();
        return true;
    }
    ly += 30;                               // toggle row (satoru)
    ly += 22;                               // controller row (satoru)
    ly += 22;                               // driver row (satoru)
    if(bt_drv) ly += 22;                    // binding row (satoru)
    ly += 22;                               // paired row (satoru)
    ly += 30;                               // discoverable row (satoru)

    // interfaces table is read-only; no hit-testing needed past here. (satoru)
    return false;
}

// total content height for the scrollbar. it mirrors the conditional advances in
// render so the scroll range tracks what is actually drawn. (satoru)
static int network_content_height(){
    int h = 8;

    NetworkInterface* eth = Network::GetInterface("eth0");
    const LinuxDriver* bt_drv = net_find_bt_driver();

    // ethernet (satoru)
    h += 26 + 22;
    if(eth) h += 22 + 22 + 22 + 22 + 22 + 22 + 30;  // ipv4,mac,gw,dns,rx,tx,driver (satoru)
    else    h += 30;

    // wi-fi (satoru)
    h += 26;            // header (satoru)
    h += 30 + 22 + 22;  // toggle/scan, status, adapter (satoru)
    {
        WiFiNetwork* cw = WiFi::GetConnectedNetwork();
        if(WiFi::GetState() == WIFI_CONNECTED && cw) h += 22 + 22;
    }
    h += 22;            // "available networks:" label (satoru)
    {
        int rows = net_wifi_rows();
        h += (rows > 0) ? (20 * rows) : 20;
    }
    h += 28;            // connect-stub line (satoru)

    // bluetooth (satoru)
    h += 26;                  // header (satoru)
    h += 30 + 22 + 22;        // toggle, controller, driver (satoru)
    if(bt_drv) h += 22;       // binding (satoru)
    h += 22 + 30;             // paired, discoverable (satoru)

    // interfaces table (satoru)
    h += 26 + 20 + 20 + 20;   // header + lo + eth0 (satoru)

    h += 16;                  // tail padding (satoru)
    return h;
}

// `extern` forces EXTERNAL linkage on this const definition so the shell's
// `extern const SettingsModule g_network_module;` resolves at link time. (satoru)
extern const SettingsModule g_network_module = {
    "network", "Network", "\x0b",
    network_on_show, network_render, network_input, network_content_height
};
// end (satoru)
