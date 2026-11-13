// i915 register model group 6 (d4.1b): vblank interrupts and the pipe crc.
// a model thread is the display engine's frame clock: every 16.67 ms, for
// each pipe whose transcoder is enabled, it counts a frame (PIPE_FRMCOUNT),
// latches the vblank bit in DE_PIPE_IIR when it is unmasked and enabled, and
// with PIPE_CRC_CTL enabled computes the pipe crc of the plane. when an iir
// holds a pending bit and both master enables are set, it raises the device's
// interrupt through the shim's interrupt path (lx_irq_inject), exactly where
// the msi would land.
// the crc: the plane's PLANE_SURF ggtt offset is walked through the ggtt ptes
// the driver wrote (bar0 + 8 MiB); only if every pte maps the registered
// scanout object's pages in order are the visible rows hashed (fnv-1a over
// 32-bit words, not the hardware crc polynomial); otherwise the result is
// 0xbad0bad0 and the mismatch is logged. what this does NOT prove: real
// interrupt timing, msi routing on the device, hardware crc values. (satoru)

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>

#include "kimpl/lx_kshim.h"

u32 i915m_peek(unsigned long off);
u64 i915m_peek64(unsigned long off);
void i915m_poke(unsigned long off, u32 v);
void i915m_lock(void);
void i915m_unlock(void);
extern int lx_irq_inject(unsigned int irq);

#define GSM_OFF        0x800000ul
#define PIPE_VBLANK    (1u << 0)
#define CRC_ENABLE     (1u << 31)

static volatile u32 g_frame[4];
static unsigned int g_irq;
static volatile int g_thread_on;

// the scanout object the family registered (its cpu view and page dma
// addresses). (satoru)
static const u8 *g_sc_va;
static const u64 *g_sc_dma;
static unsigned long g_sc_pages;
static u32 g_sc_pitch;
static unsigned int g_crc_logged;

u32 i915_model_frame(int pipe) { return (pipe >= 0 && pipe < 4) ? g_frame[pipe] : 0; }

void i915_model_set_scanout(const void *va, const u64 *dma, unsigned long pages, u32 pitch)
{
    g_sc_va = va;
    g_sc_dma = dma;
    g_sc_pages = pages;
    g_sc_pitch = pitch;
}

static u32 pipe_crc(int p)
{
    u32 surf = i915m_peek(0x7019c + 0x1000 * p) & ~0xfffu;     // PLANE_SURF(pipe, 1)
    u32 src = i915m_peek(0x6001c + 0x1000 * p);               // PIPESRC: w-1 << 16 | h-1
    u32 w = (src >> 16) + 1, h = (src & 0xffff) + 1;
    u32 hash = 2166136261u;
    unsigned long first = surf >> 12, need;
    if (!g_sc_va || !g_sc_dma)
        return 0xbad0bad0u;
    need = ((unsigned long)g_sc_pitch * h + 4095) / 4096;
    if (need > g_sc_pages)
        return 0xbad0bad0u;
    for (unsigned long i = 0; i < need; i++) {
        u64 pte = i915m_peek64(GSM_OFF + (first + i) * 8);
        if (!(pte & 1) || (pte & 0x0000fffffffff000ull) != g_sc_dma[i]) {
            if (g_crc_logged++ < 4) {
                char b[128];
                snprintf(b, sizeof(b), "[i915m] crc: ggtt page %lu of surf 0x%x maps 0x%llx, scanout page is 0x%llx\n",
                         i, surf, (unsigned long long)pte, (unsigned long long)g_sc_dma[i]);
                lx_log(b);
            }
            return 0xbad0bad0u;
        }
    }
    for (u32 y = 0; y < h; y++) {
        const u32 *row = (const u32 *)(g_sc_va + (unsigned long)y * g_sc_pitch);
        for (u32 x = 0; x < w; x++)
            hash = (hash ^ row[x]) * 16777619u;
    }
    return hash;
}

static int vblank_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int raise = 0;
        lx_os_sleep_ns(16666667ull);
        for (int p = 0; p < 4; p++) {
            u32 conf = i915m_peek(0x70008 + 0x1000 * p);
            u32 crc_ctl = i915m_peek(0x60050 + 0x1000 * p);
            u32 crc = 0;
            if (!(conf >> 31))
                continue;
            if (crc_ctl & CRC_ENABLE)
                crc = pipe_crc(p);          // outside the lock: reads memory only
            i915m_lock();
            g_frame[p]++;
            if (crc_ctl & CRC_ENABLE)
                i915m_poke(0x60064 + 0x1000 * p, crc);     // PIPE_CRC_RES_1
            u32 ier = i915m_peek(0x4440c + 0x10 * p), imr = i915m_peek(0x44404 + 0x10 * p);
            if ((ier & ~imr) & PIPE_VBLANK)
                i915m_poke(0x44408 + 0x10 * p, i915m_peek(0x44408 + 0x10 * p) | PIPE_VBLANK);
            if (i915m_peek(0x44408 + 0x10 * p))
                raise = 1;
            i915m_unlock();
        }
        if (raise && g_irq && (i915m_peek(0x190008) >> 31) && (i915m_peek(0x44200) >> 31))
            lx_irq_inject(g_irq);
    }
    return 0;
}

// called by the family once the driver requested its interrupt. (satoru)
void i915_model_irq_start(unsigned int irq)
{
    g_irq = irq;
    if (!__atomic_exchange_n(&g_thread_on, 1, __ATOMIC_ACQ_REL)) {
        lx_task_spawn("i915-model-vblank", vblank_thread, NULL);
        lx_log("[i915m] vblank thread on (60 Hz per enabled pipe)\n");
    }
}

// end (satoru)
