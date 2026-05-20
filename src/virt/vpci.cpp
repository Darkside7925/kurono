//  kurono os  -  virtual pci bus implementation
#include "vpci.h"
#include "../drivers/serial.h"
#include "../kernel/heap.h"

VPCIDevice VPCI::devices[MAX_VPCI_DEVS];
int        VPCI::device_count   = 0;
uint64_t   VPCI::next_bar_base  = VPCI_MMIO_BASE;
uint32_t   VPCI::cfg_address    = 0;

static inline uint32_t align_up_u64(uint64_t v, uint64_t a) {
    return (uint32_t)((v + (a - 1)) & ~(a - 1));
}

void VPCI::Init() {
    for (int i = 0; i < MAX_VPCI_DEVS; i++) {
        devices[i].present = false;
    }
    device_count   = 0;
    next_bar_base  = VPCI_MMIO_BASE;
    cfg_address    = 0;
    SerialLogger::Log("VPCI: virtual pci bus initialized (mmio window 0x");
    SerialLogger::LogHex((uint32_t)VPCI_MMIO_BASE);
    SerialLogger::Log(" - 0x");
    SerialLogger::LogHex((uint32_t)(VPCI_MMIO_BASE + VPCI_MMIO_SIZE));
    SerialLogger::Log(")\r\n");
}

void VPCI::Cfg16(VPCIDevice* dev, uint8_t off, uint16_t v) {
    dev->cfg[off]     = (uint8_t)(v & 0xFF);
    dev->cfg[off + 1] = (uint8_t)((v >> 8) & 0xFF);
}
void VPCI::Cfg32(VPCIDevice* dev, uint8_t off, uint32_t v) {
    dev->cfg[off]     = (uint8_t)(v & 0xFF);
    dev->cfg[off + 1] = (uint8_t)((v >> 8) & 0xFF);
    dev->cfg[off + 2] = (uint8_t)((v >> 16) & 0xFF);
    dev->cfg[off + 3] = (uint8_t)((v >> 24) & 0xFF);
}
void VPCI::Cfg8(VPCIDevice* dev, uint8_t off, uint8_t v) {
    dev->cfg[off] = v;
}

int VPCI::RegisterDevice(VPCIDevice* src) {
    if (device_count >= MAX_VPCI_DEVS) return -1;
    int slot = device_count++;
    VPCIDevice& d = devices[slot];
    d = *src;
    d.present = true;
    d.bus  = 0;
    d.dev  = (uint8_t)slot;
    d.func = 0;

    // assign mmio bases for each bar that has a non-zero size
    for (int i = 0; i < VPCI_MAX_BARS; i++) {
        VPCIBar& b = d.bars[i];
        if (b.size == 0) continue;
        if (!b.is_mmio) continue; // io bars unsupported
        // align to size (pci bar alignment requirement)
        uint64_t base = (next_bar_base + b.size - 1) & ~((uint64_t)b.size - 1);
        b.base = base;
        next_bar_base = base + b.size;

        // plant bar value into cfg space  -  memory bar, optionally 64-bit prefetch
        uint32_t bar_lo = (uint32_t)(base & 0xFFFFFFF0u);
        if (b.is_64bit) bar_lo |= 0x04;       // type = 64-bit
        if (b.prefetch) bar_lo |= 0x08;       // prefetchable
        Cfg32(&d, PCI_BAR0 + i * 4, bar_lo);
        if (b.is_64bit) {
            Cfg32(&d, PCI_BAR0 + (i + 1) * 4, (uint32_t)(base >> 32));
        }
    }

    // standard fields baseline
    if (d.cfg[PCI_HEADER_TYPE] == 0)  d.cfg[PCI_HEADER_TYPE] = 0x00; // type 0
    if (d.cfg[PCI_INT_PIN]    == 0)   d.cfg[PCI_INT_PIN]    = 0x01; // INTA#
    if (d.irq_line) d.cfg[PCI_INT_LINE] = d.irq_line;

    SerialLogger::Log("VPCI: registered '");
    SerialLogger::Log(d.name ? d.name : "?");
    SerialLogger::Log("' at 00:");
    SerialLogger::LogHex(d.dev);
    SerialLogger::Log(".0  vendor=");
    uint16_t vid = (uint16_t)d.cfg[PCI_VENDOR_ID] |
                   ((uint16_t)d.cfg[PCI_VENDOR_ID + 1] << 8);
    uint16_t did = (uint16_t)d.cfg[PCI_DEVICE_ID] |
                   ((uint16_t)d.cfg[PCI_DEVICE_ID + 1] << 8);
    SerialLogger::LogHex(vid);
    SerialLogger::Log(":");
    SerialLogger::LogHex(did);
    SerialLogger::Log("\r\n");
    return slot;
}

uint32_t VPCI::ReadConfig(uint8_t bus, uint8_t dev, uint8_t func,
                            uint8_t off, uint8_t size) {
    if (bus != 0) return 0xFFFFFFFFu;
    if (dev >= MAX_VPCI_DEVS) return 0xFFFFFFFFu;
    VPCIDevice& d = devices[dev];
    if (!d.present || func != 0) return 0xFFFFFFFFu;
    if ((int)off + size > VPCI_CFG_SIZE) return 0xFFFFFFFFu;

    uint32_t v = 0;
    for (int i = 0; i < size; i++) {
        v |= ((uint32_t)d.cfg[off + i]) << (i * 8);
    }
    return v;
}

void VPCI::WriteConfig(uint8_t bus, uint8_t dev, uint8_t func,
                          uint8_t off, uint8_t size, uint32_t value) {
    if (bus != 0) return;
    if (dev >= MAX_VPCI_DEVS) return;
    VPCIDevice& d = devices[dev];
    if (!d.present || func != 0) return;
    if ((int)off + size > VPCI_CFG_SIZE) return;

    // bar writes  -  handle bar size probing (write 0xFFFFFFFF, read back size mask)
    if (off >= PCI_BAR0 && off < PCI_BAR0 + 6 * 4) {
        int bar = (off - PCI_BAR0) / 4;
        VPCIBar& b = d.bars[bar];
        if (b.size != 0 && b.is_mmio) {
            if (size == 4 && value == 0xFFFFFFFFu) {
                // size probe  -  return ~(size-1) | type bits
                uint32_t mask = ~((uint32_t)b.size - 1u);
                if (b.is_64bit) mask |= 0x04;
                if (b.prefetch) mask |= 0x08;
                Cfg32(&d, off, mask);
                return;
            }
            // restoration of bar to its real assignment  -  accept low bits ignored
            uint32_t real_lo = (uint32_t)(b.base & 0xFFFFFFF0u);
            if (b.is_64bit) real_lo |= 0x04;
            if (b.prefetch) real_lo |= 0x08;
            Cfg32(&d, off, real_lo);
            return;
        }
    }

    // standard write into cfg buffer
    for (int i = 0; i < size; i++) {
        d.cfg[off + i] = (uint8_t)((value >> (i * 8)) & 0xFF);
    }

    if (d.cfg_notify) {
        d.cfg_notify(&d, off, size, value);
    }
}

bool VPCI::HandlePortIO(uint16_t port, bool is_out, uint8_t size,
                          uint32_t& value) {
    if (port == 0xCF8) {
        if (is_out) cfg_address = value;
        else        value = cfg_address;
        return true;
    }
    if (port >= 0xCFC && port <= 0xCFF) {
        // enable bit must be set
        if (!(cfg_address & 0x80000000u)) {
            if (!is_out) value = 0xFFFFFFFFu;
            return true;
        }
        uint8_t bus  = (cfg_address >> 16) & 0xFF;
        uint8_t dev  = (cfg_address >> 11) & 0x1F;
        uint8_t func = (cfg_address >>  8) & 0x07;
        uint8_t off  = (uint8_t)((cfg_address & 0xFC) | (port - 0xCFC));
        if (is_out) WriteConfig(bus, dev, func, off, size, value);
        else        value = ReadConfig(bus, dev, func, off, size);
        return true;
    }
    return false;
}

VPCIDevice* VPCI::FindDeviceForMMIO(uint64_t phys, int* bar_out,
                                      uint32_t* off_out) {
    for (int i = 0; i < device_count; i++) {
        VPCIDevice& d = devices[i];
        if (!d.present) continue;
        // command register must have memory enable
        uint16_t cmd = (uint16_t)d.cfg[PCI_COMMAND] |
                       ((uint16_t)d.cfg[PCI_COMMAND + 1] << 8);
        if (!(cmd & PCI_CMD_MEM)) continue;
        for (int b = 0; b < VPCI_MAX_BARS; b++) {
            const VPCIBar& bar = d.bars[b];
            if (bar.size == 0 || !bar.is_mmio || bar.base == 0) continue;
            if (phys >= bar.base && phys < bar.base + bar.size) {
                if (bar_out) *bar_out = b;
                if (off_out) *off_out = (uint32_t)(phys - bar.base);
                return &d;
            }
        }
    }
    return nullptr;
}

bool VPCI::HandleMMIO(uint64_t phys, bool is_write, uint8_t size,
                       uint32_t& value) {
    if (phys < VPCI_MMIO_BASE || phys >= VPCI_MMIO_BASE + VPCI_MMIO_SIZE) {
        return false;
    }
    int bar = -1;
    uint32_t off = 0;
    VPCIDevice* d = FindDeviceForMMIO(phys, &bar, &off);
    if (!d) {
        if (!is_write) value = 0xFFFFFFFFu;
        return true; // claimed the address but unmapped; swallow access
    }
    if (is_write) {
        if (d->bar_write) return d->bar_write(d, bar, off, size, value);
    } else {
        value = 0;
        if (d->bar_read) return d->bar_read(d, bar, off, size, value);
        value = 0xFFFFFFFFu;
    }
    return true;
}

int VPCI::DeviceCount() { return device_count; }
VPCIDevice* VPCI::GetDevice(int slot) {
    if (slot < 0 || slot >= device_count) return nullptr;
    return &devices[slot];
}

// silence "unused" without violating freestanding c++
namespace { __attribute__((unused)) auto _vpci_keep = &align_up_u64; }

