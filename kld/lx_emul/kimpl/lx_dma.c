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

// ── dma pools ───────────────────────────────────────────────────────────────
// each pool allocation is one coherent broker allocation; the driver's gen3
// context-info/prph/IML objects are exactly this, and must be contiguous. (satoru)
struct dma_pool {
    struct device *dev;
    size_t size;
    size_t align;
    size_t allocation;
};

struct dma_pool *dma_pool_create(const char *name, struct device *dev,
                                 size_t size, size_t align, size_t allocation)
{
    (void)name;
    struct dma_pool *p = kzalloc(sizeof(*p), GFP_KERNEL);
    if (!p)
        return NULL;
    p->dev = dev;
    p->size = size;
    p->align = align;
    p->allocation = allocation ? allocation : size;
    if (p->allocation < size)
        p->allocation = size;
    return p;
}

void dma_pool_destroy(struct dma_pool *pool)
{
    kfree(pool);
}

void *dma_pool_alloc(struct dma_pool *pool, gfp_t mem_flags, dma_addr_t *handle)
{
    if (!pool || !handle)
        return NULL;
    void *v = dma_alloc_coherent(pool->dev, pool->allocation, handle, mem_flags);
    if (v && (mem_flags & __GFP_ZERO))
        memset(v, 0, pool->allocation);
    return v;
}

void dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t addr)
{
    if (!pool)
        return;
    dma_free_coherent(pool->dev, pool->allocation, vaddr, addr);
}

struct dma_pool *dmam_pool_create(const char *name, struct device *dev,
                                  size_t size, size_t align, size_t allocation)
{
    return dma_pool_create(name, dev, size, align, allocation);
}

void dmam_pool_destroy(struct dma_pool *pool)
{
    dma_pool_destroy(pool);
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
    extern void lx_task_shared_init(void);
    lx_task_shared_init();
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

// (satoru) read-only view for lx_pgobj.c: object runs (kernel-owned frames) are carved
// out of this same VA window so struct page and page_address keep working.
struct lx_arena_view {
    unsigned long va, npages, chunk_pages;
    unsigned char *used;
    dma_addr_t *chunk_bus;
    struct page *pages;
    volatile unsigned int *lock;
};
void lx_arena_get_view(struct lx_arena_view *v)
{
    lx_dma_init();
    v->va = lx_arena_va; v->npages = LX_ARENA_PAGES; v->chunk_pages = LX_CHUNK_PAGES;
    v->used = lx_arena_used; v->chunk_bus = lx_arena_chunk_bus; v->pages = lx_page_array; v->lock = &lx_dma_lock;
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

// vmap()/vunmap() need to know whether a mapping is the arena's own contiguous
// run (no alias) or a broker alias of scattered pages. (satoru)
int lx_arena_contains(const void *p)
{
    return p && lx_va_in_arena((unsigned long)p);
}

// pages from arena va to the end of its broker DMA record (one 256 KiB
// chunk): the broker aliases only runs inside one record. (satoru)
unsigned long lx_arena_chunk_left(unsigned long va)
{
    if (!lx_va_in_arena(va))
        return 1;
    unsigned long off = (va - lx_arena_va) % LX_CHUNK_BYTES;
    return (LX_CHUNK_BYTES - off) / PAGE_SIZE;
}

static dma_addr_t lx_arena_bus(unsigned long va)
{
    unsigned long off = va - lx_arena_va;
    return lx_arena_chunk_bus[off / LX_CHUNK_BYTES] + (off % LX_CHUNK_BYTES);
}

#ifdef KLD_PAGE_CENSUS
// what the page allocator is asked for (the d3 report's ttm census): requests
// and refusals per order, and whether any request wanted dma32 / zeroed /
// highmem pages. one line when an order's count reaches 1, 8, 64, 512, 4096.
// (satoru)
static unsigned long lx_census_req[MAX_PAGE_ORDER + 1];
static unsigned long lx_census_fail[MAX_PAGE_ORDER + 1];
static unsigned long lx_census_dma32, lx_census_zero, lx_census_high;

static void lx_census_note(gfp_t gfp, unsigned int order, int failed)
{
    if (order > MAX_PAGE_ORDER)
        order = MAX_PAGE_ORDER;
    unsigned long c = failed ? ++lx_census_fail[order] : ++lx_census_req[order];
    if (!failed) {
        if (gfp & __GFP_DMA32) lx_census_dma32++;
        if (gfp & __GFP_ZERO) lx_census_zero++;
        if (gfp & __GFP_HIGHMEM) lx_census_high++;
    }
    if (c != 1 && c != 8 && c != 64 && c != 512 && c != 4096)
        return;
    lx_log(failed ? "[lx_dma] census refused order=" : "[lx_dma] census order=");
    lx_log_u(order);
    lx_log(" count=");
    lx_log_u(c);
    lx_log(" gfp=");
    lx_log_hex((unsigned int)gfp);
    lx_log(" dma32=");
    lx_log_u(lx_census_dma32);
    lx_log(" zero=");
    lx_log_u(lx_census_zero);
    lx_log(" highmem=");
    lx_log_u(lx_census_high);
    lx_log("\n");
}
#else
// built for the bochs family only (-DKLD_PAGE_CENSUS); free elsewhere. (satoru)
static inline void lx_census_note(gfp_t gfp, unsigned int order, int failed)
{
    (void)gfp; (void)order; (void)failed;
}
#endif

struct page *alloc_pages_noprof(gfp_t gfp, unsigned int order)
{
    unsigned long n = 1ul << order;
    lx_census_note(gfp, order, 0);
    if (n > LX_CHUNK_PAGES) {
        lx_census_note(gfp, order, 1);
        return NULL;   // a block may not straddle two broker chunks (satoru)
    }
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
    lx_census_note(gfp, order, 1);
    lx_log("[lx_dma] page arena exhausted\n");
    return NULL;
}

// allocate a contiguous run of order-0 pages for objects that need a stable
// vmap() (drm gem shmem objects). the arena VA is one contiguous reservation,
// so a run of arena pages is already a contiguous mapping even when the
// broker frames behind different 256 KiB chunks are not physically adjacent.
// vmap() of such a run therefore returns page_address(first) without a remap.
// (satoru)
struct page *lx_alloc_page_run(unsigned long pages)
{
    if (!pages || pages > LX_ARENA_PAGES)
        return NULL;
    lx_dma_init();
    lx_flock(&lx_dma_lock);
    for (unsigned long i = 0; i + pages <= LX_ARENA_PAGES; i++) {
        unsigned long j;
        for (j = 0; j < pages; j++)
            if (lx_arena_used[i + j])
                break;
        if (j < pages)
            continue;
        // all touched chunks must exist before anything is marked used. (satoru)
        int ok = 1;
        for (unsigned long c = i / LX_CHUNK_PAGES;
             c <= (i + pages - 1) / LX_CHUNK_PAGES; c++)
            if (!lx_arena_chunk_ready(c)) { ok = 0; break; }
        if (!ok) {
            lx_funlock(&lx_dma_lock);
            return NULL;
        }
        for (j = 0; j < pages; j++) {
            lx_arena_used[i + j] = 1;
            lx_arena_order[i + j] = 0;
            memset(&lx_page_array[i + j], 0, sizeof(struct page));
        }
        set_page_count(&lx_page_array[i], 1);
        lx_funlock(&lx_dma_lock);
        return &lx_page_array[i];
    }
    lx_funlock(&lx_dma_lock);
    lx_log("[lx_dma] no contiguous page run for ");
    lx_log_u(pages);
    lx_log(" pages\n");
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
// test-only negative iommu test (kurono.kld.iommutest=1): one dma-from-device
// mapping is replaced by the kernel canary address, so the NIC's rx descriptor
// points at kernel memory. with translation on the write must fault and the
// canary stay intact; with kurono.kld.iommu=0 it must be overwritten. (satoru)
static unsigned long long lx_test_canary;
static int lx_test_armed;
static int lx_test_done;

void lx_dma_iommu_test_arm(void)
{
    long p = lx_broker_test_canary();
    if (p <= 0) {
        lx_log("[lx_dma] iommutest: canary refused\n");
        return;
    }
    lx_test_canary = (unsigned long long)p;
    lx_test_armed = 1;
    lx_log("[lx_dma] iommutest armed\n");
}

dma_addr_t dma_map_page_attrs(struct device *dev, struct page *page, size_t offset,
                              size_t size, enum dma_data_direction dir, unsigned long attrs)
{
    (void)dev; (void)attrs;
    if (lx_test_armed && !lx_test_done && dir == DMA_FROM_DEVICE && size >= 1024) {
        lx_test_done = 1;
        lx_log("[lx_dma] iommutest: one rx buffer points at the canary\n");
        return (dma_addr_t)lx_test_canary;
    }
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

// sg_next comes from lib/scatterlist.c (CORE LIBRARY). (satoru)

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


// CONFIG_NUMA=1 in the pinned config: the inline alloc_pages_node() path calls
// this 4-argument entry. the container has one node, so the node hint and mask
// are irrelevant. (satoru)
struct page *__alloc_pages_noprof(gfp_t gfp, unsigned int order,
                                  int preferred_nid, nodemask_t *nodemask)
{
    (void)preferred_nid; (void)nodemask;
    return alloc_pages_noprof(gfp, order);
}

// end (satoru)
