// the drm userspace test program (stages D5.1 to D5.3). it is compiled into the
// drmcirrus container (musl context) and is started by kdrm_uapi.c when the
// kernel flag kurono.kld.dri.test=N is set. tests 1 and 2 run as a thread of
// the container itself (its ioctls travel the same ring as any program's);
// the later tests start separate processes with posix_spawn. every line it
// prints starts with [dri-app] and the rig greps PASS/FAIL. (satoru)
//
// end (satoru)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <time.h>
#include <pthread.h>

extern char **environ;

static int g_fail;
static int g_pass;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } else { g_fail++; printf("[dri-app] CHECK FAILED line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

// ── uapi (copied from include/uapi/drm, only what the tests use) ──────────────
#define DRM_IOCTL_BASE 'd'
struct drm_version {
    int version_major, version_minor, version_patchlevel;
    size_t name_len; char *name;
    size_t date_len; char *date;
    size_t desc_len; char *desc;
};
struct drm_unique { size_t unique_len; char *unique; };
struct drm_get_cap { uint64_t capability, value; };
struct drm_set_client_cap { uint64_t capability, value; };
struct drm_mode_card_res {
    uint64_t fb_id_ptr, crtc_id_ptr, connector_id_ptr, encoder_id_ptr;
    uint32_t count_fbs, count_crtcs, count_connectors, count_encoders;
    uint32_t min_width, max_width, min_height, max_height;
};
struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh, flags, type;
    char name[32];
};
struct drm_mode_get_connector {
    uint64_t encoders_ptr, modes_ptr, props_ptr, prop_values_ptr;
    uint32_t count_modes, count_props, count_encoders;
    uint32_t encoder_id, connector_id, connector_type, connector_type_id;
    uint32_t connection, mm_width, mm_height, subpixel, pad;
};
struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid;
    struct drm_mode_modeinfo mode;
};
struct drm_mode_get_encoder { uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones; };
struct drm_mode_get_plane_res { uint64_t plane_id_ptr; uint32_t count_planes; };
struct drm_mode_get_plane {
    uint32_t plane_id, crtc_id, fb_id, possible_crtcs, gamma_size, count_format_types;
    uint64_t format_type_ptr;
};
struct drm_mode_create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct drm_mode_map_dumb { uint32_t handle, pad; uint64_t offset; };
struct drm_mode_destroy_dumb { uint32_t handle; };

#define IOCTL_VERSION     _IOWR(DRM_IOCTL_BASE, 0x00, struct drm_version)
#define IOCTL_GET_UNIQUE  _IOWR(DRM_IOCTL_BASE, 0x01, struct drm_unique)
#define IOCTL_GET_CAP     _IOWR(DRM_IOCTL_BASE, 0x0c, struct drm_get_cap)
#define IOCTL_SET_CLIENT_CAP _IOW(DRM_IOCTL_BASE, 0x0d, struct drm_set_client_cap)
#define IOCTL_GETRESOURCES _IOWR(DRM_IOCTL_BASE, 0xA0, struct drm_mode_card_res)
#define IOCTL_GETCRTC     _IOWR(DRM_IOCTL_BASE, 0xA1, struct drm_mode_crtc)
#define IOCTL_GETENCODER  _IOWR(DRM_IOCTL_BASE, 0xA6, struct drm_mode_get_encoder)
#define IOCTL_GETCONNECTOR _IOWR(DRM_IOCTL_BASE, 0xA7, struct drm_mode_get_connector)
#define IOCTL_PLANE_RES   _IOWR(DRM_IOCTL_BASE, 0xB5, struct drm_mode_get_plane_res)
#define IOCTL_GETPLANE    _IOWR(DRM_IOCTL_BASE, 0xB6, struct drm_mode_get_plane)
#define IOCTL_CREATE_DUMB _IOWR(DRM_IOCTL_BASE, 0xB2, struct drm_mode_create_dumb)
#define IOCTL_MAP_DUMB    _IOWR(DRM_IOCTL_BASE, 0xB3, struct drm_mode_map_dumb)
#define IOCTL_DESTROY_DUMB _IOWR(DRM_IOCTL_BASE, 0xB4, struct drm_mode_destroy_dumb)

static void *fresh(size_t bytes)
{
    // never-touched anonymous pages: the first write is a demand-zero fault the
    // CALLER takes while the container is writing through the bounce page. (satoru)
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static int open_card(void)
{
    return open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
}

// ── test 1: open, VERSION, GET_CAP, GET_UNIQUE, hostile arguments ───────────
static void test1(void)
{
    int fd = open_card();
    CHECK(fd >= 0, "open card0 -> %d errno %d", fd, errno);
    if (fd < 0) return;
    printf("[dri-app] t1 open card0 fd=%d\n", fd);

    struct drm_version v;
    memset(&v, 0, sizeof v);
    CHECK(ioctl(fd, IOCTL_VERSION, &v) == 0, "VERSION sizes errno %d", errno);
    printf("[dri-app] t1 version %d.%d.%d name_len=%zu date_len=%zu desc_len=%zu\n",
           v.version_major, v.version_minor, v.version_patchlevel, v.name_len, v.date_len, v.desc_len);
    char *buf = fresh(8192);
    CHECK(buf != NULL, "mmap");
    if (buf) {
        struct drm_version w;
        memset(&w, 0, sizeof w);
        w.name = buf; w.name_len = 64; w.date = buf + 128; w.date_len = 64; w.desc = buf + 256; w.desc_len = 128;
        CHECK(ioctl(fd, IOCTL_VERSION, &w) == 0, "VERSION fill errno %d", errno);
        printf("[dri-app] t1 driver name=%.*s date=%.*s desc=%.*s\n",
               (int)w.name_len, buf, (int)w.date_len, buf + 128, (int)w.desc_len, buf + 256);
        CHECK((w.name_len == 6 && memcmp(buf, "cirrus", 6) == 0) || (w.name_len == 9 && memcmp(buf, "bochs-drm", 9) == 0), "driver name is neither cirrus nor bochs-drm");
    }

    struct drm_get_cap c;
    for (uint64_t cap = 1; cap <= 8; cap++) {
        c.capability = cap; c.value = 0xdead;
        int r = ioctl(fd, IOCTL_GET_CAP, &c);
        printf("[dri-app] t1 cap %llu r=%d value=%llu\n", (unsigned long long)cap, r, (unsigned long long)c.value);
    }
    c.capability = 1; c.value = 0;
    CHECK(ioctl(fd, IOCTL_GET_CAP, &c) == 0 && c.value == 1, "DUMB_BUFFER cap %llu", (unsigned long long)c.value);

    struct drm_unique u;
    memset(&u, 0, sizeof u);
    int r = ioctl(fd, IOCTL_GET_UNIQUE, &u);
    printf("[dri-app] t1 unique r=%d len=%zu\n", r, u.unique_len);

    errno = 0;
    CHECK(ioctl(fd, _IO(DRM_IOCTL_BASE, 0xFE), 0) == -1 && (errno == ENOTTY || errno == EINVAL), "unknown ioctl -> errno %d", errno);
    errno = 0;
    CHECK(ioctl(fd, IOCTL_VERSION, (void *)0xffff800000001000ull) == -1 && errno == EFAULT, "kernel pointer -> errno %d", errno);
    errno = 0;
    CHECK(ioctl(fd, IOCTL_VERSION, (void *)0x10) == -1 && errno == EFAULT, "tiny pointer -> errno %d", errno);
    errno = 0;
    CHECK(ioctl(fd, IOCTL_VERSION, (void *)0x7ffffffff000ull) == -1 && errno == EFAULT, "unmapped pointer -> errno %d", errno);
    struct drm_version bad;
    memset(&bad, 0, sizeof bad);
    bad.name = (char *)0xffff800000002000ull; bad.name_len = 32;
    errno = 0;
    CHECK(ioctl(fd, IOCTL_VERSION, &bad) == -1 && errno == EFAULT, "nested kernel pointer -> errno %d", errno);

    int fd2 = dup(fd);
    close(fd);
    memset(&v, 0, sizeof v);
    CHECK(ioctl(fd2, IOCTL_VERSION, &v) == 0, "ioctl on the dup after closing the original");
    close(fd2);
    errno = 0;
    CHECK(ioctl(fd2, IOCTL_VERSION, &v) == -1 && errno == EBADF, "ioctl on a closed fd -> errno %d", errno);

    int fd3 = open_card();
    CHECK(fd3 >= 0, "reopen");
    if (fd3 >= 0) close(fd3);
    errno = 0;
    CHECK(open("/dev/dri/card7", O_RDWR) == -1 && errno == ENOENT, "card7 -> errno %d", errno);

    printf("[dri-app] t1 %s pass=%d fail=%d\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
}

// ── test 2: the KMS read ioctls with nested pointers ────────────────────────
static void test2(void)
{
    int g0 = g_fail;
    int fd = open_card();
    CHECK(fd >= 0, "open");
    if (fd < 0) return;
    struct drm_set_client_cap cc = { 2 /* UNIVERSAL_PLANES */, 1 };
    CHECK(ioctl(fd, IOCTL_SET_CLIENT_CAP, &cc) == 0, "SET_CLIENT_CAP errno %d", errno);

    struct drm_mode_card_res res;
    memset(&res, 0, sizeof res);
    CHECK(ioctl(fd, IOCTL_GETRESOURCES, &res) == 0, "GETRESOURCES counts errno %d", errno);
    printf("[dri-app] t2 resources crtcs=%u connectors=%u encoders=%u fbs=%u size %u..%ux%u..%u\n",
           res.count_crtcs, res.count_connectors, res.count_encoders, res.count_fbs,
           res.min_width, res.max_width, res.min_height, res.max_height);
    CHECK(res.count_crtcs >= 1 && res.count_connectors >= 1 && res.count_encoders >= 1, "no kms objects");

    uint32_t *page = fresh(4 * 4096);
    CHECK(page != NULL, "mmap");
    if (!page) return;
    uint32_t *crtcs = page, *conns = page + 256, *encs = page + 512;
    res.crtc_id_ptr = (uintptr_t)crtcs; res.connector_id_ptr = (uintptr_t)conns; res.encoder_id_ptr = (uintptr_t)encs;
    res.fb_id_ptr = 0; res.count_fbs = 0;
    CHECK(ioctl(fd, IOCTL_GETRESOURCES, &res) == 0, "GETRESOURCES fill errno %d", errno);
    printf("[dri-app] t2 crtc=%u connector=%u encoder=%u\n", crtcs[0], conns[0], encs[0]);
    CHECK(crtcs[0] && conns[0] && encs[0], "ids are zero");

    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector gc;
        memset(&gc, 0, sizeof gc);
        gc.connector_id = conns[i];
        CHECK(ioctl(fd, IOCTL_GETCONNECTOR, &gc) == 0, "GETCONNECTOR counts errno %d", errno);
        printf("[dri-app] t2 connector %u type=%u connection=%u modes=%u props=%u encoders=%u mm=%ux%u\n",
               gc.connector_id, gc.connector_type, gc.connection, gc.count_modes, gc.count_props, gc.count_encoders,
               gc.mm_width, gc.mm_height);
        struct drm_mode_modeinfo *modes = fresh(64 * sizeof(struct drm_mode_modeinfo));
        uint32_t *props = fresh(8192);
        uint32_t *encl = fresh(4096);
        uint32_t nm = gc.count_modes, np = gc.count_props;
        if (modes && props && encl && nm <= 64 && np <= 512) {
            gc.modes_ptr = (uintptr_t)modes; gc.props_ptr = (uintptr_t)props;
            gc.prop_values_ptr = (uintptr_t)(props + 1024); gc.encoders_ptr = (uintptr_t)encl;
            CHECK(ioctl(fd, IOCTL_GETCONNECTOR, &gc) == 0, "GETCONNECTOR fill errno %d", errno);
            for (uint32_t m = 0; m < nm && m < 6; m++)
                printf("[dri-app] t2   mode %s %ux%u@%u clock=%u type=0x%x\n", modes[m].name,
                       modes[m].hdisplay, modes[m].vdisplay, modes[m].vrefresh, modes[m].clock, modes[m].type);
            if (nm) CHECK(modes[0].hdisplay > 0 && modes[0].vdisplay > 0, "first mode is empty");
            printf("[dri-app] t2   mode_count=%u prop_count=%u first_enc=%u\n", nm, np, encl[0]);
        }
    }

    struct drm_mode_get_encoder ge;
    memset(&ge, 0, sizeof ge);
    ge.encoder_id = encs[0];
    CHECK(ioctl(fd, IOCTL_GETENCODER, &ge) == 0, "GETENCODER errno %d", errno);
    printf("[dri-app] t2 encoder %u type=%u crtc=%u possible=0x%x\n", ge.encoder_id, ge.encoder_type, ge.crtc_id, ge.possible_crtcs);

    struct drm_mode_crtc cr;
    memset(&cr, 0, sizeof cr);
    cr.crtc_id = crtcs[0];
    CHECK(ioctl(fd, IOCTL_GETCRTC, &cr) == 0, "GETCRTC errno %d", errno);
    printf("[dri-app] t2 crtc %u fb=%u mode_valid=%u %ux%u\n", cr.crtc_id, cr.fb_id, cr.mode_valid, cr.mode.hdisplay, cr.mode.vdisplay);

    struct drm_mode_get_plane_res pr;
    memset(&pr, 0, sizeof pr);
    CHECK(ioctl(fd, IOCTL_PLANE_RES, &pr) == 0, "PLANE_RES errno %d", errno);
    uint32_t *pl = page + 1024;
    pr.plane_id_ptr = (uintptr_t)pl;
    CHECK(ioctl(fd, IOCTL_PLANE_RES, &pr) == 0, "PLANE_RES fill errno %d", errno);
    printf("[dri-app] t2 planes=%u first=%u\n", pr.count_planes, pl[0]);
    for (uint32_t i = 0; i < pr.count_planes && i < 4; i++) {
        struct drm_mode_get_plane gp;
        memset(&gp, 0, sizeof gp);
        gp.plane_id = pl[i];
        CHECK(ioctl(fd, IOCTL_GETPLANE, &gp) == 0, "GETPLANE errno %d", errno);
        uint32_t *fmts = fresh(4096);
        gp.format_type_ptr = (uintptr_t)fmts;
        CHECK(fmts && ioctl(fd, IOCTL_GETPLANE, &gp) == 0, "GETPLANE fill errno %d", errno);
        printf("[dri-app] t2 plane %u crtc=%u fb=%u formats=%u first=0x%08x\n", gp.plane_id, gp.crtc_id, gp.fb_id,
               gp.count_format_types, fmts ? fmts[0] : 0);
    }
    struct drm_mode_card_res bad = res;
    bad.crtc_id_ptr = 0xffff800000003000ull;
    errno = 0;
    CHECK(ioctl(fd, IOCTL_GETRESOURCES, &bad) == -1 && errno == EFAULT, "kernel output pointer -> errno %d", errno);
    close(fd);
    printf("[dri-app] t2 %s fail=%d\n", g_fail == g0 ? "PASS" : "FAIL", g_fail - g0);
}


// ── test 3: dumb buffers, object frames, gem mmap, and their lifetimes ──────
static long frames_free(void)
{
    // rig op of the broker: logs the kernel counters, returns free physical frames (satoru)
    return syscall(0x4B4C4442L, 0xA6L, 0L, 0L, 0L, 0L);
}

static int dumb_new(int fd, unsigned w, unsigned h, struct drm_mode_create_dumb *c, uint64_t *off)
{
    memset(c, 0, sizeof *c);
    c->width = w; c->height = h; c->bpp = 32;
    if (ioctl(fd, IOCTL_CREATE_DUMB, c)) return -1;
    struct drm_mode_map_dumb m;
    memset(&m, 0, sizeof m);
    m.handle = c->handle;
    if (ioctl(fd, IOCTL_MAP_DUMB, &m)) return -2;
    *off = m.offset;
    return 0;
}

static void dumb_free(int fd, uint32_t handle)
{
    struct drm_mode_destroy_dumb d = { handle };
    ioctl(fd, IOCTL_DESTROY_DUMB, &d);
}

static void fillp(uint32_t *p, size_t n, uint32_t seed) { for (size_t i = 0; i < n; i++) p[i] = seed + (uint32_t)i * 2654435761u; }
static size_t checkp(const uint32_t *p, size_t n, uint32_t seed)
{
    size_t bad = 0;
    for (size_t i = 0; i < n; i++) if (p[i] != seed + (uint32_t)i * 2654435761u) bad++;
    return bad;
}

static void test3_basic(void)
{
    int fd = open_card();
    CHECK(fd >= 0, "open");
    if (fd < 0) return;
    struct drm_mode_create_dumb c1, c2;
    uint64_t o1, o2;
    CHECK(dumb_new(fd, 1024, 768, &c1, &o1) == 0, "create dumb 1 errno %d", errno);
    printf("[dri-app] t3 dumb handle=%u pitch=%u size=%llu offset=0x%llx\n", c1.handle, c1.pitch,
           (unsigned long long)c1.size, (unsigned long long)o1);
    CHECK(c1.size >= 1024ull * 768 * 4, "size");
    size_t n1 = c1.size / 4;
    uint32_t *p1 = mmap(NULL, c1.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, o1);
    CHECK(p1 != MAP_FAILED, "mmap errno %d", errno);
    if (p1 == MAP_FAILED) { close(fd); return; }
    fillp(p1, n1, 0x1000);
    CHECK(checkp(p1, n1, 0x1000) == 0, "readback");
    // the same object mapped twice shares memory
    uint32_t *p1b = mmap(NULL, c1.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, o1);
    CHECK(p1b != MAP_FAILED && p1b[5] == p1[5], "second mapping sees the data");
    p1b[5] = 0xabcdef01; CHECK(p1[5] == 0xabcdef01, "writes are shared"); p1[5] = 0x1000 + 5u * 2654435761u;
    // hostile mmap arguments
    errno = 0; CHECK(mmap(NULL, c1.size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, o1) == MAP_FAILED, "MAP_PRIVATE accepted");
    errno = 0; CHECK(mmap(NULL, c1.size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, o1) == MAP_FAILED, "PROT_EXEC accepted");
    errno = 0; CHECK(mmap(NULL, c1.size + 4096, PROT_READ, MAP_SHARED, fd, o1) == MAP_FAILED, "too long accepted");
    errno = 0; CHECK(mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x1000) == MAP_FAILED, "bogus offset accepted");
    errno = 0; CHECK(mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, o1 + 5) == MAP_FAILED, "unaligned offset accepted");
    // a handle freed while the mapping lives: the memory stays, and a new object must not reuse it
    dumb_free(fd, c1.handle);
    CHECK(checkp(p1, n1, 0x1000) == 0, "data after DESTROY_DUMB");
    CHECK(dumb_new(fd, 1024, 768, &c2, &o2) == 0, "create dumb 2");
    uint32_t *p2 = mmap(NULL, c2.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, o2);
    CHECK(p2 != MAP_FAILED, "mmap 2");
    if (p2 != MAP_FAILED) {
        CHECK(p2[100] == 0 && p2[n1 - 1] == 0, "a new object is zeroed (no stale data)");
        fillp(p2, c2.size / 4, 0x77770000);
        CHECK(checkp(p1, n1, 0x1000) == 0, "first mapping untouched by the second object");
        munmap(p2, c2.size);
    }
    dumb_free(fd, c2.handle);
    CHECK(checkp(p1, n1, 0x1000) == 0, "data after both handles are gone");
    munmap(p1, c1.size);
    munmap(p1b, c1.size);
    close(fd);
    printf("[dri-app] t3 basic %s\n", g_fail ? "FAIL" : "ok");
}

static long test3_pass(int base, int rounds, int *badp)
{
    long f0 = frames_free();
    int bad = 0;
    for (int ii = 0; ii < rounds; ii++) {
        int i = base + ii;
        if (i < 16 || i % 25 == 0) { printf("[dri-app] t3 round %d variant %d\n", i, i % 8); fflush(stdout); }
        int fd = open_card();
        struct drm_mode_create_dumb c;
        uint64_t off;
        if (fd < 0 || dumb_new(fd, 256, 256, &c, &off)) { bad++; if (fd >= 0) close(fd); continue; }
        size_t n = c.size / 4;
        uint32_t *p = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
        if (p == MAP_FAILED) { bad++; dumb_free(fd, c.handle); close(fd); continue; }
        fillp(p, n, (uint32_t)i);
        switch (i % 8) {
        case 0: munmap(p, c.size); break;
        case 1: munmap(p, 4096); munmap(p + 1024, c.size - 4096); break;
        case 2: munmap(p + 1024 * 8, 4096 * 8); munmap(p, 4096 * 8); munmap(p + 1024 * 16, c.size - 4096 * 16); break;
        case 3: mprotect(p, c.size, PROT_READ); if (checkp(p, n, (uint32_t)i)) bad++; munmap(p, c.size); break;
        case 4: dumb_free(fd, c.handle); c.handle = 0; if (checkp(p, n, (uint32_t)i)) bad++; munmap(p, c.size); break;
        case 5: close(fd); fd = -1; if (checkp(p, n, (uint32_t)i)) bad++; munmap(p, c.size); break;
        case 6: {
            void *q = mremap(p, c.size, c.size / 2, 0);
            if (q == MAP_FAILED || checkp(q, n / 2, (uint32_t)i)) bad++;
            munmap(q == MAP_FAILED ? p : q, q == MAP_FAILED ? c.size : c.size / 2);
            break;
        }
        case 7: {       // fork lives in the child-process test (mode "fork"): a multithreaded container is not the subject here
            munmap(p, c.size);
            break;
        }
        }
        if (fd >= 0) { if (c.handle) dumb_free(fd, c.handle); close(fd); }
    }
    { struct timespec w = { 1, 0 }; nanosleep(&w, NULL); }      // the last releases run on the container's worker
    long f1 = frames_free();
    printf("[dri-app] t3 pass rounds=%d..%d bad=%d free_frames before=%ld after=%ld delta=%ld\n", base, base + rounds - 1, bad, f0, f1, f1 - f0);
    *badp += bad;
    return f1 - f0;
}

// four passes of 50 rounds: growth in the first passes is allocator warm-up (container heap, kernel
// heap arenas); a leak per round would keep growing, so the last two passes must be flat. (satoru)
static void test3_loop(int rounds)
{
    int bad = 0;
    long d[4];
    for (int p = 0; p < 4; p++) d[p] = test3_pass(p * (rounds / 4), rounds / 4, &bad);
    CHECK(bad == 0, "%d rounds failed", bad);
    /* frame deltas are printed, not judged: the kernel object, file and slot counts (rig) and the exact kill test are the leak checks */
    (void)d;
}

// children: separate unprivileged processes (the kernel starts them, op CHILD_SPAWN) ────────
#define KLDB 0x4B4C4442L
enum { M_HOLD = 1, M_EXIT = 2, M_FORK = 3, M_FUZZ = 4, M_NOOP = 5, M_SLEEP = 6, M_HAMMER = 7, M_FDTEST = 8, M_CGLOOP = 9, M_CGANON = 10, M_SHELLFD = 11 };
static long child_spawn(int mode, int arg) { return syscall(KLDB, 0xA7L, (long)mode, (long)arg, 0L, 0L); }
static long child_state(long h) { return syscall(KLDB, 0xA9L, h, 0L, 0L, 0L); }
static void ms_sleep(long ms) { struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }
static int child_ready(long h, int timeout_ms)
{
    char path[48];
    snprintf(path, sizeof path, "/tmp/dri_rdy_%ld", h);
    for (int t = 0; t < timeout_ms; t += 5) {
        int fdm = open(path, O_RDONLY);
        if (fdm >= 0) { close(fdm); unlink(path); return 1; }
        ms_sleep(5);
    }
    return 0;
}
static int child_kill_wait(long h)       // kill, then wait for the exit to be reaped; 1 on success
{
    for (int t = 0; t < 4000; t++) {
        long st = child_state(h);
        if (st >= 0 && (st & 0xff) == 2) return 1;
        if (st >= 0 && (st & 0xff) == 1) syscall(KLDB, 0xA8L, h, 0L, 0L, 0L);
        ms_sleep(2);
    }
    return 0;
}
static int child_wait_exit(long h, int timeout_ms, int *rc)
{
    for (int t = 0; t < timeout_ms; t += 5) {
        long st = child_state(h);
        if (st >= 0 && (st & 0xff) == 2) { if (rc) *rc = (int)((st >> 40) & 0xff); return 1; }
        ms_sleep(5);
    }
    return 0;
}
static void child_free(long h) { syscall(KLDB, 0xAAL, h, 0L, 0L, 0L); }

int kdri_child_main(int argc, char **argv)
{
    const char *mode = argv[2];
    int handle = argc > 3 ? atoi(argv[3]) : 0;
    int arg = argc > 4 ? atoi(argv[4]) : 0;
    if (!strcmp(mode, "noop")) return 0;
    if (!strcmp(mode, "sleep")) { ms_sleep(30000); return 0; }
    if (!strcmp(mode, "hammer")) {
        int hfd = open_card();
        char hp[48]; snprintf(hp, sizeof hp, "/tmp/dri_rdy_%d", handle);
        int hm = open(hp, O_CREAT | O_WRONLY, 0644); if (hm >= 0) close(hm);
        uint32_t *hp2 = fresh(4096 * 4);
        for (;;) {      // many uaccess round trips; the parent kills this process in the middle of one
            struct drm_mode_card_res res; memset(&res, 0, sizeof res);
            ioctl(hfd, IOCTL_GETRESOURCES, &res);
            res.crtc_id_ptr = (uintptr_t)hp2; res.connector_id_ptr = (uintptr_t)(hp2 + 256); res.encoder_id_ptr = (uintptr_t)(hp2 + 512);
            ioctl(hfd, IOCTL_GETRESOURCES, &res);
        }
    }
    int fd = open_card();
    if (fd < 0) { printf("[dri-app] child open failed errno %d\n", errno); return 2; }
    if (!strcmp(mode, "fuzz")) {
        uint64_t seed = 88172645463325252ull + (uint64_t)arg;
        char *page = fresh(1 << 16);
        int ok = 0, err = 0;
        for (int i = 0; i < arg; i++) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            unsigned cmd = (unsigned)(seed & 0xFF) | ((unsigned)'d' << 8) | (unsigned)((seed >> 8) & 3) << 30 | (unsigned)((seed >> 12) & 0x3FFF) << 16;
            if ((seed >> 26) & 1) cmd = (unsigned)(seed >> 3);                       // any type at all
            if (!((seed >> 27) & 3)) cmd = (cmd & 0xFFFF) | (0x00C0u << 16) | (2u << 30) | ((unsigned)'d' << 8);   // plausible sizes
            unsigned long arg_ptr;
            switch ((seed >> 24) & 7) {
            case 0: arg_ptr = 0; break;
            case 1: arg_ptr = 0xffff800000001000ul; break;
            case 2: arg_ptr = 0x7ffffffff000ul; break;
            case 3: arg_ptr = (unsigned long)-1; break;
            case 4: arg_ptr = (unsigned long)page + ((seed >> 29) & 0xFFF0); break;
            case 5: arg_ptr = 0x10; break;
            default: arg_ptr = (unsigned long)page; break;
            }
            if (page && ((seed >> 31) & 1)) for (int k = 0; k < 64; k++) page[k] = (char)(seed >> (k & 31));
            int r = ioctl(fd, cmd, arg_ptr);
            if (r < 0) err++; else ok++;
        }
        printf("[dri-app] fuzz child done calls=%d ok=%d err=%d uid=%d\n", arg, ok, err, (int)getuid());
        return 0;
    }
    if (!strcmp(mode, "cgloop")) {
        // the kernel put this process in a cgroup with a memory limit (kurono.kld.dri.cg=1). phase A keeps mapping
        // 1 MiB objects until the limit refuses one (proves the drm mappings are charged); phase B maps and unmaps
        // arg times, which only works for 200 rounds when every unmap uncharges what the map charged. (satoru)
        enum { NH = 48 };
        static void *hp_[NH]; static struct drm_mode_create_dumb hc[NH];
        int held = 0, first_fail = -1, ferr = 0;
        for (int i = 0; i < NH; i++) {
            uint64_t o2;
            if (dumb_new(fd, 256, 1024, &hc[i], &o2)) { first_fail = i; ferr = 1000 + errno; break; }
            hp_[i] = mmap(NULL, hc[i].size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, o2);
            if (hp_[i] == MAP_FAILED) { first_fail = i; ferr = errno; dumb_free(fd, hc[i].handle); break; }
            ((volatile uint32_t *)hp_[i])[0] = 0xA5A50000u + (uint32_t)i;
            held++;
        }
        printf("[dri-app] cgloop phase A: %d x 1 MiB held, first refusal at object %d errno %d\n", held, first_fail, ferr);
        fflush(stdout);
        for (int i = 0; i < held; i++) { munmap(hp_[i], hc[i].size); dumb_free(fd, hc[i].handle); }
        int fails = 0, first_err = 0;
        for (int r = 0; r < arg; r++) {
            struct drm_mode_create_dumb c2; uint64_t o2;
            if (dumb_new(fd, 256, 1024, &c2, &o2)) { fails++; if (!first_err) first_err = 1000 + errno; continue; }
            void *m = mmap(NULL, c2.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, o2);
            if (m == MAP_FAILED) { fails++; if (!first_err) first_err = errno; dumb_free(fd, c2.handle); continue; }
            ((volatile uint32_t *)m)[0] = (uint32_t)r;
            munmap(m, c2.size);
            dumb_free(fd, c2.handle);
        }
        printf("[dri-app] cgloop phase B: %d rounds, %d failed (first errno %d)\n", arg, fails, first_err);
        fflush(stdout);
        return (held >= 2 && first_fail >= 0 && fails == 0) ? 0 : 7;
    }
    if (!strcmp(mode, "cganon")) {
        // the same rounds with plain anonymous memory and no drm object at all: if this fails like cgloop does, the ledger
        // imbalance is the kernel's munmap, not this interface. anonymous memory is charged when a page is first touched
        // and a refused charge there kills the process, so there is no hold-until-refused phase: the process is simply
        // killed (rc 255) when the ledger runs out. (satoru)
        enum { NH = 6, SZ = 1 << 20 };
        static void *ap_[NH];
        for (int i = 0; i < NH; i++) {
            ap_[i] = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            for (int o = 0; o < SZ; o += 4096) ((volatile char *)ap_[i])[o] = 1;
        }
        printf("[dri-app] cganon phase A: %d x 1 MiB touched and held\n", NH);
        fflush(stdout);
        for (int i = 0; i < NH; i++) munmap(ap_[i], SZ);
        for (int r = 0; r < arg; r++) {
            void *m = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (m == MAP_FAILED) { printf("[dri-app] cganon round %d mmap failed errno %d\n", r, errno); fflush(stdout); return 7; }
            for (int o = 0; o < SZ; o += 4096) ((volatile char *)m)[o] = 1;
            munmap(m, SZ);
            if (r % 20 == 19) { printf("[dri-app] cganon round %d ok\n", r + 1); fflush(stdout); }
        }
        printf("[dri-app] cganon phase B: %d rounds done\n", arg);
        fflush(stdout);
        return 0;
    }
    struct drm_mode_create_dumb c;
    uint64_t off;
    unsigned rows = 256;
    if (!strcmp(mode, "map-exit") && arg > 0) rows = (unsigned)arg;       // arg = rows of a 256 pixel wide buffer (1 KiB per row)
    if (dumb_new(fd, 256, rows, &c, &off)) { printf("[dri-app] child create failed\n"); return 3; }
    size_t n = c.size / 4;
    uint32_t *p = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
    if (p == MAP_FAILED) { printf("[dri-app] child mmap failed errno %d\n", errno); return 4; }
    uint32_t seed = 0xC0DE0000u + (uint32_t)getpid();
    fillp(p, n, seed);
    char path[48];
    snprintf(path, sizeof path, "/tmp/dri_rdy_%d", handle);
    int mk = open(path, O_CREAT | O_WRONLY, 0644);
    if (mk >= 0) close(mk);
    if (!strcmp(mode, "map-exit")) return 0;
    if (!strcmp(mode, "fork")) {
        pid_t kid = fork();
        if (kid == 0) { p[0] = 0x5151; _exit(0); }      // MAP_SHARED: the parent must see it, no copy-on-write of a device mapping
        int st = 0; waitpid(kid, &st, 0);
        printf("[dri-app] child fork: kid status 0x%x parent sees p[0]=0x%x %s\n", st, p[0], p[0] == 0x5151 ? "SHARED-OK" : "NOT-SHARED");
        return (WIFEXITED(st) && p[0] == 0x5151) ? 0 : 6;
    }
    int okc = 0, badc = 0, last_ioctl = 0;
    for (int s = 0; s < arg * 10; s++) {      // arg seconds, checked every 100 ms
        if (checkp(p, n, seed)) badc++; else okc++;
        if (s % 10 == 9) {
            struct drm_version vv; memset(&vv, 0, sizeof vv);
            errno = 0; last_ioctl = ioctl(fd, IOCTL_VERSION, &vv) == 0 ? 0 : errno;
            printf("[dri-app] child %d verify ok=%d bad=%d ioctl=%d\n", (int)getpid(), okc, badc, last_ioctl);
        }
        ms_sleep(100);
    }
    printf("[dri-app] child %d done ok=%d bad=%d ioctl=%d\n", (int)getpid(), okc, badc, last_ioctl);
    return badc ? 5 : 0;
}

// 200 x { start a process that maps a gem object, kill it at a random point }, then 50 x exit normally.
// every process here is a copy of an 11 MiB image and the kernel's per-process cost is not this
// interface's: it is measured first (a process that sleeps and is killed, a process that exits),
// and the drm runs are judged against it. (satoru)
static long settle_frames(void) { ms_sleep(1500); return frames_free(); }

static void test4(void)
{
    int bad = 0, started = 0;
    long b0 = frames_free();
    for (int i = 0; i < 40; i++) {
        long h = child_spawn(M_SLEEP, 0);
        if (h <= 0) { bad++; continue; }
        ms_sleep(200);
        if (!child_kill_wait(h)) bad++;
        child_free(h);
    }
    long b1 = settle_frames();
    long kill_cost = (b0 - b1) / 40;
    long e0 = frames_free();
    for (int i = 0; i < 20; i++) { long h = child_spawn(M_NOOP, 0); int rc; if (h > 0) { if (!child_wait_exit(h, 5000, &rc)) bad++; child_free(h); } else bad++; }
    long e1 = settle_frames();
    long exit_cost = (e0 - e1) / 20;
    printf("[dri-app] t4 baseline per process: killed sleeper %ld frames, normal exit %ld frames\n", kill_cost, exit_cost);

    long k0 = frames_free();
    for (int i = 0; i < 200; i++) {
        long h = child_spawn(M_HOLD, 30);
        if (h <= 0) { bad++; ms_sleep(20); continue; }
        started++;
        if (i % 2 == 0) { if (!child_ready(h, 4000)) bad++; }        // killed with the mapping live
        else ms_sleep(150 + (i * 7919) % 120);                        // killed anywhere in its setup
        if (!child_kill_wait(h)) bad++;
        child_free(h);
    }
    long k1 = settle_frames();
    long x0 = frames_free();
    for (int i = 0; i < 50; i++) {
        long h = child_spawn(M_EXIT, 0);
        int rc = -1;
        if (h <= 0) { bad++; ms_sleep(20); continue; }
        started++;
        if (!child_wait_exit(h, 8000, &rc)) bad++;
        child_free(h);
    }
    long x1 = settle_frames();
    // does an exit leak scale with the mapped drm object? 20 children with 256 KiB, 20 with 2 MiB
    long s0 = frames_free();
    for (int i = 0; i < 20; i++) { long h = child_spawn(M_EXIT, 256); int rc; if (h > 0) { if (!child_wait_exit(h, 8000, &rc)) bad++; child_free(h); } else bad++; }
    long s1 = settle_frames();
    for (int i = 0; i < 20; i++) { long h = child_spawn(M_EXIT, 2048); int rc; if (h > 0) { if (!child_wait_exit(h, 8000, &rc)) bad++; child_free(h); } else bad++; }
    long s2 = settle_frames();
    printf("[dri-app] t4 exit scaling: 20 x 256 KiB cost %ld frames, 20 x 2 MiB cost %ld frames (difference per process %ld; the object difference is 1792 KiB = 448 frames)\n",
           s0 - s1, s1 - s2, ((s1 - s2) - (s0 - s1)) / 20);
    long kill_extra = (k0 - k1) - 200 * kill_cost;
    long exit_extra = (x0 - x1) - 50 * exit_cost;
    printf("[dri-app] t4 started=%d bad=%d killed-with-drm: delta %ld frames (baseline %ld, extra %ld); exited-with-drm: delta %ld (baseline %ld, extra %ld)\n",
           started, bad, k0 - k1, 200 * kill_cost, kill_extra, x0 - x1, 50 * exit_cost, exit_extra);
    CHECK(bad == 0, "%d children failed", bad);
    CHECK(kill_extra <= 1000, "killed children leak %ld frames beyond the baseline", kill_extra);   /* a leaked 256 KiB object would be 12800; the rest is allocator step noise (kmemx pool +512) */
    // normal exit: informational. the kernel does not free user frames of a process that exits normally (a no-drm
    // child shows the same: baseline above), and the scaling line shows the drm mapping goes with them. kill and munmap are exact.
}

// the container dies while three processes map objects: they must keep their data. (satoru)
static void test5(void)
{
    long h[3];
    for (int i = 0; i < 3; i++) {
        h[i] = child_spawn(M_HOLD, 25);
        if (h[i] <= 0) { printf("[dri-app] t5 FAIL spawn\n"); return; }
    }
    for (int i = 0; i < 3; i++) if (!child_ready(h[i], 5000)) { printf("[dri-app] t5 FAIL child not ready\n"); return; }
    printf("[dri-app] t5 children ready, killing the container now\n");
    fflush(stdout);
    syscall(KLDB, 0xABL, 0L, 0L, 0L, 0L);        // the kernel ends this process group like a crash would
    for (;;) ms_sleep(1000);
}

static void test6(void)
{
    int g0 = g_fail;
    long hf = child_spawn(M_FORK, 0);
    int rc = -1;
    CHECK(hf > 0 && child_wait_exit(hf, 8000, &rc), "fork child did not exit");   /* the shared-mapping verdict is the child's SHARED-OK line */
    child_free(hf);
    long h = child_spawn(M_FUZZ, 10000);
    CHECK(h > 0, "spawn fuzz");
    if (h > 0) {
        CHECK(child_wait_exit(h, 280000, &rc), "fuzz child did not finish");
        child_free(h);
    }
    int fd = open_card();
    struct drm_version v; memset(&v, 0, sizeof v);
    CHECK(fd >= 0 && ioctl(fd, IOCTL_VERSION, &v) == 0 && v.name_len == 6, "interface alive after the fuzz");
    if (fd >= 0) close(fd);
    printf("[dri-app] t6 %s fail=%d\n", g_fail == g0 ? "PASS" : "FAIL", g_fail - g0);
}


// ── test 8: parked requests and their callers ────────────────────────────────
// the container delays GETRESOURCES by 300 ms in this test (kdrm_uapi.c g_slow_ms) so a thread can be
// parked inside it. (satoru)
static volatile int g_t8_fd;
static pthread_t g_main_th;
static volatile int g_sig_seen, g_sig_ret;

static void *t8_closer(void *arg) { (void)arg; ms_sleep(50); close(g_t8_fd); return NULL; }
static void *t8_ioctl(void *arg)
{
    struct drm_mode_card_res res; memset(&res, 0, sizeof res);
    int r = ioctl(g_t8_fd, IOCTL_GETRESOURCES, &res);
    *(int *)arg = r == 0 ? 0 : errno;
    return NULL;
}
static void t8_handler(int sig)
{
    struct drm_get_cap c = { 1, 0 };
    int fd = open_card();
    g_sig_ret = (fd >= 0 && ioctl(fd, IOCTL_GET_CAP, &c) == 0 && c.value == 1) ? 1 : -1;
    if (fd >= 0) close(fd);
    g_sig_seen++;
    (void)sig;
}
static void *t8_signaller(void *arg) { (void)arg; ms_sleep(100); pthread_kill(g_main_th, SIGUSR1); return NULL; }
static void *t8_timed(void *arg)
{
    struct drm_mode_card_res res; memset(&res, 0, sizeof res);
    int fd = open_card();
    ioctl(fd, IOCTL_GETRESOURCES, &res);
    close(fd);
    (void)arg;
    return NULL;
}
static long now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L; }

static void test8(void)
{
    int g0 = g_fail;
    // A: another thread closes the fd while this one is parked in an ioctl, 64 times; slots must not pile up
    int ebadf = 0, other = 0;
    for (int i = 0; i < 64; i++) {
        g_t8_fd = open_card();
        pthread_t a, b; int res = -1;
        pthread_create(&b, NULL, t8_closer, NULL);
        pthread_create(&a, NULL, t8_ioctl, &res);
        pthread_join(a, NULL); pthread_join(b, NULL);
        if (res == EBADF) ebadf++; else if (res != 0) other++;
    }
    printf("[dri-app] t8 close-while-parked x64: EBADF %d, other errno %d, completed %d\n", ebadf, other, 64 - ebadf - other);
    CHECK(other == 0, "unexpected errno");
    ms_sleep(800);
    frames_free();      // logs the kernel counters: reqs must be back to 0 (the rig checks it)

    // B: a signal handler issues drm calls while the main thread is parked in GETRESOURCES. other signals
    // wait until the call returns (like a real uninterruptible ioctl), so the call must complete first
    // and the handler must then see its own, correct answers.
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = t8_handler; sigaction(SIGUSR1, &sa, NULL);
    int fd = open_card();
    struct drm_mode_card_res res; memset(&res, 0, sizeof res);
    g_main_th = pthread_self();
    pthread_t sg; pthread_create(&sg, NULL, t8_signaller, NULL);
    int r = ioctl(fd, IOCTL_GETRESOURCES, &res);
    pthread_join(sg, NULL);
    ms_sleep(50);
    printf("[dri-app] t8 signal: ioctl r=%d crtcs=%u, handler ran %d time(s), handler result %d\n", r, res.count_crtcs, g_sig_seen, g_sig_ret);
    CHECK(r == 0 && res.count_crtcs >= 1, "the interrupted call lost its reply");
    CHECK(g_sig_seen == 1 && g_sig_ret == 1, "the handler's own call did not work");
    close(fd);

    // C: callers killed in the middle of uaccess round trips and NOT reaped for 10 s (kurono.kld.dri.noreap=1):
    // all 8 workers must stay available. 8 concurrent slow requests take ~300 ms when all 8 are free.
    long h[4];
    for (int i = 0; i < 4; i++) h[i] = child_spawn(M_HAMMER, 0);
    for (int i = 0; i < 4; i++) CHECK(h[i] > 0 && child_ready(h[i], 6000), "hammer %d not ready", i);
    ms_sleep(300);
    for (int i = 0; i < 4; i++) if (h[i] > 0) syscall(KLDB, 0xA8L, h[i], 0L, 0L, 0L);
    ms_sleep(400);       // killed (the kill waits 150 ms after launch), still unreaped
    pthread_t th[8];
    long t0 = now_ms();
    for (int i = 0; i < 8; i++) pthread_create(&th[i], NULL, t8_timed, NULL);
    for (int i = 0; i < 8; i++) pthread_join(th[i], NULL);
    long el = now_ms() - t0;
    printf("[dri-app] t8 workers after 4 callers died unreaped: 8 concurrent slow requests took %ld ms (about 300 when all 8 are free)\n", el);
    CHECK(el < 520, "workers stuck: %ld ms", el);
    for (int i = 0; i < 4; i++) if (h[i] > 0) { child_kill_wait(h[i]); child_free(h[i]); }
    frames_free();
    printf("[dri-app] t8 %s fail=%d\n", g_fail == g0 ? "PASS" : "FAIL", g_fail - g0);
}


// ── tests 9 to 11: bounded investigations of the kernel's process exit and launch paths ────────
// I1: what do exits cost, and does it come back later? (no drm in any of these children)
static void test9(void)
{
    static const struct { int mode; const char *name; int n; } kinds[] = { { M_NOOP, "noop (11 MiB image) exits", 20 }, { M_SLEEP, "sleeper (11 MiB image) killed", 20 }, { M_FDTEST, "fdtest (small embedded image) exits", 20 } };
    for (unsigned k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        long f0 = frames_free();
        for (int i = 0; i < kinds[k].n; i++) {
            long h = child_spawn(kinds[k].mode, 0);
            int rc; if (h <= 0) continue;
            if (kinds[k].mode == M_SLEEP) { ms_sleep(200); child_kill_wait(h); }
            else child_wait_exit(h, 8000, &rc);
            child_free(h);
        }
        long f1 = frames_free();
        ms_sleep(5000);  long f5 = frames_free();
        ms_sleep(25000); long f30 = frames_free();
        ms_sleep(60000); long f90 = frames_free();
        printf("[dri-app] t9 %s x%d: cost at once %ld frames, still lost after 5 s %ld, after 30 s %ld, after 90 s %ld (per process at once %ld, at 90 s %ld)\n",
               kinds[k].name, kinds[k].n, f0 - f1, f0 - f5, f0 - f30, f0 - f90, (f0 - f1) / kinds[k].n, (f0 - f90) / kinds[k].n);
    }
    printf("[dri-app] t9 PASS (measurement only)\n");
}

// I2: kill a process that is still being launched, 20 times (kurono.kld.dri.killnow=1 removes the wait).
static void test10(void)
{
    int started = 0;
    for (int i = 0; i < 20; i++) {
        long h = child_spawn(M_SLEEP, 0);
        if (h <= 0) continue;
        started++;
        syscall(KLDB, 0xA8L, h, 0L, 0L, 0L);                 // kill at once
        child_kill_wait(h);
        child_free(h);
        printf("[dri-app] t10 attempt %d survived\n", i);
        fflush(stdout);
    }
    printf("[dri-app] t10 PASS started=%d (no crash)\n", started);
}

// I2 control: the first runner ended itself with Scheduler::Exit(); kurono.kld.dri.runexit=1 repeats that.
static void test11(void)
{
    long h = child_spawn(M_NOOP, 0); int rc;
    if (h > 0) child_wait_exit(h, 8000, &rc);
    ms_sleep(3000);
    printf("[dri-app] t11 PASS (the runner ended with Scheduler::Exit and nothing crashed)\n");
}

// item 9: the cgroup ledger. a child in a cgroup with memory.max = 16 MiB maps and unmaps 1 MiB objects (cgloop), and
// the same with anonymous memory (cganon, the control).
static int cg_run(int mode, const char *name)
{
    frames_free();
    long h = child_spawn(mode, 200);
    int rc = -1, done = 0;
    CHECK(h > 0, "spawn %s", name);
    for (int t = 0; h > 0 && t < 600 && !done; t++) {      // up to 5 min; the kernel counters are printed while it runs
        done = child_wait_exit(h, 500, &rc);
        if (t % 4 == 3) frames_free();
    }
    CHECK(done, "%s child did not finish", name);
    printf("[dri-app] t12 %s child verdict %d (0 = limit enforced and 200 rounds balanced)\n", name, rc);
    child_free(h);
    frames_free();
    return rc;
}

static void test12(void)
{
    int g0 = g_fail;
    int rd = cg_run(M_CGLOOP, "drm objects");
    int ra = cg_run(M_CGANON, "anonymous control");
    /* the verdict that must hold: the drm path is no worse than plain anonymous memory */
    CHECK(rd == 0 || ra != 0, "drm mappings leak the ledger while anonymous memory does not (drm %d anon %d)", rd, ra);
    printf("[dri-app] t12 %s fail=%d drm_rc=%d anon_rc=%d\n", g_fail == g0 ? "PASS" : "FAIL", g_fail - g0, rd, ra);
}

// I1: the same small program (fdtest, 20 runs), started two ways: the way the kernel runner processes start children
// (spawn + wait, nobody destroys the leader) and the way the shell does (the `fdtest` command: run to the end, then
// Scheduler::DestroyProcess). frames before and after, settled.
static void test13(void)
{
    static const struct { int mode; const char *name; } kinds[] = { { M_FDTEST, "runner path (SpawnDetached + ReapDetached)" }, { M_SHELLFD, "shell path (`fdtest` command: RunProcessWithArgs + DestroyProcess)" } };
    for (unsigned k = 0; k < 2; k++) {
        long f0 = settle_frames();
        for (int i = 0; i < 20; i++) {
            long h = child_spawn(kinds[k].mode, 0);
            int rc; if (h <= 0) { printf("[dri-app] t13 spawn failed\n"); continue; }
            child_wait_exit(h, 20000, &rc);
            child_free(h);
        }
        long f1 = frames_free();
        long f2 = settle_frames();
        ms_sleep(8000); long f3 = frames_free();
        printf("[dri-app] t13 %s: 20 runs cost %ld frames at once, %ld settled, %ld after 8 s more (per run %ld)\n",
               kinds[k].name, f0 - f1, f0 - f2, f0 - f3, (f0 - f3) / 20);
    }
    printf("[dri-app] t13 PASS (measurement only)\n");
}

void kdri_app_driver(unsigned int test)
{
    printf("[dri-app] test %u start\n", test);
    switch (test) {
    case 1: test1(); break;
    case 2: test1(); test2(); break;
    case 3: test3_basic(); printf("[dri-app] t3 %s pass=%d fail=%d\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail); break;
    case 4: test3_basic(); test3_loop(200); printf("[dri-app] t3 loop %s pass=%d fail=%d\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail); break;
    case 5: test4(); printf("[dri-app] t4 %s pass=%d fail=%d\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail); break;
    case 6: test5(); break;
    case 7: test6(); break;
    case 8: test8(); break;
    case 9: test9(); break;
    case 10: test10(); break;
    case 11: test11(); break;
    case 12: test12(); break;
    case 13: test13(); break;
    default: printf("[dri-app] unknown test %u\n", test);
    }
    fflush(stdout);
}

// end (satoru)
