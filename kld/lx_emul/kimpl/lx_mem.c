// lx_emul: memory allocation.
//
// Backed by musl's malloc. GFP flags are honoured only where they change
// OBSERVABLE behaviour: __GFP_ZERO must zero, and an atomic allocation must not
// sleep (malloc does not sleep, so that holds trivially).
//
// DMA-capable memory is NOT provided here: lx_dma.c gets it from the broker.
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/shrinker.h>

#include "lx_kshim.h"

// musl entry points, declared by hand: including <stdlib.h> here would collide
// with the kernel headers. (satoru)
extern void *malloc(unsigned long);
extern void *calloc(unsigned long, unsigned long);
extern void  free(void *);
extern void *memset(void *, int, unsigned long);
extern size_t malloc_usable_size(void *);

// ── slab ────────────────────────────────────────────────────────────────────
void *__kmalloc_noprof(size_t size, gfp_t flags)
{
    void *p = malloc(size ? size : 1);
    if (p && (flags & __GFP_ZERO))
        memset(p, 0, size);
    return p;
}

size_t kmalloc_size_roundup(size_t size)
{
    // devres only needs an upper bound for its size accounting. (satoru)
    return size;
}

void *__kmalloc_node_noprof(size_t size, gfp_t flags, int node)
{
    (void)node;
    return __kmalloc_noprof(size, flags);
}

void *__kmalloc_node_track_caller_noprof(size_t size, gfp_t flags, int node,
                                         unsigned long caller)
{
    (void)node; (void)caller;
    return __kmalloc_noprof(size, flags);
}

void *__kmalloc_large_noprof(size_t size, gfp_t flags)
{
    return __kmalloc_noprof(size, flags);
}

void *__kmalloc_cache_noprof(struct kmem_cache *s, gfp_t flags, size_t size)
{
    (void)s;
    return __kmalloc_noprof(size, flags);
}

size_t ksize(const void *p)
{
    if (!p)
        return 0;
    return malloc_usable_size((void *)p);
}

void kfree_sensitive(const void *p)
{
    if (!p)
        return;
    memset((void *)p, 0, ksize(p));
    free((void *)p);
}

void kfree(const void *p)
{
    free((void *)p);
}

// ── kmem_cache ──────────────────────────────────────────────────────────────
// struct kmem_cache is opaque in the public headers. This shim's caches are a
// size/align/ctor record in front of the same allocator; the constructor runs
// on every allocation, which is stricter than SLUB's "once per slab" (and
// exactly what a driver expects to have happened before it sees the object).
// (satoru)
struct kmem_cache {
    size_t size;
    unsigned int align;
    void (*ctor)(void *);
    slab_flags_t flags;
    char *name;
};

struct kmem_cache *__kmem_cache_create_args(const char *name,
                                            unsigned int object_size,
                                            struct kmem_cache_args *args,
                                            slab_flags_t flags)
{
    struct kmem_cache *s = malloc(sizeof(*s));
    size_t n;
    if (!s)
        return NULL;
    memset(s, 0, sizeof(*s));
    s->size = object_size ? object_size : 1;
    s->align = args ? args->align : 0;
    s->ctor = args ? args->ctor : NULL;
    s->flags = flags;
    n = strlen(name) + 1;
    s->name = malloc(n);
    if (s->name)
        memcpy(s->name, name, n);
    return s;
}

void *kmem_cache_alloc_noprof(struct kmem_cache *s, gfp_t flags)
{
    void *p;
    if (!s)
        return NULL;
    p = s->align ? lx_os_aligned_alloc(s->align, s->size) : malloc(s->size);
    if (p && (flags & __GFP_ZERO))
        memset(p, 0, s->size);
    if (p && s->ctor)
        s->ctor(p);
    return p;
}

void *kmem_cache_alloc_lru_noprof(struct kmem_cache *s, struct list_lru *lru,
                                  gfp_t flags)
{
    (void)lru;
    return kmem_cache_alloc_noprof(s, flags);
}

void kmem_cache_free(struct kmem_cache *s, void *objp)
{
    (void)s;
    free(objp);
}

void kmem_cache_destroy(struct kmem_cache *s)
{
    if (!s)
        return;
    free(s->name);
    free(s);
}

// ── percpu ──────────────────────────────────────────────────────────────────
// the shim reports one cpu today (__cpu_possible_mask has one bit), so one
// copy is exactly "one per shim cpu" and per_cpu_ptr() adds a zero offset.
// (satoru)
void *pcpu_alloc_noprof(size_t size, size_t align, bool reserved, gfp_t gfp)
{
    (void)reserved; (void)gfp;
    // pcpu_alloc always returns zeroed memory upstream; mac80211 reads
    // pcpu_rx_stats (last_rx, last_rate, packets) before any frame wrote them.
    // (satoru)
    void *p = lx_os_aligned_alloc(align ? align : sizeof(void *), size ? size : 1);
    if (p)
        memset(p, 0, size ? size : 1);
    return p;
}

void free_percpu(void *p)
{
    lx_os_free(p);
}

// ── vmalloc ─────────────────────────────────────────────────────────────────
// no separate vmalloc arena in the container: same allocator, different name.
// is_vmalloc_addr() therefore has to answer "no" for everything, which is
// correct here because nothing is in a vmalloc range. (satoru)
void *vmalloc_noprof(unsigned long size)
{
    return malloc(size ? size : 1);
}

void *kvmemdup(const void *src, size_t len, gfp_t gfp)
{
    void *p = __kmalloc_noprof(len, gfp);
    if (p && src)
        memcpy(p, src, len);
    return p;
}

void *vzalloc_noprof(unsigned long size)
{
    void *p = malloc(size ? size : 1);
    if (p)
        memset(p, 0, size);
    return p;
}

void vfree(const void *p)
{
    free((void *)p);
}

// ── pages ───────────────────────────────────────────────────────────────────
// alloc_pages / __free_pages / __folio_put moved to lx_dma.c: pages now come
// from a broker-backed arena with a real struct page array, so page_address()
// and dma_map_page() agree. (satoru)

bool is_vmalloc_addr(const void *x)
{
    (void)x;
    return false;
}

// ── MMIO ────────────────────────────────────────────────────────────────────
// ioremap/iounmap moved to lx_pci.c: they map a granted BAR through the
// broker. (satoru)


// ── ttm page-allocator probes ───────────────────────────────────────────────
// ttm sizes its global pool and shrinker from si_meminfo; the container owns a
// 2 GiB guest, so report the whole guest as ram and half of it free. the pool
// only uses this for a cap, not for an allocation decision. (satoru)
void si_meminfo(struct sysinfo *val)
{
    if (!val) return;
    memset(val, 0, sizeof(*val));
    val->totalram = (2ull << 30) / 4096ull;
    val->freeram  = val->totalram / 2;
    val->sharedram = 0;
    val->bufferram = 0;
    val->totalswap = 0;
    val->freeswap = 0;
    val->procs = 1;
    val->totalhigh = 0;
    val->freehigh = 0;
    val->mem_unit = 1;
}

// the shrinker is registered so ttm's pool counts can be seen by an mm that
// would shrink them; this container has no such mm, so registration is inert.
// (satoru)
struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...)
{
    (void)flags; (void)fmt;
    return (struct shrinker *)kzalloc(sizeof(struct shrinker), GFP_KERNEL);
}

void shrinker_register(struct shrinker *shrinker) { (void)shrinker; }
void shrinker_free(struct shrinker *shrinker) { kfree(shrinker); }

// end (satoru)
