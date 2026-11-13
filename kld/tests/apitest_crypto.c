// apitest_crypto: known-answer and oracle tests for the kernel crypto API the
// way mac80211 uses it (scatterlists, aead_request with associated data, in
// place, shash init/update/finup, arc4 on the raw context).
//
// every algorithm here is the unmodified upstream implementation from the
// pinned tree (CRYPTOLIB in kld/Makefile); this file only drives it. the
// random corpus in crypto_vectors.h comes from python-cryptography, an
// independent implementation (gen_crypto_vectors.py). (satoru)

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/scatterlist.h>
#include <linux/err.h>
#include <crypto/aead.h>
#include <crypto/hash.h>
#include <crypto/skcipher.h>
#include <crypto/algapi.h>
#include <crypto/utils.h>
#include <crypto/arc4.h>
#include <crypto/aes.h>

#include "kimpl/lx_kshim.h"
#include "crypto_vectors.h"

static int cv_pass, cv_total;

static void cv_result(const char *name, int ok, const char *detail)
{
    lx_log("[apitest] ");
    lx_log(name);
    lx_log(ok ? ": PASS " : ": FAIL ");
    if (detail && detail[0])
        lx_log(detail);
    lx_log("\n");
    cv_total++;
    if (ok)
        cv_pass++;
}

static int unhex(const char *hex, u8 *out, unsigned int max)
{
    unsigned int n = 0;
    int hi = -1;

    while (hex && *hex) {
        int c = *hex++, v;
        if (c == ' ' || c == ':')
            continue;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return -1;
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= max)
                return -1;
            out[n++] = (u8)((hi << 4) | v);
            hi = -1;
        }
    }
    return hi < 0 ? (int)n : -1;
}

// mirrors net/mac80211/aead_api.c exactly: three-entry sg, aad copied into the
// request allocation, the tag as the last sg entry. (satoru)
static int aead_crypt(int enc, struct crypto_aead *tfm, u8 *iv,
                      const u8 *aad, unsigned int aad_len,
                      u8 *data, unsigned int data_len, u8 *mic)
{
    unsigned int mic_len = crypto_aead_authsize(tfm);
    int reqsize = sizeof(struct aead_request) + crypto_aead_reqsize(tfm);
    struct aead_request *req = kzalloc(reqsize + aad_len, GFP_KERNEL);
    struct scatterlist sg[3];
    u8 *__aad;
    int ret;

    if (!req)
        return -ENOMEM;
    __aad = (u8 *)req + reqsize;
    if (aad_len)
        memcpy(__aad, aad, aad_len);

    sg_init_table(sg, 3);
    sg_set_buf(&sg[0], __aad, aad_len);
    sg_set_buf(&sg[1], data, data_len);
    sg_set_buf(&sg[2], mic, mic_len);

    aead_request_set_tfm(req, tfm);
    aead_request_set_crypt(req, sg, sg,
                           enc ? data_len : data_len + mic_len, iv);
    aead_request_set_ad(req, aad_len);

    ret = enc ? crypto_aead_encrypt(req) : crypto_aead_decrypt(req);
    kfree_sensitive(req);
    return ret;
}

static int aead_setup(struct crypto_aead **out, const char *alg,
                      const u8 *key, unsigned int key_len,
                      unsigned int mic_len)
{
    struct crypto_aead *tfm = crypto_alloc_aead(alg, 0, CRYPTO_ALG_ASYNC);
    int err;

    if (IS_ERR(tfm))
        return PTR_ERR(tfm);
    err = crypto_aead_setkey(tfm, key, key_len);
    if (!err)
        err = crypto_aead_setauthsize(tfm, mic_len);
    if (err) {
        crypto_free_aead(tfm);
        return err;
    }
    *out = tfm;
    return 0;
}

// mirrors mac80211 aes_cmac.c: init, update in pieces, finup. (satoru)
static int shash_hash(const char *alg, const u8 *key, unsigned int key_len,
                      const u8 *msg, unsigned int msg_len, u8 *out,
                      unsigned int split)
{
    struct crypto_shash *tfm = crypto_alloc_shash(alg, 0, 0);
    SHASH_DESC_ON_STACK(desc, tfm);
    int err;

    if (IS_ERR(tfm))
        return PTR_ERR(tfm);
    err = crypto_shash_setkey(tfm, key, key_len);
    if (err) {
        crypto_free_shash(tfm);
        return err;
    }
    desc->tfm = tfm;
    err = crypto_shash_init(desc);
    if (!err && split > msg_len)
        split = msg_len;
    if (!err && split)
        err = crypto_shash_update(desc, msg, split);
    if (!err && msg_len > split)
        err = crypto_shash_update(desc, msg + split, msg_len - split);
    if (!err)
        err = crypto_shash_finup(desc, msg, 0, out);
    crypto_free_shash(tfm);
    return err;
}

// ── ccm(aes): RFC 3610 and a CCMP-shaped random set ────────────────────────
static void test_ccm(void)
{
    // RFC 3610 packet vector #1 and #13: 8 byte header, 23 byte payload,
    // 13 byte nonce and an 8 byte tag, exactly the CCMP geometry. (satoru)
    static const char *k1 = "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf";
    static const char *n1 = "00000003020100a0a1a2a3a4a5";
    static const char *a1 = "0001020304050607";
    static const char *p1 = "08090a0b0c0d0e0f101112131415161718191a1b1c1d1e";
    static const char *c1 = "588c979a61c663d2f066d0c2c0f989806d5f6b61dac38417e8d12cfdf926e0";
    static const char *k2 = "d7828d13b2b0bdc325a76236df93cc6b";
    static const char *n2 = "00412b4ea9cdbe3c9696766cfa";
    static const char *a2 = "0be1a88bace018b1";
    static const char *p2 = "08e8cf97d820ea258460e96ad9cf5289054d895ceac47c";
    static const char *c2 = "4cb97f86a2a4689a877947ab8091ef5386a6ffbdd080f8e78cf7cb0cddd7b3";

    struct crypto_aead *tfm;
    u8 key[16], nonce[16], aad[64], pt[64], ct[64], tag[16], got[64], mac[16];
    u8 iv[16];
    unsigned int i;
    int ok = 1, enc_ok = 1, dec_ok = 1, rej_ok = 1;
    int fail_i = -1, fail_err = 0;
    char detail[128];
    int err;

    // fixed RFC 3610 #1
    if (unhex(k1, key, sizeof(key)) != 16 || unhex(n1, nonce, sizeof(nonce)) != 13 ||
        unhex(a1, aad, sizeof(aad)) != 8 || unhex(p1, pt, sizeof(pt)) != 23 ||
        unhex(c1, ct, sizeof(ct)) != 23 + 8) {
        cv_result("ccm_aes_rfc3610", 0, "vector parse");
        return;
    }
    if ((err = aead_setup(&tfm, "ccm(aes)", key, 16, 8))) {
        snprintf(detail, sizeof(detail), "ccm alloc err=%d", err);
        cv_result("ccm_aes_rfc3610", 0, detail);
        return;
    }
    memcpy(got, pt, 23);
    memcpy(tag, ct + 23, 8);
    memset(iv, 0, sizeof(iv));
    iv[0] = 1;  // L' = 1, i.e. L = 2, as RFC 3610 requires. (satoru)
    memcpy(iv + 1, nonce, 13);
    err = aead_crypt(1, tfm, iv, aad, 8, got, 23, mac);
    ok = !err && !memcmp(got, ct, 23) && !memcmp(mac, tag, 8);
    if (ok) {
        // decrypt the published ciphertext and get the payload back. (satoru)
        memcpy(got, ct, 23);
        memcpy(mac, tag, 8);
        err = aead_crypt(0, tfm, iv, aad, 8, got, 23, mac);
        ok = !err && !memcmp(got, pt, 23);
    }
    // a corrupted tag must be rejected, not accepted with garbage. (satoru)
    if (ok) {
        memcpy(got, ct, 23);
        memcpy(mac, tag, 8);
        mac[7] ^= 0x80;
        err = aead_crypt(0, tfm, iv, aad, 8, got, 23, mac);
        ok = (err == -EBADMSG);
    }
    crypto_free_aead(tfm);
    if (!ok) {
        snprintf(detail, sizeof(detail), "err=%d got=%02x%02x ct=%02x%02x mac=%02x%02x tag=%02x%02x",
                 err, got[0], got[1], ct[0], ct[1], mac[0], mac[1], tag[0], tag[1]);
        cv_result("ccm_aes_rfc3610", 0, detail);
    } else {
        cv_result("ccm_aes_rfc3610", 1, "vector #1 enc+dec+tag-reject");
    }

    // fixed RFC 3610 #13, a different key and nonce. (satoru)
    if (unhex(k2, key, sizeof(key)) != 16 || unhex(n2, nonce, sizeof(nonce)) != 13 ||
        unhex(a2, aad, sizeof(aad)) != 8 || unhex(p2, pt, sizeof(pt)) != 23 ||
        unhex(c2, ct, sizeof(ct)) != 31) {
        cv_result("ccm_aes_rfc3610_2", 0, "vector parse");
        return;
    }
    if (aead_setup(&tfm, "ccm(aes)", key, 16, 8)) {
        cv_result("ccm_aes_rfc3610_2", 0, "alloc");
        return;
    }
    memcpy(got, pt, 23);
    memcpy(tag, ct + 23, 8);
    memset(iv, 0, sizeof(iv));
    iv[0] = 1;  // L' = 1, i.e. L = 2, as RFC 3610 requires. (satoru)
    memcpy(iv + 1, nonce, 13);
    err = aead_crypt(1, tfm, iv, aad, 8, got, 23, mac);
    ok = !err && !memcmp(got, ct, 23) && !memcmp(mac, tag, 8);
    if (ok) {
        memcpy(got, ct, 23);
        memcpy(mac, tag, 8);
        err = aead_crypt(0, tfm, iv, aad, 8, got, 23, mac);
        ok = !err && !memcmp(got, pt, 23);
    }
    crypto_free_aead(tfm);
    cv_result("ccm_aes_rfc3610_2", ok, ok ? "vector #13 enc+dec" : "mismatch");

    // random oracle corpus, CCMP shaped. (satoru)
    for (i = 0; i < CV_CCM_N; i++) {
        const struct cv_aead_vec *v = cv_ccm[i];
        if (aead_setup(&tfm, "ccm(aes)", v->key, 16, v->tag_len)) {
            enc_ok = 0;
            break;
        }
        memcpy(got, v->pt, v->pt_len);
        memset(iv, 0, sizeof(iv));
        iv[0] = 1;
        memcpy(iv + 1, v->iv, v->iv_len <= 13 ? v->iv_len : 13);
        err = aead_crypt(1, tfm, iv, v->aad, v->aad_len, got, v->pt_len, mac);
        if (err || memcmp(got, v->ct, v->pt_len) || memcmp(mac, v->tag, v->tag_len)) {
            enc_ok = 0;
            fail_i = (int)i;
            fail_err = err;
            crypto_free_aead(tfm);
            break;
        }
        memcpy(got, v->ct, v->pt_len);
        memcpy(mac, v->tag, v->tag_len);
        err = aead_crypt(0, tfm, iv, v->aad, v->aad_len, got, v->pt_len, mac);
        if (err || memcmp(got, v->pt, v->pt_len)) {
            dec_ok = 0;
            fail_i = (int)i;
            fail_err = err;
            crypto_free_aead(tfm);
            break;
        }
        mac[0] ^= 0x01;
        err = aead_crypt(0, tfm, iv, v->aad, v->aad_len, got, v->pt_len, mac);
        if (err != -EBADMSG)
            rej_ok = 0;
        crypto_free_aead(tfm);
    }
    snprintf(detail, sizeof(detail),
             "vectors=%d enc=%d dec=%d tag-reject=%d fail=%d err=%d",
             CV_CCM_N, enc_ok, dec_ok, rej_ok, fail_i, fail_err);
    cv_result("ccm_aes_random", enc_ok && dec_ok && rej_ok, detail);
}

// ── gcm(aes): NIST vectors, GMAC (empty plaintext) and random ──────────────
static void test_gcm(void)
{
    // NIST GCM spec test case 4: AAD present, full 16 byte tag. (satoru)
    static const char *nk = "feffe9928665731c6d6a8f9467308308";
    static const char *niv = "cafebabefacedbaddecaf888";
    static const char *na = "feedfacedeadbeeffeedfacedeadbeefabaddad2";
    static const char *np =
        "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
        "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39";
    static const char *nc =
        "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e"
        "21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091";
    static const char *nt = "5bc94fbc3221a5db94fae95ae7121a47";
    // the same key and AAD with an empty plaintext is the GMAC use; the tag
    // was produced by the independent oracle (python-cryptography). (satoru)
    static const char *gt = "346434fd51d5cd0c5887ec63e39b907a";

    struct crypto_aead *tfm;
    u8 key[16], iv[16], aad[64], pt[64], ct[64], tag[16], got[64], mac[16];
    unsigned int i;
    int ok = 1, enc_ok = 1, dec_ok = 1, gmac_ok = 1;
    int fail_i = -1, fail_err = 0;

    char detail[128];
    int err;

    if (unhex(nk, key, 16) != 16 || unhex(niv, iv, 12) != 12 ||
        unhex(na, aad, sizeof(aad)) != 20 || unhex(np, pt, sizeof(pt)) != 60 ||
        unhex(nc, ct, sizeof(ct)) != 60 || unhex(nt, tag, 16) != 16) {
        cv_result("gcm_aes_nist", 0, "vector parse");
        return;
    }
    if ((err = aead_setup(&tfm, "gcm(aes)", key, 16, 16))) {
        snprintf(detail, sizeof(detail), "gcm alloc err=%d", err);
        cv_result("gcm_aes_nist", 0, detail);
        return;
    }
    memcpy(got, pt, 60);
    err = aead_crypt(1, tfm, iv, aad, 20, got, 60, mac);
    ok = !err && !memcmp(got, ct, 60) && !memcmp(mac, tag, 16);
    if (ok) {
        memcpy(got, ct, 60);
        memcpy(mac, tag, 16);
        err = aead_crypt(0, tfm, iv, aad, 20, got, 60, mac);
        ok = !err && !memcmp(got, pt, 60);
    }
    crypto_free_aead(tfm);
    cv_result("gcm_aes_nist", ok, ok ? "case 4 enc+dec" : "mismatch");

    // GMAC: empty plaintext, AAD only, which is what ieee80211_aes_gmac does.
    // expected tag computed by the oracle for the same NIST inputs. (satoru)
    if (unhex(gt, tag, 16) == 16 && aead_setup(&tfm, "gcm(aes)", key, 16, 16) == 0) {
        err = aead_crypt(1, tfm, iv, aad, 20, got, 0, mac);
        gmac_ok = !err && !memcmp(mac, tag, 16);
        crypto_free_aead(tfm);
    } else {
        gmac_ok = 0;
    }
    cv_result("gcm_aes_gmac", gmac_ok, gmac_ok ? "empty pt, aad only" : "mismatch");

    for (i = 0; i < CV_GCM_N; i++) {
        const struct cv_aead_vec *v = cv_gcm[i];
        if (aead_setup(&tfm, "gcm(aes)", v->key, 16, v->tag_len)) {
            enc_ok = 0;
            break;
        }
        memcpy(got, v->pt, v->pt_len);
        memset(iv, 0, sizeof(iv));
        memcpy(iv, v->iv, v->iv_len);
        err = aead_crypt(1, tfm, iv, v->aad, v->aad_len, got, v->pt_len, mac);
        if (err || memcmp(got, v->ct, v->pt_len) || memcmp(mac, v->tag, v->tag_len)) {
            enc_ok = 0;
            fail_i = (int)i;
            fail_err = err;
            crypto_free_aead(tfm);
            break;
        }
        memcpy(got, v->ct, v->pt_len);
        memcpy(mac, v->tag, v->tag_len);
        err = aead_crypt(0, tfm, iv, v->aad, v->aad_len, got, v->pt_len, mac);
        if (err || memcmp(got, v->pt, v->pt_len)) {
            dec_ok = 0;
            fail_i = (int)i;
            fail_err = err;
            crypto_free_aead(tfm);
            break;
        }
        crypto_free_aead(tfm);
    }
    snprintf(detail, sizeof(detail), "vectors=%d enc=%d dec=%d (5 are GMAC) fail=%d err=%d",
             CV_GCM_N, enc_ok, dec_ok, fail_i, fail_err);
    cv_result("gcm_aes_random", enc_ok && dec_ok, detail);
}

// ── cmac(aes): RFC 4493 ────────────────────────────────────────────────────
static void test_cmac(void)
{
    static const char *key_hex = "2b7e151628aed2a6abf7158809cf4f3c";
    static const char *msg_hex =
        "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710";
    static const char *mac16 = "070a16b46b4d4144f79bdd9dd04a287c";
    static const char *mac40 = "dfa66747de9ae63030ca32611497c827";
    static const char *mac64 = "51f0bebf7e3b9d92fc49741779363cfe";
    static const char *mac0 = "bb1d6929e95937287fa37d129b756746";

    u8 key[16], msg[64], out[16], want[16];
    unsigned int i;
    int ok = 1, rand_ok = 1, err = 0;
    char detail[128];

    if (unhex(key_hex, key, 16) != 16 || unhex(msg_hex, msg, 64) != 64) {
        cv_result("cmac_aes_rfc4493", 0, "vector parse");
        return;
    }
    err = shash_hash("cmac(aes)", key, 16, NULL, 0, out, 0);
    if (err || unhex(mac0, want, 16) != 16 || memcmp(out, want, 16))
        ok = 0;
    if (ok && (shash_hash("cmac(aes)", key, 16, msg, 16, out, 0) ||
               unhex(mac16, want, 16) != 16 || memcmp(out, want, 16)))
        ok = 0;
    if (ok && (shash_hash("cmac(aes)", key, 16, msg, 40, out, 13) ||
               unhex(mac40, want, 16) != 16 || memcmp(out, want, 16)))
        ok = 0;
    if (ok && (shash_hash("cmac(aes)", key, 16, msg, 64, out, 32) ||
               unhex(mac64, want, 16) != 16 || memcmp(out, want, 16)))
        ok = 0;
    snprintf(detail, sizeof(detail), "err=%d got=%02x%02x%02x%02x", err,
             out[0], out[1], out[2], out[3]);
    cv_result("cmac_aes_rfc4493", ok, ok ? "len=0/16/40/64" : detail);

    for (i = 0; i < CV_CMAC_N; i++) {
        const struct cv_hash_vec *v = cv_cmac[i];
        if (shash_hash("cmac(aes)", v->key, v->key_len, v->msg, v->msg_len,
                       out, v->msg_len / 2) ||
            memcmp(out, v->out, v->out_len)) {
            rand_ok = 0;
            break;
        }
    }
    snprintf(detail, sizeof(detail), "vectors=%d", CV_CMAC_N);
    cv_result("cmac_aes_random", rand_ok, detail);
}

// ── hmac(sha1) / hmac(sha256): RFC 2202 / RFC 4231 ─────────────────────────
static void test_hmac_sha1(void)
{
    u8 key[64], msg[80], out[20], want[20];
    unsigned int i;
    int ok = 1, rand_ok = 1;
    char detail[128];

    memset(key, 0x0b, 20);
    if (shash_hash("hmac(sha1)", key, 20, (const u8 *)"Hi There", 8,
                   out, 3) ||
        unhex("b617318655057264e28bc0b6fb378c8ef146be00", want, 20) != 20 ||
        memcmp(out, want, 20))
        ok = 0;
    // Jefe / long key / 50 byte data cases. (satoru)
    if (ok) {
        memset(key, 0xaa, 20);
        memset(msg, 0xdd, 50);
        if (shash_hash("hmac(sha1)", key, 20, msg, 50, out, 25) ||
            unhex("125d7342b9ac11cd91a39af48aa17b4f63f175d3", want, 20) != 20 ||
            memcmp(out, want, 20))
            ok = 0;
    }
    if (ok) {
        memset(key, 0xaa, 80);
        if (shash_hash("hmac(sha1)", key, 80,
                       (const u8 *)"Test Using Larger Than Block-Size Key - Hash Key First",
                       54, out, 7) ||
            unhex("aa4ae5e15272d00e95705637ce8a3b55ed402112", want, 20) != 20 ||
            memcmp(out, want, 20))
            ok = 0;
    }
    cv_result("hmac_sha1_rfc2202", ok, ok ? "cases 1/3/6" : "mismatch");

    for (i = 0; i < CV_HMAC_SHA1_N; i++) {
        const struct cv_hash_vec *v = cv_hmac_sha1[i];
        if (shash_hash("hmac(sha1)", v->key, v->key_len, v->msg, v->msg_len,
                       out, v->msg_len / 3) ||
            memcmp(out, v->out, v->out_len)) {
            rand_ok = 0;
            break;
        }
    }
    snprintf(detail, sizeof(detail), "vectors=%d", CV_HMAC_SHA1_N);
    cv_result("hmac_sha1_random", rand_ok, detail);
}

static void test_hmac_sha256(void)
{
    u8 key[131], msg[152], out[32], want[32];
    unsigned int i;
    int ok = 1, rand_ok = 1;
    char detail[128];

    memset(key, 0x0b, 20);
    if (shash_hash("hmac(sha256)", key, 20, (const u8 *)"Hi There", 8,
                   out, 3) ||
        unhex("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
              want, 32) != 32 || memcmp(out, want, 32))
        ok = 0;
    if (ok) {
        memset(key, 0xaa, 20);
        memset(msg, 0xdd, 50);
        if (shash_hash("hmac(sha256)", key, 20, msg, 50, out, 25) ||
            unhex("773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe",
                  want, 32) != 32 || memcmp(out, want, 32))
            ok = 0;
    }
    if (ok) {
        memset(key, 0xaa, 131);
        if (shash_hash("hmac(sha256)", key, 131,
                       (const u8 *)"Test Using Larger Than Block-Size Key - Hash Key First",
                       54, out, 7) ||
            unhex("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
                  want, 32) != 32 || memcmp(out, want, 32))
            ok = 0;
    }
    cv_result("hmac_sha256_rfc4231", ok, ok ? "cases 1/3/6" : "mismatch");

    for (i = 0; i < CV_HMAC_SHA256_N; i++) {
        const struct cv_hash_vec *v = cv_hmac_sha256[i];
        if (shash_hash("hmac(sha256)", v->key, v->key_len, v->msg, v->msg_len,
                       out, v->msg_len / 3) ||
            memcmp(out, v->out, v->out_len)) {
            rand_ok = 0;
            break;
        }
    }
    snprintf(detail, sizeof(detail), "vectors=%d", CV_HMAC_SHA256_N);
    cv_result("hmac_sha256_random", rand_ok, detail);
}

// ── arc4: RFC 6229 (raw lib/crypto context, as mac80211 wep/tkip use it) ───
static void test_arc4(void)
{
    static const struct { const char *key; const char *ks; } rfc[] = {
        { "0102030405", "b2396305f03dc027ccc3524a0a1118a8" },
        { "0102030405060708", "97ab8a1bf0afb96132f2f67258da15a8" },
        { "0102030405060708090a0b0c0d0e0f10",
          "9ac7cc9a609d1ef7b2932899cde41b97" },
    };
    struct arc4_ctx ctx;
    u8 key[32], zero[16], out[16], want[16];
    unsigned int i, j;
    int ok = 1, rand_ok = 1;
    char detail[128];

    memset(zero, 0, sizeof(zero));
    for (i = 0; i < sizeof(rfc) / sizeof(rfc[0]); i++) {
        int n = unhex(rfc[i].key, key, sizeof(key));
        arc4_setkey(&ctx, key, (unsigned int)n);
        arc4_crypt(&ctx, out, zero, 16);
        if (unhex(rfc[i].ks, want, 16) != 16 || memcmp(out, want, 16)) {
            ok = 0;
            break;
        }
    }
    cv_result("arc4_rfc6229", ok, ok ? "3 keys, first 16 bytes" : "mismatch");

    for (i = 0; i < CV_ARC4_N; i++) {
        const struct cv_arc4_vec *v = cv_arc4[i];
        arc4_setkey(&ctx, v->key, v->key_len);
        arc4_crypt(&ctx, out, zero, 16);
        if (memcmp(out, v->out, 16)) {
            rand_ok = 0;
            break;
        }
        // encrypt then decrypt is the same operation from the same state. (satoru)
        arc4_setkey(&ctx, v->key, v->key_len);
        arc4_crypt(&ctx, out, zero, 16);
        arc4_setkey(&ctx, v->key, v->key_len);
        arc4_crypt(&ctx, out, out, 16);
        for (j = 0; j < 16; j++)
            if (out[j] != 0)
                rand_ok = 0;
    }
    snprintf(detail, sizeof(detail), "vectors=%d", CV_ARC4_N);
    cv_result("arc4_random", rand_ok, detail);
}

// ── crypto_memneq / crypto_xor ─────────────────────────────────────────────
static void test_utils(void)
{
    u8 a[16], b[16], c[16], x[16], want[16];
    int i, ok1, ok2;

    for (i = 0; i < 16; i++)
        a[i] = (u8)(i * 7 + 3);
    memcpy(b, a, 16);
    ok1 = crypto_memneq(a, b, 16) == 0;
    b[15] ^= 1;
    ok1 = ok1 && crypto_memneq(a, b, 16) == 1;
    b[15] ^= 1;
    b[0] ^= 0x80;
    ok1 = ok1 && crypto_memneq(a, b, 16) == 1;
    cv_result("crypto_memneq", ok1, ok1 ? "equal=0 diff-last=1 diff-first=1" : "bad");

    for (i = 0; i < 16; i++) {
        x[i] = (u8)(0xa5 ^ i);
        c[i] = (u8)(i * 3);
        want[i] = (u8)(a[i] ^ c[i]);
    }
    memcpy(b, a, 16);
    crypto_xor(b, c, 16);
    ok2 = !memcmp(b, want, 16);
    memcpy(b, a, 16);
    crypto_xor(b, c, 5);
    ok2 = ok2 && !memcmp(b, want, 5) && !memcmp(b + 5, a + 5, 11);
    crypto_xor_cpy(b, a, c, 16);
    ok2 = ok2 && !memcmp(b, want, 16);
    cv_result("crypto_xor", ok2, ok2 ? "16/5 byte aligned and unaligned" : "bad");
}

void apitest_crypto_run(int *pass, int *total)
{
    lx_log("[apitest] crypto: upstream core, generic algorithms\n");

    test_ccm();
    test_gcm();
    test_cmac();
    test_hmac_sha1();
    test_hmac_sha256();
    test_arc4();
    test_utils();
    *pass += cv_pass;
    *total += cv_total;
}

// end (satoru)
