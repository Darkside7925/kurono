// container side of the display bridge: map the ring and the READ-ONLY
// scanout, report the DRM client's modes, and serve damage frames by copying
// them into the dumb framebuffer and flushing. one frame is in flight; the
// kernel waits for our DONE or drops it. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/string.h>

#include "lx_kshim.h"
#include "lx_broker.h"
#include "lx_display.h"

extern int kdrm_serve_copy(const unsigned char* src, unsigned int spitch,
                           int x, int y, int w, int h);
extern int kdrm_serve_set_mode(unsigned int index);
extern void kdrm_serve_modes_done(void);

// only the drmcirrus family links kdrm_client.c; the display bridge source is
// shared with every other family, so provide no-op fallbacks. the strong
// definitions in the drm family override these. (satoru)
__attribute__((weak)) int kdrm_serve_copy(const unsigned char* src,
                                          unsigned int spitch, int x, int y,
                                          int w, int h)
{
    (void)src; (void)spitch; (void)x; (void)y; (void)w; (void)h;
    return -1;
}
__attribute__((weak)) int kdrm_serve_set_mode(unsigned int index)
{
    (void)index;
    return -1;
}
__attribute__((weak)) void kdrm_serve_modes_done(void) {}

// the serve thread reads these while the test rig detaches and re-attaches, so
// they are accessed atomically; between a detach and the next attach the kernel
// may unmap the old ring, and nothing may touch it then. (satoru)
static struct lx_disp_ring* volatile g_ring;
static unsigned char* volatile g_scanout;
static void* g_ring_va;
static void* g_scanout_va;
static unsigned int g_width, g_height, g_pitch;
static unsigned int g_current;
static int g_serving;
static struct lx_disp_mode g_modes[LX_DISP_MAX_MODES];
static unsigned int g_mode_count;
static volatile unsigned int g_pause_req;   // main: park the serve thread (satoru)
static volatile unsigned int g_pause_ack;   // serve: parked (satoru)
static volatile unsigned int g_exit_req;    // main: the serve thread exits (test) (satoru)

static int exit_thread(void* arg)
{
    (void)arg;
    lx_os_sleep_ns(200000000);
    lx_os_exit(0);
    return 0;
}

// stop the serve thread before the kernel may unmap the ring, and wait until it
// has really left the ring. the kernel detaches on DETACH/exit only after this
// returns, so no thread of ours can touch a freed mapping. (satoru)
static void display_park(void)
{
    if (!g_serving)
        return;
    __atomic_store_n(&g_pause_req, 1u, __ATOMIC_RELEASE);
    struct lx_disp_ring* r = __atomic_load_n(&g_ring, __ATOMIC_ACQUIRE);
    if (r)
        lx_os_futex_wake(&r->doorbell, 0x7fffffff);
    lx_os_futex_wake(&g_pause_req, 0x7fffffff);
    while (!__atomic_load_n(&g_pause_ack, __ATOMIC_ACQUIRE))
        lx_os_sleep_ns(1000000);
}

static void display_unpark(void)
{
    if (!g_serving)
        return;
    __atomic_store_n(&g_pause_req, 0u, __ATOMIC_RELEASE);
    lx_os_futex_wake(&g_pause_req, 0x7fffffff);
    while (__atomic_load_n(&g_pause_ack, __ATOMIC_ACQUIRE))
        lx_os_sleep_ns(1000000);
}

static int die_thread(void* arg)
{
    (void)arg;
    lx_os_sleep_ns(150000000);
    lx_log("[lx_display] diepre: exiting before the gui publishes setup\n");
    lx_os_exit(0);
    return 0;
}

static int stall_timeout_thread(void* arg)
{
    (void)arg;
    lx_os_sleep_ns(6000000000ull);
    lx_log("[lx_display] stall timeout: exiting so the supervisor restarts\n");
    lx_os_exit(0);
    return 0;
}

static int fuzz_thread(void* arg)
{
    unsigned int n = 0;
    (void)arg;
    for (;;) {
        unsigned int real = g_mode_count;
        n++;
        g_ring->mode_count = (n & 1) ? 0xffffffffu : real;
        g_ring->modes[g_current].width = (n & 1) ? 0xffffffffu : g_modes[g_current].width;
        g_ring->modes[g_current].height = (n & 2) ? 0xffffffffu : g_modes[g_current].height;
        g_ring->modes[g_current].pitch = (n & 4) ? 0xffffffffu : g_modes[g_current].pitch;
        lx_os_sleep_ns(200000);
    }
    return 0;
}

int lx_display_try_attach(void)
{
    // reserve the two windows once and re-attach into them: the kernel unmaps
    // them on every release, so unreserving between cycles would drop the refs
    // twice. (satoru)
    if (!g_ring_va)
        g_ring_va = lx_os_reserve(LX_DISP_RING_PAGES * 4096u);
    if (!g_scanout_va)
        g_scanout_va = lx_os_reserve(LX_DISP_SCANOUT_PAGES * 4096u);
    if (!g_ring_va || !g_scanout_va)
        return -1;
    long rc = lx_broker_call(LX_KLD_OP_DISPLAY_ATTACH,
                             (unsigned long)g_ring_va,
                             (unsigned long)g_scanout_va,
                             LX_DISP_RING_PAGES, LX_DISP_SCANOUT_PAGES);
    if (rc < 0)
        return -1;
    // a failed attach must never clobber a ring the kernel may still serve.
    // (satoru)
    struct lx_disp_ring* r = (struct lx_disp_ring*)g_ring_va;
    unsigned char* s = (unsigned char*)g_scanout_va;
    r->magic = LX_DISP_MAGIC;
    r->version = LX_DISP_VERSION;
    __atomic_store_n(&g_scanout, s, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ring, r, __ATOMIC_RELEASE);
    if (__atomic_load_n(&r->diepre, __ATOMIC_ACQUIRE)) {
        // deterministic pre-setup kill test: never report modes. (satoru)
        lx_log("[lx_display] diepre: exiting before modes\n");
        lx_os_exit(0);
    }
    return 0;
}

int lx_display_report_modes(const struct lx_disp_mode* modes, unsigned int count,
                            unsigned int current)
{
    if (!g_ring || !modes || !count || count > LX_DISP_MAX_MODES || current >= count)
        return -1;
    if (g_mode_count == 0) {
        for (unsigned int i = 0; i < count; i++)
            g_modes[i] = modes[i];
        g_mode_count = count;
    }
    for (unsigned int i = 0; i < count; i++)
        g_ring->modes[i] = modes[i];
    g_width = g_ring->modes[current].width;
    g_height = g_ring->modes[current].height;
    g_pitch = g_ring->modes[current].pitch;
    g_current = current;
    __atomic_store_n(&g_ring->mode_count, count, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_ring->modes_seq, 1u, __ATOMIC_RELEASE);
    long rc = lx_broker_call(LX_KLD_OP_DISPLAY_MODES, 0, 0, 0, 0);
    return rc < 0 ? (int)rc : 0;
}

static void serve_done(struct lx_disp_ring* ring, unsigned int seq)
{
    ring->done_seq = seq;
    __atomic_add_fetch(&ring->done_count, 1u, __ATOMIC_RELEASE);
    lx_broker_call(LX_KLD_OP_DISPLAY_DONE, seq, 0, 0, 0);
}

static int display_thread(void* arg)
{
    unsigned int seen = 0;
    (void)arg;
    for (;;) {
        if (__atomic_load_n(&g_exit_req, __ATOMIC_ACQUIRE))
            lx_os_exit(0);
        if (__atomic_load_n(&g_pause_req, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&g_pause_ack, 1u, __ATOMIC_RELEASE);
            lx_os_futex_wake(&g_pause_req, 1);
            while (__atomic_load_n(&g_pause_req, __ATOMIC_ACQUIRE))
                lx_os_futex_wait(&g_pause_req, 1u, -1);
            __atomic_store_n(&g_pause_ack, 0u, __ATOMIC_RELEASE);
            seen = 0;
            continue;
        }
        struct lx_disp_ring* ring = __atomic_load_n(&g_ring, __ATOMIC_ACQUIRE);
        if (!ring) {
            lx_os_futex_wait(&g_pause_req, 0u, -1);
            continue;
        }
        unsigned int now = __atomic_load_n(&ring->doorbell, __ATOMIC_ACQUIRE);
        if (now == seen) {
            lx_os_futex_wait(&ring->doorbell, seen, -1);
            continue;
        }
        seen = now;
        unsigned int prod = __atomic_load_n(&ring->down_prod, __ATOMIC_ACQUIRE);
        unsigned int cons = ring->down_cons;
        while (cons < prod) {
            struct lx_disp_down* d = &ring->down[cons % LX_DISP_DOWN_SLOTS];
            if (d->generation == ring->generation) {
                if (d->kind == LX_DISP_DOWN_FRAME) {
                    static unsigned int frames_seen;
                    static int stalled_logged;
                    if (__atomic_load_n(&ring->stall, __ATOMIC_ACQUIRE) &&
                        frames_seen++ >= 3) {
                        if (!stalled_logged) {
                            stalled_logged = 1;
                            lx_log("[lx_display] stall: not acknowledging frames\n");
                        }
                        cons++;
                        __atomic_store_n(&ring->down_cons, cons, __ATOMIC_RELEASE);
                        continue;
                    }
                    frames_seen++;
                    if (d->nr == 0) {
                        kdrm_serve_copy(__atomic_load_n(&g_scanout, __ATOMIC_ACQUIRE), g_pitch, 0, 0,
                                        (int)g_width, (int)g_height);
                    } else {
                        for (unsigned int i = 0; i < d->nr && i < LX_DISP_MAX_RECTS; i++)
                            kdrm_serve_copy(__atomic_load_n(&g_scanout, __ATOMIC_ACQUIRE), g_pitch,
                                            d->rects[i].x, d->rects[i].y,
                                            (int)d->rects[i].w, (int)d->rects[i].h);
                    }
                    serve_done(ring, d->seq);
                } else if (d->kind == LX_DISP_DOWN_SET_MODE) {
                    unsigned int idx = d->mode_index;
                    if (__atomic_load_n(&ring->modedelay, __ATOMIC_ACQUIRE))
                        lx_os_sleep_ns(1500000000ull);
                    if (idx < g_mode_count && kdrm_serve_set_mode(idx) == 0) {
                        g_current = idx;
                        g_width = g_modes[idx].width;
                        g_height = g_modes[idx].height;
                        g_pitch = g_modes[idx].pitch;
                    }
                    serve_done(ring, d->seq);
                } else if (d->kind == LX_DISP_DOWN_EXIT) {
                    lx_log("[lx_display] exit requested by the kernel\n");
                    lx_os_exit(0);
                }
            } else if (d->kind == LX_DISP_DOWN_FRAME) {
                serve_done(ring, d->seq);   // acknowledge a stale frame so the kernel moves on
            }
            cons++;
            __atomic_store_n(&ring->down_cons, cons, __ATOMIC_RELEASE);
        }
    }
    return 0;
}

static void log_rc(const char *what, long rc)
{
    lx_log(what);
    if (rc < 0) { lx_log("-"); lx_log_u((unsigned long)(-rc)); }
    else lx_log_u((unsigned long)rc);
    lx_log(" ");
}

int lx_display_start_serve(void)
{
    if (!g_ring)
        return -1;
    if (g_serving)
        return 0;

    if (__atomic_load_n(&g_ring->setupfail, __ATOMIC_ACQUIRE)) {
        // the kernel forces the next setup to fail and releases the instance,
        // which unmaps the ring below a live container. do not start a thread
        // that would touch it; the ops after the release go through the broker
        // only. then exit so the runner's restart loop re-attaches cleanly.
        // (satoru)
        lx_log("[lx_display] setupfail: the kernel will abort this setup\n");
        lx_os_sleep_ns(2000000000ull);
        long d = lx_broker_call(LX_KLD_OP_DISPLAY_DONE, 1, 0, 0, 0);
        long h = lx_broker_call(LX_KLD_OP_DISPLAY_HOTPLUG, 1, 0, 0, 0);
        long e = lx_broker_call(LX_KLD_OP_DISPLAY_ERROR, 7, 0, 0, 0);
        long x = lx_broker_call(LX_KLD_OP_DISPLAY_DETACH, 0, 0, 0, 0);
        lx_log("[lx_display] setupfail ops done=");
        log_rc("", d);
        log_rc("", h);
        log_rc("", e);
        log_rc("", x);
        lx_log("exit\n");
        lx_task_spawn("kld-display-exit", exit_thread, 0);
        g_serving = 1;
        return 0;
    }

    struct lx_task* t = lx_task_spawn("kld-display", display_thread, 0);
    if (!t)
        return -1;
    g_serving = 1;
    if (__atomic_load_n(&g_ring->attachloop, __ATOMIC_ACQUIRE)) {
        lx_log("[lx_display] attachloop: 200 hostile detach/attach cycles\n");
        unsigned long refusals = 0;
        int ok = 1;
        for (int i = 0; i < 200 && ok; i++) {
            // park before the kernel may unmap the old ring; the serve thread
            // must not touch a mapping the gui frees. (satoru)
            display_park();
            __atomic_store_n(&g_ring, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&g_scanout, 0, __ATOMIC_RELEASE);
            lx_broker_call(LX_KLD_OP_DISPLAY_DETACH, 0, 0, 0, 0);
            ok = 0;
            for (int r = 0; r < 400; r++) {
                if (lx_display_try_attach() == 0) { ok = 1; break; }
                refusals++;
                lx_os_sleep_ns(20000000);
            }
            display_unpark();
            lx_os_sleep_ns(1000000000ull);
        }
        lx_log("[lx_display] attachloop done refusals=");
        lx_log_u(refusals);
        lx_log("/200\n");
        // exit from the serve thread: exit_group then kills the leader, the
        // deferred reaper destroys the address space and its mapping ref is
        // returned, so the pmm after the loop can be flat. (satoru)
        if (!ok) {
            __atomic_store_n(&g_ring, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&g_scanout, 0, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&g_exit_req, 1u, __ATOMIC_RELEASE);
        __atomic_store_n(&g_pause_req, 0u, __ATOMIC_RELEASE);
        lx_os_futex_wake(&g_pause_req, 1);
        return 0;
    }
    if (__atomic_load_n(&g_ring->badunmap, __ATOMIC_ACQUIRE)) {
        // the broker's frames are not the container's to unmap or discard. a
        // refused call returns -13 (eacces); 100 cycles, then a clean exit so
        // the release and the pmm can be counted. (satoru)
        lx_log("[lx_display] badunmap: 100 hostile munmap/madvise cycles\n");
        unsigned long refused = 0, calls = 0;
        long last = 0;
        const unsigned long rb = LX_DISP_RING_PAGES * 4096u;
        const unsigned long sb = LX_DISP_SCANOUT_PAGES * 4096u;
        for (int i = 0; i < 100; i++) {
            long r;
            calls++; r = lx_os_munmap_rc(g_ring_va, rb);      if (r < 0) { refused++; last = r; }
            calls++; r = lx_os_munmap_rc(g_scanout_va, sb);   if (r < 0) { refused++; last = r; }
            calls++; r = lx_os_madvise_rc(g_scanout_va, sb, 4); if (r < 0) { refused++; last = r; }
            calls++; r = lx_os_madvise_rc(g_ring_va, rb, 8);  if (r < 0) { refused++; last = r; }
            calls++; r = lx_os_madvise_rc(g_scanout_va, sb, 9); if (r < 0) { refused++; last = r; }
            // an overflowing range must be refused, not wrapped past the check.
            // (satoru)
            calls++; r = lx_os_munmap_rc((void*)(~0ull & ~0xFFFull), ~0ull - 0xFFFull); if (r < 0) { refused++; last = r; }
            calls++; r = lx_os_madvise_rc((void*)(~0ull & ~0xFFFull), ~0ull - 0xFFFull, 4); if (r < 0) { refused++; last = r; }
        }
        lx_log("[lx_display] badunmap refused=");
        lx_log_u(refused);
        lx_log("/");
        lx_log_u(calls);
        lx_log(" last=");
        log_rc("", last);
        lx_log("exit\n");
        lx_os_sleep_ns(1000000000ull);
        lx_task_spawn("kld-display-exit", exit_thread, 0);
        return 0;
    }
    if (__atomic_load_n(&g_ring->badcount, __ATOMIC_ACQUIRE)) {
        unsigned long refused = 0;
        for (int i = 0; i < 100; i++) {
            long rc = lx_broker_call(LX_KLD_OP_DISPLAY_ATTACH,
                                     (unsigned long)g_ring_va,
                                     (unsigned long)g_scanout_va,
                                     64, 0xFFFFFu);
            if (rc < 0) refused++;
        }
        lx_log("[lx_display] badcount refused=");
        lx_log_u(refused);
        lx_log("/100\n");
    }
    if (__atomic_load_n(&g_ring->fuzz, __ATOMIC_ACQUIRE))
        lx_task_spawn("kld-display-fuzz", fuzz_thread, 0);
    if (__atomic_load_n(&g_ring->diepre, __ATOMIC_ACQUIRE))
        lx_task_spawn("kld-display-die", die_thread, 0);
    if (__atomic_load_n(&g_ring->stall, __ATOMIC_ACQUIRE))
        lx_task_spawn("kld-display-stall", stall_timeout_thread, 0);
    lx_log("[lx_display] serving the kernel ring\n");
    return 0;
}

int lx_display_active(void) { return g_ring != 0 && g_serving; }

// end (satoru)
