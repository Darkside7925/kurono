// mincoretest - Linux mincore semantics self-test (musl static).
//
// Runs under glrun on the native Linux ABI. Checks the review contract: order
// (EINVAL before length 0), a fresh untouched anonymous range is valid but not
// resident, touching one page shows only that page, a hole is ENOMEM, a kernel
// vec address and a read-only vec are EFAULT, and a wrapping range is ENOMEM.
// prints "[mincoretest] PASS <n>/<n>" or the failing checks. (satoru)
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>

static int g_pass = 0, g_fail = 0;

static void check(const char* name, int ok) {
    if (ok) {
        g_pass++;
        printf("[mincoretest] PASS %s\n", name);
    } else {
        g_fail++;
        printf("[mincoretest] FAIL %s (errno %d)\n", name, errno);
    }
}

int main(void) {
    const size_t pg = 4096;

    // a fresh anon mapping, never touched: demand-zero pages are valid mappings
    // but not resident. (satoru)
    unsigned char* m = mmap(NULL, 3 * pg, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap3", m != MAP_FAILED);
    if (m == MAP_FAILED) return 1;

    // a fresh, untouched vec too: mincore must fault its page in itself. (satoru)
    unsigned char* v = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmapvec", v != MAP_FAILED);
    if (v == MAP_FAILED) return 1;

    check("zero_len_null_vec", mincore(m, 0, NULL) == 0);

    errno = 0;
    check("unaligned_einval", mincore(m + 1, pg, v) == -1 && errno == EINVAL);

    errno = 0;
    check("zero_len_unaligned_einval", mincore(m + 1, 0, NULL) == -1 && errno == EINVAL);

    // first call into a never-touched vec page: success, all pages not resident
    check("fresh_ok", mincore(m, 3 * pg, v) == 0);
    check("fresh_not_resident", (v[0] & 1) == 0 && (v[1] & 1) == 0 && (v[2] & 1) == 0);

    // touch page 1 only
    m[pg] = 1;
    memset(v, 0xAA, 3);
    check("touch_ok", mincore(m, 3 * pg, v) == 0);
    check("touch_page1", (v[0] & 1) == 0 && (v[1] & 1) == 1 && (v[2] & 1) == 0);

    // a hole in the middle of the range is ENOMEM
    munmap(m + 2 * pg, pg);
    errno = 0;
    check("hole_enomem", mincore(m, 3 * pg, v) == -1 && errno == ENOMEM);

    // vec pointing into the kernel half is EFAULT (never a kernel write)
    errno = 0;
    check("vec_kernel_efault",
          mincore(m, pg, (unsigned char*)(uintptr_t)0xffff800000100000ULL) == -1 &&
          errno == EFAULT);

    // vec on a read-only page is EFAULT
    unsigned char* ro = mmap(NULL, pg, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check("mmap_ro", ro != MAP_FAILED);
    errno = 0;
    check("vec_ro_efault", mincore(m, pg, ro) == -1 && errno == EFAULT);

    // a range that wraps out of the user half is ENOMEM, not a walk
    errno = 0;
    check("wrap_enomem",
          mincore((void*)(uintptr_t)0x7FFFFFFFF000ULL, 0x2000, v) == -1 &&
          errno == ENOMEM);

    printf("[mincoretest] %s %d/%d\n", g_fail ? "FAIL" : "PASS",
           g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}

// end (satoru)
