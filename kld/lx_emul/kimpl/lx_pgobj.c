// lx_emul: page provider for drm objects whose memory a userspace process may map.
//
// the arena in lx_dma.c hands out container-private dma chunks that are reused and
// zeroed, so a process mapping such a page would see another object's data after
// the container freed the first. an OBJECT RUN here is backed by frames the kernel
// owns (KLD_OP_DRI_OBJ_ALLOC): refcounted per frame, released one at a time, alive
// as long as ANY side still holds one. the run is carved out of the same arena VA
// window (whole 256 KiB chunks, never shared with dma allocations) so struct page,
// page_address() and vmap() work exactly as for arena pages.
//
// API for every drm memory user: lx_pgobj_run / lx_pgobj_free / lx_pgobj_lookup.
// inactive (and the old arena path is used) unless the drm userspace interface is
// attached. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>

#include "lx_kshim.h"
#include "lx_dri.h"

extern long lx_broker_call(unsigned int op, unsigned long a0, unsigned long a1,
                           unsigned long a2, unsigned long a3);

struct lx_arena_view {
    unsigned long va, npages, chunk_pages;
    unsigned char *used;
    dma_addr_t *chunk_bus;
    struct page *pages;
    volatile unsigned int *lock;
};
extern void lx_arena_get_view(struct lx_arena_view *v);

#define LX_PGOBJ_RUNS 256

struct lx_run {
    int           used;
    unsigned long first;     // arena page index of the first page (satoru)
    unsigned long npages;
    unsigned long chunk0, nchunks;
    unsigned int  obj;
};

static struct lx_run g_runs[LX_PGOBJ_RUNS];
static volatile unsigned int g_pgobj_lock;
static int g_pgobj_on;

void lx_pgobj_enable(void) { g_pgobj_on = 1; }
int  lx_pgobj_active(void) { return g_pgobj_on; }

struct page *lx_pgobj_run(unsigned long pages)
{
    struct lx_arena_view v;
    unsigned long cp, nch, c, start = ~0ul;
    int slot = -1;
    if (!pages)
        return NULL;
    lx_arena_get_view(&v);
    cp = v.chunk_pages;
    nch = (pages + cp - 1) / cp;
    lx_flock(&g_pgobj_lock);
    for (int i = 0; i < LX_PGOBJ_RUNS; i++)
        if (!g_runs[i].used) { slot = i; break; }
    if (slot < 0) { lx_funlock(&g_pgobj_lock); return NULL; }
    lx_flock(v.lock);
    for (c = 0; c + nch <= v.npages / cp; c++) {
        unsigned long k, j;
        for (k = 0; k < nch; k++) {
            if (v.chunk_bus[c + k])
                break;
            for (j = 0; j < cp; j++)
                if (v.used[(c + k) * cp + j])
                    break;
            if (j < cp)
                break;
        }
        if (k == nch) { start = c; break; }
    }
    if (start == ~0ul) {
        lx_funlock(v.lock); lx_funlock(&g_pgobj_lock);
        lx_log("[lx_pgobj] no free arena chunks for an object run\n");
        return NULL;
    }
    for (unsigned long k = 0; k < nch; k++) {
        v.chunk_bus[start + k] = 1;     // not a dma chunk: nothing else may populate it (satoru)
        for (unsigned long j = 0; j < cp; j++) {
            v.used[(start + k) * cp + j] = 1;
            memset(&v.pages[(start + k) * cp + j], 0, sizeof(struct page));
        }
    }
    lx_funlock(v.lock);
    g_runs[slot].used = 1;       // reserved while the broker call runs (satoru)
    g_runs[slot].first = start * cp;
    g_runs[slot].npages = pages;
    g_runs[slot].chunk0 = start;
    g_runs[slot].nchunks = nch;
    g_runs[slot].obj = 0;
    lx_funlock(&g_pgobj_lock);

    long id = lx_broker_call(LX_KLD_OP_DRI_OBJ_ALLOC, v.va + start * cp * PAGE_SIZE, pages, 0, 0);
    lx_flock(&g_pgobj_lock);
    if (id <= 0) {
        lx_flock(v.lock);
        for (unsigned long k = 0; k < nch; k++) {
            v.chunk_bus[start + k] = 0;
            for (unsigned long j = 0; j < cp; j++)
                v.used[(start + k) * cp + j] = 0;
        }
        lx_funlock(v.lock);
        g_runs[slot].used = 0;
        lx_funlock(&g_pgobj_lock);
        lx_log("[lx_pgobj] OBJ_ALLOC failed\n");
        return NULL;
    }
    g_runs[slot].obj = (unsigned int)id;
    lx_funlock(&g_pgobj_lock);
    return &v.pages[start * cp];
}

// 0 when `first` was an object run (now released), -1 when it was not. (satoru)
int lx_pgobj_free(struct page *first, unsigned long pages)
{
    struct lx_arena_view v;
    unsigned long idx;
    if (!g_pgobj_on || !first)
        return -1;
    lx_arena_get_view(&v);
    idx = (unsigned long)(first - v.pages);
    lx_flock(&g_pgobj_lock);
    for (int i = 0; i < LX_PGOBJ_RUNS; i++) {
        struct lx_run r;
        if (!g_runs[i].used || g_runs[i].first != idx || !g_runs[i].obj)
            continue;
        r = g_runs[i];
        g_runs[i].used = 0;
        lx_funlock(&g_pgobj_lock);
        (void)pages;
        lx_broker_call(LX_KLD_OP_DRI_OBJ_FREE, r.obj, 0, 0, 0);
        lx_flock(v.lock);
        for (unsigned long k = 0; k < r.nchunks; k++) {
            v.chunk_bus[r.chunk0 + k] = 0;
            for (unsigned long j = 0; j < v.chunk_pages; j++)
                v.used[(r.chunk0 + k) * v.chunk_pages + j] = 0;
        }
        lx_funlock(v.lock);
        return 0;
    }
    lx_funlock(&g_pgobj_lock);
    return -1;
}

// which object and page index does this arena virtual address belong to? (satoru)
int lx_pgobj_lookup(unsigned long va, unsigned int *obj, unsigned int *index)
{
    struct lx_arena_view v;
    unsigned long idx;
    int rc = -1;
    lx_arena_get_view(&v);
    if (va < v.va || va >= v.va + v.npages * PAGE_SIZE)
        return -1;
    idx = (va - v.va) >> PAGE_SHIFT;
    lx_flock(&g_pgobj_lock);
    for (int i = 0; i < LX_PGOBJ_RUNS; i++) {
        if (g_runs[i].used && g_runs[i].obj && idx >= g_runs[i].first && idx < g_runs[i].first + g_runs[i].npages) {
            *obj = g_runs[i].obj;
            *index = (unsigned int)(idx - g_runs[i].first);
            rc = 0;
            break;
        }
    }
    lx_funlock(&g_pgobj_lock);
    return rc;
}

// end (satoru)
