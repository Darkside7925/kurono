// kld-acpi shim: the one kernel-only include in linux's (linuxized) ACPICA
// copy. kmemleak_not_leak() is a leak-checker annotation with no effect on
// behaviour; in userspace there is no kmemleak, so it is a no-op. (satoru)
#ifndef KLD_ACPI_SHIM_KMEMLEAK_H
#define KLD_ACPI_SHIM_KMEMLEAK_H
#define kmemleak_not_leak(p) ((void)(p))
#endif
// end (satoru)
