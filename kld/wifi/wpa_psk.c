// wpa_psk: WPA2-PSK 4-way and group key handshake, pure logic.
//
// every rule below cites IEEE 802.11-2020 clause 12.7. the crypto primitives
// are behind wpa_psk_crypto.h; this file knows nothing about the radio or the
// OS. nothing here prints, and every secret buffer is wiped by deinit. (satoru)

#include <linux/types.h>
#include <linux/string.h>
#include <linux/errno.h>

#include "wpa_psk.h"
#include "wpa_psk_internal.h"
#include "wpa_psk_crypto.h"

#define EAPOL_VERSION        2
#define EAPOL_TYPE_KEY       3
#define EAPOL_HDR_LEN        4
#define EAPOL_KEY_FIXED_LEN  95
#define EAPOL_KEY_MIN_LEN    (EAPOL_HDR_LEN + EAPOL_KEY_FIXED_LEN)
#define EAPOL_MIC_OFF        (EAPOL_HDR_LEN + 77)
#define EAPOL_KEY_DATA_LEN   (EAPOL_HDR_LEN + 93)
#define EAPOL_KEY_DATA_OFF   (EAPOL_HDR_LEN + EAPOL_KEY_FIXED_LEN)

#define RSN_EID              0x30
#define RSN_VERSION_1        1
#define RSN_DESC_TYPE        2

#define RSN_AKM_PSK          2
#define RSN_AKM_PSK_SHA256   6
#define RSN_AKM_SAE          8
#define RSN_CIPHER_TKIP      2
#define RSN_CIPHER_CCMP      4
#define RSN_CIPHER_WEP40     1
#define RSN_CIPHER_WEP104    5

#define RSN_CAP_MFPR         0x0040
#define RSN_CAP_MFPC         0x0080

#define KEY_INFO_VERSION     0x0007
#define KEY_INFO_PAIRWISE    0x0008
#define KEY_INFO_KEY_INDEX   0x0030
#define KEY_INFO_INSTALL     0x0040
#define KEY_INFO_ACK         0x0080
#define KEY_INFO_MIC         0x0100
#define KEY_INFO_SECURE      0x0200
#define KEY_INFO_REQUEST     0x0800
#define KEY_INFO_ENC_KEY     0x1000

#define EAPOL_MAX 512
#define KEY_DATA_MAX 384
#define RSN_MAX 256

enum wpa_state {
    ST_IDLE = 0,
    ST_WAIT_1OF4,
    ST_WAIT_3OF4,
    ST_COMPLETED,
};

struct wpa_psk_priv {
    u8 own_mac[6];
    u8 bssid[6];
    u8 pmk[32];
    // a 1/4 must never touch the committed ptk; it derives a temporary one
    // (tptk) that only becomes ptk after a 3/4 verifies with its kck.
    u8 ptk[WPA_PSK_PTK_LEN];
    // two candidate slots: a forged 1/4 can push a bogus candidate on top,
    // but the 3/4 the ap accepted was built from the older one. (satoru)
    u8 tptk[WPA_PSK_PTK_LEN];
    u8 tptk2[WPA_PSK_PTK_LEN];
    u8 ptk_anonce[32];
    u8 tptk_anonce[32];
    u8 tptk2_anonce[32];
    u8 tptk_snonce[32];
    u8 tptk2_snonce[32];
    u8 assoc_ie[WPA_PSK_ASSOC_IE_MAX];
    unsigned int assoc_ie_len;
    u8 ap_rsn[RSN_MAX];
    unsigned int ap_rsn_len;
    u32 akm;
    u32 descriptor_version;
    u32 state;
    u32 ptk_installed;
    u32 tptk_valid;
    u32 tptk2_valid;
    // last counter of a frame whose mic verified, never a 1/4 counter.
    u32 have_verified_replay;
    u8 last_replay[8];
    // the keys currently installed, so a retransmitted 3/4 cannot reset the
    // receive counter of an identical gtk or igtk. (satoru)
    u8 last_gtk[16];
    u32 last_gtk_valid;
    u32 last_gtk_id;
    u8 last_igtk[16];
    u32 last_igtk_valid;
    u32 last_igtk_id;
    struct wpa_psk_stats stats;
    u8 tmp[EAPOL_MAX];
    u8 key_data[KEY_DATA_MAX];
};

_Static_assert(sizeof(struct wpa_psk_priv) <= sizeof(((struct wpa_psk_ctx *)0)->priv),
               "wpa_psk_priv does not fit in wpa_psk_ctx");

static struct wpa_psk_priv *priv_of(struct wpa_psk_ctx *ctx)
{
    return (struct wpa_psk_priv *)ctx->priv;
}

static u16 get_le16(const u8 *p)
{
    return (u16)(p[0] | ((u16)p[1] << 8));
}

static u16 get_be16(const u8 *p)
{
    return (u16)(((u16)p[0] << 8) | p[1]);
}

static void put_le16(u8 *p, u16 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void put_be16(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

// constant time equality for mics and rsn elements. (satoru)
static int ct_equal(const u8 *a, const u8 *b, unsigned int n)
{
    u8 d = 0;
    unsigned int i;
    for (i = 0; i < n; i++)
        d |= (u8)(a[i] ^ b[i]);
    return d == 0;
}

// 802.11-2020 12.7.1.2: a 64 character hex key is the already derived pmk.
static int hex_pmk(const char *pass, unsigned int len, u8 pmk[32])
{
    unsigned int i;
    if (len != 64)
        return -1;
    for (i = 0; i < 64; i++) {
        int c = pass[i], v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return -1;
        if ((i & 1) == 0)
            pmk[i >> 1] = (u8)(v << 4);
        else
            pmk[i >> 1] |= (u8)v;
    }
    return 0;
}

int wpa_psk_pmk(const char *pass, unsigned int pass_len,
                const u8 *ssid, unsigned int ssid_len, u8 pmk[32])
{
    if (!pass || !ssid || !pmk || ssid_len == 0 || ssid_len > 32)
        return -EINVAL;
    if (hex_pmk(pass, pass_len, pmk) == 0)
        return 0;
    if (pass_len < 8 || pass_len > 63)
        return -EINVAL;
    // 802.11-2020 12.7.1.2: pmk = pbkdf2-hmac-sha1(passphrase, ssid, 4096, 32).
    return wpa_crypto_pbkdf2_sha1((const u8 *)pass, pass_len, ssid, ssid_len,
                                  4096, pmk, 32);
}

// 802.11-2020 12.7.1.3 prf-sha1: label includes its nul, then data, then a one
// octet counter starting at zero.
static int prf_sha1(const u8 *key, unsigned int key_len, const char *label,
                    const u8 *data, unsigned int data_len, u8 *out,
                    unsigned int out_len)
{
    u8 msg[256];
    u8 hash[20];
    unsigned int label_len = (unsigned int)strlen(label) + 1;
    unsigned int pos = 0;
    u8 counter = 0;

    if (label_len + data_len + 1 > sizeof(msg))
        return -EINVAL;
    while (pos < out_len) {
        unsigned int n = out_len - pos;
        unsigned int off = 0;
        memcpy(msg + off, label, label_len);
        off += label_len;
        if (data_len) {
            memcpy(msg + off, data, data_len);
            off += data_len;
        }
        msg[off++] = counter;
        wpa_crypto_hmac_sha1(key, key_len, msg, off, hash);
        if (n > sizeof(hash))
            n = sizeof(hash);
        memcpy(out + pos, hash, n);
        pos += n;
        counter++;
    }
    memzero_explicit(hash, sizeof(hash));
    memzero_explicit(msg, sizeof(msg));
    return 0;
}

// 802.11-2020 12.7.1.3 kdf-sha256: two octet little endian counter from one,
// label without a nul, context, two octet little endian length in bits.
static int kdf_sha256(const u8 *key, unsigned int key_len, const char *label,
                      const u8 *data, unsigned int data_len, u8 *out,
                      unsigned int out_len)
{
    u8 msg[256];
    u8 hash[32];
    unsigned int label_len = (unsigned int)strlen(label);
    unsigned int pos = 0;
    u16 counter = 1;

    if (label_len + data_len + 6 > sizeof(msg))
        return -EINVAL;
    while (pos < out_len) {
        unsigned int n = out_len - pos;
        unsigned int off = 0;
        put_le16(msg + off, counter);
        off += 2;
        memcpy(msg + off, label, label_len);
        off += label_len;
        if (data_len) {
            memcpy(msg + off, data, data_len);
            off += data_len;
        }
        put_le16(msg + off, (u16)(out_len * 8));
        off += 2;
        wpa_crypto_hmac_sha256(key, key_len, msg, off, hash);
        if (n > sizeof(hash))
            n = sizeof(hash);
        memcpy(out + pos, hash, n);
        pos += n;
        counter++;
    }
    memzero_explicit(hash, sizeof(hash));
    memzero_explicit(msg, sizeof(msg));
    return 0;
}

int wpa_psk_derive_ptk(const u8 pmk[32], const u8 aa[6], const u8 spa[6],
                       const u8 anonce[32], const u8 snonce[32],
                       unsigned int akm, u8 ptk[WPA_PSK_PTK_LEN])
{
    u8 data[76];
    const u8 *first_mac, *second_mac, *first_nonce, *second_nonce;

    // 802.11-2020 12.7.1.3: Min(AA,SPA) || Max(AA,SPA) ||
    // Min(ANonce,SNonce) || Max(ANonce,SNonce).
    if (memcmp(aa, spa, 6) <= 0) {
        first_mac = aa;
        second_mac = spa;
    } else {
        first_mac = spa;
        second_mac = aa;
    }
    if (memcmp(anonce, snonce, 32) <= 0) {
        first_nonce = anonce;
        second_nonce = snonce;
    } else {
        first_nonce = snonce;
        second_nonce = anonce;
    }
    memcpy(data, first_mac, 6);
    memcpy(data + 6, second_mac, 6);
    memcpy(data + 12, first_nonce, 32);
    memcpy(data + 44, second_nonce, 32);
    if (akm == RSN_AKM_PSK)
        return prf_sha1(pmk, 32, "Pairwise key expansion", data, 76, ptk,
                        WPA_PSK_PTK_LEN);
    if (akm == RSN_AKM_PSK_SHA256)
        return kdf_sha256(pmk, 32, "Pairwise key expansion", data, 76, ptk,
                          WPA_PSK_PTK_LEN);
    return -EINVAL;
}

int wpa_psk_calc_mic(const u8 kck[16], const u8 *frame, unsigned int len,
                     unsigned int descriptor_version, u8 mic[16])
{
    u8 hash[20];
    int err;

    // 802.11-2020 12.7.3: version 2 is HMAC-SHA1-128, version 3 is
    // AES-128-CMAC over the whole EAPOL-Key frame with the MIC zeroed.
    if (descriptor_version == 2) {
        err = wpa_crypto_hmac_sha1(kck, 16, frame, len, hash);
        if (err)
            return err;
        memcpy(mic, hash, 16);
        memzero_explicit(hash, sizeof(hash));
        return 0;
    }
    if (descriptor_version == 3)
        return wpa_crypto_cmac_aes(kck, 16, frame, len, mic);
    return -EINVAL;
}

static int rsn_get_suite(const u8 *p, u32 *suite)
{
    if (p[0] != 0x00 || p[1] != 0x0f || p[2] != 0xac)
        return -1;
    *suite = p[3];
    return 0;
}

struct rsn_info {
    u32 group;
    u32 pairwise[8];
    unsigned int pairwise_count;
    u32 akm[8];
    unsigned int akm_count;
    u16 caps;
};

// 802.11-2020 12.7.2 rsn element: version, group suite, pairwise list, akm
// list, then the optional rsn capabilities. (satoru)
static int rsn_parse(const u8 *ie, unsigned int len, struct rsn_info *ri)
{
    const u8 *p, *end;
    unsigned int i;

    memset(ri, 0, sizeof(*ri));
    if (!ie || len < 2 || ie[0] != RSN_EID)
        return -1;
    if ((unsigned int)ie[1] + 2 > len)
        return -1;
    p = ie + 2;
    end = p + ie[1];
    if (end - p < 2 || get_le16(p) != RSN_VERSION_1)
        return -1;
    p += 2;
    if (end - p < 4 || rsn_get_suite(p, &ri->group))
        return -1;
    p += 4;
    if (end - p < 2)
        return -1;
    ri->pairwise_count = get_le16(p);
    p += 2;
    if (ri->pairwise_count > 8 || (unsigned int)(end - p) < 4 * ri->pairwise_count)
        return -1;
    for (i = 0; i < ri->pairwise_count; i++, p += 4)
        if (rsn_get_suite(p, &ri->pairwise[i]))
            return -1;
    if (end - p < 2)
        return -1;
    ri->akm_count = get_le16(p);
    p += 2;
    if (ri->akm_count > 8 || (unsigned int)(end - p) < 4 * ri->akm_count)
        return -1;
    for (i = 0; i < ri->akm_count; i++, p += 4)
        if (rsn_get_suite(p, &ri->akm[i]))
            return -1;
    if (end - p >= 2)
        ri->caps = get_le16(p);
    return 0;
}

static int has_cipher(const struct rsn_info *ri, u32 cipher)
{
    unsigned int i;
    for (i = 0; i < ri->pairwise_count; i++)
        if (ri->pairwise[i] == cipher)
            return 1;
    return 0;
}

static int has_akm(const struct rsn_info *ri, u32 akm)
{
    unsigned int i;
    for (i = 0; i < ri->akm_count; i++)
        if (ri->akm[i] == akm)
            return 1;
    return 0;
}

static void report(struct wpa_psk_ctx *ctx, enum wpa_psk_status status)
{
    if (ctx->ops.done)
        ctx->ops.done(ctx->ops.priv, status);
}

static int build_assoc_ie(struct wpa_psk_ctx *ctx, u32 akm, u16 caps)
{
    u8 *p = ctx->assoc_ie;

    p[0] = RSN_EID;
    p[1] = 20;
    put_le16(p + 2, RSN_VERSION_1);
    p[4] = 0x00; p[5] = 0x0f; p[6] = 0xac; p[7] = RSN_CIPHER_CCMP;
    put_le16(p + 8, 1);
    p[10] = 0x00; p[11] = 0x0f; p[12] = 0xac; p[13] = RSN_CIPHER_CCMP;
    put_le16(p + 14, 1);
    p[16] = 0x00; p[17] = 0x0f; p[18] = 0xac; p[19] = (u8)akm;
    put_le16(p + 20, caps);
    ctx->assoc_ie_len = 22;
    return 0;
}

static int replay_cmp(const u8 *a, const u8 *b)
{
    return memcmp(a, b, 8);
}

static void save_verified_replay(struct wpa_psk_priv *p, const u8 *frame)
{
    memcpy(p->last_replay, frame + 9, 8);
    p->have_verified_replay = 1;
}

static int build_eapol(struct wpa_psk_priv *p, const u8 *kck, u8 *out,
                       unsigned int out_max, u16 key_info, const u8 *replay,
                       const u8 *nonce, const u8 *key_data,
                       unsigned int key_data_len, unsigned int *out_len)
{
    unsigned int total = EAPOL_KEY_DATA_OFF + key_data_len;
    u8 mic[16];

    if (total > out_max || key_data_len > 0xffff)
        return -EINVAL;
    memset(out, 0, total);
    out[0] = EAPOL_VERSION;
    out[1] = EAPOL_TYPE_KEY;
    put_be16(out + 2, (u16)(total - EAPOL_HDR_LEN));
    out[4] = RSN_DESC_TYPE;
    put_be16(out + 5, key_info);
    put_be16(out + 7, 0);
    memcpy(out + 9, replay, 8);
    if (nonce)
        memcpy(out + 17, nonce, 32);
    put_be16(out + EAPOL_KEY_DATA_LEN, (u16)key_data_len);
    if (key_data_len)
        memcpy(out + EAPOL_KEY_DATA_OFF, key_data, key_data_len);
    if (wpa_psk_calc_mic(kck, out, total, p->descriptor_version, mic))
        return -EINVAL;
    memcpy(out + EAPOL_MIC_OFF, mic, 16);
    *out_len = total;
    return 0;
}

static int verify_mic(struct wpa_psk_priv *p, const u8 *kck, const u8 *frame,
                      unsigned int len)
{
    u8 got[16], mic[16];

    if (len > sizeof(p->tmp))
        return -EINVAL;
    memcpy(p->tmp, frame, len);
    memcpy(got, p->tmp + EAPOL_MIC_OFF, 16);
    memset(p->tmp + EAPOL_MIC_OFF, 0, 16);
    if (wpa_psk_calc_mic(kck, p->tmp, len, p->descriptor_version, mic))
        return -EINVAL;
    return ct_equal(got, mic, 16) ? 0 : -EBADMSG;
}

// kde parse for rsn key data: gtk (00-0f-ac:1), igtk (00-0f-ac:9) and the rsn
// element the authenticator repeats in message 3/4. (satoru)
struct igtk_data {
    u16 key_id;
    u8 ipn[6];
    u8 igtk[16];
};

static int parse_key_data(struct wpa_psk_priv *p, const u8 *kd,
                          unsigned int kd_len, u8 *gtk,
                          unsigned int *gtk_len, unsigned int *gtk_id,
                          u8 *gtk_tx, struct igtk_data *igtk,
                          int *have_igtk, const u8 **rsn,
                          unsigned int *rsn_len)
{
    unsigned int i = 0;

    *gtk_len = 0;
    *gtk_id = 0;
    *gtk_tx = 0;
    *have_igtk = 0;
    *rsn = NULL;
    *rsn_len = 0;

    while (i < kd_len) {
        unsigned int elen;
        if (kd[i] == 0) {
            while (i < kd_len && kd[i] == 0)
                i++;
            break;
        }
        if (i + 2 > kd_len)
            return -EINVAL;
        elen = kd[i + 1];
        if (i + 2 + elen > kd_len)
            return -EINVAL;
        if (kd[i] == RSN_EID) {
            *rsn = kd + i;
            *rsn_len = 2 + elen;
            i += 2 + elen;
            continue;
        }
        if (kd[i] == 0xdd && elen >= 4 && kd[i + 2] == 0x00 &&
            kd[i + 3] == 0x0f && kd[i + 4] == 0xac) {
            const u8 *d = kd + i + 6;
            unsigned int dlen = elen - 4;
            u8 type = kd[i + 5];
            if (type == 1 && dlen >= 2) {
                // 802.11-2020 12.7.2 gtk kde: key id bits 0-1, tx bit 2,
                // one reserved octet, then the gtk.
                *gtk_id = d[0] & 0x3;
                *gtk_tx = (d[0] >> 2) & 0x1;
                *gtk_len = dlen - 2;
                if (*gtk_len > 32)
                    return -EINVAL;
                memcpy(gtk, d + 2, *gtk_len);
            } else if (type == 9 && dlen >= 24) {
                // igtk kde: 2 octet key id (le), 6 octet ipn, 16 octet igtk.
                igtk->key_id = get_le16(d);
                memcpy(igtk->ipn, d + 2, 6);
                memcpy(igtk->igtk, d + 8, 16);
                *have_igtk = 1;
            }
            i += 2 + elen;
            continue;
        }
        i += 2 + elen;
    }
    return 0;
}

static int tx_frame(struct wpa_psk_ctx *ctx, const u8 *frame, unsigned int len)
{
    int err;

    if (!ctx->ops.tx_eapol) {
        report(ctx, WPA_PSK_STATUS_KEY_ERROR);
        return -EIO;
    }
    err = ctx->ops.tx_eapol(ctx->ops.priv, frame, len);
    if (err) {
        report(ctx, WPA_PSK_STATUS_KEY_ERROR);
        return -EIO;
    }
    return 0;
}

static int install_key(struct wpa_psk_ctx *ctx, enum wpa_psk_key_type type,
                       unsigned int idx, const u8 *key, unsigned int key_len,
                       const u8 *rsc, unsigned int rsc_len, u8 tx)
{
    int err;

    if (!ctx->ops.set_key) {
        report(ctx, WPA_PSK_STATUS_KEY_ERROR);
        return -EIO;
    }
    err = ctx->ops.set_key(ctx->ops.priv, type, idx, key, key_len,
                           rsc, rsc_len, tx);
    if (err) {
        report(ctx, WPA_PSK_STATUS_KEY_ERROR);
        return -EIO;
    }
    return 0;
}

// 802.11-2020 12.7.6.1: a 1/4 has no mic, so it is only accepted when its
// counter is greater than the last VERIFIED one. it derives a temporary ptk
// and never touches the committed one; a forged 1/4 can therefore neither
// rekey a live link nor pin the replay counter. (satoru)
static int handle_1of4(struct wpa_psk_ctx *ctx, const u8 *frame,
                       unsigned int len, u16 key_info, u16 kd_len)
{
    struct wpa_psk_priv *p = priv_of(ctx);
    unsigned int out_len;
    u8 out[EAPOL_MAX];
    int err;

    (void)len;
    (void)kd_len;
    if ((key_info & KEY_INFO_VERSION) != p->descriptor_version) {
        p->stats.bad_version++;
        return -EINVAL;
    }
    if (p->have_verified_replay &&
        replay_cmp(frame + 9, p->last_replay) <= 0) {
        p->stats.replay++;
        return -EALREADY;
    }
    // keep the previous candidate: an injected 1/4 must not hide the one the
    // ap is actually answering. (satoru)
    if (p->tptk_valid) {
        memcpy(p->tptk2, p->tptk, sizeof(p->tptk2));
        memcpy(p->tptk2_anonce, p->tptk_anonce, 32);
        memcpy(p->tptk2_snonce, p->tptk_snonce, 32);
        p->tptk2_valid = 1;
    }
    memcpy(p->tptk_anonce, frame + 17, 32);
    wpa_crypto_random_bytes(p->tptk_snonce, 32);
    if (wpa_psk_derive_ptk(p->pmk, p->bssid, p->own_mac, p->tptk_anonce,
                           p->tptk_snonce, p->akm, p->tptk)) {
        p->stats.malformed++;
        return -EINVAL;
    }
    p->tptk_valid = 1;
    // a 1/4 on a live link only prepares a rekey candidate; the caller still
    // sees a completed link and its timer must not tear it down. (satoru)
    if (!p->ptk_installed)
        p->state = ST_WAIT_3OF4;
    err = build_eapol(p, p->tptk, out, sizeof(out),
                      (u16)(p->descriptor_version | KEY_INFO_PAIRWISE |
                            KEY_INFO_MIC),
                      frame + 9, p->tptk_snonce, p->assoc_ie,
                      p->assoc_ie_len, &out_len);
    if (err)
        return err;
    return tx_frame(ctx, out, out_len);
}

// 802.11-2020 12.7.6.3. the 3/4 is the first pairwise message with a mic. it
// is verified against the tptk from the matching 1/4 or, for a retransmitted
// 3/4 after a lost 4/4, against the committed ptk with the same anonce.
// only after that does anything commit. (satoru)
static int handle_3of4(struct wpa_psk_ctx *ctx, const u8 *frame,
                       unsigned int len, u16 key_info)
{
    struct wpa_psk_priv *p = priv_of(ctx);
    u8 gtk[32], *kd, *kck;
    unsigned int gtk_len = 0, gtk_id = 0, kd_len, plain_len, out_len;
    u8 gtk_tx = 0, out[EAPOL_MAX];
    struct igtk_data igtk;
    int have_igtk = 0, is_new, err, i;
    const u8 *rsn = NULL;
    unsigned int rsn_len = 0;

    if ((key_info & KEY_INFO_VERSION) != p->descriptor_version) {
        p->stats.bad_version++;
        return -EINVAL;
    }
    is_new = 0;
    if (p->tptk_valid && !memcmp(frame + 17, p->tptk_anonce, 32)) {
        kck = p->tptk;
        is_new = 1;
    } else if (p->tptk2_valid && !memcmp(frame + 17, p->tptk2_anonce, 32)) {
        kck = p->tptk2;
        is_new = 2;
    } else if (p->ptk_installed && !memcmp(frame + 17, p->ptk_anonce, 32)) {
        kck = p->ptk;
        is_new = 0;
    } else {
        p->stats.bad_anonce++;
        return -EINVAL;
    }
    if (p->have_verified_replay &&
        replay_cmp(frame + 9, p->last_replay) <= 0) {
        p->stats.replay++;
        return -EALREADY;
    }
    if (verify_mic(p, kck, frame, len) != 0) {
        p->stats.bad_mic++;
        return -EBADMSG;
    }
    // 802.11-2020 12.7.6.3: only a verified mic commits the replay counter.
    save_verified_replay(p, frame);

    kd_len = get_be16(frame + EAPOL_KEY_DATA_LEN);
    if (kd_len > len - EAPOL_KEY_DATA_OFF)
        return -EINVAL;
    kd = p->key_data;
    if (key_info & KEY_INFO_ENC_KEY) {
        if (kd_len < 24 || (kd_len & 7)) {
            p->stats.malformed++;
            return -EINVAL;
        }
        plain_len = (unsigned int)wpa_crypto_aes_unwrap(
            kck + WPA_PSK_KCK_LEN, frame + EAPOL_KEY_DATA_OFF, kd_len, kd);
        if ((int)plain_len < 0) {
            p->stats.malformed++;
            return -EINVAL;
        }
    } else {
        // rsne and kde key data in 3/4 is encrypted; an unencrypted
        // non-empty field is a downgrade. (satoru)
        if (kd_len) {
            p->stats.malformed++;
            return -EINVAL;
        }
        plain_len = 0;
    }
    if (parse_key_data(p, kd, plain_len, gtk, &gtk_len, &gtk_id, &gtk_tx,
                       &igtk, &have_igtk, &rsn, &rsn_len)) {
        p->stats.malformed++;
        return -EINVAL;
    }
    // 802.11-2020 12.7.6.3: the rsne in 3/4 must be present and equal to the
    // beacon's. (satoru)
    if (!rsn) {
        p->stats.rsn_missing++;
        return -EINVAL;
    }
    if (rsn_len != p->ap_rsn_len || !ct_equal(rsn, p->ap_rsn, rsn_len)) {
        p->stats.rsn_mismatch++;
        return -EINVAL;
    }
    // a fresh 3/4 must install the group key; a retransmitted 3/4 after a lost
    // 4/4 may omit it because the key is already installed. (satoru)
    if (is_new && !gtk_len) {
        p->stats.gtk_missing++;
        return -EINVAL;
    }
    if (gtk_len && gtk_len != 16) {
        p->stats.bad_key_len++;
        return -EINVAL;
    }
    // 4/4 carries the same counter, secure set and no key data. build it
    // before committing the new tk so a tx failure leaves the old one live.
    err = build_eapol(p, kck, out, sizeof(out),
                      (u16)(p->descriptor_version | KEY_INFO_PAIRWISE |
                            KEY_INFO_MIC | KEY_INFO_SECURE),
                      frame + 9, NULL, NULL, 0, &out_len);
    if (err)
        return err;
    if (tx_frame(ctx, out, out_len))
        return -EIO;
    if (is_new) {
        if (is_new == 2) {
            memcpy(p->ptk, p->tptk2, sizeof(p->ptk));
            memcpy(p->ptk_anonce, p->tptk2_anonce, 32);
        } else {
            memcpy(p->ptk, p->tptk, sizeof(p->ptk));
            memcpy(p->ptk_anonce, p->tptk_anonce, 32);
        }
        p->tptk_valid = 0;
        p->tptk2_valid = 0;
        memzero_explicit(p->tptk, sizeof(p->tptk));
        memzero_explicit(p->tptk2, sizeof(p->tptk2));
        if (install_key(ctx, WPA_PSK_KEY_PAIRWISE, 0,
                        p->ptk + WPA_PSK_KCK_LEN + WPA_PSK_KEK_LEN,
                        WPA_PSK_TK_LEN, NULL, 0, 1))
            return -EIO;
        p->ptk_installed = 1;
    }
    if (gtk_len) {
        // 802.11-2020 12.7.6.3: group keys only change when the key for that
        // id actually differs, so a retransmission cannot reset its rsc. the
        // rsc sent to the driver is the first 6 octets of the key rsc field.
        if (!(p->last_gtk_valid && p->last_gtk_id == gtk_id &&
              ct_equal(p->last_gtk, gtk, 16))) {
            if (install_key(ctx, WPA_PSK_KEY_GROUP, gtk_id, gtk, 16,
                            frame + 65, 6, gtk_tx))
                return -EIO;
            memcpy(p->last_gtk, gtk, 16);
            p->last_gtk_valid = 1;
            p->last_gtk_id = gtk_id;
        }
    }
    if (have_igtk && ctx->mfp_required) {
        if (!(p->last_igtk_valid && p->last_igtk_id == igtk.key_id &&
              ct_equal(p->last_igtk, igtk.igtk, 16))) {
            if (install_key(ctx, WPA_PSK_KEY_IGTK, igtk.key_id, igtk.igtk,
                            16, igtk.ipn, 6, 0))
                return -EIO;
            memcpy(p->last_igtk, igtk.igtk, 16);
            p->last_igtk_valid = 1;
            p->last_igtk_id = igtk.key_id;
        }
    }
    p->state = ST_COMPLETED;
    if (is_new)
        report(ctx, WPA_PSK_STATUS_OK);
    for (i = 0; i < 32; i++)
        gtk[i] = 0;
    memzero_explicit(&igtk, sizeof(igtk));
    return 0;
}

// 802.11-2020 12.7.7: group key handshake. it only runs on a completed link
// and its counter must be strictly greater than the last verified one. a
// repeated identical gtk does not get installed again. (satoru)
static int handle_group_1of2(struct wpa_psk_ctx *ctx, const u8 *frame,
                             unsigned int len, u16 key_info)
{
    struct wpa_psk_priv *p = priv_of(ctx);
    u8 gtk[32], out[EAPOL_MAX];
    unsigned int gtk_len = 0, gtk_id = 0, kd_len, plain_len, out_len;
    u8 gtk_tx = 0;
    struct igtk_data igtk;
    int have_igtk = 0, err, i;
    const u8 *rsn = NULL;
    unsigned int rsn_len = 0;

    // a group rekey is allowed while a new pairwise 4-way is in progress:
    // the completed tk is still installed and msg_3_of_4_ok stays true.
    if (!p->ptk_installed) {
        p->stats.unexpected++;
        return -EINVAL;
    }
    if ((key_info & KEY_INFO_VERSION) != p->descriptor_version) {
        p->stats.bad_version++;
        return -EINVAL;
    }
    if (p->have_verified_replay &&
        replay_cmp(frame + 9, p->last_replay) <= 0) {
        p->stats.replay++;
        return -EALREADY;
    }
    if (!(key_info & KEY_INFO_ENC_KEY) || !(key_info & KEY_INFO_SECURE)) {
        p->stats.malformed++;
        return -EINVAL;
    }
    if (verify_mic(p, p->ptk, frame, len) != 0) {
        p->stats.bad_mic++;
        return -EBADMSG;
    }
    save_verified_replay(p, frame);
    kd_len = get_be16(frame + EAPOL_KEY_DATA_LEN);
    if (kd_len < 24 || (kd_len & 7) || kd_len > len - EAPOL_KEY_DATA_OFF) {
        p->stats.malformed++;
        return -EINVAL;
    }
    plain_len = (unsigned int)wpa_crypto_aes_unwrap(
        p->ptk + WPA_PSK_KCK_LEN, frame + EAPOL_KEY_DATA_OFF, kd_len,
        p->key_data);
    if ((int)plain_len < 0) {
        p->stats.malformed++;
        return -EINVAL;
    }
    if (parse_key_data(p, p->key_data, plain_len, gtk, &gtk_len, &gtk_id,
                       &gtk_tx, &igtk, &have_igtk, &rsn, &rsn_len)) {
        p->stats.malformed++;
        return -EINVAL;
    }
    if (!gtk_len) {
        p->stats.malformed++;
        return -EINVAL;
    }
    if (gtk_len != 16) {
        p->stats.bad_key_len++;
        return -EINVAL;
    }
    // 2/2 keeps the key index bits of 1/2, sets secure and carries no data.
    err = build_eapol(p, p->ptk, out, sizeof(out),
                      (u16)((key_info & KEY_INFO_KEY_INDEX) |
                            p->descriptor_version | KEY_INFO_MIC |
                            KEY_INFO_SECURE),
                      frame + 9, NULL, NULL, 0, &out_len);
    if (err)
        return err;
    if (tx_frame(ctx, out, out_len))
        return -EIO;
    if (!(p->last_gtk_valid && p->last_gtk_id == gtk_id &&
          ct_equal(p->last_gtk, gtk, 16))) {
        if (install_key(ctx, WPA_PSK_KEY_GROUP, gtk_id, gtk, 16,
                        frame + 65, 6, gtk_tx))
            return -EIO;
        memcpy(p->last_gtk, gtk, 16);
        p->last_gtk_valid = 1;
        p->last_gtk_id = gtk_id;
    }
    if (have_igtk && ctx->mfp_required) {
        if (!(p->last_igtk_valid && p->last_igtk_id == igtk.key_id &&
              ct_equal(p->last_igtk, igtk.igtk, 16))) {
            if (install_key(ctx, WPA_PSK_KEY_IGTK, igtk.key_id, igtk.igtk,
                            16, igtk.ipn, 6, 0))
                return -EIO;
            memcpy(p->last_igtk, igtk.igtk, 16);
            p->last_igtk_valid = 1;
            p->last_igtk_id = igtk.key_id;
        }
    }
    report(ctx, WPA_PSK_STATUS_OK);
    for (i = 0; i < 32; i++)
        gtk[i] = 0;
    memzero_explicit(&igtk, sizeof(igtk));
    return 0;
}

int wpa_psk_init(struct wpa_psk_ctx *ctx, const struct wpa_psk_cfg *cfg,
                 const struct wpa_psk_ops *ops)
{
    struct wpa_psk_priv *p;
    struct rsn_info ri;
    u32 akm = 0;
    u16 caps;

    if (!ctx || !cfg || !ops || !cfg->ssid || !cfg->rsn_ie ||
        !cfg->passphrase || cfg->rsn_ie_len < 2)
        return -EINVAL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->ops = *ops;
    p = priv_of(ctx);
    memcpy(p->own_mac, cfg->own_mac, 6);
    memcpy(p->bssid, cfg->bssid, 6);
    if (cfg->rsn_ie_len > sizeof(p->ap_rsn))
        return -EINVAL;
    memcpy(p->ap_rsn, cfg->rsn_ie, cfg->rsn_ie_len);
    p->ap_rsn_len = cfg->rsn_ie_len;

    if (rsn_parse(cfg->rsn_ie, cfg->rsn_ie_len, &ri)) {
        report(ctx, WPA_PSK_STATUS_UNSUPPORTED_AP_SECURITY);
        return -EOPNOTSUPP;
    }
    // 802.11-2020 12.7.2: group cipher and pairwise cipher. tkips and wep are
    // outside this supplicant's scope, and a group cipher we cannot implement
    // would leave broadcast traffic unreadable. (satoru)
    if (ri.group != RSN_CIPHER_CCMP || !has_cipher(&ri, RSN_CIPHER_CCMP)) {
        report(ctx, WPA_PSK_STATUS_UNSUPPORTED_AP_SECURITY);
        return -EOPNOTSUPP;
    }
    // 802.11-2020 12.7.1.3: psk with sha1 (akm 2) is preferred; psk with
    // sha256 (akm 6) is chosen when the ap requires management frame
    // protection. sae-only (akm 8) is wpa3 and is not attempted. (satoru)
    ctx->mfp_required = (ri.caps & RSN_CAP_MFPR) ? 1 : 0;
    if (ctx->mfp_required) {
        if (has_akm(&ri, RSN_AKM_PSK_SHA256))
            akm = RSN_AKM_PSK_SHA256;
        else if (has_akm(&ri, RSN_AKM_PSK))
            akm = RSN_AKM_PSK;
    } else {
        if (has_akm(&ri, RSN_AKM_PSK))
            akm = RSN_AKM_PSK;
        else if (has_akm(&ri, RSN_AKM_PSK_SHA256))
            akm = RSN_AKM_PSK_SHA256;
    }
    if (!akm) {
        report(ctx, WPA_PSK_STATUS_UNSUPPORTED_AP_SECURITY);
        return -EOPNOTSUPP;
    }
    p->akm = akm;
    p->descriptor_version = (akm == RSN_AKM_PSK_SHA256) ? 3 : 2;
    // 802.11-2020 12.7.2: only an ap that requires mfp gets the mfpc/mfpr
    // bits; a merely capable ap gets neither and its igtk is ignored. (satoru)
    caps = ctx->mfp_required ? (RSN_CAP_MFPC | RSN_CAP_MFPR) : 0;
    build_assoc_ie(ctx, akm, caps);
    memcpy(p->assoc_ie, ctx->assoc_ie, ctx->assoc_ie_len);
    p->assoc_ie_len = ctx->assoc_ie_len;
    if (wpa_psk_pmk(cfg->passphrase, cfg->passphrase_len, cfg->ssid,
                    cfg->ssid_len, p->pmk))
        return -EINVAL;
    p->state = ST_WAIT_1OF4;
    return 0;
}

int wpa_psk_rx_eapol(struct wpa_psk_ctx *ctx, const u8 *frame,
                     unsigned int len)
{
    struct wpa_psk_priv *p;
    u16 key_info, eapol_len, kd_len;

    if (!ctx || !frame)
        return -EINVAL;
    p = priv_of(ctx);
    p->stats.rx_total++;
    if (len < EAPOL_KEY_MIN_LEN || frame[0] != EAPOL_VERSION ||
        frame[1] != EAPOL_TYPE_KEY) {
        p->stats.malformed++;
        return -EINVAL;
    }
    eapol_len = get_be16(frame + 2);
    if ((unsigned int)eapol_len + EAPOL_HDR_LEN > len) {
        p->stats.malformed++;
        return -EINVAL;
    }
    len = (unsigned int)eapol_len + EAPOL_HDR_LEN;
    if (frame[4] != RSN_DESC_TYPE) {
        p->stats.unexpected++;
        return -EINVAL;
    }
    key_info = get_be16(frame + 5);
    if (key_info & KEY_INFO_REQUEST) {
        p->stats.unexpected++;
        return -EINVAL;
    }
    kd_len = get_be16(frame + EAPOL_KEY_DATA_LEN);
    if (kd_len > len - EAPOL_KEY_DATA_OFF) {
        p->stats.malformed++;
        return -EINVAL;
    }

    if ((key_info & KEY_INFO_PAIRWISE) && (key_info & KEY_INFO_ACK) &&
        !(key_info & KEY_INFO_MIC))
        return handle_1of4(ctx, frame, len, key_info, kd_len);
    if ((key_info & KEY_INFO_PAIRWISE) && (key_info & KEY_INFO_ACK) &&
        (key_info & KEY_INFO_MIC) && (key_info & KEY_INFO_INSTALL))
        return handle_3of4(ctx, frame, len, key_info);
    if (!(key_info & KEY_INFO_PAIRWISE) && (key_info & KEY_INFO_ACK) &&
        (key_info & KEY_INFO_MIC) && (key_info & KEY_INFO_ENC_KEY))
        return handle_group_1of2(ctx, frame, len, key_info);
    p->stats.unexpected++;
    return -EINVAL;
}

int wpa_psk_timeout(struct wpa_psk_ctx *ctx)
{
    struct wpa_psk_priv *p;

    if (!ctx)
        return -EINVAL;
    p = priv_of(ctx);
    // a 1/4 on a live link only prepares a rekey candidate; the link is still
    // complete and the caller's timer must not tear it down. (satoru)
    if (p->ptk_installed || p->state != ST_WAIT_3OF4)
        return -EALREADY;
    report(ctx, WPA_PSK_STATUS_TIMEOUT_HINT);
    p->state = ST_IDLE;
    p->tptk_valid = 0;
    memzero_explicit(p->tptk, sizeof(p->tptk));
    return 0;
}

enum wpa_psk_state wpa_psk_state(const struct wpa_psk_ctx *ctx)
{
    const struct wpa_psk_priv *p;

    if (!ctx)
        return WPA_PSK_STATE_IDLE;
    p = (const struct wpa_psk_priv *)ctx->priv;
    if (p->state == ST_WAIT_3OF4)
        return WPA_PSK_STATE_WAIT_3OF4;
    if (p->state == ST_COMPLETED)
        return WPA_PSK_STATE_COMPLETED;
    return WPA_PSK_STATE_IDLE;
}

void wpa_psk_get_stats(const struct wpa_psk_ctx *ctx,
                       struct wpa_psk_stats *out)
{
    const struct wpa_psk_priv *p;

    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!ctx)
        return;
    p = (const struct wpa_psk_priv *)ctx->priv;
    *out = p->stats;
}

void wpa_psk_deinit(struct wpa_psk_ctx *ctx)
{
    if (!ctx)
        return;
    memzero_explicit(ctx->priv, sizeof(ctx->priv));
    memzero_explicit(ctx->assoc_ie, sizeof(ctx->assoc_ie));
    ctx->assoc_ie_len = 0;
    ctx->mfp_required = 0;
}

const char *wpa_psk_status_name(enum wpa_psk_status status)
{
    switch (status) {
    case WPA_PSK_STATUS_OK:
        return "ok";
    case WPA_PSK_STATUS_TIMEOUT_HINT:
        return "timeout hint";
    case WPA_PSK_STATUS_UNSUPPORTED_AP_SECURITY:
        return "unsupported ap security";
    case WPA_PSK_STATUS_KEY_ERROR:
        return "key install error";
    }
    return "unknown";
}

// end (satoru)
