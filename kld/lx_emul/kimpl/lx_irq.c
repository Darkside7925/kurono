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

#include "lx_kshim.h"
#include "lx_broker.h"

#define LX_IRQ_BASE 1000u
#define LX_IRQ_MAX  8

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
};

static struct lx_irq lx_irqs[LX_IRQ_MAX];
static volatile unsigned int *lx_doorbell;   // broker irq page (satoru)
static __thread int lx_irq_ctx;

int lx_in_irq(void) { return lx_irq_ctx; }

unsigned long lx_irq_count(unsigned int irq)
{
    unsigned int h = irq - LX_IRQ_BASE;
    return h < LX_IRQ_MAX ? lx_irqs[h].count : 0;
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
    unsigned int h = irq - LX_IRQ_BASE;
    if (h >= LX_IRQ_MAX || !lx_irqs[h].used)
        return NULL;
    return &lx_irqs[h];
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
    dev->irq = 0;   // no legacy INTx line is brokered (satoru)
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
    irqreturn_t r = q->handler ? q->handler((int)q->irq, q->dev_id) : IRQ_NONE;
    if (r == IRQ_WAKE_THREAD && q->thread_fn)
        q->thread_fn((int)q->irq, q->dev_id);
    lx_irq_ctx = 0;
    lx_funlock(&q->lock);
}

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
        if (__atomic_load_n(&q->disabled, __ATOMIC_ACQUIRE)) {
            q->pending = 1;
            continue;
        }
        // several edges since we last looked collapse into one handler run,
        // exactly like an edge-triggered msi on linux: the handler reads the
        // device's cause register, which holds all of them. (satoru)
        lx_irq_run(q);
    }
    return 0;
}

int request_threaded_irq(unsigned int irq, irq_handler_t handler, irq_handler_t thread_fn,
                         unsigned long irqflags, const char *devname, void *dev_id)
{
    struct lx_irq *q = lx_irq_get(irq);
    (void)irqflags;
    if (!q) {
        lx_log("[lx_irq] request_irq for an irq the broker never allocated: ");
        lx_log_u(irq);
        lx_log(" (legacy INTx is not brokered yet)\n");
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
    q->task = lx_task_spawn(devname ? devname : "irq", lx_irq_thread, q);
    if (!q->task) {
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
    if (!q || !q->requested || q->dev_id != dev_id)
        return NULL;
    const char *name = q->name;
    lx_log("[lx_irq] free_irq ");
    lx_log_u(irq);
    lx_log(": doorbell delta ");
    lx_log_u(__atomic_load_n(q->counter, __ATOMIC_ACQUIRE) - q->seen0);
    lx_log(" handler runs ");
    lx_log_u(q->count);
    lx_log(" isr hits ");
    lx_log_u((unsigned long)lx_broker_call(LX_KLD_OP_STATUS, 1ul + (unsigned long)q->handle, 0, 0, 0));
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
