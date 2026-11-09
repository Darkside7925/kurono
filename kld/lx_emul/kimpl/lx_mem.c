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

#include "lx_kshim.h"

// musl entry points, declared by hand: including <stdlib.h> here would collide
// with the kernel headers. (satoru)
extern void *malloc(unsigned long);
extern void *calloc(unsigned long, unsigned long);
extern void  free(void *);
extern void *memset(void *, int, unsigned long);

// ── slab ────────────────────────────────────────────────────────────────────
void *__kmalloc_noprof(size_t size, gfp_t flags)
{
    void *p = malloc(size ? size : 1);
    if (p && (flags & __GFP_ZERO))
        memset(p, 0, size);
    return p;
}

void *__kmalloc_cache_noprof(struct kmem_cache *s, gfp_t flags, size_t size)
{
    (void)s;
    return __kmalloc_noprof(size, flags);
}

void kfree(const void *p)
{
    free((void *)p);
}

// ── vmalloc ─────────────────────────────────────────────────────────────────
// no separate vmalloc arena in the container: same allocator, different name.
// is_vmalloc_addr() therefore has to answer "no" for everything, which is
// correct here because nothing is in a vmalloc range. (satoru)
void *vmalloc_noprof(unsigned long size)
{
    return malloc(size ? size : 1);
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

// end (satoru)
