#pragma once
//  kurono os  -  dual-boot / dual-run manager
//  manages running both kurono os and linux simultaneously, or booting
//  either independently. handles grub configuration.
//
//  modes:
//  ┌─────────────────────────────────────────────────────────────────────┐
//  │  1. integrated (default)                                           │
//  │     kurono boots as primary os, linux runs as subsystem inside it  │
//  │     • same kernel space, shared memory                             │
//  │     • linux elf binaries run through kls syscall translation       │
//  │     • deepest integration: same users, same files, same desktop    │
//  │                                                                    │
//  │  2. dual_active                                                    │
//  │     both boot from same grub, linux gets its own partition          │
//  │     kurono's kls auto-mounts the linux ext4 partition              │
//  │     • shared /home via bind mount                                  │
//  │     • user sync via userbridge                                     │
//  │                                                                    │
//  │  3. standalone_linux                                               │
//  │     pure linux boot from sda2  -  kurono not loaded                  │
//  │     • standard linux kernel + initrd                               │
//  │     • can still access kurono partition if mounted                 │
//  │                                                                    │
//  │  4. standalone_kurono                                              │
//  │     pure kurono boot  -  no linux subsystem                          │
//  └─────────────────────────────────────────────────────────────────────┘

#include "../kernel/types.h"

enum BootMode {
    BOOT_INTEGRATED = 0,        // kurono + linux subsystem (default)
    BOOT_DUAL_ACTIVE = 1,       // both with shared partitions
    BOOT_STANDALONE_LINUX = 2,  // pure linux
    BOOT_STANDALONE_KURONO = 3  // pure kurono
};

#define DUALBOOT_MAX_PARTITIONS  8

enum PartitionType {
    PART_KURONO = 0,
    PART_LINUX_EXT4,
    PART_LINUX_SWAP,
    PART_SHARED_DATA,
    PART_EFI_SYSTEM,
    PART_UNKNOWN
};

struct DiskPartition {
    char          label[32];
    PartitionType type;
    uint64_t      start_lba;
    uint64_t      size_sectors;
    uint64_t      size_bytes;      // convenience
    char          fs_type[16];     // "kurono", "ext4", "swap", "fat32"
    char          mount_point[32]; // where it's mounted
    bool          mounted;
    bool          bootable;
};

struct GRUBConfig {
    int      default_entry;      // 0-based
    int      timeout;            // seconds
    char     theme[64];
    bool     show_menu;

    // kurono entry
    char     kurono_kernel[64];  // /boot/kurono.elf
    char     kurono_args[128];   // multiboot args

    // linux entry
    char     linux_kernel[64];   // /boot/vmlinuz-6.8.0-kurono
    char     linux_initrd[64];   // /boot/initrd.img-6.8.0-kurono
    char     linux_args[128];    // root=/dev/sda2 etc.

    // integrated entry
    char     integrated_args[128];
};

struct SharedDataConfig {
    bool share_home;
    bool share_tmp;
    bool share_var_log;
    bool share_etc_overlay;
    bool share_documents;
    bool share_downloads;
    bool share_desktop;
    bool sync_users;
    bool sync_groups;
    bool sync_network;
};

//  dualbootmanager  -  controls boot modes and partition management

class DualBootManager {
public:
    static void Init();

    static BootMode GetBootMode();
    static void     SetBootMode(BootMode mode);
    static const char* BootModeName(BootMode mode);
    static bool     IsLinuxActive();
    static bool     IsKuronoActive();
    static bool     IsIntegrated();

    static void     DetectPartitions();
    static DiskPartition* GetPartitions();
    static int      GetPartitionCount();
    static DiskPartition* FindPartition(const char* label);
    static DiskPartition* FindByMount(const char* mount);

    static GRUBConfig* GetGRUBConfig();
    static void     GenerateGRUBConfig(char* out, int max_len);
    static void     SaveGRUBConfig();
    static void     SetDefaultBoot(int entry);   // 0=kurono, 1=linux, 2=integrated
    static void     SetTimeout(int seconds);

    static SharedDataConfig* GetSharedConfig();
    static void     ApplySharedConfig();

    static bool     FormatLinuxPartition();   // create ext4 on sda2
    static bool     MountLinuxPartition();
    static bool     UnmountLinuxPartition();
    static bool     InstallLinuxRootfs();     // populate rootfs

    static void     BootIntegrated();         // start kls + init
    static void     ShutdownIntegrated();

    static void     DumpStatus(char* out, int max_out);
    static void     DumpPartitions(char* out, int max_out);

    static void     RegisterShellCommands(void* shell);

private:
    static BootMode         current_mode;
    static DiskPartition    partitions[DUALBOOT_MAX_PARTITIONS];
    static int              partition_count;
    static GRUBConfig       grub_config;
    static SharedDataConfig shared_config;
    static bool             linux_partition_mounted;

    // shell command handlers
    static int cmd_dualboot(void* sh, int argc, const char** argv,
                             char* out, int mx);
    static int cmd_lsblk(void* sh, int argc, const char** argv,
                           char* out, int mx);
    static int cmd_fdisk(void* sh, int argc, const char** argv,
                          char* out, int mx);
    static int cmd_mount_linux(void* sh, int argc, const char** argv,
                                char* out, int mx);

    // helpers
    static int pa(char* out, int pos, int mx, const char* s);
    static int pd(char* out, int pos, int mx, int val);
};
