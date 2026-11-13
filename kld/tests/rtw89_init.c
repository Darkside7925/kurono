// rtw89 family init order for the shared KLD container entry.
//
// cfg80211 and mac80211 first, then the two chip pci drivers (the 8852be and
// 8852ce modules own the only module_inits in this list). LINK-ONLY family:
// there is no rtw89 hardware or emulation here, so nothing in this job runs
// it on a device. (satoru)
//
// end (satoru)

#include <linux/kernel.h>

#include "kimpl/lx_kshim.h"

extern int kld_init_core(void);
extern int kld_init_main(void);
extern int kld_init_rtw8852be(void);
extern int kld_init_rtw8852ce(void);

int kld_family_init(void)
{
    int rc = kld_init_core();
    if (rc < 0)
        return rc;
    rc = kld_init_main();
    if (rc < 0)
        return rc;
    rc = kld_init_rtw8852be();
    if (rc < 0)
        return rc;
    return kld_init_rtw8852ce();
}

// end (satoru)
