#pragma once
#include "types.h"

//  KLD broker - the kernel half of the Linux-driver compatibility layer.
//  See feats/kld-linux-driver-compat/readme.md.
//
//  A KLD driver container is an UNTRUSTED ring-3 process running unmodified
//  upstream Linux driver code. It cannot touch hardware directly; every device
//  access goes through this broker, which is the only place kernel privilege is
//  applied. The Kurono kernel links no Linux code - that invariant is the point
//  of the whole design, and this file is the boundary that preserves it.
//
//  ABI DESIGN NOTE: every op takes and returns SCALARS ONLY. No user pointers
//  cross this boundary. That is deliberate - the syscall layer currently has no
//  copy_from_user/access_ok (see the audit), so any pointer-taking broker op
//  would be a privilege-escalation surface. A container enumerates by asking
//  "find vendor V device D, skipping N matches" rather than handing over an
//  id-table pointer. It costs a syscall per table entry and buys a boundary that
//  cannot be abused with a crafted pointer.
//
//  GRANT MODEL: a container may only touch a device it has been granted, and the
//  only way to get a grant is a successful PCI_FIND. Config/BAR access to a
//  non-granted BDF is refused. This is a real boundary, but note honestly what it
//  is NOT: without an IOMMU a granted device can still DMA anywhere, so this is
//  FAULT isolation plus config-space containment, not full security isolation.
//  IOMMU enforcement is phase 6. (satoru)

namespace KldBroker {

// syscall number, dispatched from the x64 gate next to SYS_UDF_CALL. "KLDB".
constexpr uint64_t SYS_KLD_CALL = 0x4B4C4442ull;

enum KldOp : uint32_t {
    // ── enumeration + grant ────────────────────────────────────────────────
    // a0=vendor, a1=device, a2=skip. returns a packed bdf, or negative.
    // a successful find GRANTS the caller access to that bdf. (satoru)
    KLD_OP_PCI_FIND      = 0x0001,

    // ── config space (granted bdf only) ────────────────────────────────────
    // a0=bdf, a1=offset, a2=width(1|2|4). returns the value, or negative. (satoru)
    KLD_OP_PCI_CFG_READ  = 0x0002,
    // a0=bdf, a1=offset, a2=width, a3=value. (satoru)
    KLD_OP_PCI_CFG_WRITE = 0x0003,

    // a0=bdf, a1=bar index (0..5). returns the raw BAR dword. (satoru)
    KLD_OP_PCI_BAR       = 0x0004,

    // a0=bdf. enables memory space + bus mastering. (satoru)
    KLD_OP_PCI_ENABLE    = 0x0005,

    // a0=bdf. how many grants this pid holds / liveness probe. (satoru)
    KLD_OP_STATUS        = 0x0010
};

enum KldError : int64_t {
    KLD_OK       =  0,
    KLD_EINVAL   = -2,    // bad op / width / bar index (satoru)
    KLD_ENODEV   = -19,   // no matching device (satoru)
    KLD_EPERM    = -1,    // bdf not granted to this container (satoru)
    KLD_ENOSPC   = -28    // grant table full (satoru)
};

// pack/unpack a bus:dev.func triple into one scalar. (satoru)
static inline uint32_t PackBdf(uint8_t bus, uint8_t dev, uint8_t func) {
    return ((uint32_t)bus << 16) | ((uint32_t)dev << 8) | (uint32_t)func;
}

void Init();

// the SYS_KLD_CALL entry point. `pid` is the calling process so grants are
// per-container. (satoru)
int64_t Call(int pid, uint32_t op, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);

// drop every grant held by a dead container. called from process teardown so a
// crashed driver cannot leave a device claimed. (satoru)
void ReleasePid(int pid);

// shell/diagnostics. (satoru)
int Status(char* out, int mx);

}  // namespace KldBroker

// end (satoru)
