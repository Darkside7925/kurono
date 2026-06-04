#pragma once

#include "../kernel/types.h"

#define FAT32_ATTR_READONLY  0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_EOC            0x0FFFFFF8u
#define FAT32_BAD            0x0FFFFFF7u
#define FAT32_FREE           0x00000000u

#define FAT32_CACHE_BLOCKS   16
#define FAT32_CACHE_BLOCK    4096
#define FAT32_DIR_CACHE      16
#define FAT32_MAX_CLUSTER_BYTES 65536

typedef int (*Fat32BlockRead)(uint64_t byte_offset, uint32_t len, void* buf, void* ctx);
typedef int (*Fat32BlockWrite)(uint64_t byte_offset, uint32_t len, const void* buf, void* ctx);

class FAT32 {
public:
    static int  Mount(Fat32BlockRead read_fn, Fat32BlockWrite write_fn, void* dev_ctx, uint64_t part_offset = 0);
    static void Unmount();
    static bool IsMounted();
    static int  Sync();

    static int  Mkdirs(const char* path);
    static int  WriteFile(const char* path, const void* data, uint32_t len);
    static bool Exists(const char* path);

private:
    static bool mounted;
    static Fat32BlockRead blk_read;
    static Fat32BlockWrite blk_write;
    static void* blk_ctx;
    static uint64_t partition_offset;

    static uint16_t bytes_per_sector;
    static uint8_t  sectors_per_cluster;
    static uint16_t reserved_sectors;
    static uint8_t  fat_count;
    static uint32_t sectors_per_fat;
    static uint32_t root_cluster;
    static uint32_t total_sectors;
    static uint64_t fat_offset;
    static uint64_t data_offset;
    static uint32_t cluster_size;
    static uint32_t total_clusters;
    static uint32_t next_free_hint;

    // 4 KiB block LRU cache in front of underlying device
    struct CacheEntry {
        uint64_t off;       // device byte offset (aligned to FAT32_CACHE_BLOCK)
        uint32_t lru;       // higher = more recent
        bool     valid;
        bool     dirty;
        uint8_t  data[FAT32_CACHE_BLOCK];
    };
    static CacheEntry cache[FAT32_CACHE_BLOCKS];
    static uint32_t cache_clock;

    // dir-inode path cache: maps recently-walked path strings to dir cluster
    struct DirCacheEntry {
        char path[128];
        uint32_t cluster;
        uint32_t lru;
        bool valid;
    };
    static DirCacheEntry dir_cache[FAT32_DIR_CACHE];
    static uint32_t dir_cache_clock;

    // preallocated scratch buffer (cluster-sized) for hot read/write paths;
    // avoids heap allocation per operation
    static uint8_t scratch[FAT32_MAX_CLUSTER_BYTES];
    static uint8_t scratch2[FAT32_MAX_CLUSTER_BYTES];

    static int  RawRead(uint64_t off, uint32_t len, void* buf);
    static int  RawWrite(uint64_t off, uint32_t len, const void* buf);
    static int  ReadBytes(uint64_t off, uint32_t len, void* buf);
    static int  WriteBytes(uint64_t off, uint32_t len, const void* buf);
    static int  FlushCache();
    static void InvalidateCache();
    static uint64_t ClusterOffset(uint32_t cluster);
    static int  ReadCluster(uint32_t cluster, void* buf);
    static int  WriteCluster(uint32_t cluster, const void* buf);
    static uint32_t ReadFAT(uint32_t cluster);
    static int  WriteFAT(uint32_t cluster, uint32_t value);
    static bool IsClusterValid(uint32_t cluster);
    static uint32_t AllocCluster();
    static int  ClearCluster(uint32_t cluster);
    static int  FindPath(const char* path, uint32_t* parent_cluster, char short_name[11], uint32_t* found_cluster, uint8_t* found_attr, bool want_parent_only);
    static int  FindEntryInDir(uint32_t dir_cluster, const char short_name[11], uint32_t* found_cluster, uint8_t* found_attr, uint32_t* found_entry_cluster, uint32_t* found_entry_offset);
    static int  AddEntryToDir(uint32_t dir_cluster, const char short_name[11], uint8_t attr, uint32_t first_cluster, uint32_t file_size);
    static int  CreateDir(uint32_t parent_cluster, const char short_name[11], uint32_t* new_cluster_out);
    static int  EnsureDirPath(const char* path, uint32_t* dir_cluster_out);
    static void MakeShortName(const char* src, char out[11]);
    static uint32_t LookupDirCache(const char* path);
    static void InsertDirCache(const char* path, uint32_t cluster);
};
