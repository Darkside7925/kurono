//  kurono os - ept / npt implementation
//  extended page tables (intel) & nested page tables (amd)
#include "ept.h"
#include "vmm.h"
#include "../drivers/serial.h"
#include "../kernel/heap.h"
#include "../kernel/kmemx.h"   // guest-page decompression on ept violation (satoru)

// helper: allocate page-aligned memory
static void* HeapAllocAligned(size_t size, size_t align) {
    void* raw = KernelHeap::Alloc(size + align + sizeof(void*));
    if (!raw) return nullptr;
    uintptr_t addr = (uintptr_t)raw + sizeof(void*);
    uintptr_t aligned = (addr + align - 1) & ~(align - 1);
    ((void**)aligned)[-1] = raw;
    return (void*)aligned;
}

static void HeapFreeAligned(void* aligned) {
    if (!aligned) return;
    void* raw = ((void**)aligned)[-1];
    KernelHeap::Free(raw);
}

GuestMemRegion EPTManager::regions[MAX_MEM_REGIONS];
int  EPTManager::region_count = 0;
bool EPTManager::initialized  = false;

//  helpers

void* EPTManager::AllocPage() {
    void* p = HeapAllocAligned(4096, 4096);
    if (p) {
        uint8_t* b = (uint8_t*)p;
        for (int i = 0; i < 4096; i++) b[i] = 0;
    }
    return p;
}

//  init

void EPTManager::Init() {
    if (initialized) return;
    initialized = true;
    region_count = 0;
    SerialLogger::Log("EPT: Manager initialized\r\n");
}

//  ept creation / destruction (intel)

EPT_PML4* EPTManager::CreateEPT() {
    EPT_PML4* pml4 = (EPT_PML4*)AllocPage();
    if (!pml4) {
        SerialLogger::Log("EPT: Failed to allocate PML4\r\n");
        return nullptr;
    }
    SerialLogger::Log("EPT: Created EPT PML4 at ");
    SerialLogger::LogHex((uint32_t)(uintptr_t)pml4);
    SerialLogger::Log("\r\n");
    return pml4;
}

void EPTManager::DestroyEPT(EPT_PML4* pml4) {
    if (!pml4) return;

    // walk and free all sub-tables
    for (int i = 0; i < 512; i++) {
        if (!(pml4->entries[i] & (EPT_READ | EPT_WRITE | EPT_EXECUTE))) continue;
        EPT_PDPT* pdpt = (EPT_PDPT*)(uintptr_t)(pml4->entries[i] & EPT_ADDR_MASK);

        for (int j = 0; j < 512; j++) {
            if (!(pdpt->entries[j] & (EPT_READ | EPT_WRITE | EPT_EXECUTE))) continue;
            if (pdpt->entries[j] & EPT_LARGE_PAGE) continue; // 1gb page
            EPT_PD* pd = (EPT_PD*)(uintptr_t)(pdpt->entries[j] & EPT_ADDR_MASK);

            for (int k = 0; k < 512; k++) {
                if (!(pd->entries[k] & (EPT_READ | EPT_WRITE | EPT_EXECUTE))) continue;
                if (pd->entries[k] & EPT_LARGE_PAGE) continue; // 2mb page
                EPT_PT* pt = (EPT_PT*)(uintptr_t)(pd->entries[k] & EPT_ADDR_MASK);
                HeapFreeAligned(pt);
            }
            HeapFreeAligned(pd);
        }
        HeapFreeAligned(pdpt);
    }
    HeapFreeAligned(pml4);
}

uint64_t EPTManager::BuildEPTP(EPT_PML4* pml4) {
    uint64_t eptp = (uint64_t)(uintptr_t)pml4;
    eptp |= EPTP_MT_WB;     // wb memory type for ept structures
    eptp |= EPTP_WALK_4;    // 4-level page walk
    // eptp |= eptp_ad_enable; // enable a/d bits if supported
    return eptp;
}

//  npt creation / destruction (amd)

NPT_PML4* EPTManager::CreateNPT() {
    NPT_PML4* pml4 = (NPT_PML4*)AllocPage();
    if (!pml4) {
        SerialLogger::Log("NPT: Failed to allocate PML4\r\n");
        return nullptr;
    }
    SerialLogger::Log("NPT: Created NPT PML4 at ");
    SerialLogger::LogHex((uint32_t)(uintptr_t)pml4);
    SerialLogger::Log("\r\n");
    return pml4;
}

void EPTManager::DestroyNPT(NPT_PML4* pml4) {
    // same structure as ept
    DestroyEPT(pml4);
}

uint64_t EPTManager::BuildNCR3(NPT_PML4* pml4) {
    // ncr3 is just the physical address of the top-level page table
    return (uint64_t)(uintptr_t)pml4;
}

//  ept page table walk - allocate intermediate levels on demand

// kmemx hook: a no-create leaf walk. returns the existing 4kb leaf entry pointer
// or nullptr (absent uppers or a large-page mapping). thin wrapper over WalkEPT.
// (satoru)
uint64_t* EPTManager::KmemxLeafEntry(EPT_PML4* pml4, uint64_t guest_phys) {
    if (!pml4) return nullptr;
    return WalkEPT(pml4, guest_phys & ~0xFFFULL, /*create=*/false);
}

uint64_t* EPTManager::WalkEPT(EPT_PML4* pml4, uint64_t guest_phys, bool create) {
    // extract indices from guest physical address
    int pml4_idx = (int)((guest_phys >> 39) & 0x1FF);
    int pdpt_idx = (int)((guest_phys >> 30) & 0x1FF);
    int pd_idx   = (int)((guest_phys >> 21) & 0x1FF);
    int pt_idx   = (int)((guest_phys >> 12) & 0x1FF);

    // pml4 → pdpt
    uint64_t* pml4e = &pml4->entries[pml4_idx];
    EPT_PDPT* pdpt;
    if (*pml4e & (EPT_READ | EPT_WRITE | EPT_EXECUTE)) {
        pdpt = (EPT_PDPT*)(uintptr_t)(*pml4e & EPT_ADDR_MASK);
    } else {
        if (!create) return nullptr;
        pdpt = (EPT_PDPT*)AllocPage();
        if (!pdpt) return nullptr;
        *pml4e = (uint64_t)(uintptr_t)pdpt | EPT_READ | EPT_WRITE | EPT_EXECUTE;
    }

    // pdpt → pd
    uint64_t* pdpte = &pdpt->entries[pdpt_idx];
    EPT_PD* pd;
    if (*pdpte & (EPT_READ | EPT_WRITE | EPT_EXECUTE)) {
        if (*pdpte & EPT_LARGE_PAGE) return nullptr; // 1gb mapping, can't split
        pd = (EPT_PD*)(uintptr_t)(*pdpte & EPT_ADDR_MASK);
    } else {
        if (!create) return nullptr;
        pd = (EPT_PD*)AllocPage();
        if (!pd) return nullptr;
        *pdpte = (uint64_t)(uintptr_t)pd | EPT_READ | EPT_WRITE | EPT_EXECUTE;
    }

    // pd → pt
    uint64_t* pde = &pd->entries[pd_idx];
    EPT_PT* pt;
    if (*pde & (EPT_READ | EPT_WRITE | EPT_EXECUTE)) {
        if (*pde & EPT_LARGE_PAGE) return nullptr; // 2mb mapping
        pt = (EPT_PT*)(uintptr_t)(*pde & EPT_ADDR_MASK);
    } else {
        if (!create) return nullptr;
        pt = (EPT_PT*)AllocPage();
        if (!pt) return nullptr;
        *pde = (uint64_t)(uintptr_t)pt | EPT_READ | EPT_WRITE | EPT_EXECUTE;
    }

    return &pt->entries[pt_idx];
}

uint64_t* EPTManager::WalkNPT(NPT_PML4* pml4, uint64_t guest_phys, bool create) {
    // same structure but using npt flags
    int pml4_idx = (int)((guest_phys >> 39) & 0x1FF);
    int pdpt_idx = (int)((guest_phys >> 30) & 0x1FF);
    int pd_idx   = (int)((guest_phys >> 21) & 0x1FF);
    int pt_idx   = (int)((guest_phys >> 12) & 0x1FF);

    uint64_t* pml4e = &pml4->entries[pml4_idx];
    NPT_PDPT* pdpt;
    if (*pml4e & NPT_PRESENT) {
        pdpt = (NPT_PDPT*)(uintptr_t)(*pml4e & NPT_ADDR_MASK);
    } else {
        if (!create) return nullptr;
        pdpt = (NPT_PDPT*)AllocPage();
        if (!pdpt) return nullptr;
        *pml4e = (uint64_t)(uintptr_t)pdpt | NPT_PRESENT | NPT_WRITE | NPT_USER;
    }

    uint64_t* pdpte = &pdpt->entries[pdpt_idx];
    NPT_PD* pd;
    if (*pdpte & NPT_PRESENT) {
        if (*pdpte & NPT_LARGE_PAGE) return nullptr;
        pd = (NPT_PD*)(uintptr_t)(*pdpte & NPT_ADDR_MASK);
    } else {
        if (!create) return nullptr;
        pd = (NPT_PD*)AllocPage();
        if (!pd) return nullptr;
        *pdpte = (uint64_t)(uintptr_t)pd | NPT_PRESENT | NPT_WRITE | NPT_USER;
    }

    uint64_t* pde = &pd->entries[pd_idx];
    NPT_PT* pt;
    if (*pde & NPT_PRESENT) {
        if (*pde & NPT_LARGE_PAGE) return nullptr;
        pt = (NPT_PT*)(uintptr_t)(*pde & NPT_ADDR_MASK);
    } else {
        if (!create) return nullptr;
        pt = (NPT_PT*)AllocPage();
        if (!pt) return nullptr;
        *pde = (uint64_t)(uintptr_t)pt | NPT_PRESENT | NPT_WRITE | NPT_USER;
    }

    return &pt->entries[pt_idx];
}

//  memory mapping

bool EPTManager::MapGuestPhysical(EPT_PML4* pml4, uint64_t guest_phys,
                                   uint64_t host_phys, uint64_t size,
                                   uint64_t flags) {
    if (!pml4) return false;

    // align to page boundaries
    guest_phys &= ~0xFFFULL;
    host_phys  &= ~0xFFFULL;

    uint64_t mapped = 0;
    while (mapped < size) {
        uint64_t* pte = WalkEPT(pml4, guest_phys + mapped, true);
        if (!pte) {
            SerialLogger::Log("EPT: Failed to walk EPT for GPA ");
            SerialLogger::LogHex((uint32_t)((guest_phys + mapped) >> 32));
            SerialLogger::LogHex((uint32_t)(guest_phys + mapped));
            SerialLogger::Log("\r\n");
            return false;
        }
        *pte = (host_phys + mapped) | flags;
        mapped += 4096;
    }
    return true;
}

bool EPTManager::MapGuestPhysicalNPT(NPT_PML4* pml4, uint64_t guest_phys,
                                      uint64_t host_phys, uint64_t size,
                                      uint64_t flags) {
    if (!pml4) return false;

    guest_phys &= ~0xFFFULL;
    host_phys  &= ~0xFFFULL;

    uint64_t mapped = 0;
    while (mapped < size) {
        uint64_t* pte = WalkNPT(pml4, guest_phys + mapped, true);
        if (!pte) return false;
        *pte = (host_phys + mapped) | flags;
        mapped += 4096;
    }
    return true;
}

bool EPTManager::MapRAM(EPT_PML4* pml4, uint64_t guest_phys,
                         uint64_t host_phys, uint64_t size) {
    return MapGuestPhysical(pml4, guest_phys, host_phys, size,
                            EPT_READ | EPT_WRITE | EPT_EXECUTE | EPT_MT_WB);
}

bool EPTManager::MapROM(EPT_PML4* pml4, uint64_t guest_phys,
                         uint64_t host_phys, uint64_t size) {
    return MapGuestPhysical(pml4, guest_phys, host_phys, size,
                            EPT_READ | EPT_EXECUTE | EPT_MT_WB);
}

bool EPTManager::MapMMIO(EPT_PML4* pml4, uint64_t guest_phys,
                          uint64_t host_phys, uint64_t size) {
    return MapGuestPhysical(pml4, guest_phys, host_phys, size,
                            EPT_READ | EPT_WRITE | EPT_MT_UC);
}

//  2mb large page mapping (for gpu bars and large ram regions)

bool EPTManager::MapLargePages(EPT_PML4* pml4, uint64_t guest_phys,
                                uint64_t host_phys, uint64_t size,
                                uint64_t flags) {
    if (!pml4) return false;
    const uint64_t PAGE_2MB = 0x200000ULL;
    
    // align to 2mb boundaries
    guest_phys &= ~(PAGE_2MB - 1);
    host_phys  &= ~(PAGE_2MB - 1);
    
    uint64_t mapped = 0;
    while (mapped < size) {
        uint64_t gpa = guest_phys + mapped;
        uint64_t hpa = host_phys + mapped;
        
        // walk to pd level (pml4 -> pdpt -> pd)
        int pml4_idx = (gpa >> 39) & 0x1FF;
        int pdpt_idx = (gpa >> 30) & 0x1FF;
        int pd_idx   = (gpa >> 21) & 0x1FF;
        
        // ensure pml4 entry points to pdpt
        if (!(pml4->entries[pml4_idx] & (EPT_READ | EPT_WRITE))) {
            void* page = AllocPage();
            if (!page) return false;
            pml4->entries[pml4_idx] = ((uint64_t)(uintptr_t)page) | EPT_READ | EPT_WRITE | EPT_EXECUTE;
        }
        
        EPT_PDPT* pdpt = (EPT_PDPT*)((pml4->entries[pml4_idx]) & ~0xFFFULL);
        
        // ensure pdpt entry points to pd
        if (!(pdpt->entries[pdpt_idx] & (EPT_READ | EPT_WRITE))) {
            void* page = AllocPage();
            if (!page) return false;
            pdpt->entries[pdpt_idx] = ((uint64_t)(uintptr_t)page) | EPT_READ | EPT_WRITE | EPT_EXECUTE;
        }
        
        if (pdpt->entries[pdpt_idx] & EPT_LARGE_PAGE) {
            mapped += PAGE_2MB;
            continue; // already a 1gb mapping, skip
        }
        
        EPT_PD* pd = (EPT_PD*)((pdpt->entries[pdpt_idx]) & ~0xFFFULL);
        
        // set 2mb large page entry directly in pd
        pd->entries[pd_idx] = hpa | flags | EPT_LARGE_PAGE;
        
        mapped += PAGE_2MB;
    }
    return true;
}

bool EPTManager::MapLargePagesNPT(NPT_PML4* pml4, uint64_t guest_phys,
                                   uint64_t host_phys, uint64_t size,
                                   uint64_t flags) {
    if (!pml4) return false;
    const uint64_t PAGE_2MB = 0x200000ULL;
    
    guest_phys &= ~(PAGE_2MB - 1);
    host_phys  &= ~(PAGE_2MB - 1);
    
    uint64_t mapped = 0;
    while (mapped < size) {
        uint64_t gpa = guest_phys + mapped;
        uint64_t hpa = host_phys + mapped;
        
        int pml4_idx = (gpa >> 39) & 0x1FF;
        int pdpt_idx = (gpa >> 30) & 0x1FF;
        int pd_idx   = (gpa >> 21) & 0x1FF;
        
        if (!(pml4->entries[pml4_idx] & NPT_PRESENT)) {
            void* page = AllocPage();
            if (!page) return false;
            pml4->entries[pml4_idx] = ((uint64_t)(uintptr_t)page) | NPT_PRESENT | NPT_WRITE;
        }
        
        NPT_PDPT* pdpt = (NPT_PDPT*)((pml4->entries[pml4_idx]) & ~0xFFFULL);
        
        if (!(pdpt->entries[pdpt_idx] & NPT_PRESENT)) {
            void* page = AllocPage();
            if (!page) return false;
            pdpt->entries[pdpt_idx] = ((uint64_t)(uintptr_t)page) | NPT_PRESENT | NPT_WRITE;
        }
        
        if (pdpt->entries[pdpt_idx] & NPT_LARGE_PAGE) {
            mapped += PAGE_2MB;
            continue;
        }
        
        NPT_PD* pd = (NPT_PD*)((pdpt->entries[pdpt_idx]) & ~0xFFFULL);
        pd->entries[pd_idx] = hpa | flags | NPT_LARGE_PAGE;
        
        mapped += PAGE_2MB;
    }
    return true;
}

bool EPTManager::MapPCIBar(EPT_PML4* pml4, uint64_t bar_phys, uint64_t size) {
    // map pci bar mmio region 1:1 (identity) into guest physical address space
    // for gpu passthrough, the bar physical address is mapped at the same gpa
    uint64_t flags = EPT_READ | EPT_WRITE | EPT_MT_UC;
    
    // use 2mb large pages for bars >= 2mb for efficiency
    if (size >= 0x200000ULL && (bar_phys & 0x1FFFFFULL) == 0) {
        return MapLargePages(pml4, bar_phys, bar_phys, size, flags);
    }
    return MapGuestPhysical(pml4, bar_phys, bar_phys, size, flags);
}

bool EPTManager::MapPCIBarNPT(NPT_PML4* pml4, uint64_t bar_phys, uint64_t size) {
    uint64_t flags = NPT_PRESENT | NPT_WRITE | NPT_USER;
    
    if (size >= 0x200000ULL && (bar_phys & 0x1FFFFFULL) == 0) {
        return MapLargePagesNPT(pml4, bar_phys, bar_phys, size, flags);
    }
    return MapGuestPhysicalNPT(pml4, bar_phys, bar_phys, size, flags);
}

//  memory region tracking

void EPTManager::AddRegion(const GuestMemRegion& region) {
    if (region_count >= MAX_MEM_REGIONS) return;
    regions[region_count++] = region;
}

int EPTManager::GetRegionCount() { return region_count; }

const GuestMemRegion* EPTManager::GetRegion(int idx) {
    if (idx < 0 || idx >= region_count) return nullptr;
    return &regions[idx];
}

//  tlb invalidation

void EPTManager::InvalidateEPT() {
    if (VMM::GetType() != VIRT_INTEL_VTX) return;

    // invept - type 2 = global invalidation (all ept translations)
    struct {
        uint64_t eptp;
        uint64_t reserved;
    } desc = {0, 0};

    uint8_t error = 0;
    asm volatile(
        "invept %[desc], %[type]\n\t"
        "setna %[err]"
        : [err] "=rm"(error)
        : [desc] "m"(desc), [type] "r"((uint64_t)2)
        : "cc", "memory"
    );

    if (error) {
        SerialLogger::Log("EPT: INVEPT failed\r\n");
    }
}

void EPTManager::InvalidateVPID() {
    if (VMM::GetType() != VIRT_INTEL_VTX) return;

    struct {
        uint64_t vpid;
        uint64_t linear_addr;
    } desc = {0, 0};

    uint8_t error = 0;
    asm volatile(
        "invvpid %[desc], %[type]\n\t"
        "setna %[err]"
        : [err] "=rm"(error)
        : [desc] "m"(desc), [type] "r"((uint64_t)2)
        : "cc", "memory"
    );

    if (error) {
        SerialLogger::Log("EPT: INVVPID failed\r\n");
    }
}

//  ept violation / nested page fault handling

bool EPTManager::HandleEPTViolation(uint64_t guest_phys, uint64_t qualification) {
    // kmemx: if kmemx compressed this guest page out at the ept level, decompress
    // it back + restore the leaf and resume the guest. checked first + silently
    // (no serial spam) since it is the common case once guest compression is
    // active; only non-kmemx violations fall through to the diagnostic path. (satoru)
    if (KMemX::HandleGuestFaultAny(guest_phys)) return true;

    SerialLogger::Log("EPT: Violation at GPA ");
    SerialLogger::LogHex((uint32_t)(guest_phys >> 32));
    SerialLogger::LogHex((uint32_t)guest_phys);
    SerialLogger::Log(" qual=");
    SerialLogger::LogHex((uint32_t)qualification);
    SerialLogger::Log("\r\n");

    bool is_read  = (qualification & EPT_VIOL_READ) != 0;
    bool is_write = (qualification & EPT_VIOL_WRITE) != 0;
    bool is_exec  = (qualification & EPT_VIOL_EXEC) != 0;

    // check if this gpa falls in a known region
    for (int i = 0; i < region_count; i++) {
        if (guest_phys >= regions[i].guest_phys_start &&
            guest_phys < regions[i].guest_phys_start + regions[i].size) {
            // known region - might be lazy mapping
            SerialLogger::Log("EPT: GPA in region #");
            SerialLogger::LogDec(i);
            SerialLogger::Log(" type=");
            SerialLogger::LogDec(regions[i].type);
            SerialLogger::Log("\r\n");

            // for mmio regions, handle the access in the virtual device layer
            if (regions[i].type == MEM_MMIO) {
                SerialLogger::Log("EPT: MMIO access - delegate to vdevice\r\n");
                return true; // handled
            }
            return false; // not a lazy-map case
        }
    }

    // unknown region
    SerialLogger::Log("EPT: Unhandled violation - ");
    if (is_read) SerialLogger::Log("READ ");
    if (is_write) SerialLogger::Log("WRITE ");
    if (is_exec) SerialLogger::Log("EXEC ");
    SerialLogger::Log("\r\n");
    return false;
}

bool EPTManager::HandleNPF(uint64_t guest_phys, uint64_t error_code) {
    SerialLogger::Log("NPT: Nested Page Fault at GPA ");
    SerialLogger::LogHex((uint32_t)(guest_phys >> 32));
    SerialLogger::LogHex((uint32_t)guest_phys);
    SerialLogger::Log(" error=");
    SerialLogger::LogHex((uint32_t)error_code);
    SerialLogger::Log("\r\n");

    // same region check as ept
    for (int i = 0; i < region_count; i++) {
        if (guest_phys >= regions[i].guest_phys_start &&
            guest_phys < regions[i].guest_phys_start + regions[i].size) {
            if (regions[i].type == MEM_MMIO) {
                return true; // mmio handled by vdevice layer
            }
        }
    }
    return false;
}

//  debug

void EPTManager::DumpEPT(EPT_PML4* pml4, int max_entries) {
    if (!pml4) return;
    int count = 0;
    SerialLogger::Log("=== EPT Dump ===\r\n");
    for (int i = 0; i < 512 && count < max_entries; i++) {
        if (!(pml4->entries[i] & (EPT_READ | EPT_WRITE | EPT_EXECUTE))) continue;
        SerialLogger::Log("PML4[");
        SerialLogger::LogDec(i);
        SerialLogger::Log("] = ");
        SerialLogger::LogHex((uint32_t)(pml4->entries[i] >> 32));
        SerialLogger::LogHex((uint32_t)pml4->entries[i]);
        SerialLogger::Log("\r\n");
        count++;
    }
}

void EPTManager::DumpRegions() {
    SerialLogger::Log("=== Guest Memory Regions ===\r\n");
    for (int i = 0; i < region_count; i++) {
        SerialLogger::Log("  #");
        SerialLogger::LogDec(i);
        SerialLogger::Log(": GPA=");
        SerialLogger::LogHex((uint32_t)regions[i].guest_phys_start);
        SerialLogger::Log(" HPA=");
        SerialLogger::LogHex((uint32_t)regions[i].host_phys_start);
        SerialLogger::Log(" size=");
        SerialLogger::LogHex((uint32_t)regions[i].size);
        SerialLogger::Log(" type=");
        SerialLogger::LogDec(regions[i].type);
        SerialLogger::Log("\r\n");
    }
}
