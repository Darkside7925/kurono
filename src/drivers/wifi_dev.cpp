//  wifi hardware device layer for kurono os (satoru)
//  see wifi_dev.h. real pci enumeration + bar mapping + chip id for wireless nics.

#include "wifi_dev.h"
#include "../kernel/vmm.h"
#include "serial.h"

WifiDevice WifiDev::dev = {};
bool WifiDev::probed = false;

// ── pci config space (mechanism 1: 0xcf8 address / 0xcfc data) ──────── (satoru)
uint32_t WifiDev::pci_cfg_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) | (off & 0xFC);
    __asm__ __volatile__("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
    uint32_t v;
    __asm__ __volatile__("inl %1, %0" : "=a"(v) : "Nd"((uint16_t)0xCFC));
    return v;
}

void WifiDev::pci_cfg_write(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val) {
    uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
                    ((uint32_t)func << 8) | (off & 0xFC);
    __asm__ __volatile__("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
    __asm__ __volatile__("outl %0, %1" : : "a"(val), "Nd"((uint16_t)0xCFC));
}

// small helper: copy a literal model name into the device. (satoru)
static void set_model(WifiDevice* d, const char* s) {
    int i = 0;
    while (s[i] && i < 63) { d->model[i] = s[i]; i++; }
    d->model[i] = 0;
}

// ── vendor:device -> chip family + friendly model + firmware need ───── (satoru)
void WifiDev::identify(WifiDevice* d) {
    d->family = WIFI_FAM_UNKNOWN;
    d->needs_firmware = true;   // assume firmware unless we know otherwise
    set_model(d, "wireless controller");

    switch (d->vendor) {
    case WIFI_VENDOR_INTEL: {
        d->family = WIFI_FAM_INTEL_IWLWIFI;   // every intel wifi part needs ucode
        switch (d->device) {
        case 0x2723: set_model(d, "intel wi-fi 6 ax200"); break;
        case 0x2725: case 0x7af0: case 0x51f0: case 0x7e40:
                     set_model(d, "intel wi-fi 6e ax210/ax211"); break;
        case 0xa0f0: case 0x4df0: case 0x06f0: case 0x34f0:
        case 0x3df0: case 0x43f0: set_model(d, "intel wi-fi 6 ax201"); break;
        case 0x9df0: case 0xa370: case 0x31dc: set_model(d, "intel wireless-ac 9560"); break;
        case 0x2526: set_model(d, "intel wireless-ac 9260"); break;
        case 0x24fd: set_model(d, "intel wireless-ac 8265"); break;
        case 0x24f3: set_model(d, "intel wireless-ac 8260"); break;
        case 0x095a: case 0x095b: set_model(d, "intel wireless-ac 7265"); break;
        case 0x08b1: case 0x08b2: set_model(d, "intel wireless-ac 7260"); break;
        case 0x3165: case 0x3166: set_model(d, "intel wireless-ac 3165"); break;
        case 0x24fb: set_model(d, "intel wireless-ac 3168"); break;
        default:     set_model(d, "intel iwlwifi"); break;
        }
        break;
    }
    case WIFI_VENDOR_ATHEROS: {
        // ar9xxx are firmware-free (on-die mac); newer qca988x/qca6174 are ath10k.
        switch (d->device) {
        case 0x002a: case 0x002b: case 0x002c: case 0x002d: case 0x002e:
        case 0x0030: case 0x0032: case 0x0034: case 0x0036: case 0x0037:
            d->family = WIFI_FAM_ATHEROS_ATH9K; d->needs_firmware = false;
            set_model(d, "atheros ar9xxx (ath9k)"); break;
        case 0x003c: d->family = WIFI_FAM_ATHEROS_ATH10K; set_model(d, "qualcomm qca988x"); break;
        case 0x003e: d->family = WIFI_FAM_ATHEROS_ATH10K; set_model(d, "qualcomm qca6174"); break;
        case 0x0042: d->family = WIFI_FAM_ATHEROS_ATH10K; set_model(d, "qualcomm qca9377"); break;
        default:     d->family = WIFI_FAM_ATHEROS_ATH9K; d->needs_firmware = false;
                     set_model(d, "atheros wireless"); break;
        }
        break;
    }
    case WIFI_VENDOR_QUALCOMM:
        d->family = WIFI_FAM_ATHEROS_ATH10K; set_model(d, "qualcomm qca wireless"); break;
    case WIFI_VENDOR_REALTEK: {
        d->family = WIFI_FAM_REALTEK_RTW;
        switch (d->device) {
        case 0x8821: case 0xc821: case 0xc822: set_model(d, "realtek rtl8821ce"); break;
        case 0x8723: case 0xb723: set_model(d, "realtek rtl8723be"); break;
        case 0xb822: set_model(d, "realtek rtl8822ce"); break;
        case 0x8812: set_model(d, "realtek rtl8812ae"); break;
        default:     set_model(d, "realtek rtw wireless"); break;
        }
        break;
    }
    case WIFI_VENDOR_BROADCOM:
        d->family = WIFI_FAM_BROADCOM_BRCM; set_model(d, "broadcom bcm43xx"); break;
    case WIFI_VENDOR_RALINK:
        d->family = WIFI_FAM_RALINK_RT2X00; set_model(d, "mediatek/ralink wireless"); break;
    default:
        d->family = WIFI_FAM_UNKNOWN; set_model(d, "unknown wireless controller"); break;
    }
}

// is this a wireless nic? pci class 0x02 subclass 0x80, or a known wifi vendor/
// device whose subclass is mis-reported as ethernet. (satoru)
static bool is_wireless(uint16_t vendor, uint16_t device, uint8_t cls, uint8_t sub) {
    if (cls != WIFI_PCI_CLASS) return false;
    if (sub == WIFI_PCI_SUBCLASS) return true;
    // some cards report subclass 0x00; trust the vendor for the well-known ones.
    if (vendor == WIFI_VENDOR_ATHEROS || vendor == WIFI_VENDOR_QUALCOMM ||
        vendor == WIFI_VENDOR_BROADCOM || vendor == WIFI_VENDOR_RALINK) return true;
    if (vendor == WIFI_VENDOR_INTEL &&
        (device == 0x2723 || device == 0x2725 || device == 0x24fd || device == 0x24f3 ||
         device == 0x095a || device == 0x08b1 || device == 0x2526 || device == 0x9df0 ||
         device == 0xa0f0)) return true;
    if (vendor == WIFI_VENDOR_REALTEK &&
        (device == 0x8821 || device == 0x8723 || device == 0xb822 || device == 0x8812)) return true;
    return false;
}

// ── map the mmio register window so a driver can dereference it ──────── (satoru)
bool WifiDev::map_bar(WifiDevice* d) {
    uint32_t bar0_val = pci_cfg_read(d->bus, d->slot, d->func, 0x10);
    if (bar0_val & 0x1) return false;          // i/o-space bar -> not the mmio window
    uint64_t bar_addr = (uint64_t)(bar0_val & ~0xFu);
    // 64-bit memory bar (type bits 10b): the high dword lives in bar1. (satoru)
    if (((bar0_val >> 1) & 0x3) == 0x2) {
        uint32_t bar1_val = pci_cfg_read(d->bus, d->slot, d->func, 0x14);
        bar_addr |= ((uint64_t)bar1_val << 32);
    }
    if (!bar_addr) return false;
    d->bar0_phys = bar_addr;
    d->bar0_size = 0x20000;   // 128kb covers the register block on these parts
    // identity-map the window as uncached mmio before any dereference (the bar
    // can sit above the boot identity map). mirrors nvme/virtio-gpu. the vmm now
    // correctly applies PTE_PCD even over an existing huge identity page. (satoru)
    for (uint64_t p = bar_addr & ~0xFFFULL;
         p < (bar_addr & ~0xFFFULL) + d->bar0_size; p += 0x1000ULL) {
        if (!KernelVMM::MapPage(p, p, PTE_PRESENT | PTE_WRITABLE | PTE_PCD)) return false;
    }
    d->mmio = (volatile uint8_t*)(uintptr_t)bar_addr;
    // a register read of 0xffffffff means the device isn't decoding (no response);
    // anything else means the mmio window is live. (satoru)
    uint32_t probe = *(volatile uint32_t*)d->mmio;
    d->mmio_mapped = (probe != 0xFFFFFFFFu);
    return d->mmio_mapped;
}

bool WifiDev::Probe() {
    if (probed) return dev.present;
    probed = true;
    dev = WifiDevice{};
    dev.present = false;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t id = pci_cfg_read((uint8_t)bus, slot, func, 0x00);
                uint16_t vendor = id & 0xFFFF;
                uint16_t device = (id >> 16) & 0xFFFF;
                if (vendor == 0xFFFF || vendor == 0) { if (func == 0) break; else continue; }
                uint32_t clsreg = pci_cfg_read((uint8_t)bus, slot, func, 0x08);
                uint8_t  cls = (clsreg >> 24) & 0xFF;
                uint8_t  sub = (clsreg >> 16) & 0xFF;
                if (!is_wireless(vendor, device, cls, sub)) continue;

                // found a wireless nic - capture it. (satoru)
                dev.present = true;
                dev.bus = (uint8_t)bus; dev.slot = slot; dev.func = func;
                dev.vendor = vendor; dev.device = device;
                dev.irq = (uint8_t)(pci_cfg_read((uint8_t)bus, slot, func, 0x3C) & 0xFF);
                identify(&dev);

                // enable memory space + bus mastering so the device can dma. (satoru)
                uint32_t cmd = pci_cfg_read((uint8_t)bus, slot, func, 0x04);
                cmd |= (1u << 1) | (1u << 2);   // memory space + bus master
                pci_cfg_write((uint8_t)bus, slot, func, 0x04, cmd);

                map_bar(&dev);

                SerialLogger::Log("[wifi] found ");
                SerialLogger::Log(dev.model);
                SerialLogger::Log(" (");
                SerialLogger::LogHex(vendor); SerialLogger::Log(":"); SerialLogger::LogHex(device);
                SerialLogger::Log(") bar0=");
                SerialLogger::LogHex((uint32_t)(dev.bar0_phys >> 32));
                SerialLogger::Log(":");
                SerialLogger::LogHex((uint32_t)(dev.bar0_phys & 0xFFFFFFFF));
                SerialLogger::Log(dev.mmio_mapped ? " mmio=live" : " mmio=dead");
                SerialLogger::Log(dev.needs_firmware ? " (needs firmware)\r\n" : " (firmware-free)\r\n");
                return true;
            }
        }
    }
    SerialLogger::Log("[wifi] no wireless controller on the pci bus\r\n");
    return false;
}

bool WifiDev::Present() { if (!probed) Probe(); return dev.present; }
const WifiDevice* WifiDev::Info() { if (!probed) Probe(); return &dev; }

uint32_t WifiDev::RegRead(uint32_t off) {
    if (!dev.mmio_mapped || off + 4 > dev.bar0_size) return 0;
    return *(volatile uint32_t*)(dev.mmio + off);
}
void WifiDev::RegWrite(uint32_t off, uint32_t val) {
    if (!dev.mmio_mapped || off + 4 > dev.bar0_size) return;
    *(volatile uint32_t*)(dev.mmio + off) = val;
}
// end (satoru)
