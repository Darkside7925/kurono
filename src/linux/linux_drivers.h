#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Linux Driver Compatibility Framework
//  Implements Linux-style driver model for native hardware drivers
//  that are compatible with the Linux subsystem.
//
//  Driver Categories:
//    - Character drivers (serial, tty, input, framebuffer)
//    - Block drivers (disk, loop)
//    - Network drivers (E1000, virtio-net)
//    - Platform drivers (PCI, USB, ACPI)
//    - Filesystem drivers (ext4, tmpfs, procfs, sysfs)
//    - GPU/DRM drivers (framebuffer, modesetting)
//    - Sound drivers (ALSA/SB16)
//    - Input drivers (evdev, PS/2, USB HID)
//    - Bus drivers (PCI, USB, I2C, SPI)
//    - Power management drivers (ACPI)
// ═══════════════════════════════════════════════════════════════════════════

#include "../kernel/types.h"

// ─── Driver categories ──────────────────────────────────────────────────

#define LDRV_MAX_DRIVERS    64
#define LDRV_MAX_NAME       32
#define LDRV_MAX_DESC       64

enum LinuxDriverCategory {
    LDRV_CAT_CHAR = 0,
    LDRV_CAT_BLOCK,
    LDRV_CAT_NET,
    LDRV_CAT_PLATFORM,
    LDRV_CAT_FS,
    LDRV_CAT_GPU,
    LDRV_CAT_SOUND,
    LDRV_CAT_INPUT,
    LDRV_CAT_BUS,
    LDRV_CAT_POWER,
    LDRV_CAT_OTHER
};

enum LinuxDriverState {
    LDRV_UNLOADED = 0,
    LDRV_LOADED,
    LDRV_PROBING,
    LDRV_BOUND,       // Bound to device
    LDRV_ACTIVE,
    LDRV_ERROR
};

// ─── Driver registration ────────────────────────────────────────────────

// File operations (for char/block devices)
struct LinuxFileOps {
    int (*open)(int flags);
    int (*close)();
    int (*read)(void* buf, uint32_t count);
    int (*write)(const void* buf, uint32_t count);
    int (*ioctl)(uint32_t cmd, uint64_t arg);
    int (*mmap)(uint64_t offset, uint32_t length, void** addr);
    int64_t (*lseek)(int64_t offset, int whence);
    int (*poll)(int events);    // Check if ready for I/O
};

// Network device operations
struct LinuxNetDevOps {
    int  (*open)();
    void (*close)();
    int  (*start_xmit)(const uint8_t* data, uint16_t len);
    void (*set_multicast)();
    int  (*set_mac)(const uint8_t mac[6]);
    void (*tx_timeout)();
    void (*get_stats)(uint32_t* rx_pkts, uint32_t* tx_pkts, uint32_t* rx_bytes, uint32_t* tx_bytes);
};

// PCI device ID matching
struct LinuxPCIDeviceID {
    uint16_t vendor;
    uint16_t device;
    uint16_t subvendor;
    uint16_t subdevice;
    uint32_t class_code;     // PCI class
    uint32_t class_mask;
};

// Driver structure (mirrors struct device_driver / struct pci_driver)
struct LinuxDriver {
    char                 name[LDRV_MAX_NAME];
    char                 description[LDRV_MAX_DESC];
    char                 version[16];
    char                 author[32];
    char                 license[16];      // "GPL", "BSD", "Proprietary"
    
    LinuxDriverCategory  category;
    LinuxDriverState     state;
    
    // Module info
    int                  major;            // Device major number (char/block)
    int                  minor_start;
    int                  minor_count;
    
    // PCI matching (for PCI drivers)
    LinuxPCIDeviceID     pci_ids[8];
    int                  pci_id_count;
    
    // Core callbacks
    int  (*probe)(void* dev);              // Device detection
    void (*remove)(void* dev);             // Device removal
    int  (*suspend)(void* dev);            // Power suspend
    int  (*resume)(void* dev);             // Power resume
    
    // File operations (for char/block)
    LinuxFileOps*        fops;
    
    // Net device ops (for network drivers)
    LinuxNetDevOps*      netdev_ops;
    
    // Refcount
    int                  ref_count;
    
    // Linked to hardware
    bool                 bound;            // Bound to a real device
    uint32_t             hw_base;          // Hardware base address (if any)
};

// ═══════════════════════════════════════════════════════════════════════════
//  LinuxDriverFramework — manages all drivers
// ═══════════════════════════════════════════════════════════════════════════

class LinuxDriverFramework {
public:
    static void Init();
    
    // Driver registration (like module_init / register_chrdev)
    static int  RegisterDriver(const LinuxDriver* drv);
    static void UnregisterDriver(const char* name);
    
    // Driver lookup
    static LinuxDriver* FindDriver(const char* name);
    static LinuxDriver* FindByMajor(int major);
    static LinuxDriver* FindByPCI(uint16_t vendor, uint16_t device);
    
    // Driver lifecycle
    static int  LoadDriver(const char* name);
    static void UnloadDriver(const char* name);
    static int  ProbeAll();              // Probe all loaded drivers
    
    // Get info
    static LinuxDriver* GetDrivers();
    static int GetDriverCount();
    static int GetActiveCount();
    
    // Register all built-in drivers
    static void RegisterBuiltins();
    
    // Shell commands
    static void RegisterShellCommands(void* shell);
    static int  cmd_lsmod(void* sh, int argc, const char** argv, char* out, int mx);
    static int  cmd_modprobe(void* sh, int argc, const char** argv, char* out, int mx);
    static int  cmd_modinfo(void* sh, int argc, const char** argv, char* out, int mx);
    static int  cmd_lspci(void* sh, int argc, const char** argv, char* out, int mx);
    static int  cmd_lsblk(void* sh, int argc, const char** argv, char* out, int mx);
    static int  cmd_lsusb(void* sh, int argc, const char** argv, char* out, int mx);
    
    // Dump status
    static void DumpDrivers(char* out, int max_out);

private:
    static LinuxDriver drivers[LDRV_MAX_DRIVERS];
    static int driver_count;
    
    // Built-in driver registration helpers
    static void RegisterCharDrivers();
    static void RegisterBlockDrivers();
    static void RegisterNetDrivers();
    static void RegisterFSDrivers();
    static void RegisterGPUDrivers();
    static void RegisterSoundDrivers();
    static void RegisterInputDrivers();
    static void RegisterBusDrivers();
    static void RegisterPowerDrivers();
};

// ─── procfs / sysfs helpers ─────────────────────────────────────────────

class ProcFS {
public:
    static void Init();
    static void Populate();
    static void UpdateCPUInfo();
    static void UpdateMemInfo();
    static void UpdateUptime();
    static void UpdateLoadAvg();
    static void UpdateNetDev();
    static void UpdateMounts();
    static void UpdateVersion();
};

class SysFS {
public:
    static void Init();
    static void Populate();
    static void RegisterPCIDevice(uint16_t vendor, uint16_t device, 
                                   uint8_t bus, uint8_t slot, uint8_t func);
    static void RegisterNetDevice(const char* name);
    static void RegisterBlockDevice(const char* name);
    static void RegisterInputDevice(const char* name);
};
