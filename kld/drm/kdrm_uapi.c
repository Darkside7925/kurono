// kld/drm/kdrm_uapi.c - the container side of /dev/dri (stage D5).
//
// Kurono's kernel turns a program's open/ioctl/mmap on /dev/dri/cardN into a
// request in a shared ring (kimpl/lx_dri.h). Worker threads here claim a
// request and run the UPSTREAM handler on a struct file whose f_op is the
// driver's own fops (drm_open, drm_ioctl, drm_gem_mmap, ...). Nothing in this
// file knows which drm driver is bound: the same code serves cirrus today and
// i915 later, and ioctl numbers are forwarded unfiltered.
//
// The upstream handlers reach into the caller with copy_from_user,
// copy_to_user, get_user and put_user. The shim routes those (while a request
// is being served on this thread) to the broker's UACCESS op, which wakes the
// PARKED CALLER; the caller copies between its own memory and this slot's
// bounce page and acks. The container never sees the caller's address space.
// Outside a request, or for an abandoned one, every user access fails whole.
// (satoru)
//
// end (satoru)

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/kdev_t.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>

#include "kimpl/lx_kshim.h"
#include "kimpl/lx_dri.h"

extern long lx_broker_call(unsigned int op, unsigned long a0, unsigned long a1,
                           unsigned long a2, unsigned long a3);
extern unsigned long (*lx_uaccess_hook)(int dir, unsigned long uptr, void *kbuf, unsigned long n);
extern void kdri_app_driver(unsigned int test) __attribute__((weak));
extern void lx_pgobj_enable(void);
extern int  lx_pgobj_lookup(unsigned long va, unsigned int *obj, unsigned int *index);
extern void *lx_os_malloc(unsigned long n);
extern void lx_os_free(void *p);

#define KDRI_WORKERS 8

struct kdri_req {
    struct lxdri_slot *slot;
    unsigned char     *bounce;
    unsigned long long req_id;
    unsigned int       euid;
};

struct kdri_file {
    struct file  *filp;
    struct inode *inode;
    unsigned int  refs;
    unsigned int  state;   // 0 free, 1 opening, 2 open (satoru)
    unsigned int  gen;
    unsigned int  closing;   // the base reference was dropped by a close request (satoru)
};

static struct lxdri_hdr *g_hdr;
static unsigned char    *g_ring;
static struct drm_device *g_dev;
static struct kdri_file  g_files[LXDRI_FILES];
static volatile unsigned int g_closer_lock;
static __thread struct kdri_req *kdri_cur;

static inline struct lxdri_slot *slot_at(unsigned int i)
{
    return (struct lxdri_slot *)(g_ring + LXDRI_SLOT_OFF(i));
}

// ── routed user access ──────────────────────────────────────────────────────
static unsigned long kdri_ua(int dir, unsigned long uptr, void *kbuf, unsigned long n)
{
    struct kdri_req *r = kdri_cur;
    unsigned long done = 0;
    if (!r)
        return n;                       // outside a served request: all bytes failed (satoru)
    while (done < n) {
        unsigned long chunk = n - done;
        unsigned int seq;
        long rc;
        if (chunk > LXDRI_BOUNCE)
            chunk = LXDRI_BOUNCE;
        if (dir == LXDRI_UA_TO_USER)
            memcpy(r->bounce, (char *)kbuf + done, chunk);
        seq = __atomic_load_n(&r->slot->ua_ack, __ATOMIC_ACQUIRE);
        rc = lx_broker_call(LX_KLD_OP_DRI_UACCESS, r->req_id, uptr + done, chunk, (unsigned long)dir);
        if (rc < 0)
            return n - done;            // abandoned, bad range or budget: whole rest fails (satoru)
        // bounded wait: every 50 ms ask the kernel whether the caller is still there (killed, terminated
        // but not yet reaped); if not, the whole access fails and this worker is free again. (satoru)
        while (__atomic_load_n(&r->slot->ua_ack, __ATOMIC_ACQUIRE) == seq) {
            lx_os_futex_wait(&r->slot->ua_ack, seq, 50000000ll);
            if (__atomic_load_n(&r->slot->ua_ack, __ATOMIC_ACQUIRE) != seq)
                break;
            if (lx_broker_call(LX_KLD_OP_DRI_UA_CHECK, r->req_id, 0, 0, 0) != 0)
                return n - done;
        }
        if (r->slot->ua_res)
            return n - done;
        if (dir == LXDRI_UA_FROM_USER)
            memcpy((char *)kbuf + done, r->bounce, chunk);
        done += chunk;
    }
    return 0;
}

int kdri_caller_euid(void)
{
    return kdri_cur ? (int)kdri_cur->euid : -1;
}

// get_user/put_user entry points. x86's inline asm calls these with a private
// register ABI (see arch/x86/lib/getuser.S, putuser.S): get: rax=ptr in,
// eax=err and rdx=value out; put: rax=value, rcx=ptr in, ecx=err out; nothing
// else may change. the thunks save everything and call the C routines. (satoru)
struct kdri_gu { unsigned long val; long err; };

struct kdri_gu kdri_get_user(unsigned long ptr, unsigned long size)
{
    struct kdri_gu g = { 0, 0 };
    if (kdri_ua(LXDRI_UA_FROM_USER, ptr, &g.val, size))
        g.err = -14;
    return g;
}

long kdri_put_user(unsigned long ptr, unsigned long val, unsigned long size)
{
    return kdri_ua(LXDRI_UA_TO_USER, ptr, &val, size) ? -14 : 0;
}

#define KDRI_GET_THUNK(N) \
    __asm__(".text\n.globl __get_user_" #N "\n.type __get_user_" #N ",@function\n" \
            "__get_user_" #N ":\n" \
            "push %rcx\npush %rsi\npush %rdi\npush %r8\npush %r9\npush %r10\npush %r11\n" \
            "mov %rax, %rdi\nmov $" #N ", %esi\ncall kdri_get_user\n" \
            "xchg %rax, %rdx\n" \
            "pop %r11\npop %r10\npop %r9\npop %r8\npop %rdi\npop %rsi\npop %rcx\nret\n" \
            ".size __get_user_" #N ", .-__get_user_" #N "\n")
KDRI_GET_THUNK(1);
KDRI_GET_THUNK(2);
KDRI_GET_THUNK(4);
KDRI_GET_THUNK(8);

#define KDRI_PUT_THUNK(N) \
    __asm__(".text\n.globl __put_user_" #N "\n.type __put_user_" #N ",@function\n" \
            "__put_user_" #N ":\n" \
            "push %rax\npush %rdx\npush %rsi\npush %rdi\npush %r8\npush %r9\npush %r10\npush %r11\n" \
            "sub $8, %rsp\n" \
            "mov %rcx, %rdi\nmov %rax, %rsi\nmov $" #N ", %edx\ncall kdri_put_user\n" \
            "mov %rax, %rcx\n" \
            "add $8, %rsp\n" \
            "pop %r11\npop %r10\npop %r9\npop %r8\npop %rdi\npop %rsi\npop %rdx\npop %rax\nret\n" \
            ".size __put_user_" #N ", .-__put_user_" #N "\n")
KDRI_PUT_THUNK(1);
KDRI_PUT_THUNK(2);
KDRI_PUT_THUNK(4);
KDRI_PUT_THUNK(8);

// ── files ───────────────────────────────────────────────────────────────────
static struct kdri_file *file_get(unsigned int id)
{
    struct kdri_file *f = &g_files[id & 0xFF];
    unsigned int refs;
    if (__atomic_load_n(&f->state, __ATOMIC_ACQUIRE) != 2 || f->gen != (id >> 8))
        return NULL;
    refs = __atomic_load_n(&f->refs, __ATOMIC_ACQUIRE);
    while (refs) {
        if (__atomic_compare_exchange_n(&f->refs, &refs, refs + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return f;
    }
    return NULL;
}

static void file_release(struct kdri_file *f)
{
    struct file *filp = f->filp;
    struct inode *inode = f->inode;
    if (filp->f_op && filp->f_op->release)
        filp->f_op->release(inode, filp);
    kfree(filp);
    kfree(inode);
    f->filp = NULL;
    f->inode = NULL;
    __atomic_store_n(&f->state, 0, __ATOMIC_RELEASE);
}

static void file_put(struct kdri_file *f)
{
    if (__atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) == 0)
        file_release(f);
}

static long do_open(struct lxdri_slot *s)
{
    struct kdri_file *f = &g_files[s->file_id & 0xFF];
    struct inode *inode;
    struct file *filp;
    unsigned int zero = 0;
    long ret;
    if (!__atomic_compare_exchange_n(&f->state, &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return -EBUSY;
    inode = kzalloc(sizeof(*inode), GFP_KERNEL);
    filp = kzalloc(sizeof(*filp), GFP_KERNEL);
    if (!inode || !filp || !g_dev->driver->fops) {
        kfree(inode); kfree(filp);
        __atomic_store_n(&f->state, 0, __ATOMIC_RELEASE);
        return -ENOMEM;
    }
    inode->i_rdev = MKDEV(226, s->open_minor);
    inode->i_mode = S_IFCHR | 0666;
    inode->i_mapping = &inode->i_data;
    atomic_long_set(&filp->f_count, 1);
    filp->f_inode = inode;
    filp->f_mapping = inode->i_mapping;
    filp->f_op = g_dev->driver->fops;
    filp->f_flags = s->open_flags;
    filp->f_mode = FMODE_READ | FMODE_WRITE;
    mutex_init(&filp->f_pos_lock);
    ret = filp->f_op->open ? filp->f_op->open(inode, filp) : 0;
    if (ret) {
        kfree(inode); kfree(filp);
        __atomic_store_n(&f->state, 0, __ATOMIC_RELEASE);
        return ret;
    }
    f->filp = filp;
    f->inode = inode;
    f->gen = s->file_id >> 8;
    f->closing = 0;
    __atomic_store_n(&f->refs, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&f->state, 2, __ATOMIC_RELEASE);
    return 0;
}

static unsigned int g_slow_ms;   // rig (test 8): GETRESOURCES takes this long to start, so a caller can be parked in it (satoru)

static long do_ioctl(struct lxdri_slot *s)
{
    struct kdri_file *f = file_get(s->file_id);
    long ret;
    if (!f)
        return -ENODEV;
    if (g_slow_ms && (s->cmd & 0xFF) == 0xA0)
        lx_os_sleep_ns((unsigned long long)g_slow_ms * 1000000ull);
    ret = f->filp->f_op->unlocked_ioctl ? f->filp->f_op->unlocked_ioctl(f->filp, s->cmd, (unsigned long)s->arg)
                                        : -ENOTTY;
    file_put(f);
    return ret;
}

// a release request from the kernel. it never blocks: a file with requests in flight (they hold
// references) is retried on a later pass, and the kernel reuses the index only after the release
// is complete (closeq_cons advances over finished entries, in order). no lock is held across a
// wait; the pass lock only serialises the scan. (satoru)
static unsigned char cq_done[LXDRI_CLOSEQ];
static unsigned int  g_cq_pending;

static int try_close(unsigned int id)
{
    struct kdri_file *f = &g_files[id & 0xFF];
    unsigned int st = __atomic_load_n(&f->state, __ATOMIC_ACQUIRE);
    if (st == 1)
        return 0;                       // still opening: the kernel orders the close after the open's reply (satoru)
    if (st == 2 && f->gen == (id >> 8)) {
        if (!f->closing) {
            f->closing = 1;
            file_put(f);                // the base reference; the last put releases (satoru)
        }
        return __atomic_load_n(&f->state, __ATOMIC_ACQUIRE) == 0;
    }
    return st == 0;
}

static int drain_closes(void)
{
    int did = 0;
    unsigned int zero = 0;
    if (!__atomic_compare_exchange_n(&g_closer_lock, &zero, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;
    unsigned int cons = g_hdr->closeq_cons;
    unsigned int prod = __atomic_load_n(&g_hdr->closeq_prod, __ATOMIC_ACQUIRE);
    g_cq_pending = 0;
    for (unsigned int pos = cons; pos != prod; pos++) {
        if (!cq_done[pos % LXDRI_CLOSEQ]) {
            if (try_close(g_hdr->closeq[pos % LXDRI_CLOSEQ])) { cq_done[pos % LXDRI_CLOSEQ] = 1; did = 1; }
            else g_cq_pending++;
        }
    }
    while (cons != prod && cq_done[cons % LXDRI_CLOSEQ]) {
        cq_done[cons % LXDRI_CLOSEQ] = 0;
        cons++;
        __atomic_store_n(&g_hdr->closeq_cons, cons, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_closer_lock, 0, __ATOMIC_RELEASE);
    return did;
}

// ── mmap of a gem object ─────────────────────────────────────────────────────
// the kernel asks which pages back a fake offset. the upstream mmap handler runs on a
// fake vm_area_struct (drm_gem_mmap: offset lookup, per-file access check, object
// reference, vm_ops), then the vm_ops->fault handler is called for every page and
// vmf_insert_pfn/page record which page it would have mapped instead of editing a
// page table. each page belongs to a kernel-owned object run (lx_pgobj.c), so the
// reply is a list of {object, first page, page count}; the kernel validates it and
// maps those frames into the caller. the vma is closed again at once: the frames
// stay alive by their own references, not by the container's object. (satoru)
static __thread unsigned long *cap_pfn;
static __thread unsigned long cap_start, cap_n;
static struct mm_struct kdri_dummy_mm;

vm_fault_t vmf_insert_pfn(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn)
{
    (void)vma;
    if (cap_pfn && addr >= cap_start && addr < cap_start + cap_n * PAGE_SIZE)
        cap_pfn[(addr - cap_start) >> PAGE_SHIFT] = pfn;
    return VM_FAULT_NOPAGE;
}

pgprot_t vm_get_page_prot(unsigned long vm_flags)
{
    (void)vm_flags;
    return __pgprot(0);
}

long kdri_mmap_request(struct kdri_file *f, struct lxdri_slot *s)
{
    unsigned long npages = (unsigned long)(s->mm_len >> PAGE_SHIFT);
    struct vm_area_struct *vma;
    unsigned long *pfns;
    unsigned long fl = VM_SHARED | VM_MAYSHARE;
    long ret;
    unsigned int nseg = 0, last_obj = 0, last_end = 0;
    if (!npages || npages > 65536)
        return -EINVAL;
    vma = kzalloc(sizeof(*vma), GFP_KERNEL);
    pfns = lx_os_malloc(npages * sizeof(unsigned long));
    if (!vma || !pfns) {
        kfree(vma); if (pfns) lx_os_free(pfns);
        return -ENOMEM;
    }
    memset(pfns, 0, npages * sizeof(unsigned long));
    if (s->mm_prot & 1) fl |= VM_READ | VM_MAYREAD;
    if (s->mm_prot & 2) fl |= VM_WRITE | VM_MAYWRITE;
    vma->vm_mm = &kdri_dummy_mm;
    vma->vm_start = 0x100000000000ul;
    vma->vm_end = vma->vm_start + s->mm_len;
    vma->vm_pgoff = (unsigned long)(s->mm_offset >> PAGE_SHIFT);
    vm_flags_init(vma, fl);
    vma->vm_file = f->filp;
    vma->vm_page_prot = __pgprot(0);
    if (!f->filp->f_op->mmap) { ret = -ENODEV; goto out; }
    ret = f->filp->f_op->mmap(f->filp, vma);
    if (ret)
        goto out;
    if (!vma->vm_ops || !vma->vm_ops->fault) { ret = -ENODEV; goto close; }
    cap_pfn = pfns; cap_start = vma->vm_start; cap_n = npages;
    for (unsigned long i = 0; i < npages; i++) {
        struct vm_fault vmf = { .vma = vma, .address = vma->vm_start + i * PAGE_SIZE,
                                .pgoff = vma->vm_pgoff + i, .flags = FAULT_FLAG_WRITE };
        vm_fault_t r = vma->vm_ops->fault(&vmf);
        if ((r & VM_FAULT_ERROR) || !pfns[i]) { ret = -EFAULT; break; }
    }
    cap_pfn = NULL;
    if (ret)
        goto close;
    for (unsigned long i = 0; i < npages; i++) {
        unsigned int obj, idx;
        if (lx_pgobj_lookup(pfns[i] << PAGE_SHIFT, &obj, &idx)) { ret = -EFAULT; goto close; }
        if (nseg && obj == last_obj && idx == last_end) {
            s->seg[nseg - 1].npages++;
            last_end++;
            continue;
        }
        if (nseg >= LXDRI_MAX_SEG) { ret = -ENOMEM; goto close; }
        s->seg[nseg].obj = obj; s->seg[nseg].first = idx; s->seg[nseg].npages = 1; s->seg[nseg].flags = 0;
        nseg++;
        last_obj = obj; last_end = idx + 1;
    }
    s->nseg = nseg;
    ret = 0;
close:
    if (vma->vm_ops && vma->vm_ops->close)
        vma->vm_ops->close(vma);
out:
    lx_os_free(pfns);
    kfree(vma);
    return ret;
}


// ── symbols a hostile ioctl can reach but D5.3 does not serve (satoru) ─────────
// the container's generated stubs ABORT the whole process when hit, so an
// unprivileged program could end the drm service with one prime or syncobj ioctl.
// these refuse instead. they are NOT implementations: dma-buf and fd export arrive
// with D5.4 (fd_install, get_unused_fd_flags, alloc_file_pseudo, anon_inode_getfile,
// kern_mount), event wakeups with D5.5 (__wake_up_locked_key, wake_up_state). until
// then the ioctls that need them fail with an errno. (satoru)
#include <linux/eventfd.h>
#include <linux/anon_inodes.h>
#include <linux/mount.h>
#include <linux/stdarg.h>

struct file *alloc_file_pseudo(struct inode *inode, struct vfsmount *mnt, const char *name,
                               int flags, const struct file_operations *fops)
{ (void)inode; (void)mnt; (void)name; (void)flags; (void)fops; return ERR_PTR(-ENFILE); }

struct file *anon_inode_getfile(const char *name, const struct file_operations *fops, void *priv, int flags)
{ (void)name; (void)fops; (void)priv; (void)flags; return ERR_PTR(-ENFILE); }

struct file *dentry_open(const struct path *path, int flags, const struct cred *cred)
{ (void)path; (void)flags; (void)cred; return ERR_PTR(-ENFILE); }

int get_unused_fd_flags(unsigned flags) { (void)flags; return -ENFILE; }
void put_unused_fd(unsigned int fd) { (void)fd; }
void fd_install(unsigned int fd, struct file *file) { (void)fd; (void)file; }
struct vfsmount *kern_mount(struct file_system_type *type) { (void)type; return ERR_PTR(-ENODEV); }
void kern_unmount(struct vfsmount *mnt) { (void)mnt; }
void kill_anon_super(struct super_block *sb) { (void)sb; }
struct eventfd_ctx *eventfd_ctx_fdget(int fd) { (void)fd; return ERR_PTR(-ENOSYS); }
void eventfd_ctx_put(struct eventfd_ctx *ctx) { (void)ctx; }
void eventfd_signal_mask(struct eventfd_ctx *ctx, __poll_t mask) { (void)ctx; (void)mask; }
void inode_set_bytes(struct inode *inode, loff_t bytes) { (void)inode; (void)bytes; }
loff_t noop_llseek(struct file *file, loff_t offset, int whence) { (void)offset; (void)whence; return file->f_pos; }
void vma_set_file(struct vm_area_struct *vma, struct file *file) { (void)vma; (void)file; }
void __copy_overflow(int size, unsigned long count) { (void)size; (void)count; }
void kernel_fpu_begin_mask(unsigned int kfpu_mask) { (void)kfpu_mask; }
void kernel_fpu_end(void) { }
struct work_struct *current_work(void) { return NULL; }
/* ttm (bochs) maps vram objects with pfns of device memory: no such mapping can be handed to a process yet
 * (needs the page-alias op of the ttm stage), so the fault fails and the mmap fails with an errno (satoru) */
vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn, pgprot_t pgprot)
{ (void)vma; (void)addr; (void)pfn; (void)pgprot; return VM_FAULT_SIGBUS; }
void __wake_up_locked_key(struct wait_queue_head *wq_head, unsigned int mode, void *key) { (void)wq_head; (void)mode; (void)key; }

// folio_mark_*, wake_up_state, _dev_printk: now provided by lx_drm_kapi.c
// and lx_devlog.c for every family (turn 5 merge integration). (satoru)

// ── serving ─────────────────────────────────────────────────────────────────

static void serve(unsigned int i)
{
    struct lxdri_slot *s = slot_at(i);
    struct kdri_req req;
    long res;
    req.slot = s;
    req.bounce = g_ring + LXDRI_BOUNCE_OFF(i);
    req.req_id = s->req_id;
    req.euid = s->euid;
    kdri_cur = &req;
    switch (s->kind) {
    case LXDRI_K_OPEN:
        res = do_open(s);
        break;
    case LXDRI_K_IOCTL:
        res = do_ioctl(s);
        break;
    case LXDRI_K_MMAP: {
        struct kdri_file *f = file_get(s->file_id);
        if (!f) { res = -ENODEV; break; }
        s->nseg = 0;
        res = kdri_mmap_request(f, s);
        file_put(f);
        break;
    }
    default:
        res = -EINVAL;
    }
    kdri_cur = NULL;
    lx_broker_call(LX_KLD_OP_DRI_REPLY, req.req_id, (unsigned long)res, 0, 0);
}

static int claim_and_serve(void)
{
    int did = 0;
    for (unsigned int i = 0; i < LXDRI_SLOTS; i++) {
        struct lxdri_slot *s = slot_at(i);
        unsigned int q = LXDRI_S_QUEUED;
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != LXDRI_S_QUEUED)
            continue;
        if (!__atomic_compare_exchange_n(&s->state, &q, LXDRI_S_SERVING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        serve(i);
        did = 1;
    }
    return did;
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        unsigned int seen = __atomic_load_n(&g_hdr->doorbell, __ATOMIC_ACQUIRE);
        int did = drain_closes();
        did |= claim_and_serve();
        if (!did)
            lx_os_futex_wait(&g_hdr->doorbell, seen, g_cq_pending ? 10000000ll : 200000000ll);
    }
}

static void test_thread(void *arg)
{
    unsigned int t = (unsigned int)(unsigned long)arg;
    lx_os_sleep_ns(1000000000ull);
    if (kdri_app_driver)
        kdri_app_driver(t);
}

int kdrm_uapi_start(struct drm_device *dev)
{
    unsigned long minors = 0;
    void *va;
    long rc;
    if (!dev || g_hdr)
        return -EINVAL;
    if (dev->primary)
        minors |= 1;
    if (dev->render)
        minors |= 2;
    if (!minors)
        return -ENODEV;
    va = lx_os_reserve(LXDRI_RING_PAGES * 4096ul);
    if (!va)
        return -ENOMEM;
    rc = lx_broker_call(LX_KLD_OP_DRI_ATTACH, (unsigned long)va, LXDRI_RING_PAGES, minors, 0);
    if (rc < 0) {
        lx_os_unreserve(va, LXDRI_RING_PAGES * 4096ul);
        return (int)rc;     // kurono.kld.dri is off: the kernel refuses quietly (satoru)
    }
    g_dev = dev;
    g_ring = va;
    g_hdr = (struct lxdri_hdr *)va;
    lx_uaccess_hook = kdri_ua;
    lx_pgobj_enable();        /* gem memory becomes kernel-owned object frames */
    for (int i = 0; i < KDRI_WORKERS; i++)
        lx_os_thread_create(worker, NULL);
    lx_log("[kdrm-uapi] /dev/dri attached, workers started\n");
    if (g_hdr->test == 8)
        g_slow_ms = 300;
    if (g_hdr->test)
        lx_os_thread_create(test_thread, (void *)(unsigned long)g_hdr->test);
    return 0;
}

// end (satoru)
