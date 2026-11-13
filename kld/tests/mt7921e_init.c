// mt7921e family init order for the shared KLD container entry.
//
// cfg80211 and mac80211 first, then the mt7921 pci driver (the only
// module_init in this family's source list). LINK-ONLY family: there is no
// mt7921 hardware or emulation here, so nothing in this job runs it on a
// device. (satoru)
//
// end (satoru)

#include <linux/kernel.h>

#include "kimpl/lx_kshim.h"

extern int kld_init_core(void);
extern int kld_init_main(void);
extern int kld_init_pci(void);

int kld_family_init(void)
{
    int rc = kld_init_core();
    if (rc < 0)
        return rc;
    rc = kld_init_main();
    if (rc < 0)
        return rc;
    return kld_init_pci();
}

// end (satoru)
