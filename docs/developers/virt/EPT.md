# EPT

`src/virt/ept.cpp` and `ept.h`. Intel EPT and AMD NPT, the second level of address translation for
guests. The manager is `EPTManager`.

When a guest touches a physical address, EPT translates guest physical to host physical.
Without it the hypervisor would have to trap every guest page table change, which is
painfully slow. With it, the hardware does the work.

## Shape

Four levels: EPT PML4, EPDPT, EPD, EPT, 512 entries each. Leaf entries map a 4 KB page, or a
2 MB page when the mapping is large (`MapLargePages`). Permissions are the read, write and execute
bits in each entry. A violation raises an EPT_VIOLATION VM exit.

## The calls

- `EPTManager::CreateEPT()` and `DestroyEPT()` make and free a root.
- `BuildEPTP()` builds the EPT pointer value for the VMCS.
- `MapRAM()` maps guest RAM, `MapROM()` the ROM area, `MapMMIO()` device MMIO.
- `MapPCIBar()` maps a passthrough device's BAR (see [PCI_PASSTHROUGH.md](PCI_PASSTHROUGH.md)).
- `CreateNPT()`, `BuildNCR3()` and the `*NPT` calls do the same work for AMD.

## Lazy mapping

Guest RAM is backed chunk by chunk, so the EPT entries for a chunk are added when the chunk is
first touched, not when the VM is created. See [DYNAMIC_VM.md](DYNAMIC_VM.md). A chunk that goes
back to the host is removed from EPT with `UnmapRange`.

## Violations

If the guest touches a guest physical address with no mapping, EPT_VIOLATION fires. The
`vmexit.cpp` handler calls `Hypervisor::LazyMapRAM()`, which backs the missing chunk and maps it
(one 4 KB page at a time when the chunk fell back to split pages), then re-enters the guest.

## Files

- `src/virt/ept.cpp` and `ept.h` the tables and the calls above
- `src/virt/hypervisor.cpp` creates and holds the EPT pointer
- `src/virt/vmexit.cpp` calls into EPT on EPT_VIOLATION
- `src/virt/guest_mem.cpp` backs guest RAM chunks with host frames
