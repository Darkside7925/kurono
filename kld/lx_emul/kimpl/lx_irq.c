// lx_emul: interrupts, delivered by the kernel broker.
//
// The device's MSI is programmed by the BROKER (the container may not write
// the MSI capability). Its isr does exactly one thing per interrupt: bump a
// u32 counter in a doorbell page mapped into this container, and futex-wake
// whoever waits on it. Here, one irq thread per requested irq waits on that
// counter and runs the driver's handler in "hard irq" context, which in a
// container means: on the irq thread, holding the irq's own lock. That lock is
// what disable_irq() / synchronize_irq() take to wait out a running handler.
//
// IRQ_WAKE_THREAD runs the threaded handler right after on the same thread,
// which preserves upstream's ordering guarantee (thread_fn after handler, never
// concurrently with the next hard handler for the same irq).
//
// Linux irq numbers handed to the driver are LX_IRQ_BASE + broker handle, so a
// stray irq number can never alias a real one.
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/msi.h>
#include <linux/device.h>
#include <linux/preempt.h>

#include "lx_kshim.h"
#include "lx_broker.h"

#define LX_IRQ_BASE 1000u
#define LX_IRQ_MAX  16
// software irqs for tests: slots 8..15, no broker handle. (satoru)
#define LX_IRQ_SOFT_FIRST 8
#define LX_LINE_MAX_ACT  4

// pci shim hooks (lx_pci.c); weak so a non-pci family still links. (satoru)
int lx_pci_irq_is_isa(unsigned int irq) __attribute__((weak));
int lx_pci_legacy_line(void) __attribute__((weak));

struct lx_line_action {
    irq_handler_t handler;
    irq_handler_t thread_fn;
    void *dev_id;
    const char *name;
};

struct lx_irq {
    int used;
    unsigned int irq;
    int handle;                     // broker msi handle (satoru)
    unsigned int bdf;
    irq_handler_t handler;
    irq_handler_t thread_fn;
    void *dev_id;
    const char *name;
    volatile int requested;
    volatile int disabled;          // disable_irq depth (satoru)
    volatile int pending;           // arrived while disabled: replay on enable (satoru)
    volatile int stop;
    volatile unsigned int lock;     // held while a handler runs (satoru)
    volatile unsigned int *counter; // in the broker doorbell page (satoru)
    struct lx_task *task;
    unsigned long count;
    unsigned int seen0;             // doorbell value when request_irq returned (satoru)
    unsigned long long t1;          // N1b irq-thread-woke TSC (satoru)
    volatile unsigned int soft_counter; // software irq source, not broker (satoru)
    int soft;
    // ── legacy line irq (broker IRQ_LINE_ATTACH) ───────────────────────────
    int line;
    unsigned int gsi;
    int nact;
    struct lx_line_action act[LX_LINE_MAX_ACT];
};

static struct lx_irq lx_irqs[LX_IRQ_MAX];
static volatile unsigned int *lx_doorbell;   // broker irq page (satoru)

unsigned long lx_irq_total(void)
{
    unsigned long sum = 0;
    for (int i = 0; i < (int)LX_IRQ_MAX; i++)
        sum += lx_doorbell ? lx_doorbell[i] : 0;
    return sum;
}
static __thread int lx_irq_ctx;

int lx_in_irq(void) { return lx_irq_ctx; }

unsigned long lx_irq_count(unsigned int irq)
{
    unsigned int h = irq - LX_IRQ_BASE;
    return h < LX_IRQ_MAX ? lx_irqs[h].count : 0;
}

static struct lx_irq *lx_irq_get(unsigned int irq);

// gpiolib-acpi asks whether the line's irq is free before requesting it; the
// shim tracks exactly one action per slot, so "not requested" is the answer. (satoru)
int can_request_irq(unsigned int irq, unsigned long irqflags)
{
    (void)irqflags;
    struct lx_irq *q = lx_irq_get(irq);
    if (!q)
        return irq < LX_IRQ_BASE;   // a line nobody attached yet is free (satoru)
    return !q->requested;
}

static int lx_irq_page_once(void)
{
    if (lx_doorbell)
        return 0;
    void *va = lx_os_reserve(4096);
    if (!va)
        return -ENOMEM;
    long r = lx_broker_call(LX_KLD_OP_IRQ_PAGE, (unsigned long)va, 0, 0, 0);
    if (r < 0) {
        lx_log("[lx_irq] broker IRQ_PAGE failed\n");
        return -ENODEV;
    }
    lx_doorbell = va;
    return 0;
}

static struct lx_irq *lx_irq_get(unsigned int irq)
{
    if (irq >= LX_IRQ_BASE) {
        unsigned int h = irq - LX_IRQ_BASE;
        if (h < LX_IRQ_MAX && lx_irqs[h].used)
            return &lx_irqs[h];
        return NULL;
    }
    // a legacy line keeps the driver's own irq number (the gsi), so it is
    // found by scan rather than by the LX_IRQ_BASE+handle rule. (satoru)
    for (unsigned int i = 0; i < LX_IRQ_MAX; i++)
        if (lx_irqs[i].used && lx_irqs[i].line && lx_irqs[i].irq == irq)
            return &lx_irqs[i];
    return NULL;
}

// a software interrupt line for tests (the hidsim i2c-hid model). the irq
// thread, request_threaded_irq, disable/enable and free_irq are the same as
// for a broker MSI; only the counter is a plain word the test raises. (satoru)
int lx_irq_alloc_soft(unsigned int *out_irq)
{
    for (unsigned int h = LX_IRQ_SOFT_FIRST; h < LX_IRQ_MAX; h++) {
        struct lx_irq *q = &lx_irqs[h];
        if (q->used)
            continue;
        memset(q, 0, sizeof(*q));
        q->used = 1;
        q->soft = 1;
        q->handle = -1;
        q->irq = LX_IRQ_BASE + h;
        q->counter = &q->soft_counter;
        if (out_irq)
            *out_irq = q->irq;
        return 0;
    }
    return -ENOSPC;
}

void lx_irq_raise_soft(unsigned int irq)
{
    struct lx_irq *q = lx_irq_get(irq);
    if (!q || !q->soft)
        return;
    __atomic_fetch_add(q->counter, 1, __ATOMIC_RELEASE);
    lx_os_futex_wake(q->counter, 0x7fffffff);
}

void lx_irq_free_soft(unsigned int irq)
{
    struct lx_irq *q = lx_irq_get(irq);
    if (!q || !q->soft)
        return;
    q->used = 0;
    q->soft = 0;
    q->counter = NULL;
}

// ── msi ─────────────────────────────────────────────────────────────────────
unsigned int lx_pci_bdf_of(const struct pci_dev *dev);

int pci_enable_msi(struct pci_dev *dev)
{
    unsigned int bdf = lx_pci_bdf_of(dev);
    if (bdf == 0xFFFFFFFFu)
        return -ENODEV;
    if (dev->msi_enabled)
        return -EINVAL;
    if (lx_irq_page_once())
        return -ENODEV;
    long h = lx_broker_call(LX_KLD_OP_MSI_ALLOC, bdf, 0, 0, 0);
    if (h < 0 || h >= LX_IRQ_MAX) {
        lx_log("[lx_irq] broker MSI_ALLOC refused\n");
        return -ENOSPC;
    }
    struct lx_irq *q = &lx_irqs[h];
    memset(q, 0, sizeof(*q));
    q->used = 1;
    q->handle = (int)h;
    q->irq = LX_IRQ_BASE + (unsigned int)h;
    q->bdf = bdf;
    q->counter = lx_doorbell + h;
    dev->irq = q->irq;
    dev->msi_enabled = 1;
    lx_log("[lx_irq] MSI enabled by the broker, linux irq ");
    lx_log_u(q->irq);
    lx_log("\n");
    return 0;
}

void pci_disable_msi(struct pci_dev *dev)
{
    struct lx_irq *q = lx_irq_get(dev->irq);
    if (!dev->msi_enabled || !q)
        return;
    lx_broker_call(LX_KLD_OP_MSI_FREE, (unsigned long)q->handle, 0, 0, 0);
    q->used = 0;
    dev->msi_enabled = 0;
    dev->irq = 0;
    // back to the legacy line the function was bound with, if any: a driver
    // that disables msi and keeps its irq number must find the line again. (satoru)
    if (lx_pci_legacy_line)
        dev->irq = lx_pci_legacy_line();
}

// MSI-X: the broker programs the table (in the device's msi-x bar) and gives
// each entry its own vector + doorbell counter; here each becomes a linux irq
// number in entries[i].vector, exactly what drivers then request_irq(). (satoru)
static int lx_msix_first = -1;
static int lx_msix_n;

int pci_enable_msix_range(struct pci_dev *dev, struct msix_entry *entries,
                          int minvec, int maxvec)
{
    unsigned int bdf = lx_pci_bdf_of(dev);
    if (bdf == 0xFFFFFFFFu || !entries || maxvec < minvec || minvec < 1)
        return -EINVAL;
    if (dev->msi_enabled || dev->msix_enabled)
        return -EINVAL;
    if (lx_irq_page_once())
        return -ENODEV;
    long first = lx_broker_call(LX_KLD_OP_MSIX_ALLOC, bdf, (unsigned long)maxvec, 0, 0);
    if (first < 0) {
        lx_log("[lx_irq] broker MSIX_ALLOC refused\n");
        return -ENOSPC;
    }
    long n = lx_broker_call(LX_KLD_OP_MSIX_COUNT, bdf, 0, 0, 0);
    if (n < minvec) {
        lx_broker_call(LX_KLD_OP_MSIX_FREE, bdf, 0, 0, 0);
        return -ENOSPC;
    }
    for (int i = 0; i < n; i++) {
        int h = (int)first + i;
        struct lx_irq *q = &lx_irqs[h];
        memset(q, 0, sizeof(*q));
        q->used = 1;
        q->handle = h;
        q->irq = LX_IRQ_BASE + (unsigned int)h;
        q->bdf = bdf;
        q->counter = lx_doorbell + h;
        entries[i].vector = q->irq;
    }
    lx_msix_first = (int)first;
    lx_msix_n = (int)n;
    dev->msix_enabled = 1;
    lx_log("[lx_irq] MSI-X enabled by the broker: ");
    lx_log_u((unsigned long)n);
    lx_log(" vectors, linux irqs from ");
    lx_log_u(LX_IRQ_BASE + (unsigned long)first);
    lx_log("\n");
    return (int)n;
}

void pci_disable_msix(struct pci_dev *dev)
{
    if (!dev->msix_enabled)
        return;
    lx_broker_call(LX_KLD_OP_MSIX_FREE, lx_pci_bdf_of(dev), 0, 0, 0);
    for (int i = 0; i < lx_msix_n; i++)
        lx_irqs[lx_msix_first + i].used = 0;
    lx_msix_n = 0;
    lx_msix_first = -1;
    dev->msix_enabled = 0;
}


// ── the irq thread ──────────────────────────────────────────────────────────
static void lx_irq_run(struct lx_irq *q)
{
    lx_flock(&q->lock);
    lx_irq_ctx = 1;
    q->count++;
    // the primary handler runs in hard irq context; the threaded half below
    // runs in process context, exactly like upstream. a request with a NULL
    // primary handler (i2c-hid does this) goes straight to the thread. (satoru)
    irqreturn_t r = IRQ_NONE;
    if (q->handler) {
        preempt_count_add(HARDIRQ_OFFSET);
        r = q->handler((int)q->irq, q->dev_id);
        preempt_count_sub(HARDIRQ_OFFSET);
    }
    if ((!q->handler || r == IRQ_WAKE_THREAD) && q->thread_fn)
        q->thread_fn((int)q->irq, q->dev_id);
    lx_irq_ctx = 0;
    // N1b t2: the driver's handler (and napi_schedule) has run. (satoru)
    if (q->t1) {
        unsigned long long t2 = lx_n1b_tsc();
        lx_n1b_hop(1, q->t1, t2);
        q->t1 = 0;
        lx_n1b_t2 = t2;
        lx_n1b_handlers++;
    }
    lx_funlock(&q->lock);
}

// a device model inside the container (the i915 register model) raises its
// device's interrupt: the same handler run as an msi edge, or pending while
// the line is disabled. (satoru)
int lx_irq_inject(unsigned int irq)
{
    struct lx_irq *q = lx_irq_get(irq);
    if (!q || !q->handler)
        return -EINVAL;
    if (__atomic_load_n(&q->disabled, __ATOMIC_ACQUIRE)) {
        q->pending = 1;
        return 0;
    }
    lx_irq_run(q);
    return 0;
}

// apitest hook: run fn in the same hard irq context the primary handler sees.
// lx_in_irq() and the hardirq bits are both set, and both are per thread.
// (satoru)
int lx_irq_test_handler_ctx(int (*fn)(void *), void *arg)
{
    int rc;

    lx_irq_ctx = 1;
    preempt_count_add(HARDIRQ_OFFSET);
    rc = fn(arg);
    preempt_count_sub(HARDIRQ_OFFSET);
    lx_irq_ctx = 0;
    return rc;
}

static void lx_line_run(struct lx_irq *q);

static int lx_irq_thread(void *arg)
{
    struct lx_irq *q = arg;
    // start from the value sampled IN request_irq, not when this thread first
    // runs: the handler is live the moment request_irq returns, and e1000e's
    // msi self-test fires its interrupt immediately after. sampling here lost
    // that edge and failed the test. (satoru)
    unsigned int seen = q->seen0;
    while (!__atomic_load_n(&q->stop, __ATOMIC_ACQUIRE)) {
        unsigned int cur = __atomic_load_n(q->counter, __ATOMIC_ACQUIRE);
        if (cur == seen) {
            // the broker isr wakes this futex by physical key; the kernel's
            // futex repoll (8 ms) is the backstop for a lost wake. (satoru)
            lx_os_futex_wait(q->counter, cur, -1);
            continue;
        }
        seen = cur;
        // N1b t1: this irq thread is back from its wait. t0 was stored by the
        // kernel isr in the doorbell page at 2048 + 8*handle. (satoru)
        {
            unsigned long long t1 = lx_n1b_tsc();
            volatile unsigned long long *t0p =
                (volatile unsigned long long *)((char *)lx_doorbell + 2048 + 8 * (unsigned)q->handle);
            q->t1 = t1;
            volatile unsigned int *tagp =
                (volatile unsigned int *)((char *)lx_doorbell + 3072 + 4 * (unsigned)q->handle);
            lx_n1b_hop_tag(0, *t0p, t1, (int)*tagp);
            lx_n1b_wakes++;
        }
        if (__atomic_load_n(&q->disabled, __ATOMIC_ACQUIRE)) {
            q->pending = 1;
            continue;
        }
        // several edges since we last looked collapse into one handler run,
        // exactly like an edge-triggered msi on linux: the handler reads the
        // device's cause register, which holds all of them. (satoru)
        if (q->line)
            lx_line_run(q);
        else
            lx_irq_run(q);
    }
    return 0;
}

struct lx_devm_irq {
    unsigned int irq;
    void *dev_id;
};

static void lx_devm_irq_release(void *p)
{
    struct lx_devm_irq *r = p;
    free_irq(r->irq, r->dev_id);
    kfree(r);
}

// affinity is meaningless with one shim cpu; report success. (satoru)
int __irq_apply_affinity_hint(unsigned int irq, const struct cpumask *m, bool effective)
{
    (void)irq; (void)m; (void)effective;
    return 0;
}

int devm_request_threaded_irq(struct device *dev, unsigned int irq,
                              irq_handler_t handler, irq_handler_t thread_fn,
                              unsigned long irqflags, const char *devname,
                              void *dev_id)
{
    struct lx_devm_irq *r = kmalloc(sizeof(*r), GFP_KERNEL);
    if (!r)
        return -ENOMEM;
    r->irq = irq;
    r->dev_id = dev_id;
    int rc = request_threaded_irq(irq, handler, thread_fn, irqflags, devname, dev_id);
    if (rc) {
        kfree(r);
        return rc;
    }
    rc = devm_add_action(dev, lx_devm_irq_release, r);
    if (rc) {
        free_irq(irq, dev_id);
        kfree(r);
        return rc;
    }
    return 0;
}

// ── legacy line irqs ────────────────────────────────────────────────────────
// one broker handle per gsi; several drivers may share it (IRQF_SHARED), in
// which case the irq thread runs every action in request order. (satoru)
struct lx_line_hint { unsigned int irq, flags; };
static struct lx_line_hint lx_line_hints[8];

void lx_irq_set_line_hint(unsigned int irq, unsigned int flags)
{
    for (int i = 0; i < 8; i++) {
        if (lx_line_hints[i].irq == irq || lx_line_hints[i].irq == 0) {
            lx_line_hints[i].irq = irq;
            lx_line_hints[i].flags = flags;
            return;
        }
    }
}

static unsigned int lx_line_hint(unsigned int irq)
{
    for (int i = 0; i < 8; i++)
        if (lx_line_hints[i].irq == irq)
            return lx_line_hints[i].flags;
    return 0;
}

static void lx_line_ack(struct lx_irq *q, int handled)
{
    if (q->handle < 0 || q->soft || !q->line)
        return;
    lx_broker_call(LX_KLD_OP_IRQ_LINE_UNMASK, (unsigned long)q->handle,
                   handled ? 1ul : 0ul, 0, 0);
}

static void lx_line_run(struct lx_irq *q)
{
    lx_flock(&q->lock);
    lx_irq_ctx = 1;
    q->count++;
    int handled = 0;
    for (int i = 0; i < q->nact; i++) {
        struct lx_line_action *a = &q->act[i];
        irqreturn_t r = IRQ_NONE;
        if (a->handler) {
            preempt_count_add(HARDIRQ_OFFSET);
            r = a->handler((int)q->irq, a->dev_id);
            preempt_count_sub(HARDIRQ_OFFSET);
        }
        if ((!a->handler || r == IRQ_WAKE_THREAD) && a->thread_fn)
            a->thread_fn((int)q->irq, a->dev_id);
        if (r != IRQ_NONE || a->thread_fn)
            handled = 1;
    }
    lx_irq_ctx = 0;
    lx_funlock(&q->lock);
    if (__atomic_load_n(&q->disabled, __ATOMIC_ACQUIRE)) {
        // the isr masked the level entry; the line stays masked until enable.
        // (satoru)
        q->pending = 1;
        return;
    }
    // called AFTER the handlers and outside the lock: for a level line this
    // unmasks the redirection entry, so a still-asserted source refires. (satoru)
    lx_line_ack(q, handled);
}

static int lx_line_request(unsigned int irq, irq_handler_t handler,
                           irq_handler_t thread_fn, unsigned long irqflags,
                           const char *devname, void *dev_id)
{
    struct lx_irq *q = lx_irq_get(irq);
    if (q && q->line) {
        if (q->nact >= LX_LINE_MAX_ACT)
            return -EBUSY;
        q->act[q->nact].handler = handler;
        q->act[q->nact].thread_fn = thread_fn;
        q->act[q->nact].dev_id = dev_id;
        q->act[q->nact].name = devname;
        q->nact++;
        lx_log("[lx_irq] shared line ");
        lx_log_u(irq);
        lx_log(" now has ");
        lx_log_u((unsigned long)q->nact);
        lx_log(" actions\n");
        return 0;
    }
    if (q)
        return -EBUSY;
    if (lx_irq_page_once())
        return -ENODEV;
    unsigned int flags = LX_LINE_SHARED;
    unsigned int hint = lx_line_hint(irq);
    if (hint)
        flags |= hint;
    else
        flags |= LX_LINE_LEVEL | LX_LINE_ACTIVE_LOW;
    if (lx_pci_irq_is_isa && lx_pci_irq_is_isa(irq))
        flags |= LX_LINE_ISA;
    long h = lx_broker_call(LX_KLD_OP_IRQ_LINE_ATTACH, irq, flags, 0, 0);
    if (h < 0 || h >= LX_IRQ_MAX) {
        lx_log("[lx_irq] broker IRQ_LINE_ATTACH refused for ");
        lx_log_u(irq);
        lx_log(" (flags ");
        lx_log_hex(flags);
        lx_log(")\n");
        return h == -16 ? -EBUSY
             : h == -19 ? -ENODEV
             : h == -28 ? -ENOSPC
             : -EINVAL;
    }
    q = &lx_irqs[h];
    memset(q, 0, sizeof(*q));
    q->used = 1;
    q->line = 1;
    q->handle = (int)h;
    q->irq = irq;
    q->gsi = irq;
    q->counter = lx_doorbell + h;
    q->act[0].handler = handler;
    q->act[0].thread_fn = thread_fn;
    q->act[0].dev_id = dev_id;
    q->act[0].name = devname;
    q->nact = 1;
    q->handler = handler;
    q->thread_fn = thread_fn;
    q->dev_id = dev_id;
    q->name = devname;
    q->requested = 1;
    q->seen0 = __atomic_load_n(q->counter, __ATOMIC_ACQUIRE);
    if (lx_irqdomain_request_resources(irq)) {
        lx_broker_call(LX_KLD_OP_IRQ_LINE_DETACH, (unsigned long)h, 0, 0, 0);
        q->used = 0;
        q->line = 0;
        q->requested = 0;
        return -EBUSY;
    }
    if (lx_irqdomain_activate(irq)) {
        lx_irqdomain_release_resources(irq);
        lx_broker_call(LX_KLD_OP_IRQ_LINE_DETACH, (unsigned long)h, 0, 0, 0);
        q->used = 0;
        q->line = 0;
        q->requested = 0;
        return -EBUSY;
    }
    q->task = lx_task_spawn(devname ? devname : "irq-line", lx_irq_thread, q);
    if (!q->task) {
        lx_irqdomain_deactivate(irq);
        lx_irqdomain_release_resources(irq);
        lx_broker_call(LX_KLD_OP_IRQ_LINE_DETACH, (unsigned long)h, 0, 0, 0);
        q->used = 0;
        q->line = 0;
        q->requested = 0;
        return -ENOMEM;
    }
    lx_log("[lx_irq] line irq ");
    lx_log_u(irq);
    lx_log(" '");
    lx_log(devname ? devname : "?");
    lx_log("' -> handle ");
    lx_log_u((unsigned long)h);
    lx_log(", flags ");
    lx_log_hex(flags);
    lx_log("\n");
    if (!(irqflags & IRQF_NO_AUTOEN))
        lx_line_ack(q, 1);
    return 0;
}

int request_threaded_irq(unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn,
                         unsigned long irqflags, const char *devname, void *dev_id)
{
    if (irq < LX_IRQ_BASE)
        return lx_line_request(irq, handler, thread_fn, irqflags, devname, dev_id);
    struct lx_irq *q = lx_irq_get(irq);
    (void)irqflags;
    if (!q) {
        lx_log("[lx_irq] request_irq for an irq the broker never allocated: ");
        lx_log_u(irq);
        lx_log("\n");
        return -EINVAL;
    }
    if (q->requested)
        return -EBUSY;
    q->handler = handler;
    q->thread_fn = thread_fn;
    q->dev_id = dev_id;
    q->name = devname;
    q->stop = 0;
    q->disabled = 0;
    q->pending = 0;
    q->requested = 1;
    q->count = 0;
    q->seen0 = __atomic_load_n(q->counter, __ATOMIC_ACQUIRE);
    // the irq core's request_resources step (gpiolib locks the line as IRQ). (satoru)
    if (lx_irqdomain_request_resources(irq)) {
        q->requested = 0;
        return -EBUSY;
    }
    if (lx_irqdomain_activate(irq)) {
        lx_irqdomain_release_resources(irq);
        q->requested = 0;
        return -EBUSY;
    }
    q->task = lx_task_spawn(devname ? devname : "irq", lx_irq_thread, q);
    if (!q->task) {
        lx_irqdomain_deactivate(irq);
        lx_irqdomain_release_resources(irq);
        q->requested = 0;
        return -ENOMEM;
    }
    lx_log("[lx_irq] request_irq ");
    lx_log_u(irq);
    lx_log(" '");
    lx_log(devname ? devname : "?");
    lx_log("' -> irq thread started\n");
    return 0;
}

const void *free_irq(unsigned int irq, void *dev_id)
{
    struct lx_irq *q = lx_irq_get(irq);
    if (!q || !q->requested)
        return NULL;
    if (q->line) {
        int idx = -1;
        for (int i = 0; i < q->nact; i++)
            if (q->act[i].dev_id == dev_id) { idx = i; break; }
        if (idx < 0)
            return NULL;
        const char *aname = q->act[idx].name;
        for (int i = idx; i + 1 < q->nact; i++)
            q->act[i] = q->act[i + 1];
        q->nact--;
        if (q->nact > 0) {
            lx_log("[lx_irq] free_irq ");
            lx_log_u(irq);
            lx_log(": ");
            lx_log_u((unsigned long)q->nact);
            lx_log(" shared action(s) left\n");
            return aname;
        }
        lx_irqdomain_deactivate(irq);
        lx_irqdomain_release_resources(irq);
        lx_broker_call(LX_KLD_OP_IRQ_LINE_DETACH, (unsigned long)q->handle, 0, 0, 0);
        lx_log("[lx_irq] free_irq line ");
        lx_log_u(irq);
        lx_log(": handle ");
        lx_log_u((unsigned long)q->handle);
        lx_log(" detached, handler runs ");
        lx_log_u(q->count);
        lx_log("\n");
        __atomic_store_n(&q->stop, 1, __ATOMIC_RELEASE);
        __atomic_fetch_add(q->counter, 1, __ATOMIC_RELEASE);
        lx_os_futex_wake(q->counter, 0x7fffffff);
        lx_task_join(q->task);
        q->task = NULL;
        q->requested = 0;
        q->line = 0;
        q->used = 0;
        q->nact = 0;
        q->handler = NULL;
        q->thread_fn = NULL;
        return aname;
    }
    if (q->dev_id != dev_id)
        return NULL;
    lx_irqdomain_deactivate(irq);
    lx_irqdomain_release_resources(irq);
    const char *name = q->name;
    lx_log("[lx_irq] free_irq ");
    lx_log_u(irq);
    lx_log(": doorbell delta ");
    lx_log_u(__atomic_load_n(q->counter, __ATOMIC_ACQUIRE) - q->seen0);
    lx_log(" handler runs ");
    lx_log_u(q->count);
    if (!q->soft) {
        lx_log(" isr hits ");
        lx_log_u((unsigned long)lx_broker_call(LX_KLD_OP_STATUS, 1ul + (unsigned long)q->handle, 0, 0, 0));
    }
    lx_log("\n");
    // stop the thread: flag it, then nudge its futex word ourselves. the page is
    // ours to write; the broker only ever increments it. (satoru)
    __atomic_store_n(&q->stop, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(q->counter, 1, __ATOMIC_RELEASE);
    lx_os_futex_wake(q->counter, 0x7fffffff);
    // JOIN the thread: the slot is reused by the next pci_enable_msi, and a
    // thread still alive at that point ran the NEXT handler on a stale edge -
    // which made e1000e's msi self-test pass without any interrupt. (satoru)
    lx_task_join(q->task);
    q->task = NULL;
    q->requested = 0;
    q->handler = NULL;
    q->thread_fn = NULL;
    return name;
}

void synchronize_irq(unsigned int irq)
{
    struct lx_irq *q = lx_irq_get(irq);
    if (!q || lx_in_irq())
        return;
    lx_flock(&q->lock);
    lx_funlock(&q->lock);
}

void disable_irq_nosync(unsigned int irq)
{
    struct lx_irq *q = lx_irq_get(irq);
    if (q)
        __atomic_fetch_add(&q->disabled, 1, __ATOMIC_ACQ_REL);
}

void disable_irq(unsigned int irq)
{
    disable_irq_nosync(irq);
    synchronize_irq(irq);
}

bool disable_hardirq(unsigned int irq)
{
    disable_irq_nosync(irq);
    return true;
}

void enable_irq(unsigned int irq)
{
    struct lx_irq *q = lx_irq_get(irq);
    if (!q)
        return;
    if (__atomic_sub_fetch(&q->disabled, 1, __ATOMIC_ACQ_REL) <= 0) {
        q->disabled = 0;
        if (q->line) {
            // a level line is still asserted after its delivery, so unmasking
            // is the whole replay; an edge entry was never masked. (satoru)
            q->pending = 0;
            lx_line_ack(q, 1);
            return;
        }
        if (q->pending) {
            // an edge arrived while masked: replay it, as upstream does for
            // edge irqs (IRQS_PENDING -> resend). (satoru)
            q->pending = 0;
            __atomic_fetch_add(q->counter, 1, __ATOMIC_RELEASE);
            lx_os_futex_wake(q->counter, 1);
        }
    }
}

// end (satoru)
