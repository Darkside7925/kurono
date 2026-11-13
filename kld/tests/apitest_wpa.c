// apitest_wpa: known-answer tests for the wpa2-psk supplicant plus a complete
// self-play handshake against a minimal authenticator that lives only here.
//
// the authenticator is written separately from the supplicant: it derives the
// pmk/ptk with its own pbkdf2/prf/kdf code, wraps the gtk with its own rfc 3394
// code, and checks the mics of the frames the supplicant sends. both sides use
// the kernel crypto API primitives, but no supplicant code is reused. (satoru)

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <crypto/hash.h>
#include <crypto/aes.h>

#include "kimpl/lx_kshim.h"
#include "wifi/wpa_psk.h"
#include "wifi/wpa_psk_internal.h"
#include "wifi/wpa_psk_crypto.h"
#include "wpa_vectors.h"

#define WP_MAX_FRAME 512
#define WP_MIC_OFF 81
#define WP_KD_LEN_OFF 97
#define WP_KD_OFF 99

static int wp_pass, wp_total;

static void wp_result(const char *name, int ok, const char *detail)
{
    lx_log("[apitest] ");
    lx_log(name);
    lx_log(ok ? ": PASS " : ": FAIL ");
    if (detail && detail[0])
        lx_log(detail);
    lx_log("\n");
    wp_total++;
    if (ok)
        wp_pass++;
}

// ── authenticator crypto, deliberately not the supplicant's code ───────────
static int a_hash(const char *alg, const u8 *key, unsigned int key_len,
                  const u8 *data, unsigned int data_len, u8 *out)
{
    struct crypto_shash *tfm = crypto_alloc_shash(alg, 0, 0);
    SHASH_DESC_ON_STACK(desc, tfm);
    int err;

    if (IS_ERR(tfm))
        return PTR_ERR(tfm);
    err = crypto_shash_setkey(tfm, key, key_len);
    if (!err) {
        desc->tfm = tfm;
        err = crypto_shash_digest(desc, data, data_len, out);
    }
    crypto_free_shash(tfm);
    return err;
}

static void a_hmac_sha1(const u8 *key, unsigned int key_len, const u8 *data,
                        unsigned int data_len, u8 out[20])
{
    (void)a_hash("hmac(sha1)", key, key_len, data, data_len, out);
}

static void a_hmac_sha256(const u8 *key, unsigned int key_len, const u8 *data,
                          unsigned int data_len, u8 out[32])
{
    (void)a_hash("hmac(sha256)", key, key_len, data, data_len, out);
}

static void a_cmac(const u8 *key, const u8 *data, unsigned int data_len,
                   u8 out[16])
{
    (void)a_hash("cmac(aes)", key, 16, data, data_len, out);
}

static void a_pbkdf2(const u8 *pass, unsigned int pass_len, const u8 *ssid,
                     unsigned int ssid_len, u8 out[32])
{
    struct crypto_shash *tfm = crypto_alloc_shash("hmac(sha1)", 0, 0);
    struct shash_desc *desc;
    u8 msg[64], u[20], t[20];
    unsigned int block, i, done = 0;

    if (IS_ERR(tfm))
        return;
    if (crypto_shash_setkey(tfm, pass, pass_len)) {
        crypto_free_shash(tfm);
        return;
    }
    desc = kzalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
    if (!desc) {
        crypto_free_shash(tfm);
        return;
    }
    desc->tfm = tfm;
    while (done < 32) {
        unsigned int n = 32 - done > 20 ? 20 : 32 - done;
        block = done / 20 + 1;
        memcpy(msg, ssid, ssid_len);
        msg[ssid_len] = (u8)(block >> 24);
        msg[ssid_len + 1] = (u8)(block >> 16);
        msg[ssid_len + 2] = (u8)(block >> 8);
        msg[ssid_len + 3] = (u8)block;
        crypto_shash_digest(desc, msg, ssid_len + 4, u);
        memcpy(t, u, 20);
        for (i = 1; i < 4096; i++) {
            crypto_shash_digest(desc, u, 20, u);
            for (unsigned int j = 0; j < 20; j++)
                t[j] ^= u[j];
        }
        memcpy(out + done, t, n);
        done += n;
    }
    kfree_sensitive(desc);
    crypto_free_shash(tfm);
}

static void a_prf_sha1(const u8 *key, const char *label, const u8 *data,
                       unsigned int data_len, u8 *out, unsigned int out_len)
{
    u8 msg[256], hash[20];
    unsigned int label_len = (unsigned int)strlen(label) + 1;
    unsigned int pos = 0;
    u8 counter = 0;

    while (pos < out_len) {
        unsigned int n = out_len - pos;
        unsigned int off = 0;
        memcpy(msg + off, label, label_len);
        off += label_len;
        memcpy(msg + off, data, data_len);
        off += data_len;
        msg[off++] = counter++;
        a_hmac_sha1(key, 32, msg, off, hash);
        if (n > 20)
            n = 20;
        memcpy(out + pos, hash, n);
        pos += n;
    }
}

static void a_kdf_sha256(const u8 *key, const char *label, const u8 *data,
                         unsigned int data_len, u8 *out, unsigned int out_len)
{
    u8 msg[256], hash[32];
    unsigned int label_len = (unsigned int)strlen(label);
    unsigned int pos = 0;
    u16 counter = 1;

    while (pos < out_len) {
        unsigned int n = out_len - pos;
        unsigned int off = 0;
        msg[off++] = (u8)counter;
        msg[off++] = (u8)(counter >> 8);
        memcpy(msg + off, label, label_len);
        off += label_len;
        memcpy(msg + off, data, data_len);
        off += data_len;
        msg[off++] = (u8)(out_len * 8);
        msg[off++] = (u8)((out_len * 8) >> 8);
        counter++;
        a_hmac_sha256(key, 32, msg, off, hash);
        if (n > 32)
            n = 32;
        memcpy(out + pos, hash, n);
        pos += n;
    }
}

static void a_derive_ptk(const u8 pmk[32], const u8 aa[6], const u8 spa[6],
                         const u8 anonce[32], const u8 snonce[32],
                         unsigned int akm, u8 ptk[48])
{
    u8 data[76];
    const u8 *m1 = memcmp(aa, spa, 6) <= 0 ? aa : spa;
    const u8 *m2 = m1 == aa ? spa : aa;
    const u8 *n1 = memcmp(anonce, snonce, 32) <= 0 ? anonce : snonce;
    const u8 *n2 = n1 == anonce ? snonce : anonce;

    memcpy(data, m1, 6);
    memcpy(data + 6, m2, 6);
    memcpy(data + 12, n1, 32);
    memcpy(data + 44, n2, 32);
    if (akm == 2)
        a_prf_sha1(pmk, "Pairwise key expansion", data, 76, ptk, 48);
    else
        a_kdf_sha256(pmk, "Pairwise key expansion", data, 76, ptk, 48);
}

static void a_calc_mic(const u8 kck[16], const u8 *frame, unsigned int len,
                       unsigned int ver, u8 mic[16])
{
    if (ver == 2) {
        u8 h[20];
        a_hmac_sha1(kck, 16, frame, len, h);
        memcpy(mic, h, 16);
    } else {
        a_cmac(kck, frame, len, mic);
    }
}

// rfc 3394 wrap, the authenticator's own copy for building 3/4 and 1/2.
static int a_wrap(const u8 kek[16], const u8 *in, unsigned int in_len, u8 *out)
{
    struct crypto_aes_ctx ctx;
    u8 a[8], b[16];
    unsigned int n = in_len / 8, i, j, t;

    if ((in_len & 7))
        return -EINVAL;
    if (aes_expandkey(&ctx, kek, 16))
        return -EINVAL;
    memcpy(a, (const u8[8]){0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6}, 8);
    memcpy(out + 8, in, in_len);
    for (j = 0; j <= 5; j++) {
        for (i = 1; i <= n; i++) {
            t = n * j + i;
            memcpy(b, a, 8);
            memcpy(b + 8, out + 8 + 8 * (i - 1), 8);
            aes_encrypt(&ctx, b, b);
            memcpy(a, b, 8);
            a[4] ^= (u8)(t >> 24);
            a[5] ^= (u8)(t >> 16);
            a[6] ^= (u8)(t >> 8);
            a[7] ^= (u8)t;
            memcpy(out + 8 + 8 * (i - 1), b + 8, 8);
        }
    }
    memcpy(out, a, 8);
    return (int)in_len + 8;
}

// ── authenticator state and ops ────────────────────────────────────────────
struct key_call {
    enum wpa_psk_key_type type;
    unsigned int idx;
    u8 key[32];
    unsigned int key_len;
    u8 rsc[8];
    unsigned int rsc_len;
    u8 tx;
};

struct auth {
    u8 pmk[32];
    u8 ptk[48];
    u8 aa[6];
    u8 spa[6];
    u8 anonce[32];
    u8 snonce[32];
    unsigned int akm;
    unsigned int ver;
    u8 gtk[16];
    u8 gtk_id;
    u8 gtk_tx;
    u8 igtk[16];
    u16 igtk_id;
    u8 ipn[6];
    u8 rsc[8];
    // hostapd increments the counter for every frame it sends, including a
    // retransmission. (satoru)
    u8 replay[8];
    unsigned int tx_count;
    u8 last_tx[WP_MAX_FRAME];
    unsigned int last_tx_len;
    unsigned int set_count;
    struct key_call calls[32];
    unsigned int done_count;
    unsigned int done_ok;
    unsigned int done_timeout;
    unsigned int done_unsupported;
    unsigned int done_key_error;
    enum wpa_psk_status done_status;
    int fail_set_key;
};

static int auth_tx(void *priv, const u8 *frame, unsigned int len)
{
    struct auth *a = priv;
    if (len > sizeof(a->last_tx))
        return -EINVAL;
    memcpy(a->last_tx, frame, len);
    a->last_tx_len = len;
    a->tx_count++;
    return 0;
}

static int auth_set_key(void *priv, enum wpa_psk_key_type type,
                        unsigned int idx, const u8 *key, unsigned int key_len,
                        const u8 *rsc, unsigned int rsc_len, u8 tx)
{
    struct auth *a = priv;
    struct key_call *c;

    if (a->set_count >= sizeof(a->calls) / sizeof(a->calls[0]))
        return -ENOSPC;
    c = &a->calls[a->set_count++];
    memset(c, 0, sizeof(*c));
    c->type = type;
    c->idx = idx;
    c->key_len = key_len > sizeof(c->key) ? sizeof(c->key) : key_len;
    if (key)
        memcpy(c->key, key, c->key_len);
    c->rsc_len = rsc_len > sizeof(c->rsc) ? sizeof(c->rsc) : rsc_len;
    if (rsc)
        memcpy(c->rsc, rsc, c->rsc_len);
    c->tx = tx;
    return a->fail_set_key ? -EIO : 0;
}

static void auth_done(void *priv, enum wpa_psk_status status)
{
    struct auth *a = priv;
    a->done_count++;
    a->done_status = status;
    if (status == WPA_PSK_STATUS_OK)
        a->done_ok++;
    else if (status == WPA_PSK_STATUS_TIMEOUT_HINT)
        a->done_timeout++;
    else if (status == WPA_PSK_STATUS_UNSUPPORTED_AP_SECURITY)
        a->done_unsupported++;
    else if (status == WPA_PSK_STATUS_KEY_ERROR)
        a->done_key_error++;
}

static const struct wpa_psk_ops auth_ops = {
    .tx_eapol = auth_tx,
    .set_key = auth_set_key,
    .done = auth_done,
};

static u16 g16(const u8 *p)
{
    return (u16)(((u16)p[0] << 8) | p[1]);
}

static u16 g16le(const u8 *p)
{
    return (u16)(p[0] | ((u16)p[1] << 8));
}

static void p16(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

static unsigned int build_rsn(u8 *ie, u16 caps, int sae_only, int tkip_only)
{
    u8 *p = ie + 2;
    unsigned int akm_count = sae_only ? 1 : 2;

    ie[0] = 0x30;
    p[0] = 1; p[1] = 0;  // version 1
    p[2] = 0x00; p[3] = 0x0f; p[4] = 0xac;
    p[5] = tkip_only ? 2 : 4;
    p[6] = 1; p[7] = 0;
    p[8] = 0x00; p[9] = 0x0f; p[10] = 0xac;
    p[11] = tkip_only ? 2 : 4;
    p[12] = (u8)akm_count; p[13] = 0;
    if (sae_only) {
        p[14] = 0x00; p[15] = 0x0f; p[16] = 0xac; p[17] = 8;
        p += 18;
    } else {
        // akm 2 first, akm 6 second: selection must prefer 2 unless mfpr.
        p[14] = 0x00; p[15] = 0x0f; p[16] = 0xac; p[17] = 2;
        p[18] = 0x00; p[19] = 0x0f; p[20] = 0xac; p[21] = 6;
        p += 22;
    }
    p[0] = (u8)caps; p[1] = (u8)(caps >> 8);
    p += 2;
    ie[1] = (u8)(p - (ie + 2));
    return 2 + ie[1];
}

static unsigned int build_eapol(u8 *out, u16 key_info, const u8 *replay,
                                const u8 *nonce, const u8 *kd,
                                unsigned int kd_len, const u8 *kck,
                                unsigned int ver)
{
    unsigned int total = WP_KD_OFF + kd_len;
    u8 mic[16];

    memset(out, 0, total);
    out[0] = 2;
    out[1] = 3;
    p16(out + 2, (u16)(total - 4));
    out[4] = 2;
    p16(out + 5, key_info);
    memcpy(out + 9, replay, 8);
    if (nonce)
        memcpy(out + 17, nonce, 32);
    p16(out + WP_KD_LEN_OFF, (u16)kd_len);
    if (kd_len)
        memcpy(out + WP_KD_OFF, kd, kd_len);
    if (kck) {
        a_calc_mic(kck, out, total, ver, mic);
        memcpy(out + WP_MIC_OFF, mic, 16);
    }
    return total;
}

static int frame_mic_ok(const u8 *frame, unsigned int len, const u8 *kck,
                        unsigned int ver)
{
    u8 tmp[WP_MAX_FRAME], want[16], got[16];

    if (len > sizeof(tmp))
        return 0;
    memcpy(tmp, frame, len);
    memcpy(got, tmp + WP_MIC_OFF, 16);
    memset(tmp + WP_MIC_OFF, 0, 16);
    a_calc_mic(kck, tmp, len, ver, want);
    return memcmp(got, want, 16) == 0;
}

// ── fixed/oracle vectors ───────────────────────────────────────────────────
static void test_pmk(void)
{
    u8 pmk[32];
    int i, ok = 1;
    char detail[96];

    for (i = 0; i < WV_PMK_N; i++) {
        const struct wv_pmk_vec *v = wv_pmk[i];
        if (wpa_psk_pmk((const char *)v->pass, v->pass_len, v->ssid,
                        v->ssid_len, pmk) || memcmp(pmk, v->pmk, 32)) {
            ok = 0;
            break;
        }
    }
    snprintf(detail, sizeof(detail), "vectors=%d standard+random", WV_PMK_N);
    wp_result("wpa_pmk", ok, detail);
}

static void test_ptk(void)
{
    u8 ptk[48];
    int i, ok = 1;
    char detail[96];

    for (i = 0; i < WV_PTK_N; i++) {
        const struct wv_ptk_vec *v = wv_ptk[i];
        if (wpa_psk_derive_ptk(v->pmk, v->aa, v->spa, v->anonce, v->snonce,
                               v->akm, ptk) ||
            memcmp(ptk, v->ptk, 48)) {
            ok = 0;
            break;
        }
    }
    snprintf(detail, sizeof(detail), "vectors=%d akm2+akm6", WV_PTK_N);
    wp_result("wpa_ptk", ok, detail);
}

static void test_mic(void)
{
    u8 mic[16];
    int i, ok = 1;
    char detail[96];

    for (i = 0; i < WV_MIC_N; i++) {
        const struct wv_mic_vec *v = wv_mic[i];
        if (wpa_psk_calc_mic(v->kck, v->frame, v->frame_len, v->ver, mic) ||
            memcmp(mic, v->mic, 16)) {
            ok = 0;
            break;
        }
    }
    snprintf(detail, sizeof(detail), "vectors=%d ver2+ver3 full frames",
             WV_MIC_N);
    wp_result("wpa_mic", ok, detail);
}

static void test_unwrap(void)
{
    u8 pt[32];
    int i, ok = 1, reject_ok = 1;
    char detail[96];

    for (i = 0; i < WV_UNWRAP_N; i++) {
        const struct wv_unwrap_vec *v = wv_unwrap[i];
        int n = wpa_crypto_aes_unwrap(v->kek, v->wrapped, v->wrapped_len, pt);
        if (n != (int)v->pt_len || memcmp(pt, v->pt, v->pt_len)) {
            ok = 0;
            break;
        }
        {
            u8 bad[40];
            memcpy(bad, v->wrapped, v->wrapped_len);
            bad[0] ^= 1;
            if (wpa_crypto_aes_unwrap(v->kek, bad, v->wrapped_len, pt) >= 0)
                reject_ok = 0;
        }
    }
    snprintf(detail, sizeof(detail), "vectors=%d bad-integrity=%d",
             WV_UNWRAP_N, reject_ok);
    wp_result("wpa_unwrap", ok && reject_ok, detail);
}

// ── self-play handshake, authenticator behaves like hostapd ────────────────
static u8 *auth_next_replay(struct auth *a)
{
    int i;
    for (i = 7; i >= 0; i--)
        if (++a->replay[i])
            break;
    return a->replay;
}

static unsigned int count_keys(const struct auth *a, enum wpa_psk_key_type type)
{
    unsigned int i, n = 0;
    for (i = 0; i < a->set_count; i++)
        if (a->calls[i].type == type)
            n++;
    return n;
}

static struct key_call *last_key(struct auth *a, enum wpa_psk_key_type type)
{
    unsigned int i;
    for (i = a->set_count; i > 0; i--)
        if (a->calls[i - 1].type == type)
            return &a->calls[i - 1];
    return NULL;
}

struct hs {
    struct wpa_psk_ctx ctx;
    struct auth a;
    u8 rsn[64];
    unsigned int rsn_len;
    u8 anonce[32];
    unsigned int ver;
    unsigned int akm;
    unsigned int mfp_mode;  // 0 none, 1 mfpc only, 2 mfpr
};

struct kd_opts {
    int corrupt_mic;
    int missing_rsn;
    int wrong_rsn;
    unsigned int gtk_len;
    int include_igtk;
    int new_gtk;
    int missing_gtk;
};

static int hs_feed(struct hs *h, const u8 *frame, unsigned int len)
{
    return wpa_psk_rx_eapol(&h->ctx, frame, len);
}

static int hs_start(struct hs *h, unsigned int mfp_mode)
{
    struct wpa_psk_cfg cfg;
    struct wpa_psk_ops ops = auth_ops;
    u16 caps = 0;
    int err;

    memset(h, 0, sizeof(*h));
    h->mfp_mode = mfp_mode;
    if (mfp_mode == 1)
        caps = 0x0080;  // mfpc only
    else if (mfp_mode == 2)
        caps = 0x00c0;  // mfpr | mfpc
    h->akm = mfp_mode == 2 ? 6 : 2;
    h->ver = h->akm == 6 ? 3 : 2;
    h->rsn_len = build_rsn(h->rsn, caps, 0, 0);
    memset(h->a.aa, 0x02, 6);
    memset(h->a.spa, 0x10, 6);
    memset(h->anonce, 0x11, 32);
    h->a.akm = h->akm;
    h->a.ver = h->ver;
    memcpy(h->a.anonce, h->anonce, 32);
    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.own_mac, h->a.spa, 6);
    memcpy(cfg.bssid, h->a.aa, 6);
    cfg.ssid = (const u8 *)"IEEE";
    cfg.ssid_len = 4;
    cfg.passphrase = "password";
    cfg.passphrase_len = 8;
    cfg.rsn_ie = h->rsn;
    cfg.rsn_ie_len = h->rsn_len;
    ops.priv = &h->a;
    err = wpa_psk_init(&h->ctx, &cfg, &ops);
    if (err)
        return err;
    if (h->ctx.mfp_required != (u8)(mfp_mode == 2))
        return -EINVAL;
    if (h->ctx.assoc_ie_len != 22 || h->ctx.assoc_ie[0] != 0x30 ||
        h->ctx.assoc_ie[13] != 4 || h->ctx.assoc_ie[19] != h->akm)
        return -EINVAL;
    if (g16le(h->ctx.assoc_ie + 20) != (mfp_mode == 2 ? 0x00c0 : 0))
        return -EINVAL;
    a_pbkdf2((const u8 *)"password", 8, (const u8 *)"IEEE", 4, h->a.pmk);
    return 0;
}

static int hs_1of4(struct hs *h, const u8 *anonce, const u8 *replay)
{
    u8 frame[WP_MAX_FRAME];
    unsigned int len;

    if (anonce)
        memcpy(h->anonce, anonce, 32);
    memcpy(h->a.anonce, h->anonce, 32);
    len = build_eapol(frame, (u16)(h->ver | 0x0008 | 0x0080),
                      replay ? replay : auth_next_replay(&h->a),
                      h->anonce, NULL, 0, NULL, h->ver);
    return hs_feed(h, frame, len);
}

static int hs_2of4(struct hs *h)
{
    u8 *f = h->a.last_tx;

    if (h->a.tx_count == 0 || h->a.last_tx_len < WP_KD_OFF)
        return -1;
    if (g16(f + 5) != (u16)(h->ver | 0x0008 | 0x0100))
        return -1;
    if (g16(f + WP_KD_LEN_OFF) != h->ctx.assoc_ie_len)
        return -1;
    if (memcmp(f + WP_KD_OFF, h->ctx.assoc_ie, h->ctx.assoc_ie_len))
        return -1;
    memcpy(h->a.snonce, f + 17, 32);
    a_derive_ptk(h->a.pmk, h->a.aa, h->a.spa, h->a.anonce, h->a.snonce,
                 h->akm, h->a.ptk);
    if (!frame_mic_ok(f, h->a.last_tx_len, h->a.ptk, h->ver))
        return -1;
    return 0;
}

static unsigned int hs_make_3of4(struct hs *h, const struct kd_opts *o,
                                 u8 *frame)
{
    u8 kd[256], wrapped[256];
    unsigned int n = 0, wlen, total;
    u8 rsc[8] = { 1, 2, 3, 4, 5, 6, 0, 0 };
    u16 key_info = (u16)(h->ver | 0x0008 | 0x0040 | 0x0080 | 0x0100 |
                         0x0200 | 0x1000);
    unsigned int gtk_len = o->gtk_len ? o->gtk_len : 16;

    if (o->new_gtk)
        memset(h->a.gtk, 0x6b, 16);
    if (!o->missing_gtk) {
        kd[n++] = 0xdd;
        kd[n++] = (u8)(4 + 2 + gtk_len);
        kd[n++] = 0x00; kd[n++] = 0x0f; kd[n++] = 0xac; kd[n++] = 1;
        kd[n++] = (u8)(h->a.gtk_id | (h->a.gtk_tx << 2));
        kd[n++] = 0;
        if (gtk_len <= 16) {
            memcpy(kd + n, h->a.gtk, gtk_len);
        } else {
            for (unsigned int i = 0; i < gtk_len; i++)
                kd[n + i] = (u8)(0x40 + i);
        }
        n += gtk_len;
    }
    if (o->include_igtk) {
        kd[n++] = 0xdd;
        kd[n++] = 28;
        kd[n++] = 0x00; kd[n++] = 0x0f; kd[n++] = 0xac; kd[n++] = 9;
        kd[n++] = (u8)h->a.igtk_id;
        kd[n++] = (u8)(h->a.igtk_id >> 8);
        memcpy(kd + n, h->a.ipn, 6);
        n += 6;
        memcpy(kd + n, h->a.igtk, 16);
        n += 16;
    }
    if (!o->missing_rsn) {
        memcpy(kd + n, h->rsn, h->rsn_len);
        if (o->wrong_rsn)
            kd[n + 3] ^= 0x01;
        n += h->rsn_len;
    }
    while (n & 7)
        kd[n++] = 0;
    wlen = (unsigned int)a_wrap(h->a.ptk + 16, kd, n, wrapped);
    if ((int)wlen < 0)
        return 0;
    total = build_eapol(frame, key_info, auth_next_replay(&h->a), h->anonce,
                        wrapped, wlen, NULL, h->ver);
    memcpy(frame + 65, rsc, 8);
    {
        u8 mic[16];
        a_calc_mic(h->a.ptk, frame, total, h->ver, mic);
        memcpy(frame + WP_MIC_OFF, mic, 16);
    }
    if (o->corrupt_mic)
        frame[WP_MIC_OFF + 3] ^= 0x40;
    return total;
}

static unsigned int hs_make_group_1of2(struct hs *h, const u8 *gtk,
                                       unsigned int gtk_id, u8 *frame)
{
    u8 kd[64], wrapped[64];
    unsigned int n = 0, wlen, total;
    u16 key_info = (u16)(h->ver | 0x0080 | 0x0100 | 0x0200 | 0x1000);

    kd[n++] = 0xdd;
    kd[n++] = 4 + 2 + 16;
    kd[n++] = 0x00; kd[n++] = 0x0f; kd[n++] = 0xac; kd[n++] = 1;
    kd[n++] = (u8)gtk_id;
    kd[n++] = 0;
    memcpy(kd + n, gtk, 16);
    n += 16;
    while (n & 7)
        kd[n++] = 0;
    wlen = (unsigned int)a_wrap(h->a.ptk + 16, kd, n, wrapped);
    if ((int)wlen < 0)
        return 0;
    total = build_eapol(frame, key_info, auth_next_replay(&h->a), NULL,
                        wrapped, wlen, NULL, h->ver);
    memcpy(frame + 65, (u8[8]){9, 8, 7, 6, 5, 4, 0, 0}, 8);
    {
        u8 mic[16];
        a_calc_mic(h->a.ptk, frame, total, h->ver, mic);
        memcpy(frame + WP_MIC_OFF, mic, 16);
    }
    return total;
}

static int hs_complete(struct hs *h, const struct kd_opts *opts)
{
    u8 frame[WP_MAX_FRAME];
    unsigned int len = hs_make_3of4(h, opts, frame);

    if (!len)
        return -1;
    if (hs_feed(h, frame, len))
        return -1;
    if (h->a.tx_count < 2)
        return -1;
    if (g16(h->a.last_tx + 5) !=
        (u16)(h->ver | 0x0008 | 0x0100 | 0x0200))
        return -1;
    if (g16(h->a.last_tx + WP_KD_LEN_OFF) != 0)
        return -1;
    if (!frame_mic_ok(h->a.last_tx, h->a.last_tx_len, h->a.ptk, h->ver))
        return -1;
    return 0;
}

// ── handshake cases ────────────────────────────────────────────────────────
static void test_handshake_basic(unsigned int mfp_mode)
{
    struct hs h;
    struct kd_opts o;
    char detail[128];
    int ok = 1;
    const char *name = mfp_mode == 2 ? "wpa_handshake_akm6" :
                       "wpa_handshake_akm2";

    if (hs_start(&h, mfp_mode))
        ok = 0;
    if (ok && (hs_1of4(&h, NULL, NULL) || hs_2of4(&h)))
        ok = 0;
    memset(&o, 0, sizeof(o));
    o.gtk_len = 16;
    o.include_igtk = mfp_mode == 2;
    if (ok && hs_complete(&h, &o))
        ok = 0;
    if (ok && (wpa_psk_state(&h.ctx) != WPA_PSK_STATE_COMPLETED ||
               count_keys(&h.a, WPA_PSK_KEY_PAIRWISE) != 1 ||
               count_keys(&h.a, WPA_PSK_KEY_GROUP) != 1 ||
               count_keys(&h.a, WPA_PSK_KEY_IGTK) != (mfp_mode == 2 ? 1u : 0u)))
        ok = 0;
    if (ok) {
        struct key_call *c = last_key(&h.a, WPA_PSK_KEY_PAIRWISE);
        if (!c || memcmp(c->key, h.a.ptk + 32, 16))
            ok = 0;
    }
    // one group rekey 1/2 -> 2/2 with a new gtk
    if (ok) {
        u8 gtk[16], frame[WP_MAX_FRAME];
        unsigned int len;
        memset(gtk, 0x7c, 16);
        len = hs_make_group_1of2(&h, gtk, 0, frame);
        if (!len || hs_feed(&h, frame, len) ||
            count_keys(&h.a, WPA_PSK_KEY_GROUP) != 2 || h.a.done_ok != 2 ||
            wpa_psk_state(&h.ctx) != WPA_PSK_STATE_COMPLETED)
            ok = 0;
        if (ok) {
            struct key_call *c = last_key(&h.a, WPA_PSK_KEY_GROUP);
            if (!c || memcmp(c->key, gtk, 16) || c->idx != 0)
                ok = 0;
        }
    }
    wpa_psk_deinit(&h.ctx);
    snprintf(detail, sizeof(detail), "akm=%u mfp=%u tx=%u pair=%u grp=%u igtk=%u",
             h.akm, mfp_mode, h.a.tx_count,
             count_keys(&h.a, WPA_PSK_KEY_PAIRWISE),
             count_keys(&h.a, WPA_PSK_KEY_GROUP),
             count_keys(&h.a, WPA_PSK_KEY_IGTK));
    wp_result(name, ok, detail);
}

static void test_lost_4of4(void)
{
    struct hs h;
    struct kd_opts o;
    u8 frame[WP_MAX_FRAME];
    unsigned int len;
    int ok = 1;

    if (hs_start(&h, 0) || hs_1of4(&h, NULL, NULL) || hs_2of4(&h))
        ok = 0;
    memset(&o, 0, sizeof(o));
    o.gtk_len = 16;
    if (ok && hs_complete(&h, &o))
        ok = 0;
    if (ok) {
        // hostapd retransmits 3/4 with counter+1 and the same anonce after a
        // lost 4/4; the station answers each one, but must not reinstall.
        for (int i = 0; i < 2; i++) {
            len = hs_make_3of4(&h, &o, frame);
            if (!len || hs_feed(&h, frame, len) ||
                h.a.tx_count != (unsigned int)(3 + i) ||
                count_keys(&h.a, WPA_PSK_KEY_PAIRWISE) != 1 ||
                count_keys(&h.a, WPA_PSK_KEY_GROUP) != 1 ||
                h.a.done_ok != 1)
                ok = 0;
        }
    }
    wpa_psk_deinit(&h.ctx);
    wp_result("wpa_lost_4of4", ok, ok ? "two 4/4, no reinstall"
                                         : "unexpected");
}

static void test_forged_1of4_after(void)
{
    struct hs h;
    struct kd_opts o;
    u8 frame[WP_MAX_FRAME], fake_nonce[32], fake_replay[8];
    unsigned int len;
    int ok = 1;

    if (hs_start(&h, 0) || hs_1of4(&h, NULL, NULL) || hs_2of4(&h))
        ok = 0;
    memset(&o, 0, sizeof(o));
    o.gtk_len = 16;
    if (ok && hs_complete(&h, &o))
        ok = 0;
    if (ok) {
        unsigned int done = h.a.done_count;
        memset(fake_nonce, 0x5a, 32);
        memset(fake_replay, 0xff, 8);
        len = build_eapol(frame, (u16)(h.ver | 0x0008 | 0x0080),
                          fake_replay, fake_nonce, NULL, 0, NULL, h.ver);
        if (hs_feed(&h, frame, len) || h.a.done_count != done ||
            wpa_psk_state(&h.ctx) != WPA_PSK_STATE_COMPLETED ||
            wpa_psk_timeout(&h.ctx) != -EALREADY)
            ok = 0;
    }
    // the committed tk must still work for a real group rekey.
    if (ok) {
        u8 gtk[16];
        memset(gtk, 0x6d, 16);
        len = hs_make_group_1of2(&h, gtk, 1, frame);
        if (!len || hs_feed(&h, frame, len) ||
            count_keys(&h.a, WPA_PSK_KEY_GROUP) != 2 || h.a.done_ok != 2)
            ok = 0;
    }
    wpa_psk_deinit(&h.ctx);
    wp_result("wpa_forged_1of4_after", ok, ok ? "tk intact, group rekey ok"
                                               : "unexpected");
}

static void test_forged_1of4_ff(void)
{
    struct hs h;
    struct kd_opts o;
    u8 frame[WP_MAX_FRAME], nonce[32], replay[8];
    unsigned int len;
    int ok = 1;

    if (hs_start(&h, 0))
        ok = 0;
    if (ok) {
        memset(nonce, 0x33, 32);
        memset(replay, 0xff, 8);
        len = build_eapol(frame, (u16)(h.ver | 0x0008 | 0x0080), replay,
                          nonce, NULL, 0, NULL, h.ver);
        if (hs_feed(&h, frame, len))
            ok = 0;
    }
    // the real 1/4 has a lower counter but no counter has been VERIFIED yet.
    if (ok && (hs_1of4(&h, NULL, NULL) || hs_2of4(&h)))
        ok = 0;
    memset(&o, 0, sizeof(o));
    o.gtk_len = 16;
    if (ok && hs_complete(&h, &o))
        ok = 0;
    if (ok && (count_keys(&h.a, WPA_PSK_KEY_PAIRWISE) != 1 ||
               h.a.done_ok != 1))
        ok = 0;
    wpa_psk_deinit(&h.ctx);
    wp_result("wpa_forged_1of4_ff", ok, ok ? "real 3/4 still completes"
                                            : "unexpected");
}

static void test_forged_3of4_bad_mic(void)
{
    struct hs h;
    struct kd_opts o;
    u8 frame[WP_MAX_FRAME];
    unsigned int len;
    int ok = 1;

    if (hs_start(&h, 0) || hs_1of4(&h, NULL, NULL) || hs_2of4(&h))
        ok = 0;
    memset(&o, 0, sizeof(o));
    o.gtk_len = 16;
    o.corrupt_mic = 1;
    if (ok) {
        unsigned int tx = h.a.tx_count, done = h.a.done_count;
        len = hs_make_3of4(&h, &o, frame);
        if (!len || hs_feed(&h, frame, len) != -EBADMSG ||
            h.a.tx_count != tx || h.a.done_count != done ||
            h.a.set_count != 0)
            ok = 0;
    }
    if (ok) {
        struct wpa_psk_stats st;
        wpa_psk_get_stats(&h.ctx, &st);
        if (st.bad_mic != 1)
            ok = 0;
    }
    o.corrupt_mic = 0;
    if (ok && hs_complete(&h, &o))
        ok = 0;
    wpa_psk_deinit(&h.ctx);
    wp_result("wpa_forged_3of4_bad_mic", ok, ok ? "dropped, real 3/4 ok"
                                                 : "unexpected");
}

static void test_bad_3of4_content(void)
{
    struct hs h;
    struct kd_opts o;
    u8 frame[WP_MAX_FRAME];
    unsigned int len;
    int ok = 1, missing = 1, badlen = 1, mismatch = 1, nogtk = 1;

    if (hs_start(&h, 0) || hs_1of4(&h, NULL, NULL) || hs_2of4(&h))
        ok = 0;
    memset(&o, 0, sizeof(o));
    o.gtk_len = 16;
    if (ok) {
        o.missing_rsn = 1;
        len = hs_make_3of4(&h, &o, frame);
        if (!len || hs_feed(&h, frame, len) != -EINVAL ||
            h.a.done_count != 0 || h.a.set_count != 0)
            missing = 0;
        o.missing_rsn = 0;
        o.gtk_len = 32;
        len = hs_make_3of4(&h, &o, frame);
        if (!len || hs_feed(&h, frame, len) != -EINVAL ||
            h.a.done_count != 0 || h.a.set_count != 0)
            badlen = 0;
        o.gtk_len = 16;
        o.wrong_rsn = 1;
        len = hs_make_3of4(&h, &o, frame);
        if (!len || hs_feed(&h, frame, len) != -EINVAL ||
            h.a.done_count != 0 || h.a.set_count != 0)
            mismatch = 0;
        o.wrong_rsn = 0;
        o.missing_gtk = 1;
        len = hs_make_3of4(&h, &o, frame);
        if (!len || hs_feed(&h, frame, len) != -EINVAL ||
            h.a.done_count != 0 || h.a.set_count != 0)
            nogtk = 0;
        o.missing_gtk = 0;
        if (!missing || !badlen || !mismatch || !nogtk)
            ok = 0;
    }
    if (ok) {
        struct wpa_psk_stats st;
        wpa_psk_get_stats(&h.ctx, &st);
        if (st.rsn_missing != 1 || st.bad_key_len != 1 ||
            st.rsn_mismatch != 1 || st.gtk_missing != 1)
            ok = 0;
    }
    if (ok && hs_complete(&h, &o))
        ok = 0;
    wpa_psk_deinit(&h.ctx);
    {
        char d[128];
        snprintf(d, sizeof(d), "missing=%d badlen=%d mismatch=%d nogtk=%d",
                 missing, badlen, mismatch, nogtk);
        wp_result("wpa_bad_3of4_content", ok, ok ? "all three dropped" : d);
    }
}

static void test_key_error(void)
{
    struct hs h;
    struct kd_opts o;
    u8 frame[WP_MAX_FRAME];
    unsigned int len;
    int ok = 1;

    if (hs_start(&h, 0) || hs_1of4(&h, NULL, NULL) || hs_2of4(&h))
        ok = 0;
    memset(&o, 0, sizeof(o));
    o.gtk_len = 16;
    h.a.fail_set_key = 1;
    if (ok) {
        len = hs_make_3of4(&h, &o, frame);
        if (!len || hs_feed(&h, frame, len) != -EIO ||
            h.a.done_count != 1 || h.a.done_key_error != 1 ||
            h.a.done_ok != 0)
            ok = 0;
    }
    wpa_psk_deinit(&h.ctx);
    wp_result("wpa_key_error", ok, ok ? "one done(KEY_ERROR)" : "unexpected");
}

static void test_gtk_missing(void)
{
    struct hs h;
    struct kd_opts o;
    u8 frame[WP_MAX_FRAME];
    unsigned int len;
    int ok = 1;

    if (hs_start(&h, 0) || hs_1of4(&h, NULL, NULL) || hs_2of4(&h))
        ok = 0;
    memset(&o, 0, sizeof(o));
    o.gtk_len = 16;
    o.missing_gtk = 1;
    if (ok) {
        len = hs_make_3of4(&h, &o, frame);
        if (!len || hs_feed(&h, frame, len) != -EINVAL ||
            h.a.done_count != 0 || h.a.set_count != 0)
            ok = 0;
        else {
            struct wpa_psk_stats st;
            wpa_psk_get_stats(&h.ctx, &st);
            if (st.gtk_missing != 1)
                ok = 0;
        }
    }
    o.missing_gtk = 0;
    if (ok && hs_complete(&h, &o))
        ok = 0;
    wpa_psk_deinit(&h.ctx);
    wp_result("wpa_gtk_missing", ok, ok ? "dropped, real 3/4 ok" : "unexpected");
}

static void test_two_tpk(void)
{
    struct hs h;
    struct kd_opts o;
    u8 frame[WP_MAX_FRAME], real_ptk[48], real_anonce[32], fake_anonce[32];
    unsigned int len;
    int ok = 1;

    if (hs_start(&h, 0) || hs_1of4(&h, NULL, NULL) || hs_2of4(&h))
        ok = 0;
    memcpy(real_ptk, h.a.ptk, 48);
    memcpy(real_anonce, h.anonce, 32);
    // a forged 1/4 in the middle of the handshake pushes a bogus candidate.
    if (ok) {
        memset(fake_anonce, 0x99, 32);
        len = build_eapol(frame, (u16)(h.ver | 0x0008 | 0x0080),
                          auth_next_replay(&h.a), fake_anonce, NULL, 0, NULL,
                          h.ver);
        if (hs_feed(&h, frame, len) || h.a.tx_count != 2)
            ok = 0;
    }
    // the real 3/4 matches the older candidate and must still complete.
    memcpy(h.a.ptk, real_ptk, 48);
    memcpy(h.anonce, real_anonce, 32);
    memcpy(h.a.anonce, real_anonce, 32);
    memset(&o, 0, sizeof(o));
    o.gtk_len = 16;
    if (ok && hs_complete(&h, &o))
        ok = 0;
    if (ok) {
        struct key_call *c = last_key(&h.a, WPA_PSK_KEY_PAIRWISE);
        if (count_keys(&h.a, WPA_PSK_KEY_PAIRWISE) != 1 || h.a.done_ok != 1 ||
            !c || memcmp(c->key, real_ptk + 32, 16))
            ok = 0;
    }
    wpa_psk_deinit(&h.ctx);
    wp_result("wpa_two_tpk", ok, ok ? "older candidate verified" : "unexpected");
}

static void test_group_replay(void)
{
    struct hs h;
    struct kd_opts o;
    u8 frame[WP_MAX_FRAME], old[WP_MAX_FRAME];
    unsigned int len, old_len = 0;
    int ok = 1;

    if (hs_start(&h, 0) || hs_1of4(&h, NULL, NULL) || hs_2of4(&h))
        ok = 0;
    memset(&o, 0, sizeof(o));
    o.gtk_len = 16;
    if (ok && hs_complete(&h, &o))
        ok = 0;
    if (ok) {
        u8 gtk1[16], gtk2[16];
        memset(gtk1, 0x81, 16);
        memset(gtk2, 0x82, 16);
        old_len = hs_make_group_1of2(&h, gtk1, 0, old);
        len = hs_make_group_1of2(&h, gtk2, 0, frame);
        if (!old_len || !len || hs_feed(&h, old, old_len) ||
            hs_feed(&h, frame, len) ||
            count_keys(&h.a, WPA_PSK_KEY_GROUP) != 3 || h.a.done_ok != 3)
            ok = 0;
    }
    if (ok) {
        unsigned int sc = h.a.set_count, dc = h.a.done_count;
        if (hs_feed(&h, old, old_len) != -EALREADY ||
            h.a.set_count != sc || h.a.done_count != dc)
            ok = 0;
        else {
            struct wpa_psk_stats st;
            wpa_psk_get_stats(&h.ctx, &st);
            if (st.replay != 1)
                ok = 0;
        }
    }
    wpa_psk_deinit(&h.ctx);
    wp_result("wpa_group_replay", ok, ok ? "old counter dropped" : "unexpected");
}

static void test_mfpc_only(void)
{
    struct hs h;
    struct kd_opts o;
    int ok = 1;

    if (hs_start(&h, 1))
        ok = 0;
    if (ok && (h.akm != 2 || h.ctx.mfp_required != 0 ||
               g16le(h.ctx.assoc_ie + 20) != 0))
        ok = 0;
    if (ok && (hs_1of4(&h, NULL, NULL) || hs_2of4(&h)))
        ok = 0;
    memset(&o, 0, sizeof(o));
    o.gtk_len = 16;
    o.include_igtk = 1;
    if (ok && hs_complete(&h, &o))
        ok = 0;
    if (ok && count_keys(&h.a, WPA_PSK_KEY_IGTK) != 0)
        ok = 0;
    wpa_psk_deinit(&h.ctx);
    wp_result("wpa_mfpc_only", ok, ok ? "no mfp bits, igtk ignored" : "unexpected");
}

static void test_pairwise_rekey(void)
{
    struct hs h;
    struct kd_opts o;
    u8 new_anonce[32];
    int ok = 1;

    if (hs_start(&h, 0) || hs_1of4(&h, NULL, NULL) || hs_2of4(&h))
        ok = 0;
    memset(&o, 0, sizeof(o));
    o.gtk_len = 16;
    if (ok && hs_complete(&h, &o))
        ok = 0;
    if (ok) {
        memset(new_anonce, 0x44, 32);
        // mfpc-only case already covered; a fresh 1/4 with a higher counter
        // starts a new 4-way on the completed link. (satoru)
        if (hs_1of4(&h, new_anonce, NULL) || hs_2of4(&h))
            ok = 0;
        else if (h.a.akm != 2)
            ok = 0;
    }
    if (ok && hs_complete(&h, &o))
        ok = 0;
    if (ok) {
        struct key_call *c = last_key(&h.a, WPA_PSK_KEY_PAIRWISE);
        if (count_keys(&h.a, WPA_PSK_KEY_PAIRWISE) != 2 ||
            count_keys(&h.a, WPA_PSK_KEY_GROUP) != 1 ||
            h.a.done_ok != 2 || !c || memcmp(c->key, h.a.ptk + 32, 16))
            ok = 0;
    }
    wpa_psk_deinit(&h.ctx);
    wp_result("wpa_pairwise_rekey", ok, ok ? "new tk installed once" : "unexpected");
}

static void test_repeated_1of4(void)
{
    struct hs h;
    u8 frame[WP_MAX_FRAME], first[32];
    unsigned int len;
    int ok = 1;

    if (hs_start(&h, 0))
        ok = 0;
    if (ok) {
        len = build_eapol(frame, (u16)(h.ver | 0x0008 | 0x0080),
                          auth_next_replay(&h.a), h.anonce, NULL, 0, NULL,
                          h.ver);
        if (hs_feed(&h, frame, len))
            ok = 0;
    }
    if (ok) {
        memcpy(first, h.a.last_tx + 17, 32);
        len = build_eapol(frame, (u16)(h.ver | 0x0008 | 0x0080),
                          auth_next_replay(&h.a), h.anonce, NULL, 0, NULL,
                          h.ver);
        if (hs_feed(&h, frame, len) || h.a.tx_count != 2 ||
            !memcmp(first, h.a.last_tx + 17, 32))
            ok = 0;
        else {
            memcpy(h.a.snonce, h.a.last_tx + 17, 32);
            a_derive_ptk(h.a.pmk, h.a.aa, h.a.spa, h.a.anonce, h.a.snonce,
                         h.akm, h.a.ptk);
            if (!frame_mic_ok(h.a.last_tx, h.a.last_tx_len, h.a.ptk, h.ver) ||
                h.a.set_count != 0 || h.a.done_count != 0)
                ok = 0;
        }
    }
    wpa_psk_deinit(&h.ctx);
    wp_result("wpa_repeated_1of4", ok, ok ? "fresh snonce before tk" : "unexpected");
}

static void test_wrong_passphrase(void)
{
    struct hs h;
    struct wpa_psk_cfg cfg;
    struct wpa_psk_ops ops = auth_ops;
    u8 frame[WP_MAX_FRAME], rsn[64];
    unsigned int len, rsn_len;
    int ok;

    memset(&h, 0, sizeof(h));
    rsn_len = build_rsn(rsn, 0, 0, 0);
    memset(h.a.aa, 0x02, 6);
    memset(h.a.spa, 0x10, 6);
    memset(h.a.anonce, 0x22, 32);
    h.a.akm = 2;
    h.a.ver = 2;
    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.own_mac, h.a.spa, 6);
    memcpy(cfg.bssid, h.a.aa, 6);
    cfg.ssid = (const u8 *)"IEEE";
    cfg.ssid_len = 4;
    cfg.passphrase = "wrongpass";
    cfg.passphrase_len = 9;
    cfg.rsn_ie = rsn;
    cfg.rsn_ie_len = rsn_len;
    ops.priv = &h.a;
    if (wpa_psk_init(&h.ctx, &cfg, &ops)) {
        wp_result("wpa_wrong_passphrase", 0, "init");
        return;
    }
    a_pbkdf2((const u8 *)"password", 8, (const u8 *)"IEEE", 4, h.a.pmk);
    len = build_eapol(frame, (u16)(2 | 0x0008 | 0x0080),
                      auth_next_replay(&h.a), h.a.anonce, NULL, 0, NULL, 2);
    ok = hs_feed(&h, frame, len) == 0 && h.a.tx_count == 1;
    // authenticator rejects the 2/4 mic, so no 3/4 arrives; the connect
    // timer surfaces the timeout hint. (satoru)
    memcpy(h.a.snonce, h.a.last_tx + 17, 32);
    a_derive_ptk(h.a.pmk, h.a.aa, h.a.spa, h.a.anonce, h.a.snonce, 2, h.a.ptk);
    ok = ok && !frame_mic_ok(h.a.last_tx, h.a.last_tx_len, h.a.ptk, 2);
    ok = ok && wpa_psk_timeout(&h.ctx) == 0 && h.a.done_timeout == 1 &&
         h.a.set_count == 0;
    wpa_psk_deinit(&h.ctx);
    wp_result("wpa_wrong_passphrase", ok,
              ok ? "2/4 mic rejected, timeout hint" : "unexpected");
}

static void test_unsupported(void)
{
    struct hs h;
    struct wpa_psk_cfg cfg;
    struct wpa_psk_ops ops = auth_ops;
    u8 rsn[64];
    unsigned int rsn_len;
    int ok = 1, ok2 = 1;

    memset(&h, 0, sizeof(h));
    ops.priv = &h.a;
    rsn_len = build_rsn(rsn, 0, 1, 0);
    memset(&cfg, 0, sizeof(cfg));
    memset(cfg.own_mac, 0x10, 6);
    memset(cfg.bssid, 0x02, 6);
    cfg.ssid = (const u8 *)"IEEE";
    cfg.ssid_len = 4;
    cfg.passphrase = "password";
    cfg.passphrase_len = 8;
    cfg.rsn_ie = rsn;
    cfg.rsn_ie_len = rsn_len;
    if (wpa_psk_init(&h.ctx, &cfg, &ops) != -EOPNOTSUPP ||
        h.a.done_unsupported != 1)
        ok = 0;
    wp_result("wpa_sae_only", ok, ok ? "unsupported" : "unexpected");

    memset(&h, 0, sizeof(h));
    ops.priv = &h.a;
    rsn_len = build_rsn(rsn, 0, 0, 1);
    cfg.rsn_ie = rsn;
    cfg.rsn_ie_len = rsn_len;
    if (wpa_psk_init(&h.ctx, &cfg, &ops) != -EOPNOTSUPP ||
        h.a.done_unsupported != 1)
        ok2 = 0;
    wp_result("wpa_tkip_only", ok2, ok2 ? "unsupported" : "unexpected");
}

void apitest_wpa_run(int *pass, int *total)
{
    lx_log("[apitest] wpa2-psk: oracle vectors and self-play\n");
    test_pmk();
    test_ptk();
    test_mic();
    test_unwrap();
    test_handshake_basic(0);
    test_handshake_basic(2);
    test_lost_4of4();
    test_forged_1of4_after();
    test_forged_1of4_ff();
    test_forged_3of4_bad_mic();
    test_bad_3of4_content();
    test_key_error();
    test_gtk_missing();
    test_two_tpk();
    test_group_replay();
    test_mfpc_only();
    test_pairwise_rekey();
    test_repeated_1of4();
    test_wrong_passphrase();
    test_unsupported();
    *pass += wp_pass;
    *total += wp_total;
}

// end (satoru)
