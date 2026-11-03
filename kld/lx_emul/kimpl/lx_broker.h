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
#define LX_KLD_OP_STATUS        0x0010u

long lx_broker_call(unsigned int op, unsigned long a0, unsigned long a1,
                    unsigned long a2, unsigned long a3);
int  lx_broker_available(void);
long lx_broker_pci_find(unsigned int vendor, unsigned int device, unsigned int skip);
long lx_broker_cfg_read(unsigned int bdf, unsigned int off, unsigned int width);
long lx_broker_cfg_write(unsigned int bdf, unsigned int off, unsigned int width,
                         unsigned int val);
long lx_broker_bar(unsigned int bdf, unsigned int idx);
long lx_broker_enable(unsigned int bdf);

#endif
// end (satoru)
