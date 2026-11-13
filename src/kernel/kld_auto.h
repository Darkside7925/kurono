// KLD auto-match: pick the driver container from the ids the driver declares.
//
// The match table is generated at build time by kld/tools/gen_match_table.sh
// from each built family's own pci id table (the container's --kld-ids mode
// prints it). Nothing here knows a device id: the kernel only implements
// Linux's matching rule (drivers/pci/pci-driver.c pci_match_one_device) over
// that table, plus the native-owner skip rule and the per-family flags from
// kld/families/<family>.manifest.
//
// kurono.kld=auto turns this on. Without the flag nothing in this file runs
// and the boot is byte-for-byte the old one.
//
// (satoru)

#pragma once

#include "types.h"

namespace KldAuto {

// one pci id entry, in the order the driver declares it. (satoru)
struct MatchId {
    uint32_t vendor;
    uint32_t device;
    uint32_t subvendor;
    uint32_t subdevice;
    uint32_t class_id;
    uint32_t class_mask;
};


// a family's slice of kld_auto_ids, plus its manifest properties. (satoru)
struct MatchFamily {
    const char* name;
    uint32_t flags;
    uint32_t first_id;
    uint32_t id_count;
    // manifest bar_window lines, "<bar>:<lo>-<hi>,...;<bar>:..." (hex,
    // inclusive offsets), or null. (satoru)
    const char* bar_window;
};

// from kld/families/<family>.manifest: the family needs to map the BAR page
// that holds the MSI-X table (its own registers share that page). the broker
// grants that page only to a device matched to this family. (satoru)
constexpr uint32_t FAMILY_NEEDS_MSIX_TABLE_PAGE = 1u << 0;
// the family may use the legacy VGA port range through the broker, and only
// when its granted display device is the one decoding those ports. (satoru)
constexpr uint32_t FAMILY_LEGACY_VGA = 1u << 1;
// the family drives a display adapter through the kld display bridge; required
// for DISPLAY_ATTACH, independent of whether it also needs legacy vga ports.
// (satoru)
constexpr uint32_t FAMILY_DISPLAY = 1u << 2;
// from line_irq=acpi|pci and platform_mmio=acpi: the families allowed to take
// a line interrupt (or map a platform MMIO range) through the broker. (satoru)
constexpr uint32_t FAMILY_LINE_IRQ_ACPI        = 1u << 3;
constexpr uint32_t FAMILY_LINE_IRQ_PCI         = 1u << 4;
constexpr uint32_t FAMILY_PLATFORM_MMIO_ACPI   = 1u << 5;
// version of the flag bit layout above. gen_match_table.sh copies this number
// into the table; the kernel refuses a table with another (or no) version, so a
// stale table can never grant the wrong privileges. bump it with any change to
// the bits. (satoru)
constexpr unsigned FLAG_LAYOUT_VERSION = 3;   // 3: d4 bits 6-8 + MatchFamily.bar_window (satoru)
// d4 manifest properties: opregion 1 (the broker may map the device's
// graphics opregion read-only), bar_policy 1 (kurono.kld.bar=ro|window
// applies to this family's bars), host_bridge_read 1 (read-only config of
// 00:00.0). (satoru)
constexpr uint32_t FAMILY_OPREGION = 1u << 6;
constexpr uint32_t FAMILY_BAR_POLICY = 1u << 7;
constexpr uint32_t FAMILY_HOST_BRIDGE_READ = 1u << 8;

// the generated table (kld/build/kld_match_table.cpp) is reached through four
// weak accessor functions, so a default build without it links: a null
// function pointer means "no table". (satoru)

// vt-d flags (kurono.kld.iommu=0, .iommutest=1, .iommuaw=39|48). parsed for
// every kld boot, including the legacy bench path that never calls BootInit.
// (satoru)
void ParseIommuFlags(const char* boot_cmdline);

// parse the flag and the take override before Network::Init, and tell the
// native NIC drivers to stand down when an embedded family can drive a NIC.
// called from the boot cmdline parser. (satoru)
void BootInit(const char* boot_cmdline);

// the auto-match kernel process: scan once (through the broker's lazy scan
// data), log one decision per pci device, start one container per matched
// family that is embedded. (satoru)
void RunnerEntry();

// Linux pci_match_one_device over the generated table. returns the first
// family whose id table matches; family_out is valid while the kernel lives.
// used by the broker's grant path and by the self check. (satoru)
bool MatchDevice(uint32_t vendor, uint32_t device, uint32_t subvendor,
                 uint32_t subdevice, uint32_t class_code,
                 const char** family_out, bool* needs_table_page_out);

// same match, but only against the named family's own entries. this is the
// "family started by name" path used by the legacy flags, and the negative
// case the self check needs (a flagged family and a foreign device id).
// (satoru)
bool FamilyMatchDeviceByName(const char* family, uint32_t vendor,
                             uint32_t device, uint32_t subvendor,
                             uint32_t subdevice, uint32_t class_code,
                             bool* needs_table_page_out);

// is the generated match table in this kernel? the broker uses this to log
// the reason a grant got no MSI-X table page exception. (satoru)
bool TablePresent();

// flags of a family in the generated table (0 when unknown). the broker uses
// FAMILY_LEGACY_VGA on the grant path. (satoru)
uint32_t FamilyFlags(const char* family);
// the family's manifest bar write window string, or null. (satoru)
const char* FamilyBarWindow(const char* family);
// the family is in the generated table (its manifest properties are known). (satoru)
bool FamilyKnown(const char* family);

// kurono.kld.display.legacyvga=1: allow the legacy VGA port ops even when the
// broker's sole-decoder check is not satisfied. parsed in BootInit. (satoru)
bool LegacyVgaForced();

// kernel self check only: pretend the table is absent so the empty-table
// refusal and its log line are exercised. (satoru)
void SetTableAbsentForTest(bool on);

// kernel self check for the MSI-X table page rule (kurono.kld.msixcheck=1).
// (satoru)
void MsixPolicySelfCheck();

// synthetic device lists through the same per-device decision the boot uses:
// native ids kept, take override, unknown ids, matched but not embedded.
// (satoru)
void PolicySelfCheck();

// the tiny kernel process the flag starts: sleeps, runs both self checks,
// stays parked so the scheduler has a normal live kernel task. (satoru)
void MsixCheckEntry();

// the device is taken for a kld container (kurono.kld=auto + kurono.kld.take=):
// native drivers must not touch it. (satoru)
bool TakesDevice(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t vendor, uint16_t device);

}  // namespace KldAuto

// end (satoru)
