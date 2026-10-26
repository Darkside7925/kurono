#pragma once
#include "../kernel/types.h"

//  KFS - Kurono File System.
//
//  a small, fast, inode-based on-disk filesystem designed from scratch for
//  Kurono - no linux baggage. it is the on-disk PERSISTENCE layer: the in-memory
//  KVFS stays the runtime filesystem, and KFS stores the user-data subset across
//  reboots as REAL files + directories (not an opaque blob), so a volume is
//  browsable + a Linux kfs-fuse driver could mount it later (the on-disk format
//  below is the spec for that).
//
//  on-disk layout (4 KB blocks, all multi-byte fields little-endian):
//    block 0              superblock
//    block 1 .. B         free-block bitmap   (1 bit/block, bit set = used)
//    block B+1 .. I       inode table         (16 inodes/block, 256 B each)
//    block I+1 ..         data blocks
//
//  EXTENT-BASED layout (KFS v2): a file/dir is a list of extents
//  {start_lba, length_blocks} instead of per-block pointers. the inode holds
//  KFS_INLINE_EXTENTS inline; when a file needs more, the inode chains to an
//  "extent overflow" block (itself a list of extents + a next pointer), so a
//  file is limited only by free disk space - NO ~4 MB direct/indirect cap. the
//  bump allocator hands out one contiguous run per file, so the common case is a
//  SINGLE extent (a 174 MB binary = 1 extent, not 43520 pointers). directories
//  use the same extent machinery, so there is no max-dir-entry cap either.
//
//  inline data: a tiny file (<= KFS_INLINE_MAX bytes) is stored directly in the
//  inode's extent area (no data block at all).
//
//  device-agnostic: the caller supplies block read/write callbacks (the kernel
//  wires NVMe; a fuse driver would wire a host file). (satoru)

#define KFS_MAGIC        0x4B465332u   // "KFS2" - extent-based on-disk format (satoru)
#define KFS_MAGIC_V1     0x4B465331u   // "KFS1" - legacy direct/indirect format (satoru)
#define KFS_VERSION      2u
#define KFS_BLOCK_SIZE   4096u
#define KFS_INODE_SIZE   256u          // grew 128->256 to hold more inline extents (satoru)
#define KFS_INODES_PER_BLOCK  (KFS_BLOCK_SIZE / KFS_INODE_SIZE)   // 16 (satoru)
#define KFS_NAME_MAX     55            // dirent is exactly 64 bytes (satoru)
#define KFS_ROOT_INODE   1            // inode 0 reserved (means "none"); 1 = root dir (satoru)

//  an extent: a contiguous run of `len` 4 KB blocks starting at block `start`.
//  len == 0 marks an unused slot. (satoru)
struct KFSExtent {
    uint32_t start;   // first data block of the run (satoru)
    uint32_t len;     // number of contiguous 4 KB blocks (satoru)
} __attribute__((packed));

//  how many extents live inline in the inode, and how many in one overflow block.
//  with a 256-byte inode the fixed header is 72 bytes, leaving 184 bytes -> 23
//  inline extents (8 bytes each). inline data shares that same 184-byte area. (satoru)
#define KFS_INLINE_EXTENTS  23
#define KFS_INLINE_MAX      (KFS_INLINE_EXTENTS * (uint32_t)sizeof(KFSExtent))  // 184 bytes inline (satoru)
//  one 4 KB overflow block: a small header (next pointer + count) then extents.
//  (4096 - 8) / 8 = 511 extents per overflow block. each extent can describe a
//  run far larger than a block, so the chain is effectively unbounded. (satoru)
#define KFS_EXT_PER_OVF     ((KFS_BLOCK_SIZE - 8) / (uint32_t)sizeof(KFSExtent))

enum KFSType : uint32_t { KFS_FREE = 0, KFS_FILE = 1, KFS_DIR = 2, KFS_SYMLINK = 3 };

//  inode flags. (satoru)
#define KFS_FLAG_INLINE  0x1u   // file content is stored inline in the inode (satoru)

//  superblock (block 0). carries the log-structured / checkpoint plumbing fields
//  even though the snapshot writer doesn't use them yet (they're reserved for the
//  log-structured + checkpoint layers and read back as 0 on a v2-snapshot
//  volume). (satoru)
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
    uint32_t inode_size;     // 256 in v2 (was implicitly 128 in v1) (satoru)
    uint32_t checkpoint_gen; // monotonically rising; the cleaner / checkpoint use it (satoru)
    uint32_t reserved[3];    // future log-structured fields (satoru)
    uint32_t crc;            // crc32 of the fields above (satoru)
} __attribute__((packed));

//  inode - exactly 256 bytes. fixed 72-byte header + 184-byte inline area that
//  holds either KFS_INLINE_EXTENTS extents OR (for a tiny file) inline data. an
//  overflow block chain extends the extent list with no size cap. (satoru)
struct KFSInode {
    uint32_t type;           // KFSType (satoru)
    uint16_t mode;           // unix rwxrwxrwx (satoru)
    uint16_t uid;
    uint16_t gid;
    uint16_t gid_pad;        // (kept for layout symmetry) (satoru)
    uint16_t flags;          // KFS_FLAG_* (satoru)
    uint64_t size;           // bytes - 64-bit so files exceed 4 GB (satoru)
    uint32_t ctime, mtime, atime;
    uint32_t nlink;
    uint32_t blocks;         // total data blocks across all extents (satoru)
    uint32_t ext_count;      // number of valid extents (inline + overflow) (satoru)
    uint32_t ext_overflow;   // first overflow block, or 0 (satoru)
    uint8_t  _hdrpad[22];    // pad the fixed header to exactly 72 bytes (satoru)
    union {
        KFSExtent extent[KFS_INLINE_EXTENTS];   // inline extent list (satoru)
        uint8_t   inline_data[KFS_INLINE_MAX];  // OR tiny-file content (satoru)
    };
} __attribute__((packed));

//  one extent-overflow block: next-pointer + count, then a packed extent array.
//  chained off KFSInode.ext_overflow for files/dirs that exceed the inline slots.
//  (satoru)
struct KFSExtOverflow {
    uint32_t next;           // next overflow block, or 0 (satoru)
    uint32_t count;          // valid extents in this block (satoru)
    KFSExtent ext[KFS_EXT_PER_OVF];
} __attribute__((packed));

//  directory entry - exactly 64 bytes; a directory's data blocks are arrays of
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

//  benchmark / diagnostic counters the bench harness reads to show per-op extent
//  efficiency without instrumenting the driver. (satoru)
struct KFSStats {
    uint64_t files_written;
    uint64_t dirs_made;
    uint64_t extents_used;     // total extents allocated (1 per contiguous file = ideal) (satoru)
    uint64_t inline_files;     // files stored without any data block (satoru)
    uint64_t bytes_written;
};

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
    //  free the in-ram metadata caches (~36 mb on the 4 gb data disk) and mark
    //  unmounted. writers must Sync() first; a later Mount() re-reads from
    //  disk. (satoru)
    static void Unmount();
    //  flush all dirty metadata to disk (call after a batch of writes). (satoru)
    static bool Sync();

    //  path ops - absolute '/'-separated paths. (satoru)
    static bool Mkdirs(const char* path);                       // create dir + parents (satoru)
    static bool WriteFile(const char* path, const void* data, uint64_t len);
    static int64_t ReadFile(const char* path, void* buf, uint64_t max);   // bytes read, -1 on error
    static bool Symlink(const char* path, const char* target);  // create a symlink node (satoru)
    static int  ReadLink(const char* path, char* buf, int max); // target len, -1 if not a symlink (satoru)
    static bool Exists(const char* path);
    static bool IsDir(const char* path);
    static bool IsSymlink(const char* path);
    static int64_t FileSize(const char* path);                  // bytes, -1 if absent (satoru)

    //  enumerate a directory; cb is called per child. returns child count or -1.
    //  (used by the restore walk to rebuild the in-memory KVFS.) (satoru)
    typedef void (*ListCb)(const char* name, bool is_dir, void* ctx);
    static int  List(const char* path, ListCb cb, void* ctx);

    //  live counters (reset at Format). (satoru)
    static const KFSStats& Stats();
    static void ResetStats();

    //  layer 6 - incremental snapshot. the 64-bit content fingerprint of the last
    //  saved user-data tree is persisted in the superblock (reserved fields), so a
    //  save can compare the current tree's fingerprint against it and skip the
    //  whole reformat+rewrite when nothing changed. these read/write the MOUNTED
    //  superblock's stored fingerprint; the caller computes the fingerprint. (satoru)
    static uint64_t MountedFingerprint();              // 0 if not mounted / unset (satoru)
    static void     SetFingerprint(uint64_t fp);       // stamp it; flushed by Sync() (satoru)
};
// end (satoru)
