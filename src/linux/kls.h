#pragma once
//  kurono linux subsystem (kls)
//  the master integration layer that lets linux run inside kurono os.
//
//  architecture:
//  ┌──────────────────────────────────────────────────────────────────┐
//  │                     kurono os kernel                             │
//  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────────┐│
//  │  │  kvfs    │  │ graphics │  │  supr    │  │  window manager  ││
//  │  │(shared)  │  │  (host)  │  │(shared)  │  │   (host)         ││
//  │  └────┬─────┘  └──────────┘  └────┬─────┘  └──────────────────┘│
//  │       │                            │                             │
//  │  ┌────┴────────────────────────────┴─────────────────────┐      │
//  │  │            kurono linux subsystem (kls)               │      │
//  │  │  ┌───────────┐ ┌────────────┐ ┌──────────────────┐   │      │
//  │  │  │ elf loader│ │ syscall abi│ │ shared fs mount  │   │      │
//  │  │  └───────────┘ └────────────┘ └──────────────────┘   │      │
//  │  │  ┌───────────┐ ┌────────────┐ ┌──────────────────┐   │      │
//  │  │  │ ext4 drv  │ │ user bridge│ │ linux rootfs     │   │      │
//  │  │  └───────────┘ └────────────┘ └──────────────────┘   │      │
//  │  └───────────────────────────────────────────────────────┘      │
//  │                                                                  │
//  │  linux elf binaries run through kls syscall translation          │
//  │  same users, same files, same permissions                        │
//  └──────────────────────────────────────────────────────────────────┘
//
//  key design principles:
//  1. same users: supr users === linux /etc/passwd entries
//  2. same files: kvfs and ext4 share /home, /tmp, /var
//  3. independent: linux can boot standalone or inside kurono
//  4. dual boot:  grub chainloads either kurono kernel or linux kernel

#include "../kernel/types.h"

enum KLSState {
    KLS_STOPPED = 0,
    KLS_INITIALIZING,
    KLS_RUNNING,
    KLS_ERROR,
    KLS_SUSPENDED
};

#define ELF_MAGIC  0x464C457F    // "\x7felf"

struct Elf32Header {
    uint32_t e_magic;
    uint8_t  e_class;       // 1 = 32-bit
    uint8_t  e_data;        // 1 = little-endian
    uint8_t  e_version_b;
    uint8_t  e_osabi;
    uint8_t  e_pad[8];
    uint16_t e_type;        // 2 = et_exec
    uint16_t e_machine;     // 3 = em_386
    uint32_t e_version;
    uint32_t e_entry;       // entry point
    uint32_t e_phoff;       // program header offset
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

#define KLS_MAX_PACKAGES  64

struct KLSPackage {
    char name[32];
    char version[16];
    bool installed;
    bool essential;     // cannot be removed
};

struct KLSConfig {
    bool share_home;       // /home shared between kurono and linux
    bool share_tmp;        // /tmp shared
    bool share_users;      // supr users synced to /etc/passwd
    bool auto_mount_ext4;  // auto-mount linux partition
    bool enable_x11;       // x11 forwarding to kurono wm (future)

    char linux_root[64];   // ext4 mount point in kvfs namespace
    char hostname[32];
    char default_shell[32];

    uint32_t linux_partition_lba;   // disk lba for linux partition
    uint32_t linux_partition_size;  // size in sectors
};

//  kls - kurono linux subsystem main class

class KLS {
public:
    // lifecycle
    static void Init();
    static void Start();
    static void Stop();
    static void Suspend();
    static void Resume();
    static KLSState GetState();

    // configuration
    static KLSConfig* GetConfig();
    static void SetConfig(const KLSConfig& cfg);
    static void SaveConfig();
    static void LoadConfig();

    // elf execution
    static int  LoadELF(const char* path);
    static int  ExecELF(const char* path, int argc, const char** argv);
    static bool IsValidELF(const void* data, uint32_t size);

    // shared filesystem
    static void MountSharedDirs();
    static void UnmountSharedDirs();
    static void SyncFilesystems();

    // user synchronization
    static void SyncUsersToLinux();     // supr → /etc/passwd,shadow
    static void SyncUsersFromLinux();   // /etc/passwd → supr
    static void SyncSingleUser(const char* username);

    // linux rootfs management
    static bool HasRootfs();
    static void InitRootfs();
    static void PopulateDefaultRootfs();

    // package management (for the linux side)
    static int  InstallPackage(const char* name);
    static int  RemovePackage(const char* name);
    static KLSPackage* GetPackages();
    static int  GetPackageCount();
    static KLSPackage* FindPackage(const char* name);

    // linux command execution (via syscall layer)
    static int  RunCommand(const char* cmd, char* output, int max_output);
    static int  RunShellCommand(const char* cmd, char* output, int max_output);

    // shell integration - register linux commands with kurono shell
    static void RegisterShellCommands(void* shell);

    // syscall handler (called from idt int 0x80)
    static int32_t HandleSyscall(uint32_t eax, uint32_t ebx, uint32_t ecx,
                                  uint32_t edx, uint32_t esi, uint32_t edi);

    // status info
    static int  LinuxProcessCount();
    static const char* GetKernelVersion();
    static uint64_t GetExt4FreeSpace();
    static uint64_t GetExt4TotalSpace();

private:
    static KLSState state;
    static KLSConfig config;
    static KLSPackage packages[KLS_MAX_PACKAGES];
    static int package_count;

    // elf loading internals
    static int  LoadELFSegments(const void* elf_data, uint32_t size);
    static uint32_t FindEntryPoint(const void* elf_data);

    // /etc/passwd generation
    static void GeneratePasswd(char* buf, int max_len);
    static void GenerateGroup(char* buf, int max_len);
    static void GenerateShadow(char* buf, int max_len);

    // shell command handlers
    static int cmd_kls(void* sh, int argc, const char** argv,
                       char* out, int mx);
    static int cmd_linux(void* sh, int argc, const char** argv,
                         char* out, int mx);
    static int cmd_lsb(void* sh, int argc, const char** argv,
                       char* out, int mx);
    static int cmd_dpkg(void* sh, int argc, const char** argv,
                        char* out, int mx);
};
