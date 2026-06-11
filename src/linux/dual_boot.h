#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Dual-Boot / Dual-Run Manager
//  Manages running BOTH Kurono OS and Linux simultaneously, or booting
//  either independently. Handles GRUB configuration.
//
//  Modes:
//  ┌─────────────────────────────────────────────────────────────────────┐
//  │  1. INTEGRATED (default)                                           │
//  │     Kurono boots as primary OS, Linux runs as subsystem inside it  │
//  │     • Same kernel space, shared memory                             │
//  │     • Linux ELF binaries run through KLS syscall translation       │
//  │     • Deepest integration: same users, same files, same desktop    │
//  │                                                                    │
//  │  2. DUAL_ACTIVE                                                    │
//  │     Both boot from same GRUB, Linux gets its own partition          │
//  │     Kurono's KLS auto-mounts the Linux ext4 partition              │
//  │     • Shared /home via bind mount                                  │
//  │     • User sync via UserBridge                                     │
//  │                                                                    │
//  │  3. STANDALONE_LINUX                                               │
//  │     Pure Linux boot from sda2 — Kurono not loaded                  │
//  │     • Standard Linux kernel + initrd                               │
//  │     • Can still access Kurono partition if mounted                 │
//  │                                                                    │
//  │  4. STANDALONE_KURONO                                              │
//  │     Pure Kurono boot — no Linux subsystem                          │
//  └─────────────────────────────────────────────────────────────────────┘
// ═══════════════════════════════════════════════════════════════════════════

#include "../kernel/types.h"

// ─── Boot modes ─────────────────────────────────────────────────────────

enum BootMode {
    BOOT_INTEGRATED = 0,        // Kurono + Linux subsystem (default)
    BOOT_DUAL_ACTIVE = 1,       // Both with shared partitions
    BOOT_STANDALONE_LINUX = 2,  // Pure Linux
    BOOT_STANDALONE_KURONO = 3  // Pure Kurono
};

// ─── Partition table ────────────────────────────────────────────────────

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
    uint64_t      size_bytes;      // Convenience
    char          fs_type[16];     // "kurono", "ext4", "swap", "fat32"
    char          mount_point[32]; // Where it's mounted
    bool          mounted;
    bool          bootable;
};

// ─── GRUB configuration ────────────────────────────────────────────────

struct GRUBConfig {
    int      default_entry;      // 0-based
    int      timeout;            // Seconds
    char     theme[64];
    bool     show_menu;

    // Kurono entry
    char     kurono_kernel[64];  // /boot/kurono.elf
    char     kurono_args[128];   // Multiboot args

    // Linux entry
    char     linux_kernel[64];   // /boot/vmlinuz-6.8.0-kurono
    char     linux_initrd[64];   // /boot/initrd.img-6.8.0-kurono
    char     linux_args[128];    // root=/dev/sda2 etc.

    // Integrated entry
    char     integrated_args[128];
};

// ─── Shared data configuration ─────────────────────────────────────────

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

// ═══════════════════════════════════════════════════════════════════════════
//  DualBootManager — Controls boot modes and partition management
// ═══════════════════════════════════════════════════════════════════════════

class DualBootManager {
public:
    // ── Initialization ────────────────────────────────────────────────
    static void Init();

    // ── Boot mode ─────────────────────────────────────────────────────
    static BootMode GetBootMode();
    static void     SetBootMode(BootMode mode);
    static const char* BootModeName(BootMode mode);
    static bool     IsLinuxActive();
    static bool     IsKuronoActive();
    static bool     IsIntegrated();

    // ── Partition management ──────────────────────────────────────────
    static void     DetectPartitions();
    static DiskPartition* GetPartitions();
    static int      GetPartitionCount();
    static DiskPartition* FindPartition(const char* label);
    static DiskPartition* FindByMount(const char* mount);

    // ── GRUB configuration ────────────────────────────────────────────
    static GRUBConfig* GetGRUBConfig();
    static void     GenerateGRUBConfig(char* out, int max_len);
    static void     SaveGRUBConfig();
    static void     SetDefaultBoot(int entry);   // 0=Kurono, 1=Linux, 2=Integrated
    static void     SetTimeout(int seconds);

    // ── Shared data ───────────────────────────────────────────────────
    static SharedDataConfig* GetSharedConfig();
    static void     ApplySharedConfig();

    // ── Linux partition operations ────────────────────────────────────
    static bool     FormatLinuxPartition();   // Create ext4 on sda2
    static bool     MountLinuxPartition();
    static bool     UnmountLinuxPartition();
    static bool     InstallLinuxRootfs();     // Populate rootfs

    // ── Boot sequence for integrated mode ─────────────────────────────
    static void     BootIntegrated();         // Start KLS + init
    static void     ShutdownIntegrated();

    // ── Status ────────────────────────────────────────────────────────
    static void     DumpStatus(char* out, int max_out);
    static void     DumpPartitions(char* out, int max_out);

    // ── Shell integration ─────────────────────────────────────────────
    static void     RegisterShellCommands(void* shell);

private:
    static BootMode         current_mode;
    static DiskPartition    partitions[DUALBOOT_MAX_PARTITIONS];
    static int              partition_count;
    static GRUBConfig       grub_config;
    static SharedDataConfig shared_config;
    static bool             linux_partition_mounted;

    // Shell command handlers
    static int cmd_dualboot(void* sh, int argc, const char** argv,
                             char* out, int mx);
    static int cmd_lsblk(void* sh, int argc, const char** argv,
                           char* out, int mx);
    static int cmd_fdisk(void* sh, int argc, const char** argv,
                          char* out, int mx);
    static int cmd_mount_linux(void* sh, int argc, const char** argv,
                                char* out, int mx);

    // Helpers
    static int pa(char* out, int pos, int mx, const char* s);
    static int pd(char* out, int pos, int mx, int val);
};
