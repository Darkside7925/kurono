// lx_acpi: ACPICA inside a KLD container, kernel context.
//
// the pinned tree's drivers/acpi/acpica sources are compiled a second time
// here, the way linux builds them (__KERNEL__, KGEN headers), so the
// interpreter itself needs no libc. the OS services it calls come from
// lx_acpi_osl.c; the physical table provider and the namespace bring-up live
// in lx_acpi_tables.c. (satoru)
//
// end (satoru)

#ifndef KLD_LX_ACPI_H
#define KLD_LX_ACPI_H

#include <linux/types.h>

// a value S3a had to seed into the firmware's NVS area for the DSDT's
// module-level code to expose \_SB.PC02 (and with it the touchpad). the name
// is resolved to its field address in the first table load, then the final
// load serves it through the SystemMemory handler. (satoru)
struct lx_acpi_seed {
    const char *name;
    u64 value;
    u64 addr;
    u32 bit;
    u32 len;
    u32 resolved;
};

// generated into build/<family>/acpi_blob by tools/gen_acpi_blob.py; the
// laptop tables themselves are never tracked. (satoru)
struct lx_acpi_blob_table {
    u32 length;
    u32 offset;
};
extern const unsigned char lx_acpi_blob_data[];
extern const struct lx_acpi_blob_table lx_acpi_blob_tables[];
extern const unsigned int lx_acpi_blob_ntables;

enum lx_acpi_provider {
    LX_ACPI_PROVIDER_NONE = 0,
    LX_ACPI_PROVIDER_BLOB,
    LX_ACPI_PROVIDER_BROKER,
};

// provider + namespace bring-up. returns 0 when the namespace is up.
int lx_acpi_set_provider(enum lx_acpi_provider provider);
int lx_acpi_init(struct lx_acpi_seed *seeds, int nseeds);

// the S3a enumerator, same lines and field order, through lx_log. (satoru)
int lx_acpi_i2chid_scan(void);

// every opregion/port/config write the AML performs, logged once per distinct
// (space, address, width) with the executing method, and printed as a table
// when the run is over. broker mode refuses writes not on the (empty)
// allowlist: they never reach real hardware. (satoru)
int lx_acpi_write_note(const char *space, u64 addr, u32 width);
void lx_acpi_write_census(void);

// true when the provider is the laptop broker (real hardware behind writes). (satoru)
int lx_acpi_broker_mode(void);

// companions for exactly the namespace nodes the touchpad path uses. (satoru)
struct acpi_device;
struct device;
struct pci_dev;
struct acpi_device *lx_acpi_companion_path(const char *path);
int lx_acpi_set_companion(struct device *dev, const char *path);
void lx_acpi_attach_pci_companion(struct pci_dev *pdev);

// C4: a platform device for an ACPI node, built from its _CRS by upstream
// acpi_dev_get_resources(). returns it NOT yet added, so the caller can point
// the register model at its memory resources first. (satoru)
struct platform_device;
struct platform_device *lx_acpi_platform_device_alloc(const char *path);
int lx_acpi_gsi_to_irq(u32 gsi);

// provider side (lx_acpi_tables.c): mem returns NULL outside the provider's
// own arena, in which case the OSL falls back to a zero page. (satoru)
void *lx_acpi_provider_mem(u64 where, unsigned long len);
u64 lx_acpi_provider_rsdp(void);

// hardware hooks the provider may install: blob mode leaves them NULL and the
// OSL returns zero, broker mode routes them through the kernel's ACPI ops. (satoru)
extern long (*lx_acpi_port_read_hook)(unsigned long port, unsigned int width);
extern long (*lx_acpi_port_write_hook)(unsigned long port, unsigned int width,
                                       unsigned long value);
extern long (*lx_acpi_pci_read_hook)(unsigned int seg, unsigned int bus,
                                     unsigned int dev, unsigned int fn,
                                     unsigned int reg, unsigned int width);
extern long (*lx_acpi_pci_write_hook)(unsigned int seg, unsigned int bus,
                                      unsigned int dev, unsigned int fn,
                                      unsigned int reg, unsigned int width,
                                      unsigned long value);

// what the file-mode run reported and what the container reports. (satoru)
extern unsigned long lx_acpi_opregion_reads;
extern unsigned long lx_acpi_opregion_writes;
extern unsigned long lx_acpi_port_reads;
extern unsigned long lx_acpi_pci_reads;

// measurement hook, NULL unless a test sets it: every opregion access the AML
// makes (space, write?, address, width, value). the C6.0 survey uses it to list
// what _REG, _BST, _Qxx and friends touch. pci addresses are bus<<16|dev<<8|fn.
// (satoru)
extern void (*lx_acpi_trace_hook)(const char *space, int write, u64 addr,
                                  u32 width, u64 value);

#endif

// end (satoru)
