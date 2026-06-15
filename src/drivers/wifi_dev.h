#pragma once
//  wifi hardware device layer for kurono os (satoru)
//  real pci enumeration + bar mapping + chip identification for wireless nics.
//  this is the hardware foundation the 802.11 radio driver sits on: it finds the
//  card on the pci bus, names the exact model, maps its mmio register window, and
//  enables bus-mastering so a driver can talk to it. the 802.11 mac (scan/auth/
//  assoc), per-chip firmware load, and wpa2 are the next phase and need real
//  hardware to bring up (qemu emulates no wifi nic). (satoru)

#include "../kernel/types.h"

// pci class 0x02 subclass 0x80 = "other network controller" = where wireless
// nics live (ethernet is subclass 0x00). (satoru)
#define WIFI_PCI_CLASS      0x02
#define WIFI_PCI_SUBCLASS   0x80

// known wireless vendor ids. (satoru)
#define WIFI_VENDOR_INTEL     0x8086
#define WIFI_VENDOR_ATHEROS   0x168c   // qualcomm atheros
#define WIFI_VENDOR_QUALCOMM  0x17cb   // qualcomm (newer qca)
#define WIFI_VENDOR_REALTEK   0x10ec
#define WIFI_VENDOR_BROADCOM  0x14e4
#define WIFI_VENDOR_RALINK    0x1814   // mediatek/ralink

// chip families a per-chip 802.11 driver would dispatch on. (satoru)
enum WifiChipFamily {
    WIFI_FAM_UNKNOWN = 0,
    WIFI_FAM_INTEL_IWLWIFI,   // intel ax2xx / 9xxx / 8xxx / 7xxx (needs ucode)
    WIFI_FAM_ATHEROS_ATH9K,   // ar9xxx (firmware-free, on-die)
    WIFI_FAM_ATHEROS_ATH10K,  // qca988x / qca6174 (needs firmware)
    WIFI_FAM_REALTEK_RTW,     // rtl88xx / rtl87xx
    WIFI_FAM_BROADCOM_BRCM,   // bcm43xx (brcmfmac)
    WIFI_FAM_RALINK_RT2X00
};

// a probed wireless nic. populated by WifiDev::Probe() from the live pci bus.
// (satoru)
struct WifiDevice {
    bool      present;
    uint8_t   bus, slot, func;
    uint16_t  vendor;
    uint16_t  device;
    uint8_t   irq;            // pci interrupt line
    uint64_t  bar0_phys;      // mmio register window base (64-bit-bar aware)
    uint64_t  bar0_size;      // mapped window size (best-effort)
    volatile uint8_t* mmio;   // mapped + dereferenceable register window, or null
    WifiChipFamily family;
    char      model[64];      // friendly name, e.g. "intel wi-fi 6 ax200"
    bool      needs_firmware; // true for intel/ath10k/realtek/broadcom
    bool      mmio_mapped;    // bar window mapped + a register read returned non-bus-error
};

class WifiDev {
public:
    // walk the pci bus once, find the first wireless nic, capture + map it.
    // idempotent (caches). returns true if a wireless nic was found. (satoru)
    static bool Probe();
    static bool Present();
    static const WifiDevice* Info();

    // a 32-bit mmio register read/write through the mapped bar (no-ops if the
    // bar isn't mapped). the radio driver uses these. (satoru)
    static uint32_t RegRead(uint32_t off);
    static void     RegWrite(uint32_t off, uint32_t val);

private:
    static WifiDevice dev;
    static bool probed;

    static uint32_t pci_cfg_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
    static void     pci_cfg_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val);
    static void     identify(WifiDevice* d);   // vendor:device -> family + model
    static bool     map_bar(WifiDevice* d);    // identity-map the mmio window
};
// end (satoru)
