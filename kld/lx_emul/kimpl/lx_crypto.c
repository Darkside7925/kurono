// lx_crypto: bring up the unmodified kernel crypto API in the container.
//
// the crypto sources are compiled exactly as upstream builds them under
// -DMODULE, so every initcall(kld_init_<file>) below is the module_init()
// alias the kernel would run at boot. a real kernel orders them by initcall
// level (arch_initcall for the cryptomgr notifier, subsys_initcall for the
// generic algorithms and templates, late_initcall for the algapi bookkeeping),
// and that order is reproduced here. (satoru)
//
// the container has no initcall sections and its entry point is main(), so the
// order is driven by a constructor: it runs before kld_family_init() and before
// kld_test_main(), and it only needs libc malloc and futexes, which are already
// usable at that point. (satoru)

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>

#include "lx_kshim.h"

extern int kld_init_algboss(void);
extern int kld_init_aes_generic(void);
extern int kld_init_sha1_generic(void);
extern int kld_init_sha256_generic(void);
extern int kld_init_ghash_generic(void);
extern int kld_init_crypto_null(void);
extern int kld_init_ctr(void);
extern int kld_init_cbc(void);
extern int kld_init_ccm(void);
extern int kld_init_gcm(void);
extern int kld_init_cmac(void);
extern int kld_init_hmac(void);
extern int kld_init_algapi(void);

static int lx_crypto_once;

static int run_one(const char *name, int (*fn)(void))
{
    int rc = fn ? fn() : -1;

    lx_log("[lx_crypto] init ");
    lx_log(name);
    lx_log(" -> ");
    if (rc < 0) {
        lx_log("-");
        lx_log_u((unsigned long)(-rc));
    } else {
        lx_log_u((unsigned long)rc);
    }
    lx_log("\n");
    return rc;
}

// idempotent, and safe when called from more than one thread. (satoru)
void lx_crypto_ensure_init(void)
{
    if (__atomic_exchange_n(&lx_crypto_once, 1, __ATOMIC_ACQ_REL))
        return;

    // gcm's template init caches a scatterlist page for its zero block. that
    // page pointer is only meaningful once vmemmap_base points at the shim
    // page array, so the page arena must exist before the algorithms init. the
    // call is idempotent and main() repeats it after this constructor. (satoru)
    lx_dma_init();

    // the crypto registry takes a module reference around every algorithm;
    // the single module in a container starts live at one. (satoru)
    atomic_set(&__this_module.refcnt, 1);

    // arch_initcall: the cryptomgr notifier must exist before a template
    // lookup can ask it to instantiate "ccm(aes)" and friends. (satoru)
    run_one("algboss", kld_init_algboss);
    // subsys_initcall: generic implementations first, then the templates that
    // spawn them. (satoru)
    run_one("aes_generic", kld_init_aes_generic);
    run_one("sha1_generic", kld_init_sha1_generic);
    run_one("sha256_generic", kld_init_sha256_generic);
    run_one("ghash_generic", kld_init_ghash_generic);
    run_one("crypto_null", kld_init_crypto_null);
    run_one("ctr", kld_init_ctr);
    run_one("cbc", kld_init_cbc);
    run_one("ccm", kld_init_ccm);
    run_one("gcm", kld_init_gcm);
    run_one("cmac", kld_init_cmac);
    run_one("hmac", kld_init_hmac);
    // late_initcall: procfs registration; self-tests are compiled out via
    // CONFIG_CRYPTO_MANAGER_DISABLE_TESTS. (satoru)
    run_one("algapi", kld_init_algapi);
}

__attribute__((constructor))
static void lx_crypto_ctor(void)
{
    lx_crypto_ensure_init();
}

// end (satoru)
