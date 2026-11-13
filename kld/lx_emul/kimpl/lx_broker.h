// KLD broker ABI, container side. MUST stay in lockstep with
// src/kernel/kld_broker.h - these constants are the wire format.
// end (satoru)

#ifndef KLD_LX_BROKER_H
#define KLD_LX_BROKER_H

#define LX_SYS_KLD_CALL        0x4B4C4442ul   /* "KLDB" */

#define LX_KLD_OP_PCI_FIND      0x0001u
#define LX_KLD_OP_PCI_CFG_READ  0x0002u
#define LX_KLD_OP_PCI_CFG_WRITE 0x0003u
#define LX_KLD_OP_PCI_BAR       0x0004u
#define LX_KLD_OP_PCI_ENABLE    0x0005u
#define LX_KLD_OP_PCI_BAR_SIZE  0x0006u
#define LX_KLD_OP_PCI_DISABLE   0x0007u
#define LX_KLD_OP_STATUS        0x0010u
#define LX_KLD_OP_RELEASE       0x0011u
#define LX_KLD_OP_MAP_BAR       0x0020u
#define LX_KLD_OP_DMA_ALLOC     0x0021u
#define LX_KLD_OP_DMA_FREE      0x0022u
#define LX_KLD_OP_DMA_ALIAS     0x0023u
#define LX_KLD_OP_DMA_UNALIAS   0x0024u
#define LX_KLD_OP_PORT_IN       0x0025u
#define LX_KLD_OP_PORT_OUT      0x0026u
#define LX_KLD_OP_OPREGION_MAP  0x0027u
#define LX_KLD_OP_HOST_CFG_READ 0x0028u
#define LX_KLD_OP_GT_WRITE      0x0029u
#define LX_KLD_OP_DISPLAY_ATTACH 0x0080u
#define LX_KLD_OP_DISPLAY_MODES  0x0081u
#define LX_KLD_OP_DISPLAY_DONE   0x0082u
#define LX_KLD_OP_DISPLAY_HOTPLUG 0x0083u
#define LX_KLD_OP_DISPLAY_ERROR  0x0084u
#define LX_KLD_OP_DISPLAY_DETACH 0x0085u
#define LX_KLD_OP_IRQ_PAGE      0x0030u
#define LX_KLD_OP_MSI_ALLOC     0x0031u
#define LX_KLD_OP_MSI_FREE      0x0032u
#define LX_KLD_OP_MSIX_ALLOC    0x0033u
#define LX_KLD_OP_MSIX_FREE     0x0034u
#define LX_KLD_OP_MSIX_COUNT    0x0035u
#define LX_KLD_OP_IRQ_LINE_ATTACH 0x0036u
#define LX_KLD_OP_IRQ_LINE_UNMASK 0x0037u
#define LX_KLD_OP_IRQ_LINE_DETACH 0x0038u
#define LX_KLD_OP_PLAT_MAP      0x0039u

// line-irq flags, must match src/kernel/kld_ioapic.h. (satoru)
#define LX_LINE_LEVEL      1u
#define LX_LINE_ACTIVE_LOW 2u
#define LX_LINE_SHARED     4u
#define LX_LINE_ISA        8u
#define LX_LINE_DEFAULT    16u
#define LX_KLD_OP_HEARTBEAT     0x003Au
#define LX_KLD_OP_NET_ATTACH    0x0040u
#define LX_KLD_OP_NET_CARRIER   0x0041u
#define LX_KLD_OP_NET_RX_KICK   0x0042u
#define LX_KLD_OP_NET_TICK      0x0043u
#define LX_KLD_OP_TSC_HZ        0x0058u   /* N1b: TSC Hz, same clock as the kernel */
#define LX_KLD_OP_INPUT_POINTER 0x0070u
// the acpi container's table/hardware reads (kld-acpi broker mode). (satoru)
#define LX_KLD_OP_ACPI_RSDP_READ 0x0050u
#define LX_KLD_OP_ACPI_MAP_PHYS  0x0051u
#define LX_KLD_OP_ACPI_PORT_IN   0x0052u
#define LX_KLD_OP_ACPI_PORT_OUT  0x0053u
#define LX_KLD_OP_ACPI_PCI_READ  0x0054u
#define LX_KLD_OP_ACPI_PCI_WRITE 0x0055u
#define LX_KLD_OP_TEST_CANARY   0x0060u   /* kurono.kld.iommutest=1 only (satoru) */

// shared packet ring geometry: must match KldNet:: in kld_broker.h. (satoru)
#define LX_NET_MAGIC      0x4B4C4E31u
#define LX_NET_TX_SLOTS   64u
#define LX_NET_RX_SLOTS   128u
#define LX_NET_SLOT_BYTES 2048u
#define LX_NET_PAGES      (1u + (LX_NET_TX_SLOTS + LX_NET_RX_SLOTS) * LX_NET_SLOT_BYTES / 4096u)

struct lx_net_ring {
    unsigned int magic;
    unsigned int tx_slots, rx_slots, slot_bytes;
    volatile unsigned int tx_prod;
    volatile unsigned int tx_cons;
    volatile unsigned int rx_prod;
    volatile unsigned int rx_cons;
    volatile unsigned int tx_doorbell;
    volatile unsigned int rx_drops;
    volatile unsigned int tx_drops;
    unsigned int pad0;
    volatile unsigned short tx_len[LX_NET_TX_SLOTS];
    volatile unsigned short rx_len[LX_NET_RX_SLOTS];
    volatile unsigned long long rx_tsc[LX_NET_RX_SLOTS];   /* N1b t4 per slot (satoru) */
};

long lx_broker_call(unsigned int op, unsigned long a0, unsigned long a1,
                    unsigned long a2, unsigned long a3);
int  lx_broker_available(void);
long lx_broker_pci_find(unsigned int vendor, unsigned int device, unsigned int skip);
long lx_broker_cfg_read(unsigned int bdf, unsigned int off, unsigned int width);
long lx_broker_cfg_write(unsigned int bdf, unsigned int off, unsigned int width,
                         unsigned int val);
long lx_broker_bar(unsigned int bdf, unsigned int idx);
long lx_broker_bar_size(unsigned int bdf, unsigned int idx);
long lx_broker_enable(unsigned int bdf);
long lx_broker_disable(unsigned int bdf);
long lx_broker_test_canary(void);

#endif
// end (satoru)
