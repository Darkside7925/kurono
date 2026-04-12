#pragma once
//  kurono os  -  ext4 filesystem driver (read/write)
//  full ext4 support for accessing the linux partition from kurono

#include "../kernel/types.h"

#define EXT4_SUPER_MAGIC       0xEF53
#define EXT4_ROOT_INO          2
#define EXT4_NAME_LEN          255
#define EXT4_NDIR_BLOCKS       12
#define EXT4_IND_BLOCK         12
#define EXT4_DIND_BLOCK        13
#define EXT4_TIND_BLOCK        14
#define EXT4_N_BLOCKS          15
#define EXT4_BLOCK_SIZE_MIN    1024
#define EXT4_BLOCK_SIZE_MAX    65536
#define EXT4_MAX_OPEN_FILES    32
#define EXT4_MAX_PATH          256
#define EXT4_DIR_ENTRY_BUF     4096

// inode flags
#define EXT4_EXTENTS_FL        0x00080000
#define EXT4_INDEX_FL          0x00001000

// file types in directory entries
#define EXT4_FT_UNKNOWN        0
#define EXT4_FT_REG_FILE       1
#define EXT4_FT_DIR            2
#define EXT4_FT_CHRDEV         3
#define EXT4_FT_BLKDEV         4
#define EXT4_FT_FIFO           5
#define EXT4_FT_SOCK           6
#define EXT4_FT_SYMLINK        7

// s_ifmt bits
#define EXT4_S_IFDIR           0x4000
#define EXT4_S_IFREG           0x8000
#define EXT4_S_IFLNK           0xA000
#define EXT4_S_IFMT            0xF000

struct Ext4Superblock {
    uint32_t s_inodes_count;          // total inodes
    uint32_t s_blocks_count_lo;       // total blocks (low 32)
    uint32_t s_r_blocks_count_lo;     // reserved blocks (low 32)
    uint32_t s_free_blocks_count_lo;  // free blocks (low 32)
    uint32_t s_free_inodes_count;     // free inodes
    uint32_t s_first_data_block;      // first data block (usually 0 or 1)
    uint32_t s_log_block_size;        // block size = 1024 << s_log_block_size
    uint32_t s_log_cluster_size;
    uint32_t s_blocks_per_group;
    uint32_t s_clusters_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;                 // last mount time
    uint32_t s_wtime;                 // last write time
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;                 // must be 0xef53
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;

    // ext4_dynamic_rev fields
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algorithm_usage_bitmap;

    // performance hints
    uint8_t  s_prealloc_blocks;
    uint8_t  s_prealloc_dir_blocks;
    uint16_t s_reserved_gdt_blocks;

    // journaling
    uint8_t  s_journal_uuid[16];
    uint32_t s_journal_inum;
    uint32_t s_journal_dev;
    uint32_t s_last_orphan;
    uint32_t s_hash_seed[4];
    uint8_t  s_def_hash_version;
    uint8_t  s_jnl_backup_type;
    uint16_t s_desc_size;
    uint32_t s_default_mount_opts;
    uint32_t s_first_meta_bg;
    uint32_t s_mkfs_time;
    uint32_t s_jnl_blocks[17];

    // 64-bit support
    uint32_t s_blocks_count_hi;
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;
    uint32_t s_flags;
    uint16_t s_raid_stride;
    uint16_t s_mmp_update_interval;
    uint64_t s_mmp_block;
    uint32_t s_raid_stripe_width;
    uint8_t  s_log_groups_per_flex;
    uint8_t  s_checksum_type;
    uint16_t s_reserved_pad;
    uint64_t s_kbytes_written;

    uint8_t  s_padding[3584];   // pad to 4096 if needed (superblock is 1024 real)
} __attribute__((packed));

struct Ext4GroupDesc {
    uint32_t bg_block_bitmap_lo;
    uint32_t bg_inode_bitmap_lo;
    uint32_t bg_inode_table_lo;
    uint16_t bg_free_blocks_count_lo;
    uint16_t bg_free_inodes_count_lo;
    uint16_t bg_used_dirs_count_lo;
    uint16_t bg_flags;
    uint32_t bg_exclude_bitmap_lo;
    uint16_t bg_block_bitmap_csum_lo;
    uint16_t bg_inode_bitmap_csum_lo;
    uint16_t bg_itable_unused_lo;
    uint16_t bg_checksum;

    // 64-bit fields (when s_desc_size >= 64)
    uint32_t bg_block_bitmap_hi;
    uint32_t bg_inode_bitmap_hi;
    uint32_t bg_inode_table_hi;
    uint16_t bg_free_blocks_count_hi;
    uint16_t bg_free_inodes_count_hi;
    uint16_t bg_used_dirs_count_hi;
    uint16_t bg_itable_unused_hi;
    uint32_t bg_exclude_bitmap_hi;
    uint16_t bg_block_bitmap_csum_hi;
    uint16_t bg_inode_bitmap_csum_hi;
    uint32_t bg_reserved;
} __attribute__((packed));

struct Ext4Inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size_lo;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks_lo;       // 512-byte block count
    uint32_t i_flags;
    uint32_t i_osd1;

    union {
        uint32_t i_block[EXT4_N_BLOCKS];         // traditional block map
        struct {
            uint16_t eh_magic;    // 0xf30a
            uint16_t eh_entries;
            uint16_t eh_max;
            uint16_t eh_depth;
            uint32_t eh_generation;
            // extent entries follow
            uint8_t  extents_data[48]; // remaining space for extent entries
        } __attribute__((packed)) extent_header;
    };

    uint32_t i_generation;
    uint32_t i_file_acl_lo;
    uint32_t i_size_high;     // upper 32 bits of file size (for >2gb files)
    uint32_t i_obso_faddr;
    uint8_t  i_osd2[12];

    uint16_t i_extra_isize;
    uint16_t i_checksum_hi;
    uint32_t i_ctime_extra;
    uint32_t i_mtime_extra;
    uint32_t i_atime_extra;
    uint32_t i_crtime;
    uint32_t i_crtime_extra;
    uint32_t i_version_hi;
    uint32_t i_projid;
} __attribute__((packed));

struct Ext4ExtentHeader {
    uint16_t eh_magic;     // 0xf30a
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
} __attribute__((packed));

struct Ext4ExtentIdx {
    uint32_t ei_block;      // logical block covered
    uint32_t ei_leaf_lo;    // pointer to child extent node
    uint16_t ei_leaf_hi;
    uint16_t ei_unused;
} __attribute__((packed));

struct Ext4Extent {
    uint32_t ee_block;      // first logical block
    uint16_t ee_len;        // number of blocks
    uint16_t ee_start_hi;   // high 16 bits of physical block
    uint32_t ee_start_lo;   // low 32 bits of physical block
} __attribute__((packed));

struct Ext4DirEntry2 {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[EXT4_NAME_LEN];
} __attribute__((packed));

struct Ext4File {
    uint32_t inode_num;
    Ext4Inode inode;
    uint64_t offset;
    bool     open;
    uint8_t  flags;   // 1=read, 2=write
};

struct Ext4DirInfo {
    char     name[EXT4_NAME_LEN + 1];
    uint32_t inode;
    uint8_t  file_type;
    uint32_t size;
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
};

// the ext4 driver doesn't directly access hardware; it uses a registered
// block i/o callback to read/write sectors.  the kls or disk driver
// provides this at mount time.

typedef int (*Ext4BlockRead)(uint64_t byte_offset, uint32_t len, void* buf,
                              void* dev_ctx);
typedef int (*Ext4BlockWrite)(uint64_t byte_offset, uint32_t len,
                               const void* buf, void* dev_ctx);

//  ext4 driver  -  class interface

class Ext4 {
public:
    // mount / unmount
    static int  Mount(Ext4BlockRead read_fn, Ext4BlockWrite write_fn,
                      void* dev_ctx, uint64_t part_offset = 0);
    static void Unmount();
    static bool IsMounted();

    // path operations
    static int  LookupInode(const char* path, uint32_t* ino_out);
    static int  ReadInode(uint32_t ino, Ext4Inode* out);
    static int  ReadInodeByPath(const char* path, Ext4Inode* out);

    // file operations
    static int  Open(const char* path, uint8_t flags);   // returns fd or <0
    static int  Read(int fd, void* buf, uint32_t len);
    static int  Write(int fd, const void* buf, uint32_t len);
    static int  Seek(int fd, int64_t offset, int whence);
    static int  Close(int fd);
    static int64_t FileSize(const char* path);

    // directory listing
    static int  ListDir(const char* path, Ext4DirInfo* entries, int max);

    // metadata
    static int  Stat(const char* path, Ext4Inode* out);
    static bool Exists(const char* path);
    static bool IsDir(const char* path);
    static bool IsFile(const char* path);
    static int  ReadLink(const char* path, char* buf, int max);

    // write operations
    static int  CreateFile(const char* path, uint16_t mode);
    static int  WriteFile(const char* path, const void* data, uint32_t len);
    static int  Mkdir(const char* path, uint16_t mode);
    static int  Unlink(const char* path);
    static int  Rmdir(const char* path);

    // whole-file read helper
    static int  ReadWholeFile(const char* path, void* buf, uint32_t max_len);
    static int  ReadString(const char* path, char* buf, int max_len);
    static int  WriteString(const char* path, const char* str);

    // info
    static uint64_t TotalBlocks();
    static uint64_t FreeBlocks();
    static uint32_t BlockSize();
    static const char* VolumeName();

private:
    static bool           mounted;
    static Ext4Superblock  sb;
    static Ext4BlockRead   blk_read;
    static Ext4BlockWrite  blk_write;
    static void*           blk_ctx;
    static uint64_t        partition_offset;
    static uint32_t        block_size;
    static uint32_t        groups_count;
    static uint32_t        inodes_per_group;
    static uint32_t        inode_size;
    static uint16_t        desc_size;
    static Ext4File        open_files[EXT4_MAX_OPEN_FILES];

    // cache
    static uint8_t         block_cache[EXT4_BLOCK_SIZE_MAX];
    static uint64_t        cached_block;

    // internal helpers
    static int  ReadBlock(uint64_t block_num, void* buf);
    static int  WriteBlock(uint64_t block_num, const void* buf);
    static int  ReadBytes(uint64_t offset, uint32_t len, void* buf);
    static int  WriteBytes(uint64_t offset, uint32_t len, const void* buf);
    static int  GetGroupDesc(uint32_t group, Ext4GroupDesc* out);
    static int  GetInodeBlock(uint32_t ino, uint64_t* block_out,
                              uint32_t* offset_out);

    // extent tree walking
    static uint64_t ExtentLogicalToPhysical(Ext4Inode* inode,
                                             uint32_t logical_block);
    static uint64_t ExtentWalkIndex(uint64_t index_block,
                                     uint32_t logical_block, int depth);

    // traditional block map
    static uint64_t BlockMapLogical(Ext4Inode* inode, uint32_t logical_block);

    // read inode data (either extent or block-map)
    static int  ReadInodeData(Ext4Inode* inode, uint64_t offset,
                               uint32_t len, void* buf);
    static int  WriteInodeData(Ext4Inode* inode, uint32_t ino,
                                uint64_t offset, uint32_t len,
                                const void* buf);

    // directory helpers
    static int  DirLookup(Ext4Inode* dir_inode, const char* name,
                           uint32_t* ino_out);
    static int  DirAddEntry(uint32_t dir_ino, uint32_t new_ino,
                             const char* name, uint8_t file_type);
    static int  DirRemoveEntry(uint32_t dir_ino, const char* name);

    // allocation
    static int  AllocBlock(uint32_t hint_group, uint64_t* block_out);
    static int  AllocInode(uint32_t hint_group, uint32_t* ino_out);
    static void FreeBlock(uint64_t block);
    static void FreeInode(uint32_t ino);

    // path parsing
    static int  PathWalk(const char* path, uint32_t* ino_out);
    static void SplitPath(const char* path, char* parent, char* basename);
};
