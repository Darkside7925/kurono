//  kurono os  -  linux driver compatibility framework  -  implementation
//  full driver model with built-in drivers for all detected hardware

#include "linux_drivers.h"
#include "linux_devices.h"
#include "../kernel/types.h"
#include "../kernel/time.h"
#include "../kernel/heap.h"
#include "../shell/shell.h"
#include "../drivers/serial.h"
#include "../drivers/graphics.h"
#include "../drivers/e1000.h"
#include "../drivers/audio.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../drivers/bga.h"
#include "../drivers/timer.h"
#include "../drivers/nvidia_gpu.h"
#include "../drivers/amd_gpu.h"
#include "../drivers/hda.h"
#include "../drivers/cpu_detect.h"
#include "../fs/kvfs.h"
#include "../hal/hal.h"
#include "../virt/vmm.h"
#include "../virt/hypervisor.h"
#include "../net/network.h"
#include "../system/logging.h"

LinuxDriver LinuxDriverFramework::drivers[LDRV_MAX_DRIVERS];
int LinuxDriverFramework::driver_count = 0;

static void lf_scpy(char* d, const char* s, int mx) {
    int i = 0; while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; } d[i] = 0;
}
static bool lf_seq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; } return *a == 0 && *b == 0;
}
static int lf_slen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static int lf_a(char* b, int p, int m, const char* s) { while (*s && p < m-1) b[p++] = *s++; b[p] = 0; return p; }
static int lf_ac(char* b, int p, int m, char c) { if (p < m-1) { b[p++] = c; b[p] = 0; } return p; }
static int lf_ai(char* b, int p, int m, int v) {
    if (v < 0) { p = lf_ac(b, p, m, '-'); v = -v; }
    if (v == 0) return lf_ac(b, p, m, '0');
    char t[12]; int ti = 0;
    while (v > 0) { t[ti++] = '0' + (v % 10); v /= 10; }
    while (ti > 0) p = lf_ac(b, p, m, t[--ti]);
    return p;
}

static const char* lf_category_name(LinuxDriverCategory category) {
    switch (category) {
        case LDRV_CAT_CHAR:     return "char";
        case LDRV_CAT_BLOCK:    return "block";
        case LDRV_CAT_NET:      return "net";
        case LDRV_CAT_PLATFORM: return "platform";
        case LDRV_CAT_FS:       return "filesystem";
        case LDRV_CAT_GPU:      return "gpu";
        case LDRV_CAT_SOUND:    return "sound";
        case LDRV_CAT_INPUT:    return "input";
        case LDRV_CAT_BUS:      return "bus";
        case LDRV_CAT_POWER:    return "power";
        default:                return "other";
    }
}

static const char* lf_state_name(LinuxDriverState state) {
    switch (state) {
        case LDRV_UNLOADED: return "unloaded";
        case LDRV_LOADED:   return "loaded";
        case LDRV_PROBING:  return "probing";
        case LDRV_BOUND:    return "bound";
        case LDRV_ACTIVE:   return "active";
        case LDRV_ERROR:    return "error";
        default:            return "unknown";
    }
}

static void lf_publish_driver_inventory() {
    KVFS::Mkdirs("/kurono/drivers");
    LinuxDriver* current_drivers = LinuxDriverFramework::GetDrivers();
    int current_driver_count = LinuxDriverFramework::GetDriverCount();

    char readme[512];
    int rp = 0;
    rp = lf_a(readme, rp, sizeof(readme), "Kurono driver manifests\n");
    rp = lf_a(readme, rp, sizeof(readme), "Each *.drv file mirrors one kernel driver registered in the LinuxDriverFramework.\n");
    rp = lf_a(readme, rp, sizeof(readme), "See index.txt for the current summary.\n");
    KVFS::WriteString("/kurono/drivers/README.txt", readme);

    char index[4096];
    int ip = 0;
    ip = lf_a(index, ip, sizeof(index), "Kurono drivers\n\n");
    for (int i = 0; i < current_driver_count && ip < (int)sizeof(index) - 128; i++) {
        LinuxDriver* drv = &current_drivers[i];
        ip = lf_a(index, ip, sizeof(index), drv->name);
        ip = lf_a(index, ip, sizeof(index), " [");
        ip = lf_a(index, ip, sizeof(index), lf_state_name(drv->state));
        ip = lf_a(index, ip, sizeof(index), "] ");
        ip = lf_a(index, ip, sizeof(index), lf_category_name(drv->category));
        ip = lf_a(index, ip, sizeof(index), " - ");
        ip = lf_a(index, ip, sizeof(index), drv->description[0] ? drv->description : "(no description)");
        ip = lf_ac(index, ip, sizeof(index), '\n');

        char path[128];
        int pp = 0;
        pp = lf_a(path, pp, sizeof(path), "/kurono/drivers/");
        pp = lf_a(path, pp, sizeof(path), drv->name);
        pp = lf_a(path, pp, sizeof(path), ".drv");

        char info[1024];
        int p = 0;
        p = lf_a(info, p, sizeof(info), "name=");
        p = lf_a(info, p, sizeof(info), drv->name);
        p = lf_ac(info, p, sizeof(info), '\n');
        p = lf_a(info, p, sizeof(info), "description=");
        p = lf_a(info, p, sizeof(info), drv->description[0] ? drv->description : "(none)");
        p = lf_ac(info, p, sizeof(info), '\n');
        p = lf_a(info, p, sizeof(info), "category=");
        p = lf_a(info, p, sizeof(info), lf_category_name(drv->category));
        p = lf_ac(info, p, sizeof(info), '\n');
        p = lf_a(info, p, sizeof(info), "state=");
        p = lf_a(info, p, sizeof(info), lf_state_name(drv->state));
        p = lf_ac(info, p, sizeof(info), '\n');
        p = lf_a(info, p, sizeof(info), "bound=");
        p = lf_a(info, p, sizeof(info), drv->bound ? "true" : "false");
        p = lf_ac(info, p, sizeof(info), '\n');
        p = lf_a(info, p, sizeof(info), "version=");
        p = lf_a(info, p, sizeof(info), drv->version[0] ? drv->version : "unknown");
        p = lf_ac(info, p, sizeof(info), '\n');
        p = lf_a(info, p, sizeof(info), "author=");
        p = lf_a(info, p, sizeof(info), drv->author[0] ? drv->author : "unknown");
        p = lf_ac(info, p, sizeof(info), '\n');
        p = lf_a(info, p, sizeof(info), "license=");
        p = lf_a(info, p, sizeof(info), drv->license[0] ? drv->license : "unknown");
        p = lf_ac(info, p, sizeof(info), '\n');
        p = lf_a(info, p, sizeof(info), "major=");
        p = lf_ai(info, p, sizeof(info), drv->major);
        p = lf_ac(info, p, sizeof(info), '\n');
        p = lf_a(info, p, sizeof(info), "minor_start=");
        p = lf_ai(info, p, sizeof(info), drv->minor_start);
        p = lf_ac(info, p, sizeof(info), '\n');
        p = lf_a(info, p, sizeof(info), "minor_count=");
        p = lf_ai(info, p, sizeof(info), drv->minor_count);
        p = lf_ac(info, p, sizeof(info), '\n');
        p = lf_a(info, p, sizeof(info), "ref_count=");
        p = lf_ai(info, p, sizeof(info), drv->ref_count);
        p = lf_ac(info, p, sizeof(info), '\n');
        p = lf_a(info, p, sizeof(info), "pci_ids=");
        p = lf_ai(info, p, sizeof(info), drv->pci_id_count);
        p = lf_ac(info, p, sizeof(info), '\n');

        for (int pci = 0; pci < drv->pci_id_count && p < (int)sizeof(info) - 64; pci++) {
            p = lf_a(info, p, sizeof(info), "pci[");
            p = lf_ai(info, p, sizeof(info), pci);
            p = lf_a(info, p, sizeof(info), "] vendor=0x");
            p = lf_ai(info, p, sizeof(info), drv->pci_ids[pci].vendor);
            p = lf_a(info, p, sizeof(info), " device=0x");
            p = lf_ai(info, p, sizeof(info), drv->pci_ids[pci].device);
            p = lf_ac(info, p, sizeof(info), '\n');
        }

        KVFS::WriteString(path, info);
    }

    KVFS::WriteString("/kurono/drivers/index.txt", index);
}

//  init

void LinuxDriverFramework::Init() {
    memset(drivers, 0, sizeof(drivers));
    driver_count = 0;
    
    RegisterBuiltins();
    ProbeAll();
    
    // init virtual filesystems
    ProcFS::Init();
    SysFS::Init();
    lf_publish_driver_inventory();
    RuntimeLog::LogSystem("drivers", "published driver manifests to /kurono/drivers");
    
    SerialLogger::Log("[LinuxDrivers] Framework initialized (");
    SerialLogger::LogDec(driver_count);
    SerialLogger::Log(" drivers, ");
    SerialLogger::LogDec(GetActiveCount());
    SerialLogger::Log(" active)\r\n");
}

//  driver registration

int LinuxDriverFramework::RegisterDriver(const LinuxDriver* drv) {
    if (!drv || driver_count >= LDRV_MAX_DRIVERS) return -1;
    
    LinuxDriver* d = &drivers[driver_count];
    *d = *drv;
    d->state = LDRV_LOADED;
    d->ref_count = 0;
    int idx = driver_count++;
    lf_publish_driver_inventory();
    return idx;
}

void LinuxDriverFramework::UnregisterDriver(const char* name) {
    for (int i = 0; i < driver_count; i++) {
        if (lf_seq(drivers[i].name, name)) {
            if (drivers[i].remove) drivers[i].remove(nullptr);
            drivers[i].state = LDRV_UNLOADED;
            drivers[i].bound = false;
            lf_publish_driver_inventory();
            return;
        }
    }
}

LinuxDriver* LinuxDriverFramework::FindDriver(const char* name) {
    for (int i = 0; i < driver_count; i++) {
        if (lf_seq(drivers[i].name, name)) return &drivers[i];
    }
    return nullptr;
}

LinuxDriver* LinuxDriverFramework::FindByMajor(int major) {
    for (int i = 0; i < driver_count; i++) {
        if (drivers[i].major == major && drivers[i].state >= LDRV_LOADED) return &drivers[i];
    }
    return nullptr;
}

LinuxDriver* LinuxDriverFramework::FindByPCI(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < driver_count; i++) {
        for (int j = 0; j < drivers[i].pci_id_count; j++) {
            if (drivers[i].pci_ids[j].vendor == vendor &&
                drivers[i].pci_ids[j].device == device) return &drivers[i];
        }
    }
    return nullptr;
}

int LinuxDriverFramework::LoadDriver(const char* name) {
    LinuxDriver* d = FindDriver(name);
    if (!d || d->state == LDRV_UNLOADED) return -1;
    if (d->probe) {
        d->state = LDRV_PROBING;
        int ret = d->probe(nullptr);
        if (ret == 0) {
            d->state = LDRV_ACTIVE;
            d->bound = true;
            lf_publish_driver_inventory();
            return 0;
        } else {
            d->state = LDRV_ERROR;
            lf_publish_driver_inventory();
            return ret;
        }
    }
    d->state = LDRV_ACTIVE;
    lf_publish_driver_inventory();
    return 0;
}

void LinuxDriverFramework::UnloadDriver(const char* name) {
    LinuxDriver* d = FindDriver(name);
    if (!d) return;
    if (d->remove) d->remove(nullptr);
    d->state = LDRV_LOADED;
    d->bound = false;
    lf_publish_driver_inventory();
}

int LinuxDriverFramework::ProbeAll() {
    int activated = 0;
    for (int i = 0; i < driver_count; i++) {
        if (drivers[i].state == LDRV_LOADED && drivers[i].probe) {
            drivers[i].state = LDRV_PROBING;
            int ret = drivers[i].probe(nullptr);
            if (ret == 0) {
                drivers[i].state = LDRV_ACTIVE;
                drivers[i].bound = true;
                activated++;
            } else {
                drivers[i].state = LDRV_LOADED;
            }
        }
    }
    lf_publish_driver_inventory();
    return activated;
}

LinuxDriver* LinuxDriverFramework::GetDrivers() { return drivers; }
int LinuxDriverFramework::GetDriverCount() { return driver_count; }

int LinuxDriverFramework::GetActiveCount() {
    int n = 0;
    for (int i = 0; i < driver_count; i++)
        if (drivers[i].state == LDRV_ACTIVE) n++;
    return n;
}

//  built-in driver registration

// probe callbacks for built-in drivers
static int probe_e1000(void* dev) { (void)dev; return E1000::IsDetected() ? 0 : -1; }
static int probe_bga(void* dev) { (void)dev; return BGA::IsAvailable() ? 0 : -1; }
static int probe_sb16(void* dev) { (void)dev; return Audio::IsAvailable() ? 0 : -1; }
static int probe_ps2kbd(void* dev) { (void)dev; return 0; /* PS/2 always present */ }
static int probe_ps2mouse(void* dev) { (void)dev; return 0; }
static int probe_pit(void* dev) { (void)dev; return 0; /* PIT always present */ }
static int probe_serial(void* dev) { (void)dev; return 0; }
static int probe_vga(void* dev) { (void)dev; return BGA::IsAvailable() ? 0 : -1; }
static int probe_kvfs(void* dev) { (void)dev; return 0; }
static int probe_ext4(void* dev) { (void)dev; return 0; }
static int probe_virtio(void* dev) { (void)dev; return -1; /* Not yet implemented */ }
static int probe_kvm(void* dev) { (void)dev; return VMM::IsSupported() ? 0 : -1; }
static int probe_acpi(void* dev) { (void)dev; return 0; }
static int probe_nvidia(void* dev) { (void)dev; return NvidiaGPU::IsDetected() ? 0 : -1; }
static int probe_amdgpu(void* dev) { (void)dev; return AmdGPU::IsAvailable() ? 0 : -1; }
static int probe_hda(void* dev) { (void)dev; return HDAudio::IsDetected() ? 0 : -1; }

void LinuxDriverFramework::RegisterBuiltins() {
    RegisterNetDrivers();
    RegisterCharDrivers();
    RegisterBlockDrivers();
    RegisterGPUDrivers();
    RegisterSoundDrivers();
    RegisterInputDrivers();
    RegisterBusDrivers();
    RegisterFSDrivers();
    RegisterPowerDrivers();
}

void LinuxDriverFramework::RegisterNetDrivers() {
    // e1000 ethernet driver
    LinuxDriver drv = {};
    lf_scpy(drv.name, "e1000", LDRV_MAX_NAME);
    lf_scpy(drv.description, "Intel 82540EM Gigabit Ethernet", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.author, "Kurono", 32);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_NET;
    drv.pci_ids[0] = {0x8086, 0x100E, 0, 0, 0x020000, 0xFFFF00};
    drv.pci_ids[1] = {0x8086, 0x100F, 0, 0, 0x020000, 0xFFFF00};
    drv.pci_ids[2] = {0x8086, 0x10D3, 0, 0, 0x020000, 0xFFFF00};
    drv.pci_id_count = 3;
    drv.probe = probe_e1000;
    RegisterDriver(&drv);
    
    // virtio-net
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "virtio_net", LDRV_MAX_NAME);
    lf_scpy(drv.description, "Virtio network device", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_NET;
    drv.pci_ids[0] = {0x1AF4, 0x1000, 0, 0, 0x020000, 0xFFFF00};
    drv.pci_id_count = 1;
    drv.probe = probe_virtio;
    RegisterDriver(&drv);
}

void LinuxDriverFramework::RegisterCharDrivers() {
    // serial (ttys0)
    LinuxDriver drv = {};
    lf_scpy(drv.name, "serial8250", LDRV_MAX_NAME);
    lf_scpy(drv.description, "8250/16550A UART serial driver", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_CHAR;
    drv.major = 4;
    drv.minor_start = 64;
    drv.minor_count = 4;
    drv.probe = probe_serial;
    RegisterDriver(&drv);
    
    // tty
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "tty", LDRV_MAX_NAME);
    lf_scpy(drv.description, "TTY subsystem", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_CHAR;
    drv.major = 5;
    drv.minor_start = 0;
    drv.minor_count = 16;
    drv.probe = probe_serial;
    RegisterDriver(&drv);
    
    // pit timer
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "i8253", LDRV_MAX_NAME);
    lf_scpy(drv.description, "Intel 8253/8254 PIT Timer", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_CHAR;
    drv.probe = probe_pit;
    RegisterDriver(&drv);
}

void LinuxDriverFramework::RegisterBlockDrivers() {
    // virtual disk
    LinuxDriver drv = {};
    lf_scpy(drv.name, "sd_mod", LDRV_MAX_NAME);
    lf_scpy(drv.description, "SCSI disk driver", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_BLOCK;
    drv.major = 8;
    drv.minor_start = 0;
    drv.minor_count = 16;
    drv.probe = probe_kvfs;
    RegisterDriver(&drv);
    
    // loop device
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "loop", LDRV_MAX_NAME);
    lf_scpy(drv.description, "Loopback block device", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_BLOCK;
    drv.major = 7;
    drv.probe = probe_kvfs;
    RegisterDriver(&drv);
}

void LinuxDriverFramework::RegisterGPUDrivers() {
    LinuxDriver drv = {};
    lf_scpy(drv.name, "nvidia", LDRV_MAX_NAME);
    lf_scpy(drv.description, "NVIDIA GPU driver (Blackwell/Ada/Ampere)", LDRV_MAX_DESC);
    lf_scpy(drv.version, "560.35", 16);
    lf_scpy(drv.author, "NVIDIA", 32);
    lf_scpy(drv.license, "Proprietary", 16);
    drv.category = LDRV_CAT_GPU;
    drv.major = 195;
    // rtx 5090/5080/5070, rtx 4090/4080/4070, rtx 3090/3080
    drv.pci_ids[0] = {0x10DE, 0x2B84, 0, 0, 0x030000, 0xFFFF00}; // rtx 5090
    drv.pci_ids[1] = {0x10DE, 0x2B80, 0, 0, 0x030000, 0xFFFF00}; // rtx 5080
    drv.pci_ids[2] = {0x10DE, 0x2684, 0, 0, 0x030000, 0xFFFF00}; // rtx 4090
    drv.pci_ids[3] = {0x10DE, 0x2704, 0, 0, 0x030000, 0xFFFF00}; // rtx 4080
    drv.pci_ids[4] = {0x10DE, 0x2206, 0, 0, 0x030000, 0xFFFF00}; // rtx 3090
    drv.pci_ids[5] = {0x10DE, 0x2204, 0, 0, 0x030000, 0xFFFF00}; // rtx 3080
    drv.pci_id_count = 6;
    drv.probe = probe_nvidia;
    RegisterDriver(&drv);

    // nvidia nvenc/nvdec (video encode/decode engine)
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "nvidia_uvm", LDRV_MAX_NAME);
    lf_scpy(drv.description, "NVIDIA Unified Virtual Memory + NVENC/NVDEC", LDRV_MAX_DESC);
    lf_scpy(drv.version, "560.35", 16);
    lf_scpy(drv.license, "Proprietary", 16);
    drv.category = LDRV_CAT_GPU;
    drv.major = 195;
    drv.probe = probe_nvidia;
    RegisterDriver(&drv);

    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "amdgpu", LDRV_MAX_NAME);
    lf_scpy(drv.description, "AMD Radeon GPU driver (RDNA 3.5/3/2/1, GCN)", LDRV_MAX_DESC);
    lf_scpy(drv.version, "6.7.0", 16);
    lf_scpy(drv.author, "AMD", 32);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_GPU;
    drv.major = 226;
    // rx 9070 xt, 7900 xtx, 7900 xt, 7800 xt, 6900 xt, 6800 xt
    drv.pci_ids[0] = {0x1002, 0x7480, 0, 0, 0x030000, 0xFFFF00}; // rx 9070 xt
    drv.pci_ids[1] = {0x1002, 0x744C, 0, 0, 0x030000, 0xFFFF00}; // rx 7900 xtx
    drv.pci_ids[2] = {0x1002, 0x7470, 0, 0, 0x030000, 0xFFFF00}; // rx 7800 xt
    drv.pci_ids[3] = {0x1002, 0x73BF, 0, 0, 0x030000, 0xFFFF00}; // rx 6900 xt
    drv.pci_ids[4] = {0x1002, 0x73A5, 0, 0, 0x030000, 0xFFFF00}; // rx 6800 xt
    drv.pci_ids[5] = {0x1002, 0x731F, 0, 0, 0x030000, 0xFFFF00}; // rx 5700 xt
    drv.pci_id_count = 6;
    drv.probe = probe_amdgpu;
    RegisterDriver(&drv);

    // amd vcn (video core next  -  hardware encode/decode)
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "amdgpu_vcn", LDRV_MAX_NAME);
    lf_scpy(drv.description, "AMD VCN video encode/decode (H.264/HEVC/AV1)", LDRV_MAX_DESC);
    lf_scpy(drv.version, "6.7.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_GPU;
    drv.probe = probe_amdgpu;
    RegisterDriver(&drv);

    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "i915", LDRV_MAX_NAME);
    lf_scpy(drv.description, "Intel HD/UHD/Iris/Xe/Arc GPU driver", LDRV_MAX_DESC);
    lf_scpy(drv.version, "6.7.0", 16);
    lf_scpy(drv.author, "Intel", 32);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_GPU;
    drv.major = 226;
    // intel igpus  -  detect by vendor + class since device ids vary per gen
    drv.pci_ids[0] = {0x8086, 0x56A5, 0, 0, 0x030000, 0xFFFF00}; // arc a770
    drv.pci_ids[1] = {0x8086, 0x56A0, 0, 0, 0x030000, 0xFFFF00}; // arc a750
    drv.pci_ids[2] = {0x8086, 0xA780, 0, 0, 0x030000, 0xFFFF00}; // rpl uhd 770
    drv.pci_ids[3] = {0x8086, 0x4680, 0, 0, 0x030000, 0xFFFF00}; // adl uhd 770
    drv.pci_ids[4] = {0x8086, 0x9A49, 0, 0, 0x030000, 0xFFFF00}; // tgl xe
    drv.pci_ids[5] = {0x8086, 0x3E92, 0, 0, 0x030000, 0xFFFF00}; // cfl uhd 630
    drv.pci_id_count = 6;
    // intel igpu probe: pci class 0x03 + vendor 0x8086
    drv.probe = probe_bga; // reuse bga probe  -  real intel gpu uses same fb
    RegisterDriver(&drv);

    // intel qsv (quick sync video  -  hardware encode/decode)
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "i915_qsv", LDRV_MAX_NAME);
    lf_scpy(drv.description, "Intel Quick Sync Video encode/decode", LDRV_MAX_DESC);
    lf_scpy(drv.version, "6.7.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_GPU;
    drv.probe = probe_bga;
    RegisterDriver(&drv);

    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "bochs_drm", LDRV_MAX_NAME);
    lf_scpy(drv.description, "Bochs/BGA DRM framebuffer driver", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_GPU;
    drv.major = 29;
    drv.pci_ids[0] = {0x1234, 0x1111, 0, 0, 0x030000, 0xFFFF00};
    drv.pci_id_count = 1;
    drv.probe = probe_bga;
    RegisterDriver(&drv);

    // drm kms
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "drm_kms", LDRV_MAX_NAME);
    lf_scpy(drv.description, "DRM Kernel Mode Setting (universal)", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_GPU;
    drv.major = 226;
    drv.probe = probe_bga;
    RegisterDriver(&drv);

    // framebuffer console
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "fbcon", LDRV_MAX_NAME);
    lf_scpy(drv.description, "Framebuffer console driver", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_GPU;
    drv.major = 29;
    drv.probe = probe_bga;
    RegisterDriver(&drv);
}

void LinuxDriverFramework::RegisterSoundDrivers() {
    LinuxDriver drv = {};
    lf_scpy(drv.name, "snd_hda_intel", LDRV_MAX_NAME);
    lf_scpy(drv.description, "Intel HD Audio / HDA controller (all vendors)", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_SOUND;
    // hda controllers from intel, amd/ati, nvidia
    drv.pci_ids[0] = {0x8086, 0xA171, 0, 0, 0x040300, 0xFFFF00}; // intel
    drv.pci_ids[1] = {0x1002, 0xAB38, 0, 0, 0x040300, 0xFFFF00}; // amd/ati
    drv.pci_ids[2] = {0x10DE, 0x10F0, 0, 0, 0x040300, 0xFFFF00}; // nvidia
    drv.pci_id_count = 3;
    drv.probe = probe_hda;
    RegisterDriver(&drv);

    // hda codec (realtek / conexant / etc.)
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "snd_hda_codec", LDRV_MAX_NAME);
    lf_scpy(drv.description, "HDA codec driver (Realtek/Conexant/IDT)", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_SOUND;
    drv.probe = probe_hda;
    RegisterDriver(&drv);

    // sb16 / legacy alsa (fallback)
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "snd_sb16", LDRV_MAX_NAME);
    lf_scpy(drv.description, "Sound Blaster 16 (ALSA legacy)", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_SOUND;
    drv.probe = probe_sb16;
    RegisterDriver(&drv);

    // pcm core
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "snd_pcm", LDRV_MAX_NAME);
    lf_scpy(drv.description, "ALSA PCM core", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_SOUND;
    drv.probe = probe_sb16;
    RegisterDriver(&drv);

    // mixer
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "snd_mixer", LDRV_MAX_NAME);
    lf_scpy(drv.description, "ALSA Mixer core", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_SOUND;
    drv.probe = probe_sb16;
    RegisterDriver(&drv);
}

void LinuxDriverFramework::RegisterInputDrivers() {
    // ps/2 keyboard
    LinuxDriver drv = {};
    lf_scpy(drv.name, "atkbd", LDRV_MAX_NAME);
    lf_scpy(drv.description, "AT/PS2 keyboard driver", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_INPUT;
    drv.major = 13;
    drv.probe = probe_ps2kbd;
    RegisterDriver(&drv);
    
    // ps/2 mouse
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "psmouse", LDRV_MAX_NAME);
    lf_scpy(drv.description, "PS/2 mouse driver", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_INPUT;
    drv.major = 13;
    drv.probe = probe_ps2mouse;
    RegisterDriver(&drv);
    
    // evdev
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "evdev", LDRV_MAX_NAME);
    lf_scpy(drv.description, "Event device input driver", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_INPUT;
    drv.probe = probe_ps2kbd;
    RegisterDriver(&drv);
}

void LinuxDriverFramework::RegisterBusDrivers() {
    // pci bus driver
    LinuxDriver drv = {};
    lf_scpy(drv.name, "pci_bus", LDRV_MAX_NAME);
    lf_scpy(drv.description, "PCI bus driver", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_BUS;
    drv.probe = probe_acpi;
    RegisterDriver(&drv);
    
    // i8042 (ps/2 controller)
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "i8042", LDRV_MAX_NAME);
    lf_scpy(drv.description, "i8042 PS/2 controller", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_BUS;
    drv.probe = probe_ps2kbd;
    RegisterDriver(&drv);
}

void LinuxDriverFramework::RegisterFSDrivers() {
    // kvfs (maps to ext4 interface)
    LinuxDriver drv = {};
    lf_scpy(drv.name, "ext4", LDRV_MAX_NAME);
    lf_scpy(drv.description, "ext4 filesystem", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_FS;
    drv.probe = probe_ext4;
    RegisterDriver(&drv);
    
    // tmpfs
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "tmpfs", LDRV_MAX_NAME);
    lf_scpy(drv.description, "Temporary filesystem", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_FS;
    drv.probe = probe_kvfs;
    RegisterDriver(&drv);
    
    // procfs
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "proc", LDRV_MAX_NAME);
    lf_scpy(drv.description, "Process filesystem", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_FS;
    drv.probe = probe_kvfs;
    RegisterDriver(&drv);
    
    // sysfs
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "sysfs", LDRV_MAX_NAME);
    lf_scpy(drv.description, "System filesystem", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_FS;
    drv.probe = probe_kvfs;
    RegisterDriver(&drv);
    
    // devtmpfs
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "devtmpfs", LDRV_MAX_NAME);
    lf_scpy(drv.description, "Device temporary filesystem", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_FS;
    drv.probe = probe_kvfs;
    RegisterDriver(&drv);
}

void LinuxDriverFramework::RegisterPowerDrivers() {
    // acpi
    LinuxDriver drv = {};
    lf_scpy(drv.name, "acpi", LDRV_MAX_NAME);
    lf_scpy(drv.description, "ACPI power management", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_POWER;
    drv.probe = probe_acpi;
    RegisterDriver(&drv);
    
    // kvm (if virtualization available)
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "kvm", LDRV_MAX_NAME);
    lf_scpy(drv.description, "Kernel-based Virtual Machine", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_OTHER;
    drv.probe = probe_kvm;
    RegisterDriver(&drv);
    
    // kvm-amd
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "kvm_amd", LDRV_MAX_NAME);
    lf_scpy(drv.description, "KVM AMD SVM extension", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_OTHER;
    drv.probe = probe_kvm;
    RegisterDriver(&drv);
    
    // kvm-intel
    memset(&drv, 0, sizeof(drv));
    lf_scpy(drv.name, "kvm_intel", LDRV_MAX_NAME);
    lf_scpy(drv.description, "KVM Intel VMX extension", LDRV_MAX_DESC);
    lf_scpy(drv.version, "1.0.0", 16);
    lf_scpy(drv.license, "GPL", 16);
    drv.category = LDRV_CAT_OTHER;
    drv.probe = probe_kvm;
    RegisterDriver(&drv);
}

//  shell commands (lsmod, modprobe, modinfo, lspci, lsblk, lsusb)

void LinuxDriverFramework::RegisterShellCommands(void* shell_ptr) {
    KuronoShell* sh = (KuronoShell*)shell_ptr;
    sh->RegisterCommand("lsmod",    "List loaded kernel modules",     ENV_LINUX, "drivers", (ShellCmdHandler)cmd_lsmod);
    sh->RegisterCommand("modprobe", "Load/unload kernel module",      ENV_LINUX, "drivers", (ShellCmdHandler)cmd_modprobe);
    sh->RegisterCommand("modinfo",  "Show kernel module info",        ENV_LINUX, "drivers", (ShellCmdHandler)cmd_modinfo);
    sh->RegisterCommand("lspci",    "List PCI devices",               ENV_LINUX, "hardware", (ShellCmdHandler)cmd_lspci);
    sh->RegisterCommand("lsblk",    "List block devices",             ENV_LINUX, "hardware", (ShellCmdHandler)cmd_lsblk);
    sh->RegisterCommand("lsusb",    "List USB devices",               ENV_LINUX, "hardware", (ShellCmdHandler)cmd_lsusb);
}

int LinuxDriverFramework::cmd_lsmod(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = lf_a(out, p, mx, "Module                  Size  Used by\n");
    for (int i = 0; i < driver_count; i++) {
        if (drivers[i].state < LDRV_LOADED) continue;
        p = lf_a(out, p, mx, drivers[i].name);
        int nl = lf_slen(drivers[i].name);
        for (int j = nl; j < 24; j++) p = lf_ac(out, p, mx, ' ');
        
        // simulated size based on category
        int sz = 16384;
        if (drivers[i].category == LDRV_CAT_NET) sz = 131072;
        else if (drivers[i].category == LDRV_CAT_GPU) sz = 262144;
        else if (drivers[i].category == LDRV_CAT_SOUND) sz = 65536;
        else if (drivers[i].category == LDRV_CAT_FS) sz = 524288;
        p = lf_ai(out, p, mx, sz);
        p = lf_a(out, p, mx, "  ");
        p = lf_ai(out, p, mx, drivers[i].ref_count);
        if (drivers[i].state == LDRV_ACTIVE) p = lf_a(out, p, mx, "  [loaded]");
        p = lf_ac(out, p, mx, '\n');
    }
    return p;
}

int LinuxDriverFramework::cmd_modprobe(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return lf_a(out, 0, mx, "Usage: modprobe <module_name>\n");
    
    LinuxDriver* d = FindDriver(argv[1]);
    if (!d) {
        int p = lf_a(out, 0, mx, "modprobe: FATAL: Module ");
        p = lf_a(out, p, mx, argv[1]);
        p = lf_a(out, p, mx, " not found.\n");
        return p;
    }
    
    int ret = LoadDriver(argv[1]);
    if (ret == 0) {
        int p = lf_a(out, 0, mx, "modprobe: ");
        p = lf_a(out, p, mx, argv[1]);
        p = lf_a(out, p, mx, " loaded successfully\n");
        return p;
    }
    
    int p = lf_a(out, 0, mx, "modprobe: ");
    p = lf_a(out, p, mx, argv[1]);
    p = lf_a(out, p, mx, " failed to probe hardware\n");
    return p;
}

int LinuxDriverFramework::cmd_modinfo(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return lf_a(out, 0, mx, "Usage: modinfo <module_name>\n");
    
    LinuxDriver* d = FindDriver(argv[1]);
    if (!d) {
        int p = lf_a(out, 0, mx, "modinfo: ERROR: Module ");
        p = lf_a(out, p, mx, argv[1]);
        p = lf_a(out, p, mx, " not found.\n");
        return p;
    }
    
    int p = 0;
    p = lf_a(out, p, mx, "filename:       /lib/modules/kurono/");
    p = lf_a(out, p, mx, d->name);
    p = lf_a(out, p, mx, ".ko\n");
    p = lf_a(out, p, mx, "description:    ");
    p = lf_a(out, p, mx, d->description);
    p = lf_ac(out, p, mx, '\n');
    p = lf_a(out, p, mx, "version:        ");
    p = lf_a(out, p, mx, d->version[0] ? d->version : "1.0.0");
    p = lf_ac(out, p, mx, '\n');
    p = lf_a(out, p, mx, "author:         ");
    p = lf_a(out, p, mx, d->author[0] ? d->author : "Kurono");
    p = lf_ac(out, p, mx, '\n');
    p = lf_a(out, p, mx, "license:        ");
    p = lf_a(out, p, mx, d->license[0] ? d->license : "GPL");
    p = lf_ac(out, p, mx, '\n');
    
    const char* cat_names[] = {"char", "block", "net", "platform", "fs", "gpu", "sound", "input", "bus", "power", "other"};
    p = lf_a(out, p, mx, "category:       ");
    p = lf_a(out, p, mx, cat_names[d->category]);
    p = lf_ac(out, p, mx, '\n');
    
    const char* state_names[] = {"unloaded", "loaded", "probing", "bound", "active", "error"};
    p = lf_a(out, p, mx, "state:          ");
    p = lf_a(out, p, mx, state_names[d->state]);
    p = lf_ac(out, p, mx, '\n');
    
    if (d->pci_id_count > 0) {
        p = lf_a(out, p, mx, "pci_ids:        ");
        char hex[16];
        for (int i = 0; i < d->pci_id_count; i++) {
            // format as xxxx:xxxx
            p = lf_a(out, p, mx, "0x");
            int vi = 0;
            hex[vi++] = "0123456789ABCDEF"[(d->pci_ids[i].vendor >> 12) & 0xF];
            hex[vi++] = "0123456789ABCDEF"[(d->pci_ids[i].vendor >> 8) & 0xF];
            hex[vi++] = "0123456789ABCDEF"[(d->pci_ids[i].vendor >> 4) & 0xF];
            hex[vi++] = "0123456789ABCDEF"[d->pci_ids[i].vendor & 0xF];
            hex[vi] = 0;
            p = lf_a(out, p, mx, hex);
            p = lf_ac(out, p, mx, ':');
            p = lf_a(out, p, mx, "0x");
            vi = 0;
            hex[vi++] = "0123456789ABCDEF"[(d->pci_ids[i].device >> 12) & 0xF];
            hex[vi++] = "0123456789ABCDEF"[(d->pci_ids[i].device >> 8) & 0xF];
            hex[vi++] = "0123456789ABCDEF"[(d->pci_ids[i].device >> 4) & 0xF];
            hex[vi++] = "0123456789ABCDEF"[d->pci_ids[i].device & 0xF];
            hex[vi] = 0;
            p = lf_a(out, p, mx, hex);
            p = lf_ac(out, p, mx, ' ');
        }
        p = lf_ac(out, p, mx, '\n');
    }
    return p;
}

int LinuxDriverFramework::cmd_lspci(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    
    // scan actual pci bus
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            // read vendor/device
            uint32_t addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | 0;
            __asm__ __volatile__("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
            uint32_t id;
            __asm__ __volatile__("inl %1, %0" : "=a"(id) : "Nd"((uint16_t)0xCFC));
            
            uint16_t vendor = id & 0xFFFF;
            uint16_t device = (id >> 16) & 0xFFFF;
            if (vendor == 0xFFFF || vendor == 0) continue;
            
            // read class code
            addr = (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | 0x08;
            __asm__ __volatile__("outl %0, %1" : : "a"(addr), "Nd"((uint16_t)0xCF8));
            uint32_t class_reg;
            __asm__ __volatile__("inl %1, %0" : "=a"(class_reg) : "Nd"((uint16_t)0xCFC));
            uint8_t class_code = (class_reg >> 24) & 0xFF;
            uint8_t subclass = (class_reg >> 16) & 0xFF;
            
            // format: bb:ss.f class: vendor device [driver]
            char hex[8];
            // bus
            hex[0] = "0123456789abcdef"[(bus >> 4) & 0xF];
            hex[1] = "0123456789abcdef"[bus & 0xF];
            hex[2] = 0;
            p = lf_a(out, p, mx, hex);
            p = lf_ac(out, p, mx, ':');
            hex[0] = "0123456789abcdef"[(slot >> 4) & 0xF];
            hex[1] = "0123456789abcdef"[slot & 0xF];
            p = lf_a(out, p, mx, hex);
            p = lf_a(out, p, mx, ".0 ");
            
            // class name
            const char* cls = "Unknown";
            if (class_code == 0x01) cls = "Mass storage controller";
            else if (class_code == 0x02) cls = "Network controller";
            else if (class_code == 0x03) cls = "Display controller";
            else if (class_code == 0x04) cls = "Multimedia controller";
            else if (class_code == 0x06) cls = "Bridge";
            else if (class_code == 0x0C) cls = "Serial bus controller";
            p = lf_a(out, p, mx, cls);
            p = lf_a(out, p, mx, ": ");
            
            // vendor name
            const char* vname = "Unknown";
            if (vendor == 0x8086) vname = "Intel Corporation";
            else if (vendor == 0x1234) vname = "Bochs/QEMU";
            else if (vendor == 0x1AF4) vname = "Red Hat (virtio)";
            else if (vendor == 0x10DE) vname = "NVIDIA Corporation";
            else if (vendor == 0x1022) vname = "Advanced Micro Devices";
            p = lf_a(out, p, mx, vname);
            p = lf_a(out, p, mx, " [");
            
            // hex vendor:device
            hex[0] = "0123456789abcdef"[(vendor >> 12) & 0xF];
            hex[1] = "0123456789abcdef"[(vendor >> 8) & 0xF];
            hex[2] = "0123456789abcdef"[(vendor >> 4) & 0xF];
            hex[3] = "0123456789abcdef"[vendor & 0xF];
            hex[4] = 0;
            p = lf_a(out, p, mx, hex);
            p = lf_ac(out, p, mx, ':');
            hex[0] = "0123456789abcdef"[(device >> 12) & 0xF];
            hex[1] = "0123456789abcdef"[(device >> 8) & 0xF];
            hex[2] = "0123456789abcdef"[(device >> 4) & 0xF];
            hex[3] = "0123456789abcdef"[device & 0xF];
            p = lf_a(out, p, mx, hex);
            p = lf_a(out, p, mx, "]");
            
            // check if we have a driver bound
            LinuxDriver* drv = FindByPCI(vendor, device);
            if (drv && drv->state == LDRV_ACTIVE) {
                p = lf_a(out, p, mx, " (");
                p = lf_a(out, p, mx, drv->name);
                p = lf_ac(out, p, mx, ')');
            }
            p = lf_ac(out, p, mx, '\n');
        }
    }
    
    if (p == 0) p = lf_a(out, p, mx, "No PCI devices found\n");
    return p;
}

int LinuxDriverFramework::cmd_lsblk(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = lf_a(out, p, mx, "NAME   MAJ:MIN RM   SIZE RO TYPE MOUNTPOINT\n");
    p = lf_a(out, p, mx, "sda      8:0    0   352M  0 disk\n");
    p = lf_a(out, p, mx, "├─sda1   8:1    0    64M  0 part /\n");
    p = lf_a(out, p, mx, "├─sda2   8:2    0   192M  0 part /linux\n");
    p = lf_a(out, p, mx, "├─sda3   8:3    0    64M  0 part /shared\n");
    p = lf_a(out, p, mx, "└─sda4   8:4    0    32M  0 part [SWAP]\n");
    return p;
}

int LinuxDriverFramework::cmd_lsusb(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = lf_a(out, p, mx, "Bus 001 Device 001: ID 1d6b:0002 Linux Foundation 2.0 root hub\n");
    p = lf_a(out, p, mx, "Bus 001 Device 002: ID 0627:0001 QEMU USB Tablet\n");
    return p;
}

void LinuxDriverFramework::DumpDrivers(char* out, int max_out) {
    int p = 0;
    p = lf_a(out, p, max_out, "=== Linux Driver Framework ===\n");
    p = lf_a(out, p, max_out, "Drivers: ");
    p = lf_ai(out, p, max_out, driver_count);
    p = lf_a(out, p, max_out, " loaded, ");
    p = lf_ai(out, p, max_out, GetActiveCount());
    p = lf_a(out, p, max_out, " active\n\n");
    
    const char* cat_names[] = {"CHAR", "BLOCK", "NET", "PLATFORM", "FS", "GPU", "SOUND", "INPUT", "BUS", "POWER", "OTHER"};
    for (int i = 0; i < driver_count; i++) {
        p = lf_a(out, p, max_out, "[");
        p = lf_a(out, p, max_out, cat_names[drivers[i].category]);
        p = lf_a(out, p, max_out, "] ");
        p = lf_a(out, p, max_out, drivers[i].name);
        p = lf_a(out, p, max_out, " - ");
        p = lf_a(out, p, max_out, drivers[i].description);
        p = lf_a(out, p, max_out, " [");
        const char* state_names[] = {"unloaded", "loaded", "probing", "bound", "ACTIVE", "ERROR"};
        p = lf_a(out, p, max_out, state_names[drivers[i].state]);
        p = lf_a(out, p, max_out, "]\n");
    }
}

//  procfs  -  virtual /proc filesystem

void ProcFS::Init() {
    Populate();
    SerialLogger::Log("[ProcFS] Initialized\r\n");
}

void ProcFS::Populate() {
    UpdateVersion();
    UpdateCPUInfo();
    UpdateMemInfo();
    UpdateUptime();
    UpdateLoadAvg();
    UpdateNetDev();
    UpdateMounts();
}

void ProcFS::UpdateVersion() {
    KVFS::WriteString("/proc/version", "Linux version 6.8.0-kurono (kurono@kurono-os) "
                      "(x86_64-elf-gcc 14.1) #1 SMP PREEMPT_DYNAMIC\n");
}

void ProcFS::UpdateCPUInfo() {
    CpuInfo cpu = CPUDetect::GetInfo();
    const char* vendor = cpu.vendor_string[0] ? cpu.vendor_string : "unknown";
    const char* brand = cpu.brand_string[0] ? cpu.brand_string : vendor;
    int cpu_mhz = cpu.frequency.base_mhz > 0 ? cpu.frequency.base_mhz : 3600;
    int siblings = cpu.topology.logical_cores > 0 ? cpu.topology.logical_cores : 1;
    int cores = cpu.topology.physical_cores > 0 ? cpu.topology.physical_cores : siblings;
    int cache_kb = 0;
    for (int i = 0; i < cpu.num_caches; i++) {
        if (cpu.cache[i].size_kb > cache_kb) cache_kb = cpu.cache[i].size_kb;
    }
    if (cache_kb <= 0) cache_kb = 8192;

    char buf[512];
    int p = 0;
    p = lf_a(buf, p, 512, "processor\t: 0\n");
    p = lf_a(buf, p, 512, "vendor_id\t: ");
    p = lf_a(buf, p, 512, vendor);
    p = lf_a(buf, p, 512, "\nmodel name\t: ");
    p = lf_a(buf, p, 512, brand);
    p = lf_a(buf, p, 512, "\ncpu MHz\t\t: ");
    p = lf_ai(buf, p, 512, cpu_mhz);
    p = lf_a(buf, p, 512, ".000\n");
    p = lf_a(buf, p, 512, "cache size\t: ");
    p = lf_ai(buf, p, 512, cache_kb);
    p = lf_a(buf, p, 512, " KB\n");
    p = lf_a(buf, p, 512, "physical id\t: 0\n");
    p = lf_a(buf, p, 512, "siblings\t: ");
    p = lf_ai(buf, p, 512, siblings);
    p = lf_a(buf, p, 512, "\n");
    p = lf_a(buf, p, 512, "cpu cores\t: ");
    p = lf_ai(buf, p, 512, cores);
    p = lf_a(buf, p, 512, "\n");
    p = lf_a(buf, p, 512, "flags\t\t: fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat "
                          "pse36 clflush mmx fxsr sse sse2 ht syscall nx lm constant_tsc rep_good nopl "
                          "cpuid tsc_known_freq pni ssse3 cx16 sse4_1 sse4_2 x2apic popcnt aes xsave "
                          "avx hypervisor lahf_lm svm\n");
    p = lf_a(buf, p, 512, "bogomips\t: 7200.00\n");
    
    KVFS::WriteString("/proc/cpuinfo", buf);
}

void ProcFS::UpdateMemInfo() {
    // 10gb ram configured
    char buf[512];
    int p = 0;
    p = lf_a(buf, p, 512, "MemTotal:       10485760 kB\n");
    p = lf_a(buf, p, 512, "MemFree:         8388608 kB\n");
    p = lf_a(buf, p, 512, "MemAvailable:    9437184 kB\n");
    p = lf_a(buf, p, 512, "Buffers:          262144 kB\n");
    p = lf_a(buf, p, 512, "Cached:           786432 kB\n");
    p = lf_a(buf, p, 512, "SwapTotal:        32768 kB\n");
    p = lf_a(buf, p, 512, "SwapFree:         32768 kB\n");
    KVFS::WriteString("/proc/meminfo", buf);
}

void ProcFS::UpdateUptime() {
    uint32_t ms = Timer::GetRealMs();
    uint32_t secs = ms / 1000;
    char buf[64];
    int p = 0;
    p = lf_ai(buf, p, 64, secs);
    p = lf_a(buf, p, 64, ".");
    p = lf_ai(buf, p, 64, (ms % 1000) / 10);
    p = lf_a(buf, p, 64, " ");
    p = lf_ai(buf, p, 64, secs);  // idle time ≈ uptime
    p = lf_a(buf, p, 64, ".00\n");
    KVFS::WriteString("/proc/uptime", buf);
}

void ProcFS::UpdateLoadAvg() {
    KVFS::WriteString("/proc/loadavg", "0.12 0.08 0.05 1/42 128\n");
}

void ProcFS::UpdateNetDev() {
    char buf[512];
    int p = 0;
    p = lf_a(buf, p, 512, "Inter-|   Receive                                                |  Transmit\n");
    p = lf_a(buf, p, 512, " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");
    
    NetworkInterface* ifaces = Network::GetInterfaces();
    int count = Network::GetInterfaceCount();
    for (int i = 0; i < count; i++) {
        p = lf_a(buf, p, 512, "    ");
        p = lf_a(buf, p, 512, ifaces[i].name);
        p = lf_a(buf, p, 512, ":");
        int nl = lf_slen(ifaces[i].name);
        for (int j = nl; j < 6; j++) p = lf_ac(buf, p, 512, ' ');
        // rx
        p = lf_ai(buf, p, 512, ifaces[i].rx_bytes);
        p = lf_a(buf, p, 512, "    ");
        p = lf_ai(buf, p, 512, ifaces[i].rx_packets);
        p = lf_a(buf, p, 512, "    0    0    0     0          0         0 ");
        // tx
        p = lf_ai(buf, p, 512, ifaces[i].tx_bytes);
        p = lf_a(buf, p, 512, "    ");
        p = lf_ai(buf, p, 512, ifaces[i].tx_packets);
        p = lf_a(buf, p, 512, "    0    0    0     0       0          0\n");
    }
    KVFS::WriteString("/proc/net/dev", buf);
}

void ProcFS::UpdateMounts() {
    KVFS::WriteString("/proc/mounts",
        "/dev/sda1 / kvfs rw,relatime 0 0\n"
        "/dev/sda2 /linux ext4 rw,relatime 0 0\n"
        "/dev/sda3 /shared ext4 rw,relatime 0 0\n"
        "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n"
        "sysfs /sys sysfs rw,nosuid,nodev,noexec,relatime 0 0\n"
        "devtmpfs /dev devtmpfs rw,nosuid,size=5242880k,nr_inodes=1310720,mode=755 0 0\n"
        "tmpfs /tmp tmpfs rw,nosuid,nodev 0 0\n"
        "tmpfs /run tmpfs rw,nosuid,nodev,mode=755 0 0\n");
}

//  sysfs  -  virtual /sys filesystem

void SysFS::Init() {
    Populate();
    SerialLogger::Log("[SysFS] Initialized\r\n");
}

void SysFS::Populate() {
    // create /sys tree
    KVFS::Mkdirs("/sys/class/net");
    KVFS::Mkdirs("/sys/class/block");
    KVFS::Mkdirs("/sys/class/input");
    KVFS::Mkdirs("/sys/class/drm");
    KVFS::Mkdirs("/sys/class/sound");
    KVFS::Mkdirs("/sys/bus/pci/devices");
    KVFS::Mkdirs("/sys/bus/usb/devices");
    KVFS::Mkdirs("/sys/devices/system/cpu/cpu0");
    KVFS::Mkdirs("/sys/kernel");
    
    // cpu info
    KVFS::WriteString("/sys/devices/system/cpu/cpu0/online", "1");
    KVFS::WriteString("/sys/devices/system/cpu/present", "0-3");
    KVFS::WriteString("/sys/devices/system/cpu/online", "0-3");
    
    // network devices
    RegisterNetDevice("lo");
    RegisterNetDevice("eth0");
    RegisterNetDevice("wlan0");
    
    // block devices
    RegisterBlockDevice("sda");
    
    // input devices
    RegisterInputDevice("keyboard");
    RegisterInputDevice("mouse");
    
    // kernel info
    KVFS::WriteString("/sys/kernel/hostname", "kurono");
    KVFS::WriteString("/sys/kernel/ostype", "Linux");
    KVFS::WriteString("/sys/kernel/osrelease", "6.8.0-kurono");
    KVFS::WriteString("/sys/kernel/version", "#1 SMP PREEMPT_DYNAMIC");
}

void SysFS::RegisterPCIDevice(uint16_t vendor, uint16_t device,
                               uint8_t bus, uint8_t slot, uint8_t func) {
    char path[128];
    int p = 0;
    p = lf_a(path, p, 128, "/sys/bus/pci/devices/0000:");
    char hex[4];
    hex[0] = "0123456789abcdef"[(bus >> 4) & 0xF];
    hex[1] = "0123456789abcdef"[bus & 0xF];
    hex[2] = 0;
    p = lf_a(path, p, 128, hex);
    p = lf_ac(path, p, 128, ':');
    hex[0] = "0123456789abcdef"[(slot >> 4) & 0xF];
    hex[1] = "0123456789abcdef"[slot & 0xF];
    p = lf_a(path, p, 128, hex);
    p = lf_ac(path, p, 128, '.');
    p = lf_ai(path, p, 128, func);
    
    KVFS::Mkdirs(path);
    
    char vendor_str[8], device_str[8];
    int vi = 0;
    vendor_str[vi++] = '0'; vendor_str[vi++] = 'x';
    vendor_str[vi++] = "0123456789abcdef"[(vendor >> 12) & 0xF];
    vendor_str[vi++] = "0123456789abcdef"[(vendor >> 8) & 0xF];
    vendor_str[vi++] = "0123456789abcdef"[(vendor >> 4) & 0xF];
    vendor_str[vi++] = "0123456789abcdef"[vendor & 0xF];
    vendor_str[vi] = 0;
    
    vi = 0;
    device_str[vi++] = '0'; device_str[vi++] = 'x';
    device_str[vi++] = "0123456789abcdef"[(device >> 12) & 0xF];
    device_str[vi++] = "0123456789abcdef"[(device >> 8) & 0xF];
    device_str[vi++] = "0123456789abcdef"[(device >> 4) & 0xF];
    device_str[vi++] = "0123456789abcdef"[device & 0xF];
    device_str[vi] = 0;
    
    char full_path[160];
    int fp;
    
    fp = 0; fp = lf_a(full_path, fp, 160, path); fp = lf_a(full_path, fp, 160, "/vendor");
    KVFS::WriteString(full_path, vendor_str);
    
    fp = 0; fp = lf_a(full_path, fp, 160, path); fp = lf_a(full_path, fp, 160, "/device");
    KVFS::WriteString(full_path, device_str);
}

void SysFS::RegisterNetDevice(const char* name) {
    char path[128];
    int p = 0;
    p = lf_a(path, p, 128, "/sys/class/net/");
    p = lf_a(path, p, 128, name);
    KVFS::Mkdirs(path);
    
    char full[160];
    int fp;
    
    fp = 0; fp = lf_a(full, fp, 160, path); fp = lf_a(full, fp, 160, "/operstate");
    KVFS::WriteString(full, "up");
    
    fp = 0; fp = lf_a(full, fp, 160, path); fp = lf_a(full, fp, 160, "/mtu");
    KVFS::WriteString(full, "1500");
    
    fp = 0; fp = lf_a(full, fp, 160, path); fp = lf_a(full, fp, 160, "/type");
    KVFS::WriteString(full, "1");  // ethernet
}

void SysFS::RegisterBlockDevice(const char* name) {
    char path[128];
    int p = 0;
    p = lf_a(path, p, 128, "/sys/class/block/");
    p = lf_a(path, p, 128, name);
    KVFS::Mkdirs(path);
    
    char full[160];
    int fp;
    
    fp = 0; fp = lf_a(full, fp, 160, path); fp = lf_a(full, fp, 160, "/size");
    KVFS::WriteString(full, "720896");  // 352mb in 512-byte sectors
}

void SysFS::RegisterInputDevice(const char* name) {
    char path[128];
    int p = 0;
    p = lf_a(path, p, 128, "/sys/class/input/");
    p = lf_a(path, p, 128, name);
    KVFS::Mkdirs(path);
}
