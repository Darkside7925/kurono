// uatest - user-pointer validation regression test.
//
// The audit's #1 finding was that the syscall layer dereferenced user pointers
// with no validation, so a ring-3 process could hand the kernel a KERNEL address
// and have it read or written on its behalf. src/linux/uaccess.{h,cpp} adds the
// primitives; this proves they actually reject.
//
// A PASS here means the kernel returned -EFAULT and kept running. A kernel panic
// or a silent success are both FAILURES - and note that before the fix, passing a
// kernel pointer to uname() made the kernel memset ITS OWN memory, which would
// not necessarily crash immediately. That is exactly why this test checks the
// return value rather than just "did we survive".
//
// end (satoru)

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/utsname.h>

static void logs(const char* s) { (void)!write(2, s, strlen(s)); }
static void logd(long v) {
    char b[24]; int n = 0; char t[24]; int k = 0;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (u == 0) t[k++] = '0';
    while (u) { t[k++] = (char)('0' + u % 10); u /= 10; }
    if (neg) b[n++] = '-';
    while (k) b[n++] = t[--k];
    (void)!write(2, b, (size_t)n);
}

static int fails = 0;

static void expect_efault(const char* what, long rc) {
    logs("[uatest] ");
    logs(what);
    logs(" -> rc=");
    logd(rc);
    // musl's syscall() returns -1 and sets errno; a raw syscall returns -errno.
    if (rc == -14 || (rc == -1 && errno == EFAULT)) {
        logs("  PASS (rejected)\n");
    } else {
        logs("  FAIL (expected -EFAULT)\n");
        fails++;
    }
}

int main(void) {
    logs("[uatest] user-pointer validation test starting\n");

    // a few addresses a hostile or buggy process might pass. all are outside the
    // canonical lower-half user range, so AccessOk must reject every one. (satoru)
    const unsigned long bad[] = {
        0xFFFFFFFF80000000UL,   // kernel text region
        0xFFFF800000000000UL,   // canonical upper half base
        0x0000800000000000UL,   // first address above USER_SPACE_TOP
        0xFFFFFFFFFFFFFFFFUL,   // all ones
    };
    const char* names[] = {
        "uname(kernel text)",
        "uname(upper half)",
        "uname(user_top+1)",
        "uname(~0)",
    };

    for (int i = 0; i < 4; i++)
        expect_efault(names[i], syscall(SYS_uname, (void*)bad[i]));

    for (int i = 0; i < 4; i++)
        expect_efault("getcwd(bad)", syscall(SYS_getcwd, (void*)bad[i], 256));

    // a wrapping range: base is valid but base+len crosses out of the user half.
    // This is the case a naive `ptr < LIMIT` check misses. (satoru)
    expect_efault("getcwd(wrap)",
                  syscall(SYS_getcwd, (void*)0x00007FFFFFFFFF00UL, 4096));

    // and the control: a legitimate buffer must still WORK. A validation layer
    // that rejects everything is not a fix. (satoru)
    struct utsname u;
    memset(&u, 0, sizeof(u));
    long rc = syscall(SYS_uname, &u);
    logs("[uatest] uname(valid) -> rc=");
    logd(rc);
    if (rc == 0 && u.sysname[0]) {
        logs("  PASS sysname=");
        logs(u.sysname);
        logs(" release=");
        logs(u.release);
        logs("\n");
    } else {
        logs("  FAIL (valid call must succeed)\n");
        fails++;
    }

    char cwd[256];
    cwd[0] = 0;
    rc = syscall(SYS_getcwd, cwd, sizeof(cwd));
    logs("[uatest] getcwd(valid) -> rc=");
    logd(rc);
    if (rc >= 0 || cwd[0]) { logs("  PASS cwd="); logs(cwd); logs("\n"); }
    else { logs("  FAIL\n"); fails++; }

    logs(fails == 0 ? "[uatest] OVERALL PASS\n" : "[uatest] OVERALL FAIL\n");
    return fails == 0 ? 0 : 1;
}
