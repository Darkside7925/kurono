#include "fat32.h"
#include "../kernel/heap.h"
#include "../drivers/serial.h"

#pragma pack(push, 1)
struct FAT32BPB {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors16;
    uint8_t  media;
    uint16_t fat_size16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors32;
    uint32_t fat_size32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t bk_boot_sec;
    uint8_t  reserved[12];
    uint8_t  drive_num;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
};

struct FAT32DirEntry {
    char     name[11];
    uint8_t  attr;
    uint8_t  ntres;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t last_access_date;
    uint16_t first_cluster_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
};
#pragma pack(pop)

bool FAT32::mounted = false;
Fat32BlockRead FAT32::blk_read = nullptr;
Fat32BlockWrite FAT32::blk_write = nullptr;
void* FAT32::blk_ctx = nullptr;
uint64_t FAT32::partition_offset = 0;
uint16_t FAT32::bytes_per_sector = 512;
uint8_t FAT32::sectors_per_cluster = 1;
uint16_t FAT32::reserved_sectors = 0;
uint8_t FAT32::fat_count = 2;
uint32_t FAT32::sectors_per_fat = 0;
uint32_t FAT32::root_cluster = 2;
uint32_t FAT32::total_sectors = 0;
uint64_t FAT32::fat_offset = 0;
uint64_t FAT32::data_offset = 0;
uint32_t FAT32::cluster_size = 512;
uint32_t FAT32::total_clusters = 0;
uint32_t FAT32::next_free_hint = 2;

FAT32::CacheEntry FAT32::cache[FAT32_CACHE_BLOCKS];
uint32_t FAT32::cache_clock = 0;
FAT32::DirCacheEntry FAT32::dir_cache[FAT32_DIR_CACHE];
uint32_t FAT32::dir_cache_clock = 0;
uint8_t FAT32::scratch[FAT32_MAX_CLUSTER_BYTES];
uint8_t FAT32::scratch2[FAT32_MAX_CLUSTER_BYTES];

namespace {
static bool f_eq11(const char a[11], const char b[11]) {
    for (int i = 0; i < 11; i++) if (a[i] != b[i]) return false;
    return true;
}

static bool is_end(uint8_t c) { return c == 0x00; }
static bool is_deleted(uint8_t c) { return c == 0xE5; }
static bool is_lfn(uint8_t attr) { return (attr & 0x3F) == 0x0F; }

static int path_len_norm(const char* p) {
    int n = 0;
    while (p[n]) n++;
    while (n > 1 && p[n - 1] == '/') n--;
    return n;
}
}

int FAT32::RawRead(uint64_t off, uint32_t len, void* buf) {
    if (!blk_read) return -1;
    return blk_read(partition_offset + off, len, buf, blk_ctx);
}

int FAT32::RawWrite(uint64_t off, uint32_t len, const void* buf) {
    if (!blk_write) return -1;
    return blk_write(partition_offset + off, len, buf, blk_ctx);
}

void FAT32::InvalidateCache() {
    for (int i = 0; i < FAT32_CACHE_BLOCKS; i++) {
        cache[i].valid = false;
        cache[i].dirty = false;
        cache[i].lru = 0;
    }
    cache_clock = 0;
    for (int i = 0; i < FAT32_DIR_CACHE; i++) {
        dir_cache[i].valid = false;
        dir_cache[i].lru = 0;
        dir_cache[i].path[0] = 0;
    }
    dir_cache_clock = 0;
}

int FAT32::FlushCache() {
    int err = 0;
    for (int i = 0; i < FAT32_CACHE_BLOCKS; i++) {
        if (cache[i].valid && cache[i].dirty) {
            if (RawWrite(cache[i].off, FAT32_CACHE_BLOCK, cache[i].data) != 0) err = -1;
            else cache[i].dirty = false;
        }
    }
    return err;
}

// Reads through 4KiB LRU cache. Sequential reads are accelerated by read-around:
// when we miss on block N and the request is small, we fault in just block N
// (callers doing larger I/O issue multiple aligned 4K fetches naturally).
int FAT32::ReadBytes(uint64_t off, uint32_t len, void* buf) {
    uint8_t* dst = (uint8_t*)buf;
    while (len > 0) {
        uint64_t blk_off = off & ~(uint64_t)(FAT32_CACHE_BLOCK - 1);
        uint32_t in_blk = (uint32_t)(off - blk_off);
        uint32_t chunk = FAT32_CACHE_BLOCK - in_blk;
        if (chunk > len) chunk = len;

        int hit = -1;
        int victim = 0;
        uint32_t worst = 0xFFFFFFFFu;
        for (int i = 0; i < FAT32_CACHE_BLOCKS; i++) {
            if (cache[i].valid && cache[i].off == blk_off) { hit = i; break; }
            if (!cache[i].valid) { victim = i; worst = 0; }
            else if (!cache[i].dirty && cache[i].lru < worst) { victim = i; worst = cache[i].lru; }
        }
        if (hit < 0) {
            // all clean entries chosen, but if every slot is dirty we must flush one
            if (cache[victim].valid && cache[victim].dirty) {
                if (RawWrite(cache[victim].off, FAT32_CACHE_BLOCK, cache[victim].data) != 0) return -1;
                cache[victim].dirty = false;
            }
            if (RawRead(blk_off, FAT32_CACHE_BLOCK, cache[victim].data) != 0) {
                cache[victim].valid = false;
                return -1;
            }
            cache[victim].off = blk_off;
            cache[victim].valid = true;
            cache[victim].dirty = false;
            hit = victim;
        }
        cache[hit].lru = ++cache_clock;
        for (uint32_t i = 0; i < chunk; i++) dst[i] = cache[hit].data[in_blk + i];
        dst += chunk;
        off += chunk;
        len -= chunk;
    }
    return 0;
}

int FAT32::WriteBytes(uint64_t off, uint32_t len, const void* buf) {
    const uint8_t* src = (const uint8_t*)buf;
    while (len > 0) {
        uint64_t blk_off = off & ~(uint64_t)(FAT32_CACHE_BLOCK - 1);
        uint32_t in_blk = (uint32_t)(off - blk_off);
        uint32_t chunk = FAT32_CACHE_BLOCK - in_blk;
        if (chunk > len) chunk = len;

        int hit = -1;
        int victim = 0;
        uint32_t worst = 0xFFFFFFFFu;
        for (int i = 0; i < FAT32_CACHE_BLOCKS; i++) {
            if (cache[i].valid && cache[i].off == blk_off) { hit = i; break; }
            if (!cache[i].valid) { victim = i; worst = 0; }
            else if (cache[i].lru < worst) { victim = i; worst = cache[i].lru; }
        }
        if (hit < 0) {
            if (cache[victim].valid && cache[victim].dirty) {
                if (RawWrite(cache[victim].off, FAT32_CACHE_BLOCK, cache[victim].data) != 0) return -1;
                cache[victim].dirty = false;
            }
            // only read if we're not overwriting the whole block
            if (chunk != FAT32_CACHE_BLOCK) {
                if (RawRead(blk_off, FAT32_CACHE_BLOCK, cache[victim].data) != 0) {
                    cache[victim].valid = false;
                    return -1;
                }
            }
            cache[victim].off = blk_off;
            cache[victim].valid = true;
            hit = victim;
        }
        cache[hit].lru = ++cache_clock;
        for (uint32_t i = 0; i < chunk; i++) cache[hit].data[in_blk + i] = src[i];
        cache[hit].dirty = true;
        src += chunk;
        off += chunk;
        len -= chunk;
    }
    return 0;
}

uint64_t FAT32::ClusterOffset(uint32_t cluster) {
    return data_offset + (uint64_t)(cluster - 2) * cluster_size;
}

bool FAT32::IsClusterValid(uint32_t cluster) {
    return cluster >= 2 && cluster < total_clusters + 2 && cluster < FAT32_BAD;
}

int FAT32::ReadCluster(uint32_t cluster, void* buf) {
    if (!IsClusterValid(cluster)) return -1;
    return ReadBytes(ClusterOffset(cluster), cluster_size, buf);
}

int FAT32::WriteCluster(uint32_t cluster, const void* buf) {
    if (!IsClusterValid(cluster)) return -1;
    return WriteBytes(ClusterOffset(cluster), cluster_size, buf);
}

uint32_t FAT32::ReadFAT(uint32_t cluster) {
    if (cluster >= total_clusters + 2) return FAT32_EOC;
    uint32_t value = 0;
    uint64_t off = fat_offset + (uint64_t)cluster * 4;
    if (ReadBytes(off, 4, &value) != 0) return FAT32_EOC;
    return value & 0x0FFFFFFF;
}

int FAT32::WriteFAT(uint32_t cluster, uint32_t value) {
    if (cluster >= total_clusters + 2) return -1;
    value &= 0x0FFFFFFF;
    for (int i = 0; i < fat_count; i++) {
        uint64_t off = fat_offset + (uint64_t)i * sectors_per_fat * bytes_per_sector + (uint64_t)cluster * 4;
        if (WriteBytes(off, 4, &value) != 0) return -1;
    }
    return 0;
}

uint32_t FAT32::AllocCluster() {
    if (total_clusters == 0) return 0;
    uint32_t start = next_free_hint < 2 ? 2 : next_free_hint;
    uint32_t end = total_clusters + 2;
    for (uint32_t pass = 0; pass < 2; pass++) {
        uint32_t from = pass == 0 ? start : 2;
        uint32_t to = pass == 0 ? end : start;
        for (uint32_t c = from; c < to; c++) {
            uint32_t v = ReadFAT(c);
            if (v == FAT32_FREE) {
                if (WriteFAT(c, FAT32_EOC) != 0) return 0;
                if (ClearCluster(c) != 0) return 0;
                next_free_hint = c + 1;
                return c;
            }
        }
    }
    return 0;
}

int FAT32::ClearCluster(uint32_t cluster) {
    if (!IsClusterValid(cluster)) return -1;
    for (uint32_t i = 0; i < cluster_size; i++) scratch[i] = 0;
    return WriteCluster(cluster, scratch);
}

void FAT32::MakeShortName(const char* src, char out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    if (!src) return;
    int di = 0;
    int ei = 8;
    bool ext = false;
    int last_dot = -1;
    for (int i = 0; src[i]; i++) if (src[i] == '.') last_dot = i;
    for (int i = 0; src[i]; i++) {
        char c = src[i];
        if (i == last_dot) { ext = true; continue; }
        if (c >= 'a' && c <= 'z') c -= 32;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '~') {
            if (!ext && di < 8) out[di++] = c;
            else if (ext && ei < 11) out[ei++] = c;
        }
    }
}

uint32_t FAT32::LookupDirCache(const char* path) {
    int plen = path_len_norm(path);
    if (plen <= 0 || plen >= (int)sizeof(dir_cache[0].path)) return 0;
    for (int i = 0; i < FAT32_DIR_CACHE; i++) {
        if (!dir_cache[i].valid) continue;
        bool eq = true;
        for (int k = 0; k < plen; k++) {
            if (dir_cache[i].path[k] != path[k]) { eq = false; break; }
        }
        if (eq && dir_cache[i].path[plen] == 0) {
            dir_cache[i].lru = ++dir_cache_clock;
            return dir_cache[i].cluster;
        }
    }
    return 0;
}

void FAT32::InsertDirCache(const char* path, uint32_t cluster) {
    int plen = path_len_norm(path);
    if (plen <= 0 || plen >= (int)sizeof(dir_cache[0].path)) return;
    int victim = 0;
    uint32_t worst = 0xFFFFFFFFu;
    for (int i = 0; i < FAT32_DIR_CACHE; i++) {
        if (!dir_cache[i].valid) { victim = i; break; }
        if (dir_cache[i].lru < worst) { victim = i; worst = dir_cache[i].lru; }
    }
    for (int k = 0; k < plen; k++) dir_cache[victim].path[k] = path[k];
    dir_cache[victim].path[plen] = 0;
    dir_cache[victim].cluster = cluster;
    dir_cache[victim].valid = true;
    dir_cache[victim].lru = ++dir_cache_clock;
}

int FAT32::FindEntryInDir(uint32_t dir_cluster, const char short_name[11], uint32_t* found_cluster, uint8_t* found_attr, uint32_t* found_entry_cluster, uint32_t* found_entry_offset, uint32_t* found_size) {
    uint32_t cluster = dir_cluster;
    uint32_t cl_sz = cluster_size;
    uint32_t guard = 0;
    uint32_t max_chain = total_clusters + 2;

    while (IsClusterValid(cluster)) {
        if (guard++ > max_chain) return -1;
        if (ReadCluster(cluster, scratch) != 0) return -1;
        for (uint32_t off = 0; off < cl_sz; off += 32) {
            FAT32DirEntry* de = (FAT32DirEntry*)(scratch + off);
            if (is_end((uint8_t)de->name[0])) return 1;
            if (is_deleted((uint8_t)de->name[0]) || is_lfn(de->attr)) continue;
            if (f_eq11(de->name, short_name)) {
                if (found_cluster) *found_cluster = ((uint32_t)de->first_cluster_hi << 16) | de->first_cluster_lo;
                if (found_attr) *found_attr = de->attr;
                if (found_entry_cluster) *found_entry_cluster = cluster;
                if (found_entry_offset) *found_entry_offset = off;
                if (found_size) *found_size = de->file_size;   // for the read path (satoru)
                return 0;
            }
        }
        uint32_t next = ReadFAT(cluster);
        if (next >= FAT32_EOC || next == FAT32_BAD || next < 2) break;
        cluster = next;
    }
    return 1;
}

int FAT32::CreateDir(uint32_t parent_cluster, const char short_name[11], uint32_t* new_cluster_out) {
    uint32_t new_cluster = AllocCluster();
    if (!new_cluster) return -1;

    uint32_t cl_sz = cluster_size;
    for (uint32_t i = 0; i < cl_sz; i++) scratch2[i] = 0;

    FAT32DirEntry* dot = (FAT32DirEntry*)scratch2;
    for (int i = 0; i < 11; i++) dot->name[i] = ' ';
    dot->name[0] = '.';
    dot->attr = FAT32_ATTR_DIRECTORY;
    dot->first_cluster_hi = (uint16_t)(new_cluster >> 16);
    dot->first_cluster_lo = (uint16_t)(new_cluster & 0xFFFF);

    FAT32DirEntry* dotdot = (FAT32DirEntry*)(scratch2 + 32);
    for (int i = 0; i < 11; i++) dotdot->name[i] = ' ';
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attr = FAT32_ATTR_DIRECTORY;
    uint32_t pp = (parent_cluster == root_cluster) ? 0 : parent_cluster;
    dotdot->first_cluster_hi = (uint16_t)(pp >> 16);
    dotdot->first_cluster_lo = (uint16_t)(pp & 0xFFFF);

    if (WriteCluster(new_cluster, scratch2) != 0) {
        WriteFAT(new_cluster, FAT32_FREE);
        return -1;
    }
    int r = AddEntryToDir(parent_cluster, short_name, FAT32_ATTR_DIRECTORY, new_cluster, 0);
    if (r != 0) {
        WriteFAT(new_cluster, FAT32_FREE);
        return r;
    }
    if (new_cluster_out) *new_cluster_out = new_cluster;
    return 0;
}

int FAT32::AddEntryToDir(uint32_t dir_cluster, const char short_name[11], uint8_t attr, uint32_t first_cluster, uint32_t file_size) {
    uint32_t cluster = dir_cluster;
    uint32_t prev = cluster;
    uint32_t cl_sz = cluster_size;
    uint32_t guard = 0;
    uint32_t max_chain = total_clusters + 2;

    while (IsClusterValid(cluster)) {
        if (guard++ > max_chain) return -1;
        if (ReadCluster(cluster, scratch) != 0) return -1;
        for (uint32_t off = 0; off < cl_sz; off += 32) {
            FAT32DirEntry* de = (FAT32DirEntry*)(scratch + off);
            uint8_t marker = (uint8_t)de->name[0];
            if (is_end(marker) || is_deleted(marker)) {
                bool was_end = is_end(marker);
                for (int i = 0; i < 11; i++) de->name[i] = short_name[i];
                de->attr = attr;
                de->ntres = 0;
                de->crt_time_tenth = 0;
                de->crt_time = 0;
                de->crt_date = 0;
                de->last_access_date = 0;
                de->first_cluster_hi = (uint16_t)(first_cluster >> 16);
                de->first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
                de->wrt_time = 0;
                de->wrt_date = 0;
                de->file_size = file_size;
                // if we reused an end-of-dir slot, make sure the very next entry
                // is still a terminator so the directory remains well-formed
                if (was_end && off + 32 < cl_sz) {
                    FAT32DirEntry* nxt = (FAT32DirEntry*)(scratch + off + 32);
                    nxt->name[0] = 0x00;
                }
                return WriteCluster(cluster, scratch);
            }
        }
        prev = cluster;
        uint32_t next = ReadFAT(cluster);
        if (next >= FAT32_EOC || next == FAT32_BAD || next < 2) break;
        cluster = next;
    }

    uint32_t new_cluster = AllocCluster();
    if (!new_cluster) return -1;
    if (WriteFAT(prev, new_cluster) != 0) {
        WriteFAT(new_cluster, FAT32_FREE);
        return -1;
    }
    for (uint32_t i = 0; i < cl_sz; i++) scratch[i] = 0;
    FAT32DirEntry* de = (FAT32DirEntry*)scratch;
    for (int i = 0; i < 11; i++) de->name[i] = short_name[i];
    de->attr = attr;
    de->first_cluster_hi = (uint16_t)(first_cluster >> 16);
    de->first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
    de->file_size = file_size;
    return WriteCluster(new_cluster, scratch);
}

int FAT32::FindPath(const char* path, uint32_t* parent_cluster, char short_name[11], uint32_t* found_cluster, uint8_t* found_attr, bool want_parent_only) {
    if (!mounted || !path || path[0] != '/') return -1;
    uint32_t current = root_cluster;
    char comp[64];
    int ci = 0;
    int i = 1;
    bool have_component = false;

    while (true) {
        char c = path[i];
        if (c == '/' || c == 0) {
            if (ci > 0) {
                comp[ci] = 0;
                MakeShortName(comp, short_name);
                have_component = true;
                if (c == 0 && want_parent_only) {
                    if (parent_cluster) *parent_cluster = current;
                    return 0;
                }
                uint32_t child_cluster = 0;
                uint8_t attr = 0;
                int r = FindEntryInDir(current, short_name, &child_cluster, &attr, nullptr, nullptr);
                if (r != 0) return r;
                if (c == 0) {
                    if (parent_cluster) *parent_cluster = current;
                    if (found_cluster) *found_cluster = child_cluster;
                    if (found_attr) *found_attr = attr;
                    return 0;
                }
                if (!(attr & FAT32_ATTR_DIRECTORY)) return -1;
                if (!IsClusterValid(child_cluster)) return -1;
                current = child_cluster;
                ci = 0;
            }
            if (c == 0) break;
        } else {
            if (ci < (int)sizeof(comp) - 1) comp[ci++] = c;
        }
        i++;
    }

    if (!have_component) {
        if (parent_cluster) *parent_cluster = root_cluster;
        if (found_cluster) *found_cluster = root_cluster;
        if (found_attr) *found_attr = FAT32_ATTR_DIRECTORY;
        for (int j = 0; j < 11; j++) short_name[j] = ' ';
        return 0;
    }
    return -1;
}

int FAT32::EnsureDirPath(const char* path, uint32_t* dir_cluster_out) {
    if (!path || path[0] != '/') return -1;
    if (path[1] == 0) {
        if (dir_cluster_out) *dir_cluster_out = root_cluster;
        return 0;
    }

    uint32_t cached = LookupDirCache(path);
    if (cached >= 2) {
        if (dir_cluster_out) *dir_cluster_out = cached;
        return 0;
    }

    uint32_t current = root_cluster;
    char comp[64];
    int ci = 0;
    char short_name[11];

    for (int i = 1;; i++) {
        char c = path[i];
        if (c == '/' || c == 0) {
            if (ci > 0) {
                comp[ci] = 0;
                MakeShortName(comp, short_name);
                uint32_t child = 0;
                uint8_t attr = 0;
                int r = FindEntryInDir(current, short_name, &child, &attr, nullptr, nullptr);
                if (r != 0) {
                    if (CreateDir(current, short_name, &child) != 0) return -1;
                    attr = FAT32_ATTR_DIRECTORY;
                }
                if (!(attr & FAT32_ATTR_DIRECTORY)) return -1;
                if (!IsClusterValid(child)) return -1;
                current = child;
                ci = 0;
            }
            if (c == 0) break;
        } else {
            if (ci < (int)sizeof(comp) - 1) comp[ci++] = c;
        }
    }

    InsertDirCache(path, current);
    if (dir_cluster_out) *dir_cluster_out = current;
    return 0;
}

int FAT32::Mount(Fat32BlockRead read_fn, Fat32BlockWrite write_fn, void* dev_ctx, uint64_t part_offset) {
    blk_read = read_fn;
    blk_write = write_fn;
    blk_ctx = dev_ctx;
    partition_offset = part_offset;
    mounted = false;
    InvalidateCache();

    FAT32BPB bpb;
    if (RawRead(0, sizeof(bpb), &bpb) != 0) return -1;
    if (!(bpb.fs_type[0] == 'F' && bpb.fs_type[1] == 'A' && bpb.fs_type[2] == 'T')) return -2;
    if (bpb.bytes_per_sector == 0 || bpb.sectors_per_cluster == 0) return -3;

    bytes_per_sector = bpb.bytes_per_sector;
    sectors_per_cluster = bpb.sectors_per_cluster;
    reserved_sectors = bpb.reserved_sector_count;
    fat_count = bpb.num_fats;
    sectors_per_fat = bpb.fat_size32;
    root_cluster = bpb.root_cluster;
    total_sectors = bpb.total_sectors32 ? bpb.total_sectors32 : bpb.total_sectors16;
    fat_offset = (uint64_t)reserved_sectors * bytes_per_sector;
    data_offset = (uint64_t)(reserved_sectors + (uint32_t)fat_count * sectors_per_fat) * bytes_per_sector;
    cluster_size = (uint32_t)sectors_per_cluster * bytes_per_sector;
    if (cluster_size == 0 || cluster_size > FAT32_MAX_CLUSTER_BYTES) return -4;
    uint32_t data_sectors = total_sectors - reserved_sectors - (uint32_t)fat_count * sectors_per_fat;
    total_clusters = data_sectors / sectors_per_cluster;
    next_free_hint = 2;
    mounted = true;

    SerialLogger::Log("[fat32] Mounted ESP\r\n");
    return 0;
}

void FAT32::Unmount() {
    if (mounted) {
        FlushCache();
        InvalidateCache();
    }
    mounted = false;
}

bool FAT32::IsMounted() { return mounted; }

int FAT32::Sync() {
    if (!mounted) return -1;
    return FlushCache();
}

int FAT32::Mkdirs(const char* path) {
    return EnsureDirPath(path, nullptr);
}

bool FAT32::Exists(const char* path) {
    uint32_t parent = 0, child = 0;
    uint8_t attr = 0;
    char short_name[11];
    return FindPath(path, &parent, short_name, &child, &attr, false) == 0;
}

int FAT32::WriteFile(const char* path, const void* data, uint32_t len) {
    if (!mounted || !path || path[0] != '/') return -1;
    uint32_t file_size = len;

    char parent_path[256];
    int last_slash = -1;
    int plen = 0;
    for (int i = 0; path[i]; i++) { if (path[i] == '/') last_slash = i; plen++; }
    if (last_slash < 0 || plen >= (int)sizeof(parent_path)) return -1;
    if (last_slash == 0) { parent_path[0] = '/'; parent_path[1] = 0; }
    else {
        for (int i = 0; i < last_slash; i++) parent_path[i] = path[i];
        parent_path[last_slash] = 0;
    }

    uint32_t dir_cluster = 0;
    if (EnsureDirPath(parent_path, &dir_cluster) != 0) return -1;

    const char* base = path + last_slash + 1;
    if (base[0] == 0) return -1;
    char short_name[11];
    MakeShortName(base, short_name);

    uint32_t cl_sz = cluster_size;
    uint32_t needed_clusters = len == 0 ? 0 : (len + cl_sz - 1) / cl_sz;
    uint32_t first_cluster = 0;
    uint32_t prev = 0;
    const uint8_t* src = (const uint8_t*)data;
    uint32_t remaining = len;

    for (uint32_t i = 0; i < needed_clusters; i++) {
        uint32_t c = AllocCluster();
        if (!c) {
            // free what we already chained
            uint32_t cur = first_cluster;
            while (cur >= 2 && cur < FAT32_EOC) {
                uint32_t nx = ReadFAT(cur);
                WriteFAT(cur, FAT32_FREE);
                if (nx >= FAT32_EOC || nx < 2) break;
                cur = nx;
            }
            FlushCache();
            return -1;
        }
        if (!first_cluster) first_cluster = c;
        if (prev) {
            if (WriteFAT(prev, c) != 0) { FlushCache(); return -1; }
        }
        prev = c;

        for (uint32_t j = 0; j < cl_sz; j++) scratch[j] = 0;
        if (src && remaining > 0) {
            uint32_t copy = remaining > cl_sz ? cl_sz : remaining;
            for (uint32_t j = 0; j < copy; j++) scratch[j] = src[j];
            src += copy;
            remaining -= copy;
        }
        if (WriteCluster(c, scratch) != 0) { FlushCache(); return -1; }
    }
    if (prev) WriteFAT(prev, FAT32_EOC);

    int r = AddEntryToDir(dir_cluster, short_name, FAT32_ATTR_ARCHIVE, first_cluster, file_size);
    FlushCache();
    return r;
}

// ---- read side (the live mount consumer) (satoru) ---------------------------

int FAT32::StatPath(const char* path, uint32_t* first_cluster, uint8_t* attr, uint32_t* size) {
    if (!mounted || !path || path[0] != '/') return -1;
    // root is a synthetic directory entry (satoru)
    if (path_len_norm(path) <= 1) {
        if (first_cluster) *first_cluster = root_cluster;
        if (attr) *attr = FAT32_ATTR_DIRECTORY;
        if (size) *size = 0;
        return 0;
    }
    uint32_t parent = 0;
    char short_name[11];
    int r = FindPath(path, &parent, short_name, nullptr, nullptr, true);
    if (r != 0) return r;
    uint32_t fc = 0; uint8_t at = 0; uint32_t sz = 0;
    r = FindEntryInDir(parent, short_name, &fc, &at, nullptr, nullptr, &sz);
    if (r != 0) return r;
    if (first_cluster) *first_cluster = fc;
    if (attr) *attr = at;
    if (size) *size = sz;
    return 0;
}

int FAT32::GetFileSize(const char* path) {
    uint8_t at = 0; uint32_t sz = 0;
    if (StatPath(path, nullptr, &at, &sz) != 0) return -1;
    if (at & FAT32_ATTR_DIRECTORY) return -1;
    return (int)sz;
}

int FAT32::ReadFile(const char* path, void* buf, uint32_t max_len) {
    if (!mounted || !buf) return -1;
    uint32_t fc = 0; uint8_t at = 0; uint32_t sz = 0;
    if (StatPath(path, &fc, &at, &sz) != 0) return -1;
    if (at & FAT32_ATTR_DIRECTORY) return -1;
    uint32_t want = sz < max_len ? sz : max_len;
    uint8_t* dst = (uint8_t*)buf;
    uint32_t done = 0;
    uint32_t cluster = fc;
    uint32_t guard = 0;
    // walk the cluster chain, copying at most one cluster per hop; the byte
    // cache underneath turns this into 4k device reads. (satoru)
    while (done < want && IsClusterValid(cluster)) {
        if (guard++ > total_clusters + 2) return -1;   // cyclic fat chain (satoru)
        uint32_t chunk = want - done;
        if (chunk > cluster_size) chunk = cluster_size;
        if (ReadBytes(ClusterOffset(cluster), chunk, dst + done) != 0) return -1;
        done += chunk;
        if (done >= want) break;
        uint32_t next = ReadFAT(cluster);
        if (next >= FAT32_EOC || next == FAT32_BAD || next < 2) break;
        cluster = next;
    }
    return (int)done;
}

int FAT32::ListDir(const char* path, char* out, int max_out) {
    if (!mounted || !out || max_out < 2) return -1;
    uint32_t fc = 0; uint8_t at = 0;
    if (StatPath(path, &fc, &at, nullptr) != 0) return -1;
    if (!(at & FAT32_ATTR_DIRECTORY)) return -1;
    uint32_t cluster = (fc >= 2) ? fc : root_cluster;   // ".." to root stores 0 (satoru)
    int p = 0;
    // uses scratch2 so nothing that ran inside StatPath (scratch) is live. (satoru)
    uint32_t guard = 0;
    while (IsClusterValid(cluster)) {
        if (guard++ > total_clusters + 2) break;
        if (ReadCluster(cluster, scratch2) != 0) { out[p] = 0; return -1; }
        for (uint32_t off = 0; off < cluster_size; off += 32) {
            FAT32DirEntry* de = (FAT32DirEntry*)(scratch2 + off);
            uint8_t marker = (uint8_t)de->name[0];
            if (is_end(marker)) { out[p] = 0; return p; }
            if (is_deleted(marker) || is_lfn(de->attr)) continue;
            if (de->attr & FAT32_ATTR_VOLUME_ID) continue;
            // 8.3 -> "NAME.EXT" one line per entry (satoru)
            for (int i = 0; i < 8 && de->name[i] != ' '; i++) {
                if (p < max_out - 1) out[p++] = de->name[i];
            }
            if (de->name[8] != ' ') {
                if (p < max_out - 1) out[p++] = '.';
                for (int i = 8; i < 11 && de->name[i] != ' '; i++) {
                    if (p < max_out - 1) out[p++] = de->name[i];
                }
            }
            if (p < max_out - 1) out[p++] = ' ';
            if (p < max_out - 1) out[p++] = ' ';
            if (de->attr & FAT32_ATTR_DIRECTORY) {
                const char* d = "<DIR>";
                for (int i = 0; d[i]; i++) if (p < max_out - 1) out[p++] = d[i];
            } else {
                char t[12]; int ti = 0; uint32_t v = de->file_size;
                if (v == 0) t[ti++] = '0';
                while (v && ti < 12) { t[ti++] = (char)('0' + (v % 10)); v /= 10; }
                while (ti) { char c2 = t[--ti]; if (p < max_out - 1) out[p++] = c2; }
            }
            if (p < max_out - 1) out[p++] = '\n';
        }
        uint32_t next = ReadFAT(cluster);
        if (next >= FAT32_EOC || next == FAT32_BAD || next < 2) break;
        cluster = next;
    }
    out[p] = 0;
    return p;
}
// ---- end read side (satoru) --------------------------------------------------
