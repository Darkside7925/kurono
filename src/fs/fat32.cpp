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

namespace {
static int f_len(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void f_cpy(char* d, const char* s, int mx) {
    int i = 0;
    if (!d || mx < 1) return;
    while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static bool f_eq11(const char a[11], const char b[11]) {
    for (int i = 0; i < 11; i++) if (a[i] != b[i]) return false;
    return true;
}

static bool is_end(uint8_t c) { return c == 0x00; }
static bool is_deleted(uint8_t c) { return c == 0xE5; }
static bool is_lfn(uint8_t attr) { return attr == 0x0F; }
}

int FAT32::ReadBytes(uint64_t off, uint32_t len, void* buf) {
    if (!blk_read) return -1;
    return blk_read(partition_offset + off, len, buf, blk_ctx);
}

int FAT32::WriteBytes(uint64_t off, uint32_t len, const void* buf) {
    if (!blk_write) return -1;
    return blk_write(partition_offset + off, len, buf, blk_ctx);
}

uint64_t FAT32::ClusterOffset(uint32_t cluster) {
    return data_offset + (uint64_t)(cluster - 2) * sectors_per_cluster * bytes_per_sector;
}

int FAT32::ReadCluster(uint32_t cluster, void* buf) {
    return ReadBytes(ClusterOffset(cluster), (uint32_t)sectors_per_cluster * bytes_per_sector, buf);
}

int FAT32::WriteCluster(uint32_t cluster, const void* buf) {
    return WriteBytes(ClusterOffset(cluster), (uint32_t)sectors_per_cluster * bytes_per_sector, buf);
}

uint32_t FAT32::ReadFAT(uint32_t cluster) {
    uint32_t value = 0;
    uint64_t off = fat_offset + (uint64_t)cluster * 4;
    if (ReadBytes(off, 4, &value) != 0) return FAT32_EOC;
    return value & 0x0FFFFFFF;
}

int FAT32::WriteFAT(uint32_t cluster, uint32_t value) {
    value &= 0x0FFFFFFF;
    for (int i = 0; i < fat_count; i++) {
        uint64_t off = fat_offset + (uint64_t)i * sectors_per_fat * bytes_per_sector + (uint64_t)cluster * 4;
        if (WriteBytes(off, 4, &value) != 0) return -1;
    }
    return 0;
}

uint32_t FAT32::AllocCluster() {
    uint32_t total_clusters = (total_sectors - reserved_sectors - fat_count * sectors_per_fat) / sectors_per_cluster;
    for (uint32_t c = 2; c < total_clusters + 2; c++) {
        if (ReadFAT(c) == FAT32_FREE) {
            if (WriteFAT(c, FAT32_EOC) != 0) return 0;
            ClearCluster(c);
            return c;
        }
    }
    return 0;
}

int FAT32::ClearCluster(uint32_t cluster) {
    uint32_t sz = (uint32_t)sectors_per_cluster * bytes_per_sector;
    uint8_t* zero = (uint8_t*)KernelHeap::Alloc(sz);
    if (!zero) return -1;
    for (uint32_t i = 0; i < sz; i++) zero[i] = 0;
    int r = WriteCluster(cluster, zero);
    KernelHeap::Free(zero);
    return r;
}

void FAT32::MakeShortName(const char* src, char out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    if (!src) return;
    int di = 0;
    int ei = 8;
    bool ext = false;
    for (int i = 0; src[i]; i++) {
        char c = src[i];
        if (c == '.') { ext = true; continue; }
        if (c >= 'a' && c <= 'z') c -= 32;
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            if (!ext && di < 8) out[di++] = c;
            else if (ext && ei < 11) out[ei++] = c;
        }
    }
}

int FAT32::FindEntryInDir(uint32_t dir_cluster, const char short_name[11], uint32_t* found_cluster, uint8_t* found_attr, uint32_t* found_entry_cluster, uint32_t* found_entry_offset) {
    uint32_t cluster = dir_cluster;
    uint32_t cl_sz = (uint32_t)sectors_per_cluster * bytes_per_sector;
    uint8_t* buf = (uint8_t*)KernelHeap::Alloc(cl_sz);
    if (!buf) return -1;

    while (cluster >= 2 && cluster < FAT32_EOC) {
        if (ReadCluster(cluster, buf) != 0) { KernelHeap::Free(buf); return -1; }
        for (uint32_t off = 0; off < cl_sz; off += 32) {
            FAT32DirEntry* de = (FAT32DirEntry*)(buf + off);
            if (is_end((uint8_t)de->name[0])) { KernelHeap::Free(buf); return 1; }
            if (is_deleted((uint8_t)de->name[0]) || is_lfn(de->attr)) continue;
            if (f_eq11(de->name, short_name)) {
                if (found_cluster) *found_cluster = ((uint32_t)de->first_cluster_hi << 16) | de->first_cluster_lo;
                if (found_attr) *found_attr = de->attr;
                if (found_entry_cluster) *found_entry_cluster = cluster;
                if (found_entry_offset) *found_entry_offset = off;
                KernelHeap::Free(buf);
                return 0;
            }
        }
        uint32_t next = ReadFAT(cluster);
        if (next >= FAT32_EOC) break;
        cluster = next;
    }
    KernelHeap::Free(buf);
    return 1;
}

int FAT32::CreateDir(uint32_t parent_cluster, const char short_name[11], uint32_t* new_cluster_out) {
    uint32_t new_cluster = AllocCluster();
    if (!new_cluster) return -1;

    uint32_t cl_sz = (uint32_t)sectors_per_cluster * bytes_per_sector;
    uint8_t* buf = (uint8_t*)KernelHeap::Alloc(cl_sz);
    if (!buf) return -1;
    for (uint32_t i = 0; i < cl_sz; i++) buf[i] = 0;

    FAT32DirEntry* dot = (FAT32DirEntry*)buf;
    for (int i = 0; i < 11; i++) dot->name[i] = ' ';
    dot->name[0] = '.';
    dot->attr = FAT32_ATTR_DIRECTORY;
    dot->first_cluster_hi = (uint16_t)(new_cluster >> 16);
    dot->first_cluster_lo = (uint16_t)(new_cluster & 0xFFFF);

    FAT32DirEntry* dotdot = (FAT32DirEntry*)(buf + 32);
    for (int i = 0; i < 11; i++) dotdot->name[i] = ' ';
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attr = FAT32_ATTR_DIRECTORY;
    dotdot->first_cluster_hi = (uint16_t)(parent_cluster >> 16);
    dotdot->first_cluster_lo = (uint16_t)(parent_cluster & 0xFFFF);

    int r = WriteCluster(new_cluster, buf);
    KernelHeap::Free(buf);
    if (r != 0) return -1;

    r = AddEntryToDir(parent_cluster, short_name, FAT32_ATTR_DIRECTORY, new_cluster, 0);
    if (r != 0) return r;
    if (new_cluster_out) *new_cluster_out = new_cluster;
    return 0;
}

int FAT32::AddEntryToDir(uint32_t dir_cluster, const char short_name[11], uint8_t attr, uint32_t first_cluster, uint32_t file_size) {
    uint32_t cluster = dir_cluster;
    uint32_t prev = 0;
    uint32_t cl_sz = (uint32_t)sectors_per_cluster * bytes_per_sector;
    uint8_t* buf = (uint8_t*)KernelHeap::Alloc(cl_sz);
    if (!buf) return -1;

    while (true) {
        if (ReadCluster(cluster, buf) != 0) { KernelHeap::Free(buf); return -1; }
        for (uint32_t off = 0; off < cl_sz; off += 32) {
            FAT32DirEntry* de = (FAT32DirEntry*)(buf + off);
            if (is_end((uint8_t)de->name[0]) || is_deleted((uint8_t)de->name[0])) {
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
                if (off + 32 < cl_sz && is_end((uint8_t)buf[off + 32])) {
                    // preserve end marker after newly used entry
                }
                int r = WriteCluster(cluster, buf);
                KernelHeap::Free(buf);
                return r;
            }
        }
        prev = cluster;
        uint32_t next = ReadFAT(cluster);
        if (next >= FAT32_EOC) break;
        cluster = next;
    }

    uint32_t new_cluster = AllocCluster();
    if (!new_cluster) { KernelHeap::Free(buf); return -1; }
    if (WriteFAT(prev, new_cluster) != 0) { KernelHeap::Free(buf); return -1; }
    for (uint32_t i = 0; i < cl_sz; i++) buf[i] = 0;
    FAT32DirEntry* de = (FAT32DirEntry*)buf;
    for (int i = 0; i < 11; i++) de->name[i] = short_name[i];
    de->attr = attr;
    de->first_cluster_hi = (uint16_t)(first_cluster >> 16);
    de->first_cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
    de->file_size = file_size;
    int r = WriteCluster(new_cluster, buf);
    KernelHeap::Free(buf);
    return r;
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
                current = child;
                ci = 0;
            }
            if (c == 0) break;
        } else {
            if (ci < (int)sizeof(comp) - 1) comp[ci++] = c;
        }
    }

    if (dir_cluster_out) *dir_cluster_out = current;
    return 0;
}

int FAT32::Mount(Fat32BlockRead read_fn, Fat32BlockWrite write_fn, void* dev_ctx, uint64_t part_offset) {
    blk_read = read_fn;
    blk_write = write_fn;
    blk_ctx = dev_ctx;
    partition_offset = part_offset;
    mounted = false;

    FAT32BPB bpb;
    if (ReadBytes(0, sizeof(bpb), &bpb) != 0) return -1;
    if (!(bpb.fs_type[0] == 'F' && bpb.fs_type[1] == 'A' && bpb.fs_type[2] == 'T')) return -2;

    bytes_per_sector = bpb.bytes_per_sector;
    sectors_per_cluster = bpb.sectors_per_cluster;
    reserved_sectors = bpb.reserved_sector_count;
    fat_count = bpb.num_fats;
    sectors_per_fat = bpb.fat_size32;
    root_cluster = bpb.root_cluster;
    total_sectors = bpb.total_sectors16 ? bpb.total_sectors16 : bpb.total_sectors32;
    fat_offset = (uint64_t)reserved_sectors * bytes_per_sector;
    data_offset = (uint64_t)(reserved_sectors + fat_count * sectors_per_fat) * bytes_per_sector;
    mounted = true;

    SerialLogger::Log("[fat32] Mounted ESP\r\n");
    return 0;
}

void FAT32::Unmount() {
    mounted = false;
}

bool FAT32::IsMounted() { return mounted; }

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
    for (int i = 0; path[i]; i++) if (path[i] == '/') last_slash = i;
    if (last_slash <= 0) return -1;
    for (int i = 0; i < last_slash && i < 255; i++) parent_path[i] = path[i];
    parent_path[last_slash] = 0;

    uint32_t dir_cluster = 0;
    if (EnsureDirPath(parent_path, &dir_cluster) != 0) return -1;

    const char* base = path + last_slash + 1;
    char short_name[11];
    MakeShortName(base, short_name);

    uint32_t cl_sz = (uint32_t)sectors_per_cluster * bytes_per_sector;
    uint32_t needed_clusters = len == 0 ? 1 : (len + cl_sz - 1) / cl_sz;
    uint32_t first_cluster = 0;
    uint32_t prev = 0;
    const uint8_t* src = (const uint8_t*)data;
    uint8_t* tmp = (uint8_t*)KernelHeap::Alloc(cl_sz);
    if (!tmp) return -1;

    for (uint32_t i = 0; i < needed_clusters; i++) {
        uint32_t c = AllocCluster();
        if (!c) { KernelHeap::Free(tmp); return -1; }
        if (!first_cluster) first_cluster = c;
        if (prev) WriteFAT(prev, c);
        prev = c;

        for (uint32_t j = 0; j < cl_sz; j++) tmp[j] = 0;
        if (src && len > 0) {
            uint32_t copy = len > cl_sz ? cl_sz : len;
            for (uint32_t j = 0; j < copy; j++) tmp[j] = src[j];
            src += copy;
            len -= copy;
        }
        if (WriteCluster(c, tmp) != 0) { KernelHeap::Free(tmp); return -1; }
    }
    KernelHeap::Free(tmp);

    return AddEntryToDir(dir_cluster, short_name, FAT32_ATTR_ARCHIVE, first_cluster, file_size);
}
