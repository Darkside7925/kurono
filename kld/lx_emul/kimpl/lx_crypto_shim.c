// lx_crypto_shim: the few kernel facilities the upstream crypto core uses
// that the container does not otherwise provide.
//
// this is a binding file, not a crypto implementation: every algorithm and
// template comes from the unmodified tree (see CRYPTOLIB in kld/Makefile).
// what lives here is module reference counting (the crypto registry refuses to
// hand out an algorithm whose owner cannot be referenced), the /proc/crypto
// registration (the container has no procfs, so it is a documented no-op) and
// the kthread exit the cryptomgr probe thread needs. (satoru)

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/init.h>
#include <linux/completion.h>

// musl entry point, declared by hand: including <pthread.h> here would collide
// with the kernel headers. (satoru)
extern void pthread_exit(void *ret) __attribute__((noreturn));

// ── module references ───────────────────────────────────────────────────────
// module reference counting lives in lx_bus.c now, together with the rest of
// the driver core: one module exists in a container (__this_module) and its
// refcount is real because the crypto registry takes a reference around every
// registered algorithm and template probe. (satoru)
// the cryptomgr probe runs in its own kthread and exits from deep inside the
// upstream code. exiting the whole container here would kill the driver on the
// first template instantiation; ending only this shim thread is what a kernel
// kthread_exit() does. the cryptomgr threads are neither joined nor stopped,
// so the lx_task bookkeeping they skip is never observed. (satoru)
void __module_put_and_kthread_exit(struct module *module, long code)
{
    module_put(module);
    pthread_exit((void *)code);
}

// ── module autoload ─────────────────────────────────────────────────────────
// every algorithm and template the containers use is linked in and registered
// by lx_crypto_ensure_init(); there is no module store to autoload from, so a
// request is a clean -ENOENT and the caller reports the missing algorithm the
// normal way instead of aborting the container. (satoru)
int __request_module(bool wait, const char *fmt, ...)
{
    (void)wait;
    (void)fmt;
    return -ENOENT;
}

// crypto_larval_wait() waits for the template probe with the killable variant.
// a container signal is not a thing here, so this is the plain timed wait. (satoru)
long wait_for_completion_killable_timeout(struct completion *x, unsigned long timeout)
{
    return (long)wait_for_completion_timeout(x, timeout);
}

// ── /proc/crypto ────────────────────────────────────────────────────────────
// the pinned config has CONFIG_PROC_FS=y, so the crypto core registers its
// debugging file. the container has no procfs; the registration is a no-op and
// the per-algorithm show() callbacks are never reached. seq_printf exists
// because those callbacks are referenced by the type tables at link time even
// when the file is never opened. (satoru)
void crypto_init_proc(void)
{
}

void crypto_exit_proc(void)
{
}

void seq_printf(struct seq_file *m, const char *fmt, ...)
{
    (void)m;
    (void)fmt;
}

// end (satoru)
