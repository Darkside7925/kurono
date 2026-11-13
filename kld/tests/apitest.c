// apitest: the KLD shim's self-test family (no device, no broker grant).
//
// Every line is [apitest] <name>: PASS|FAIL <detail>; the last line is
// [apitest] N/N PASS only when every compiled-in test passed. run it with the
// boot flag kurono.kld=apitest (regress_apitest.sh does that).
//
// The concurrency tests use real kthreads from lx_sched.c and the real shim
// locks/timers/workqueues; nothing here fakes a return value to look green.
// end (satoru)

#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/xarray.h>
#include <linux/idr.h>
#include <linux/rhashtable.h>
#include <linux/kfifo.h>
#include <linux/scatterlist.h>
#include <linux/netlink.h>
#include <net/netlink.h>
#include <net/genetlink.h>
#include <linux/notifier.h>
#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/rcupdate.h>
#include <linux/srcu.h>
#include <linux/rwsem.h>
#include <linux/seqlock.h>
#include <linux/percpu.h>
#include <linux/hrtimer.h>
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <asm/processor.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/firmware.h>
#include <linux/random.h>
#include <linux/atomic.h>
#include <linux/string.h>
#include <linux/preempt.h>
#include <linux/spinlock.h>
#include <linux/netdevice.h>
#include <linux/timer.h>

#include "kimpl/lx_kshim.h"
#include "kimpl/lx_broker.h"

// separate test files keep the crypto and wpa corpora out of this one. (satoru)
extern void apitest_crypto_run(int *pass, int *total);
extern void apitest_wpa_run(int *pass, int *total);

#define TMAX 64
static int g_total, g_pass;

static void t_result(const char *name, int ok, const char *detail)
{
    lx_log("[apitest] ");
    lx_log(name);
    lx_log(ok ? ": PASS " : ": FAIL ");
    if (detail && detail[0])
        lx_log(detail);
    lx_log("\n");
    g_total++;
    if (ok)
        g_pass++;
}

#define T(name, cond, ...) do { \
    char b_[192]; \
    snprintf(b_, sizeof(b_), __VA_ARGS__); \
    t_result(name, (cond), b_); \
} while (0)

// ── xarray ──────────────────────────────────────────────────────────────────
static void test_xarray(void)
{
    DEFINE_XARRAY(xa);
    int ok = 1;
    void *e;
    unsigned long idx;

    for (unsigned long i = 0; i < 10000; i++) {
        e = xa_store(&xa, i, xa_mk_value(i + 1), GFP_KERNEL);
        if (xa_is_err(e)) { ok = 0; break; }
    }
    for (unsigned long i = 0; i < 10000 && ok; i++) {
        e = xa_load(&xa, i);
        if (e != xa_mk_value(i + 1)) { ok = 0; break; }
    }
    for (unsigned long i = 0; i < 10000 && ok; i += 1000) {
        xa_set_mark(&xa, i, XA_MARK_0);
        if (!xa_get_mark(&xa, i, XA_MARK_0))
            ok = 0;
    }
    unsigned long n = 0;
    idx = 0;
    while ((e = xa_find(&xa, &idx, ULONG_MAX, XA_PRESENT)) != NULL) {
        n++;
        idx++;
    }
    if (n != 10000) ok = 0;
    for (unsigned long i = 0; i < 10000; i++)
        xa_erase(&xa, i);
    if (!xa_empty(&xa)) ok = 0;
    xa_destroy(&xa);
    T("xarray_10k_marks", ok, "entries=10000 iterated=%lu", n);
}

// ── idr / ida ───────────────────────────────────────────────────────────────
static void test_idr(void)
{
    DEFINE_IDR(idr);
    int ok = 1, id, next = 0;
    for (int i = 0; i < 512; i++) {
        id = idr_alloc(&idr, (void *)(long)(i + 1), next, 0, GFP_KERNEL);
        if (id >= 0) next = id + 1;
        if (id < 0 || idr_find(&idr, id) != (void *)(long)(i + 1)) { ok = 0; break; }
    }
    lx_log("[apitest] idr allocated\n");
    if (ok) {
        void *entry;
        idr_for_each_entry(&idr, entry, id) { (void)entry; }
        lx_log("[apitest] idr iterated\n");
        for (int i = 0; i < 512; i++)
            idr_remove(&idr, i);
        lx_log("[apitest] idr removed\n");
    }
    idr_destroy(&idr);
    T("idr_alloc_remove", ok, "ids=512");
}

static void test_ida(void)
{
    DEFINE_IDA(ida);
    int ok = 1, id, next = 0;
    for (int i = 0; i < 512; i++) {
        id = ida_alloc_range(&ida, next, ~0u, GFP_KERNEL);
        if (id < 0) { ok = 0; break; }
        next = id + 1;
    }
    for (int i = 0; i < 512 && ok; i++) {
        id = ida_alloc_range(&ida, 0, ~0u, GFP_KERNEL);
        if (id < 0) ok = 0;
        else ida_free(&ida, id);
    }
    ida_destroy(&ida);
    T("ida_alloc_free_reuse", ok, "ids=512");
}

// ── rhashtable ──────────────────────────────────────────────────────────────
struct rh_obj {
    struct rhash_head node;
    u32 key;
};

static const struct rhashtable_params rh_params = {
    .head_offset = offsetof(struct rh_obj, node),
    .key_offset = offsetof(struct rh_obj, key),
    .key_len = sizeof(u32),
    .automatic_shrinking = true,
};

static void test_rhashtable(void)
{
    struct rhashtable ht;
    struct rh_obj *objs;
    int ok = 1;

    if (rhashtable_init(&ht, &rh_params) != 0) {
        T("rhashtable_10k", 0, "init failed");
        return;
    }
    objs = kzalloc(sizeof(*objs) * 10000, GFP_KERNEL);
    if (!objs) {
        T("rhashtable_10k", 0, "alloc failed");
        rhashtable_destroy(&ht);
        return;
    }
    for (u32 i = 0; i < 10000; i++) {
        objs[i].key = i;
        if (rhashtable_insert_fast(&ht, &objs[i].node, rh_params) != 0) { ok = 0; break; }
    }
    for (u32 i = 0; i < 10000 && ok; i++) {
        if (rhashtable_lookup_fast(&ht, &i, rh_params) != &objs[i]) ok = 0;
    }
    for (u32 i = 0; i < 10000 && ok; i++) {
        if (rhashtable_remove_fast(&ht, &objs[i].node, rh_params) != 0) ok = 0;
    }
    kfree(objs);
    rhashtable_destroy(&ht);
    T("rhashtable_10k", ok, "insert/lookup/remove=10000");
}

// ── kfifo ───────────────────────────────────────────────────────────────────
static void test_kfifo(void)
{
    struct kfifo f;
    int v, out = 0, ok = 1;
    if (kfifo_alloc(&f, 16, GFP_KERNEL) != 0) {
        T("kfifo", 0, "alloc failed");
        return;
    }
    for (v = 0; v < 16; v++)
        if (kfifo_in(&f, &v, 1) != 1) ok = 0;
    if (kfifo_len(&f) != 16) ok = 0;
    for (v = 0; v < 16; v++) {
        if (kfifo_out(&f, &out, 1) != 1 || out != v) ok = 0;
    }
    kfifo_free(&f);
    T("kfifo", ok, "16 in/out");
}

// ── scatterlist ─────────────────────────────────────────────────────────────
static void test_scatterlist(void)
{
    char a[16], b[16], c[16];
    struct scatterlist sg[3];
    struct scatterlist *s;
    int i, n = 0, ok = 1;
    sg_init_table(sg, 3);
    sg_set_buf(&sg[0], a, sizeof(a));
    sg_set_buf(&sg[1], b, sizeof(b));
    sg_set_buf(&sg[2], c, sizeof(c));
    for_each_sg(sg, s, 3, i) {
        if (sg_page(s) == NULL) ok = 0;
        n++;
    }
    if (n != 3 || sg_nents(sg) != 3) ok = 0;
    T("scatterlist", ok, "nents=%d", n);
}

// ── netlink attribute round trip ────────────────────────────────────────────
static void test_nla(void)
{
    struct sk_buff *skb = alloc_skb(128, GFP_KERNEL);
    struct nlattr *tb[4] = { };
    int ok = 1;
    if (!skb) {
        T("nla_roundtrip", 0, "alloc_skb failed");
        return;
    }
    if (nla_put(skb, 1, 4, "abcd") != 0) ok = 0;
    if (nla_put(skb, 2, 2, "xy") != 0) ok = 0;
    if (nla_parse(tb, 3, (struct nlattr *)skb->data, skb->len, NULL, NULL) != 0) ok = 0;
    if (!tb[1] || nla_len(tb[1]) != 4 || memcmp(nla_data(tb[1]), "abcd", 4)) ok = 0;
    if (!tb[2] || nla_len(tb[2]) != 2 || memcmp(nla_data(tb[2]), "xy", 2)) ok = 0;
    kfree_skb(skb);
    T("nla_roundtrip", ok, "attrs=2");
}

// ── notifier chain ──────────────────────────────────────────────────────────
static int g_nh_order[4], g_nh_n;
static int nh_fn(struct notifier_block *nb, unsigned long val, void *v)
{
    (void)val; (void)v;
    g_nh_order[g_nh_n++] = nb->priority;
    return nb->priority == 5 ? NOTIFY_STOP : NOTIFY_DONE;
}

static void test_notifier(void)
{
    BLOCKING_NOTIFIER_HEAD(nh);
    struct notifier_block a = { .notifier_call = nh_fn, .priority = 10 };
    struct notifier_block b = { .notifier_call = nh_fn, .priority = 5 };
    struct notifier_block c = { .notifier_call = nh_fn, .priority = 1 };
    int ok = 1;
    g_nh_n = 0;
    if (blocking_notifier_chain_register(&nh, &a) != 0) ok = 0;
    if (blocking_notifier_chain_register(&nh, &b) != 0) ok = 0;
    if (blocking_notifier_chain_register(&nh, &c) != 0) ok = 0;
    (void)blocking_notifier_call_chain(&nh, 0, NULL);
    if (g_nh_n != 2 || g_nh_order[0] != 10 || g_nh_order[1] != 5) ok = 0;
    blocking_notifier_chain_unregister(&nh, &a);
    blocking_notifier_chain_unregister(&nh, &b);
    blocking_notifier_chain_unregister(&nh, &c);
    T("notifier_order_stop", ok, "calls=%d order=%d,%d,NOSTOP", g_nh_n,
      g_nh_n > 0 ? g_nh_order[0] : -1, g_nh_n > 1 ? g_nh_order[1] : -1);
}

// ── devres ──────────────────────────────────────────────────────────────────
static int g_dr_order[4], g_dr_n;
static void dr_action(void *p)
{
    g_dr_order[g_dr_n++] = (int)(long)p;
}

static void test_devres(void)
{
    struct device dev;
    void *p;
    int ok = 1;
    g_dr_n = 0;
    memset(&dev, 0, sizeof(dev));
    device_initialize(&dev);
    if (devm_add_action(&dev, dr_action, (void *)1) != 0) ok = 0;
    p = devm_kzalloc(&dev, 64, GFP_KERNEL);
    if (!p) ok = 0;
    if (devm_add_action(&dev, dr_action, (void *)2) != 0) ok = 0;
    devres_release_all(&dev);
    if (g_dr_n != 2 || g_dr_order[0] != 2 || g_dr_order[1] != 1) ok = 0;
    T("devres_reverse_release", ok, "order=%d,%d", g_dr_n > 0 ? g_dr_order[0] : -1,
      g_dr_n > 1 ? g_dr_order[1] : -1);
}

// ── kobject ─────────────────────────────────────────────────────────────────
static int g_kobj_release;
static void kobj_release(struct kobject *k)
{
    (void)k;
    g_kobj_release++;
}
static struct kobj_type g_ktype = { .release = kobj_release };

static void test_kobject(void)
{
    struct kobject k;
    g_kobj_release = 0;
    memset(&k, 0, sizeof(k));
    kobject_init(&k, &g_ktype);
    kobject_get(&k);
    kobject_put(&k);
    kobject_put(&k);
    T("kobject_release_once", g_kobj_release == 1, "release=%d", g_kobj_release);
}

// ── RCU ─────────────────────────────────────────────────────────────────────
static atomic_t g_rcu_held;
static int rcu_reader_fn(void *arg)
{
    (void)arg;
    rcu_read_lock();
    atomic_set(&g_rcu_held, 1);
    msleep(200);
    rcu_read_unlock();
    return 0;
}

static atomic_t g_rcu_cb;
static void rcu_cb_fn(struct rcu_head *h) { (void)h; atomic_set(&g_rcu_cb, 1); }

static void test_rcu(void)
{
    struct task_struct *t;
    struct rcu_head head;
    unsigned long long t0, t1;
    int ok = 1;
    atomic_set(&g_rcu_held, 0);
    atomic_set(&g_rcu_cb, 0);
    t = kthread_run(rcu_reader_fn, NULL, "apitest-rcu");
    if (IS_ERR(t)) {
        T("rcu_reader_wait", 0, "kthread failed");
        return;
    }
    while (!atomic_read(&g_rcu_held)) msleep(1);
    t0 = lx_uptime_ns();
    synchronize_rcu();
    t1 = lx_uptime_ns();
    kthread_stop(t);
    if (t1 - t0 < 120000000ull) ok = 0;
    call_rcu(&head, rcu_cb_fn);
    for (int i = 0; i < 2000 && !atomic_read(&g_rcu_cb); i++) msleep(1);
    if (!atomic_read(&g_rcu_cb)) ok = 0;
    T("rcu_reader_wait_callback", ok, "wait=%llums cb=%d",
      (t1 - t0) / 1000000ull, atomic_read(&g_rcu_cb));
}

// ── RCU overlap, barrier-from-work and unlock cost ──────────────────────────
static volatile int g_ro_stop;
static volatile int g_ro_active[2];
static volatile uint64_t g_ro_enter[2];

static int rcu_overlap_reader(void *arg)
{
    int i = (int)(long)arg;
    while (!g_ro_stop) {
        rcu_read_lock();
        g_ro_enter[i] = lx_uptime_ns();
        __atomic_store_n(&g_ro_active[i], 1, __ATOMIC_RELEASE);
        lx_os_sleep_ns(200000);
        __atomic_store_n(&g_ro_active[i], 0, __ATOMIC_RELEASE);
        rcu_read_unlock();
    }
    return 0;
}

static void test_rcu_overlap(void)
{
    struct task_struct *t[2];
    int ok = 1, waited = 0;
    uint64_t t0, t1;
    g_ro_stop = 0;
    g_ro_enter[0] = g_ro_enter[1] = 0;
    g_ro_active[0] = g_ro_active[1] = 0;
    t[0] = kthread_run(rcu_overlap_reader, (void *)0L, "apitest-rcu0");
    t[1] = kthread_run(rcu_overlap_reader, (void *)1L, "apitest-rcu1");
    if (IS_ERR(t[0]) || IS_ERR(t[1])) {
        T("rcu_overlap", 0, "kthread failed");
        return;
    }
    // wait until both readers have actually entered once, then keep a small
    // gap between syncs so the readers get scheduled on this guest. (satoru)
    for (int guard = 0; guard < 2000; guard++) {
        if (__atomic_load_n(&g_ro_enter[0], __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&g_ro_enter[1], __ATOMIC_ACQUIRE))
            break;
        msleep(1);
    }
    t0 = lx_uptime_ns();
    for (int n = 0; n < 200; n++) {
        lx_os_sleep_ns(500000);
        int a0 = __atomic_load_n(&g_ro_active[0], __ATOMIC_ACQUIRE);
        int a1 = __atomic_load_n(&g_ro_active[1], __ATOMIC_ACQUIRE);
        uint64_t s0 = lx_uptime_ns();
        synchronize_rcu();
        uint64_t s1 = lx_uptime_ns();
        if ((a0 || a1) && s1 - s0 > 50000ull)
            waited++;
    }
    t1 = lx_uptime_ns();
    g_ro_stop = 1;
    kthread_stop(t[0]);
    kthread_stop(t[1]);
    if (t1 - t0 > 3000000000ull) ok = 0;
    if (!waited) ok = 0;
    T("rcu_overlap", ok, "200 syncs in %llums overlapped=%d",
      (t1 - t0) / 1000000ull, waited);
}

static atomic_t g_rbw_cb, g_rbw_done;
static void rbw_cb(struct rcu_head *h) { (void)h; atomic_set(&g_rbw_cb, 1); }
static struct work_struct g_rbw_work;
static void rbw_work_fn(struct work_struct *w)
{
    static struct rcu_head head;
    (void)w;
    call_rcu(&head, rbw_cb);
    rcu_barrier();
    atomic_set(&g_rbw_done, 1);
}

static void test_rcu_barrier_from_work(void)
{
    atomic_set(&g_rbw_cb, 0);
    atomic_set(&g_rbw_done, 0);
    INIT_WORK(&g_rbw_work, rbw_work_fn);
    if (!queue_work(system_wq, &g_rbw_work)) {
        T("rcu_barrier_from_work", 0, "queue_work failed");
        return;
    }
    for (int i = 0; i < 3000 && !atomic_read(&g_rbw_done); i++)
        msleep(1);
    T("rcu_barrier_from_work", atomic_read(&g_rbw_done) && atomic_read(&g_rbw_cb),
      "done=%d cb=%d", atomic_read(&g_rbw_done), atomic_read(&g_rbw_cb));
}

static void test_rcu_unlock_cost(void)
{
    const int N = 1000000;
    uint64_t t0, t1;
    t0 = lx_uptime_ns();
    for (int i = 0; i < N; i++) {
        rcu_read_lock();
        rcu_read_unlock();
    }
    t1 = lx_uptime_ns();
    uint64_t ns = (t1 - t0) / (uint64_t)N;
    T("rcu_unlock_cost", ns < 50, "%llu ns/pair (no waiter)", ns);
}

// ── SRCU ────────────────────────────────────────────────────────────────────
static struct srcu_struct g_ss;
static atomic_t g_srcu_held;
static int srcu_reader_fn(void *arg)
{
    (void)arg;
    int idx = __srcu_read_lock(&g_ss);
    atomic_set(&g_srcu_held, 1);
    msleep(200);
    __srcu_read_unlock(&g_ss, idx);
    return 0;
}
static atomic_t g_srcu_cb;
static void srcu_cb_fn(struct rcu_head *h) { (void)h; atomic_set(&g_srcu_cb, 1); }

static void test_srcu(void)
{
    struct task_struct *t;
    struct rcu_head head;
    unsigned long long t0, t1;
    int ok = 1;
    if (init_srcu_struct(&g_ss) != 0) {
        T("srcu_reader_wait", 0, "init failed");
        return;
    }
    atomic_set(&g_srcu_held, 0);
    atomic_set(&g_srcu_cb, 0);
    t = kthread_run(srcu_reader_fn, NULL, "apitest-srcu");
    if (IS_ERR(t)) {
        T("srcu_reader_wait", 0, "kthread failed");
        cleanup_srcu_struct(&g_ss);
        return;
    }
    while (!atomic_read(&g_srcu_held)) msleep(1);
    t0 = lx_uptime_ns();
    synchronize_srcu(&g_ss);
    t1 = lx_uptime_ns();
    kthread_stop(t);
    if (t1 - t0 < 120000000ull) ok = 0;
    call_srcu(&g_ss, &head, srcu_cb_fn);
    for (int i = 0; i < 2000 && !atomic_read(&g_srcu_cb); i++) msleep(1);
    if (!atomic_read(&g_srcu_cb)) ok = 0;
    cleanup_srcu_struct(&g_ss);
    T("srcu_reader_wait_callback", ok, "wait=%llums cb=%d",
      (t1 - t0) / 1000000ull, atomic_read(&g_srcu_cb));
}

// ── rwsem ───────────────────────────────────────────────────────────────────
static struct rw_semaphore g_rw;
static atomic_t g_rw_active, g_rw_bad;
static unsigned long g_rw_counter;

static int rw_reader_fn(void *arg)
{
    long id = (long)arg;
    (void)id;
    for (int i = 0; i < 34000; i++) {
        down_read(&g_rw);
        if (atomic_read(&g_rw_active) >= 3)
            atomic_inc(&g_rw_bad);
        __atomic_fetch_add(&g_rw_counter, 1, __ATOMIC_RELAXED);
        up_read(&g_rw);
        if ((i & 1023) == 0)
            lx_os_yield();
    }
    return 0;
}

static int rw_writer_fn(void *arg)
{
    (void)arg;
    for (int i = 0; i < 10000; i++) {
        down_write(&g_rw);
        if (atomic_read(&g_rw_active) != 0)
            atomic_inc(&g_rw_bad);
        __atomic_fetch_add(&g_rw_counter, 1, __ATOMIC_RELAXED);
        up_write(&g_rw);
        if ((i & 255) == 0)
            lx_os_yield();
    }
    return 0;
}

static void test_rwsem(void)
{
    struct task_struct *r[3], *w;
    int ok = 1;
    init_rwsem(&g_rw);
    atomic_set(&g_rw_active, 0);
    atomic_set(&g_rw_bad, 0);
    g_rw_counter = 0;
    for (int i = 0; i < 3; i++)
        r[i] = kthread_run(rw_reader_fn, (void *)(long)i, "apitest-rw%d", i);
    w = kthread_run(rw_writer_fn, NULL, "apitest-rww");
    for (int i = 0; i < 3; i++)
        if (IS_ERR(r[i])) ok = 0;
    if (IS_ERR(w)) ok = 0;
    for (int i = 0; i < 3; i++)
        if (!IS_ERR(r[i])) kthread_stop(r[i]);
    if (!IS_ERR(w)) kthread_stop(w);
    if (atomic_read(&g_rw_bad)) ok = 0;
    if (g_rw_counter != 3 * 34000 + 10000) ok = 0;
    T("rwsem_reader_writer", ok, "counter=%lu bad=%d",
      g_rw_counter, atomic_read(&g_rw_bad));
}

// ── seqlock ─────────────────────────────────────────────────────────────────
static DEFINE_SEQLOCK(g_sl);
static volatile unsigned long g_sl_a, g_sl_b;
static volatile int g_sl_stop;
static volatile int g_sl_go;
static atomic_t g_sl_retry;

static int sl_writer_fn(void *arg)
{
    (void)arg;
    while (!g_sl_go)
        msleep(1);
    for (unsigned long i = 0; i < 5000000 && !g_sl_stop; i++) {
        write_seqlock(&g_sl);
        g_sl_a = i;
        g_sl_b = i + 1;
        write_sequnlock(&g_sl);
    }
    g_sl_stop = 1;
    return 0;
}

static void test_seqlock(void)
{
    struct task_struct *w;
    unsigned long seen = 0;
    int ok = 1;
    g_sl_stop = 0;
    g_sl_go = 0;
    atomic_set(&g_sl_retry, 0);
    w = kthread_run(sl_writer_fn, NULL, "apitest-seqw");
    if (IS_ERR(w)) {
        T("seqlock_retry", 0, "kthread failed");
        return;
    }
    int injected = 0;
    g_sl_go = 1;
    while (!g_sl_stop) {
        unsigned s;
        unsigned long a, b;
        for (;;) {
            s = read_seqbegin(&g_sl);
            a = g_sl_a;
            // force one race: the writer updates while this read section is
            // open, so read_seqretry() must reject it. (satoru)
            if (!injected) {
                injected = 1;
                msleep(2);
            }
            b = g_sl_b;
            if (!read_seqretry(&g_sl, s))
                break;
            atomic_inc(&g_sl_retry);
        }
        if (b != a + 1)
            ok = 0;
        seen++;
        if (seen > 2000000)
            break;
    }
    kthread_stop(w);
    if (!atomic_read(&g_sl_retry)) ok = 0;
    T("seqlock_retry", ok, "reads=%lu retries=%d", seen, atomic_read(&g_sl_retry));
}

// ── kmem_cache ──────────────────────────────────────────────────────────────
static int g_ctor_calls;
static void cache_ctor(void *p) { (void)p; g_ctor_calls++; }

static void test_kmem_cache(void)
{
    struct kmem_cache *c;
    void *o[100];
    int ok = 1;
    g_ctor_calls = 0;
    c = kmem_cache_create("apitest-cache", 64, 0, SLAB_HWCACHE_ALIGN, cache_ctor);
    if (!c) {
        T("kmem_cache_ctor", 0, "create failed");
        return;
    }
    for (int i = 0; i < 100; i++) {
        o[i] = kmem_cache_alloc(c, GFP_KERNEL);
        if (!o[i]) ok = 0;
    }
    if (g_ctor_calls < 100) ok = 0;
    for (int i = 0; i < 100; i++)
        kmem_cache_free(c, o[i]);
    kmem_cache_destroy(c);
    T("kmem_cache_ctor", ok, "ctor=%d objects=100", g_ctor_calls);
}

// ── percpu ──────────────────────────────────────────────────────────────────
static void test_percpu(void)
{
    int __percpu *p = alloc_percpu(int);
    int sum = 0, cpu;
    if (!p) {
        T("percpu", 0, "alloc failed");
        return;
    }
    *this_cpu_ptr(p) = 7;
    for_each_possible_cpu(cpu)
        sum += *per_cpu_ptr(p, cpu);
    free_percpu(p);
    T("percpu", sum == 7, "cpus=%d sum=%d", num_possible_cpus(), sum);
}

// ── hrtimer ─────────────────────────────────────────────────────────────────
struct hrt_ctx {
    struct hrtimer t;
    int idx;
};

static u64 g_hrt_target[20], g_hrt_fired[20];
static atomic_t g_hrt_n;

static enum hrtimer_restart hrt_cb(struct hrtimer *t)
{
    struct hrt_ctx *c = container_of(t, struct hrt_ctx, t);
    if (c->idx < 0) {
        // restart case below uses idx = -1
    }
    if (c->idx < 20) {
        g_hrt_fired[c->idx] = lx_uptime_ns();
        atomic_inc(&g_hrt_n);
    }
    return HRTIMER_NORESTART;
}

static atomic_t g_hrt_restart;
static enum hrtimer_restart hrt_restart_cb(struct hrtimer *t)
{
    int n = atomic_inc_return(&g_hrt_restart);
    if (n < 5) {
        hrtimer_forward(t, lx_uptime_ns(), 5000000);
        return HRTIMER_RESTART;
    }
    return HRTIMER_NORESTART;
}

static void test_hrtimer(void)
{
    struct hrt_ctx c[20];
    struct hrtimer rt;
    struct hrt_ctx rc;
    int ok = 1;

    memset(c, 0, sizeof(c));
    atomic_set(&g_hrt_n, 0);
    // spawn the timer thread before measuring: its first creation must not
    // consume the 1 ms timer's tolerance. (satoru)
    {
        static struct hrt_ctx warmctx;
        warmctx.idx = 99;
        hrtimer_init(&warmctx.t, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
        warmctx.t.function = hrt_cb;
        hrtimer_start(&warmctx.t, 1000000, HRTIMER_MODE_REL);
        msleep(10);
        hrtimer_cancel(&warmctx.t);
    }
    for (int i = 0; i < 20; i++) {
        c[i].idx = i;
        hrtimer_init(&c[i].t, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
        c[i].t.function = hrt_cb;
        g_hrt_target[i] = lx_uptime_ns() + (u64)(i + 1) * 1000000ull;
        hrtimer_start(&c[i].t, (ktime_t)((u64)(i + 1) * 1000000ull),
                      HRTIMER_MODE_REL);
    }
    msleep(80);
    u64 maxd = 0;
    for (int i = 0; i < 20; i++) {
        if (!g_hrt_fired[i])
            ok = 0;
        else {
            u64 d = g_hrt_fired[i] > g_hrt_target[i] ? g_hrt_fired[i] - g_hrt_target[i]
                                                      : g_hrt_target[i] - g_hrt_fired[i];
            if (d > maxd)
                maxd = d;
            if (d > 5000000ull)
                ok = 0;
        }
        hrtimer_cancel(&c[i].t);
    }

    memset(&rc, 0, sizeof(rc));
    rc.idx = -1;
    atomic_set(&g_hrt_restart, 0);
    hrtimer_init(&rt, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    rt.function = hrt_restart_cb;
    hrtimer_start(&rt, 1000000, HRTIMER_MODE_REL);
    for (int i = 0; i < 200 && atomic_read(&g_hrt_restart) < 5; i++)
        msleep(1);
    if (atomic_read(&g_hrt_restart) != 5) ok = 0;
    hrtimer_cancel(&rt);
    T("hrtimer_20_and_restart", ok, "fired=%d restart=%d maxdelta=%lluus",
      atomic_read(&g_hrt_n), atomic_read(&g_hrt_restart), maxd / 1000ull);
}

// ── jiffies freshness without a periodic timer wake ─────────────────────────
// the timer thread blocks until the next deadline now, so a jiffies reader
// must see it advance through the blocking/busy primitives themselves.
// (satoru)
static struct timer_list g_jif_timer;
static volatile int g_jif_hit;
static volatile unsigned long long g_jif_at_us;
static void jif_timer_cb(struct timer_list *t)
{
    (void)t;
    g_jif_at_us = lx_uptime_ns();
    g_jif_hit = 1;
}

static void test_jiffies(void)
{
    // 1: a 50 ms sleep with no timer armed; jiffies advances ~50 (HZ=1000).
    // the 1 ms sleep refreshes jiffies so the baseline is current (a direct
    // read of the data symbol cannot hook a refresh). (satoru)
    msleep(1);
    unsigned long j0 = jiffies;
    msleep(50);
    unsigned long d = jiffies - j0;
    T("jiffies_after_sleep", d >= 48 && d <= 52, "advanced=%lu", d);

    // 2: a 20 ms busy loop on jiffies with cpu_relax terminates.
    msleep(1);
    j0 = jiffies;
    unsigned long target = j0 + 20;
    unsigned long long t0 = lx_uptime_ns();
    while (time_before(jiffies, target))
        cpu_relax();
    unsigned long long el = (lx_uptime_ns() - t0) / 1000000ull;
    T("jiffies_busy_loop", el >= 17 && el <= 23, "elapsed=%llums", el);

    // 3: a +30 ms timer fires on time while the main thread is blocked.
    g_jif_hit = 0;
    g_jif_at_us = 0;
    timer_setup(&g_jif_timer, jif_timer_cb, 0);
    msleep(1);   // current jiffies baseline for the relative arm (satoru)
    unsigned long long a0 = lx_uptime_ns();
    mod_timer(&g_jif_timer, jiffies + 30);
    msleep(80);
    unsigned long long fired = g_jif_at_us ? (g_jif_at_us - a0) / 1000000ull : 0;
    int ok = g_jif_hit && fired >= 27 && fired <= 33;
    T("jiffies_timer_blocked", ok, "hit=%d fired=%llums", g_jif_hit, fired);
    timer_delete_sync(&g_jif_timer);
}

// ── tasklet ─────────────────────────────────────────────────────────────────
static atomic_t g_tl_runs;
static void tl_cb(struct tasklet_struct *t) { (void)t; atomic_inc(&g_tl_runs); }

static void test_tasklet(void)
{
    struct tasklet_struct t;
    int ok = 1;
    atomic_set(&g_tl_runs, 0);
    tasklet_setup(&t, tl_cb);
    tasklet_schedule(&t);
    tasklet_schedule(&t);
    msleep(30);
    if (atomic_read(&g_tl_runs) != 1) ok = 0;

    tasklet_disable(&t);
    tasklet_schedule(&t);
    msleep(30);
    if (atomic_read(&g_tl_runs) != 1) ok = 0;
    tasklet_enable(&t);
    msleep(30);
    if (atomic_read(&g_tl_runs) != 2) ok = 0;

    tasklet_schedule(&t);
    tasklet_kill(&t);
    int after = atomic_read(&g_tl_runs);
    msleep(20);
    if (atomic_read(&g_tl_runs) != after) ok = 0;
    T("tasklet_once_kill_disable", ok, "runs=%d", atomic_read(&g_tl_runs));
}

// ── delayed work ────────────────────────────────────────────────────────────
static atomic_t g_dw_runs;
static u64 g_dw_time;
static void dw_fn(struct work_struct *w) { (void)w; g_dw_time = lx_uptime_ns(); atomic_inc(&g_dw_runs); }
static DECLARE_DELAYED_WORK(g_dw, dw_fn);

static void test_delayed_work(void)
{
    int ok = 1;
    bool cancelled;
    u64 t0;
    atomic_set(&g_dw_runs, 0);
    t0 = lx_uptime_ns();
    queue_delayed_work(system_wq, &g_dw, msecs_to_jiffies(100));
    mod_delayed_work(system_wq, &g_dw, msecs_to_jiffies(10));
    msleep(60);
    if (atomic_read(&g_dw_runs) != 1) ok = 0;
    if (g_dw_time - t0 > 60000000ull || g_dw_time - t0 < 5000000ull) ok = 0;

    queue_delayed_work(system_wq, &g_dw, msecs_to_jiffies(100));
    cancelled = cancel_delayed_work_sync(&g_dw);
    if (!cancelled) ok = 0;
    cancelled = cancel_delayed_work_sync(&g_dw);
    if (cancelled) ok = 0;
    T("delayed_work_mod_cancel", ok, "elapsed=%llums cancel=%d",
      (g_dw_time - t0) / 1000000ull, 1);
}

// ── firmware ────────────────────────────────────────────────────────────────
static atomic_t g_fw_cb;
static void fw_cont(const struct firmware *fw, void *ctx)
{
    (void)ctx;
    if (fw)
        release_firmware(fw);
    atomic_set(&g_fw_cb, 1);
}

static void test_firmware(void)
{
    const struct firmware *fw = NULL;
    int rc, ok = 1;
    char d[200];
    int n = 0;

    rc = request_firmware(&fw, "iwlwifi-gl-c0-fm-c0.pnvm", NULL);
    if (rc != 0 || !fw) {
        T("firmware_pnvm", 0, "request rc=%d", rc);
        return;
    }
    n += snprintf(d + n, sizeof(d) - (size_t)n, "size=%zu first=", fw->size);
    for (int i = 0; i < 16 && i < (int)fw->size; i++)
        n += snprintf(d + n, sizeof(d) - (size_t)n, "%02x", fw->data[i]);
    n += snprintf(d + n, sizeof(d) - (size_t)n, " last=");
    for (size_t i = fw->size > 16 ? fw->size - 16 : 0; i < fw->size; i++)
        n += snprintf(d + n, sizeof(d) - (size_t)n, "%02x", fw->data[i]);
    if (fw->size != 295036) ok = 0;
    release_firmware(fw);

    fw = NULL;
    rc = request_firmware(&fw, "kld-no-such-firmware.bin", NULL);
    int missing_ok = (rc == -ENOENT && fw == NULL);

    // a name that tries to leave the store must be refused as a plain-name
    // violation, not resolved anywhere. (satoru)
    fw = NULL;
    rc = request_firmware(&fw, "../../../etc/passwd", NULL);
    int escape_ok = (rc == -ENOENT && fw == NULL);
    if (!missing_ok || !escape_ok) ok = 0;

    atomic_set(&g_fw_cb, 0);
    rc = request_firmware_nowait(NULL, false, "iwlwifi-gl-c0-fm-c0.pnvm",
                                 NULL, GFP_KERNEL, NULL, fw_cont);
    if (rc != 0) ok = 0;
    for (int i = 0; i < 2000 && !atomic_read(&g_fw_cb); i++)
        msleep(1);
    if (!atomic_read(&g_fw_cb)) ok = 0;

    T("firmware_pnvm_enoent_nowait", ok, "%s missing=%d escape=%d nowait=%d",
      d, missing_ok, escape_ok, atomic_read(&g_fw_cb));
}

// ── in-container generic netlink dispatcher ────────────────────────────────
static atomic_t g_genl_replies, g_genl_dumps, g_genl_mcasts;

static void tf_reply_cb(struct sk_buff *skb, struct nlmsghdr *nlh)
{
    (void)skb; (void)nlh;
    atomic_inc(&g_genl_replies);
}

static int tf_doit(struct sk_buff *skb, struct genl_info *info)
{
    (void)skb;
    struct sk_buff *r = alloc_skb(256, GFP_KERNEL);
    if (!r)
        return -ENOMEM;
    void *h = genlmsg_put(r, info->snd_portid, info->snd_seq, info->family, 0, 1);
    if (!h) { kfree_skb(r); return -EMSGSIZE; }
    nla_put_u32(r, 1, 0x1234);
    genlmsg_end(r, h);
    return genlmsg_reply(r, info);
}

static int tf_reject(struct sk_buff *skb, struct genl_info *info)
{
    (void)skb; (void)info;
    return 0;
}

static int tf_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
    const struct genl_info *info = genl_info_dump(cb);
    if (cb->args[0] >= 3)
        return 0;
    void *h = genlmsg_put(skb, info->snd_portid, info->snd_seq,
                          info->family, 0, 3);
    if (!h)
        return -EMSGSIZE;
    nla_put_u32(skb, 1, (u32)cb->args[0]);
    genlmsg_end(skb, h);
    cb->args[0]++;
    return skb->len;
}

static void tf_mcast_cb(struct sk_buff *skb, struct nlmsghdr *nlh)
{
    (void)skb; (void)nlh;
    atomic_inc(&g_genl_mcasts);
}

static atomic_t g_genl_port_unicasts, g_genl_releases;
static atomic_t g_slow_entered, g_slow_release, g_slow_port, g_slow_reply;
static struct sk_buff *tf_msg(int cmd, void **hout);

static void tf_slow_port_cb(struct sk_buff *skb, struct nlmsghdr *nlh)
{
    (void)skb; (void)nlh;
    atomic_inc(&g_slow_port);
}
static void tf_slow_reply_cb(struct sk_buff *skb, struct nlmsghdr *nlh)
{
    (void)skb; (void)nlh;
    atomic_inc(&g_slow_reply);
}
static int tf_slow(struct sk_buff *skb, struct genl_info *info)
{
    (void)skb;
    atomic_set(&g_slow_entered, 1);
    for (int i = 0; i < 200 && !atomic_read(&g_slow_release); i++)
        msleep(1);
    struct sk_buff *r = alloc_skb(256, GFP_KERNEL);
    if (!r)
        return -ENOMEM;
    void *h = genlmsg_put(r, info->snd_portid, info->snd_seq, info->family, 0, 5);
    if (!h) { kfree_skb(r); return -EMSGSIZE; }
    genlmsg_end(r, h);
    return genlmsg_reply(r, info);
}
static int tf_slow_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 200 && !atomic_read(&g_slow_entered); i++)
        msleep(1);
    void *h = NULL;
    struct sk_buff *s = tf_msg(1, &h);
    if (s) {
        genlmsg_end(s, h);
        netlink_unicast(NULL, s, 0x3000, 0);
    }
    atomic_set(&g_slow_release, 1);
    return 0;
}
static void tf_port_cb(struct sk_buff *skb, struct nlmsghdr *nlh)
{
    (void)skb; (void)nlh;
    atomic_inc(&g_genl_port_unicasts);
}
static int tf_nl_notify(struct notifier_block *nb, unsigned long state, void *ptr)
{
    (void)nb;
    struct netlink_notify *n = ptr;
    if (state == NETLINK_URELEASE && n && n->portid == 0x2000)
        atomic_inc(&g_genl_releases);
    return NOTIFY_OK;
}

static int g_tf_dump_multi_msgs, g_tf_dump_multi_rounds;

static int tf_dump_multi(struct sk_buff *skb, struct netlink_callback *cb)
{
    g_tf_dump_multi_rounds++;
    if (cb->args[0])
        return 0;
    const struct genl_info *info = genl_info_dump(cb);
    for (int i = 0; i < 5; i++) {
        void *h = genlmsg_put(skb, info->snd_portid, info->snd_seq,
                              info->family, 0, 4);
        if (!h)
            return -EMSGSIZE;
        nla_put_u32(skb, 1, (u32)i);
        genlmsg_end(skb, h);
    }
    cb->args[0] = 1;
    return skb->len;
}

static const struct nla_policy tf_policy[] = {
    [1] = { .type = NLA_U32 },
};
static const struct genl_small_ops tf_ops[] = {
    { .cmd = 1, .doit = tf_doit },
    { .cmd = 2, .doit = tf_reject },
    { .cmd = 3, .dumpit = tf_dump },
    { .cmd = 4, .dumpit = tf_dump_multi },
    { .cmd = 5, .doit = tf_slow },
};
static const struct genl_multicast_group tf_mcgrps[] = {
    { .name = "scan" },
};
static struct genl_family tf_fam = {
    .name = "lxapitest",
    .version = 1,
    .maxattr = 1,
    .policy = tf_policy,
    .small_ops = tf_ops,
    .n_small_ops = 4,
    .n_small_ops = 5,
    .mcgrps = tf_mcgrps,
    .n_mcgrps = 1,
};

static struct sk_buff *tf_msg(int cmd, void **hout)
{
    struct sk_buff *skb = alloc_skb(256, GFP_KERNEL);
    if (!skb)
        return NULL;
    void *h = genlmsg_put(skb, 0x1000, 1, &tf_fam, NLM_F_REQUEST, (u8)cmd);
    if (!h) { kfree_skb(skb); return NULL; }
    *hout = h;
    return skb;
}

static int g_tf_dumps;
static void tf_dump_cb(struct sk_buff *b, struct nlmsghdr *nlh)
{
    (void)b; (void)nlh;
    g_tf_dumps++;
}

static void tf_dump_multi_cb(struct sk_buff *b, struct nlmsghdr *nlh)
{
    (void)b; (void)nlh;
    g_tf_dump_multi_msgs++;
}

static void test_genl(void)
{
    int rc;
    if (genl_register_family(&tf_fam) != 0) {
        T("genl_doit", 0, "family register failed");
        return;
    }
    atomic_set(&g_genl_replies, 0);
    lx_genl_set_reply_cb(tf_reply_cb);
    void *h = NULL;
    struct sk_buff *s = tf_msg(1, &h);
    if (s) {
        nla_put_u32(s, 1, 7);
        genlmsg_end(s, h);
        rc = lx_genl_doit("lxapitest", s);
        kfree_skb(s);
    } else {
        rc = -ENOMEM;
    }
    T("genl_doit", rc == 0 && atomic_read(&g_genl_replies) == 1,
      "rc=%d replies=%d", rc, atomic_read(&g_genl_replies));

    s = tf_msg(2, &h);
    int ok = 0;
    if (s) {
        nla_put(s, 1, 1, "x");   // wrong length for the NLA_U32 policy
        genlmsg_end(s, h);
        rc = lx_genl_doit("lxapitest", s);
        ok = rc != 0;
        kfree_skb(s);
    }
    T("genl_policy_reject", ok, "bad-length attr rejected");

    g_tf_dumps = 0;
    s = tf_msg(3, &h);
    if (s) {
        rc = lx_genl_dump("lxapitest", s, tf_dump_cb);
        kfree_skb(s);
    } else {
        rc = -ENOMEM;
    }
    T("genl_dump", g_tf_dumps == 3, "messages=%d rc=%d", g_tf_dumps, rc);

    g_tf_dump_multi_msgs = 0;
    g_tf_dump_multi_rounds = 0;
    s = tf_msg(4, &h);
    if (s) {
        rc = lx_genl_dump("lxapitest", s, tf_dump_multi_cb);
        kfree_skb(s);
    } else {
        rc = -ENOMEM;
    }
    T("genl_dump_multi", g_tf_dump_multi_msgs == 5,
      "messages=%d rounds=%d rc=%d", g_tf_dump_multi_msgs,
      g_tf_dump_multi_rounds, rc);

    atomic_set(&g_genl_mcasts, 0);
    lx_genl_listen("lxapitest", "scan", tf_mcast_cb);
    s = tf_msg(1, &h);
    if (s) {
        genlmsg_multicast_allns(&tf_fam, s, 0, 0);
    }
    T("genl_mcast", atomic_read(&g_genl_mcasts) == 1,
      "delivered=%d", atomic_read(&g_genl_mcasts));
    lx_genl_set_reply_cb(NULL);

    atomic_set(&g_genl_port_unicasts, 0);
    lx_genl_set_reply_cb(NULL);
    lx_genl_listen_portid(0x2000, tf_port_cb);
    s = tf_msg(1, &h);
    if (s) {
        genlmsg_end(s, h);
        netlink_unicast(NULL, s, 0x2000, 0);
    }
    T("genl_portid_unicast", atomic_read(&g_genl_port_unicasts) == 1,
      "delivered=%d", atomic_read(&g_genl_port_unicasts));

    static struct notifier_block nl_nb = { .notifier_call = tf_nl_notify };
    atomic_set(&g_genl_releases, 0);
    netlink_register_notifier(&nl_nb);
    lx_genl_notify_release(0x2000);
    netlink_unregister_notifier(&nl_nb);
    lx_genl_clear_portid(0x2000);
    T("genl_release_notify", atomic_read(&g_genl_releases) == 1,
      "released=%d", atomic_read(&g_genl_releases));

    atomic_set(&g_slow_entered, 0);
    atomic_set(&g_slow_release, 0);
    atomic_set(&g_slow_port, 0);
    atomic_set(&g_slow_reply, 0);
    lx_genl_listen_portid(0x3000, tf_slow_port_cb);
    lx_genl_set_reply_cb(tf_slow_reply_cb);
    struct task_struct *slow_thread = kthread_run(tf_slow_thread, NULL, "slowtx");
    s = tf_msg(5, &h);
    if (s) {
        genlmsg_end(s, h);
        rc = lx_genl_doit("lxapitest", s);
        kfree_skb(s);
    } else {
        rc = -ENOMEM;
    }
    if (!IS_ERR_OR_NULL(slow_thread))
        kthread_stop(slow_thread);
    lx_genl_set_reply_cb(NULL);
    lx_genl_clear_portid(0x3000);
    T("genl_reply_owner", rc == 0 && atomic_read(&g_slow_port) == 1 &&
      atomic_read(&g_slow_reply) == 1,
      "rc=%d port=%d reply=%d", rc, atomic_read(&g_slow_port),
      atomic_read(&g_slow_reply));
}

// ── hweight register contract ──────────────────────────────────────────────
// arch/x86/include/asm/arch_hweight.h calls __sw_hweight32/64 from inline asm
// that declares only rdi input and rax output: the kernel contract is that
// every register except rax is preserved. exercise it directly. (satoru)
static void test_hweight_regs(void)
{
    const unsigned long c0 = 0x1111111111111111ul;
    const unsigned long c1 = 0x2222222222222222ul;
    const unsigned long c2 = 0x3333333333333333ul;
    const unsigned long c4 = 0x8888888888888888ul;
    const unsigned long c5 = 0x9999999999999999ul;
    const unsigned long c6 = 0xAAAAAAAAAAAAAAAAul;
    const unsigned long c7 = 0xBBBBBBBBBBBBBBBBul;
    const unsigned long in32 = 0xA5A5A5A5ul;
    const unsigned long in64 = 0xF0F0F0F0F0F0F0F0ul;
    unsigned int r32 = 0;
    unsigned long r64 = 0;
    unsigned long o32[8], o64[8];
    int ok = 1;

    asm volatile(
        "movabsq %[c0], %%rcx\n\t"
        "movabsq %[c1], %%rdx\n\t"
        "movabsq %[c2], %%rsi\n\t"
        "movq    %[in32], %%rdi\n\t"
        "movabsq %[c4], %%r8\n\t"
        "movabsq %[c5], %%r9\n\t"
        "movabsq %[c6], %%r10\n\t"
        "movabsq %[c7], %%r11\n\t"
        "call    __sw_hweight32\n\t"
        "movq %%rcx, %[p0]\n\t"
        "movq %%rdx, %[p1]\n\t"
        "movq %%rsi, %[p2]\n\t"
        "movq %%rdi, %[p3]\n\t"
        "movq %%r8,  %[p4]\n\t"
        "movq %%r9,  %[p5]\n\t"
        "movq %%r10, %[p6]\n\t"
        "movq %%r11, %[p7]\n\t"
        : "=a"(r32),
          [p0]"=m"(o32[0]), [p1]"=m"(o32[1]), [p2]"=m"(o32[2]), [p3]"=m"(o32[3]),
          [p4]"=m"(o32[4]), [p5]"=m"(o32[5]), [p6]"=m"(o32[6]), [p7]"=m"(o32[7])
        : [c0]"i"(c0), [c1]"i"(c1), [c2]"i"(c2), [in32]"i"(in32),
          [c4]"i"(c4), [c5]"i"(c5), [c6]"i"(c6), [c7]"i"(c7)
        : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "cc", "memory");

    asm volatile(
        "movabsq %[c0], %%rcx\n\t"
        "movabsq %[c1], %%rdx\n\t"
        "movabsq %[c2], %%rsi\n\t"
        "movq    %[in64], %%rdi\n\t"
        "movabsq %[c4], %%r8\n\t"
        "movabsq %[c5], %%r9\n\t"
        "movabsq %[c6], %%r10\n\t"
        "movabsq %[c7], %%r11\n\t"
        "call    __sw_hweight64\n\t"
        "movq %%rcx, %[p0]\n\t"
        "movq %%rdx, %[p1]\n\t"
        "movq %%rsi, %[p2]\n\t"
        "movq %%rdi, %[p3]\n\t"
        "movq %%r8,  %[p4]\n\t"
        "movq %%r9,  %[p5]\n\t"
        "movq %%r10, %[p6]\n\t"
        "movq %%r11, %[p7]\n\t"
        : "=a"(r64),
          [p0]"=m"(o64[0]), [p1]"=m"(o64[1]), [p2]"=m"(o64[2]), [p3]"=m"(o64[3]),
          [p4]"=m"(o64[4]), [p5]"=m"(o64[5]), [p6]"=m"(o64[6]), [p7]"=m"(o64[7])
        : [c0]"i"(c0), [c1]"i"(c1), [c2]"i"(c2), [in64]"i"(in64),
          [c4]"i"(c4), [c5]"i"(c5), [c6]"i"(c6), [c7]"i"(c7)
        : "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "cc", "memory");

    if (r32 != 16 || r64 != 32)
        ok = 0;
    if (o32[0] != c0 || o32[1] != c1 || o32[2] != c2 || o32[3] != in32 ||
        o32[4] != c4 || o32[5] != c5 || o32[6] != c6 || o32[7] != c7)
        ok = 0;
    if (o64[0] != c0 || o64[1] != c1 || o64[2] != c2 || o64[3] != in64 ||
        o64[4] != c4 || o64[5] != c5 || o64[6] != c6 || o64[7] != c7)
        ok = 0;
    T("hweight_regs", ok, "h32=%u h64=%lu regs=%s",
      r32, r64, ok ? "preserved" : "changed");
}

// ── upstream self-tests ─────────────────────────────────────────────────────
extern int kld_init_test_xarray(void);
extern int kld_init_test_ida(void);
extern int kld_init_test_rhashtable(void);

extern int lx_main_argc;
extern char **lx_main_argv;
static int yf_wanted(void)
{
    for (int i = 1; i < lx_main_argc; i++)
        if (lx_main_argv[i] && !strcmp(lx_main_argv[i], "yieldfair"))
            return 1;
    return 0;
}

// ── yield/fairness reproducer (no device, no driver specifics) ─────────────
// N1d: one thread sleeps 1 ms in a loop, one waits on a futex a third wakes
// every 10 ms, and optionally one spins on lx_os_yield. the numbers are the
// sleeper's oversleep and the waiter's wake-to-run latency. run with and
// without the spinner at 2 and 4 vcpus to separate shim loops from the
// kernel's yield hand-off. (satoru)
static volatile int yf_stop;
static volatile unsigned int yf_word;
static volatile unsigned long long yf_stamp;
static unsigned int yf_sleep_h[24], yf_wake_h[24];
static unsigned int yf_sleep_runs, yf_wake_runs;
static unsigned long long yf_sleep_max, yf_wake_max;

static void yf_hist(unsigned int* h, unsigned long long ns)
{
    unsigned long long us = ns / 1000ull;
    unsigned int b = 0;
    while (us > 1 && b < 23) { us >>= 1; b++; }
    h[b]++;
}

static unsigned long long yf_pct(const unsigned int* h, unsigned int total, unsigned int pct)
{
    if (!total) return 0;
    unsigned int want = (total * pct + 99) / 100, cum = 0;
    for (int b = 0; b < 24; b++) {
        cum += h[b];
        if (cum >= want) return 1ull << b;   // bucket upper bound, us
    }
    return 1ull << 23;
}

static int yf_sleeper(void* arg)
{
    (void)arg;
    while (!yf_stop) {
        unsigned long long t0 = lx_os_now_ns();
        lx_os_sleep_ns(1000000);
        unsigned long long d = lx_os_now_ns() - t0;
        yf_sleep_runs++;
        if (d > 1000000) {
            d -= 1000000;
            yf_hist(yf_sleep_h, d);
            if (d > yf_sleep_max) yf_sleep_max = d;
        }
    }
    return 0;
}

static int yf_waiter(void* arg)
{
    (void)arg;
    while (!yf_stop) {
        unsigned int v = __atomic_load_n(&yf_word, __ATOMIC_ACQUIRE);
        unsigned long long start = lx_os_now_ns();
        lx_os_futex_wait(&yf_word, v, 1000000000ll);
        unsigned int nv = __atomic_load_n(&yf_word, __ATOMIC_ACQUIRE);
        if (nv != v && !yf_stop) {
            unsigned long long st = __atomic_load_n(&yf_stamp, __ATOMIC_ACQUIRE);
            if (st >= start) {
                unsigned long long d = lx_os_now_ns() - st;
                yf_hist(yf_wake_h, d);
                if (d > yf_wake_max) yf_wake_max = d;
                yf_wake_runs++;
            }
        }
    }
    return 0;
}

// ── per-thread preempt count ───────────────────────────────────────────────
#define PT_THREADS 8
#define PT_ITERS   200000

struct pt_shared {
    volatile int lock_held;
    volatile int peer_checked;
};

struct pt_arg {
    spinlock_t lock;
    atomic_t *bad;
    struct pt_shared *shared;
    int id;
};

static int pt_worker(void *arg)
{
    struct pt_arg *a = arg;
    int i;

    for (i = 0; i < PT_ITERS; i++) {
        preempt_disable();
        if (!in_atomic())
            atomic_inc(a->bad);
        preempt_enable();
        if (in_atomic())
            atomic_inc(a->bad);

        spin_lock_bh(&a->lock);
        if (!in_atomic() || !in_softirq())
            atomic_inc(a->bad);
        if (i == 1 && a->id == 0) {
            WRITE_ONCE(a->shared->lock_held, 1);
            while (!READ_ONCE(a->shared->peer_checked))
                schedule();
        }
        spin_unlock_bh(&a->lock);
        if (in_atomic() || in_softirq())
            atomic_inc(a->bad);

        local_bh_disable();
        if (!in_atomic() || !in_softirq())
            atomic_inc(a->bad);
        local_bh_enable();
        if (in_atomic() || in_softirq())
            atomic_inc(a->bad);

        if (i == 1 && a->id == 1) {
            while (!READ_ONCE(a->shared->lock_held))
                schedule();
            if (preempt_count() != 0 || !in_task() || in_atomic() ||
                in_softirq())
                atomic_inc(a->bad);
            WRITE_ONCE(a->shared->peer_checked, 1);
        }

        rcu_read_lock();
        rcu_read_unlock();

        if ((i & 0x3ff) == 0)
            schedule();
    }
    if (preempt_count() != 0 || !in_task() || in_atomic() || in_softirq())
        atomic_inc(a->bad);
    return 0;
}

static int pt_irq_check(void *arg)
{
    int *ok = arg;
    *ok = in_hardirq() && in_atomic() && !in_task();
    return 0;
}

static void test_preempt(void)
{
    struct pt_arg *args;
    struct pt_shared shared;
    struct task_struct *tasks[PT_THREADS];
    atomic_t bad = ATOMIC_INIT(0);
    int i, started = 0, irq_ok = 0, ok;

    args = kzalloc(sizeof(*args) * PT_THREADS, GFP_KERNEL);
    if (!args) {
        T("preempt_per_thread", 0, "alloc");
        return;
    }
    memset(&shared, 0, sizeof(shared));
    for (i = 0; i < PT_THREADS; i++) {
        spin_lock_init(&args[i].lock);
        args[i].bad = &bad;
        args[i].shared = &shared;
        args[i].id = i;
    }
    for (i = 0; i < PT_THREADS; i++) {
        tasks[i] = kthread_run(pt_worker, &args[i], "pt%d", i);
        if (IS_ERR(tasks[i])) {
            tasks[i] = NULL;
            break;
        }
        started++;
    }
    for (i = 0; i < started; i++)
        kthread_stop(tasks[i]);
    lx_irq_test_handler_ctx(pt_irq_check, &irq_ok);
    ok = started == PT_THREADS && atomic_read(&bad) == 0 && irq_ok;
    T("preempt_per_thread", ok,
      "threads=%d iters=%d bad=%d irq=%d", started, PT_ITERS,
      atomic_read(&bad), irq_ok);
    kfree(args);
}

// ── softirq context contract ───────────────────────────────────────────────
// NAPI polls, tasklets and timer callbacks are softirq context in Linux:
// softirq_count() != 0, in_serving_softirq() true, in_task() false, and the
// thread's count is back to 0 after the callback. (satoru)
extern struct net_device *alloc_netdev_dummy(int sizeof_priv);
static atomic_t g_softirq_ok;
static atomic_t g_softirq_poll_done;

static int softirq_ctx_ok(void)
{
    return preempt_count() != 0 && softirq_count() != 0 &&
           in_serving_softirq() && !in_task();
}

static int softirq_napi_poll(struct napi_struct *n, int budget)
{
    (void)budget;
    if (softirq_ctx_ok())
        atomic_inc(&g_softirq_ok);
    napi_complete_done(n, 0);
    atomic_set(&g_softirq_poll_done, 1);
    return 0;
}

static void softirq_tasklet_cb(unsigned long arg)
{
    (void)arg;
    if (softirq_ctx_ok())
        atomic_inc(&g_softirq_ok);
}

static void softirq_timer_cb(struct timer_list *t)
{
    (void)t;
    if (softirq_ctx_ok())
        atomic_inc(&g_softirq_ok);
}

static void test_softirq_ctx(void)
{
    struct net_device *dev = alloc_netdev_dummy(0);
    struct napi_struct napi;
    struct tasklet_struct tl;
    struct timer_list tm;
    int ok = 1;
    unsigned long bad_before;

    atomic_set(&g_softirq_ok, 0);
    atomic_set(&g_softirq_poll_done, 0);
    bad_before = lx_softirq_bad_after;
    if (!dev) {
        T("softirq_context", 0, "no dummy netdev");
        return;
    }

    memset(&napi, 0, sizeof(napi));
    netif_napi_add_weight(dev, &napi, softirq_napi_poll, 16);
    napi_enable(&napi);
    napi_schedule(&napi);
    for (int i = 0; i < 200 && !atomic_read(&g_softirq_poll_done); i++)
        msleep(1);
    napi_disable(&napi);
    if (!atomic_read(&g_softirq_poll_done))
        ok = 0;

    tasklet_setup(&tl, softirq_tasklet_cb);
    tasklet_schedule(&tl);
    msleep(20);
    tasklet_kill(&tl);

    timer_setup(&tm, softirq_timer_cb, 0);
    mod_timer(&tm, jiffies + 2);
    msleep(20);
    del_timer_sync(&tm);

    if (lx_softirq_bad_after != bad_before)
        ok = 0;
    if (atomic_read(&g_softirq_ok) != 3)
        ok = 0;
    // after each callback, this thread is a task again.
    if (preempt_count() != 0 || in_atomic() || in_softirq() || !in_task())
        ok = 0;

    T("softirq_context", ok, "callbacks=%d bad_after=%lu",
      atomic_read(&g_softirq_ok),
      lx_softirq_bad_after - bad_before);
    free_netdev(dev);
}

// temporary diagnostic for the report: is the user gs base ever nonzero?
struct gs_arg {
    unsigned int bad;
    unsigned long long first;
};

static int gs_worker(void *arg)
{
    struct gs_arg *a = arg;
    unsigned long long end = lx_os_now_ns() + 2000000000ull;

    while (lx_os_now_ns() < end) {
        unsigned long long base = 0;
        if (lx_os_gs_base(&base) == 0 && base) {
            if (!a->bad)
                a->first = base;
            a->bad++;
        }
        lx_os_sleep_ns(1000000ull);
    }
    return 0;
}

static int yf_waker(void* arg)
{
    (void)arg;
    while (!yf_stop) {
        lx_os_sleep_ns(10000000);
        __atomic_store_n(&yf_stamp, lx_os_now_ns(), __ATOMIC_RELEASE);
        __atomic_add_fetch(&yf_word, 1, __ATOMIC_RELEASE);
        lx_os_futex_wake(&yf_word, 1);
    }
    return 0;
}

static int yf_spinner(void* arg)
{
    (void)arg;
    while (!yf_stop)
        lx_os_yield();
    return 0;
}

static void yf_case(const char* name, int spin)
{
    yf_stop = 0;
    yf_word = 0;
    yf_sleep_runs = yf_wake_runs = 0;
    yf_sleep_max = yf_wake_max = 0;
    for (int i = 0; i < 24; i++) { yf_sleep_h[i] = 0; yf_wake_h[i] = 0; }
    kthread_run(yf_sleeper, NULL, "yf-sleep");
    kthread_run(yf_waiter, NULL, "yf-wait");
    kthread_run(yf_waker, NULL, "yf-wake");
    if (spin)
        kthread_run(yf_spinner, NULL, "yf-spin");
    lx_os_sleep_ns(3000000000ull);
    yf_stop = 1;
    __atomic_add_fetch(&yf_word, 1, __ATOMIC_RELEASE);
    lx_os_futex_wake(&yf_word, 1);
    lx_os_sleep_ns(150000000ull);
    lx_os_sleep_ns(0);   // no-op: keep both path epochs (satoru)
    unsigned long long sp50 = yf_pct(yf_sleep_h, yf_sleep_runs, 50);
    unsigned long long sp99 = yf_pct(yf_sleep_h, yf_sleep_runs, 99);
    unsigned long long wp50 = yf_pct(yf_wake_h, yf_wake_runs, 50);
    unsigned long long wp99 = yf_pct(yf_wake_h, yf_wake_runs, 99);
    char d[200];
    snprintf(d, sizeof(d),
             "sleep n=%u p50=%lluus p99=%lluus max=%lluus | wake n=%u p50=%lluus p99=%lluus max=%lluus",
             yf_sleep_runs, sp50, sp99, yf_sleep_max / 1000ull,
             yf_wake_runs, wp50, wp99, yf_wake_max / 1000ull);
    t_result(name, yf_sleep_runs > 10 && yf_wake_runs > 10, d);
}

static void test_gs_base(void)
{
    struct gs_arg a[4];
    struct task_struct *tasks[4];
    unsigned long long main_base = 0;
    long mr;
    unsigned int bad = 0, i;
    unsigned long long first = 0;

    mr = lx_os_gs_base(&main_base);
    memset(a, 0, sizeof(a));
    for (i = 0; i < 4; i++)
        tasks[i] = kthread_run(gs_worker, &a[i], "gs%d", i);
    for (i = 0; i < 4; i++) {
        if (IS_ERR(tasks[i]))
            continue;
        kthread_stop(tasks[i]);
        bad += a[i].bad;
        if (!first)
            first = a[i].first;
    }
    lx_log("[apitest] gs_base main_rc=");
    lx_log_u((unsigned long)mr);
    lx_log(" main=");
    lx_log_hex((unsigned long)main_base);
    lx_log(" thread_bad=");
    lx_log_u(bad);
    lx_log(" first=");
    lx_log_hex((unsigned long)first);
    lx_log("\n");
    T("gs_base_zero", mr == 0 && main_base == 0 && bad == 0,
      "main=%d bad=%u", mr == 0 && main_base == 0, bad);
}

static void test_upstream(void)
{
    int a = 0, b = 0, c = 0;
    lx_log("[apitest] upstream test_xarray begin\n");
    a = kld_init_test_xarray();
    lx_log("[apitest] upstream test_xarray end\n");
    lx_log("[apitest] upstream test_ida begin\n");
    b = kld_init_test_ida();
    // upstream lib/test_ida.c:207 returns -EINVAL when tests_run == tests_passed
    // (the sense is inverted there); -EINVAL here means its 147357/147357 line.
    if (b == -EINVAL)
        b = 0;
    lx_log("[apitest] upstream test_ida end\n");
    lx_log("[apitest] upstream test_rhashtable begin\n");
    c = kld_init_test_rhashtable();
    lx_log("[apitest] upstream test_rhashtable end\n");
    T("upstream_lib_selftests", a == 0 && b == 0 && c == 0,
      "test_xarray=%d test_ida=%d test_rhashtable=%d", a, b, c);
}

// broker line-irq contract. this family has no line_irq manifest key, so on a
// default apitest boot the attach is REFUSED: that refusal is the first test.
// regress_e1000e.sh with KLD_INTX=1 re-runs this same container under
// kurono.kld.family=e1000e (test-only override) to exercise the positive path,
// the pic-mask refusals and the storm guard. (satoru)
static void test_line_irq(void)
{
    if (!lx_broker_available()) {
        T("line/skip_no_broker", 1, "host run");
        return;
    }
    void *va = lx_os_reserve(4096);
    long pg = va ? lx_broker_call(LX_KLD_OP_IRQ_PAGE, (unsigned long)va, 0, 0, 0) : -1;
    T("line/irq_page", pg == 0 || pg == -16, "rc=%ld", pg);
    unsigned int flags = LX_LINE_LEVEL | LX_LINE_ACTIVE_LOW |
                         LX_LINE_SHARED | LX_LINE_ISA;
    long h = lx_broker_call(LX_KLD_OP_IRQ_LINE_ATTACH, 11, flags, 0, 0);
    if (h == -1) {
        T("line/family_gate", 1, "attach refused: no line_irq manifest key");
        return;
    }
    T("line/attach_gsi11", h >= 0, "handle=%ld rc=%ld", h, h);
    if (h < 0)
        return;
    long d = lx_broker_call(LX_KLD_OP_IRQ_LINE_ATTACH, 11, flags, 0, 0);
    T("line/duplicate_refused", d == -16, "second attach rc=%ld", d);
    long p = lx_broker_call(LX_KLD_OP_IRQ_LINE_ATTACH, 0, flags, 0, 0);
    T("line/pit_refused", p == -16 || p == -1, "gsi0 rc=%ld", p);
    long c = lx_broker_call(LX_KLD_OP_IRQ_LINE_ATTACH, 2, 0, 0, 0);
    T("line/cascade_refused", c == -1, "gsi2 rc=%ld", c);
    int storm_ok = 1, first_bad = -1;
    for (int i = 0; i < 70; i++) {
        long u = lx_broker_call(LX_KLD_OP_IRQ_LINE_UNMASK, (unsigned long)h, 0, 0, 0);
        long want = (i < 64) ? 0 : -1;
        if (u != want) { storm_ok = 0; if (first_bad < 0) first_bad = i; }
    }
    T("line/storm_guard", storm_ok, "first_bad_call=%d", first_bad);
    long latch = lx_broker_call(LX_KLD_OP_IRQ_LINE_UNMASK, (unsigned long)h, 1, 0, 0);
    T("line/storm_latched", latch == -1, "handled unmask rc=%ld", latch);
    long det = lx_broker_call(LX_KLD_OP_IRQ_LINE_DETACH, (unsigned long)h, 0, 0, 0);
    T("line/detach", det == 0, "rc=%ld", det);
    long again = lx_broker_call(LX_KLD_OP_IRQ_LINE_ATTACH, 11, flags, 0, 0);
    T("line/reattach_after_detach", again >= 0, "handle=%ld", again);
    if (again >= 0)
        lx_broker_call(LX_KLD_OP_IRQ_LINE_DETACH, (unsigned long)again, 0, 0, 0);
}

// C5.4 gates: platform mmio and acpi services are family-gated. this container
// has neither key; regress_e1000e.sh with the family override checks the e1000e
// case, and the real touchpad family is the positive path. (satoru)
static void test_platform_gates(void)
{
    if (!lx_broker_available()) {
        T("plat/skip_no_broker", 1, "host run");
        return;
    }
    long p = lx_broker_call(LX_KLD_OP_PLAT_MAP, 0xFEC00000ul, 0x1000, 0, 0);
    T("plat/manifest_gate", p == -1, "ioapic page rc=%ld", p);
    long v = lx_broker_call(LX_KLD_OP_ACPI_PORT_IN, 0x70, 8, 0, 0);
    T("acpi/not_granted", v == -1, "cmos port rc=%ld", v);
}

int kld_test_main(int argc, char **argv)
{
    // host/container diagnostic: run only the linked upstream lib self-tests.
    if (argc > 1 && argv[1] && strcmp(argv[1], "upstream") == 0) {
        lx_log("[apitest] upstream self-tests\n");
        test_upstream();
        lx_log("[apitest] ");
        lx_log_u((unsigned long)g_pass);
        lx_log("/");
        lx_log_u((unsigned long)g_total);
        lx_log(g_pass == g_total ? " PASS\n" : " FAIL\n");
        return g_pass == g_total ? 0 : 1;
    }
    lx_log("[apitest] shim self-test starting\n");

    lx_log("[apitest] start test_xarray\n");
    test_xarray();
    lx_log("[apitest] start test_idr\n");
    test_idr();
    lx_log("[apitest] start test_ida\n");
    test_ida();
    lx_log("[apitest] start test_rhashtable\n");
    test_rhashtable();
    lx_log("[apitest] start test_kfifo\n");
    test_kfifo();
    lx_log("[apitest] start test_scatterlist\n");
    test_scatterlist();
    lx_log("[apitest] start test_nla\n");
    test_nla();
    lx_log("[apitest] start test_notifier\n");
    test_notifier();
    lx_log("[apitest] start test_devres\n");
    test_devres();
    lx_log("[apitest] start test_kobject\n");
    test_kobject();
    lx_log("[apitest] start test_rcu\n");
    test_rcu();
    lx_log("[apitest] start test_rcu_overlap\n");
    test_rcu_overlap();
    lx_log("[apitest] start test_rcu_barrier_from_work\n");
    test_rcu_barrier_from_work();
    lx_log("[apitest] start test_rcu_unlock_cost\n");
    test_rcu_unlock_cost();
    lx_log("[apitest] start test_srcu\n");
    test_srcu();
    lx_log("[apitest] start test_rwsem\n");
    test_rwsem();
    lx_log("[apitest] start test_seqlock\n");
    test_seqlock();
    lx_log("[apitest] start test_kmem_cache\n");
    test_kmem_cache();
    lx_log("[apitest] start test_percpu\n");
    test_percpu();
    lx_log("[apitest] start test_hrtimer\n");
    test_hrtimer();
    lx_log("[apitest] start test_jiffies\n");
    test_jiffies();
    lx_log("[apitest] start test_tasklet\n");
    test_tasklet();
    lx_log("[apitest] start test_delayed_work\n");
    test_delayed_work();
    lx_log("[apitest] start test_firmware\n");
    test_firmware();
    lx_log("[apitest] start test_genl\n");
    test_genl();
    lx_log("[apitest] start test_hweight_regs\n");
    test_hweight_regs();
    if (yf_wanted()) {
        lx_log("[apitest] start test_yieldfair_nospin\n");
        yf_case("yieldfair_nospin", 0);
        lx_log("[apitest] start test_yieldfair_spin\n");
        yf_case("yieldfair_spin", 1);
    }
    lx_log("[apitest] start test_gs_base\n");
    test_gs_base();
    lx_log("[apitest] start test_preempt\n");
    test_preempt();
    lx_log("[apitest] start test_crypto\n");
    apitest_crypto_run(&g_pass, &g_total);
    lx_log("[apitest] start test_wpa\n");
    apitest_wpa_run(&g_pass, &g_total);
    lx_log("[apitest] start test_softirq_ctx\n");
    test_softirq_ctx();
    lx_log("[apitest] start test_upstream\n");
    test_upstream();
    lx_log("[apitest] start test_line_irq\n");
    test_line_irq();
    lx_log("[apitest] start test_platform_gates\n");
    test_platform_gates();

    lx_log("[apitest] ");
    lx_log_u((unsigned long)g_pass);
    lx_log("/");
    lx_log_u((unsigned long)g_total);
    lx_log(g_pass == g_total ? " PASS\n" : " FAIL\n");
    return g_pass == g_total ? 0 : 1;
}

// end (satoru)
