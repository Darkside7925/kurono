// KLD VT-d: put a container's device in a second-level domain that maps only
// the frames the broker handed to that container.
//
// The engine is self-contained (DMAR parse, root/context tables, second-level
// pages, queued or register invalidation, fault drain and the negative-test
// canary). The old GPU passthrough code in src/virt/iommu.cpp is left exactly
// as it was; its bugs are listed in scratchpad/oc_kldg_report.md (M7).
//
// (satoru)

#pragma once

#include "types.h"

namespace KldIommu {

// kurono.kld.iommu=0 turns the whole thing off (today's behaviour). (satoru)
void SetEnabled(bool on);
bool Enabled();

// kurono.kld.iommutest=1: test-only canary op + checker process. (satoru)
void SetTestMode(bool on);
bool TestMode();

// kurono.kld.iommu.native=1: test-only native NVMe read after translation
// is enabled, so a pass-through context bug cannot hide behind a quiet bus.
// (satoru)
void SetNativeTest(bool on);

// kurono.kld.iommuaw=39|48 forces the second-level width (if SAGAW supports
// it) so both table shapes are exercised. 0 = best supported. (satoru)
void SetForcedAw(int bits);

// called on the first KLD grant for bdf. parses DMAR, selects the remapping
// unit that covers bdf, prepares all context tables and enables translation
// on that unit only. false = today's DMA behaviour stays. (satoru)
bool EnsureOn(uint32_t bdf);

// put bdf in the container's own domain. family is for the log only. (satoru)
bool Attach(uint64_t address_space, const char* family, uint32_t bdf);

// map/unmap one contiguous broker DMA allocation (iova == physical). (satoru)
bool MapDma(uint64_t address_space, uint64_t phys, uint32_t pages);
void UnmapDma(uint64_t address_space, uint64_t phys, uint32_t pages);

// vm passthrough: map guest-physical iova -> host phys (iova != phys) in the
// domain Attach made for `address_space`, then FlushDomain once at the end.
// (satoru)
bool MapIova(uint64_t address_space, uint64_t iova, uint64_t phys, uint32_t pages);
void FlushDomain(uint64_t address_space);

// device detached and blocked, invalidations completed, page tables freed.
// called by the broker after bus master off and msi off, before frame frees.
// (satoru)
void ReleaseContainer(uint64_t address_space);

// test-only: allocate/fill the canary and return its physical address.
// (satoru)
bool TestCanary(uint64_t* phys);
void TestCheckEntry();

// drains the fault recording registers, rate limited, never panics. (satoru)
void FaultPollEntry();

bool IsOn();

}  // namespace KldIommu

// end (satoru)
