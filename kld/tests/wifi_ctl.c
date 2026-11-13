// wifi_ctl: M3 in-container nl80211 scan driver.
//
// This is a shim-side control thread, not a ring-3 tool: it builds real
// nl80211 messages and submits them to the in-container genl dispatcher. T1
// gets the wiphy/interface, T2 opens wlan0 through mac80211, T3 triggers a
// wildcard scan, T4 dumps the BSS list, T5 scans again, closes wlan0 and exits.
// (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/if.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/netlink.h>
#include <net/genetlink.h>
#include <net/cfg80211.h>
#include "../wifi/wpa_psk.h"

struct ieee80211_sta;
extern int __real_ieee80211_start_tx_ba_session(struct ieee80211_sta *sta,
                                                u16 tid, u16 timeout);
volatile unsigned long g_tx_ba_sessions;
struct ieee80211_vif;
extern void __real_ieee80211_stop_rx_ba_session(struct ieee80211_vif *vif,
                                                 u16 ba_rx_bitmap, const u8 *addr);
volatile unsigned long g_rx_ba_stops;
void __wrap_ieee80211_stop_rx_ba_session(struct ieee80211_vif *vif,
                                         u16 ba_rx_bitmap, const u8 *addr)
{
    g_rx_ba_stops++;
    __real_ieee80211_stop_rx_ba_session(vif, ba_rx_bitmap, addr);
}
int __wrap_ieee80211_start_tx_ba_session(struct ieee80211_sta *sta,
                                         u16 tid, u16 timeout)
{
    g_tx_ba_sessions++;
    return __real_ieee80211_start_tx_ba_session(sta, tid, timeout);
}

#include "kimpl/lx_kshim.h"

static int g_seq;
static int g_wiphy = -1, g_ifindex = -1, g_iftype = -1;
static u8 g_mac[6];
static char g_ifname[IFNAMSIZ];
static char g_wiphy_name[32];
static volatile int g_scan_event, g_scan_aborted;
static int g_bss_count, g_band24, g_band5, g_band6;
static int g_sig_bad, g_freq_bad;
static struct wiphy *g_wiphy_ptr;
static char g_reg_alpha2[4];
static int g_reg_wiphy_attr;
static int g_connect_mode, g_soak_mode, g_reconnect_mode;
static char g_target_ssid[33];
static int g_target_ssid_len;
static u8 g_target_bssid[6];
static int g_target_freq, g_target_signal, g_target_score = -10000;
static int g_force_2g;
static u8 g_target_rsn[257];
static int g_target_rsn_len;
static char g_psk[129];
static int g_psk_len;
static struct wpa_psk_ctx g_wpa;
static int g_assoc_event, g_assoc_status, g_disconnect_event;
static int g_wpa_done, g_wpa_status;
static int g_pairwise_installed, g_group_idx = -1;
static int g_connect_portid = 0x1000;
static u8 g_eapol_buf[2048];
static int g_eapol_len, g_eapol_ready;
static u32 g_chosen_akm;
static int g_timeout_hint;

static struct sk_buff *wl_req_open(int cmd, int ifindex, int wiphy, bool dump)
{
    struct sk_buff *s = alloc_skb(2048, GFP_KERNEL);
    if (!s)
        return NULL;
    struct nlmsghdr *nlh = nlmsg_put(s, 0x1000, ++g_seq, 0, GENL_HDRLEN,
                                     NLM_F_REQUEST | (dump ? NLM_F_DUMP : 0));
    if (!nlh) { kfree_skb(s); return NULL; }
    struct genlmsghdr *gh = nlmsg_data(nlh);
    gh->cmd = (u8)cmd;
    gh->version = 0;
    gh->reserved = 0;
    if (ifindex >= 0)
        nla_put_u32(s, NL80211_ATTR_IFINDEX, (u32)ifindex);
    if (wiphy >= 0)
        nla_put_u32(s, NL80211_ATTR_WIPHY, (u32)wiphy);
    return s;
}

static void wl_req_close(struct sk_buff *s)
{
    struct nlmsghdr *nlh = (struct nlmsghdr *)s->data;
    nlmsg_end(s, nlh);
}

static void wl_reply_cb(struct sk_buff *s, struct nlmsghdr *nlh)
{
    (void)s;
    struct genlmsghdr *gh = nlmsg_data(nlh);
    (void)gh;
    int len = nlmsg_attrlen(nlh, GENL_HDRLEN);
    struct nlattr *a = (struct nlattr *)nlmsg_attrdata(nlh, GENL_HDRLEN);
    while (nla_ok(a, len)) {
        switch (nla_type(a)) {
        case NL80211_ATTR_WIPHY: g_wiphy = (int)nla_get_u32(a); break;
        case NL80211_ATTR_IFINDEX: g_ifindex = (int)nla_get_u32(a); break;
        case NL80211_ATTR_IFTYPE: g_iftype = (int)nla_get_u32(a); break;
        case NL80211_ATTR_IFNAME:
            strscpy(g_ifname, (const char *)nla_data(a), sizeof(g_ifname));
            break;
        case NL80211_ATTR_MAC:
            if (nla_len(a) >= 6) memcpy(g_mac, nla_data(a), 6);
            break;
        case NL80211_ATTR_WIPHY_NAME:
            strscpy(g_wiphy_name, (const char *)nla_data(a), sizeof(g_wiphy_name));
            break;
        default: break;
        }
        a = nla_next(a, &len);
    }
}

static int wl_doit_cb(int cmd, int ifindex, int wiphy, lx_genl_msg_cb cb)
{
    struct sk_buff *s = wl_req_open(cmd, ifindex, wiphy, false);
    if (!s)
        return -ENOMEM;
    wl_req_close(s);
    lx_genl_set_reply_cb(cb);
    int rc = lx_genl_doit("nl80211", s);
    lx_genl_set_reply_cb(NULL);
    kfree_skb(s);
    return rc;
}

static int wl_doit(int cmd, int ifindex, int wiphy)
{
    return wl_doit_cb(cmd, ifindex, wiphy, wl_reply_cb);
}

static void wl_reg_reply(struct sk_buff *s, struct nlmsghdr *nlh)
{
    (void)s;
    int len = nlmsg_attrlen(nlh, GENL_HDRLEN);
    struct nlattr *a = (struct nlattr *)nlmsg_attrdata(nlh, GENL_HDRLEN);
    while (nla_ok(a, len)) {
        if (nla_type(a) == NL80211_ATTR_REG_ALPHA2)
            strscpy(g_reg_alpha2, (const char *)nla_data(a), sizeof(g_reg_alpha2));
        else if (nla_type(a) == NL80211_ATTR_WIPHY)
            g_reg_wiphy_attr = 1;
        a = nla_next(a, &len);
    }
}

static void wl_print_dbm(int dbm)
{
    if (dbm < 0)
        lx_log("-");
    lx_log_u((unsigned long)(dbm < 0 ? -dbm : dbm));
    lx_log("dBm");
}

static void wl_print_mac(const u8 *m)
{
    static const char h[] = "0123456789abcdef";
    char b[18];
    for (int i = 0; i < 6; i++) {
        b[i * 3] = h[m[i] >> 4];
        b[i * 3 + 1] = h[m[i] & 15];
        b[i * 3 + 2] = (i == 5) ? 0 : ':';
    }
    lx_log(b);
}

static void wl_scan_event(struct sk_buff *s, struct nlmsghdr *nlh)
{
    (void)s;
    struct genlmsghdr *gh = nlmsg_data(nlh);
    if (gh->cmd == NL80211_CMD_NEW_SCAN_RESULTS)
        g_scan_event = 1;
    else if (gh->cmd == NL80211_CMD_SCAN_ABORTED)
        g_scan_aborted = 1;
}

static int wl_trigger_scan(void)
{
    struct sk_buff *s = wl_req_open(NL80211_CMD_TRIGGER_SCAN, g_ifindex, -1, false);
    if (!s)
        return -ENOMEM;
    struct nlattr *ssids = nla_nest_start(s, NL80211_ATTR_SCAN_SSIDS);
    if (ssids) {
        nla_put(s, 0, 0, NULL);          // wildcard
        nla_nest_end(s, ssids);
    }
    wl_req_close(s);
    g_scan_event = g_scan_aborted = 0;
    int rc = lx_genl_doit("nl80211", s);
    kfree_skb(s);
    return rc;
}

static void wl_print_ssid(const u8 *ies, int len)
{
    while (len >= 2) {
        int id = ies[0], l = ies[1];
        if (l + 2 > len)
            break;
        if (id == 0) {
            int any = 0;
            for (int i = 0; i < l; i++)
                if (ies[2 + i]) any = 1;
            if (!l || !any) {
                lx_log("<hidden>");
            } else {
                char b[33];
                int n = l > 32 ? 32 : l;
                for (int i = 0; i < n; i++) {
                    u8 c = ies[2 + i];
                    b[i] = (c >= 32 && c < 127) ? (char)c : '?';
                }
                b[n] = 0;
                lx_log(b);
            }
            return;
        }
        ies += l + 2;
        len -= l + 2;
    }
    lx_log("<hidden>");
}

static const u8 *wl_ie_find(const u8 *ies, int len, u8 id, int *out_len)
{
    while (ies && len >= 2) {
        int l = ies[1];
        if (l + 2 > len)
            break;
        if (ies[0] == id) {
            if (out_len)
                *out_len = l + 2;
            return ies;
        }
        ies += l + 2;
        len -= l + 2;
    }
    return NULL;
}

static void wl_bss_cb(struct sk_buff *s, struct nlmsghdr *nlh)
{
    (void)s;
    int len = nlmsg_attrlen(nlh, GENL_HDRLEN);
    struct nlattr *a = (struct nlattr *)nlmsg_attrdata(nlh, GENL_HDRLEN);
    const u8 *bssid = NULL;
    const u8 *ies = NULL;
    int ie_len = 0, freq = 0, sig = 0, raw_sig = 0;
    while (nla_ok(a, len)) {
        if (nla_type(a) == NL80211_ATTR_BSS) {
            int blen = nla_len(a);
            struct nlattr *b = (struct nlattr *)nla_data(a);
            while (nla_ok(b, blen)) {
                switch (nla_type(b)) {
                case NL80211_BSS_BSSID: bssid = nla_data(b); break;
                case NL80211_BSS_FREQUENCY: freq = (int)nla_get_u32(b); break;
                case NL80211_BSS_SIGNAL_MBM:
                    raw_sig = nla_get_s32(b);
                    sig = raw_sig / 100;
                    break;
                case NL80211_BSS_INFORMATION_ELEMENTS:
                    ies = nla_data(b);
                    ie_len = nla_len(b);
                    break;
                default: break;
                }
                b = nla_next(b, &blen);
            }
        }
        a = nla_next(a, &len);
    }
    if (bssid) {
        if (g_connect_mode && ies) {
            int slen = 0, rlen = 0;
            const u8 *ssid = wl_ie_find(ies, ie_len, 0, &slen);
            const u8 *rsn = wl_ie_find(ies, ie_len, 0x30, &rlen);
            int alen = slen >= 2 ? slen - 2 : 0;
            int pref = 0;
            if (!g_force_2g && freq >= 4900 && freq < 5925 && sig >= -65)
                pref = 1000;       // 5 GHz, above the -65 dBm rule
            else if (!g_force_2g && freq >= 5925 && freq <= 7125 && sig >= -65)
                pref = 1100;       // 6 GHz, same rule
            int score = sig + pref;
            if (ssid && alen == g_target_ssid_len &&
                memcmp(ssid + 2, g_target_ssid, (size_t)alen) == 0 &&
                score > g_target_score) {
                memcpy(g_target_bssid, bssid, 6);
                g_target_freq = freq;
                g_target_signal = sig;
                g_target_score = score;
                g_target_rsn_len = rlen;
                if (rsn && rlen <= (int)sizeof(g_target_rsn))
                    memcpy(g_target_rsn, rsn, (size_t)rlen);
            }
        }
        g_bss_count++;
        if (freq >= 2400 && freq < 2500) g_band24++;
        else if (freq >= 4900 && freq < 5925) g_band5++;
        else if (freq >= 5925 && freq <= 7125) g_band6++;
        if (raw_sig < -10000 || raw_sig > -1000) {
            g_sig_bad++;
            lx_log("[kld-wifi] bad signal raw=");
            lx_log_hex((unsigned long)(u32)raw_sig);
            lx_log("\n");
        }
        if (!g_wiphy_ptr ||
            !ieee80211_get_channel_khz(g_wiphy_ptr, MHZ_TO_KHZ(freq))) {
            g_freq_bad++;
            lx_log("[kld-wifi] bad freq=");
            lx_log_u((unsigned long)freq);
            lx_log("\n");
        }
        lx_log("[kld-wifi] bss ");
        wl_print_mac(bssid);
        lx_log(" freq="); lx_log_u((unsigned long)freq);
        lx_log("MHz signal=");
        wl_print_dbm(sig);
        lx_log(" ssid=");
        if (ies)
            wl_print_ssid(ies, ie_len);
        else
            lx_log("<hidden>");
        lx_log("\n");
    }
}

static int wl_dump_scan(void)
{
    g_bss_count = g_band24 = g_band5 = g_band6 = 0;
    g_sig_bad = g_freq_bad = 0;
    struct sk_buff *s = wl_req_open(NL80211_CMD_GET_SCAN, g_ifindex, -1, true);
    if (!s)
        return -ENOMEM;
    wl_req_close(s);
    int rc = lx_genl_dump("nl80211", s, wl_bss_cb);
    kfree_skb(s);
    lx_log("[kld-wifi] scan: ");
    lx_log_u((unsigned long)g_bss_count);
    lx_log(" bss (2.4=");
    lx_log_u((unsigned long)g_band24);
    lx_log(" 5=");
    lx_log_u((unsigned long)g_band5);
    lx_log(" 6=");
    lx_log_u((unsigned long)g_band6);
    lx_log(")\n");
    if (g_sig_bad || g_freq_bad) {
        lx_log("[kld-wifi] FAIL invalid bss signal/freq\n");
        return -ERANGE;
    }
    return rc;
}

static bool wl_wait_scan(int seconds)
{
    for (int i = 0; i < seconds * 10; i++) {
        if (g_scan_event || g_scan_aborted)
            return true;
        lx_os_sleep_ns(100000000);
    }
    return false;
}

static void wl_wipe(void *p, unsigned long n)
{
    volatile u8 *b = (volatile u8 *)p;
    while (n--)
        *b++ = 0;
}

static bool wl_read_wifi_conf(void)
{
    u8 buf[512];
    long n = lx_os_read_file("/run/kld/wifi.conf", buf, sizeof(buf) - 1);
    if (n <= 0) {
        lx_log("[kld-wifi] wifi.conf missing\n");
        return false;
    }
    buf[n] = 0;
    int ok_ssid = 0, ok_psk = 0;
    char *p = (char *)buf;
    while (*p) {
        char *line = p;
        char *e = p;
        while (*e && *e != '\n') e++;
        char save = *e;
        *e = 0;
        int l = (int)strlen(line);
        if (l && line[l - 1] == '\r') line[--l] = 0;
        if (!strncmp(line, "SSID=", 5)) {
            int l2 = l - 5;
            if (l2 > 32) l2 = 32;
            memcpy(g_target_ssid, line + 5, (size_t)l2);
            g_target_ssid[l2] = 0;
            g_target_ssid_len = l2;
            ok_ssid = l2 > 0;
        } else if (!strncmp(line, "PSK=", 4)) {
            int l2 = l - 4;
            if (l2 > 128) l2 = 128;
            memcpy(g_psk, line + 4, (size_t)l2);
            g_psk[l2] = 0;
            g_psk_len = l2;
            ok_psk = l2 >= 8;
        }
        *e = save;
        p = *e ? e + 1 : e;
    }
    wl_wipe(buf, sizeof(buf));
    if (!ok_ssid || !ok_psk) {
        lx_log("[kld-wifi] wifi.conf invalid\n");
        wpa_psk_deinit(&g_wpa); // no-op on a zeroed ctx
        wl_wipe(g_psk, sizeof(g_psk));
        return false;
    }
    return true;
}

static void wl_print_security(void)
{
    if (g_target_rsn_len < 22) {
        lx_log("[kld-wifi] security: no RSN element\n");
        return;
    }
    const u8 *p = g_target_rsn + 2;
    int len = g_target_rsn_len - 2;
    if (len < 8)
        return;
    u32 group = (u32)p[2] << 24 | (u32)p[3] << 16 | (u32)p[4] << 8 | p[5];
    int pc = p[6] | (p[7] << 8);
    int off = 8;
    lx_log("[kld-wifi] security: RSN group=");
    lx_log_hex(group);
    lx_log(" pairwise=");
    for (int i = 0; i < pc && off + 4 <= len; i++, off += 4) {
        u32 c = (u32)p[off] << 24 | (u32)p[off+1] << 16 | (u32)p[off+2] << 8 | p[off+3];
        lx_log_hex(c);
        lx_log(",");
    }
    if (off + 2 <= len) {
        int ac = p[off] | (p[off+1] << 8);
        off += 2;
        lx_log(" akm=");
        for (int i = 0; i < ac && off + 4 <= len; i++, off += 4) {
            u32 a = (u32)p[off] << 24 | (u32)p[off+1] << 16 | (u32)p[off+2] << 8 | p[off+3];
            lx_log_hex(a);
            lx_log(",");
        }
    }
    u16 caps = 0;
    if (off + 2 <= len)
        caps = (u16)(p[off] | (p[off+1] << 8));
    lx_log(" caps=");
    lx_log_hex(caps);
    lx_log(" mfpc=");
    lx_log_u((caps & 0x0080) ? 1 : 0);
    lx_log(" mfpr=");
    lx_log_u((caps & 0x0040) ? 1 : 0);
    lx_log("\n");
}

static void wl_mlme_event(struct sk_buff *s, struct nlmsghdr *nlh)
{
    (void)s;
    struct genlmsghdr *gh = nlmsg_data(nlh);
    if (gh->cmd == NL80211_CMD_CONNECT) {
        int len = nlmsg_attrlen(nlh, GENL_HDRLEN);
        struct nlattr *a = (struct nlattr *)nlmsg_attrdata(nlh, GENL_HDRLEN);
        while (nla_ok(a, len)) {
            if (nla_type(a) == NL80211_ATTR_STATUS_CODE)
                g_assoc_status = (int)nla_get_u16(a);
            if (nla_type(a) == NL80211_ATTR_MAC && nla_len(a) >= 6)
                memcpy(g_target_bssid, nla_data(a), 6);
            a = nla_next(a, &len);
        }
        g_assoc_event = 1;
    } else if (gh->cmd == NL80211_CMD_DISCONNECT) {
        g_disconnect_event = 1;
    }
}

static void wl_control_port_event(struct sk_buff *s, struct nlmsghdr *nlh)
{
    (void)s;
    struct genlmsghdr *gh = nlmsg_data(nlh);
    if (gh->cmd != NL80211_CMD_CONTROL_PORT_FRAME)
        return;
    int len = nlmsg_attrlen(nlh, GENL_HDRLEN);
    struct nlattr *a = (struct nlattr *)nlmsg_attrdata(nlh, GENL_HDRLEN);
    while (nla_ok(a, len)) {
        if (nla_type(a) == NL80211_ATTR_FRAME) {
            int l = nla_len(a);
            if (l > (int)sizeof(g_eapol_buf))
                l = (int)sizeof(g_eapol_buf);
            memcpy(g_eapol_buf, nla_data(a), (size_t)l);
            g_eapol_len = l;
            g_eapol_ready = 1;
            break;
        }
        a = nla_next(a, &len);
    }
}

static int wl_tx_eapol(void *priv, const u8 *frame, unsigned int len)
{
    (void)priv;
    struct sk_buff *s = wl_req_open(NL80211_CMD_CONTROL_PORT_FRAME, g_ifindex, -1, false);
    if (!s)
        return -ENOMEM;
    nla_put(s, NL80211_ATTR_FRAME, (int)len, frame);
    nla_put_u16(s, NL80211_ATTR_CONTROL_PORT_ETHERTYPE, 0x888e);
    nla_put(s, NL80211_ATTR_MAC, 6, g_target_bssid);
    if (!g_pairwise_installed)
        nla_put_flag(s, NL80211_ATTR_CONTROL_PORT_NO_ENCRYPT);
    wl_req_close(s);
    int rc = lx_genl_doit("nl80211", s);
    kfree_skb(s);
    return rc;
}

static int wl_set_key(void *priv, enum wpa_psk_key_type type,
                      unsigned int key_idx, const u8 *key, unsigned int key_len,
                      const u8 *rsc, unsigned int rsc_len, u8 tx)
{
    (void)priv; (void)tx;
    struct sk_buff *s = wl_req_open(NL80211_CMD_NEW_KEY, g_ifindex, -1, false);
    if (!s)
        return -ENOMEM;
    u32 cipher = 0x000fac04;   // WLAN_CIPHER_SUITE_CCMP
    u32 ktype = type == WPA_PSK_KEY_PAIRWISE ? 1 : 0;
    if (type == WPA_PSK_KEY_IGTK)
        cipher = 0x000fac06;   // AES-CMAC for the IGTK
    nla_put(s, NL80211_ATTR_KEY_DATA, (int)key_len, key);
    nla_put_u32(s, NL80211_ATTR_KEY_CIPHER, cipher);
    nla_put_u8(s, NL80211_ATTR_KEY_IDX, (u8)key_idx);
    nla_put_u32(s, NL80211_ATTR_KEY_TYPE, ktype);
    nla_put(s, NL80211_ATTR_MAC, 6, g_target_bssid);
    if (rsc && rsc_len)
        nla_put(s, NL80211_ATTR_KEY_SEQ, (int)rsc_len, rsc);
    if (type == WPA_PSK_KEY_GROUP)
        nla_put_flag(s, NL80211_ATTR_KEY_DEFAULT);
    wl_req_close(s);
    int rc = lx_genl_doit("nl80211", s);
    kfree_skb(s);
    if (rc)
        return rc;
    if (type == WPA_PSK_KEY_PAIRWISE) {
        struct nl80211_sta_flag_update flags;
        memset(&flags, 0, sizeof(flags));
        flags.mask = 1u << NL80211_STA_FLAG_AUTHORIZED;
        flags.set = 1u << NL80211_STA_FLAG_AUTHORIZED;
        s = wl_req_open(NL80211_CMD_SET_STATION, g_ifindex, -1, false);
        if (!s)
            return -ENOMEM;
        nla_put(s, NL80211_ATTR_MAC, 6, g_target_bssid);
        nla_put(s, NL80211_ATTR_STA_FLAGS2, (int)sizeof(flags), &flags);
        wl_req_close(s);
        rc = lx_genl_doit("nl80211", s);
        kfree_skb(s);
        if (rc)
            return rc;
        g_pairwise_installed = 1;
    } else if (type == WPA_PSK_KEY_GROUP) {
        g_group_idx = (int)key_idx;
    }
    return 0;
}

static void wl_wpa_done(void *priv, enum wpa_psk_status status)
{
    (void)priv;
    g_wpa_done = 1;
    g_wpa_status = status;
}

static void wl_print_rate(const char *tag, struct nlattr *n)
{
    int len = nla_len(n);
    struct nlattr *a = (struct nlattr *)nla_data(n);
    unsigned int raw = 0, mcs = 0, nss = 0;
    const char *bw = "20", *gi = "long", *mode = "legacy";
    while (nla_ok(a, len)) {
        switch (nla_type(a)) {
        case NL80211_RATE_INFO_BITRATE: raw = nla_get_u16(a); break;
        case NL80211_RATE_INFO_BITRATE32: raw = nla_get_u32(a); break;
        case NL80211_RATE_INFO_MCS: mcs = nla_get_u8(a); break;
        case NL80211_RATE_INFO_VHT_MCS: mcs = nla_get_u8(a); mode = "VHT"; break;
        case NL80211_RATE_INFO_VHT_NSS: nss = nla_get_u8(a); break;
        case NL80211_RATE_INFO_HE_MCS: mcs = nla_get_u8(a); mode = "HE"; break;
        case NL80211_RATE_INFO_HE_NSS: nss = nla_get_u8(a); break;
        case NL80211_RATE_INFO_EHT_MCS: mcs = nla_get_u8(a); mode = "EHT"; break;
        case NL80211_RATE_INFO_EHT_NSS: nss = nla_get_u8(a); break;
        case NL80211_RATE_INFO_40_MHZ_WIDTH: bw = "40"; break;
        case NL80211_RATE_INFO_80_MHZ_WIDTH: bw = "80"; break;
        case NL80211_RATE_INFO_80P80_MHZ_WIDTH: bw = "80+80"; break;
        case NL80211_RATE_INFO_160_MHZ_WIDTH: bw = "160"; break;
        case NL80211_RATE_INFO_320_MHZ_WIDTH: bw = "320"; break;
        case NL80211_RATE_INFO_SHORT_GI: gi = "short"; break;
        case NL80211_RATE_INFO_HE_GI: gi = "HE"; break;
        case NL80211_RATE_INFO_EHT_GI: gi = "EHT"; break;
        default: break;
        }
        a = nla_next(a, &len);
    }
    lx_log(tag); lx_log("=");
    lx_log_u(raw / 10); lx_log("."); lx_log_u(raw % 10);
    lx_log("Mbps/"); lx_log(mode);
    lx_log(" mcs="); lx_log_u(mcs);
    lx_log(" nss="); lx_log_u(nss);
    lx_log(" bw="); lx_log(bw);
    lx_log(" gi="); lx_log(gi);
}

static void wl_sta_reply(struct sk_buff *s, struct nlmsghdr *nlh)
{
    (void)s;
    int len = nlmsg_attrlen(nlh, GENL_HDRLEN);
    struct nlattr *a = (struct nlattr *)nlmsg_attrdata(nlh, GENL_HDRLEN);
    struct nlattr *sinfo = NULL;
    while (nla_ok(a, len)) {
        if (nla_type(a) == NL80211_ATTR_STA_INFO)
            sinfo = a;
        a = nla_next(a, &len);
    }
    if (!sinfo) {
        lx_log("[kld-wifi] link: no station info\n");
        return;
    }
    int slen = nla_len(sinfo);
    struct nlattr *b = (struct nlattr *)nla_data(sinfo);
    lx_log("[kld-wifi] link: ");
    while (nla_ok(b, slen)) {
        switch (nla_type(b)) {
        case NL80211_STA_INFO_SIGNAL:
            lx_log("signal="); wl_print_dbm((s8)nla_get_u8(b)); lx_log(" ");
            break;
        case NL80211_STA_INFO_SIGNAL_AVG:
            lx_log("signal_avg="); wl_print_dbm((s8)nla_get_u8(b)); lx_log(" ");
            break;
        case NL80211_STA_INFO_TX_BITRATE:
            wl_print_rate("tx", b); lx_log(" ");
            break;
        case NL80211_STA_INFO_RX_BITRATE:
            wl_print_rate("rx", b); lx_log(" ");
            break;
        case NL80211_STA_INFO_TX_PACKETS:
            lx_log("tx_packets="); lx_log_u(nla_get_u32(b)); lx_log(" ");
            break;
        case NL80211_STA_INFO_TX_RETRIES:
            lx_log("tx_retries="); lx_log_u(nla_get_u32(b)); lx_log(" ");
            break;
        case NL80211_STA_INFO_TX_FAILED:
            lx_log("tx_failed="); lx_log_u(nla_get_u32(b)); lx_log(" ");
            break;
        case NL80211_STA_INFO_RX_PACKETS:
            lx_log("rx_packets="); lx_log_u(nla_get_u32(b)); lx_log(" ");
            break;
        case NL80211_STA_INFO_RX_DROP_MISC:
            lx_log("rx_dropped="); lx_log_u(nla_get_u32(b)); lx_log(" ");
            break;
        case NL80211_STA_INFO_BEACON_LOSS:
            lx_log("beacon_loss="); lx_log_u(nla_get_u32(b)); lx_log(" ");
            break;
        case NL80211_STA_INFO_CONNECTED_TIME:
            lx_log("connected_s="); lx_log_u(nla_get_u32(b)); lx_log(" ");
            break;
        default: break;
        }
        b = nla_next(b, &slen);
    }
    lx_log("\n");
}

static void wl_get_station(void)
{
    struct sk_buff *s = wl_req_open(NL80211_CMD_GET_STATION, g_ifindex, -1, false);
    if (!s)
        return;
    nla_put(s, NL80211_ATTR_MAC, 6, g_target_bssid);
    wl_req_close(s);
    lx_genl_set_reply_cb(wl_sta_reply);
    int rc = lx_genl_doit("nl80211", s);
    if (rc) {
        lx_log("[kld-wifi] link: GET_STATION rc=");
        lx_log_u((unsigned long)(-rc));
        lx_log("\n");
    }
    lx_genl_set_reply_cb(NULL);
    kfree_skb(s);
    lx_log("[kld-wifi] agg: tx_ba_sessions=");
    lx_log_u(g_tx_ba_sessions);
    lx_log(" rx_ba_stops=");
    lx_log_u(g_rx_ba_stops);
    lx_log(" amsdu=not-exposed\n");
}

static void wl_disable_ps(void)
{
    struct sk_buff *s = wl_req_open(NL80211_CMD_SET_POWER_SAVE, g_ifindex, -1, false);
    if (!s)
        return;
    nla_put_u32(s, NL80211_ATTR_PS_STATE, NL80211_PS_DISABLED);
    wl_req_close(s);
    lx_genl_set_reply_cb(wl_reply_cb);
    int rc = lx_genl_doit("nl80211", s);
    lx_genl_set_reply_cb(NULL);
    kfree_skb(s);
    lx_log("[kld-wifi] power save: ");
    lx_log(rc ? "set failed" : "off");
    lx_log("\n");
}

static int wl_connect_flow(bool final_session)
{
    if (!wl_read_wifi_conf())
        return 1;

    g_target_ssid[g_target_ssid_len] = 0;
    lx_log("[kld-wifi] connect target: ");
    lx_log(g_target_ssid);
    lx_log("\n");

    /* one scan; the target BSS is chosen by SSID + strongest signal */
    lx_genl_listen("nl80211", "scan", wl_scan_event);
    g_scan_event = g_scan_aborted = 0;
    g_assoc_event = g_disconnect_event = 0;
    g_assoc_status = 0;
    g_wpa_done = 0;
    g_wpa_status = 0;
    g_pairwise_installed = 0;
    g_group_idx = -1;
    g_eapol_ready = 0;
    g_target_rsn_len = 0;
    g_target_signal = -1000;
    g_target_score = -10000;
    g_target_freq = 0;
    int rc = wl_trigger_scan();
    if (rc) {
        lx_log("[kld-wifi] scan trigger FAIL rc="); lx_log_u((unsigned long)(-rc)); lx_log("\n");
        wl_wipe(g_psk, sizeof(g_psk));
        return 1;
    }
    if (!wl_wait_scan(20)) {
        lx_log("[kld-wifi] scan timeout\n");
        wl_wipe(g_psk, sizeof(g_psk));
        return 1;
    }
    wl_dump_scan();
    if (g_target_signal == -1000 || g_target_ssid_len == 0) {
        lx_log("[kld-wifi] target SSID not found\n");
        wl_wipe(g_psk, sizeof(g_psk));
        return 1;
    }
    if (g_force_2g && g_target_freq >= 2400 && g_target_freq < 2500)
        lx_log("[kld-wifi] band rule: 2.4GHz diagnostic override\n");
    else if (g_target_freq >= 5925 && g_target_freq <= 7125 && g_target_signal >= -65)
        lx_log("[kld-wifi] band rule: 6GHz preferred (signal >= -65dBm)\n");
    else if (g_target_freq >= 4900 && g_target_freq < 5925 && g_target_signal >= -65)
        lx_log("[kld-wifi] band rule: 5GHz preferred (signal >= -65dBm)\n");
    else
        lx_log("[kld-wifi] band rule: 2.4GHz fallback (5/6GHz below -65dBm or absent)\n");
    lx_log("[kld-wifi] target bssid="); wl_print_mac(g_target_bssid);
    lx_log(" freq="); lx_log_u((unsigned long)g_target_freq);
    lx_log(" signal=");
    wl_print_dbm(g_target_signal);
    lx_log("\n");
    wl_print_security();

    if (g_target_rsn_len == 0) {
        lx_log("[kld-wifi] AP has no RSN element; open/WEP/WPA1 unsupported\n");
        wl_wipe(g_psk, sizeof(g_psk));
        return 1;
    }

    struct wpa_psk_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.ssid = (const u8 *)g_target_ssid;
    cfg.ssid_len = (unsigned int)g_target_ssid_len;
    cfg.passphrase = g_psk;
    cfg.passphrase_len = (unsigned int)g_psk_len;
    memcpy(cfg.own_mac, g_mac, 6);
    memcpy(cfg.bssid, g_target_bssid, 6);
    cfg.rsn_ie = g_target_rsn;
    cfg.rsn_ie_len = (unsigned int)g_target_rsn_len;
    struct wpa_psk_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.tx_eapol = wl_tx_eapol;
    ops.set_key = wl_set_key;
    ops.done = wl_wpa_done;
    memset(&g_wpa, 0, sizeof(g_wpa));
    rc = wpa_psk_init(&g_wpa, &cfg, &ops);
    wl_wipe(g_psk, sizeof(g_psk));
    g_psk_len = 0;
    if (rc) {
        lx_log("[kld-wifi] AP security unsupported: ");
        lx_log(wpa_psk_status_name(rc == -EOPNOTSUPP ? WPA_PSK_STATUS_UNSUPPORTED_AP_SECURITY
                                                    : WPA_PSK_STATUS_KEY_ERROR));
        lx_log("\n");
        return 1;
    }
    g_chosen_akm = 0x000fac00u | g_wpa.assoc_ie[18];

    lx_genl_listen("nl80211", "mlme", wl_mlme_event);
    lx_genl_listen_portid((u32)g_connect_portid, wl_control_port_event);

    struct sk_buff *s = wl_req_open(NL80211_CMD_CONNECT, g_ifindex, -1, false);
    if (!s) { wpa_psk_deinit(&g_wpa); return 1; }
    nla_put(s, NL80211_ATTR_SSID, g_target_ssid_len, g_target_ssid);
    nla_put(s, NL80211_ATTR_MAC, 6, g_target_bssid);
    nla_put_u32(s, NL80211_ATTR_WIPHY_FREQ, (u32)g_target_freq);
    nla_put(s, NL80211_ATTR_IE, (int)g_wpa.assoc_ie_len, g_wpa.assoc_ie);
    nla_put_u32(s, NL80211_ATTR_AUTH_TYPE, NL80211_AUTHTYPE_OPEN_SYSTEM);
    nla_put_flag(s, NL80211_ATTR_PRIVACY);
    nla_put_u32(s, NL80211_ATTR_WPA_VERSIONS, NL80211_WPA_VERSION_2);
    u32 ccmp = 0x000fac04;
    nla_put(s, NL80211_ATTR_CIPHER_SUITES_PAIRWISE, 4, &ccmp);
    nla_put_u32(s, NL80211_ATTR_CIPHER_SUITE_GROUP, ccmp);
    nla_put(s, NL80211_ATTR_AKM_SUITES, 4, &g_chosen_akm);
    if (g_wpa.mfp_required)
        nla_put_u32(s, NL80211_ATTR_USE_MFP, NL80211_MFP_REQUIRED);
    nla_put_flag(s, NL80211_ATTR_CONTROL_PORT);
    nla_put_u16(s, NL80211_ATTR_CONTROL_PORT_ETHERTYPE, 0x888e);
    nla_put_flag(s, NL80211_ATTR_CONTROL_PORT_OVER_NL80211);
    nla_put_flag(s, NL80211_ATTR_SOCKET_OWNER);
    wl_req_close(s);
    lx_genl_set_reply_cb(wl_reply_cb);
    rc = lx_genl_doit("nl80211", s);
    lx_genl_set_reply_cb(NULL);
    kfree_skb(s);
    if (rc) {
        lx_log("[kld-wifi] connect FAIL rc="); lx_log_u((unsigned long)(-rc)); lx_log("\n");
        wpa_psk_deinit(&g_wpa);
        return 1;
    }

    u64 t0 = lx_uptime_ns();
    while (!g_assoc_event && (lx_uptime_ns() - t0) < 10000000000ull)
        lx_os_sleep_ns(10000000ull);
    if (!g_assoc_event) {
        lx_log("[kld-wifi] association timeout\n");
        wpa_psk_deinit(&g_wpa);
        return 1;
    }
    if (g_assoc_status != 0) {
        lx_log("[kld-wifi] association FAIL status="); lx_log_u((unsigned long)g_assoc_status); lx_log("\n");
        wpa_psk_deinit(&g_wpa);
        return 1;
    }
    lx_log("[kld-wifi] associated: "); wl_print_mac(g_target_bssid);
    lx_log(" "); lx_log_u((unsigned long)g_target_freq); lx_log(" status=0\n");

    u64 w0 = lx_uptime_ns();
    u64 t2of4 = 0;
    while (!g_wpa_done) {
        if (g_eapol_ready) {
            g_eapol_ready = 0;
            wpa_psk_rx_eapol(&g_wpa, g_eapol_buf, (unsigned int)g_eapol_len);
            wl_wipe(g_eapol_buf, sizeof(g_eapol_buf));
        }
        if (!t2of4 && wpa_psk_state(&g_wpa) == WPA_PSK_STATE_WAIT_3OF4)
            t2of4 = lx_uptime_ns();
        if (t2of4 && (lx_uptime_ns() - t2of4) > 5000000000ull) {
            int tr = wpa_psk_timeout(&g_wpa);
            if (tr == 0) {
                lx_log("[kld-wifi] wpa2: no 3/4 from the AP (wrong passphrase?)\n");
                g_timeout_hint = 1;
                wpa_psk_deinit(&g_wpa);
                lx_genl_notify_release((u32)g_connect_portid);
                lx_genl_clear_portid((u32)g_connect_portid);
                lx_net_close_name("wlan0");
                lx_log("[kld-wifi] wlan0 down\n");
                lx_log("[kld-wifi] disconnect done\n");
                lx_os_exit(0);
                return 0;
            }
            t2of4 = lx_uptime_ns();
        }
        if ((lx_uptime_ns() - w0) > 15000000000ull)
            break;
        lx_os_sleep_ns(10000000ull);
    }
    if (!g_wpa_done || g_wpa_status != WPA_PSK_STATUS_OK) {
        lx_log("[kld-wifi] wpa2: handshake failed status=");
        lx_log(wpa_psk_status_name((enum wpa_psk_status)g_wpa_status));
        lx_log("\n");
        wpa_psk_deinit(&g_wpa);
        lx_genl_notify_release((u32)g_connect_portid);
        lx_genl_clear_portid((u32)g_connect_portid);
        return 1;
    }
    lx_log("[kld-wifi] wpa2: 4-way handshake complete (pairwise + group idx ");
    lx_log_u((unsigned long)(g_group_idx >= 0 ? g_group_idx : 0));
    lx_log(" installed)\n");

    /* port authorized: attach wlan0 to the kernel bridge now, not earlier. */
    lx_net_bringup_all();
    lx_log("[kld-wifi] wlan0 attached to the kernel bridge\n");
    wl_get_station();

    /* keep the link up while the kernel DHCP client and the bench use it. */
    if (g_soak_mode) {
        for (int i = 0; i < 31; i++) {
            msleep(60000);
            lx_log("[kld-wifi] soak minute=");
            lx_log_u((unsigned long)(i + 1));
            lx_log("\n");
            wl_get_station();
        }
    } else {
        for (int i = 0; i < 180; i++)
            msleep(1000);
    }

    wl_get_station();

    struct sk_buff *d = wl_req_open(NL80211_CMD_DISCONNECT, g_ifindex, -1, false);
    if (d) {
        wl_req_close(d);
        lx_genl_doit("nl80211", d);
        kfree_skb(d);
    }
    u64 td = lx_uptime_ns();
    while (!g_disconnect_event && (lx_uptime_ns() - td) < 3000000000ull)
        lx_os_sleep_ns(10000000ull);
    lx_log("[kld-wifi] agg final: tx_ba_sessions=");
    lx_log_u(g_tx_ba_sessions);
    lx_log(" rx_ba_stops=");
    lx_log_u(g_rx_ba_stops);
    lx_log("\n");
    wpa_psk_deinit(&g_wpa);
    lx_genl_notify_release((u32)g_connect_portid);
    lx_genl_clear_portid((u32)g_connect_portid);
    if (final_session) {
        lx_net_close_name("wlan0");
        lx_log("[kld-wifi] wlan0 down\n");
        lx_log("[kld-wifi] disconnect done\n");
        lx_os_exit(0);
    }
    lx_log("[kld-wifi] first session disconnected\n");
    return 0;
}

static int wl_wifi_ctl(void *arg)
{
    (void)arg;
    /* wait for wlan0 to appear (the probe may still be finishing) */
    for (int i = 0; i < 1200; i++) {
        g_ifindex = lx_net_ifindex_name("wlan0");
        if (g_ifindex > 0)
            break;
        lx_os_sleep_ns(100000000);
    }
    if (g_ifindex <= 0) {
        lx_log("[kld-wifi] FAIL no wlan0\n");
        return 1;
    }

    g_wiphy = -1;
    int rc = wl_doit(NL80211_CMD_GET_INTERFACE, g_ifindex, -1);
    if (rc == 0) {
        lx_log("[kld-wifi] ifname=");
        lx_log(g_ifname[0] ? g_ifname : "?");
        lx_log(" ifindex="); lx_log_u((unsigned long)g_ifindex);
        lx_log(" iftype="); lx_log_u((unsigned long)g_iftype);
        lx_log(" mac="); wl_print_mac(g_mac);
        lx_log("\n");
        int wiphy = g_wiphy >= 0 ? g_wiphy : 0;
        rc = wl_doit(NL80211_CMD_GET_WIPHY, -1, wiphy);
        if (rc == 0) {
            lx_log("[kld-wifi] wiphy=");
            lx_log(g_wiphy_name[0] ? g_wiphy_name : "?");
            lx_log(" index="); lx_log_u((unsigned long)wiphy);
            lx_log("\n");
            struct net_device *d = __dev_get_by_index(&init_net, g_ifindex);
            struct wireless_dev *wd = d ? d->ieee80211_ptr : NULL;
            if (wd) {
                g_wiphy_ptr = wd->wiphy;
                const struct ieee80211_supported_band *b6 =
                    wd->wiphy->bands[NL80211_BAND_6GHZ];
                if (b6 && b6->n_channels) {
                    lx_log("[kld-wifi] 6ghz: ");
                    lx_log_u((unsigned long)b6->channels[0].center_freq);
                    lx_log("MHz flags=");
                    lx_log_hex((unsigned long)b6->channels[0].flags);
                    lx_log("\n");
                } else {
                    lx_log("[kld-wifi] 6ghz: none\n");
                }
            }
        }
    }
    if (rc != 0) {
        lx_log("[kld-wifi] FAIL nl80211 rc=");
        lx_log_u((unsigned long)(-rc));
        lx_log("\n");
        return 1;
    }
    lx_log("[kld-wifi] nl80211 ok\n");

    rc = lx_net_open_name("wlan0");
    if (rc != 0) {
        lx_log("[kld-wifi] wlan0 up FAIL rc=");
        lx_log_u((unsigned long)(-rc));
        lx_log("\n");
        return 1;
    }
    lx_log("[kld-wifi] wlan0 up\n");
    lx_log("[kld-wifi] irq_deliveries=");
    lx_log_u(lx_irq_total());
    lx_log("\n");

    if (g_connect_mode) {
        int sessions = g_reconnect_mode ? 2 : 1;
        for (int session = 0; session < sessions; session++) {
            int crc = wl_connect_flow(session == sessions - 1);
            if (crc)
                return crc;
            if (session + 1 < sessions) {
                lx_log("[kld-wifi] reconnect in 5s\n");
                msleep(5000);
            }
        }
        lx_os_exit(0);
    }

    lx_genl_listen("nl80211", "scan", wl_scan_event);

    for (int pass = 1; pass <= 2; pass++) {
        rc = wl_trigger_scan();
        if (rc != 0) {
            lx_log("[kld-wifi] scan trigger FAIL rc=");
            lx_log_u((unsigned long)(-rc));
            lx_log("\n");
            return 1;
        }
        lx_log("[kld-wifi] scan triggered\n");
        if (!wl_wait_scan(40)) {
            lx_log("[kld-wifi] scan timeout\n");
            return 1;
        }
        if (g_scan_aborted) {
            lx_log("[kld-wifi] scan aborted\n");
            return 1;
        }
        rc = wl_dump_scan();
        if (rc == -ERANGE)
            return 1;
        if (pass == 1) {
            memset(g_reg_alpha2, 0, sizeof(g_reg_alpha2));
            g_reg_wiphy_attr = 0;
            int rr = wl_doit_cb(NL80211_CMD_GET_REG, -1, g_wiphy, wl_reg_reply);
            lx_log("[kld-wifi] regdom: ");
            if (rr == 0 && g_reg_alpha2[0])
                lx_log(g_reg_alpha2);
            else
                lx_log("??");
            lx_log(" wiphy_regd=");
            lx_log_u((unsigned long)g_reg_wiphy_attr);
            lx_log("\n");
            msleep(4000);
            msleep(4000);
            msleep(2000);
        }
    }

    lx_log("[kld-wifi] max_yield_spin_us=");
    lx_log_u(lx_time_spin_max_ns() / 1000);
    lx_log("\n");
    lx_net_close_name("wlan0");
    lx_log("[kld-wifi] wlan0 down\n");
    lx_os_exit(0);
    return 0;
}

void lx_wifi_scan_start(void)
{
    g_connect_mode = 0;
    lx_task_spawn("kld-wifi", wl_wifi_ctl, NULL);
}

void lx_wifi_connect_start(void)
{
    extern int lx_main_argc;
    extern char **lx_main_argv;
    for (int i = 1; i < lx_main_argc; i++) {
        if (!lx_main_argv[i])
            continue;
        if (strcmp(lx_main_argv[i], "band24") == 0)
            g_force_2g = 1;
        if (strcmp(lx_main_argv[i], "soak") == 0)
            g_soak_mode = 1;
        if (strcmp(lx_main_argv[i], "reconnect") == 0)
            g_reconnect_mode = 1;
    }
    g_connect_mode = 1;
    lx_task_spawn("kld-wifi", wl_wifi_ctl, NULL);
}

// end (satoru)
