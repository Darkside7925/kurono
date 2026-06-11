#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Extended Page Tables (EPT) / Nested Page Tables (NPT)
//  Memory virtualization for Intel VT-x and AMD-V
// ═══════════════════════════════════════════════════════════════════════════
#include "../kernel/types.h"

// ═══════════════════════════════════════════════════════════════════════════
//  Intel EPT — 4-level page table (PML4 → PDPT → PD → PT)
//  Each entry is 64 bits. We use 4KB pages for fine-grained control.
// ═══════════════════════════════════════════════════════════════════════════

// ── EPT Entry Bits ──
#define EPT_READ        (1ULL << 0)   // Allow read
#define EPT_WRITE       (1ULL << 1)   // Allow write
#define EPT_EXECUTE     (1ULL << 2)   // Allow execute
#define EPT_MEM_TYPE_MASK (7ULL << 3) // Memory type (PAT-like)
#define EPT_MT_UC       (0ULL << 3)   // Uncacheable
#define EPT_MT_WC       (1ULL << 3)   // Write Combining
#define EPT_MT_WT       (4ULL << 3)   // Write Through
#define EPT_MT_WP       (5ULL << 3)   // Write Protect
#define EPT_MT_WB       (6ULL << 3)   // Write Back
#define EPT_IGNORE_PAT  (1ULL << 6)   // Ignore guest PAT
#define EPT_LARGE_PAGE  (1ULL << 7)   // 2MB/1GB page
#define EPT_ACCESSED    (1ULL << 8)   // Accessed (if enabled)
#define EPT_DIRTY       (1ULL << 9)   // Dirty (if enabled)
#define EPT_EXEC_USER   (1ULL << 10)  // User-mode execute (MBEC)

// Address mask — bits [51:12] hold the physical page frame number
#define EPT_ADDR_MASK   0x000FFFFFFFFFF000ULL

// ── EPT Pointer (EPTP) bits ──
#define EPTP_MT_WB      (6ULL << 0)   // Memory type = WB for EPT structure
#define EPTP_WALK_4     (3ULL << 3)   // Page-walk length = 4 (PML4)
#define EPTP_AD_ENABLE  (1ULL << 6)   // Enable accessed/dirty bits

// ── EPT Violation Qualification Bits ──
#define EPT_VIOL_READ   (1 << 0)
#define EPT_VIOL_WRITE  (1 << 1)
#define EPT_VIOL_EXEC   (1 << 2)
#define EPT_VIOL_READABLE (1 << 3)
#define EPT_VIOL_WRITABLE (1 << 4)
#define EPT_VIOL_EXECUTABLE (1 << 5)

// ═══════════════════════════════════════════════════════════════════════════
//  AMD NPT — Nested Page Tables (same 4-level structure as normal x86 PT)
//  Uses standard PTE format bits with some additions
// ═══════════════════════════════════════════════════════════════════════════

#define NPT_PRESENT     (1ULL << 0)
#define NPT_WRITE       (1ULL << 1)
#define NPT_USER        (1ULL << 2)
#define NPT_PWT         (1ULL << 3)   // Page-level Write-Through
#define NPT_PCD         (1ULL << 4)   // Page-level Cache Disable
#define NPT_ACCESSED    (1ULL << 5)
#define NPT_DIRTY       (1ULL << 6)
#define NPT_LARGE_PAGE  (1ULL << 7)   // 2MB/1GB page
#define NPT_NX          (1ULL << 63)  // No Execute
#define NPT_ADDR_MASK   0x000FFFFFFFFFF000ULL

// ═══════════════════════════════════════════════════════════════════════════
//  Page Table structures — 512 entries per level
// ═══════════════════════════════════════════════════════════════════════════

struct alignas(4096) EPT_PML4 {
    uint64_t entries[512];
};

struct alignas(4096) EPT_PDPT {
    uint64_t entries[512];
};

struct alignas(4096) EPT_PD {
    uint64_t entries[512];
};

struct alignas(4096) EPT_PT {
    uint64_t entries[512];
};

// Same structures used for AMD NPT (just standard page tables)
typedef EPT_PML4 NPT_PML4;
typedef EPT_PDPT NPT_PDPT;
typedef EPT_PD   NPT_PD;
typedef EPT_PT   NPT_PT;

// ═══════════════════════════════════════════════════════════════════════════
//  Guest Physical Memory Region descriptor
// ═══════════════════════════════════════════════════════════════════════════

enum MemRegionType {
    MEM_RAM = 0,        // Normal RAM
    MEM_ROM,            // Read-only (BIOS)
    MEM_MMIO,           // Memory-Mapped I/O
    MEM_RESERVED,       // Not accessible
    MEM_FRAMEBUFFER     // Video framebuffer
};

struct GuestMemRegion {
    uint64_t guest_phys_start;  // Guest physical address
    uint64_t host_phys_start;   // Host physical address backing
    uint64_t size;              // Region size in bytes
    MemRegionType type;
    bool     read;
    bool     write;
    bool     execute;
};

#define MAX_MEM_REGIONS 32

// ═══════════════════════════════════════════════════════════════════════════
//  EPT Manager — handles EPT/NPT page table creation and manipulation
// ═══════════════════════════════════════════════════════════════════════════

class EPTManager {
public:
    // ── Initialization ──
    static void Init();

    // ── EPT table management (Intel) ──
    static EPT_PML4* CreateEPT();
    static void      DestroyEPT(EPT_PML4* pml4);
    static uint64_t  BuildEPTP(EPT_PML4* pml4);  // Build EPTP value for VMCS

    // ── NPT table management (AMD) ──
    static NPT_PML4* CreateNPT();
    static void       DestroyNPT(NPT_PML4* pml4);
    static uint64_t   BuildNCR3(NPT_PML4* pml4);  // Build nCR3 for VMCB

    // ── Memory mapping ──
    static bool MapGuestPhysical(EPT_PML4* pml4, uint64_t guest_phys,
                                  uint64_t host_phys, uint64_t size,
                                  uint64_t flags);

    static bool MapGuestPhysicalNPT(NPT_PML4* pml4, uint64_t guest_phys,
                                     uint64_t host_phys, uint64_t size,
                                     uint64_t flags);

    // ── Convenience mappings ──
    static bool MapRAM(EPT_PML4* pml4, uint64_t guest_phys,
                        uint64_t host_phys, uint64_t size);
    static bool MapROM(EPT_PML4* pml4, uint64_t guest_phys,
                        uint64_t host_phys, uint64_t size);
    static bool MapMMIO(EPT_PML4* pml4, uint64_t guest_phys,
                         uint64_t host_phys, uint64_t size);

    // ── 2MB large page mapping (for GPU BARs, large RAM regions) ──
    static bool MapLargePages(EPT_PML4* pml4, uint64_t guest_phys,
                               uint64_t host_phys, uint64_t size,
                               uint64_t flags);
    static bool MapLargePagesNPT(NPT_PML4* pml4, uint64_t guest_phys,
                                  uint64_t host_phys, uint64_t size,
                                  uint64_t flags);
    
    // ── PCI BAR passthrough (map device MMIO BARs into guest) ──
    static bool MapPCIBar(EPT_PML4* pml4, uint64_t bar_phys, uint64_t size);
    static bool MapPCIBarNPT(NPT_PML4* pml4, uint64_t bar_phys, uint64_t size);

    // ── Memory region tracking ──
    static void AddRegion(const GuestMemRegion& region);
    static int  GetRegionCount();
    static const GuestMemRegion* GetRegion(int idx);

    // ── TLB management ──
    static void InvalidateEPT();   // INVEPT
    static void InvalidateVPID();   // INVVPID

    // ── EPT violation handling ──
    static bool HandleEPTViolation(uint64_t guest_phys, uint64_t qualification);
    static bool HandleNPF(uint64_t guest_phys, uint64_t error_code);  // Nested Page Fault

    // ── Debug ──
    static void DumpEPT(EPT_PML4* pml4, int max_entries);
    static void DumpRegions();

private:
    static GuestMemRegion regions[MAX_MEM_REGIONS];
    static int region_count;
    static bool initialized;

    // Internal page table walk helpers
    static uint64_t* WalkEPT(EPT_PML4* pml4, uint64_t guest_phys, bool create);
    static uint64_t* WalkNPT(NPT_PML4* pml4, uint64_t guest_phys, bool create);
    static void*     AllocPage();  // 4KB-aligned zero-filled page
};
