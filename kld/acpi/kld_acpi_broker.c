// kld-acpi: the broker backend, used with --broker inside Kurono.
//
// The tables, physical mappings, port i/o and pci config all come from the
// kernel broker's ACPI ops, which only the address space the kernel's own
// kld-acpi runner registered may use. MAP_PHYS refuses usable ram outright, so
// this container sees acpi tables, nvs and mmio - never kernel memory. (satoru)
//
// end (satoru)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>

#include <acpi/acpi.h>
#include "kld_acpi.h"

#define KLD_SYS        0x4B4C4442l
#define OP_RSDP_READ   0x0050
#define OP_MAP_PHYS    0x0051
#define OP_PORT_IN     0x0052
#define OP_PORT_OUT    0x0053
#define OP_PCI_READ    0x0054
#define OP_PCI_WRITE   0x0055

static long kb(unsigned op, unsigned long a0, unsigned long a1, unsigned long a2, unsigned long a3)
{
    return syscall(KLD_SYS, op, a0, a1, a2, a3);
}

static unsigned char kld_rsdp_copy[64];
#define KLD_FAKE_RSDP 0xFFFFE000ull   /* not a real address: the rsdp is a local copy (satoru) */

int kld_acpi_broker_init(void)
{
    for (unsigned off = 0; off <= 32; off += 8) {
        long v = kb(OP_RSDP_READ, off, 0, 0, 0);
        if (v < 0 && off == 0) {
            fprintf(stderr, "[kld-acpi] broker refused RSDP_READ (%ld) - not the privileged container?\n", v);
            return -1;
        }
        memcpy(kld_rsdp_copy + off, &v, 8);
    }
    if (memcmp(kld_rsdp_copy, "RSD PTR ", 8)) {
        fprintf(stderr, "[kld-acpi] no RSDP from the bootloader\n");
        return -1;
    }
    kld_phys_add(KLD_FAKE_RSDP, kld_rsdp_copy, 36);
    kld_osl_rsdp = KLD_FAKE_RSDP;
    fprintf(stderr, "[kld-acpi] RSDP from the broker: rev %u\n", kld_rsdp_copy[15]);
    return 0;
}

struct kld_win { unsigned long long pa; unsigned long len; unsigned char *va; };
static struct kld_win kld_wins[512];
static int kld_nwins;

void *kld_broker_map_phys(acpi_physical_address addr, acpi_size len)
{
    if (!len) len = 1;
    for (int i = 0; i < kld_nwins; i++)
        if (addr >= kld_wins[i].pa && addr + len <= kld_wins[i].pa + kld_wins[i].len)
            return kld_wins[i].va + (addr - kld_wins[i].pa);
    if (kld_nwins >= 512) return NULL;
    unsigned long long pb = addr & ~0xFFFull;
    unsigned long pl = (unsigned long)(((addr + len + 0xFFF) & ~0xFFFull) - pb);
    void *va = mmap(NULL, pl, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (va == MAP_FAILED) return NULL;
    long r = kb(OP_MAP_PHYS, pb, pl, (unsigned long)va, 0);
    if (r < 0) {
        munmap(va, pl);
        fprintf(stderr, "[kld-acpi] MAP_PHYS 0x%llx+0x%lx refused (%ld)\n", pb, pl, r);
        return NULL;
    }
    kld_wins[kld_nwins].pa = pb;
    kld_wins[kld_nwins].len = pl;
    kld_wins[kld_nwins].va = va;
    kld_nwins++;
    return (unsigned char *)va + (addr - pb);
}

long kld_broker_port_read(unsigned long port, unsigned width) { return kb(OP_PORT_IN, port, width, 0, 0); }
long kld_broker_port_write(unsigned long port, unsigned width, unsigned long val) { return kb(OP_PORT_OUT, port, width, val, 0); }

long kld_broker_pci_read(unsigned seg, unsigned bus, unsigned dev, unsigned fn, unsigned reg, unsigned width)
{
    if (seg) return -1;
    return kb(OP_PCI_READ, (bus << 16) | (dev << 8) | fn, reg, width / 8, 0);
}

long kld_broker_pci_write(unsigned seg, unsigned bus, unsigned dev, unsigned fn, unsigned reg, unsigned width,
                          unsigned long val)
{
    if (seg) return -1;
    return kb(OP_PCI_WRITE, (bus << 16) | (dev << 8) | fn, reg, width / 8, val);
}

// end (satoru)
