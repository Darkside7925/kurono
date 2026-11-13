// installer_proto.h: the fixed wire format between the ring-3 installer app and
// the kernel installer service. both ends include this header; it is
// freestanding c. (satoru)
#pragma once
#include <stdint.h>
#include <stddef.h>

#define KIN_MAGIC         0x4B494E53u   // "KINS" request (satoru)
#define KIN_REPLY_MAGIC   0x4B494E52u   // "KINR" reply (satoru)
#define KIN_LIST_MAGIC    0x4B494C53u   // "KILS" disk list (satoru)
#define KIN_PLAN_MAGIC    0x4B49504Cu   // "KIPL" plan (satoru)
#define KIN_STATUS_MAGIC  0x4B495354u   // "KIST" progress (satoru)
#define KIN_PATH          "/system/run/user/1000/installer-svc"

// start requests must carry this constant. it is not a secret; it only stops
// an accidental zero-filled request from beginning a destructive install. (satoru)
#define KIN_CONFIRM       0x4B55524Fu   // "KURO" (satoru)

#define KIN_MAX_DISKS 8
#define KIN_MAX_PARTS 32

enum KinKind : uint32_t {
    KIN_LIST   = 1,   // reply: KIN_LIST_MAGIC + KinList (satoru)
    KIN_PLAN   = 2,   // reply: KIN_PLAN_MAGIC + KinPlan for one disk (satoru)
    KIN_START  = 3,   // begin a confirmed install (satoru)
    KIN_STATUS = 4,   // reply: KIN_STATUS_MAGIC + KinStatus (satoru)
};

enum KinScheme : uint32_t {
    KIN_SCHEME_NONE = 0,   // no partition table (blank disk) (satoru)
    KIN_SCHEME_MBR  = 1,
    KIN_SCHEME_GPT  = 2,
    KIN_SCHEME_BAD  = 3,   // a table we refuse to modify (satoru)
};

enum KinPlanMode : uint32_t {
    KIN_PLAN_NONE     = 0,
    KIN_PLAN_CREATE   = 1,   // blank disk: write a fresh gpt (satoru)
    KIN_PLAN_APPEND   = 2,   // existing gpt: add one entry in free space (satoru)
    KIN_PLAN_CONVERT  = 3,   // mbr disk: convert to gpt, preserve partitions (satoru)
    KIN_PLAN_EXISTING = 4,   // a KURONO partition is already there (satoru)
    KIN_PLAN_REFUSE   = 5,   // no safe plan, see why (satoru)
};

enum KinPartFlags : uint32_t {
    KIN_PART_ESP       = 1u << 0,
    KIN_PART_BOOTABLE  = 1u << 1,
    KIN_PART_KURONO    = 1u << 2,
    KIN_PART_OTHER_OS  = 1u << 3,   // ntfs / windows / ext4 / linux data (satoru)
};

enum KinDiskFlags : uint32_t {
    KIN_DISK_KURONO    = 1u << 0,
    KIN_DISK_OTHER_OS  = 1u << 1,
    KIN_DISK_REMOVABLE = 1u << 2,
    KIN_DISK_BADGPT    = 1u << 3,
    KIN_DISK_WINDOWS   = 1u << 4,   // another os found (see KinOsMask) (satoru)
    KIN_DISK_MAC       = 1u << 5,
    KIN_DISK_LINUX     = 1u << 6,
    KIN_DISK_RUNNING   = 1u << 7,   // the stick or disk kurono is running from (satoru)
    KIN_DISK_READONLY  = 1u << 8,   // the disk write guard forbids writes (satoru)
};

// which other operating systems a scan found on a disk. whole = a filesystem
// with no partition table at all, so writing a table would erase it. (satoru)
enum KinOsMask : uint32_t {
    KIN_OS_WINDOWS = 1u << 0,
    KIN_OS_MAC     = 1u << 1,
    KIN_OS_LINUX   = 1u << 2,
    KIN_OS_WHOLE   = 1u << 3,
};

struct KinPart {
    char     name[24];
    char     label[32];
    char     fstype[12];    // "ntfs", "fat32", "ext4", "unknown" (satoru)
    char     typestr[24];   // human readable gpt/mbr type (satoru)
    uint64_t start_lba;
    uint64_t last_lba;
    uint64_t bytes;
    uint32_t flags;
    uint32_t reserved;
};

struct KinDisk {
    int32_t  index;         // BlockDev index to pass back to KIN_PLAN (satoru)
    uint32_t present;
    char     name[16];
    char     driver[16];
    char     model[64];
    uint64_t bytes;
    uint32_t sector_size;
    uint32_t scheme;
    uint32_t part_count;
    uint32_t flags;
};

struct KinList {
    uint32_t magic;
    uint32_t count;
    KinDisk  disks[KIN_MAX_DISKS];
};

struct KinPlan {
    uint32_t magic;
    int32_t  ok;
    uint32_t mode;
    uint32_t need_convert;   // mbr -> gpt; the ui must ask for a second ack (satoru)
    uint32_t has_existing;   // exactly one KURONO partition already on the disk (satoru)
    uint32_t has_other_os;   // existing partitions will be left untouched (satoru)
    uint32_t guard_blocked;  // the disk write guard refuses the needed writes (satoru)
    uint32_t auto_mode;      // kurono.installer.auto=1 test driver (satoru)
    uint32_t payloads;       // this kernel carries the embedded EFI loader (satoru)
    int32_t  disk_index;
    uint32_t parts_n;
    uint64_t part_start_lba;
    uint64_t part_last_lba;
    uint64_t part_bytes;
    uint64_t kfs_bytes;
    uint64_t log_bytes;
    char     disk_name[16];
    char     model[64];
    char     why[96];
    uint32_t os_mask;        // KinOsMask bits found on this disk (satoru)
    uint32_t need_os_ack;    // the plan rewrites a table or filesystem that holds another os (satoru)
    uint32_t running_disk;   // this disk is the medium kurono is running from (satoru)
    char     os_line[48];    // "This disk has Windows on it" or empty (satoru)
    KinPart  parts[KIN_MAX_PARTS];
};

struct KinStatus {
    uint32_t magic;
    uint32_t running;
    uint32_t done;
    uint32_t failed;
    uint32_t pct;
    char     stage[64];
    char     detail[160];
};

struct KinReq {
    uint32_t magic;
    uint32_t kind;
    int32_t  disk;
    uint32_t confirm;
    uint32_t convert_ack;
    uint32_t keep_ack;
    int32_t  kfs_mb;
    uint32_t os_ack;         // second confirmation for a disk that holds another os (satoru)
};

struct KinReply {
    uint32_t magic;
    int32_t  ok;
    int32_t  code;
    char     why[96];
};

// end (satoru)
