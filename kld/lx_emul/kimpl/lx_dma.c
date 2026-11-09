// lx_emul: DMA and the page allocator.
//
// Three kinds of memory a device can see, all from broker-owned frames (the
// container never learns a physical address any other way):
//
//  1. COHERENT  dma_alloc_coherent -> one broker DMA_ALLOC per allocation. x86
//     DMA is cache coherent, so a write-back mapping is correct.
//  2. PAGES     alloc_pages() comes from a 64 MiB VA arena populated on demand
//     in 256 KiB broker chunks, with a REAL struct page array behind it.
//     vmemmap_base is set so the inline virt_to_page()/page_address() maths
//     (page_offset_base = 0, so __pa(x) == x) lands in that array. A page
//     handed to dma_map_page() is mapped directly, zero copy.
//  3. BOUNCE    anything else (kmalloc'd skb heads, stack buffers) is mapped
//     through a bounce pool, the swiotlb design Linux itself uses when there is
//     no IOMMU: map copies TO_DEVICE data in, unmap/sync_for_cpu copies
//     FROM_DEVICE data out. One memcpy per packet, correct for every driver.
//
// Without an IOMMU the bus address equals the physical address; under VFIO or
// VT-d it is the IOVA the broker reports. Nothing here assumes which. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/dma-mapping.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/scatterlist.h>
#include <linux/string.h>

#include "lx_kshim.h"
#include "lx_broker.h"

#define LX_CHUNK_PAGES  64u
#define LX_CHUNK_BYTES  (LX_CHUNK_PAGES * PAGE_SIZE)

static volatile unsigned int lx_dma_lock;

static long lx_broker_dma_alloc(void *va, unsigned long pages, unsigned int bits)
{
    return lx_broker_call(LX_KLD_OP_DMA_ALLOC, (unsigned long)va, pages, bits, 0);
}

static unsigned int lx_mask_bits(u64 mask)
{
    if (!mask)
        return 32;
    return 64u - (unsigned int)__builtin_clzll(mask);
}

// ── 1. coherent ─────────────────────────────────────────────────────────────
#define LX_MAX_COHERENT 128
static struct { void *va; unsigned long pages; dma_addr_t bus; } lx_coh[LX_MAX_COHERENT];

void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle,
                      gfp_t flag, unsigned long attrs)
{
    (void)flag; (void)attrs;
    unsigned long pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 mask = dev ? dev->coherent_dma_mask : DMA_BIT_MASK(32);
    void *va = lx_os_reserve(pages * PAGE_SIZE);
    if (!va)
        return NULL;
    long bus = lx_broker_dma_alloc(va, pages, lx_mask_bits(mask));
    if (bus < 0) {
        lx_log("[lx_dma] coherent alloc refused by the broker\n");
        lx_os_unreserve(va, pages * PAGE_SIZE);
        return NULL;
    }
    lx_flock(&lx_dma_lock);
    for (int i = 0; i < LX_MAX_COHERENT; i++) {
        if (!lx_coh[i].va) {
            lx_coh[i].va = va;
            lx_coh[i].pages = pages;
            lx_coh[i].bus = (dma_addr_t)bus;
            break;
        }
    }
    lx_funlock(&lx_dma_lock);
    *dma_handle = (dma_addr_t)bus;
    return va;   // broker frames are zeroed (satoru)
}

void dma_free_attrs(struct device *dev, size_t size, void *cpu_addr,
                    dma_addr_t dma_handle, unsigned long attrs)
{
    (void)dev; (void)size; (void)dma_handle; (void)attrs;
    unsigned long pages = 0;
    lx_flock(&lx_dma_lock);
    for (int i = 0; i < LX_MAX_COHERENT; i++) {
        if (lx_coh[i].va == cpu_addr) {
            pages = lx_coh[i].pages;
            lx_coh[i].va = NULL;
            break;
        }
    }
    lx_funlock(&lx_dma_lock);
    if (!pages)
        return;
    lx_broker_call(LX_KLD_OP_DMA_FREE, (unsigned long)cpu_addr, 0, 0, 0);
    lx_os_unreserve(cpu_addr, pages * PAGE_SIZE);
}

int dma_set_mask(struct device *dev, u64 mask)
{
    if (!dev || !dev->dma_mask)
        return -EIO;
    *dev->dma_mask = mask;
    return 0;
}

int dma_set_coherent_mask(struct device *dev, u64 mask)
{
    if (!dev)
        return -EIO;
    dev->coherent_dma_mask = mask;
    return 0;
}

// ── 2. the page arena ───────────────────────────────────────────────────────
#define LX_ARENA_BYTES  (64ul << 20)
#define LX_ARENA_PAGES  (LX_ARENA_BYTES / PAGE_SIZE)
#define LX_ARENA_CHUNKS (LX_ARENA_PAGES / LX_CHUNK_PAGES)

static unsigned long lx_arena_va;
static dma_addr_t lx_arena_chunk_bus[LX_ARENA_CHUNKS];
static unsigned char lx_arena_used[LX_ARENA_PAGES];
static unsigned char lx_arena_order[LX_ARENA_PAGES];
static struct page lx_page_array[LX_ARENA_PAGES];

void lx_dma_init(void)
{
    if (lx_arena_va)
        return;
    lx_arena_va = (unsigned long)lx_os_reserve(LX_ARENA_BYTES + LX_CHUNK_BYTES);
    // align the arena to a chunk so chunk index = offset / 256 KiB. (satoru)
    lx_arena_va = (lx_arena_va + LX_CHUNK_BYTES - 1) & ~(unsigned long)(LX_CHUNK_BYTES - 1);
    // the inline virt_to_page(x) is vmemmap + (x >> PAGE_SHIFT) because
    // page_offset_base == 0. point vmemmap so that the arena's first page maps
    // onto lx_page_array[0]. addresses outside the arena compute a struct page
    // pointer nobody may dereference - which is why only arena pages are ever
    // treated as pages. (satoru)
    vmemmap_base = (unsigned long)lx_page_array -
                   (lx_arena_va >> PAGE_SHIFT) * sizeof(struct page);
}

static int lx_arena_chunk_ready(unsigned long chunk)
{
    if (lx_arena_chunk_bus[chunk])
        return 1;
    void *va = (void *)(lx_arena_va + chunk * LX_CHUNK_BYTES);
    long bus = lx_broker_dma_alloc(va, LX_CHUNK_PAGES, 32);
    if (bus < 0)
        return 0;
    lx_arena_chunk_bus[chunk] = (dma_addr_t)bus;
    return 1;
}

static int lx_va_in_arena(unsigned long va)
{
    return lx_arena_va && va >= lx_arena_va && va < lx_arena_va + LX_ARENA_BYTES;
}

static dma_addr_t lx_arena_bus(unsigned long va)
{
    unsigned long off = va - lx_arena_va;
    return lx_arena_chunk_bus[off / LX_CHUNK_BYTES] + (off % LX_CHUNK_BYTES);
}

struct page *alloc_pages_noprof(gfp_t gfp, unsigned int order)
{
    unsigned long n = 1ul << order;
    if (n > LX_CHUNK_PAGES)
        return NULL;   // a block may not straddle two broker chunks (satoru)
    lx_dma_init();
    lx_flock(&lx_dma_lock);
    for (unsigned long i = 0; i + n <= LX_ARENA_PAGES; i += n) {
        unsigned long j;
        for (j = 0; j < n; j++)
            if (lx_arena_used[i + j])
                break;
        if (j < n)
            continue;
        if (!lx_arena_chunk_ready(i / LX_CHUNK_PAGES)) {
            lx_funlock(&lx_dma_lock);
            return NULL;
        }
        for (j = 0; j < n; j++) {
            lx_arena_used[i + j] = 1;
            memset(&lx_page_array[i + j], 0, sizeof(struct page));
        }
        lx_arena_order[i] = (unsigned char)order;
        set_page_count(&lx_page_array[i], 1);
        lx_funlock(&lx_dma_lock);
        if (gfp & __GFP_ZERO)
            memset((void *)(lx_arena_va + i * PAGE_SIZE), 0, n * PAGE_SIZE);
        return &lx_page_array[i];
    }
    lx_funlock(&lx_dma_lock);
    lx_log("[lx_dma] page arena exhausted\n");
    return NULL;
}

static void lx_arena_free(struct page *pg)
{
    unsigned long i = (unsigned long)(pg - lx_page_array);
    if (i >= LX_ARENA_PAGES)
        return;   // not ours: never touch it (satoru)
    lx_flock(&lx_dma_lock);
    unsigned long n = 1ul << lx_arena_order[i];
    for (unsigned long j = 0; j < n && i + j < LX_ARENA_PAGES; j++)
        lx_arena_used[i + j] = 0;
    lx_funlock(&lx_dma_lock);
}

void __free_pages(struct page *page, unsigned int order)
{
    (void)order;
    if (page && put_page_testzero(page))
        lx_arena_free(page);
}

void free_pages(unsigned long addr, unsigned int order)
{
    if (addr)
        __free_pages(virt_to_page((void *)addr), order);
}

unsigned long get_free_pages_noprof(gfp_t gfp_mask, unsigned int order)
{
    struct page *p = alloc_pages_noprof(gfp_mask, order);
    return p ? (unsigned long)page_address(p) : 0;
}

unsigned long get_zeroed_page_noprof(gfp_t gfp_mask)
{
    return get_free_pages_noprof(gfp_mask | __GFP_ZERO, 0);
}

// put_page() on the last reference ends up here. (satoru)
void __folio_put(struct folio *folio)
{
    lx_arena_free(&folio->page);
}

// ── 3. bounce pool ──────────────────────────────────────────────────────────
#define LX_BOUNCE_SLOT     2048u
#define LX_SLOTS_PER_CHUNK (LX_CHUNK_BYTES / LX_BOUNCE_SLOT)
#define LX_BOUNCE_CHUNKS   32u   /* up to 8 MiB in flight (satoru) */

struct lx_bounce_rec {
    void *orig;
    unsigned int size;
    unsigned short nslots;
    unsigned char dir;
    unsigned char used;
};

static struct {
    unsigned char *va;
    dma_addr_t bus;
    struct lx_bounce_rec rec[LX_SLOTS_PER_CHUNK];
    unsigned char slot_used[LX_SLOTS_PER_CHUNK];
} lx_bounce[LX_BOUNCE_CHUNKS];

unsigned long lx_bounce_maps;   // statistics (satoru)

static int lx_bounce_chunk_ready(unsigned int c)
{
    if (lx_bounce[c].va)
        return 1;
    void *va = lx_os_reserve(LX_CHUNK_BYTES);
    if (!va)
        return 0;
    long bus = lx_broker_dma_alloc(va, LX_CHUNK_PAGES, 32);
    if (bus < 0) {
        lx_os_unreserve(va, LX_CHUNK_BYTES);
        return 0;
    }
    lx_bounce[c].bus = (dma_addr_t)bus;
    lx_bounce[c].va = va;
    return 1;
}

static dma_addr_t lx_bounce_map(void *orig, size_t size, enum dma_data_direction dir)
{
    unsigned int need = (unsigned int)((size + LX_BOUNCE_SLOT - 1) / LX_BOUNCE_SLOT);
    if (!need || need > LX_SLOTS_PER_CHUNK)
        return DMA_MAPPING_ERROR;
    lx_flock(&lx_dma_lock);
    for (unsigned int c = 0; c < LX_BOUNCE_CHUNKS; c++) {
        if (!lx_bounce_chunk_ready(c))
            break;
        for (unsigned int s = 0; s + need <= LX_SLOTS_PER_CHUNK; s++) {
            unsigned int k;
            for (k = 0; k < need; k++)
                if (lx_bounce[c].slot_used[s + k])
                    break;
            if (k < need) {
                s += k;
                continue;
            }
            for (k = 0; k < need; k++)
                lx_bounce[c].slot_used[s + k] = 1;
            struct lx_bounce_rec *r = &lx_bounce[c].rec[s];
            r->orig = orig;
            r->size = (unsigned int)size;
            r->nslots = (unsigned short)need;
            r->dir = (unsigned char)dir;
            r->used = 1;
            lx_bounce_maps++;
            unsigned char *slot = lx_bounce[c].va + s * LX_BOUNCE_SLOT;
            lx_funlock(&lx_dma_lock);
            if (dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL)
                memcpy(slot, orig, size);
            return lx_bounce[c].bus + s * LX_BOUNCE_SLOT;
        }
    }
    lx_funlock(&lx_dma_lock);
    lx_log("[lx_dma] bounce pool exhausted\n");
    return DMA_MAPPING_ERROR;
}

// find the record + slot for a bounce bus address; NULL if not a bounce. (satoru)
static struct lx_bounce_rec *lx_bounce_find(dma_addr_t addr, unsigned char **slot_va,
                                            unsigned int *chunk, unsigned int *slot)
{
    for (unsigned int c = 0; c < LX_BOUNCE_CHUNKS && lx_bounce[c].va; c++) {
        if (addr < lx_bounce[c].bus || addr >= lx_bounce[c].bus + LX_CHUNK_BYTES)
            continue;
        unsigned int s = (unsigned int)((addr - lx_bounce[c].bus) / LX_BOUNCE_SLOT);
        if (!lx_bounce[c].rec[s].used)
            return NULL;
        *slot_va = lx_bounce[c].va + s * LX_BOUNCE_SLOT;
        *chunk = c;
        *slot = s;
        return &lx_bounce[c].rec[s];
    }
    return NULL;
}

// ── streaming api ───────────────────────────────────────────────────────────
dma_addr_t dma_map_page_attrs(struct device *dev, struct page *page, size_t offset,
                              size_t size, enum dma_data_direction dir, unsigned long attrs)
{
    (void)dev; (void)attrs;
    // the same arithmetic as the inline page_address(): exact inverse of the
    // virt_to_page() the caller used, whether or not it is an arena page. (satoru)
    unsigned long va = (unsigned long)page_address(page) + offset;
    if (lx_va_in_arena(va) && lx_va_in_arena(va + size - 1) &&
        (va - lx_arena_va) / LX_CHUNK_BYTES == (va + size - 1 - lx_arena_va) / LX_CHUNK_BYTES)
        return lx_arena_bus(va);
    return lx_bounce_map((void *)va, size, dir);
}

void dma_unmap_page_attrs(struct device *dev, dma_addr_t addr, size_t size,
                          enum dma_data_direction dir, unsigned long attrs)
{
    (void)dev; (void)attrs;
    unsigned char *slot;
    unsigned int c, s;
    struct lx_bounce_rec *r = lx_bounce_find(addr, &slot, &c, &s);
    if (!r)
        return;   // arena page: nothing to undo (satoru)
    if ((dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL) && r->orig)
        memcpy(r->orig, slot, size < r->size ? size : r->size);
    lx_flock(&lx_dma_lock);
    for (unsigned int k = 0; k < r->nslots; k++)
        lx_bounce[c].slot_used[s + k] = 0;
    r->used = 0;
    lx_funlock(&lx_dma_lock);
}

void __dma_sync_single_for_cpu(struct device *dev, dma_addr_t addr, size_t size,
                               enum dma_data_direction dir)
{
    (void)dev;
    unsigned char *slot;
    unsigned int c, s;
    struct lx_bounce_rec *r = lx_bounce_find(addr, &slot, &c, &s);
    if (r && (dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL))
        memcpy(r->orig, slot, size < r->size ? size : r->size);
}

void __dma_sync_single_for_device(struct device *dev, dma_addr_t addr, size_t size,
                                  enum dma_data_direction dir)
{
    (void)dev;
    unsigned char *slot;
    unsigned int c, s;
    struct lx_bounce_rec *r = lx_bounce_find(addr, &slot, &c, &s);
    if (r && (dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL))
        memcpy(slot, r->orig, size < r->size ? size : r->size);
}

// lib/scatterlist.c (satoru)
struct scatterlist *sg_next(struct scatterlist *sg)
{
    if (sg_is_last(sg))
        return NULL;
    sg++;
    if (unlikely(sg_is_chain(sg)))
        sg = sg_chain_ptr(sg);
    return sg;
}

unsigned int dma_map_sg_attrs(struct device *dev, struct scatterlist *sg, int nents,
                              enum dma_data_direction dir, unsigned long attrs)
{
    struct scatterlist *s;
    int i;
    for_each_sg(sg, s, nents, i) {
        s->dma_address = dma_map_page_attrs(dev, sg_page(s), s->offset, s->length, dir, attrs);
        if (s->dma_address == DMA_MAPPING_ERROR)
            return 0;
        sg_dma_len(s) = s->length;
    }
    return (unsigned int)nents;
}

void dma_unmap_sg_attrs(struct device *dev, struct scatterlist *sg, int nents,
                        enum dma_data_direction dir, unsigned long attrs)
{
    struct scatterlist *s;
    int i;
    for_each_sg(sg, s, nents, i)
        dma_unmap_page_attrs(dev, s->dma_address, sg_dma_len(s), dir, attrs);
}

// end (satoru)
