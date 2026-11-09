// lx_emul: net_device, sk_buff, NAPI, and the bridge into Kurono's eth0.
//
// What is faithful and what is not, stated plainly:
//  - struct net_device / sk_buff / napi_struct are the REAL 6.12 layouts (this
//    file is compiled against the configured headers), and every inline helper
//    the driver uses (skb_reserve, skb_put_data, netif_tx_stop_queue, BQL's
//    netdev_tx_sent_queue, napi_schedule) runs unmodified upstream code on them.
//  - the out-of-line pieces are reimplemented here following the upstream bit
//    protocols (NAPI SCHED/MISSED/DISABLE/NPSVC, queue XOFF bits, skb data
//    layout with the shared info at end). BQL itself is the imported upstream
//    lib/dynamic_queue_limits.c, not a rewrite.
//  - there is no qdisc, no GRO merging (napi_gro_receive delivers each skb as
//    GRO_NORMAL), no RPS/XPS, no netlink, no sysfs.
//
// Data path:
//  TX  Kurono's TCP/IP stack -> kernel copies the frame into a TX slot ->
//      futex-wakes kld-xmit here -> skb -> ndo_start_xmit under the tx lock.
//  RX  driver's napi poll -> napi_gro_receive -> RX slot -> after the poll round
//      one NET_RX_KICK makes the kernel drain the ring into TCPStack.
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/if_ether.h>
#include <linux/dynamic_queue_limits.h>
#include <linux/rtnetlink.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <net/gro.h>

#include "lx_kshim.h"
#include "lx_broker.h"

// ── skb core ────────────────────────────────────────────────────────────────
struct sk_buff *__alloc_skb(unsigned int size, gfp_t gfp_mask, int flags, int node)
{
    (void)flags; (void)node;
    struct sk_buff *skb = kmalloc(sizeof(*skb), gfp_mask & ~__GFP_ZERO);
    if (!skb)
        return NULL;
    memset(skb, 0, sizeof(*skb));
    size = SKB_DATA_ALIGN(size);
    u8 *data = kmalloc(size + SKB_DATA_ALIGN(sizeof(struct skb_shared_info)), gfp_mask & ~__GFP_ZERO);
    if (!data) {
        kfree(skb);
        return NULL;
    }
    skb->head = data;
    skb->data = data;
    skb_reset_tail_pointer(skb);
    skb_set_end_offset(skb, size);
    skb->mac_header = (typeof(skb->mac_header))~0U;
    skb->transport_header = (typeof(skb->transport_header))~0U;
    skb->truesize = SKB_TRUESIZE(size);
    refcount_set(&skb->users, 1);
    struct skb_shared_info *sh = skb_shinfo(skb);
    memset(sh, 0, offsetof(struct skb_shared_info, dataref));
    atomic_set(&sh->dataref, 1);
    return skb;
}

static void lx_skb_free(struct sk_buff *skb)
{
    struct skb_shared_info *sh = skb_shinfo(skb);
    for (int i = 0; i < sh->nr_frags; i++)
        put_page(skb_frag_page(&sh->frags[i]));
    kfree(skb->head);
    kfree(skb);
}

void __kfree_skb(struct sk_buff *skb) { lx_skb_free(skb); }

void sk_skb_reason_drop(struct sock *sk, struct sk_buff *skb, enum skb_drop_reason reason)
{
    (void)sk; (void)reason;
    if (skb && skb_unref(skb))
        lx_skb_free(skb);
}

void consume_skb(struct sk_buff *skb)
{
    if (skb && skb_unref(skb))
        lx_skb_free(skb);
}

void dev_kfree_skb_any_reason(struct sk_buff *skb, enum skb_drop_reason reason)
{
    (void)reason;
    consume_skb(skb);
}

void dev_kfree_skb_irq_reason(struct sk_buff *skb, enum skb_drop_reason reason)
{
    (void)reason;
    consume_skb(skb);
}

void *skb_put(struct sk_buff *skb, unsigned int len)
{
    void *tmp = skb_tail_pointer(skb);
    if (skb->data_len) {
        lx_log("[lx_net] skb_put on a nonlinear skb\n");
        return tmp;
    }
    skb->tail += len;
    skb->len  += len;
    if (unlikely(skb->tail > skb->end))
        lx_unimpl("skb_put: over panic (tail past end)");
    return tmp;
}

void *skb_push(struct sk_buff *skb, unsigned int len)
{
    skb->data -= len;
    skb->len  += len;
    if (unlikely(skb->data < skb->head))
        lx_unimpl("skb_push: under panic");
    return skb->data;
}

void *skb_pull(struct sk_buff *skb, unsigned int len)
{
    return skb_pull_inline(skb, len);
}

void skb_trim(struct sk_buff *skb, unsigned int len)
{
    if (skb->len > len)
        __skb_trim(skb, len);
}

int ___pskb_trim(struct sk_buff *skb, unsigned int len)
{
    if (skb->data_len)
        return -ENOMEM;   // nonlinear trim not needed by the supported drivers (satoru)
    __skb_trim(skb, len);
    return 0;
}

int pskb_expand_head(struct sk_buff *skb, int nhead, int ntail, gfp_t gfp_mask)
{
    unsigned int osize = skb_end_offset(skb);
    unsigned int size = SKB_DATA_ALIGN(osize + nhead + ntail);
    if (skb_shared(skb))
        lx_unimpl("pskb_expand_head on a shared skb");
    u8 *data = kmalloc(size + SKB_DATA_ALIGN(sizeof(struct skb_shared_info)), gfp_mask);
    if (!data)
        return -ENOMEM;
    // linear bytes from head to tail, plus the shared info (frag list). (satoru)
    memcpy(data + nhead, skb->head, skb_tail_pointer(skb) - skb->head);
    memcpy((struct skb_shared_info *)(data + size), skb_shinfo(skb),
           offsetof(struct skb_shared_info, frags[skb_shinfo(skb)->nr_frags]));
    long off = (data + nhead) - skb->head;
    kfree(skb->head);
    skb->head = data;
    skb->data += off;
    skb_set_end_offset(skb, size);
    skb->tail += nhead;
    if (skb_mac_header_was_set(skb))
        skb->mac_header += nhead;
    skb->network_header += nhead;
    if (skb_transport_header_was_set(skb))
        skb->transport_header += nhead;
    skb->cloned = 0;
    skb->nohdr = 0;
    atomic_set(&skb_shinfo(skb)->dataref, 1);
    return 0;
}

int __skb_pad(struct sk_buff *skb, int pad, bool free_on_error)
{
    int err, ntail;
    if (!skb_cloned(skb) && skb_tailroom(skb) >= pad) {
        memset(skb->data + skb->len, 0, pad);
        return 0;
    }
    ntail = skb->data_len + pad - (skb->end - skb->tail);
    if (likely(skb_cloned(skb) || ntail > 0)) {
        err = pskb_expand_head(skb, 0, ntail, GFP_ATOMIC);
        if (unlikely(err))
            goto free_skb;
    }
    if (skb->data_len) {
        err = -ENOMEM;
        goto free_skb;
    }
    memset(skb->data + skb->len, 0, pad);
    return 0;
free_skb:
    if (free_on_error)
        kfree_skb(skb);
    return err;
}

int skb_copy_bits(const struct sk_buff *skb, int offset, void *to, int len)
{
    int start = skb_headlen(skb);
    int copy;
    if (offset > (int)skb->len - len)
        return -EFAULT;
    if ((copy = start - offset) > 0) {
        if (copy > len)
            copy = len;
        memcpy(to, skb->data + offset, copy);
        if ((len -= copy) == 0)
            return 0;
        offset += copy;
        to += copy;
    }
    for (int i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
        const skb_frag_t *f = &skb_shinfo(skb)->frags[i];
        int end = start + skb_frag_size(f);
        if ((copy = end - offset) > 0) {
            if (copy > len)
                copy = len;
            memcpy(to, (u8 *)skb_frag_address(f) + (offset - start), copy);
            if ((len -= copy) == 0)
                return 0;
            offset += copy;
            to += copy;
        }
        start = end;
    }
    return len ? -EFAULT : 0;
}

void *__pskb_pull_tail(struct sk_buff *skb, int delta)
{
    if (!skb->data_len)
        return skb_tail_pointer(skb);
    // pulling paged data into the head: the supported drivers only ever see
    // linear skbs from this shim's TX path, so this is reported, not faked.
    lx_log("[lx_net] __pskb_pull_tail on a nonlinear skb: not supported, dropping\n");
    (void)delta;
    return NULL;
}

void skb_tstamp_tx(struct sk_buff *orig_skb, struct skb_shared_hwtstamps *hwtstamps)
{
    // no socket error queue to deliver a tx timestamp to. (satoru)
    (void)orig_skb; (void)hwtstamps;
}

struct sk_buff *__netdev_alloc_skb(struct net_device *dev, unsigned int len, gfp_t gfp_mask)
{
    struct sk_buff *skb = __alloc_skb(len + NET_SKB_PAD, gfp_mask, 0, NUMA_NO_NODE);
    if (!skb)
        return NULL;
    skb_reserve(skb, NET_SKB_PAD);
    skb->dev = dev;
    return skb;
}

struct sk_buff *napi_alloc_skb(struct napi_struct *napi, unsigned int len)
{
    struct sk_buff *skb = __netdev_alloc_skb(napi ? napi->dev : NULL, len + NET_IP_ALIGN, GFP_ATOMIC);
    if (skb)
        skb_reserve(skb, NET_IP_ALIGN);
    return skb;
}

__be16 eth_type_trans(struct sk_buff *skb, struct net_device *dev)
{
    const struct ethhdr *eth;
    skb->dev = dev;
    skb_reset_mac_header(skb);
    eth = (struct ethhdr *)skb->data;
    skb_pull_inline(skb, ETH_HLEN);
    if (unlikely(!ether_addr_equal_64bits(eth->h_dest, dev->dev_addr))) {
        if (unlikely(is_multicast_ether_addr_64bits(eth->h_dest))) {
            if (ether_addr_equal_64bits(eth->h_dest, dev->broadcast))
                skb->pkt_type = PACKET_BROADCAST;
            else
                skb->pkt_type = PACKET_MULTICAST;
        } else {
            skb->pkt_type = PACKET_OTHERHOST;
        }
    }
    if (likely(eth_proto_is_802_3(eth->h_proto)))
        return eth->h_proto;
    return htons(ETH_P_802_2);
}

// ── the bridge state ────────────────────────────────────────────────────────
#define LX_MAX_NETDEV 2

static struct net_device *lx_netdevs[LX_MAX_NETDEV];
static int lx_netdev_count;
static struct lx_net_ring *lx_ring;
static unsigned char *lx_ring_tx, *lx_ring_rx;
static struct net_device *lx_bridged;
static volatile int lx_rx_pending;
static unsigned long lx_rx_frames, lx_rx_dropped, lx_tx_frames, lx_tx_busy;
static volatile unsigned long lx_xmit_loops, lx_xmit_waits;   // heartbeat (satoru)

static void lx_xmit_kick(void)
{
    if (!lx_ring)
        return;
    __atomic_fetch_add(&lx_ring->tx_doorbell, 1, __ATOMIC_RELEASE);
    lx_os_futex_wake(&lx_ring->tx_doorbell, 1);
}

static void lx_rx_kick(void)
{
    if (__atomic_exchange_n(&lx_rx_pending, 0, __ATOMIC_ACQ_REL))
        lx_broker_call(LX_KLD_OP_NET_RX_KICK, 0, 0, 0, 0);
}

// copy one received frame (mac header onward) into the next RX slot. (satoru)
static void lx_rx_deliver(struct sk_buff *skb)
{
    if (!lx_ring || skb->dev != lx_bridged) {
        consume_skb(skb);
        return;
    }
    unsigned char *start = skb_mac_header_was_set(skb) ? skb_mac_header(skb) : skb->data;
    unsigned int hdr = (unsigned int)(skb->data - start);
    unsigned int len = hdr + skb->len;
    unsigned int prod = lx_ring->rx_prod;
    unsigned int cons = __atomic_load_n(&lx_ring->rx_cons, __ATOMIC_ACQUIRE);
    if (len > LX_NET_SLOT_BYTES || prod - cons >= LX_NET_RX_SLOTS) {
        lx_ring->rx_drops++;
        lx_rx_dropped++;
        consume_skb(skb);
        return;
    }
    unsigned int slot = prod % LX_NET_RX_SLOTS;
    unsigned char *dst = lx_ring_rx + slot * LX_NET_SLOT_BYTES;
    memcpy(dst, start, hdr);
    if (skb_copy_bits(skb, 0, dst + hdr, (int)skb->len)) {
        consume_skb(skb);
        return;
    }
    lx_ring->rx_len[slot] = (unsigned short)len;
    __atomic_store_n(&lx_ring->rx_prod, prod + 1, __ATOMIC_RELEASE);
    lx_rx_frames++;
    lx_rx_pending = 1;
    skb->dev->stats.rx_packets++;
    consume_skb(skb);
}

gro_result_t napi_gro_receive(struct napi_struct *napi, struct sk_buff *skb)
{
    (void)napi;
    lx_rx_deliver(skb);
    return GRO_NORMAL;
}

int netif_receive_skb(struct sk_buff *skb)
{
    lx_rx_deliver(skb);
    return NET_RX_SUCCESS;
}

int netif_rx(struct sk_buff *skb)
{
    lx_rx_deliver(skb);
    lx_rx_kick();
    return NET_RX_SUCCESS;
}

// ── NAPI ────────────────────────────────────────────────────────────────────
static volatile unsigned int lx_napi_lock;
static LIST_HEAD(lx_napi_list);
static volatile unsigned int lx_napi_kick;
static struct lx_task *lx_napi_task;

static int lx_napi_thread(void *arg);

void netif_napi_add_weight(struct net_device *dev, struct napi_struct *napi,
                           int (*poll)(struct napi_struct *, int), int weight)
{
    INIT_LIST_HEAD(&napi->poll_list);
    INIT_LIST_HEAD(&napi->rx_list);
    napi->skb = NULL;
    napi->timer.function = NULL;
    napi->gro_bitmask = 0;
    napi->poll = poll;
    napi->weight = weight;
    napi->dev = dev;
    napi->list_owner = -1;
    // upstream: a fresh napi is "scheduled + not servicing" so nothing polls it
    // until napi_enable(). (satoru)
    set_bit(NAPI_STATE_SCHED, &napi->state);
    set_bit(NAPI_STATE_NPSVC, &napi->state);
    list_add(&napi->dev_list, &dev->napi_list);
    if (!lx_napi_task)
        lx_napi_task = lx_task_spawn("kld-napi", lx_napi_thread, NULL);
}

void napi_enable(struct napi_struct *n)
{
    unsigned long new, val = READ_ONCE(n->state);
    do {
        new = val & ~(NAPIF_STATE_SCHED | NAPIF_STATE_NPSVC);
    } while (!try_cmpxchg(&n->state, &val, new));
}

void napi_disable(struct napi_struct *n)
{
    unsigned long val, new;
    set_bit(NAPI_STATE_DISABLE, &n->state);
    val = READ_ONCE(n->state);
    do {
        while (val & (NAPIF_STATE_SCHED | NAPIF_STATE_NPSVC)) {
            lx_os_sleep_ns(100000);
            val = READ_ONCE(n->state);
        }
        new = val | NAPIF_STATE_SCHED | NAPIF_STATE_NPSVC;
        new &= ~(NAPIF_STATE_THREADED | NAPIF_STATE_PREFER_BUSY_POLL);
    } while (!try_cmpxchg(&n->state, &val, new));
    clear_bit(NAPI_STATE_DISABLE, &n->state);
}

bool napi_schedule_prep(struct napi_struct *n)
{
    unsigned long new, val = READ_ONCE(n->state);
    do {
        if (unlikely(val & NAPIF_STATE_DISABLE))
            return false;
        new = val | NAPIF_STATE_SCHED;
        new |= (val & NAPIF_STATE_SCHED) / NAPIF_STATE_SCHED * NAPIF_STATE_MISSED;
    } while (!try_cmpxchg(&n->state, &val, new));
    return !(val & NAPIF_STATE_SCHED);
}

void __napi_schedule(struct napi_struct *n)
{
    lx_flock(&lx_napi_lock);
    if (list_empty(&n->poll_list))
        list_add_tail(&n->poll_list, &lx_napi_list);
    lx_funlock(&lx_napi_lock);
    __atomic_fetch_add(&lx_napi_kick, 1, __ATOMIC_RELEASE);
    lx_os_futex_wake(&lx_napi_kick, 1);
}

void __napi_schedule_irqoff(struct napi_struct *n) { __napi_schedule(n); }

bool napi_complete_done(struct napi_struct *n, int work_done)
{
    unsigned long val, new;
    (void)work_done;
    if (unlikely(n->state & (NAPIF_STATE_NPSVC | NAPIF_STATE_IN_BUSY_POLL)))
        return false;
    lx_flock(&lx_napi_lock);
    if (!list_empty(&n->poll_list))
        list_del_init(&n->poll_list);
    lx_funlock(&lx_napi_lock);
    WRITE_ONCE(n->list_owner, -1);
    val = READ_ONCE(n->state);
    do {
        new = val & ~(NAPIF_STATE_MISSED | NAPIF_STATE_SCHED |
                      NAPIF_STATE_SCHED_THREADED | NAPIF_STATE_PREFER_BUSY_POLL);
        new |= (val & NAPIF_STATE_MISSED) / NAPIF_STATE_MISSED * NAPIF_STATE_SCHED;
    } while (!try_cmpxchg(&n->state, &val, new));
    if (unlikely(val & NAPIF_STATE_MISSED)) {
        __napi_schedule(n);
        return false;
    }
    return true;
}

static int lx_napi_thread(void *arg)
{
    (void)arg;
    for (;;) {
        unsigned int kick = __atomic_load_n(&lx_napi_kick, __ATOMIC_ACQUIRE);
        struct napi_struct *n = NULL;
        lx_flock(&lx_napi_lock);
        if (!list_empty(&lx_napi_list)) {
            n = list_first_entry(&lx_napi_list, struct napi_struct, poll_list);
            list_del_init(&n->poll_list);
        }
        lx_funlock(&lx_napi_lock);
        if (!n) {
            lx_rx_kick();
            lx_os_futex_wait(&lx_napi_kick, kick, -1);
            continue;
        }
        int work = n->poll(n, n->weight);
        // upstream __napi_poll: a poll that used its whole budget did NOT
        // complete, so it stays scheduled and is polled again. (satoru)
        if (work >= n->weight && test_bit(NAPI_STATE_SCHED, &n->state)) {
            lx_flock(&lx_napi_lock);
            if (list_empty(&n->poll_list))
                list_add_tail(&n->poll_list, &lx_napi_list);
            lx_funlock(&lx_napi_lock);
        }
        lx_rx_kick();
    }
    return 0;
}

// ── tx queues ───────────────────────────────────────────────────────────────
void netif_tx_wake_queue(struct netdev_queue *dev_queue)
{
    if (test_and_clear_bit(__QUEUE_STATE_DRV_XOFF, &dev_queue->state))
        lx_xmit_kick();
}

void netif_schedule_queue(struct netdev_queue *txq)
{
    if (!(txq->state & QUEUE_STATE_ANY_XOFF))
        lx_xmit_kick();
}

unsigned long dev_trans_start(struct net_device *dev)
{
    unsigned long res = READ_ONCE(netdev_get_tx_queue(dev, 0)->trans_start);
    for (unsigned int i = 1; i < dev->num_tx_queues; i++) {
        unsigned long v = READ_ONCE(netdev_get_tx_queue(dev, i)->trans_start);
        if (v && time_after(v, res))
            res = v;
    }
    return res;
}

netdev_features_t passthru_features_check(struct sk_buff *skb, struct net_device *dev,
                                          netdev_features_t features)
{
    (void)skb; (void)dev;
    return features;
}

static int lx_xmit_thread(void *arg)
{
    struct net_device *dev = arg;
    struct netdev_queue *txq = netdev_get_tx_queue(dev, 0);
    for (;;) {
        lx_xmit_loops++;
        unsigned int bell = __atomic_load_n(&lx_ring->tx_doorbell, __ATOMIC_ACQUIRE);
        unsigned int cons = lx_ring->tx_cons;
        unsigned int prod = __atomic_load_n(&lx_ring->tx_prod, __ATOMIC_ACQUIRE);
        if (cons == prod || netif_xmit_frozen_or_stopped(txq) ||
            !test_bit(__LINK_STATE_START, &dev->state)) {
            lx_xmit_waits++;
            lx_os_futex_wait(&lx_ring->tx_doorbell, bell, -1);
            continue;
        }
        unsigned int slot = cons % LX_NET_TX_SLOTS;
        unsigned int len = lx_ring->tx_len[slot];
        if (len < ETH_HLEN || len > LX_NET_SLOT_BYTES) {
            __atomic_store_n(&lx_ring->tx_cons, cons + 1, __ATOMIC_RELEASE);
            continue;
        }
        struct sk_buff *skb = __netdev_alloc_skb(dev, len, GFP_KERNEL);
        if (!skb) {
            lx_os_sleep_ns(1000000);
            continue;
        }
        unsigned char *src = lx_ring_tx + slot * LX_NET_SLOT_BYTES;
        skb_put_data(skb, src, len);
        skb_reset_mac_header(skb);
        skb_set_network_header(skb, ETH_HLEN);
        skb->protocol = ((struct ethhdr *)skb->data)->h_proto;
        skb->ip_summed = CHECKSUM_NONE;   // kurono's stack computed every checksum (satoru)
        skb_set_queue_mapping(skb, 0);

        __netif_tx_lock(txq, 0);
        netdev_tx_t rc = NETDEV_TX_BUSY;
        if (!netif_xmit_frozen_or_stopped(txq))
            rc = dev->netdev_ops->ndo_start_xmit(skb, dev);
        __netif_tx_unlock(txq);

        if (rc == NETDEV_TX_OK) {
            txq_trans_cond_update(txq);
            __atomic_store_n(&lx_ring->tx_cons, cons + 1, __ATOMIC_RELEASE);
            lx_tx_frames++;
        } else {
            // the driver did not take it: keep the slot and retry once the
            // queue is woken. upstream requeues the skb on the qdisc. (satoru)
            consume_skb(skb);
            lx_tx_busy++;
        }
    }
    return 0;
}

// ── net_device lifecycle ────────────────────────────────────────────────────
static void lx_ether_setup(struct net_device *dev)
{
    dev->type = ARPHRD_ETHER;
    dev->hard_header_len = ETH_HLEN;
    dev->min_header_len = ETH_HLEN;
    dev->mtu = ETH_DATA_LEN;
    dev->min_mtu = ETH_MIN_MTU;
    dev->max_mtu = ETH_DATA_LEN;
    dev->addr_len = ETH_ALEN;
    dev->tx_queue_len = 1000;   // upstream DEFAULT_TX_QUEUE_LEN (satoru)
    dev->flags = IFF_BROADCAST | IFF_MULTICAST;
    dev->priv_flags |= IFF_TX_SKB_SHARING;
    eth_broadcast_addr(dev->broadcast);
}

struct net_device *alloc_netdev_mqs(int sizeof_priv, const char *name,
                                    unsigned char name_assign_type,
                                    void (*setup)(struct net_device *),
                                    unsigned int txqs, unsigned int rxqs)
{
    (void)name_assign_type; (void)rxqs;
    size_t sz = sizeof(struct net_device) + (size_t)sizeof_priv;
    struct net_device *dev = lx_os_aligned_alloc(SMP_CACHE_BYTES, sz);
    if (!dev)
        return NULL;
    memset(dev, 0, sz);
    dev->priv_len = (u32)sizeof_priv;
    if (!txqs)
        txqs = 1;
    dev->_tx = kzalloc(sizeof(struct netdev_queue) * txqs, GFP_KERNEL);
    dev->dev_addr = kzalloc(MAX_ADDR_LEN, GFP_KERNEL);
    if (!dev->_tx || !dev->dev_addr)
        return NULL;
    dev->num_tx_queues = txqs;
    dev->real_num_tx_queues = txqs;
    for (unsigned int i = 0; i < txqs; i++) {
        struct netdev_queue *q = &dev->_tx[i];
        q->dev = dev;
        spin_lock_init(&q->_xmit_lock);
        q->xmit_lock_owner = -1;
        dql_init(&q->dql, HZ);
    }
    INIT_LIST_HEAD(&dev->napi_list);
    strscpy(dev->name, name ? name : "eth%d", sizeof(dev->name));
    dev->state = BIT(__LINK_STATE_PRESENT);
    dev->gso_max_size = GSO_LEGACY_MAX_SIZE;
    dev->gso_max_segs = GSO_MAX_SEGS;
    if (setup)
        setup(dev);
    return dev;
}

struct net_device *alloc_etherdev_mqs(int sizeof_priv, unsigned int txqs, unsigned int rxqs)
{
    return alloc_netdev_mqs(sizeof_priv, "eth%d", NET_NAME_ENUM, lx_ether_setup, txqs, rxqs);
}

void free_netdev(struct net_device *dev)
{
    if (!dev)
        return;
    kfree(dev->_tx);
    kfree((void *)dev->dev_addr);
    lx_os_free(dev);
}

void dev_addr_mod(struct net_device *dev, unsigned int offset, const void *addr, size_t len)
{
    memcpy((unsigned char *)dev->dev_addr + offset, addr, len);
    memcpy(&dev->dev_addr_shadow[offset], addr, len);
}

int register_netdev(struct net_device *dev)
{
    if (lx_netdev_count >= LX_MAX_NETDEV)
        return -ENOSPC;
    char *pct = strchr(dev->name, '%');
    if (pct) {
        pct[0] = (char)('0' + lx_netdev_count);
        pct[1] = 0;
    }
    if (dev->netdev_ops && dev->netdev_ops->ndo_init) {
        int r = dev->netdev_ops->ndo_init(dev);
        if (r)
            return r;
    }
    dev->reg_state = NETREG_REGISTERED;
    lx_netdevs[lx_netdev_count++] = dev;
    lx_log("[lx_net] register_netdev ");
    lx_log(dev->name);
    lx_log(" mac ");
    for (int i = 0; i < 6; i++) {
        static const char h[] = "0123456789abcdef";
        char b[4] = { h[dev->dev_addr[i] >> 4], h[dev->dev_addr[i] & 15], (char)(i < 5 ? ':' : '\n'), 0 };
        lx_log(b);
    }
    return 0;
}

void unregister_netdev(struct net_device *dev)
{
    rtnl_lock();
    if (test_and_clear_bit(__LINK_STATE_START, &dev->state) && dev->netdev_ops->ndo_stop)
        dev->netdev_ops->ndo_stop(dev);
    rtnl_unlock();
    if (dev == lx_bridged)
        lx_broker_call(LX_KLD_OP_NET_CARRIER, 0, 0, 0, 0);
    dev->reg_state = NETREG_UNREGISTERED;
    for (int i = 0; i < lx_netdev_count; i++)
        if (lx_netdevs[i] == dev)
            lx_netdevs[i] = lx_netdevs[--lx_netdev_count];
}

void netif_carrier_on(struct net_device *dev)
{
    if (test_and_clear_bit(__LINK_STATE_NOCARRIER, &dev->state)) {
        lx_log("[lx_net] ");
        lx_log(dev->name);
        lx_log(": carrier on\n");
        if (dev == lx_bridged)
            lx_broker_call(LX_KLD_OP_NET_CARRIER, 1, 0, 0, 0);
    }
}

void netif_carrier_off(struct net_device *dev)
{
    if (!test_and_set_bit(__LINK_STATE_NOCARRIER, &dev->state)) {
        if (dev == lx_bridged)
            lx_broker_call(LX_KLD_OP_NET_CARRIER, 0, 0, 0, 0);
    }
}

void netif_tx_stop_all_queues(struct net_device *dev)
{
    for (unsigned int i = 0; i < dev->num_tx_queues; i++)
        netif_tx_stop_queue(netdev_get_tx_queue(dev, i));
}

void netif_device_detach(struct net_device *dev)
{
    if (test_and_clear_bit(__LINK_STATE_PRESENT, &dev->state) && netif_running(dev))
        netif_tx_stop_all_queues(dev);
}

void netif_device_attach(struct net_device *dev)
{
    if (!test_and_set_bit(__LINK_STATE_PRESENT, &dev->state) && netif_running(dev)) {
        netif_tx_wake_all_queues(dev);
        lx_xmit_kick();
    }
}

struct rtnl_link_stats64 *dev_get_stats(struct net_device *dev, struct rtnl_link_stats64 *storage)
{
    memset(storage, 0, sizeof(*storage));
    if (dev->netdev_ops && dev->netdev_ops->ndo_get_stats64)
        dev->netdev_ops->ndo_get_stats64(dev, storage);
    else
        netdev_stats_to_stats64(storage, &dev->stats);
    return storage;
}

void netdev_stats_to_stats64(struct rtnl_link_stats64 *stats64, const struct net_device_stats *netdev_stats)
{
    size_t n = sizeof(*netdev_stats) / sizeof(unsigned long);
    const atomic_long_t *src = (const atomic_long_t *)netdev_stats;
    u64 *dst = (u64 *)stats64;
    for (size_t i = 0; i < n && i < sizeof(*stats64) / sizeof(u64); i++)
        dst[i] = (u64)atomic_long_read(&src[i]);
}

void netdev_rss_key_fill(void *buffer, size_t len)
{
    static u8 key[NETDEV_RSS_KEY_LEN];
    static int filled;
    if (!filled) {
        lx_os_random(key, sizeof(key));
        filled = 1;
    }
    memcpy(buffer, key, len < sizeof(key) ? len : sizeof(key));
}

// drives Kurono's network loop body every 10 ms from a preemptive ring-3
// thread (see KldNet::Tick in the kernel for the starvation it works around).
// the wait is a timed futex on a word nobody wakes, re-armed against the clock
// so an early return never turns into a spin. (satoru)
static int lx_net_tick_thread(void *arg)
{
    static volatile unsigned int never;
    (void)arg;
    for (;;) {
        unsigned long long end = lx_os_now_ns() + 10000000ull;
        unsigned long long now;
        while ((now = lx_os_now_ns()) < end)
            lx_os_futex_wait(&never, 0, (long long)(end - now));
        lx_broker_call(LX_KLD_OP_NET_TICK, 0, 0, 0, 0);
    }
    return 0;
}

// ── bring-up: what `ip link set eth0 up` does, then attach to Kurono ─────────
static int lx_net_open(struct net_device *dev)
{
    int rc = 0;
    rtnl_lock();
    set_bit(__LINK_STATE_START, &dev->state);
    if (dev->netdev_ops->ndo_open)
        rc = dev->netdev_ops->ndo_open(dev);
    if (rc)
        clear_bit(__LINK_STATE_START, &dev->state);
    else
        dev->flags |= IFF_UP;
    rtnl_unlock();
    return rc;
}

void lx_net_bringup_all(void)
{
    for (int i = 0; i < lx_netdev_count; i++) {
        struct net_device *dev = lx_netdevs[i];
        lx_log("[lx_net] opening ");
        lx_log(dev->name);
        lx_log(" (ndo_open)\n");
        int rc = lx_net_open(dev);
        lx_log("[lx_net] ndo_open returned ");
        if (rc < 0) { lx_log("-"); lx_log_u((unsigned long)-rc); } else lx_log_u((unsigned long)rc);
        lx_log("\n");
        if (rc || lx_bridged)
            continue;
        void *va = lx_os_reserve((unsigned long)LX_NET_PAGES * 4096ul);
        if (!va)
            continue;
        unsigned long mlo = 0, mhi = 0;
        for (int b = 0; b < 4; b++) mlo |= (unsigned long)dev->dev_addr[b] << (8 * b);
        mhi = (unsigned long)dev->dev_addr[4] | ((unsigned long)dev->dev_addr[5] << 8);
        long r = lx_broker_call(LX_KLD_OP_NET_ATTACH, (unsigned long)va, mlo, mhi, 0);
        if (r < 0) {
            lx_log("[lx_net] broker NET_ATTACH failed\n");
            continue;
        }
        lx_ring = va;
        lx_ring_tx = (unsigned char *)va + 4096;
        lx_ring_rx = lx_ring_tx + LX_NET_TX_SLOTS * LX_NET_SLOT_BYTES;
        if (lx_ring->magic != LX_NET_MAGIC)
            lx_log("[lx_net] WARNING: ring magic mismatch\n");
        lx_bridged = dev;
        lx_task_spawn("kld-xmit", lx_xmit_thread, dev);
        lx_task_spawn("kld-nettick", lx_net_tick_thread, NULL);
        lx_log("[lx_net] ");
        lx_log(dev->name);
        lx_log(" attached to Kurono eth0\n");
        if (netif_carrier_ok(dev))
            lx_broker_call(LX_KLD_OP_NET_CARRIER, 1, 0, 0, 0);
    }
}

void lx_net_stats_log(void)
{
    lx_log("[lx_net] rx frames ");
    lx_log_u(lx_rx_frames);
    lx_log(" rx dropped ");
    lx_log_u(lx_rx_dropped);
    lx_log(" tx frames ");
    lx_log_u(lx_tx_frames);
    lx_log(" tx busy ");
    lx_log_u(lx_tx_busy);
    if (lx_bridged && lx_ring) {
        struct netdev_queue *q = netdev_get_tx_queue(lx_bridged, 0);
        lx_log(" | txq state ");
        lx_log_hex(q->state);
        lx_log(" dql queued ");
        lx_log_u(q->dql.num_queued);
        lx_log(" completed ");
        lx_log_u(q->dql.num_completed);
        lx_log(" limit ");
        lx_log_u(q->dql.limit);
        lx_log(" ring prod ");
        lx_log_u(lx_ring->tx_prod);
        lx_log(" cons ");
        lx_log_u(lx_ring->tx_cons);
        lx_log(" kdrops ");
        lx_log_u(lx_ring->tx_drops);
        lx_log(" xmit loops ");
        lx_log_u(lx_xmit_loops);
        lx_log(" waits ");
        lx_log_u(lx_xmit_waits);
    }
    lx_log("\n");
}

// end (satoru)
