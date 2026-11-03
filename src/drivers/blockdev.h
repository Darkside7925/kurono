#pragma once
#include "../kernel/types.h"

// generic block-device registry. (satoru)
//
// every disk path in the tree currently calls NVMe:: directly - the installer's
// gpt/mbr scanner, the partition enumerator, the persist layer. that is why a
// machine with a sata ssd looks like it has no disks at all: the partition
// parsing is fine, it just has nothing but nvme to parse.
//
// this is the seam. a driver registers one entry per attached disk and everyone
// else iterates the registry, so adding ahci (or usb mass storage, or virtio-blk)
// makes the existing gpt scanner work on those disks with no change to it.
//
// read/write take LBAs and a count, matching what both nvme and ahci speak
// natively; the byte-offset callbacks the filesystems want (Fat32BlockRead /
// Ext4BlockRead) are built on top of this plus a partition start_lba. (satoru)

constexpr int BLOCKDEV_MAX = 16;

struct BlockDevice {
    bool     present;
    char     name[16];      // stable-ish kernel name: "nvme0n1", "sda" (satoru)
    char     driver[16];    // "nvme", "ahci" (satoru)
    char     model[64];
    uint64_t capacity_lba;  // total addressable sectors (satoru)
    uint32_t sector_size;   // bytes per sector (512 or 4096) (satoru)
    bool     removable;

    // driver-private handle. ahci stores its port index here; nvme ignores it.
    // passed back to the fn pointers so one driver can back several disks. (satoru)
    void*    ctx;

    bool (*read)(void* ctx, uint64_t lba, uint32_t count, void* buf);
    bool (*write)(void* ctx, uint64_t lba, uint32_t count, const void* buf);
    bool (*flush)(void* ctx);
};

class BlockDev {
public:
    // wipe the registry. drivers re-register on Rescan. (satoru)
    static void Init();

    // register one attached disk. returns its index, or -1 if the table is full
    // or the entry is malformed (no read fn / zero sector size). the caller owns
    // the BlockDevice contents; they are copied in. (satoru)
    static int  Register(const BlockDevice& dev);

    static int  Count();
    static BlockDevice* Get(int idx);

    // find by kernel name ("sda"), or null. (satoru)
    static BlockDevice* FindByName(const char* name);

    // convenience wrappers that bounds-check against capacity before touching
    // the driver - a gpt scanner reading past the end of a disk should get a
    // clean false, not whatever the controller decides to do. (satoru)
    static bool Read(int idx, uint64_t lba, uint32_t count, void* buf);
    static bool Write(int idx, uint64_t lba, uint32_t count, const void* buf);
    static bool Flush(int idx);

    // byte-granular helpers over a partition. `part_offset` is the partition's
    // byte offset on the disk (start_lba * sector_size); these do the
    // read-modify-write dance for offsets that are not sector aligned, which is
    // what the ext4/fat32 byte callbacks need. (satoru)
    static bool ReadBytes(int idx, uint64_t byte_off, uint32_t len, void* buf);
    static bool WriteBytes(int idx, uint64_t byte_off, uint32_t len, const void* buf);

    // one line per disk, for `lsblk`-style output. (satoru)
    static void Describe(int idx, char* out, int max_len);

private:
    static BlockDevice s_devs[BLOCKDEV_MAX];
    static int         s_count;
};

// end (satoru)
