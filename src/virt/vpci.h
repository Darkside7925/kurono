//  kurono os - virtual pci bus for guest vms
//  emulates pci configuration mechanism #1 (ports 0xcf8/0xcfc) and routes
//  mmio accesses to per-device callbacks for emulated bars.
//
//  used by the hypervisor to expose virtio devices (virtio-gpu, virtio-blk,
//  virtio-net, ...) to the alpine/debian linux guest.
//
//  design:
//    - up to MAX_VPCI_DEVS slots on bus 0, dev 0..n, func 0
//    - each device supplies a 256-byte standard config space buffer plus
//      callbacks for bar mmio and post-config-write notifications
//    - bars are allocated from a fixed mmio window
//        (VPCI_MMIO_BASE .. VPCI_MMIO_BASE + VPCI_MMIO_SIZE)
//      and mapped 1:1 into ept by the hypervisor at vm-create time
#pragma once
#include "../kernel/types.h"

// ----------------------------------------------------------------- constants
constexpr int      MAX_VPCI_DEVS    = 16;
constexpr uint64_t VPCI_MMIO_BASE   = 0xC0000000ULL;   // below 4 gb mmio hole
constexpr uint64_t VPCI_MMIO_SIZE   = 0x10000000ULL;   // 256 mb of bar space
constexpr int      VPCI_MAX_BARS    = 6;
constexpr int      VPCI_CFG_SIZE    = 256;             // standard pci cfg

// pci config offsets used externally
constexpr uint8_t PCI_VENDOR_ID    = 0x00;
constexpr uint8_t PCI_DEVICE_ID    = 0x02;
constexpr uint8_t PCI_COMMAND      = 0x04;
constexpr uint8_t PCI_STATUS       = 0x06;
constexpr uint8_t PCI_REVISION_ID  = 0x08;
constexpr uint8_t PCI_PROG_IF      = 0x09;
constexpr uint8_t PCI_SUBCLASS     = 0x0A;
constexpr uint8_t PCI_CLASS        = 0x0B;
constexpr uint8_t PCI_HEADER_TYPE  = 0x0E;
constexpr uint8_t PCI_BAR0         = 0x10;
constexpr uint8_t PCI_SUBSYS_VEND  = 0x2C;
constexpr uint8_t PCI_SUBSYS_ID    = 0x2E;
constexpr uint8_t PCI_CAP_PTR      = 0x34;
constexpr uint8_t PCI_INT_LINE     = 0x3C;
constexpr uint8_t PCI_INT_PIN      = 0x3D;

// command register bits
constexpr uint16_t PCI_CMD_IO      = 0x0001;
constexpr uint16_t PCI_CMD_MEM     = 0x0002;
constexpr uint16_t PCI_CMD_MASTER  = 0x0004;

struct VPCIDevice; // fwd

//  bar callbacks - invoked when the guest reads/writes within a bar.
//  offset is relative to the bar's base address.
typedef bool (*VPCIBarRead)(VPCIDevice* dev, int bar, uint32_t off,
                             uint8_t size, uint32_t& value);
typedef bool (*VPCIBarWrite)(VPCIDevice* dev, int bar, uint32_t off,
                              uint8_t size, uint32_t value);

//  invoked after a config-space write so devices can react
//  (e.g. virtio common cfg writes happen in cfg space when in legacy mode,
//   but for modern virtio we mostly need this for command register changes).
typedef void (*VPCICfgNotify)(VPCIDevice* dev, uint8_t off, uint8_t size,
                               uint32_t value);

struct VPCIBar {
    uint64_t base;          // assigned mmio base (0 = unassigned)
    uint32_t size;          // power-of-two size in bytes
    bool     is_mmio;       // false = io space (unused for virtio modern)
    bool     is_64bit;      // 64-bit memory bar (occupies two slots)
    bool     prefetch;      // prefetchable
};

struct VPCIDevice {
    bool     present;
    uint8_t  bus;
    uint8_t  dev;
    uint8_t  func;
    uint8_t  irq_line;      // legacy intx line (1-15)

    uint8_t  cfg[VPCI_CFG_SIZE];
    VPCIBar  bars[VPCI_MAX_BARS];

    VPCIBarRead   bar_read;
    VPCIBarWrite  bar_write;
    VPCICfgNotify cfg_notify;

    void* user;             // device-specific pointer (passed back implicitly via dev)
    const char* name;
};

//  vpci - virtual pci bus singleton
class VPCI {
public:
    static void Init();

    // register a new device. returns the slot index (>=0) or -1 on error.
    // caller must have populated cfg[] (vendor/device/class/etc), bars[]
    // (size + flags), and the callbacks. base addresses are assigned here.
    static int RegisterDevice(VPCIDevice* dev);

    // ports 0xcf8 / 0xcfc handler - returns true if handled
    static bool HandlePortIO(uint16_t port, bool is_out, uint8_t size,
                              uint32_t& value);

    // any mmio access in the vpci mmio window - returns true if a bar matches
    static bool HandleMMIO(uint64_t phys_addr, bool is_write, uint8_t size,
                            uint32_t& value);

    // returns the device that owns the given guest physical address, or null
    static VPCIDevice* FindDeviceForMMIO(uint64_t phys_addr, int* bar_out,
                                          uint32_t* off_out);

    // device count + enumeration helpers (for shell debug)
    static int  DeviceCount();
    static VPCIDevice* GetDevice(int slot);

    // helpers used by virtio devices to plant cfg-space values
    static void Cfg16(VPCIDevice* dev, uint8_t off, uint16_t v);
    static void Cfg32(VPCIDevice* dev, uint8_t off, uint32_t v);
    static void Cfg8 (VPCIDevice* dev, uint8_t off, uint8_t  v);

private:
    static VPCIDevice  devices[MAX_VPCI_DEVS];
    static int         device_count;
    static uint64_t    next_bar_base;     // bump allocator within VPCI_MMIO_*
    static uint32_t    cfg_address;        // last value written to 0xcf8

    static uint32_t ReadConfig(uint8_t bus, uint8_t dev, uint8_t func,
                                uint8_t off, uint8_t size);
    static void     WriteConfig(uint8_t bus, uint8_t dev, uint8_t func,
                                  uint8_t off, uint8_t size, uint32_t value);
};
