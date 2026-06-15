#pragma once
//  kurono os  -  ieee 802.11 software mac (the mac80211/cfg80211-equivalent) (satoru)
//
//  this is the shared 802.11 stack that every per-vendor wifi radio driver plugs
//  into. it owns the protocol: scan/auth/assoc state machine, the management-
//  frame builders/parsers, information elements, and the wpa2/wpa2-psk
//  supplicant (4-way handshake + ccmp). it sits ON TOP of the hardware layer
//  (drivers/wifi_dev) and BELOW the net stack (net/network WiFi class). it does
//  NOT touch registers itself  -  all radio i/o goes through a WifiRadioOps vtable
//  that a vendor driver registers. (satoru)
//
//  layering:
//    net/network.cpp  WiFi::Scan/Connect   (user-facing)
//        │
//    net/ieee80211    Ieee80211::Scan/Connect  (this  -  protocol + supplicant)
//        │  WifiRadioOps (function-pointer vtable)
//    vendor driver    (phase 2: iwlwifi/ath9k/rtw  -  implements WifiRadioOps)
//        │  drivers/wifi_dev RegRead/RegWrite
//    hardware (pci mmio)
//
//  ref: ieee 802.11-2016 for frame formats + the state machine; linux mac80211 +
//  wpa_supplicant for the protocol sequence. code is original kurono. (satoru)

#include "../kernel/types.h"
#include "network.h"          // WiFiNetwork / WiFiSecurity / MACAddress (satoru)

struct WifiDevice;            // fwd (drivers/wifi_dev.h) (satoru)

namespace Ieee80211 {

// ── frame-control type/subtype (ieee 802.11-2016 §9.2.4.1) ───────────── (satoru)
#define IEEE80211_FTYPE_MGMT  0x0
#define IEEE80211_FTYPE_CTL   0x1
#define IEEE80211_FTYPE_DATA  0x2

// management subtypes (satoru)
#define IEEE80211_STYPE_ASSOC_REQ    0x0
#define IEEE80211_STYPE_ASSOC_RESP   0x1
#define IEEE80211_STYPE_REASSOC_REQ  0x2
#define IEEE80211_STYPE_REASSOC_RESP 0x3
#define IEEE80211_STYPE_PROBE_REQ    0x4
#define IEEE80211_STYPE_PROBE_RESP   0x5
#define IEEE80211_STYPE_BEACON       0x8
#define IEEE80211_STYPE_DISASSOC     0xA
#define IEEE80211_STYPE_AUTH         0xB
#define IEEE80211_STYPE_DEAUTH       0xC

// data subtype: qos-data carries the priority/tid used by the ccmp nonce (satoru)
#define IEEE80211_STYPE_DATA         0x0
#define IEEE80211_STYPE_QOS_DATA     0x8

// frame-control field bits (in the 16-bit fc, little-endian on the wire) (satoru)
#define IEEE80211_FC_TODS     0x0100
#define IEEE80211_FC_FROMDS   0x0200
#define IEEE80211_FC_PROTECTED 0x4000  // wep/ccmp protected-frame bit (satoru)

// information-element ids (ieee 802.11-2016 §9.4.2.1) (satoru)
#define IEEE80211_EID_SSID         0
#define IEEE80211_EID_SUPP_RATES   1
#define IEEE80211_EID_DS_PARAMS    3
#define IEEE80211_EID_RSN          48   // wpa2 rsn ie (satoru)
#define IEEE80211_EID_EXT_RATES    50
#define IEEE80211_EID_VENDOR       221  // wpa1 lives in a vendor-specific ie (satoru)

// the 24-byte 802.11 mac header (3-address management/data form). (satoru)
struct Mac80211Hdr {
    uint16_t frame_control;    // little-endian (satoru)
    uint16_t duration_id;
    uint8_t  addr1[6];         // ra / da (satoru)
    uint8_t  addr2[6];         // ta / sa (satoru)
    uint8_t  addr3[6];         // bssid (satoru)
    uint16_t seq_ctrl;         // (seq<<4)|frag (satoru)
} __attribute__((packed));

// ── eapol / 4-way-handshake constants (ieee 802.1x-2010 + 802.11i) ───── (satoru)
#define EAPOL_VERSION        2
#define EAPOL_TYPE_KEY       3      // eapol-key packet type (satoru)
#define EAPOL_KEY_TYPE_RSN   2      // descriptor type 2 = wpa2/rsn (satoru)
#define EAPOL_KEY_TYPE_WPA   254    // descriptor type 254 = wpa1 (satoru)

// key-information bit flags inside an eapol-key frame (satoru)
#define WPA_KEY_INFO_KEY_TYPE   0x0008  // 1=pairwise (satoru)
#define WPA_KEY_INFO_INSTALL    0x0040
#define WPA_KEY_INFO_ACK        0x0080
#define WPA_KEY_INFO_MIC        0x0100
#define WPA_KEY_INFO_SECURE     0x0200
#define WPA_KEY_INFO_ENCR_DATA  0x1000

// the eapol-key body (ieee 802.11-2016 §12.7.2). big-endian fields. the key-data
// field (variable length) follows immediately after. (satoru)
struct EapolKey {
    uint8_t  version;          // eapol protocol version (satoru)
    uint8_t  type;             // eapol packet type (3 = key) (satoru)
    uint16_t length;           // body length following these 4 bytes, big-endian (satoru)
    uint8_t  descriptor_type;  // 2=rsn, 254=wpa (satoru)
    uint16_t key_info;         // big-endian (satoru)
    uint16_t key_length;       // big-endian (satoru)
    uint8_t  replay_counter[8];
    uint8_t  key_nonce[32];
    uint8_t  key_iv[16];
    uint8_t  key_rsc[8];
    uint8_t  key_id[8];
    uint8_t  key_mic[16];
    uint16_t key_data_length;  // big-endian (satoru)
    // uint8_t key_data[key_data_length] follows (satoru)
} __attribute__((packed));

// ── the supplicant state machine ─────────────────────────────────────── (satoru)
enum SupplicantState {
    SUPP_IDLE = 0,
    SUPP_SCANNING,
    SUPP_AUTHENTICATING,   // open-system auth in flight (satoru)
    SUPP_ASSOCIATING,      // assoc-req sent, awaiting assoc-resp (satoru)
    SUPP_4WAY_MSG1,        // associated; awaiting eapol msg 1 (satoru)
    SUPP_4WAY_MSG3,        // sent msg 2; awaiting msg 3 (satoru)
    SUPP_HANDSHAKE_DONE,   // ptk+gtk installed; link is secure (satoru)
    SUPP_FAILED
};

// ── WifiRadioOps  -  THE driver contract ───────────────────────────────── (satoru)
//
//  a per-vendor radio driver fills this vtable and registers it via
//  Ieee80211::RegisterRadio(&ops, wifi_device). every function takes the same
//  opaque `ctx` the driver supplied at registration (its private state). the
//  802.11 stack NEVER touches the hardware directly  -  it only calls these. (satoru)
//
//  contract for each op (this is what phase-2 vendor drivers implement):
struct WifiRadioOps {
    // bring the radio out of reset and ready to tx/rx management frames. called
    // once at the start of a scan/connect. return true on success. for chips that
    // need firmware, load it inside start() (or via load_firmware first). (satoru)
    bool (*start)(void* ctx);

    // quiesce the radio (stop rx, idle the mac). called on disconnect / shutdown.
    void (*stop)(void* ctx);

    // tune the phy to 802.11 channel `ch` (1..14 for 2.4ghz, 36.. for 5ghz). the
    // stack calls this while sweeping channels during a scan and once more to
    // park on the target ap's channel before auth. must block until the synth is
    // settled (or the caller's dwell time will race the tune). (satoru)
    bool (*set_channel)(void* ctx, int ch);

    // program the bss the sta is joining: the ap bssid (6 bytes) and ssid
    // (NUL-terminated). drivers use this to set rx address filters so only frames
    // for this bss reach rx_poll. called right before auth. (satoru)
    bool (*config_bss)(void* ctx, const uint8_t bssid[6], const char* ssid);

    // install a key into the hardware crypto engine (or accept null to mean
    // "software crypto  -  the stack will ccmp in software"). idx is the key index
    // (0..3; pairwise keys conventionally use a separate slot), key is `key_len`
    // bytes, type is one of WIFI_KEY_* below. a driver that can't offload returns
    // false and the stack falls back to software ccmp. (satoru)
    bool (*set_key)(void* ctx, int idx, const uint8_t* key, int key_len, int type);

    // transmit one fully-formed 802.11 frame (mac header + body, no fcs  -  the
    // hardware appends the fcs). len bytes at buf. return true if queued. used for
    // probe-req, auth, assoc-req, eapol, and (post-handshake) data frames. (satoru)
    bool (*tx_frame)(void* ctx, const uint8_t* buf, int len);

    // poll the rx path: if a frame is available, copy it (mac header + body, fcs
    // stripped) into buf (capacity buf_max) and return its length; return 0 if
    // nothing is pending, <0 on error. the stack calls this in a loop during each
    // state with a real-ms deadline. a driver may instead push frames via
    // Ieee80211::DeliverRx() from its irq and leave rx_poll returning 0. (satoru)
    int  (*rx_poll)(void* ctx, uint8_t* buf, int buf_max);

    // current rssi of the associated link (or the last beacon), in dBm
    // (-100..0). used for the signal-bars indicator. (satoru)
    int  (*get_signal)(void* ctx);

    // load a firmware/ucode blob into the device (intel/ath10k/realtek/broadcom).
    // a NO-OP returning true for firmware-free chips (ath9k). the stack does not
    // ship firmware; the driver sources it. (satoru)
    bool (*load_firmware)(void* ctx, const uint8_t* blob, int len);
};

// key types passed to set_key (satoru)
#define WIFI_KEY_CCMP   1   // aes-128 ccmp pairwise/group (satoru)
#define WIFI_KEY_TKIP   2
#define WIFI_KEY_WEP40  3
#define WIFI_KEY_WEP104 4

// ── public api (driven by net/network.cpp WiFi) ──────────────────────── (satoru)

// register the per-vendor radio. `ctx` is handed back to every op. `dev` is the
// probed pci device (for the sta mac + diagnostics). only one radio at a time.
// after this, HasRadio()==true and Scan/Connect will drive real hardware. (satoru)
void RegisterRadio(const WifiRadioOps* ops, void* ctx, WifiDevice* dev);
bool HasRadio();
void UnregisterRadio();

// a driver may push a received frame here from its rx irq instead of (or in
// addition to) implementing rx_poll. the frame is mac-header + body, fcs
// stripped. thread-unsafe ring; intended for the cooperative scheduler. (satoru)
void DeliverRx(const uint8_t* frame, int len);

// active scan: sweep channels, send probe-reqs, collect beacons/probe-resps into
// `out` (capacity max). returns the number of networks found. if no radio is
// registered, returns 0 (honest  -  no fake results). (satoru)
int  Scan(WiFiNetwork* out, int max);

// full connect: auth (open) → assoc → (if secured) wpa2 4-way handshake → install
// keys. `ssid`/`pass` are NUL-terminated (pass may be "" for open networks).
// returns true only on a completed handshake (or completed assoc for open nets).
// fills bssid_out with the ap's bssid on success. (satoru)
bool Connect(const char* ssid, const char* pass, uint8_t bssid_out[6]);

// tear the link down (send deauth, stop the radio). (satoru)
void Disconnect();

SupplicantState State();
int  GetSignal();              // current link rssi in dBm, or -100 (satoru)
const char* StateString();

// encrypt/decrypt a data-frame payload with the negotiated ccmp keys (post-
// handshake). these are the hooks a data path would use; exposed for testing and
// for a future tx/rx data path. return false if no ptk is installed. (satoru)
bool EncryptData(const uint8_t* plain, int len, uint8_t* out, int* out_len);
bool DecryptData(const uint8_t* cipher, int len, uint8_t* out, int* out_len);

// ── frame builders / parsers (exposed for the self-test + vendor drivers) ─ (satoru)

// build an 802.11 mac header into buf (24 bytes). returns bytes written. (satoru)
int  BuildMacHeader(uint8_t* buf, uint8_t ftype, uint8_t stype, uint16_t fc_flags,
                    const uint8_t addr1[6], const uint8_t addr2[6],
                    const uint8_t addr3[6], uint16_t seq);

// build a probe-request body (ssid ie + supported-rates ie) after the header.
// returns the full frame length. (satoru)
int  BuildProbeRequest(uint8_t* buf, int buf_max,
                       const uint8_t sta[6], const char* ssid, uint16_t seq);

// build an open-system authentication frame (algo 0, seq 1). (satoru)
int  BuildAuthRequest(uint8_t* buf, int buf_max,
                      const uint8_t sta[6], const uint8_t bssid[6], uint16_t seq);

// build an association request (ssid + rates + rsn ie for wpa2). (satoru)
int  BuildAssocRequest(uint8_t* buf, int buf_max,
                       const uint8_t sta[6], const uint8_t bssid[6],
                       const char* ssid, bool wpa2, uint16_t seq);

// parse a beacon/probe-resp body into a WiFiNetwork. `frame` is the whole frame
// (header included), `len` its length. returns true if it was a beacon/probe-
// resp we could parse. (satoru)
bool ParseBeacon(const uint8_t* frame, int len, WiFiNetwork* out);

// find an information element of id `eid` in a body region [ies, ies+len).
// returns a pointer to the ie's value (and its length via *out_len), or null. (satoru)
const uint8_t* FindIE(const uint8_t* ies, int len, uint8_t eid, int* out_len);

} // namespace Ieee80211
// end (satoru)
