#pragma once
//  kurono os - extended page tables (ept) / nested page tables (npt)
//  memory virtualization for intel vt-x and amd-v
#include "../kernel/types.h"

//  intel ept - 4-level page table (pml4 → pdpt → pd → pt)
//  each entry is 64 bits. we use 4kb pages for fine-grained control.

#define EPT_READ        (1ULL << 0)   // allow read
#define EPT_WRITE       (1ULL << 1)   // allow write
#define EPT_EXECUTE     (1ULL << 2)   // allow execute
#define EPT_MEM_TYPE_MASK (7ULL << 3) // memory type (pat-like)
#define EPT_MT_UC       (0ULL << 3)   // uncacheable
#define EPT_MT_WC       (1ULL << 3)   // write combining
#define EPT_MT_WT       (4ULL << 3)   // write through
#define EPT_MT_WP       (5ULL << 3)   // write protect
#define EPT_MT_WB       (6ULL << 3)   // write back
#define EPT_IGNORE_PAT  (1ULL << 6)   // ignore guest pat
#define EPT_LARGE_PAGE  (1ULL << 7)   // 2mb/1gb page
#define EPT_ACCESSED    (1ULL << 8)   // accessed (if enabled)
#define EPT_DIRTY       (1ULL << 9)   // dirty (if enabled)
#define EPT_EXEC_USER   (1ULL << 10)  // user-mode execute (mbec)

// address mask - bits [51:12] hold the physical page frame number
#define EPT_ADDR_MASK   0x000FFFFFFFFFF000ULL

#define EPTP_MT_WB      (6ULL << 0)   // memory type = wb for ept structure
#define EPTP_WALK_4     (3ULL << 3)   // page-walk length = 4 (pml4)
#define EPTP_AD_ENABLE  (1ULL << 6)   // enable accessed/dirty bits

#define EPT_VIOL_READ   (1 << 0)
#define EPT_VIOL_WRITE  (1 << 1)
#define EPT_VIOL_EXEC   (1 << 2)
#define EPT_VIOL_READABLE (1 << 3)
#define EPT_VIOL_WRITABLE (1 << 4)
#define EPT_VIOL_EXECUTABLE (1 << 5)

//  amd npt - nested page tables (same 4-level structure as normal x86 pt)
//  uses standard pte format bits with some additions

#define NPT_PRESENT     (1ULL << 0)
#define NPT_WRITE       (1ULL << 1)
#define NPT_USER        (1ULL << 2)
#define NPT_PWT         (1ULL << 3)   // page-level write-through
#define NPT_PCD         (1ULL << 4)   // page-level cache disable
#define NPT_ACCESSED    (1ULL << 5)
#define NPT_DIRTY       (1ULL << 6)
#define NPT_LARGE_PAGE  (1ULL << 7)   // 2mb/1gb page
#define NPT_NX          (1ULL << 63)  // no execute
#define NPT_ADDR_MASK   0x000FFFFFFFFFF000ULL

//  page table structures - 512 entries per level

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

// same structures used for amd npt (just standard page tables)
typedef EPT_PML4 NPT_PML4;
typedef EPT_PDPT NPT_PDPT;
typedef EPT_PD   NPT_PD;
typedef EPT_PT   NPT_PT;

//  guest physical memory region descriptor

enum MemRegionType {
    MEM_RAM = 0,        // normal ram
    MEM_ROM,            // read-only (bios)
    MEM_MMIO,           // memory-mapped i/o
    MEM_RESERVED,       // not accessible
    MEM_FRAMEBUFFER     // video framebuffer
};

struct GuestMemRegion {
    uint64_t guest_phys_start;  // guest physical address
    uint64_t host_phys_start;   // host physical address backing
    uint64_t size;              // region size in bytes
    MemRegionType type;
    bool     read;
    bool     write;
    bool     execute;
};

#define MAX_MEM_REGIONS 32

//  ept manager - handles ept/npt page table creation and manipulation

class EPTManager {
public:
    static void Init();

    static EPT_PML4* CreateEPT();
    static void      DestroyEPT(EPT_PML4* pml4);
    static uint64_t  BuildEPTP(EPT_PML4* pml4);  // build eptp value for vmcs

    static NPT_PML4* CreateNPT();
    static void       DestroyNPT(NPT_PML4* pml4);
    static uint64_t   BuildNCR3(NPT_PML4* pml4);  // build ncr3 for vmcb

    static bool MapGuestPhysical(EPT_PML4* pml4, uint64_t guest_phys,
                                  uint64_t host_phys, uint64_t size,
                                  uint64_t flags);

    static bool MapGuestPhysicalNPT(NPT_PML4* pml4, uint64_t guest_phys,
                                     uint64_t host_phys, uint64_t size,
                                     uint64_t flags);

    static bool MapRAM(EPT_PML4* pml4, uint64_t guest_phys,
                        uint64_t host_phys, uint64_t size);
    static bool MapROM(EPT_PML4* pml4, uint64_t guest_phys,
                        uint64_t host_phys, uint64_t size);
    static bool MapMMIO(EPT_PML4* pml4, uint64_t guest_phys,
                         uint64_t host_phys, uint64_t size);

    static bool MapLargePages(EPT_PML4* pml4, uint64_t guest_phys,
                               uint64_t host_phys, uint64_t size,
                               uint64_t flags);
    static bool MapLargePagesNPT(NPT_PML4* pml4, uint64_t guest_phys,
                                  uint64_t host_phys, uint64_t size,
                                  uint64_t flags);
    
    static bool MapPCIBar(EPT_PML4* pml4, uint64_t bar_phys, uint64_t size);
    static bool MapPCIBarNPT(NPT_PML4* pml4, uint64_t bar_phys, uint64_t size);

    static void AddRegion(const GuestMemRegion& region);
    static int  GetRegionCount();
    static const GuestMemRegion* GetRegion(int idx);

    static void InvalidateEPT();   // invept
    static void InvalidateVPID();   // invvpid

    static bool HandleEPTViolation(uint64_t guest_phys, uint64_t qualification);
    static bool HandleNPF(uint64_t guest_phys, uint64_t error_code);  // nested page fault

    static void DumpEPT(EPT_PML4* pml4, int max_entries);
    static void DumpRegions();

    // kmemx hook: return a pointer to the existing 4kb LEAF ept entry for
    // `guest_phys` (no create), or nullptr if the upper levels are absent or the
    // mapping is served by a large page (kmemx only compresses 4kb leaves). lets
    // the memory-compression engine read/clear/restore a single guest page's ept
    // entry at the hypervisor level without exposing the private walker. (satoru)
    static uint64_t* KmemxLeafEntry(EPT_PML4* pml4, uint64_t guest_phys);

private:
    static GuestMemRegion regions[MAX_MEM_REGIONS];
    static int region_count;
    static bool initialized;

    // internal page table walk helpers
    static uint64_t* WalkEPT(EPT_PML4* pml4, uint64_t guest_phys, bool create);
    static uint64_t* WalkNPT(NPT_PML4* pml4, uint64_t guest_phys, bool create);
    static void*     AllocPage();  // 4kb-aligned zero-filled page
};
