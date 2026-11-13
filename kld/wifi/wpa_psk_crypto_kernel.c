// wpa_psk_crypto_kernel: the six wpa_psk_crypto primitives on top of the
// kernel crypto API compiled into the container (CRYPTOLIB in kld/Makefile).
//
// this is glue, not a second crypto implementation: hmac and cmac come from
// the unmodified crypto/shash.c and its templates, aes key unwrap uses the
// unmodified lib/crypto/aes.c block functions as 802.11-2020 12.7.2 requires.
// (satoru)

#include <linux/types.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <crypto/hash.h>
#include <crypto/aes.h>

#include "wpa_psk_crypto.h"

static int shash_oneshot(const char *alg, const u8 *key, unsigned int key_len,
                         const u8 *data, unsigned int data_len, u8 *out,
                         unsigned int out_len)
{
    struct crypto_shash *tfm;
    SHASH_DESC_ON_STACK(desc, tfm);
    int err;

    tfm = crypto_alloc_shash(alg, 0, 0);
    if (IS_ERR(tfm))
        return PTR_ERR(tfm);
    err = crypto_shash_setkey(tfm, key, key_len);
    if (err)
        goto out;
    desc->tfm = tfm;
    err = crypto_shash_digest(desc, data, data_len, out);
    if (!err && crypto_shash_digestsize(tfm) < out_len)
        err = -EINVAL;
out:
    crypto_free_shash(tfm);
    return err;
}

int wpa_crypto_pbkdf2_sha1(const u8 *pass, unsigned int pass_len,
                           const u8 *salt, unsigned int salt_len,
                           unsigned int iterations, u8 *out,
                           unsigned int out_len)
{
    struct crypto_shash *tfm;
    struct shash_desc *desc;
    u8 msg[64];
    u8 u[20], t[20];
    unsigned int done = 0, block = 1;
    int err = 0;

    if (!pass || !salt || !out || iterations == 0 || salt_len > 60)
        return -EINVAL;
    tfm = crypto_alloc_shash("hmac(sha1)", 0, 0);
    if (IS_ERR(tfm))
        return PTR_ERR(tfm);
    err = crypto_shash_setkey(tfm, pass, pass_len);
    if (err)
        goto out_tfm;
    desc = kzalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
    if (!desc) {
        err = -ENOMEM;
        goto out_tfm;
    }
    desc->tfm = tfm;
    while (done < out_len) {
        unsigned int i, n = out_len - done;
        if (n > sizeof(t))
            n = sizeof(t);
        memcpy(msg, salt, salt_len);
        msg[salt_len] = (u8)(block >> 24);
        msg[salt_len + 1] = (u8)(block >> 16);
        msg[salt_len + 2] = (u8)(block >> 8);
        msg[salt_len + 3] = (u8)block;
        err = crypto_shash_digest(desc, msg, salt_len + 4, u);
        if (err)
            break;
        memcpy(t, u, sizeof(t));
        // 802.11-2020 12.7.1.2 pbkdf2: t = u1 xor u2 ... xor u4096.
        for (i = 1; i < iterations; i++) {
            err = crypto_shash_digest(desc, u, sizeof(u), u);
            if (err)
                break;
            for (unsigned int j = 0; j < sizeof(t); j++)
                t[j] ^= u[j];
        }
        if (err)
            break;
        memcpy(out + done, t, n);
        done += n;
        block++;
    }
    memzero_explicit(u, sizeof(u));
    memzero_explicit(t, sizeof(t));
    memzero_explicit(msg, sizeof(msg));
    kfree_sensitive(desc);
out_tfm:
    crypto_free_shash(tfm);
    return err;
}

int wpa_crypto_hmac_sha1(const u8 *key, unsigned int key_len,
                         const u8 *data, unsigned int data_len, u8 out[20])
{
    return shash_oneshot("hmac(sha1)", key, key_len, data, data_len, out, 20);
}

int wpa_crypto_hmac_sha256(const u8 *key, unsigned int key_len,
                           const u8 *data, unsigned int data_len, u8 out[32])
{
    return shash_oneshot("hmac(sha256)", key, key_len, data, data_len, out, 32);
}

int wpa_crypto_cmac_aes(const u8 *key, unsigned int key_len,
                        const u8 *data, unsigned int data_len, u8 out[16])
{
    return shash_oneshot("cmac(aes)", key, key_len, data, data_len, out, 16);
}

// RFC 3394 aes key unwrap, 128 bit kek. 802.11-2020 12.7.2 uses it to carry
// the key data of message 3/4 and of the group key handshake. (satoru)
int wpa_crypto_aes_unwrap(const u8 kek[16], const u8 *in, unsigned int in_len,
                          u8 *out)
{
    struct crypto_aes_ctx ctx;
    u8 a[8], b[16];
    unsigned int n, i, t;
    int j;
    u8 diff = 0;

    if (!kek || !in || !out || in_len < 24 || (in_len & 7))
        return -EINVAL;
    if (aes_expandkey(&ctx, kek, 16))
        return -EINVAL;
    n = in_len / 8 - 1;
    memcpy(a, in, 8);
    memcpy(out, in + 8, in_len - 8);
    for (j = 5; j >= 0; j--) {
        for (i = n; i >= 1; i--) {
            t = n * j + i;
            memcpy(b, a, 8);
            b[4] ^= (u8)(t >> 24);
            b[5] ^= (u8)(t >> 16);
            b[6] ^= (u8)(t >> 8);
            b[7] ^= (u8)t;
            memcpy(b + 8, out + 8 * (i - 1), 8);
            aes_decrypt(&ctx, b, b);
            memcpy(a, b, 8);
            memcpy(out + 8 * (i - 1), b + 8, 8);
        }
    }
    for (i = 0; i < 8; i++)
        diff |= (u8)(a[i] ^ 0xa6);
    memzero_explicit(&ctx, sizeof(ctx));
    memzero_explicit(a, sizeof(a));
    memzero_explicit(b, sizeof(b));
    return diff == 0 ? (int)(in_len - 8) : -EBADMSG;
}

void wpa_crypto_random_bytes(void *buf, unsigned int len)
{
    get_random_bytes(buf, len);
}

// end (satoru)
