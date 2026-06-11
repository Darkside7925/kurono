#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono Linux Subsystem (KLS)
//  The master integration layer that lets Linux run inside Kurono OS.
//
//  Architecture:
//  ┌──────────────────────────────────────────────────────────────────┐
//  │                     Kurono OS Kernel                             │
//  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────────┐│
//  │  │  KVFS    │  │ Graphics │  │  SUPR    │  │  Window Manager  ││
//  │  │(shared)  │  │  (host)  │  │(shared)  │  │   (host)         ││
//  │  └────┬─────┘  └──────────┘  └────┬─────┘  └──────────────────┘│
//  │       │                            │                             │
//  │  ┌────┴────────────────────────────┴─────────────────────┐      │
//  │  │            Kurono Linux Subsystem (KLS)               │      │
//  │  │  ┌───────────┐ ┌────────────┐ ┌──────────────────┐   │      │
//  │  │  │ ELF Loader│ │ Syscall ABI│ │ Shared FS Mount  │   │      │
//  │  │  └───────────┘ └────────────┘ └──────────────────┘   │      │
//  │  │  ┌───────────┐ ┌────────────┐ ┌──────────────────┐   │      │
//  │  │  │ ext4 drv  │ │ User Bridge│ │ Linux Rootfs     │   │      │
//  │  │  └───────────┘ └────────────┘ └──────────────────┘   │      │
//  │  └───────────────────────────────────────────────────────┘      │
//  │                                                                  │
//  │  Linux ELF binaries run through KLS syscall translation          │
//  │  Same users, same files, same permissions                        │
//  └──────────────────────────────────────────────────────────────────┘
//
//  Key Design Principles:
//  1. SAME USERS: SUPR users === Linux /etc/passwd entries
//  2. SAME FILES: KVFS and ext4 share /home, /tmp, /var
//  3. INDEPENDENT: Linux can boot standalone OR inside Kurono
//  4. DUAL BOOT:  GRUB chainloads either Kurono kernel or Linux kernel
// ═══════════════════════════════════════════════════════════════════════════

#include "../kernel/types.h"

// ─── KLS status ──────────────────────────────────────────────────────────

enum KLSState {
    KLS_STOPPED = 0,
    KLS_INITIALIZING,
    KLS_RUNNING,
    KLS_ERROR,
    KLS_SUSPENDED
};

// ─── ELF header structures (32-bit) ─────────────────────────────────────

#define ELF_MAGIC  0x464C457F    // "\x7FELF"

struct Elf32Header {
    uint32_t e_magic;
    uint8_t  e_class;       // 1 = 32-bit
    uint8_t  e_data;        // 1 = little-endian
    uint8_t  e_version_b;
    uint8_t  e_osabi;
    uint8_t  e_pad[8];
    uint16_t e_type;        // 2 = ET_EXEC
    uint16_t e_machine;     // 3 = EM_386
    uint32_t e_version;
    uint32_t e_entry;       // Entry point
    uint32_t e_phoff;       // Program header offset
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct Elf32Phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed));

#define PT_LOAD    1
#define PT_INTERP  3
#define PT_NOTE    4

// ─── Linux package info ──────────────────────────────────────────────────

#define KLS_MAX_PACKAGES  64

struct KLSPackage {
    char name[32];
    char version[16];
    bool installed;
    bool essential;     // Cannot be removed
};

// ─── KLS configuration ──────────────────────────────────────────────────

struct KLSConfig {
    bool share_home;       // /home shared between Kurono and Linux
    bool share_tmp;        // /tmp shared
    bool share_users;      // SUPR users synced to /etc/passwd
    bool auto_mount_ext4;  // Auto-mount Linux partition
    bool enable_x11;       // X11 forwarding to Kurono WM (future)

    char linux_root[64];   // ext4 mount point in KVFS namespace
    char hostname[32];
    char default_shell[32];

    uint32_t linux_partition_lba;   // Disk LBA for Linux partition
    uint32_t linux_partition_size;  // Size in sectors
};

// ═══════════════════════════════════════════════════════════════════════════
//  KLS — Kurono Linux Subsystem main class
// ═══════════════════════════════════════════════════════════════════════════

class KLS {
public:
    // Lifecycle
    static void Init();
    static void Start();
    static void Stop();
    static void Suspend();
    static void Resume();
    static KLSState GetState();

    // Configuration
    static KLSConfig* GetConfig();
    static void SetConfig(const KLSConfig& cfg);
    static void SaveConfig();
    static void LoadConfig();

    // ELF execution
    static int  LoadELF(const char* path);
    static int  ExecELF(const char* path, int argc, const char** argv);
    static bool IsValidELF(const void* data, uint32_t size);

    // Shared filesystem
    static void MountSharedDirs();
    static void UnmountSharedDirs();
    static void SyncFilesystems();

    // User synchronization
    static void SyncUsersToLinux();     // SUPR → /etc/passwd,shadow
    static void SyncUsersFromLinux();   // /etc/passwd → SUPR
    static void SyncSingleUser(const char* username);

    // Linux rootfs management
    static bool HasRootfs();
    static void InitRootfs();
    static void PopulateDefaultRootfs();

    // Package management (for the Linux side)
    static int  InstallPackage(const char* name);
    static int  RemovePackage(const char* name);
    static KLSPackage* GetPackages();
    static int  GetPackageCount();
    static KLSPackage* FindPackage(const char* name);

    // Linux command execution (via syscall layer)
    static int  RunCommand(const char* cmd, char* output, int max_output);
    static int  RunShellCommand(const char* cmd, char* output, int max_output);

    // Shell integration — register Linux commands with Kurono shell
    static void RegisterShellCommands(void* shell);

    // Syscall handler (called from IDT int 0x80)
    static int32_t HandleSyscall(uint32_t eax, uint32_t ebx, uint32_t ecx,
                                  uint32_t edx, uint32_t esi, uint32_t edi);

    // Status info
    static int  LinuxProcessCount();
    static const char* GetKernelVersion();
    static uint64_t GetExt4FreeSpace();
    static uint64_t GetExt4TotalSpace();

private:
    static KLSState state;
    static KLSConfig config;
    static KLSPackage packages[KLS_MAX_PACKAGES];
    static int package_count;

    // ELF loading internals
    static int  LoadELFSegments(const void* elf_data, uint32_t size);
    static uint32_t FindEntryPoint(const void* elf_data);

    // /etc/passwd generation
    static void GeneratePasswd(char* buf, int max_len);
    static void GenerateGroup(char* buf, int max_len);
    static void GenerateShadow(char* buf, int max_len);

    // Shell command handlers
    static int cmd_kls(void* sh, int argc, const char** argv,
                       char* out, int mx);
    static int cmd_linux(void* sh, int argc, const char** argv,
                         char* out, int mx);
    static int cmd_lsb(void* sh, int argc, const char** argv,
                       char* out, int mx);
    static int cmd_dpkg(void* sh, int argc, const char** argv,
                        char* out, int mx);
};
