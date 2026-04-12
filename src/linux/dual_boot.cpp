//  kurono os  -  dual-boot / dual-run manager  -  implementation

#include "dual_boot.h"
#include "kls.h"
#include "linux_kernel.h"
#include "linux_init.h"
#include "linux_devices.h"
#include "linux_signals.h"
#include "shared_mount.h"
#include "user_bridge.h"
#include "ext4.h"
#include "../kernel/heap.h"
#include "../kernel/time.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../shell/shell.h"
#include "../virt/hypervisor.h"

BootMode         DualBootManager::current_mode = BOOT_INTEGRATED;
DiskPartition    DualBootManager::partitions[DUALBOOT_MAX_PARTITIONS];
int              DualBootManager::partition_count = 0;
GRUBConfig       DualBootManager::grub_config;
SharedDataConfig DualBootManager::shared_config;
bool             DualBootManager::linux_partition_mounted = false;

int DualBootManager::pa(char* out, int pos, int mx, const char* s) {
    while (*s && pos < mx - 1) out[pos++] = *s++;
    out[pos] = 0;
    return pos;
}

int DualBootManager::pd(char* out, int pos, int mx, int val) {
    if (val == 0) { if (pos < mx - 1) out[pos++] = '0'; out[pos] = 0; return pos; }
    char tmp[12]; int i = 0;
    bool neg = val < 0;
    if (neg) val = -val;
    while (val > 0) { tmp[i++] = '0' + (val % 10); val /= 10; }
    if (neg && pos < mx - 1) out[pos++] = '-';
    for (int j = i - 1; j >= 0 && pos < mx - 1; j--) out[pos++] = tmp[j];
    out[pos] = 0;
    return pos;
}

static bool db_seq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

static void db_scpy(char* d, const char* s, int mx) {
    int i = 0;
    while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

//  initialization

void DualBootManager::Init() {
    memset(partitions, 0, sizeof(partitions));
    memset(&grub_config, 0, sizeof(grub_config));
    partition_count = 0;
    current_mode = BOOT_INTEGRATED;
    linux_partition_mounted = false;

    // default shared data config  -  everything shared
    shared_config.share_home = true;
    shared_config.share_tmp = true;
    shared_config.share_var_log = true;
    shared_config.share_etc_overlay = true;
    shared_config.share_documents = true;
    shared_config.share_downloads = true;
    shared_config.share_desktop = true;
    shared_config.sync_users = true;
    shared_config.sync_groups = true;
    shared_config.sync_network = true;

    // default grub config
    grub_config.default_entry = 2;  // integrated mode
    grub_config.timeout = 5;
    grub_config.show_menu = true;
    db_scpy(grub_config.theme, "/boot/grub/themes/kurono/theme.txt",
            sizeof(grub_config.theme));
    db_scpy(grub_config.kurono_kernel, "/boot/kurono.elf",
            sizeof(grub_config.kurono_kernel));
    db_scpy(grub_config.kurono_args, "root=/dev/sda1 console=ttyS0",
            sizeof(grub_config.kurono_args));
    db_scpy(grub_config.linux_kernel, "/boot/vmlinuz-6.8.0-kurono",
            sizeof(grub_config.linux_kernel));
    db_scpy(grub_config.linux_initrd, "/boot/initrd.img-6.8.0-kurono",
            sizeof(grub_config.linux_initrd));
    db_scpy(grub_config.linux_args,
            "root=/dev/sda2 ro quiet splash vt.global_cursor_default=0",
            sizeof(grub_config.linux_args));
    db_scpy(grub_config.integrated_args,
            "root=/dev/sda1 kurono.linux=integrated kurono.share_users=1",
            sizeof(grub_config.integrated_args));

    // detect partitions
    DetectPartitions();

    SerialLogger::Log("[DualBoot] Manager initialized  -  mode: ");
    SerialLogger::Log(BootModeName(current_mode));
    SerialLogger::Log("\r\n");
}

//  boot mode

BootMode DualBootManager::GetBootMode() { return current_mode; }

void DualBootManager::SetBootMode(BootMode mode) {
    current_mode = mode;
    SerialLogger::Log("[DualBoot] Boot mode set to: ");
    SerialLogger::Log(BootModeName(mode));
    SerialLogger::Log("\r\n");
}

const char* DualBootManager::BootModeName(BootMode mode) {
    switch (mode) {
        case BOOT_INTEGRATED:       return "Integrated (Kurono + Linux)";
        case BOOT_DUAL_ACTIVE:      return "Dual Active";
        case BOOT_STANDALONE_LINUX:  return "Standalone Linux";
        case BOOT_STANDALONE_KURONO: return "Standalone Kurono";
        default:                     return "Unknown";
    }
}

bool DualBootManager::IsLinuxActive() {
    return current_mode == BOOT_INTEGRATED ||
           current_mode == BOOT_DUAL_ACTIVE ||
           current_mode == BOOT_STANDALONE_LINUX;
}

bool DualBootManager::IsKuronoActive() {
    return current_mode == BOOT_INTEGRATED ||
           current_mode == BOOT_DUAL_ACTIVE ||
           current_mode == BOOT_STANDALONE_KURONO;
}

bool DualBootManager::IsIntegrated() {
    return current_mode == BOOT_INTEGRATED;
}

//  partition management

void DualBootManager::DetectPartitions() {
    partition_count = 0;

    // partition 1: kurono os (64mb)
    DiskPartition* p1 = &partitions[partition_count++];
    db_scpy(p1->label, "Kurono OS", sizeof(p1->label));
    p1->type = PART_KURONO;
    p1->start_lba = 2048;
    p1->size_sectors = 131072;   // 64mb
    p1->size_bytes = 67108864;
    db_scpy(p1->fs_type, "kvfs", sizeof(p1->fs_type));
    db_scpy(p1->mount_point, "/", sizeof(p1->mount_point));
    p1->mounted = true;
    p1->bootable = true;

    // partition 2: linux ext4 (192mb)
    DiskPartition* p2 = &partitions[partition_count++];
    db_scpy(p2->label, "Kurono Linux", sizeof(p2->label));
    p2->type = PART_LINUX_EXT4;
    p2->start_lba = 133120;
    p2->size_sectors = 393216;   // 192mb
    p2->size_bytes = 201326592;
    db_scpy(p2->fs_type, "ext4", sizeof(p2->fs_type));
    db_scpy(p2->mount_point, "/linux", sizeof(p2->mount_point));
    p2->mounted = false;
    p2->bootable = true;

    // partition 3: shared data (64mb)
    DiskPartition* p3 = &partitions[partition_count++];
    db_scpy(p3->label, "Shared Data", sizeof(p3->label));
    p3->type = PART_SHARED_DATA;
    p3->start_lba = 526336;
    p3->size_sectors = 131072;   // 64mb
    p3->size_bytes = 67108864;
    db_scpy(p3->fs_type, "ext4", sizeof(p3->fs_type));
    db_scpy(p3->mount_point, "/shared", sizeof(p3->mount_point));
    p3->mounted = false;
    p3->bootable = false;

    // partition 4: linux swap (32mb)
    DiskPartition* p4 = &partitions[partition_count++];
    db_scpy(p4->label, "Linux Swap", sizeof(p4->label));
    p4->type = PART_LINUX_SWAP;
    p4->start_lba = 657408;
    p4->size_sectors = 65536;    // 32mb
    p4->size_bytes = 33554432;
    db_scpy(p4->fs_type, "swap", sizeof(p4->fs_type));
    db_scpy(p4->mount_point, "none", sizeof(p4->mount_point));
    p4->mounted = false;
    p4->bootable = false;
}

DiskPartition* DualBootManager::GetPartitions() { return partitions; }
int DualBootManager::GetPartitionCount() { return partition_count; }

DiskPartition* DualBootManager::FindPartition(const char* label) {
    for (int i = 0; i < partition_count; i++) {
        if (db_seq(partitions[i].label, label)) return &partitions[i];
    }
    return nullptr;
}

DiskPartition* DualBootManager::FindByMount(const char* mount) {
    for (int i = 0; i < partition_count; i++) {
        if (db_seq(partitions[i].mount_point, mount)) return &partitions[i];
    }
    return nullptr;
}

//  grub configuration

GRUBConfig* DualBootManager::GetGRUBConfig() { return &grub_config; }

void DualBootManager::GenerateGRUBConfig(char* out, int max_len) {
    int p = 0;

    // header
    p = pa(out, p, max_len, "# ═══════════════════════════════════════════════════════\n");
    p = pa(out, p, max_len, "#  Kurono OS  -  GRUB Configuration (auto-generated)\n");
    p = pa(out, p, max_len, "#  Dual-boot: Kurono OS + Kurono Linux\n");
    p = pa(out, p, max_len, "# ═══════════════════════════════════════════════════════\n\n");

    // defaults
    p = pa(out, p, max_len, "set default=");
    p = pd(out, p, max_len, grub_config.default_entry);
    p = pa(out, p, max_len, "\nset timeout=");
    p = pd(out, p, max_len, grub_config.timeout);
    p = pa(out, p, max_len, "\n\n");

    // theme
    if (grub_config.theme[0]) {
        p = pa(out, p, max_len, "# Theme\n");
        p = pa(out, p, max_len, "set theme=");
        p = pa(out, p, max_len, grub_config.theme);
        p = pa(out, p, max_len, "\n\n");
    }

    // entry 0: standalone kurono
    p = pa(out, p, max_len, "# ─── Kurono OS (standalone) ───────────────────────────\n");
    p = pa(out, p, max_len, "menuentry \"Kurono OS\" --class kurono {\n");
    p = pa(out, p, max_len, "    set root='(hd0,msdos1)'\n");
    p = pa(out, p, max_len, "    multiboot ");
    p = pa(out, p, max_len, grub_config.kurono_kernel);
    p = pa(out, p, max_len, " ");
    p = pa(out, p, max_len, grub_config.kurono_args);
    p = pa(out, p, max_len, "\n}\n\n");

    // entry 1: standalone linux
    p = pa(out, p, max_len, "# ─── Kurono Linux (standalone) ────────────────────────\n");
    p = pa(out, p, max_len, "menuentry \"Kurono Linux 6.8\" --class linux {\n");
    p = pa(out, p, max_len, "    set root='(hd0,msdos2)'\n");
    p = pa(out, p, max_len, "    linux ");
    p = pa(out, p, max_len, grub_config.linux_kernel);
    p = pa(out, p, max_len, " ");
    p = pa(out, p, max_len, grub_config.linux_args);
    p = pa(out, p, max_len, "\n");
    p = pa(out, p, max_len, "    initrd ");
    p = pa(out, p, max_len, grub_config.linux_initrd);
    p = pa(out, p, max_len, "\n}\n\n");

    // entry 2: integrated (default)
    p = pa(out, p, max_len, "# ─── Kurono OS + Linux Integrated (recommended) ──────\n");
    p = pa(out, p, max_len, "menuentry \"Kurono OS + Linux (Integrated)\" --class kurono-linux {\n");
    p = pa(out, p, max_len, "    set root='(hd0,msdos1)'\n");
    p = pa(out, p, max_len, "    multiboot ");
    p = pa(out, p, max_len, grub_config.kurono_kernel);
    p = pa(out, p, max_len, " ");
    p = pa(out, p, max_len, grub_config.integrated_args);
    p = pa(out, p, max_len, "\n}\n\n");

    // entry 3: recovery
    p = pa(out, p, max_len, "# ─── Recovery Mode ────────────────────────────────────\n");
    p = pa(out, p, max_len, "menuentry \"Kurono OS (Recovery Mode)\" --class recovery {\n");
    p = pa(out, p, max_len, "    set root='(hd0,msdos1)'\n");
    p = pa(out, p, max_len, "    multiboot ");
    p = pa(out, p, max_len, grub_config.kurono_kernel);
    p = pa(out, p, max_len, " root=/dev/sda1 recovery single\n");
    p = pa(out, p, max_len, "}\n");

    out[p] = 0;
}

void DualBootManager::SaveGRUBConfig() {
    char buf[4096];
    GenerateGRUBConfig(buf, sizeof(buf));
    KVFS::Mkdirs("/boot/grub");
    KVFS::WriteString("/boot/grub/grub.cfg", buf);
    SerialLogger::Log("[DualBoot] GRUB config saved\r\n");
}

void DualBootManager::SetDefaultBoot(int entry) {
    grub_config.default_entry = entry;
    SaveGRUBConfig();
}

void DualBootManager::SetTimeout(int seconds) {
    grub_config.timeout = seconds;
    SaveGRUBConfig();
}

//  shared data

SharedDataConfig* DualBootManager::GetSharedConfig() { return &shared_config; }

void DualBootManager::ApplySharedConfig() {
    // mount shared directories based on config
    if (shared_config.share_home) {
        SharedMountMgr::Mount("/home", "/home", SM_BIND, SM_FS_KVFS, false);
    }
    if (shared_config.share_tmp) {
        SharedMountMgr::Mount("/tmp", "/tmp", SM_BIND, SM_FS_TMP, false);
    }
    if (shared_config.share_var_log) {
        SharedMountMgr::Mount("/var/log", "/var/log", SM_BIND, SM_FS_KVFS, false);
    }
    if (shared_config.share_etc_overlay) {
        SharedMountMgr::Mount("/etc", "/linux/etc", SM_OVERLAY, SM_FS_KVFS, false);
    }
    if (shared_config.share_documents) {
        // create ~/documents shared bind
        SharedMountMgr::Mount("/home/user/Documents", "/home/user/Documents",
                               SM_BIND, SM_FS_KVFS, false);
    }
    if (shared_config.share_downloads) {
        SharedMountMgr::Mount("/home/user/Downloads", "/home/user/Downloads",
                               SM_BIND, SM_FS_KVFS, false);
    }
    if (shared_config.share_desktop) {
        SharedMountMgr::Mount("/home/user/Desktop", "/home/user/Desktop",
                               SM_BIND, SM_FS_KVFS, false);
    }

    if (shared_config.sync_users) {
        UserBridge::SetAutoSync(true);
        UserBridge::Sync();
    }

    SerialLogger::Log("[DualBoot] Shared data configuration applied\r\n");
}

//  linux partition operations

bool DualBootManager::FormatLinuxPartition() {
    DiskPartition* lp = FindPartition("Kurono Linux");
    if (!lp) return false;

    SerialLogger::Log("[DualBoot] Formatting Linux partition as ext4...\r\n");
    // in a real system, would format via block device
    // here we just mark it ready
    db_scpy(lp->fs_type, "ext4", sizeof(lp->fs_type));
    return true;
}

bool DualBootManager::MountLinuxPartition() {
    if (linux_partition_mounted) return true;

    DiskPartition* lp = FindPartition("Kurono Linux");
    if (!lp) return false;

    SerialLogger::Log("[DualBoot] Mounting Linux partition at /linux...\r\n");
    KVFS::Mkdirs("/linux");
    lp->mounted = true;
    linux_partition_mounted = true;
    return true;
}

bool DualBootManager::UnmountLinuxPartition() {
    if (!linux_partition_mounted) return true;

    DiskPartition* lp = FindPartition("Kurono Linux");
    if (lp) lp->mounted = false;
    linux_partition_mounted = false;

    if (Ext4::IsMounted()) {
        Ext4::Unmount();
    }
    return true;
}

bool DualBootManager::InstallLinuxRootfs() {
    if (!linux_partition_mounted && !MountLinuxPartition()) return false;

    SerialLogger::Log("[DualBoot] Installing Linux rootfs...\r\n");

    // create fhs structure on linux partition
    KLS::InitRootfs();

    // install kernel and initrd stubs
    KVFS::Mkdirs("/linux/boot");
    KVFS::WriteString("/linux/boot/vmlinuz-6.8.0-kurono",
        "# Kurono Linux virtual kernel image\n"
        "# This is loaded by GRUB for standalone Linux boot\n");
    KVFS::WriteString("/linux/boot/initrd.img-6.8.0-kurono",
        "# Kurono Linux initrd\n");

    // install essential linux config files
    KVFS::WriteString("/linux/etc/lsb-release",
        "DISTRIB_ID=KuronoLinux\n"
        "DISTRIB_RELEASE=1.0\n"
        "DISTRIB_CODENAME=kurono\n"
        "DISTRIB_DESCRIPTION=\"Kurono Linux 1.0\"\n");

    KVFS::WriteString("/linux/etc/debian_version", "12.0\n");

    KVFS::WriteString("/linux/etc/apt/sources.list",
        "# Kurono Linux package sources\n"
        "deb http://deb.debian.org/debian bookworm main contrib non-free\n"
        "deb http://deb.debian.org/debian-security bookworm-security main\n"
        "deb http://deb.debian.org/debian bookworm-updates main\n");

    // dpkg status (keeps track of installed packages)
    KVFS::Mkdirs("/linux/var/lib/dpkg");
    KVFS::WriteString("/linux/var/lib/dpkg/status", "");

    // create essential symlinks
    KVFS::WriteString("/linux/bin/sh", "#!/bin/bash\n");
    KVFS::WriteString("/linux/usr/bin/env", "#!/bin/bash\n");

    SerialLogger::Log("[DualBoot] Linux rootfs installed successfully\r\n");
    return true;
}

//  integrated boot

void DualBootManager::BootIntegrated() {
    SerialLogger::Log("[DualBoot] === INTEGRATED BOOT ===\r\n");
    SerialLogger::Log("[DualBoot] Both Kurono OS and Linux running simultaneously\r\n");

    // step 1: initialize virtual linux kernel
    LinuxKernel::Init();
    LinuxKernel::Start();

    // step 2: initialize signal system
    LinuxSignals::Init();

    // step 3: initialize kls
    KLS::Init();
    KLS::Start();

    // step 4: initialize device bridge
    LinuxDeviceBridge::Init();

    // step 5: initialize init system and boot
    LinuxInit::Init();
    LinuxInit::Boot();

    // step 6: apply shared data configuration
    ApplySharedConfig();

    // step 7: save grub config
    SaveGRUBConfig();

    // step 8: alpine boot remains manual until vmx bring-up is proven
    // stable across bare-metal laptops.
    if (VMM::GetType() == VIRT_INTEL_VTX) {
        SerialLogger::Log("[DualBoot] Intel VT-x detected  -  Alpine VM available for manual boot\r\n");
    } else {
        SerialLogger::Log("[DualBoot] Alpine VM unavailable for auto-start on this platform\r\n");
    }

    current_mode = BOOT_INTEGRATED;

    SerialLogger::Log("[DualBoot] === INTEGRATED BOOT COMPLETE ===\r\n");
    SerialLogger::Log("[DualBoot] Kurono OS + Linux running as one system\r\n");
    SerialLogger::Log("[DualBoot] Same users | Same files | Same desktop\r\n");
}

void DualBootManager::ShutdownIntegrated() {
    SerialLogger::Log("[DualBoot] Shutting down integrated Linux...\r\n");

    // shutdown alpine vm if running
    if (Hypervisor::IsAlpineBooted()) {
        Hypervisor::DestroyVM();
        SerialLogger::Log("[DualBoot] Alpine VM destroyed\r\n");
    }

    LinuxInit::Shutdown();
    KLS::Stop();
    LinuxKernel::Stop();
    UnmountLinuxPartition();

    SerialLogger::Log("[DualBoot] Integrated Linux shutdown complete\r\n");
}

//  shell integration

void DualBootManager::RegisterShellCommands(void* shell) {
    Shell* sh = (Shell*)shell;
    if (!sh) return;
    sh->RegisterCommand("dualboot",    cmd_dualboot,    "Dual-boot manager");
    sh->RegisterCommand("lsblk",       cmd_lsblk,       "List block devices");
    sh->RegisterCommand("fdisk",       cmd_fdisk,       "Partition manager");
    sh->RegisterCommand("mount-linux", cmd_mount_linux,  "Mount Linux partition");
}

int DualBootManager::cmd_dualboot(void*, int argc, const char** argv,
                                    char* out, int mx) {
    int p = 0;
    if (argc < 2) {
        p = pa(out, p, mx, "Kurono Dual-Boot Manager\n");
        p = pa(out, p, mx, "Current mode: ");
        p = pa(out, p, mx, BootModeName(current_mode));
        p = pa(out, p, mx, "\n\nCommands:\n");
        p = pa(out, p, mx, "  dualboot status      -  Show status\n");
        p = pa(out, p, mx, "  dualboot mode <n>    -  Set boot mode (0-3)\n");
        p = pa(out, p, mx, "  dualboot grub        -  Show GRUB config\n");
        p = pa(out, p, mx, "  dualboot install     -  Install Linux rootfs\n");
        return p;
    }

    if (db_seq(argv[1], "status")) {
        DumpStatus(out, mx);
        return (int)strlen(out);
    }
    if (db_seq(argv[1], "grub")) {
        GenerateGRUBConfig(out, mx);
        return (int)strlen(out);
    }
    if (db_seq(argv[1], "install")) {
        InstallLinuxRootfs();
        p = pa(out, p, mx, "Linux rootfs installed.\n");
        return p;
    }
    if (db_seq(argv[1], "mode") && argc >= 3) {
        int mode = argv[2][0] - '0';
        if (mode >= 0 && mode <= 3) {
            SetBootMode((BootMode)mode);
            p = pa(out, p, mx, "Boot mode set to: ");
            p = pa(out, p, mx, BootModeName((BootMode)mode));
            p = pa(out, p, mx, "\n");
        } else {
            p = pa(out, p, mx, "Invalid mode (0-3)\n");
        }
        return p;
    }

    p = pa(out, p, mx, "Unknown command. Use 'dualboot' for help.\n");
    return p;
}

int DualBootManager::cmd_lsblk(void*, int, const char**, char* out, int mx) {
    int p = 0;
    p = pa(out, p, mx, "NAME   SIZE   TYPE  FSTYPE  MOUNTPOINT\n");

    // whole disk
    p = pa(out, p, mx, "sda    352M   disk\n");

    for (int i = 0; i < partition_count; i++) {
        DiskPartition* pt = &partitions[i];
        p = pa(out, p, mx, " sda");
        p = pd(out, p, mx, i + 1);

        // size in mb
        int mb = (int)(pt->size_bytes / (1024 * 1024));
        p = pa(out, p, mx, "  ");
        if (mb < 100) p = pa(out, p, mx, " ");
        p = pd(out, p, mx, mb);
        p = pa(out, p, mx, "M   part  ");
        p = pa(out, p, mx, pt->fs_type);

        // pad
        int flen = 0;
        for (const char* c = pt->fs_type; *c; c++) flen++;
        for (int j = flen; j < 8; j++) p = pa(out, p, mx, " ");

        p = pa(out, p, mx, pt->mount_point);
        p = pa(out, p, mx, pt->mounted ? " [mounted]" : "");
        p = pa(out, p, mx, "\n");
    }
    return p;
}

int DualBootManager::cmd_fdisk(void*, int, const char**, char* out, int mx) {
    int p = 0;
    p = pa(out, p, mx, "Disk /dev/sda: 352 MiB, 4 partitions\n\n");
    p = pa(out, p, mx, "Device     Start    End  Sectors  Size   Type\n");

    for (int i = 0; i < partition_count; i++) {
        DiskPartition* pt = &partitions[i];
        p = pa(out, p, mx, "/dev/sda");
        p = pd(out, p, mx, i + 1);
        p = pa(out, p, mx, "  ");
        p = pd(out, p, mx, (int)pt->start_lba);
        p = pa(out, p, mx, "  ");
        p = pd(out, p, mx, (int)(pt->start_lba + pt->size_sectors - 1));
        p = pa(out, p, mx, "  ");
        p = pd(out, p, mx, (int)pt->size_sectors);
        p = pa(out, p, mx, "  ");
        p = pd(out, p, mx, (int)(pt->size_bytes / (1024 * 1024)));
        p = pa(out, p, mx, "M  ");
        p = pa(out, p, mx, pt->label);
        p = pa(out, p, mx, "\n");
    }
    return p;
}

int DualBootManager::cmd_mount_linux(void*, int, const char**, char* out, int mx) {
    int p = 0;
    if (MountLinuxPartition()) {
        p = pa(out, p, mx, "Linux partition mounted at /linux\n");
    } else {
        p = pa(out, p, mx, "Failed to mount Linux partition\n");
    }
    return p;
}

//  status dumps

void DualBootManager::DumpStatus(char* out, int max_out) {
    int p = 0;
    p = pa(out, p, max_out, "╔════════════════════════════════════════════╗\n");
    p = pa(out, p, max_out, "║   Kurono Dual-Boot / Dual-Run Manager     ║\n");
    p = pa(out, p, max_out, "╚════════════════════════════════════════════╝\n\n");

    p = pa(out, p, max_out, "  Boot Mode:     ");
    p = pa(out, p, max_out, BootModeName(current_mode));
    p = pa(out, p, max_out, "\n  Kurono:        ");
    p = pa(out, p, max_out, IsKuronoActive() ? "ACTIVE" : "inactive");
    p = pa(out, p, max_out, "\n  Linux:         ");
    p = pa(out, p, max_out, IsLinuxActive() ? "ACTIVE" : "inactive");
    p = pa(out, p, max_out, "\n  Integrated:    ");
    p = pa(out, p, max_out, IsIntegrated() ? "YES  -  same users, files, desktop" : "NO");
    p = pa(out, p, max_out, "\n  Linux Part:    ");
    p = pa(out, p, max_out, linux_partition_mounted ? "mounted" : "not mounted");
    p = pa(out, p, max_out, "\n\n  Shared:\n");
    p = pa(out, p, max_out, "    /home:       ");
    p = pa(out, p, max_out, shared_config.share_home ? "YES" : "NO");
    p = pa(out, p, max_out, "\n    /tmp:        ");
    p = pa(out, p, max_out, shared_config.share_tmp ? "YES" : "NO");
    p = pa(out, p, max_out, "\n    Users sync:  ");
    p = pa(out, p, max_out, shared_config.sync_users ? "YES" : "NO");
    p = pa(out, p, max_out, "\n    Network:     ");
    p = pa(out, p, max_out, shared_config.sync_network ? "YES" : "NO");
    p = pa(out, p, max_out, "\n");
    out[p] = 0;
}

void DualBootManager::DumpPartitions(char* out, int max_out) {
    cmd_lsblk(nullptr, 0, nullptr, out, max_out);
}
