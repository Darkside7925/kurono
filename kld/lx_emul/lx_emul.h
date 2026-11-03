// lx_emul - the KLD shim surface. see feats/kld-linux-driver-compat/readme.md.
//
// This is the Linux-internal-API emulation layer that lets an unmodified Linux
// driver run inside a Kurono userspace container process. The Kurono KERNEL
// never links any of this - that invariant is the whole point of the design.
//
// Phase 1 rule: every symbol a driver references must EXIST so the container
// links, and every unimplemented one must abort loudly with its own name the
// first time it is reached. A silent wrong answer from a shim is far worse than
// a hard stop, because it surfaces later as inexplicable device misbehaviour.
//
// end (satoru)

#ifndef KLD_LX_EMUL_H
#define KLD_LX_EMUL_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── logging ─────────────────────────────────────────────────────────────────
// everything the container says goes to fd 2, which Kurono wires to com1, so a
// headless boot log is the primary debugging surface. (satoru)
void lx_log(const char* s);
void lx_log_u(unsigned long v);
void lx_log_line(const char* s);

// ── the unimplemented trap ──────────────────────────────────────────────────
// aborts the container with the symbol name. this is a SUCCESS signal during
// phase 1: it means the loader, the link, and the call path all worked, and it
// names the next thing to implement. (satoru)
__attribute__((noreturn)) void lx_unimpl(const char* sym);

#define LX_UNIMPL(name) lx_unimpl(name)

// a generated stub body. kept as a macro so the generator output stays tiny and
// greppable. (satoru)
#define LX_STUB(sym) void sym(void) { lx_unimpl(#sym); }

// ── module registration ─────────────────────────────────────────────────────
// Linux drivers register their entry points with module_init()/module_exit(),
// which upstream expands into a special ELF section. We cannot rely on that in
// a userspace container, so the build defines module_init/module_exit to
// register into this table instead, and the container walks it. (satoru)
typedef int  (*lx_initcall_t)(void);
typedef void (*lx_exitcall_t)(void);

#define LX_MAX_INITCALLS 32

void lx_register_init(lx_initcall_t fn, const char* name);
void lx_register_exit(lx_exitcall_t fn, const char* name);

// run every registered module_init in registration order. returns the number
// that returned 0, and logs each one's name + result. (satoru)
int  lx_run_initcalls(void);
void lx_run_exitcalls(void);

// how many initcalls were registered - lets the container distinguish "the
// driver's constructor never ran" from "it ran and failed". (satoru)
int  lx_initcall_count(void);

#ifdef __cplusplus
}
#endif

#endif  // KLD_LX_EMUL_H
// end (satoru)
