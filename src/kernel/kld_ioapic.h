#pragma once
#include "types.h"

//  KLD line interrupts: the I/O APIC half.
//
//  Kurono has no I/O APIC driver and native interrupts use the 8259 through
//  LINT0. A container that needs a real line (the PCH GPIO controller's GSI 14,
//  a PCI function's INTx) gets its own redirection entry programmed here by the
//  broker, and the vector is one of the broker's MSI vectors. Nothing is
//  written to any I/O APIC until a container attaches a line; the MADT is only
//  read (the same XSDT/RSDT walk SMP::find_madt uses, no AML).
//
// end (satoru)

namespace KldIoApic {

// flags of an attached line (the broker op's a1). LINE_ISA means the number is
// an 8259 IRQ that the MADT may remap through a source override; without it the
// number is already an I/O APIC GSI. LINE_DEFAULT means "polarity/trigger come
// from the MADT override or the ISA bus default"; without it the LEVEL and
// ACTIVE_LOW bits are explicit, including "both clear = edge, active high".
// (satoru)
constexpr uint32_t LINE_LEVEL      = 1u << 0;
constexpr uint32_t LINE_ACTIVE_LOW = 1u << 1;
constexpr uint32_t LINE_SHARED     = 1u << 2;
constexpr uint32_t LINE_ISA        = 1u << 3;
constexpr uint32_t LINE_DEFAULT    = 1u << 4;

// read the MADT (memory only). idempotent, touches no I/O APIC register. (satoru)
bool Ensure();

uint32_t IoApicCount();
uint64_t Base();

// type 2 interrupt source overrides: an ISA IRQ (0..15) may map to another GSI
// and carry raw ACPI trigger/polarity flags (polarity bits 1:0, trigger bits
// 3:2). @isa selects whether the override applies (a source IRQ) or the number
// is already an I/O APIC GSI. returns false if the GSI is not owned by any I/O
// APIC. reads the I/O APIC version/pin count on the first call. (satoru)
bool Resolve(uint32_t gsi, bool isa, uint32_t* out_gsi, uint32_t* out_flags);

// the 8259 cascade is never a device line. (satoru)
bool IsKernelOwned(uint32_t gsi);

// false when any 8259 line involved in this request is currently unmasked, i.e.
// a native user (PIT, keyboard, PS/2 mouse, serial, SCI) owns it. reads the two
// pic mask registers; touches no I/O APIC register. (satoru)
bool LegacyBusy(uint32_t gsi, uint32_t resolved_gsi);

// program the redirection entry for @gsi: fixed delivery to @apic_id, masked
// unless @masked is false. polarity/trigger come from the flags above. the
// I/O APIC registers are first touched here (lazy version + pin count read).
// (satoru)
bool Program(uint32_t gsi, uint32_t vector, uint32_t apic_id, uint32_t flags,
             bool masked);
bool Mask(uint32_t gsi);
bool Unmask(uint32_t gsi);

// how a level delivery's remote irr is cleared for this line:
//   0 = the local EOI broadcasts (SVR bit 12 clear), 1 = write the ioapic's
//   direct EOI register at base+0x40 (version >= 0x20, SVR bit 12 set),
//   -1 = suppression set and no EOI register: the attach must be refused.
// reads the ioapic version (first register access if lazy). (satoru)
int EoiMode(uint32_t gsi, bool lapic_suppress);
uint32_t VersionForGsi(uint32_t gsi);

// write base+0x40 directly (NOT through IOREGSEL/IOWIN) when the mode is 1.
// counted in the access counters, under the register lock. (satoru)
bool Eoi(uint32_t gsi, uint32_t vector);

// test/diagnostics: snapshot every redirection entry of the first I/O APIC at
// attach and compare the entries other than `gsi` at detach (volatile status
// bits masked). logs PASS/FAIL. (satoru)
// both are no-ops unless SetAudit(true) (kurono.kld.ioapic.audit=1): the
// production attach and detach paths must not read ~1000 registers with
// interrupts off. (satoru)
void SetAudit(bool on);
bool AuditEnabled();
void SnapshotEntries();
bool CheckEntriesExcept(uint32_t gsi);

// keep the 8259 line of the same number masked so the interrupt cannot arrive
// through both paths. only for ISA IRQs. (satoru)
void MaskLegacyPic(uint32_t gsi);

// true when [base, base+len) overlaps any I/O APIC register page from the
// MADT. used by PLAT_MAP to keep mmio grants off the interrupt hardware.
// reads no registers. (satoru)
bool OwnsRange(uint64_t base, uint64_t len);

// diagnostics: every IOREGSEL/IOWIN access, 0 on a boot that never attached a
// line (kurono.kld.stats=1 prints it once after boot). (satoru)
unsigned long Accesses();
unsigned long RegReads();
unsigned long RegWrites();

}  // namespace KldIoApic

// end (satoru)
