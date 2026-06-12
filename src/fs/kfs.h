#pragma once
#include "../kernel/types.h"

//  KFS  -  Kurono File System.
//
//  a small, fast, inode-based on-disk filesystem designed from scratch for
//  Kurono  -  no linux baggage. it is the on-disk PERSISTENCE layer: the in-memory
//  KVFS stays the runtime filesystem, and KFS stores the user-data subset across
//  reboots as REAL files + directories (not an opaque blob), so a volume is
//  browsable + a Linux kfs-fuse driver could mount it later (the on-disk format
//  below is the spec for that).
//
//  on-disk layout (4 KB blocks, all multi-byte fields little-endian):
//    block 0              superblock
//    block 1 .. B         free-block bitmap   (1 bit/block, bit set = used)
//    block B+1 .. I       inode table         (32 inodes/block, 128 B each)
//    block I+1 ..         data blocks
//
//  device-agnostic: the caller supplies block read/write callbacks (the kernel
//  wires NVMe; a fuse driver would wire a host file). (satoru)

#define KFS_MAGIC        0x4B465331u   // "KFS1" (satoru)
#define KFS_VERSION      1u
#define KFS_BLOCK_SIZE   4096u
#define KFS_INODE_SIZE   128u
#define KFS_INODES_PER_BLOCK  (KFS_BLOCK_SIZE / KFS_INODE_SIZE)   // 32 (satoru)
#define KFS_NAME_MAX     55            // dirent is exactly 64 bytes (satoru)
#define KFS_DIRECT       13            // direct block pointers (52 KB direct) (satoru)
#define KFS_PTRS_PER_BLOCK (KFS_BLOCK_SIZE / 4)                   // 1024 (satoru)
#define KFS_MAX_FILE     ((KFS_DIRECT + KFS_PTRS_PER_BLOCK) * KFS_BLOCK_SIZE) // ~4.05 MB (satoru)
#define KFS_ROOT_INODE   1            // inode 0 reserved (means "none"); 1 = root dir (satoru)

enum KFSType : uint32_t { KFS_FREE = 0, KFS_FILE = 1, KFS_DIR = 2 };

//  superblock (block 0). (satoru)
struct KFSSuper {
    uint32_t magic;          // KFS_MAGIC (satoru)
    uint32_t version;
    uint32_t block_size;     // 4096 (satoru)
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t bitmap_start;   // first block of the free-block bitmap (satoru)
    uint32_t bitmap_blocks;
    uint32_t inode_start;    // first block of the inode table (satoru)
    uint32_t inode_blocks;
    uint32_t data_start;     // first data block (satoru)
    uint32_t free_blocks;    // running free-block count (satoru)
    uint32_t crc;            // crc32 of the fields above (satoru)
} __attribute__((packed));

//  inode  -  exactly 128 bytes. 13 direct + 1 single-indirect => ~4 MB max. (satoru)
struct KFSInode {
    uint32_t type;           // KFSType (satoru)
    uint16_t mode;           // unix rwxrwxrwx (satoru)
    uint16_t uid;
    uint16_t gid;
    uint16_t _pad0;
    uint32_t size;           // bytes (satoru)
    uint32_t ctime, mtime, atime;
    uint32_t nlink;
    uint32_t direct[KFS_DIRECT];   // 52 bytes (satoru)
    uint32_t indirect;             // block of KFS_PTRS_PER_BLOCK pointers (satoru)
    uint32_t _pad1[8];             // reserve to 128 bytes (satoru)
} __attribute__((packed));

//  directory entry  -  exactly 64 bytes; a directory's data blocks are arrays of
//  these (64 entries/block), inode==0 marks a free slot. (satoru)
struct KFSDirEnt {
    uint32_t inode;          // 0 = empty slot (satoru)
    uint16_t name_len;
    uint16_t type;           // KFSType hint for listings (satoru)
    char     name[KFS_NAME_MAX + 1];   // 56 bytes -> 64 total (satoru)
} __attribute__((packed));

//  block i/o backend (count = number of 4 KB blocks). (satoru)
typedef bool (*KFSReadFn)(uint64_t block, uint32_t count, void* buf, void* ctx);
typedef bool (*KFSWriteFn)(uint64_t block, uint32_t count, const void* buf, void* ctx);

class KFS {
public:
    //  wire the block device before Format/Mount. (satoru)
    static void SetBackend(KFSReadFn rd, KFSWriteFn wr, void* ctx);

    //  lay down a fresh empty volume spanning `total_blocks` 4 KB blocks (root
    //  dir created). returns false on a too-small device or i/o error. (satoru)
    static bool Format(uint32_t total_blocks);

    //  mount an existing volume (validates the superblock + crc); loads the
    //  bitmap + inode table into ram for fast operation. (satoru)
    static bool Mount();
    static bool IsMounted();
    //  flush all dirty metadata to disk (call after a batch of writes). (satoru)
    static bool Sync();

    //  path ops  -  absolute '/'-separated paths. (satoru)
    static bool Mkdirs(const char* path);                       // create dir + parents (satoru)
    static bool WriteFile(const char* path, const void* data, uint32_t len);
    static int  ReadFile(const char* path, void* buf, uint32_t max);   // bytes read, -1 on error
    static bool Exists(const char* path);
    static bool IsDir(const char* path);
    static int  FileSize(const char* path);                     // bytes, -1 if absent (satoru)

    //  enumerate a directory; cb is called per child. returns child count or -1.
    //  (used by the restore walk to rebuild the in-memory KVFS.) (satoru)
    typedef void (*ListCb)(const char* name, bool is_dir, void* ctx);
    static int  List(const char* path, ListCb cb, void* ctx);
};
// end (satoru)
