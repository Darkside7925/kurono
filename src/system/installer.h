#pragma once

#include "../kernel/types.h"

#define INSTALLER_MAX_DISKS       4
#define INSTALLER_MAX_PARTITIONS  32

enum InstallerDiskType : uint8_t {
    INST_DISK_NONE = 0,
    INST_DISK_NVME = 1,
};

enum InstallerPartScheme : uint8_t {
    INST_SCHEME_UNKNOWN = 0,
    INST_SCHEME_MBR     = 1,
    INST_SCHEME_GPT     = 2,
};

enum InstallerFsType : uint8_t {
    INST_FS_UNKNOWN = 0,
    INST_FS_FAT32   = 1,
    INST_FS_EXT4    = 2,
};

struct InstallerDiskInfo {
    bool              present;
    InstallerDiskType type;
    InstallerPartScheme scheme;
    char              name[16];
    char              driver[16];
    char              model[64];
    uint64_t          total_lba;
    uint32_t          sector_size;
    bool              has_esp;
};

struct InstallerPartitionInfo {
    bool              present;
    int               disk_index;
    int               part_index;
    InstallerPartScheme scheme;
    char              name[20];
    char              label[64];
    uint64_t          start_lba;
    uint64_t          last_lba;
    uint64_t          sector_count;
    uint64_t          size_bytes;
    InstallerFsType   fs_type;
    bool              esp;
    bool              bootable;
    uint8_t           mbr_type;
    uint8_t           type_guid[16];
};

class Installer {
public:
    static void Init();
    static void Rescan();

    static int  GetDiskCount();
    static int  GetPartitionCount();
    static InstallerDiskInfo* GetDisk(int idx);
    static InstallerPartitionInfo* GetPartition(int idx);
    static int  FindESPPartition();
    static int  FindFirstExt4Partition();

    static void RegisterShellCommands(void* shell);
    static int  InstallToPartition(int partition_index, char* out, int max_out);
    static void DescribeInstallPlan(int partition_index, char* out, int max_out);
    static void DumpDisks(char* out, int max_out);
    static void DumpPartitions(char* out, int max_out);

private:
    static InstallerDiskInfo disks[INSTALLER_MAX_DISKS];
    static InstallerPartitionInfo parts[INSTALLER_MAX_PARTITIONS];
    static int disk_count;
    static int part_count;
    static bool initialized;

    static int cmd_installer(void* sh, int argc, const char** argv, char* out, int mx);
};
