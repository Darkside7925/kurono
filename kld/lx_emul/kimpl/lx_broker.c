// lx_emul: the container side of the KLD kernel broker.
//
// An untrusted container cannot touch hardware. Every privileged device
// operation goes through SYS_KLD_CALL, which the Kurono kernel implements in
// src/kernel/kld_broker.cpp.
//
// The ABI is SCALARS ONLY, by design on the kernel side: no user pointer crosses
// the boundary, because Kurono's syscall layer has no copy_from_user/access_ok
// yet and a pointer-taking broker op would be a privilege-escalation surface.
// The cost is one syscall per id_table entry during enumeration; the benefit is a
// boundary that cannot be abused with a crafted pointer.
//
// end (satoru)

#include <linux/kernel.h>

#include "lx_kshim.h"
#include "lx_broker.h"

// raw syscall: musl's syscall() wrapper is not reachable from kernel-context
// code (header collision), so issue it directly. (satoru)
long lx_broker_call(unsigned int op, unsigned long a0, unsigned long a1,
                    unsigned long a2, unsigned long a3)
{
    long ret;
    register unsigned long r10 __asm__("r10") = a3;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(LX_SYS_KLD_CALL),
                           "D"((unsigned long)op),
                           "S"(a0),
                           "d"(a1),
                           "r"(r10)
                         : "rcx", "r11", "memory");
    (void)a2;
    return ret;
}

// NOTE on the register mapping: the x86_64 syscall ABI passes args in
// rdi, rsi, rdx, r10, r8, r9. Kurono's SYS_KLD_CALL case reads them as
// (op, a0, a1, a2, a3) = (rdi, rsi, rdx, r10, r8), so the 4-arg ops used here
// map to rdi/rsi/rdx/r10. A 5th argument would need r8 and is not used yet.
// (satoru)

int lx_broker_available(void)
{
    // STATUS on an ungranted pid returns a grant count (0), never an error, so a
    // non-negative result proves the broker is present and reachable. (satoru)
    return lx_broker_call(LX_KLD_OP_STATUS, 0, 0, 0, 0) >= 0;
}

long lx_broker_pci_find(unsigned int vendor, unsigned int device, unsigned int skip)
{
    return lx_broker_call(LX_KLD_OP_PCI_FIND, vendor, device, skip, 0);
}

long lx_broker_cfg_read(unsigned int bdf, unsigned int off, unsigned int width)
{
    return lx_broker_call(LX_KLD_OP_PCI_CFG_READ, bdf, off, width, 0);
}

long lx_broker_cfg_write(unsigned int bdf, unsigned int off, unsigned int width,
                         unsigned int val)
{
    return lx_broker_call(LX_KLD_OP_PCI_CFG_WRITE, bdf, off, width, val);
}

long lx_broker_bar(unsigned int bdf, unsigned int idx)
{
    return lx_broker_call(LX_KLD_OP_PCI_BAR, bdf, idx, 0, 0);
}

long lx_broker_enable(unsigned int bdf)
{
    return lx_broker_call(LX_KLD_OP_PCI_ENABLE, bdf, 0, 0, 0);
}

// end (satoru)
