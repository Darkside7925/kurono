// wpa_psk: a small WPA2-PSK supplicant state machine (pure logic, no radio).
//
// the caller moves bytes: it puts ctx.assoc_ie into the association request,
// feeds every received EAPOL-Key frame (ethertype 0x888E payload) to
// wpa_psk_rx_eapol(), and implements the three ops below on its own stack.
// the library never logs and never exposes the passphrase, PMK, PTK, KCK, KEK,
// TK or GTK through the API. (satoru)
//
// rules are from IEEE 802.11-2020 clause 12.7; the clause is cited next to each
// rule in wpa_psk.c. (satoru)

#ifndef KLD_WPA_PSK_H
#define KLD_WPA_PSK_H

#include <linux/types.h>

#define WPA_PSK_ASSOC_IE_MAX 64
#define WPA_PSK_PRIV_QWORDS  512

enum wpa_psk_key_type {
    WPA_PSK_KEY_PAIRWISE = 0,
    WPA_PSK_KEY_GROUP = 1,
    WPA_PSK_KEY_IGTK = 2,
};

enum wpa_psk_status {
    WPA_PSK_STATUS_OK = 0,
    WPA_PSK_STATUS_TIMEOUT_HINT,
    WPA_PSK_STATUS_UNSUPPORTED_AP_SECURITY,
    WPA_PSK_STATUS_KEY_ERROR,
};

// caller-visible handshake state. idle includes "init done, waiting for the
// first 1/4"; waiting means our 2/4 is out; completed means a verified 3/4
// installed the tk. all calls are allowed in any state except wpa_psk_timeout,
// which only has an effect in waiting-3of4. (satoru)
enum wpa_psk_state {
    WPA_PSK_STATE_IDLE = 0,
    WPA_PSK_STATE_WAIT_3OF4,
    WPA_PSK_STATE_COMPLETED,
};

// per-reason drop counters for received frames. a dropped frame is never a
// terminal done(); these counters are how the caller sees what an attacker or
// a broken AP sent. (satoru)
struct wpa_psk_stats {
    unsigned int rx_total;
    unsigned int bad_mic;
    unsigned int replay;
    unsigned int bad_version;
    unsigned int malformed;
    unsigned int unexpected;
    unsigned int bad_anonce;
    unsigned int rsn_missing;
    unsigned int rsn_mismatch;
    unsigned int bad_key_len;
    unsigned int gtk_missing;
};

struct wpa_psk_ops {
    // send one EAPOL-Key frame (EAPOL header included, no ethernet header).
    // return 0 on success, negative on a transmit error. (satoru)
    int (*tx_eapol)(void *priv, const u8 *frame, unsigned int len);
    // install a key. rsc is the replay counter as received; rsc_len is 0 for
    // the pairwise TK (802.11-2020 12.7.6.3 uses a zero RSC there). tx is the
    // GTK KDE tx bit (or 1 for the pairwise tk); the api has no other way to
    // carry it and a group-key-only ap needs it. (satoru)
    int (*set_key)(void *priv, enum wpa_psk_key_type type,
                   unsigned int key_idx, const u8 *key, unsigned int key_len,
                   const u8 *rsc, unsigned int rsc_len, u8 tx);
    // final result of this handshake. exactly one terminal status is reported
    // per attempt; a retransmitted frame does not repeat it. (satoru)
    void (*done)(void *priv, enum wpa_psk_status status);
    void *priv;
};

struct wpa_psk_cfg {
    const u8 *ssid;
    unsigned int ssid_len;
    const char *passphrase;
    unsigned int passphrase_len;
    u8 own_mac[6];
    u8 bssid[6];
    // the AP's RSN element exactly as received in the beacon/probe response:
    // element id 0x30, length, body. (satoru)
    const u8 *rsn_ie;
    unsigned int rsn_ie_len;
};

struct wpa_psk_ctx {
    struct wpa_psk_ops ops;
    // our RSN element for the association request, filled by init. (satoru)
    u8 assoc_ie[WPA_PSK_ASSOC_IE_MAX];
    unsigned int assoc_ie_len;
    // 1 when the AP requires management frame protection (MFPR), so the
    // caller can pass the nl80211 use-mfp attribute and we install the IGTK.
    // mfpc-only APs are not mfp: we neither advertise nor use it. (satoru)
    u8 mfp_required;
    // private state (keys live here; zeroed by deinit). (satoru)
    unsigned long long priv[WPA_PSK_PRIV_QWORDS];
};

// derive pmk, select cipher/akm, build assoc_ie. returns 0, -EINVAL for a bad
// config, or -EOPNOTSUPP for an AP we cannot do (SAE-only, TKIP/WEP, no CCMP).
// on -EOPNOTSUPP ops->done(UNSUPPORTED_AP_SECURITY) has been called. (satoru)
int wpa_psk_init(struct wpa_psk_ctx *ctx, const struct wpa_psk_cfg *cfg,
                 const struct wpa_psk_ops *ops);

// feed one received EAPOL-Key frame (ethertype 0x888E payload). any frame
// that fails a check is dropped silently: it increments a stats counter and
// the return is negative, but ops->done is NOT called for it. returns 0 for a
// handled frame (including a retransmission), -EBADMSG for a bad MIC,
// -EALREADY for a replay, -EINVAL for a malformed or unexpected frame, -EIO
// after a tx/set_key failure (that one does call done(KEY_ERROR)). three
// verified messages commit state; a 1/4 never does. (satoru)
int wpa_psk_rx_eapol(struct wpa_psk_ctx *ctx, const u8 *frame,
                     unsigned int len);

// current handshake state and a copy of the drop counters. (satoru)
enum wpa_psk_state wpa_psk_state(const struct wpa_psk_ctx *ctx);
void wpa_psk_get_stats(const struct wpa_psk_ctx *ctx,
                       struct wpa_psk_stats *out);

// the caller's connect timer calls this when no 3/4 arrived after a 2/4. it
// reports TIMEOUT_HINT once and returns 0; -EALREADY if there is nothing to
// time out. this is the path that surfaces a wrong passphrase, because the
// authenticator silently drops a 2/4 with a bad MIC. (satoru)
int wpa_psk_timeout(struct wpa_psk_ctx *ctx);

// wipe every secret. call before freeing ctx. (satoru)
void wpa_psk_deinit(struct wpa_psk_ctx *ctx);

// stable text for logs, never a secret. (satoru)
const char *wpa_psk_status_name(enum wpa_psk_status status);

#endif
// end (satoru)
