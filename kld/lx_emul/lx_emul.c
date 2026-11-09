// lx_emul core: logging, the unimplemented trap, and the module registry.
// This is hand-written shim surface. Everything a driver needs that is NOT in
// here gets an auto-generated aborting stub (see tools/gen_stubs.sh), so the
// container always links and always names the next gap at runtime.
//
// end (satoru)

#include "lx_emul.h"

#include <unistd.h>
#include <string.h>
#include <stdlib.h>

// ── logging ─────────────────────────────────────────────────────────────────
// fd 2 is com1 under Kurono. write() directly - no stdio, because a driver can
// abort from a context where buffered output would be lost. (satoru)
void lx_log(const char* s) {
    if (!s) return;
    size_t n = strlen(s);
    if (n) (void)!write(2, s, n);
}

void lx_log_u(unsigned long v) {
    char b[24];
    int n = 0;
    char t[24];
    int k = 0;
    if (v == 0) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + (v % 10)); v /= 10; }
    while (k) b[n++] = t[--k];
    (void)!write(2, b, (size_t)n);
}

// hex, full width: at least 8 nibbles, 16 when the value needs them. the old
// printer truncated to 4 nibbles and made a real BAR look unassigned. (satoru)
void lx_log_hex(unsigned long v) {
    static const char d[] = "0123456789abcdef";
    char b[20];
    int n = 0;
    int top = (v >> 32) ? 60 : 28;
    b[n++] = '0'; b[n++] = 'x';
    for (int i = top; i >= 0; i -= 4) b[n++] = d[(v >> i) & 0xF];
    (void)!write(2, b, (size_t)n);
}

void lx_log_line(const char* s) {
    lx_log(s);
    lx_log("\n");
}

// ── the unimplemented trap ──────────────────────────────────────────────────
// deliberately loud and fatal. during phase 1 this firing IS the exit
// criterion: it proves the driver's code is executing and names what to build
// next. _exit(42) rather than abort() so there is no signal/coredump noise in
// the serial log and the exit code is greppable. (satoru)
__attribute__((noreturn)) void lx_unimpl(const char* sym) {
    lx_log("\n[lx_emul] LX_UNIMPL: ");
    lx_log(sym ? sym : "(null)");
    lx_log("\n[lx_emul] this symbol is referenced by the driver but not implemented.\n");
    lx_log("[lx_emul] container aborting (rc=42)\n");
    _exit(42);
}

// ── module registry ─────────────────────────────────────────────────────────
// upstream module_init() puts a pointer in a linker section. we cannot use that
// mechanism reliably in a userspace ELF, so the build redefines module_init to
// emit a constructor that calls lx_register_init instead. same ordering
// guarantee (registration order == declaration order within a TU). (satoru)
struct lx_call {
    void* fn;
    const char* name;
};

static struct lx_call g_inits[LX_MAX_INITCALLS];
static int g_init_n = 0;
static struct lx_call g_exits[LX_MAX_INITCALLS];
static int g_exit_n = 0;

void lx_register_init(lx_initcall_t fn, const char* name) {
    if (g_init_n >= LX_MAX_INITCALLS) {
        lx_log_line("[lx_emul] too many initcalls - raise LX_MAX_INITCALLS");
        return;
    }
    g_inits[g_init_n].fn = (void*)fn;
    g_inits[g_init_n].name = name ? name : "?";
    g_init_n++;
}

void lx_register_exit(lx_exitcall_t fn, const char* name) {
    if (g_exit_n >= LX_MAX_INITCALLS) return;
    g_exits[g_exit_n].fn = (void*)fn;
    g_exits[g_exit_n].name = name ? name : "?";
    g_exit_n++;
}

int lx_initcall_count(void) { return g_init_n; }

int lx_run_initcalls(void) {
    int ok = 0;
    for (int i = 0; i < g_init_n; i++) {
        lx_log("[lx_emul] module_init -> ");
        lx_log_line(g_inits[i].name);
        lx_initcall_t f = (lx_initcall_t)g_inits[i].fn;
        int rc = f ? f() : -1;
        lx_log("[lx_emul] module_init ");
        lx_log(g_inits[i].name);
        lx_log(" returned ");
        if (rc < 0) { lx_log("-"); lx_log_u((unsigned long)(-rc)); }
        else        { lx_log_u((unsigned long)rc); }
        lx_log("\n");
        if (rc == 0) ok++;
    }
    return ok;
}

void lx_run_exitcalls(void) {
    // reverse order, like the kernel's module teardown. (satoru)
    for (int i = g_exit_n - 1; i >= 0; i--) {
        lx_log("[lx_emul] module_exit -> ");
        lx_log_line(g_exits[i].name);
        lx_exitcall_t f = (lx_exitcall_t)g_exits[i].fn;
        if (f) f();
    }
}

// end (satoru)
