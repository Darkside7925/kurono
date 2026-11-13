// installer_disk.h: the kernel side of the graphical installer. it scans disks,
// works out a safe place for a KURONO partition and writes the partition table,
// a FAT32 volume, the embedded EFI loader and the log/kfs files. all writes go
// through BlockDev, so the existing bare-metal disk write guard still applies.
// (satoru)
#pragma once
#include "../kernel/types.h"
#include "installer_proto.h"

namespace InstallerDisk {

// progress is reported from the install worker. the callback runs often (per
// few megabytes for the big zero-filled files), so it must stay short. (satoru)
struct Progress {
    void (*fn)(uint32_t pct, const char* stage, void* ctx);
    void* ctx;
};

// true when this kernel image carries the embedded EFI loader payloads. the
// full kernel (build/kurono.elf) does; the base kernel used by the normal
// desktop entry does not, and the setup entry boots the full one. (satoru)
bool PayloadAvailable();

// fill a disk list. only BlockDev disks are included; the boot medium has no
// block device entry. (satoru)
int Scan(KinList* out);

// compute the install plan for one disk. kfs_mb 0 means choose a default. the
// plan never writes anything. (satoru)
int Plan(int disk, uint32_t kfs_mb, KinPlan* out);

// write the plan. convert_ack and keep_ack must both be true when the plan
// needs them (the app's confirm screen supplies them). returns 0 on success,
// -1 with why filled otherwise. (satoru)
// os_ack must also be true when the plan sets need_os_ack (the disk holds another
// operating system and the plan rewrites its table). every raw write goes through
// one choke point that only allows the table areas and the new partition. (satoru)
int Install(int disk, uint32_t kfs_mb, uint32_t convert_ack, uint32_t keep_ack,
            uint32_t os_ack, Progress* prog, char* why, int why_cap);

// what the scan of one BlockDev found, for the kernel gui and the shell
// installer, which do not go through Plan. read only. (satoru)
struct DiskSafety {
    uint32_t os_mask;   // KinOsMask bits (satoru)
    bool     running;   // the medium kurono is running from (satoru)
    bool     readonly;  // the disk write guard is on (satoru)
    char     line[48];  // "This disk has Windows on it" or empty (satoru)
};
bool Safety(int dev, DiskSafety* out);

// end (satoru)
}  // namespace InstallerDisk

// end (satoru)
