# Extended Page Tables (EPT)

`src/virt/ept.cpp` and `ept.h` implement Intel EPT and AMD NPT (Nested Page Tables) for guest memory isolation.

## 1. What EPT does

EPT provides a second level of address translation for virtual machines. When a guest accesses a physical address, EPT translates that guest physical address to a real host physical address. Without EPT, the hypervisor would have to intercept every single guest page table modification - very slow. With EPT, the hardware handles most translations in hardware.

## 2. Structure

The EPT uses a four-level page table: EPT PML4 → EPDPT → EPD → EPT. Each level has 512 entries. Leaf entries map a 4 KB guest physical page frame to a host physical page.

Page permissions are encoded in each entry: read, write, and execute bits. Violations cause EPT_VIOLATION VM exits.

## 3. Building the initial mapping

At VM create time, the hypervisor calls `EPT::BuildIdentityMap(guest_phys_base, size)` to create an identity-mapped region covering the guest RAM allocation. This lets the guest see a flat contiguous physical address space starting at 0.

## 4. EPT violation handling

When the guest accesses a guest physical address not covered by the current EPT, an EPT_VIOLATION VM exit fires. The `vmexit.cpp` handler calls `EPT::MapPage(gpa, hpa, perms)` to add the mapping and re-enters the guest. This on-demand mapping is used for device MMIO ranges.

## 5. Related files

- `src/virt/hypervisor.cpp` - creates and holds the EPT pointer
- `src/virt/vmexit.cpp` - calls EPT on EPT_VIOLATION exits
- `src/virt/guest_mem.cpp` - allocates host physical pages for guest RAM
