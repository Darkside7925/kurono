// lx_emul: pure-computation helpers and diagnostics.
//
// Everything in here is real, complete, and stateless - no device interaction,
// so there is no reason to stub any of it. These are the cheapest symbols in the
// census to retire.
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/bitmap.h>
#include <linux/crc32.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/uaccess.h>
#include <linux/printk.h>

#include "lx_kshim.h"

extern void *memcpy(void *, const void *, unsigned long);
extern unsigned long strlen(const char *);
extern void *malloc(unsigned long);

// ── strings ─────────────────────────────────────────────────────────────────
// strscpy semantics: NUL-terminate, return the length copied, or -E2BIG on
// truncation. Getting the return convention wrong here silently truncates
// device names, so it is worth being exact. (satoru)
ssize_t sized_strscpy(char *dst, const char *src, size_t count)
{
    size_t i;
    if (count == 0)
        return -E2BIG;
    for (i = 0; i < count - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
    return src[i] ? -E2BIG : (ssize_t)i;
}

char *kstrdup(const char *s, gfp_t gfp)
{
    size_t n;
    char *p;
    (void)gfp;
    if (!s)
        return NULL;
    n = strlen(s) + 1;
    p = (char *)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

// ── bitmaps ─────────────────────────────────────────────────────────────────
bool __bitmap_andnot(unsigned long *dst, const unsigned long *src1,
                     const unsigned long *src2, unsigned int nbits)
{
    unsigned int k, lim = nbits / BITS_PER_LONG;
    unsigned long result = 0;

    for (k = 0; k < lim; k++)
        result |= (dst[k] = src1[k] & ~src2[k]);
    if (nbits % BITS_PER_LONG)
        result |= (dst[k] = src1[k] & ~src2[k] & BITMAP_LAST_WORD_MASK(nbits));
    return result != 0;
}

unsigned long _find_next_bit(const unsigned long *addr, unsigned long nbits,
                             unsigned long start)
{
    unsigned long i;
    for (i = start; i < nbits; i++)
        if (addr[i / BITS_PER_LONG] & (1UL << (i % BITS_PER_LONG)))
            return i;
    return nbits;
}

// ── crc32 ───────────────────────────────────────────────────────────────────
// bitwise reflected CRC-32 (IEEE 802.3), table-free. e1000e uses it for the
// multicast hash, so correctness is observable as working multicast. (satoru)
u32 __pure crc32_le(u32 crc, unsigned char const *p, size_t len)
{
    int i;
    while (len--) {
        crc ^= *p++;
        for (i = 0; i < 8; i++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
    }
    return crc;
}

// ── checksums ───────────────────────────────────────────────────────────────
// only reached on the IPv6 TX-offload path. Folding is standard ones-complement.
__sum16 csum_ipv6_magic(const struct in6_addr *saddr, const struct in6_addr *daddr,
                        __u32 len, __u8 proto, __wsum csum)
{
    u64 sum = (u64)csum;
    int i;
    for (i = 0; i < 4; i++) sum += (u64)saddr->s6_addr32[i];
    for (i = 0; i < 4; i++) sum += (u64)daddr->s6_addr32[i];
    sum += (u64)htonl(len);
    sum += (u64)htonl(proto);
    while (sum >> 32)
        sum = (sum & 0xffffffff) + (sum >> 32);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (__sum16)~sum;
}

// ── ethtool link-mode conversion ────────────────────────────────────────────
void ethtool_convert_legacy_u32_to_link_mode(unsigned long *dst, u32 legacy)
{
    bitmap_zero(dst, __ETHTOOL_LINK_MODE_MASK_NBITS);
    dst[0] = legacy;
}

bool ethtool_convert_link_mode_to_legacy_u32(u32 *legacy, const unsigned long *src)
{
    bool retval = true;
    *legacy = (u32)src[0];
    // report whether any bit above 32 was dropped, like upstream does
    if (__ETHTOOL_LINK_MODE_MASK_NBITS > 32) {
        int i;
        for (i = 1; i < BITS_TO_LONGS(__ETHTOOL_LINK_MODE_MASK_NBITS); i++)
            if (src[i])
                retval = false;
    }
    return retval;
}

u32 ethtool_op_get_link(struct net_device *dev)
{
    return netif_carrier_ok(dev) ? 1 : 0;
}

int ethtool_op_get_ts_info(struct net_device *dev, struct kernel_ethtool_ts_info *info)
{
    (void)dev;
    info->so_timestamping = 0;
    info->phc_index = -1;
    info->tx_types = 0;
    info->rx_filters = 0;
    return 0;
}

// ── ethernet helpers ────────────────────────────────────────────────────────
int eth_validate_addr(struct net_device *dev)
{
    return is_valid_ether_addr(dev->dev_addr) ? 0 : -EADDRNOTAVAIL;
}

// ── diagnostics ─────────────────────────────────────────────────────────────
int net_ratelimit(void)
{
    return 1;   // never suppress: a container's log volume is not a problem
}

void __warn_printk(const char *fmt, ...)
{
    (void)fmt;
    lx_log("[lx_emul] WARN from driver (see preceding context)\n");
}

void refcount_warn_saturate(refcount_t *r, enum refcount_saturation_type t)
{
    (void)r; (void)t;
    lx_log("[lx_emul] refcount saturated - a real refcount bug in the shim\n");
}

void print_hex_dump(const char *level, const char *prefix_str, int prefix_type,
                    int rowsize, int groupsize, const void *buf, size_t len,
                    bool ascii)
{
    (void)level; (void)prefix_type; (void)rowsize; (void)groupsize; (void)ascii;
    lx_log("[lx_emul] hexdump ");
    lx_log(prefix_str ? prefix_str : "");
    lx_log(" len=");
    lx_log_u((unsigned long)len);
    lx_log(" (contents omitted)\n");
    (void)buf;
}

// ── user copies ─────────────────────────────────────────────────────────────
// Reached only from the ethtool ioctl path, which nothing in the container
// drives yet. A container and its caller share no address space boundary here,
// so a straight copy is the right semantic once ioctls are wired. (satoru)
unsigned long _copy_from_user(void *to, const void __user *from, unsigned long n)
{
    memcpy(to, (const void *)from, n);
    return 0;
}

unsigned long _copy_to_user(void __user *to, const void *from, unsigned long n)
{
    memcpy((void *)to, from, n);
    return 0;
}

// end (satoru)
