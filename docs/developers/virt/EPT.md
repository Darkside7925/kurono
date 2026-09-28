# EPT

`src/virt/ept.cpp` and `ept.h`. Intel EPT and AMD NPT, the second level of address
translation for guests.

When a guest touches a physical address, EPT translates guest physical to host physical.
Without it the hypervisor would have to trap every guest page table change, which is
painfully slow. With it, the hardware does the work.

## Shape

Four levels: EPT PML4, EPDPT, EPD, EPT. 512 entries each. Leaf entries map a 4KB guest
physical page to a host physical page.

Permissions are the read/write/execute bits in each entry. Violations raise an
EPT_VIOLATION VM exit.

## Initial mapping

At VM create time the hypervisor calls `EPT::BuildIdentityMap(guest_phys_base, size)` to
identity-map the guest RAM allocation, so the guest sees flat contiguous physical memory
starting at 0.

## Violations

If the guest touches a guest physical address with no mapping, EPT_VIOLATION fires. The
`vmexit.cpp` handler calls `EPT::MapPage(gpa, hpa, perms)` to add it and re-enters the
guest. Device MMIO ranges get mapped on demand this way.

## Files

- `src/virt/hypervisor.cpp` creates and holds the EPT pointer
- `src/virt/vmexit.cpp` calls into EPT on EPT_VIOLATION
- `src/virt/guest_mem.cpp` allocates host physical pages for guest RAM
