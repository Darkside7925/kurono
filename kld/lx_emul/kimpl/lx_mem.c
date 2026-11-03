// lx_emul: memory allocation.
//
// Backed by musl's malloc. GFP flags are honoured only where they change
// OBSERVABLE behaviour: __GFP_ZERO must zero, and an atomic allocation must not
// sleep (malloc does not sleep, so that holds trivially).
//
// DMA-capable memory is deliberately NOT provided here. dma_alloc_* must go
// through the kernel broker so the buffer is physically contiguous and
// IOMMU-mapped; handing back malloc memory would appear to work and then
// corrupt at the first device access. Those stay stubbed until the broker exists.
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
// The driver uses alloc_pages for RX buffers. Returning a real struct page is
// awkward without a mem_map, so hand back a small allocated descriptor whose
// virtual address is recoverable. This is enough for the non-DMA paths; the
// moment a page is handed to the device it must come from the broker instead.
// (satoru)
struct page *alloc_pages_noprof(gfp_t gfp, unsigned int order)
{
    unsigned long bytes = PAGE_SIZE << order;
    struct page *pg = (struct page *)calloc(1, sizeof(struct page));
    void *mem;

    if (!pg)
        return NULL;
    mem = malloc(bytes);
    if (!mem) {
        free(pg);
        return NULL;
    }
    if (gfp & __GFP_ZERO)
        memset(mem, 0, bytes);
    // stash the backing pointer where page_address() style helpers can find it.
    // virtual is only present with CONFIG_HIGHMEM-ish configs, so use private.
    pg->private = (unsigned long)mem;
    return pg;
}

void __folio_put(struct folio *folio)
{
    struct page *pg = (struct page *)folio;
    if (!pg)
        return;
    if (pg->private)
        free((void *)pg->private);
    free(pg);
}

bool is_vmalloc_addr(const void *x)
{
    (void)x;
    return false;
}

// ── MMIO ────────────────────────────────────────────────────────────────────
// A container cannot map device MMIO itself - that is the kernel broker's job
// (BAR mapping IPC). Refusing loudly is the only honest answer: returning a
// malloc'd buffer would make every register read return garbage that looks like
// a dead device. (satoru)
void __iomem *ioremap(phys_addr_t offset, size_t size)
{
    (void)size;
    lx_log("[lx_emul] ioremap(");
    lx_log_u((unsigned long)offset);
    lx_log(") - needs the kernel BAR-mapping broker (phase 2)\n");
    lx_unimpl("ioremap");
}

void iounmap(volatile void __iomem *addr)
{
    (void)addr;
}

// end (satoru)
