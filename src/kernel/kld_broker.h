#pragma once
#include "types.h"

//  KLD broker - the kernel half of the Linux-driver compatibility layer.
//  See scratchpad/agent_kld_design.md and feats/kld-linux-driver-compat/readme.md.
//
//  A KLD driver container is an UNTRUSTED ring-3 process running unmodified
//  upstream Linux driver code. It cannot touch hardware directly; every device
//  access goes through this broker, which is the only place kernel privilege is
//  applied. The Kurono kernel links no Linux code - that invariant is the point
//  of the whole design, and this file is the boundary that preserves it.
//
//  ABI DESIGN NOTE: every op takes and returns SCALARS ONLY. No user pointer is
//  ever dereferenced here. Data moves through broker-OWNED frames that the
//  broker maps into the container and reads through the kernel identity map, so
//  the boundary does not depend on the uaccess sweep or on SMAP. A user VA
//  argument is only ever a PLACE to install a mapping, validated to be page
//  aligned, in the user half, inside the container's address space and
//  currently unmapped (the container reserves it with an anonymous mmap first).
//
//  GRANT MODEL: a container may only touch a device it has been granted, and the
//  only way to get a grant is a successful PCI_FIND. Grants are keyed by the
//  container's ADDRESS SPACE, so every thread of the container (irq thread,
//  napi thread, workqueue workers) shares them. Config writes that would move a
//  BAR or reprogram MSI/MSI-X are refused: interrupt routing and decode belong to
//  the broker. Honest limit: without an IOMMU a granted device can still DMA
//  anywhere, so this is FAULT isolation plus config containment, not security
//  isolation. (satoru)

struct Process;

namespace KldBroker {

// syscall number, dispatched from the x64 gate next to SYS_UDF_CALL. "KLDB".
constexpr uint64_t SYS_KLD_CALL = 0x4B4C4442ull;

// the idt vectors the broker owns for MSI delivery. installed by HAL::InitIDT
// (isr stubs 80..87) and dispatched from isr_common_handler. (satoru)
constexpr uint32_t MSI_VECTOR_BASE  = 0x50;
constexpr uint32_t MSI_VECTOR_COUNT = 8;

enum KldOp : uint32_t {
    // ── enumeration + grant ────────────────────────────────────────────────
    // a0=vendor, a1=device, a2=skip. returns a packed bdf, or negative.
    // a successful find GRANTS the caller access to that bdf. (satoru)
    KLD_OP_PCI_FIND      = 0x0001,

    // ── config space (granted bdf only) ────────────────────────────────────
    // a0=bdf, a1=offset (0..0xFF), a2=width(1|2|4). returns the value. (satoru)
    KLD_OP_PCI_CFG_READ  = 0x0002,
    // a0=bdf, a1=offset, a2=width, a3=value. BAR + MSI/MSI-X bodies refused. (satoru)
    KLD_OP_PCI_CFG_WRITE = 0x0003,

    // a0=bdf, a1=bar index (0..5). returns the raw BAR dword. (satoru)
    KLD_OP_PCI_BAR       = 0x0004,

    // a0=bdf. enables memory space + bus mastering. (satoru)
    KLD_OP_PCI_ENABLE    = 0x0005,

    // a0=bdf, a1=bar index. returns the decoded size in bytes (0 = unused). (satoru)
    KLD_OP_PCI_BAR_SIZE  = 0x0006,

    // a0=bdf. bus mastering OFF (pci_clear_master / teardown). (satoru)
    KLD_OP_PCI_DISABLE   = 0x0007,

    // ── memory ─────────────────────────────────────────────────────────────
    // a0=bdf, a1=bar, a2=user va (page aligned, reserved + unmapped), a3=bytes.
    // maps the bar uncached + nx. returns 0. (satoru)
    KLD_OP_MAP_BAR       = 0x0020,
    // a0=user va, a1=pages, a2=max physical address bits (32..52). allocates
    // zeroed contiguous frames below that limit, maps them at va (wb). returns
    // the bus address (== physical without an iommu). (satoru)
    KLD_OP_DMA_ALLOC     = 0x0021,
    // a0=user va of a previous DMA_ALLOC. unmaps + frees. (satoru)
    KLD_OP_DMA_FREE      = 0x0022,

    // ── interrupts ─────────────────────────────────────────────────────────
    // a0=user va (one page, reserved + unmapped). maps the doorbell page: u32
    // counter per irq handle at offset 4*handle, bumped by the isr. (satoru)
    KLD_OP_IRQ_PAGE      = 0x0030,
    // a0=bdf. allocates a vector, programs the device's MSI capability and
    // enables it. returns the handle (0..7). (satoru)
    KLD_OP_MSI_ALLOC     = 0x0031,
    // a0=handle. disables MSI on the device and frees the vector. (satoru)
    KLD_OP_MSI_FREE      = 0x0032,
    // a0=bdf, a1=vectors wanted. programs that many MSI-X table entries (one
    // broker vector each, consecutive handles) through the KERNEL's mapping of
    // the table bar and enables MSI-X. returns the first handle; the count is
    // min(wanted, table size, free vectors) and is reported by MSIX_COUNT. (satoru)
    KLD_OP_MSIX_ALLOC    = 0x0033,
    // a0=bdf. masks + disables MSI-X and frees every vector of that device. (satoru)
    KLD_OP_MSIX_FREE     = 0x0034,
    // a0=bdf. how many MSI-X vectors the last MSIX_ALLOC granted. (satoru)
    KLD_OP_MSIX_COUNT    = 0x0035,

    // ── class bridge: network ──────────────────────────────────────────────
    // a0=user va (KLD_NET_PAGES pages, reserved + unmapped), a1=mac bytes 0..3,
    // a2=mac bytes 4..5. maps the shared packet rings and makes the container
    // the backend of Kurono's eth0. (satoru)
    KLD_OP_NET_ATTACH    = 0x0040,
    // a0=1 carrier on, 0 off. (satoru)
    KLD_OP_NET_CARRIER   = 0x0041,
    // frames are in the rx ring: drain them into the tcp/ip stack now. (satoru)
    KLD_OP_NET_RX_KICK   = 0x0042,
    // run one iteration of the network loop body (tcp timers + rx drain, link
    // refresh, dhcp). see KldNet::Tick for why the container drives it. (satoru)
    KLD_OP_NET_TICK      = 0x0043,

    // ── ACPI container (privileged: only the address space the kernel's own
    // kld-acpi runner registered via GrantAcpi) ─────────────────────────────
    // a0=byte offset (0..35, step 8). 8 bytes of the bootloader's RSDP. (satoru)
    KLD_OP_ACPI_RSDP_READ = 0x0050,
    // a0=phys, a1=bytes, a2=user va. maps physical memory that is NOT usable ram
    // per the firmware memory map (acpi tables, nvs, mmio). (satoru)
    KLD_OP_ACPI_MAP_PHYS  = 0x0051,
    // a0=port, a1=width(8|16|32). ports the kernel drives itself are refused. (satoru)
    KLD_OP_ACPI_PORT_IN   = 0x0052,
    // a0=port, a1=width, a2=value. (satoru)
    KLD_OP_ACPI_PORT_OUT  = 0x0053,
    // a0=bdf, a1=offset, a2=width(1|2|4): config read of ANY device (acpi
    // PCI_Config opregions name devices the container was never granted). (satoru)
    KLD_OP_ACPI_PCI_READ  = 0x0054,
    // a0=bdf, a1=offset, a2=width, a3=value. bars and msi bodies still refused. (satoru)
    KLD_OP_ACPI_PCI_WRITE = 0x0055,

    // how many grants this container holds / liveness probe. (satoru)
    KLD_OP_STATUS        = 0x0010,
    // explicit clean teardown of everything this container holds. (satoru)
    KLD_OP_RELEASE       = 0x0011
};

enum KldError : int64_t {
    KLD_OK       =  0,
    KLD_EPERM    = -1,    // bdf not granted to this container (satoru)
    KLD_EINVAL   = -2,    // bad op / width / bar index / va (satoru)
    KLD_ENOMEM   = -12,   // no frames under the requested limit (satoru)
    KLD_EBUSY    = -16,   // already attached / va already mapped (satoru)
    KLD_ENODEV   = -19,   // no matching device / no capability (satoru)
    KLD_ENOSPC   = -28    // table full (satoru)
};

// pack/unpack a bus:dev.func triple into one scalar. (satoru)
static inline uint32_t PackBdf(uint8_t bus, uint8_t dev, uint8_t func) {
    return ((uint32_t)bus << 16) | ((uint32_t)dev << 8) | (uint32_t)func;
}

void Init();

// copy the firmware memory map (multiboot1-format mmap, which the multiboot2
// path also produces) so MAP_PHYS can refuse usable ram. call early, while the
// map is intact. (satoru)
void SnapshotMemoryMap(const void* mbi);

// the kernel's kld-acpi runner marks the one address space allowed to use the
// ACPI ops. (satoru)
void GrantAcpi(uint64_t address_space);

// the SYS_KLD_CALL entry point, called with interrupts enabled. `cur` is the
// calling task (any thread of the container). (satoru)
int64_t Call(Process* cur, uint32_t op, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);

// tear down everything a container held: bus mastering off FIRST, then msi,
// then dma frames, then grants. called by the runner after the container exits
// (keyed by its address space, which is still a valid number even after the
// space itself is gone). safe to call twice. (satoru)
void ReleaseAddressSpace(uint64_t address_space);

// kurono.kld.irqwake=0: isr only bumps the counter, the futex repoll wakes. (satoru)
void SetIrqWake(bool on);

// isr entry for vectors MSI_VECTOR_BASE.. (interrupts off, any cpu). (satoru)
void OnVector(uint32_t vector);

// shell/diagnostics. (satoru)
int Status(char* out, int mx);

}  // namespace KldBroker

// ── KLD network backend behind the E1000:: facade ───────────────────────────
// the container's net_device becomes Kurono's eth0. shared ring layout is the
// wire format with kld/lx_emul/kimpl/lx_net.c and must stay in lockstep. (satoru)
namespace KldNet {

constexpr uint32_t KLD_NET_MAGIC      = 0x4B4C4E31u;  // "KLN1" (satoru)
constexpr uint32_t KLD_NET_TX_SLOTS   = 64;
constexpr uint32_t KLD_NET_RX_SLOTS   = 128;
constexpr uint32_t KLD_NET_SLOT_BYTES = 2048;
// 1 header page + (64 + 128) slots of 2 KiB = 1 + 96 pages. (satoru)
constexpr uint32_t KLD_NET_PAGES      = 1 + (KLD_NET_TX_SLOTS + KLD_NET_RX_SLOTS) * KLD_NET_SLOT_BYTES / 4096;

struct RingHeader {
    uint32_t magic;
    uint32_t tx_slots, rx_slots, slot_bytes;
    volatile uint32_t tx_prod;      // kernel writes (satoru)
    volatile uint32_t tx_cons;      // container writes (satoru)
    volatile uint32_t rx_prod;      // container writes (satoru)
    volatile uint32_t rx_cons;      // kernel writes (satoru)
    volatile uint32_t tx_doorbell;  // futex word the container's xmit thread waits on (satoru)
    volatile uint32_t rx_drops;     // container: ring full (satoru)
    volatile uint32_t tx_drops;     // kernel: ring full (satoru)
    uint32_t pad0;
    volatile uint16_t tx_len[KLD_NET_TX_SLOTS];
    volatile uint16_t rx_len[KLD_NET_RX_SLOTS];
};

// set from the boot cmdline before Network::Init: eth0 is served by the KLD
// container instead of the native e1000/igc driver. (satoru)
void SetWanted(bool on);
bool Wanted();

bool Attached();
bool LinkUp();
void GetMAC(uint8_t out[6]);
bool Send(const uint8_t* data, uint16_t len);
void Poll();
void SetPacketHandler(void (*h)(const uint8_t*, uint16_t));
uint32_t TxCount();
uint32_t RxCount();
uint32_t TxBytes();
uint32_t RxBytes();
uint32_t RxDrops();

// broker side. (satoru)
int64_t Attach(uint64_t address_space, uint64_t ring_phys, const uint8_t mac[6]);
void    Detach(uint64_t address_space);
int64_t SetCarrier(uint64_t address_space, bool on);
int64_t RxKick(uint64_t address_space);
int64_t Tick(uint64_t address_space);

}  // namespace KldNet

// end (satoru)
