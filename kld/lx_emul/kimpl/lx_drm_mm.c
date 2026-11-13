// lx_emul: the memory and VFS subset the drm core stands on.
//
// drm_gem_object_init() backs every gem object with a shmem file, and
// drm_gem_get_pages() turns that file's folios into struct pages. the real
// mm/shmem.c cannot come along (it needs the whole VFS, page cache, swap and
// reclaim), so this file implements the little the drm core actually uses:
//
//   shmem_file_setup()       a fake file + inode + mapping record
//   shmem_read_folio_gfp()   one broker-backed arena page per file index
//   vmap()/vunmap()          a gem object's pages are one contiguous run in
//                            the lx_dma.c arena, so vmap returns that run
//
// pages come from alloc_pages() -> lx_dma.c -> KLD_OP_DMA_ALLOC, i.e. broker
// frames the kernel knows by physical address. that is what makes a later
// client mmap and dma-buf export of the same object possible (D5); the
// container never invents memory the kernel cannot see. (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/shmem_fs.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/kstrtox.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/file.h>
#include <linux/pagevec.h>
#include <linux/fs_context.h>
#include <linux/pseudo_fs.h>
#include <linux/slab.h>
#include <linux/math64.h>
#include <linux/version.h>
#include <linux/stdarg.h>
#include <asm/set_memory.h>
#include <asm/smp.h>

#include "lx_kshim.h"
#include "lx_broker.h"

extern int lx_pgobj_active(void);   // lx_pgobj.c (satoru)
extern struct page *lx_pgobj_run(unsigned long pages);
extern int lx_pgobj_free(struct page *first, unsigned long pages);

// ── fake shmem files ────────────────────────────────────────────────────────
// one record per drm gem object. the mapping pointer is the key the drm core
// hands back to shmem_read_folio_gfp(). (satoru)
#define LX_SHM_MAX 64

struct lx_shm_rec {
    bool                 used;
    struct file         *filp;
    struct inode        *inode;
    struct address_space *mapping;
    struct page        **pages;
    unsigned long        npages;
};

static struct lx_shm_rec g_shm[LX_SHM_MAX];
static volatile unsigned int g_shm_lock;
static unsigned long g_shm_ino = 1;

static struct lx_shm_rec *lx_shm_by_mapping(struct address_space *mapping)
{
    for (int i = 0; i < LX_SHM_MAX; i++)
        if (g_shm[i].used && g_shm[i].mapping == mapping)
            return &g_shm[i];
    return NULL;
}

static struct address_space *lx_anon_mapping(void)
{
    struct inode *inode = kzalloc(sizeof(*inode), GFP_KERNEL);
    if (!inode)
        return NULL;
    inode->i_ino = g_shm_ino++;
    inode->i_mode = S_IFREG | 0600;
    inode->i_size = 0;
    inode->i_mapping = &inode->i_data;
    inode->i_data.host = inode;
    inode->i_data.gfp_mask = GFP_KERNEL;
    xa_init(&inode->i_data.i_pages);
    init_rwsem(&inode->i_data.invalidate_lock);
    init_rwsem(&inode->i_data.i_mmap_rwsem);
    INIT_LIST_HEAD(&inode->i_data.i_private_list);
    inode->i_private = NULL;
    return &inode->i_data;
}

static struct inode *lx_mapping_inode(struct address_space *mapping)
{
    return mapping ? mapping->host : NULL;
}

// simple_pin_fs/alloc_anon_inode are hand-written here instead of importing
// fs/libfs.c: its alloc_anon_inode() goes through new_inode_pseudo() and the
// real superblock/inode allocators, which the container does not have. drm
// only needs an inode with an address_space. (satoru)
int simple_pin_fs(struct file_system_type *type, struct vfsmount **mount, int *count)
{
    (void)type;
    if (count)
        (*count)++;
    if (mount && !*mount)
        *mount = (struct vfsmount *)&g_shm;   // non-null token, never dereferenced
    return 0;
}

void simple_release_fs(struct vfsmount **mount, int *count)
{
    if (count && *count > 0)
        (*count)--;
    if (count && *count == 0 && mount)
        *mount = NULL;
}

struct inode *alloc_anon_inode(struct super_block *s)
{
    struct address_space *mapping = lx_anon_mapping();
    (void)s;
    if (!mapping)
        return ERR_PTR(-ENOMEM);
    return mapping->host;
}

struct file *shmem_file_setup(const char *name, loff_t size, unsigned long flags)
{
    struct file *filp;
    struct inode *inode;
    struct address_space *mapping;
    int slot = -1;

    (void)name; (void)flags;
    if (size < 0)
        return ERR_PTR(-EINVAL);
    lx_flock(&g_shm_lock);
    for (int i = 0; i < LX_SHM_MAX; i++)
        if (!g_shm[i].used) { slot = i; break; }
    lx_funlock(&g_shm_lock);
    if (slot < 0)
        return ERR_PTR(-ENFILE);

    mapping = lx_anon_mapping();
    if (!mapping)
        return ERR_PTR(-ENOMEM);
    inode = lx_mapping_inode(mapping);
    inode->i_size = size;
    filp = kzalloc(sizeof(*filp), GFP_KERNEL);
    if (!filp) {
        kfree(inode);
        return ERR_PTR(-ENOMEM);
    }
    atomic_long_set(&filp->f_count, 1);
    filp->f_inode = inode;
    filp->f_mapping = mapping;
    filp->f_mode = FMODE_READ | FMODE_WRITE;
    mutex_init(&filp->f_pos_lock);

    lx_flock(&g_shm_lock);
    g_shm[slot].used = true;
    g_shm[slot].filp = filp;
    g_shm[slot].inode = inode;
    g_shm[slot].mapping = mapping;
    g_shm[slot].pages = NULL;
    g_shm[slot].npages = 0;
    lx_funlock(&g_shm_lock);
    return filp;
}

static void lx_shm_free_pages(struct lx_shm_rec *r)
{
    if (!r->pages)
        return;
    if (lx_pgobj_free(r->pages[0], r->npages) == 0) {   // kernel-owned object frames (lx_pgobj.c) (satoru)
        kvfree(r->pages); r->pages = NULL; r->npages = 0;
        return;
    }
    for (unsigned long i = 0; i < r->npages; i++)
        if (r->pages[i]) {
            set_page_count(r->pages[i], 1);
            __free_pages(r->pages[i], 0);
            r->pages[i] = NULL;
        }
    kvfree(r->pages);
    r->pages = NULL;
    r->npages = 0;
}

static void lx_shm_release(struct lx_shm_rec *r)
{
    lx_shm_free_pages(r);
    kfree(r->filp);
    if (r->inode) {
        kfree(r->inode);
    }
    r->used = false;
    r->filp = NULL;
    r->inode = NULL;
    r->mapping = NULL;
}

struct folio *shmem_read_folio_gfp(struct address_space *mapping, pgoff_t index,
                                   gfp_t gfp)
{
    struct lx_shm_rec *r = lx_shm_by_mapping(mapping);
    struct page *run;
    unsigned long npages;
    if (!r || !r->inode)
        return ERR_PTR(-EINVAL);
    npages = DIV_ROUND_UP((unsigned long)r->inode->i_size, PAGE_SIZE);
    if (index >= npages)
        return ERR_PTR(-EINVAL);
    lx_flock(&g_shm_lock);
    if (!r->pages) {
        r->pages = kvcalloc(npages, sizeof(struct page *), GFP_KERNEL);
        if (!r->pages) {
            lx_funlock(&g_shm_lock);
            return ERR_PTR(-ENOMEM);
        }
        run = lx_pgobj_active() ? lx_pgobj_run(npages) : lx_alloc_page_run(npages);
        if (!run) {
            kvfree(r->pages);
            r->pages = NULL;
            lx_funlock(&g_shm_lock);
            return ERR_PTR(-ENOMEM);
        }
        for (unsigned long i = 0; i < npages; i++) {
            r->pages[i] = run + i;
            // the shmem cache holds one reference and the caller gets one:
            // drm_gem_put_pages() drops the caller's, fput drops the cache's.
            set_page_count(r->pages[i], 2);
        }
        r->npages = npages;
    }
    lx_funlock(&g_shm_lock);
    if (!(gfp & __GFP_ZERO) && r->pages[index])
        ;   // pages start zeroed by the broker and are never swapped (satoru)
    return page_folio(r->pages[index]);
}

void shmem_truncate_range(struct inode *inode, loff_t start, loff_t end)
{
    struct lx_shm_rec *r;
    if (!inode || !inode->i_mapping)
        return;
    r = lx_shm_by_mapping(inode->i_mapping);
    if (!r)
        return;
    (void)start; (void)end;   // whole-object only; drm shmem objects are fixed size
    lx_flock(&g_shm_lock);
    lx_shm_free_pages(r);
    lx_funlock(&g_shm_lock);
}

unsigned long invalidate_mapping_pages(struct address_space *mapping,
                                       pgoff_t start, pgoff_t end)
{
    struct lx_shm_rec *r = lx_shm_by_mapping(mapping);
    unsigned long n = 0;
    if (!r)
        return 0;
    lx_flock(&g_shm_lock);
    if (r->pages) {
        n = r->npages;
        lx_shm_free_pages(r);
    }
    (void)start; (void)end;
    lx_funlock(&g_shm_lock);
    return n;
}

void unmap_mapping_range(struct address_space *mapping, loff_t const holebegin,
                         loff_t const holelen, int even_cows)
{
    // the container's own vmap of these pages is not a user VMA, and no client
    // maps this fake inode yet (D5 will route client mmaps through the broker).
    (void)mapping; (void)holebegin; (void)holelen; (void)even_cows;
}

void iput(struct inode *inode)
{
    // the fake inode is owned by its shmem record; fput frees both. (satoru)
    (void)inode;
}

// kasprintf: lib/kasprintf.c is not imported because lx_device.c already owns
// kvasprintf for every family; this is the variadic wrapper it lacks. (satoru)
extern char *kvasprintf(gfp_t gfp, const char *fmt, va_list ap);

char *kasprintf(gfp_t gfp, const char *fmt, ...)
{
    va_list ap;
    char *p;
    va_start(ap, fmt);
    p = kvasprintf(gfp, fmt, ap);
    va_end(ap);
    return p;
}

// simple_open/simple_read_from_buffer/init_pseudo come from fs/libfs.c in the
// kernel; importing that file would drag in the real inode and mount
// allocators. these three are the only ones the drm core touches. (satoru)
int simple_open(struct inode *inode, struct file *file)
{
    if (inode && inode->i_private)
        file->private_data = inode->i_private;
    return 0;
}

ssize_t simple_read_from_buffer(void __user *to, size_t count, loff_t *ppos,
                                const void *from, size_t available)
{
    loff_t pos = *ppos;
    size_t ret;
    if (pos < 0)
        return -EINVAL;
    if ((unsigned long long)pos >= available)
        return 0;
    if (count > available - (size_t)pos)
        count = available - (size_t)pos;
    if (!access_ok(to, count))
        return -EFAULT;
    ret = count;
    memcpy((void *)to, (const char *)from + pos, count);
    *ppos = pos + ret;
    return (ssize_t)ret;
}

struct pseudo_fs_context *init_pseudo(struct fs_context *fc, unsigned long magic)
{
    // never reached: simple_pin_fs above does not mount a real fs. (satoru)
    struct pseudo_fs_context *ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (ctx) {
        ctx->magic = magic;
        fc->fs_private = ctx;
    }
    return ctx;
}

long strnlen_user(const char __user *s, long n)
{
    long i = 0;
    if (n <= 0)
        return 0;
    while (i < n && s[i])
        i++;
    return i < n ? i + 1 : i + 1;   // includes the NUL, like the kernel
}

// ── mmio accessors (lib/iomap.c is not imported: it also defines pci_iounmap,
// which lx_pci.c owns for every family). a granted BAR is a user VA here, so a
// volatile load/store is the real access. (satoru)
u8 ioread8(const void __iomem *addr)  { return *(const volatile u8 *)addr; }
u16 ioread16(const void __iomem *addr) { return *(const volatile u16 *)addr; }
u32 ioread32(const void __iomem *addr) { return *(const volatile u32 *)addr; }
u64 ioread64(const void __iomem *addr) { return *(const volatile u64 *)addr; }
u64 ioread64_lo_hi(const void __iomem *addr) { return *(const volatile u64 *)addr; }
u64 ioread64_hi_lo(const void __iomem *addr) { return *(const volatile u64 *)addr; }
void iowrite8(u8 v, void __iomem *addr)   { *(volatile u8 *)addr = v; }
void iowrite16(u16 v, void __iomem *addr) { *(volatile u16 *)addr = v; }
void iowrite32(u32 v, void __iomem *addr) { *(volatile u32 *)addr = v; }
void iowrite64(u64 v, void __iomem *addr) { *(volatile u64 *)addr = v; }
void memcpy_toio(void __iomem *dst, const void *src, size_t count)
{
    volatile u8 *d = dst;
    const u8 *s = src;
    while (count--)
        *d++ = *s++;
}
void memcpy_fromio(void *dst, const void __iomem *src, size_t count)
{
    u8 *d = dst;
    const volatile u8 *s = src;
    while (count--)
        *d++ = *s++;
}
void memset_io(void __iomem *dst, int c, size_t count)
{
    volatile u8 *d = dst;
    while (count--)
        *d++ = (u8)c;
}

void fput(struct file *file)
{
    struct lx_shm_rec *r;
    if (!file)
        return;
    if (!atomic_long_dec_and_test(&file->f_count))
        return;
    lx_flock(&g_shm_lock);
    r = NULL;
    for (int i = 0; i < LX_SHM_MAX; i++)
        if (g_shm[i].used && g_shm[i].filp == file) { r = &g_shm[i]; break; }
    if (r)
        lx_shm_release(r);
    lx_funlock(&g_shm_lock);
}

struct file *fget(unsigned int fd)
{
    (void)fd;
    return NULL;
}

struct file *fget_raw(unsigned int fd)
{
    (void)fd;
    return NULL;
}

struct fd fdget(unsigned int fd)
{
    struct fd f = { .word = 0 };
    (void)fd;
    return f;
}

// ── vmap: contiguous arena run or a broker alias of scattered pages ─────────
// the arena fast path returns its own contiguous run. a ttm system buffer's
// page array is often scattered (blocks from the pool's free lists), so for
// that case reserve a VA and ask the broker to map exactly those frames there,
// one contiguous run per call (KLD_OP_DMA_ALIAS, scalars only); the frames are
// the container's own DMA frames, and vunmap drops the alias. (satoru)
struct lx_vmap_alias { void *va; unsigned long bytes; };
static struct lx_vmap_alias g_vmap_alias[64];
static volatile unsigned int g_vmap_alias_lock;
static unsigned long g_vmap_alias_count;

void *vmap(struct page **pages, unsigned int count, unsigned long flags,
           pgprot_t prot)
{
    unsigned long base;
    (void)flags; (void)prot;
    if (!pages || !count || !pages[0])
        return NULL;
    base = (unsigned long)page_address(pages[0]);
    int contiguous = 1;
    for (unsigned int i = 1; i < count; i++) {
        if (!pages[i] || (unsigned long)page_address(pages[i]) != base + (unsigned long)i * PAGE_SIZE) {
            contiguous = 0;
            break;
        }
    }
    if (contiguous)
        return (void *)base;
    for (unsigned int i = 0; i < count; i++)
        if (!pages[i])
            return NULL;
    int slot = -1;
    lx_flock(&g_vmap_alias_lock);
    for (int i = 0; i < 64; i++) {
        if (!g_vmap_alias[i].va && !g_vmap_alias[i].bytes) {
            g_vmap_alias[i].bytes = 1;   // claimed (satoru)
            slot = i;
            break;
        }
    }
    lx_funlock(&g_vmap_alias_lock);
    if (slot < 0) {
        lx_log("[lx_drm_mm] vmap: alias table full\n");
        return NULL;
    }
    unsigned long bytes = (unsigned long)count * PAGE_SIZE;
    void *va = lx_os_reserve(bytes);
    long rc = va ? 0 : -12;
    unsigned int runs = 0;
    for (unsigned int i = 0; va && i < count && rc >= 0;) {
        unsigned long src = (unsigned long)page_address(pages[i]);
        // a run stops at a broker DMA record (arena chunk) boundary as well:
        // DMA_ALIAS takes runs inside one record only. (satoru)
        unsigned long left = lx_arena_chunk_left(src);
        unsigned int n = 1;
        while (i + n < count && n < left &&
               (unsigned long)page_address(pages[i + n]) == src + (unsigned long)n * PAGE_SIZE)
            n++;
        // a3: 0 starts the record at va, 1 extends it by this run. (satoru)
        rc = lx_broker_call(LX_KLD_OP_DMA_ALIAS,
                            (unsigned long)va + (unsigned long)i * PAGE_SIZE, src, n,
                            i ? 1ul : 0ul);
        i += n;
        runs++;
    }
    if (rc < 0) {
        lx_log("[lx_drm_mm] vmap: broker alias refused rc=");
        lx_log_u((unsigned long)(-rc));
        lx_log("\n");
        if (va) {
            if (runs > 1)   // earlier runs are mapped in the record at va (satoru)
                lx_broker_call(LX_KLD_OP_DMA_UNALIAS, (unsigned long)va, 0, 0, 0);
            lx_os_unreserve(va, bytes);
        }
        lx_flock(&g_vmap_alias_lock);
        g_vmap_alias[slot].bytes = 0;
        lx_funlock(&g_vmap_alias_lock);
        return NULL;
    }
    lx_flock(&g_vmap_alias_lock);
    g_vmap_alias[slot].va = va;
    g_vmap_alias[slot].bytes = bytes;
    unsigned long nth = ++g_vmap_alias_count;
    lx_funlock(&g_vmap_alias_lock);
    if (nth <= 4) {
        lx_log("[lx_drm_mm] vmap alias pages=");
        lx_log_u((unsigned long)count);
        lx_log(" runs=");
        lx_log_u((unsigned long)runs);
        lx_log("\n");
    }
    return va;
}

void vunmap(const void *addr)
{
    if (!addr || lx_arena_contains(addr)) {
        // the contiguous fast path returns the arena's own mapping; the pages
        // are freed by their owner, not by vunmap. (satoru)
        return;
    }
    unsigned long bytes = 0;
    lx_flock(&g_vmap_alias_lock);
    for (int i = 0; i < 64; i++) {
        if (g_vmap_alias[i].va == addr) {
            bytes = g_vmap_alias[i].bytes;
            g_vmap_alias[i].va = NULL;
            g_vmap_alias[i].bytes = 0;
            break;
        }
    }
    lx_funlock(&g_vmap_alias_lock);
    if (!bytes)
        return;
    lx_broker_call(LX_KLD_OP_DMA_UNALIAS, (unsigned long)addr, 0, 0, 0);
    lx_os_unreserve((void *)addr, bytes);
}

// ── user copies for memdup/strndup ──────────────────────────────────────────
static int lx_copy_from_user_ptr(void *to, const void __user *from, unsigned long n)
{
    if (n && !access_ok(from, n))
        return -EFAULT;
    memcpy(to, (const void *)from, n);
    return 0;
}

void *memdup_user(const void __user *src, size_t len)
{
    void *p = kmalloc(len ? len : 1, GFP_KERNEL);
    if (!p)
        return ERR_PTR(-ENOMEM);
    if (len && lx_copy_from_user_ptr(p, src, len)) {
        kfree(p);
        return ERR_PTR(-EFAULT);
    }
    return p;
}

void *memdup_user_nul(const void __user *src, size_t len)
{
    char *p = kmalloc(len + 1, GFP_KERNEL);
    if (!p)
        return ERR_PTR(-ENOMEM);
    if (len && lx_copy_from_user_ptr(p, src, len)) {
        kfree(p);
        return ERR_PTR(-EFAULT);
    }
    p[len] = 0;
    return p;
}

char *strndup_user(const char __user *s, long n)
{
    char *p;
    long len;
    if (n < 0)
        return ERR_PTR(-EINVAL);
    len = strnlen_user(s, n);
    if (!len)
        return ERR_PTR(-EFAULT);
    if (len > n)
        return ERR_PTR(-EINVAL);
    p = kmalloc(len, GFP_KERNEL);
    if (!p)
        return ERR_PTR(-ENOMEM);
    if (lx_copy_from_user_ptr(p, s, len)) {
        kfree(p);
        return ERR_PTR(-EFAULT);
    }
    p[len - 1] = 0;
    return p;
}

// ── string helpers musl owns in this container ──────────────────────────────
char *strnchr(const char *s, size_t count, int c)
{
    for (; count-- && *s; s++)
        if (*s == (char)c)
            return (char *)s;
    return NULL;
}

void *memchr_inv(const void *start, int c, size_t bytes)
{
    const unsigned char *p = start;
    unsigned char v = (unsigned char)c;
    while (bytes--) {
        if (*p != v)
            return (void *)p;
        p++;
    }
    return NULL;
}

long simple_strtol(const char *cp, char **endp, unsigned int base)
{
    long v = 0;
    int neg = 0;
    if (!base) {
        base = 10;
        if (*cp == '0') {
            base = 8;
            cp++;
            if (*cp == 'x' || *cp == 'X') { base = 16; cp++; }
        }
    }
    if (*cp == '-') { neg = 1; cp++; }
    while (*cp) {
        int d;
        char ch = *cp;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
        else break;
        if (d >= (int)base) break;
        v = v * base + d;
        cp++;
    }
    if (endp)
        *endp = (char *)cp;
    return neg ? -v : v;
}

// ── cache attribute requests from drm_cache ─────────────────────────────────
// the gem pages are ordinary write-back user mappings in the container; user
// PTEs cannot be made WC here, so the only truthful answer for a WC request is
// "still WB". callers use this for performance, never correctness. (satoru)
int set_pages_array_wb(struct page **pages, int addrinarray)
{
    (void)pages; (void)addrinarray;
    return 0;
}

int set_pages_array_wc(struct page **pages, int addrinarray)
{
    static bool logged;
    (void)pages; (void)addrinarray;
    if (!logged) {
        lx_log("[lx_drm_mm] WC page request: container mappings stay write-back\n");
        logged = true;
    }
    return 0;
}

int wbinvd_on_all_cpus(void)
{
    __sync_synchronize();
    return 0;
}

pgprot_t pgprot_writecombine(pgprot_t prot)
{
    // vmap ignores prot; the pages are WB. (satoru)
    return prot;
}

bool mutex_is_locked(struct mutex *lock)
{
    return atomic_long_read(&lock->owner) != 0;
}

u64 nsecs_to_jiffies64(u64 n)
{
    return div_u64(n, (u64)(NSEC_PER_SEC / HZ));
}

// end (satoru)

// shmem truncate (GEM buffer teardown on a modeset) walks the folio batch
// looking for unevictable pages; the container has no LRU, so nothing moves.
void check_move_unevictable_folios(struct folio_batch *fbatch) { (void)fbatch; }

// the batch holds references taken by filemap_get_folios(); drop them here
// (lru_add_drain is a no-op: the container has no per-cpu LRU). (satoru)
void __folio_batch_release(struct folio_batch *fbatch)
{
    unsigned i;
    for (i = 0; i < fbatch->nr; i++)
        folio_put(fbatch->folios[i]);
    fbatch->nr = 0;
}
