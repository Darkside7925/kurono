//  kurono os  -  ext4 filesystem driver implementation
//  full read/write ext4 support for accessing the linux partition

#include "ext4.h"
#include "../kernel/heap.h"
#include "../drivers/serial.h"
#include "../drivers/nvme.h"
#include "../kernel/time.h"

// current unix time in whole seconds, for inode timestamps (satoru)
static uint32_t e4_now_secs() {
    return (uint32_t)(TimeManager::NowUTC().us / 1000000ull);
}

bool           Ext4::mounted = false;
Ext4Superblock Ext4::sb;
Ext4BlockRead  Ext4::blk_read = nullptr;
Ext4BlockWrite Ext4::blk_write = nullptr;
void*          Ext4::blk_ctx = nullptr;
uint64_t       Ext4::partition_offset = 0;
uint32_t       Ext4::block_size = 0;
uint32_t       Ext4::groups_count = 0;
uint32_t       Ext4::inodes_per_group = 0;
uint32_t       Ext4::inode_size = 128;
uint16_t       Ext4::desc_size = 32;
Ext4File       Ext4::open_files[EXT4_MAX_OPEN_FILES];
uint8_t        Ext4::block_cache[EXT4_BLOCK_SIZE_MAX];
uint64_t       Ext4::cached_block = (uint64_t)-1;

static int e4_slen(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void e4_scpy(char* d, const char* s, int mx) {
    int i = 0;
    while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static bool e4_seq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

static bool e4_seqn(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

//  low-level i/o

int Ext4::ReadBytes(uint64_t offset, uint32_t len, void* buf) {
    if (!blk_read) return -1;
    return blk_read(partition_offset + offset, len, buf, blk_ctx);
}

int Ext4::WriteBytes(uint64_t offset, uint32_t len, const void* buf) {
    if (!blk_write) return -1;
    return blk_write(partition_offset + offset, len, buf, blk_ctx);
}

int Ext4::ReadBlock(uint64_t block_num, void* buf) {
    if (block_num == cached_block && buf == block_cache) return 0;
    uint64_t off = block_num * block_size;
    int r = ReadBytes(off, block_size, buf);
    if (r == 0 && buf == block_cache) cached_block = block_num;
    return r;
}

int Ext4::WriteBlock(uint64_t block_num, const void* buf) {
    cached_block = (uint64_t)-1;  // invalidate cache
    uint64_t off = block_num * block_size;
    return WriteBytes(off, block_size, buf);
}

//  mount / unmount

int Ext4::Mount(Ext4BlockRead read_fn, Ext4BlockWrite write_fn,
                void* dev_ctx, uint64_t part_offset) {
    if (mounted) Unmount();

    blk_read = read_fn;
    blk_write = write_fn;
    blk_ctx = dev_ctx;
    partition_offset = part_offset;
    cached_block = (uint64_t)-1;

    // read superblock at offset 1024
    if (ReadBytes(1024, sizeof(Ext4Superblock), &sb) != 0) {
        SerialLogger::Log("[ext4] Failed to read superblock\r\n");
        return -1;
    }

    if (sb.s_magic != EXT4_SUPER_MAGIC) {
        SerialLogger::Log("[ext4] Bad magic: 0x");
        SerialLogger::LogHex(sb.s_magic);
        SerialLogger::Log("\r\n");
        return -2;
    }

    block_size = EXT4_BLOCK_SIZE_MIN << sb.s_log_block_size;
    if (block_size > EXT4_BLOCK_SIZE_MAX) {
        SerialLogger::Log("[ext4] Block size too large\r\n");
        return -3;
    }

    inodes_per_group = sb.s_inodes_per_group;
    inode_size = (sb.s_rev_level >= 1) ? sb.s_inode_size : 128;
    desc_size = (sb.s_feature_incompat & 0x80) ? sb.s_desc_size : 32;
    if (desc_size < 32) desc_size = 32;

    // calculate group count
    uint64_t total_blocks = sb.s_blocks_count_lo;
    if (sb.s_feature_incompat & 0x80)  // 64-bit
        total_blocks |= ((uint64_t)sb.s_blocks_count_hi << 32);
    groups_count = (uint32_t)((total_blocks + sb.s_blocks_per_group - 1)
                              / sb.s_blocks_per_group);

    // clear open file table
    for (int i = 0; i < EXT4_MAX_OPEN_FILES; i++)
        open_files[i].open = false;

    mounted = true;

    SerialLogger::Log("[ext4] Mounted: ");
    SerialLogger::Log(sb.s_volume_name[0] ? sb.s_volume_name : "(unnamed)");
    SerialLogger::Log("  blksz=");
    SerialLogger::LogDec((int)block_size);
    SerialLogger::Log("  groups=");
    SerialLogger::LogDec((int)groups_count);
    SerialLogger::Log("  inodes/grp=");
    SerialLogger::LogDec((int)inodes_per_group);
    SerialLogger::Log("\r\n");

    return 0;
}

void Ext4::Unmount() {
    // close all open files
    for (int i = 0; i < EXT4_MAX_OPEN_FILES; i++) {
        if (open_files[i].open) Close(i);
    }
    mounted = false;
    cached_block = (uint64_t)-1;
    SerialLogger::Log("[ext4] Unmounted\r\n");
}

bool Ext4::IsMounted() { return mounted; }

//  group descriptors and inodes

int Ext4::GetGroupDesc(uint32_t group, Ext4GroupDesc* out) {
    // group descriptor table starts at the block after the superblock
    uint64_t gdt_block = (block_size == 1024) ? 2 : 1;
    uint64_t offset = gdt_block * block_size + (uint64_t)group * desc_size;
    return ReadBytes(offset, desc_size, out);
}

int Ext4::GetInodeBlock(uint32_t ino, uint64_t* block_out,
                        uint32_t* offset_out) {
    if (ino == 0) return -1;
    uint32_t group = (ino - 1) / inodes_per_group;
    uint32_t index = (ino - 1) % inodes_per_group;

    Ext4GroupDesc gd;
    memset(&gd, 0, sizeof(gd));
    if (GetGroupDesc(group, &gd) != 0) return -1;

    uint64_t inode_table = gd.bg_inode_table_lo;
    if (desc_size >= 64)
        inode_table |= ((uint64_t)gd.bg_inode_table_hi << 32);

    uint64_t byte_off = inode_table * block_size + (uint64_t)index * inode_size;
    *block_out = byte_off / block_size;
    *offset_out = (uint32_t)(byte_off % block_size);
    return 0;
}

int Ext4::ReadInode(uint32_t ino, Ext4Inode* out) {
    uint64_t blk;
    uint32_t off;
    if (GetInodeBlock(ino, &blk, &off) != 0) return -1;

    uint64_t byte_offset = blk * block_size + off;
    memset(out, 0, sizeof(Ext4Inode));
    return ReadBytes(byte_offset, (inode_size < sizeof(Ext4Inode))
                     ? inode_size : sizeof(Ext4Inode), out);
}

//  extent tree

uint64_t Ext4::ExtentLogicalToPhysical(Ext4Inode* inode,
                                        uint32_t logical_block) {
    Ext4ExtentHeader* eh = (Ext4ExtentHeader*)inode->i_block;
    if (eh->eh_magic != 0xF30A) return 0;
    // eh_depth/eh_entries come straight from the on-disk inode. real ext4
    // trees are <=5 deep and the inline i_block header holds <=eh_max (4)
    // extents. unbounded depth = kernel stack overflow; entries>eh_max =
    // out-of-bounds read past the inode. reject corrupt headers. (satoru)
    if (eh->eh_depth > 5 || eh->eh_entries > eh->eh_max) return 0;

    if (eh->eh_depth == 0) {
        // leaf node  -  scan extents
        Ext4Extent* ext = (Ext4Extent*)(eh + 1);
        for (int i = 0; i < eh->eh_entries; i++) {
            uint32_t start = ext[i].ee_block;
            uint32_t len = ext[i].ee_len;
            if (len > 32768) len = len - 32768;  // uninitialized extent
            if (logical_block >= start && logical_block < start + len) {
                uint64_t phys = ext[i].ee_start_lo |
                                ((uint64_t)ext[i].ee_start_hi << 32);
                return phys + (logical_block - start);
            }
        }
        return 0;
    }

    // internal index node  -  find the right child
    Ext4ExtentIdx* idx = (Ext4ExtentIdx*)(eh + 1);
    int found = -1;
    for (int i = 0; i < eh->eh_entries; i++) {
        if (logical_block >= idx[i].ei_block) found = i;
        else break;
    }
    if (found < 0) return 0;

    uint64_t child = idx[found].ei_leaf_lo |
                     ((uint64_t)idx[found].ei_leaf_hi << 32);
    return ExtentWalkIndex(child, logical_block, eh->eh_depth - 1);
}

uint64_t Ext4::ExtentWalkIndex(uint64_t index_block,
                                uint32_t logical_block, int depth) {
    if (depth < 0 || depth > 5) return 0;   // bound recursion: a corrupt eh_depth must not blow the kernel stack (satoru)
    uint8_t* buf = (uint8_t*)KernelHeap::Alloc(block_size);
    if (!buf) return 0;

    if (ReadBlock(index_block, buf) != 0) {
        KernelHeap::Free(buf);
        return 0;
    }

    Ext4ExtentHeader* eh = (Ext4ExtentHeader*)buf;
    if (eh->eh_magic != 0xF30A) { KernelHeap::Free(buf); return 0; }
    if (eh->eh_entries > eh->eh_max) { KernelHeap::Free(buf); return 0; }  // OOB guard: don't scan past the block (satoru)

    uint64_t result = 0;

    if (depth == 0) {
        // leaf level
        Ext4Extent* ext = (Ext4Extent*)(eh + 1);
        for (int i = 0; i < eh->eh_entries; i++) {
            uint32_t start = ext[i].ee_block;
            uint32_t len = ext[i].ee_len;
            if (len > 32768) len -= 32768;
            if (logical_block >= start && logical_block < start + len) {
                uint64_t phys = ext[i].ee_start_lo |
                                ((uint64_t)ext[i].ee_start_hi << 32);
                result = phys + (logical_block - start);
                break;
            }
        }
    } else {
        Ext4ExtentIdx* idx = (Ext4ExtentIdx*)(eh + 1);
        int found = -1;
        for (int i = 0; i < eh->eh_entries; i++) {
            if (logical_block >= idx[i].ei_block) found = i;
            else break;
        }
        if (found >= 0) {
            uint64_t child = idx[found].ei_leaf_lo |
                             ((uint64_t)idx[found].ei_leaf_hi << 32);
            result = ExtentWalkIndex(child, logical_block, depth - 1);
        }
    }

    KernelHeap::Free(buf);
    return result;
}

//  traditional block map (for non-extent inodes)

uint64_t Ext4::BlockMapLogical(Ext4Inode* inode, uint32_t logical_block) {
    uint32_t ptrs_per_block = block_size / 4;

    // direct blocks (0..11)
    if (logical_block < EXT4_NDIR_BLOCKS) {
        return inode->i_block[logical_block];
    }

    // single indirect (12..12+ppb-1)
    logical_block -= EXT4_NDIR_BLOCKS;
    if (logical_block < ptrs_per_block) {
        uint32_t ind_block = inode->i_block[EXT4_IND_BLOCK];
        if (ind_block == 0) return 0;
        uint32_t val = 0;
        ReadBytes((uint64_t)ind_block * block_size +
                  logical_block * 4, 4, &val);
        return val;
    }

    // double indirect
    logical_block -= ptrs_per_block;
    if (logical_block < ptrs_per_block * ptrs_per_block) {
        uint32_t dind = inode->i_block[EXT4_DIND_BLOCK];
        if (dind == 0) return 0;
        uint32_t idx1 = logical_block / ptrs_per_block;
        uint32_t idx2 = logical_block % ptrs_per_block;
        uint32_t ind = 0;
        ReadBytes((uint64_t)dind * block_size + idx1 * 4, 4, &ind);
        if (ind == 0) return 0;
        uint32_t val = 0;
        ReadBytes((uint64_t)ind * block_size + idx2 * 4, 4, &val);
        return val;
    }

    // triple indirect
    logical_block -= ptrs_per_block * ptrs_per_block;
    uint32_t tind = inode->i_block[EXT4_TIND_BLOCK];
    if (tind == 0) return 0;
    uint32_t ppb2 = ptrs_per_block * ptrs_per_block;
    uint32_t i1 = logical_block / ppb2;
    uint32_t rem = logical_block % ppb2;
    uint32_t i2 = rem / ptrs_per_block;
    uint32_t i3 = rem % ptrs_per_block;

    uint32_t dind = 0;
    ReadBytes((uint64_t)tind * block_size + i1 * 4, 4, &dind);
    if (dind == 0) return 0;
    uint32_t ind = 0;
    ReadBytes((uint64_t)dind * block_size + i2 * 4, 4, &ind);
    if (ind == 0) return 0;
    uint32_t val = 0;
    ReadBytes((uint64_t)ind * block_size + i3 * 4, 4, &val);
    return val;
}

//  generic inode data read

int Ext4::ReadInodeData(Ext4Inode* inode, uint64_t offset,
                         uint32_t len, void* buf) {
    uint64_t file_size = inode->i_size_lo |
                         ((uint64_t)inode->i_size_high << 32);
    if (offset >= file_size) return 0;
    if (offset + len > file_size)
        len = (uint32_t)(file_size - offset);

    bool use_extents = (inode->i_flags & EXT4_EXTENTS_FL) != 0;
    uint8_t* dst = (uint8_t*)buf;
    uint32_t total = 0;

    while (len > 0) {
        uint32_t logical = (uint32_t)(offset / block_size);
        uint32_t blk_off = (uint32_t)(offset % block_size);
        uint32_t to_read = block_size - blk_off;
        if (to_read > len) to_read = len;

        uint64_t phys = use_extents
                        ? ExtentLogicalToPhysical(inode, logical)
                        : BlockMapLogical(inode, logical);

        if (phys == 0) {
            // sparse block  -  zeros
            memset(dst, 0, to_read);
        } else {
            if (ReadBytes(phys * block_size + blk_off, to_read, dst) != 0)
                return -1;
        }

        dst += to_read;
        offset += to_read;
        len -= to_read;
        total += to_read;
    }

    return (int)total;
}

int Ext4::WriteInodeData(Ext4Inode* inode, uint32_t ino,
                          uint64_t offset, uint32_t len,
                          const void* buf) {
    // writes file data, growing the file by allocating + LINKING new blocks
    // into the inode's classic block map when a logical offset is unmapped.
    // this pass implements direct/indirect addressing only; extent inodes that
    // need a brand-new block are rejected (no silent data loss). (satoru)
    bool use_extents = (inode->i_flags & EXT4_EXTENTS_FL) != 0;
    const uint8_t* src = (const uint8_t*)buf;
    uint32_t total = 0;
    uint32_t hint_group = (ino - 1) / inodes_per_group;

    // count of 512-byte sectors newly allocated (data + indirect metadata),
    // used to bump i_blocks_lo once at the end. (satoru)
    uint32_t sectors_added = 0;
    bool dirty = false;
    const uint32_t ppb = block_size / 4;
    const uint32_t sectors_per_block = block_size / 512;

    // allocate a fresh block, zero it on disk, and account it as metadata.
    // returns the physical block number, or 0 on failure. (satoru)
    auto alloc_meta = [&](uint32_t* out_blk) -> bool {
        uint64_t nb = 0;
        if (AllocBlock(hint_group, &nb) != 0) return false;
        uint8_t* zero = (uint8_t*)KernelHeap::Alloc(block_size);
        if (!zero) return false;
        memset(zero, 0, block_size);
        WriteBytes(nb * block_size, block_size, zero);
        KernelHeap::Free(zero);
        sectors_added += sectors_per_block;
        *out_blk = (uint32_t)nb;
        return true;
    };

    // ensure the indirect pointer word at (container_block, index) is non-zero,
    // allocating + zero-initing a child block if needed and writing the pointer
    // back to disk. returns the child block number, or 0 on failure. (satoru)
    auto ensure_disk_ptr = [&](uint32_t container_block,
                               uint32_t index, uint32_t* out_child) -> bool {
        uint32_t val = 0;
        ReadBytes((uint64_t)container_block * block_size + index * 4, 4, &val);
        if (val != 0) { *out_child = val; return true; }
        uint32_t nb = 0;
        if (!alloc_meta(&nb)) return false;
        val = nb;
        WriteBytes((uint64_t)container_block * block_size + index * 4, 4, &val);
        *out_child = nb;
        return true;
    };

    // link an already-allocated data block `phys` at logical index `lb` into
    // the classic block map, creating indirect levels as required. (satoru)
    auto link_block = [&](uint32_t lb, uint64_t phys) -> bool {
        uint32_t phys32 = (uint32_t)phys;

        // direct (satoru)
        if (lb < EXT4_NDIR_BLOCKS) {
            inode->i_block[lb] = phys32;
            return true;
        }
        // single indirect (satoru)
        lb -= EXT4_NDIR_BLOCKS;
        if (lb < ppb) {
            uint32_t ind = inode->i_block[EXT4_IND_BLOCK];
            if (ind == 0) {
                if (!alloc_meta(&ind)) return false;
                inode->i_block[EXT4_IND_BLOCK] = ind;
            }
            WriteBytes((uint64_t)ind * block_size + lb * 4, 4, &phys32);
            return true;
        }
        // double indirect (satoru)
        lb -= ppb;
        if (lb < ppb * ppb) {
            uint32_t dind = inode->i_block[EXT4_DIND_BLOCK];
            if (dind == 0) {
                if (!alloc_meta(&dind)) return false;
                inode->i_block[EXT4_DIND_BLOCK] = dind;
            }
            uint32_t ind = 0;
            if (!ensure_disk_ptr(dind, lb / ppb, &ind)) return false;
            WriteBytes((uint64_t)ind * block_size + (lb % ppb) * 4, 4, &phys32);
            return true;
        }
        // triple indirect (satoru)
        lb -= ppb * ppb;
        uint32_t ppb2 = ppb * ppb;
        uint32_t tind = inode->i_block[EXT4_TIND_BLOCK];
        if (tind == 0) {
            if (!alloc_meta(&tind)) return false;
            inode->i_block[EXT4_TIND_BLOCK] = tind;
        }
        uint32_t i1 = lb / ppb2;
        uint32_t rem = lb % ppb2;
        uint32_t i2 = rem / ppb;
        uint32_t i3 = rem % ppb;
        uint32_t dind = 0, ind = 0;
        if (!ensure_disk_ptr(tind, i1, &dind)) return false;
        if (!ensure_disk_ptr(dind, i2, &ind)) return false;
        WriteBytes((uint64_t)ind * block_size + i3 * 4, 4, &phys32);
        return true;
    };

    while (len > 0) {
        uint32_t logical = (uint32_t)(offset / block_size);
        uint32_t blk_off = (uint32_t)(offset % block_size);
        uint32_t to_write = block_size - blk_off;
        if (to_write > len) to_write = len;

        uint64_t phys = use_extents
                        ? ExtentLogicalToPhysical(inode, logical)
                        : BlockMapLogical(inode, logical);

        if (phys == 0) {
            // unmapped logical block  -  must allocate and LINK it (satoru)
            if (use_extents) {
                // this pass does not insert into the extent tree; refuse rather
                // than allocate an orphan block that the next read can't find
                // and would return as zeros (silent data loss). (satoru)
                SerialLogger::Log("[ext4] WriteInodeData: cannot grow extent "
                                  "inode (no extent insertion this pass)\r\n");
                break;
            }
            if (AllocBlock(hint_group, &phys) != 0) break;
            sectors_added += sectors_per_block;
            if (!link_block(logical, phys)) {
                // couldn't wire the block into the map; release it so it
                // isn't leaked, then stop. (satoru)
                FreeBlock(phys);
                sectors_added -= sectors_per_block;
                break;
            }
            dirty = true;
            // a freshly allocated data block written only partially must start
            // from zeros, not stale disk contents (satoru)
            if (blk_off != 0 || to_write != block_size) {
                uint8_t* z = (uint8_t*)KernelHeap::Alloc(block_size);
                if (z) {
                    memset(z, 0, block_size);
                    WriteBytes(phys * block_size, block_size, z);
                    KernelHeap::Free(z);
                }
            }
        }

        if (blk_off != 0 || to_write != block_size) {
            // partial block  -  read-modify-write
            uint8_t* tmp = (uint8_t*)KernelHeap::Alloc(block_size);
            if (!tmp) break;
            ReadBytes(phys * block_size, block_size, tmp);
            memcpy(tmp + blk_off, src, to_write);
            WriteBytes(phys * block_size, block_size, tmp);
            KernelHeap::Free(tmp);
        } else {
            WriteBytes(phys * block_size, to_write, src);
        }

        src += to_write;
        offset += to_write;
        len -= to_write;
        total += to_write;
    }

    // update size if needed (satoru)
    uint64_t new_size = offset;
    uint64_t old_size = inode->i_size_lo |
                        ((uint64_t)inode->i_size_high << 32);
    if (new_size > old_size) {
        inode->i_size_lo = (uint32_t)(new_size & 0xFFFFFFFF);
        inode->i_size_high = (uint32_t)(new_size >> 32);
        dirty = true;
    }

    // account newly allocated sectors and stamp mtime (satoru)
    if (sectors_added > 0) {
        inode->i_blocks_lo += sectors_added;
        dirty = true;
    }
    if (total > 0) {
        inode->i_mtime = e4_now_secs();
        dirty = true;
    }

    // write the inode back if anything changed (satoru)
    if (dirty) {
        uint64_t blk;
        uint32_t off;
        if (GetInodeBlock(ino, &blk, &off) == 0) {
            WriteBytes(blk * block_size + off,
                       (inode_size < sizeof(Ext4Inode))
                       ? inode_size : sizeof(Ext4Inode), inode);
        }
    }

    // if we couldn't write everything the caller asked for, surface an error
    // unless we made partial progress (which we report as the byte count).
    // (satoru)
    if (total == 0 && len > 0) return -1;
    return (int)total;
}

//  directory operations

int Ext4::DirLookup(Ext4Inode* dir_inode, const char* name,
                     uint32_t* ino_out) {
    uint64_t dir_size = dir_inode->i_size_lo |
                        ((uint64_t)dir_inode->i_size_high << 32);
    int name_len = e4_slen(name);

    uint8_t* buf = (uint8_t*)KernelHeap::Alloc(block_size);
    if (!buf) return -1;

    uint64_t pos = 0;
    while (pos < dir_size) {
        uint32_t logical = (uint32_t)(pos / block_size);
        bool use_extents = (dir_inode->i_flags & EXT4_EXTENTS_FL) != 0;
        uint64_t phys = use_extents
                        ? ExtentLogicalToPhysical(dir_inode, logical)
                        : BlockMapLogical(dir_inode, logical);
        if (phys == 0) { pos += block_size; continue; }
        if (ReadBlock(phys, buf) != 0) { pos += block_size; continue; }

        uint32_t off = 0;
        while (off < block_size) {
            Ext4DirEntry2* de = (Ext4DirEntry2*)(buf + off);
            if (de->rec_len == 0) break;
            if (de->inode != 0 && de->name_len == name_len &&
                e4_seqn(de->name, name, name_len)) {
                *ino_out = de->inode;
                KernelHeap::Free(buf);
                return 0;
            }
            off += de->rec_len;
        }
        pos += block_size;
    }

    KernelHeap::Free(buf);
    return -1;  // not found
}

int Ext4::DirAddEntry(uint32_t dir_ino, uint32_t new_ino,
                       const char* name, uint8_t file_type) {
    Ext4Inode dir;
    if (ReadInode(dir_ino, &dir) != 0) return -1;

    uint64_t dir_size = dir.i_size_lo |
                        ((uint64_t)dir.i_size_high << 32);
    int name_len = e4_slen(name);
    uint16_t needed = (uint16_t)(8 + name_len + 3) & ~3;  // align to 4

    uint8_t* buf = (uint8_t*)KernelHeap::Alloc(block_size);
    if (!buf) return -1;

    uint64_t pos = 0;
    while (pos < dir_size) {
        uint32_t logical = (uint32_t)(pos / block_size);
        bool use_extents = (dir.i_flags & EXT4_EXTENTS_FL) != 0;
        uint64_t phys = use_extents
                        ? ExtentLogicalToPhysical(&dir, logical)
                        : BlockMapLogical(&dir, logical);
        if (phys == 0) { pos += block_size; continue; }
        if (ReadBlock(phys, buf) != 0) { pos += block_size; continue; }

        uint32_t off = 0;
        while (off < block_size) {
            Ext4DirEntry2* de = (Ext4DirEntry2*)(buf + off);
            if (de->rec_len == 0) break;

            uint16_t actual = (uint16_t)(8 + de->name_len + 3) & ~3;
            uint16_t slack = de->rec_len - actual;

            if (de->inode == 0 && de->rec_len >= needed) {
                // reuse deleted entry
                de->inode = new_ino;
                de->name_len = (uint8_t)name_len;
                de->file_type = file_type;
                memcpy(de->name, name, name_len);
                WriteBlock(phys, buf);
                KernelHeap::Free(buf);
                return 0;
            }

            if (slack >= needed) {
                // split this entry
                de->rec_len = actual;
                Ext4DirEntry2* ne = (Ext4DirEntry2*)(buf + off + actual);
                ne->inode = new_ino;
                ne->rec_len = slack;
                ne->name_len = (uint8_t)name_len;
                ne->file_type = file_type;
                memcpy(ne->name, name, name_len);
                WriteBlock(phys, buf);
                KernelHeap::Free(buf);
                return 0;
            }

            off += de->rec_len;
        }
        pos += block_size;
    }

    // no existing block has room  -  extend the directory with a new block whose
    // single entry spans the whole block, then write our dirent there. (satoru)
    bool use_extents = (dir.i_flags & EXT4_EXTENTS_FL) != 0;

    // build the new directory block: one entry covering the entire block.
    // reuse `buf` (block-sized) as the staging buffer. (satoru)
    memset(buf, 0, block_size);
    Ext4DirEntry2* nd = (Ext4DirEntry2*)buf;
    nd->inode = new_ino;
    nd->rec_len = (uint16_t)block_size;
    nd->name_len = (uint8_t)name_len;
    nd->file_type = file_type;
    memcpy(nd->name, name, name_len);

    if (!use_extents) {
        // classic block-map directory: append the block via WriteInodeData,
        // which allocates + links it into the block map, grows i_size, updates
        // i_blocks/i_mtime, and persists the inode. (satoru)
        int w = WriteInodeData(&dir, dir_ino, dir_size,
                               block_size, buf);
        KernelHeap::Free(buf);
        if (w != (int)block_size) {
            SerialLogger::Log("[ext4] DirAddEntry: block-map extend failed\r\n");
            return -1;
        }
        return 0;
    }

    // extent directory: append a new single-block extent to the inline header
    // (depth 0, free slot). this is the only extent growth we can do safely in
    // this pass; anything deeper is refused rather than faked. (satoru)
    Ext4ExtentHeader* eh = (Ext4ExtentHeader*)dir.i_block;
    if (eh->eh_magic != 0xF30A || eh->eh_depth != 0 ||
        eh->eh_entries >= eh->eh_max) {
        KernelHeap::Free(buf);
        SerialLogger::Log("[ext4] DirAddEntry: extent dir extend unsupported "
                          "(depth>0 or header full)\r\n");
        return -1;
    }

    uint64_t new_blk = 0;
    uint32_t grp = (dir_ino - 1) / inodes_per_group;
    if (AllocBlock(grp, &new_blk) != 0) {
        KernelHeap::Free(buf);
        return -1;
    }

    // write the prepared dirent block to the freshly allocated block (satoru)
    if (WriteBlock(new_blk, buf) != 0) {
        FreeBlock(new_blk);
        KernelHeap::Free(buf);
        return -1;
    }
    KernelHeap::Free(buf);

    // append the extent entry. new logical block index = current dir size in
    // blocks. (satoru)
    Ext4Extent* exts = (Ext4Extent*)(eh + 1);
    Ext4Extent* ne = &exts[eh->eh_entries];
    ne->ee_block = (uint32_t)(dir_size / block_size);
    ne->ee_len = 1;
    ne->ee_start_lo = (uint32_t)(new_blk & 0xFFFFFFFF);
    ne->ee_start_hi = (uint16_t)(new_blk >> 32);
    eh->eh_entries++;

    // grow size, account the block, stamp mtime, persist inode (satoru)
    uint64_t nsz = dir_size + block_size;
    dir.i_size_lo = (uint32_t)(nsz & 0xFFFFFFFF);
    dir.i_size_high = (uint32_t)(nsz >> 32);
    dir.i_blocks_lo += block_size / 512;
    dir.i_mtime = e4_now_secs();

    uint64_t iblk;
    uint32_t ioff;
    if (GetInodeBlock(dir_ino, &iblk, &ioff) == 0) {
        WriteBytes(iblk * block_size + ioff,
                   (inode_size < sizeof(Ext4Inode))
                   ? inode_size : sizeof(Ext4Inode), &dir);
    }
    return 0;
}

int Ext4::DirRemoveEntry(uint32_t dir_ino, const char* name) {
    Ext4Inode dir;
    if (ReadInode(dir_ino, &dir) != 0) return -1;

    uint64_t dir_size = dir.i_size_lo |
                        ((uint64_t)dir.i_size_high << 32);
    int name_len = e4_slen(name);

    uint8_t* buf = (uint8_t*)KernelHeap::Alloc(block_size);
    if (!buf) return -1;

    uint64_t pos = 0;
    while (pos < dir_size) {
        uint32_t logical = (uint32_t)(pos / block_size);
        bool use_extents = (dir.i_flags & EXT4_EXTENTS_FL) != 0;
        uint64_t phys = use_extents
                        ? ExtentLogicalToPhysical(&dir, logical)
                        : BlockMapLogical(&dir, logical);
        if (phys == 0) { pos += block_size; continue; }
        if (ReadBlock(phys, buf) != 0) { pos += block_size; continue; }

        Ext4DirEntry2* prev = nullptr;
        uint32_t off = 0;
        while (off < block_size) {
            Ext4DirEntry2* de = (Ext4DirEntry2*)(buf + off);
            if (de->rec_len == 0) break;

            if (de->inode != 0 && de->name_len == name_len &&
                e4_seqn(de->name, name, name_len)) {
                if (prev) {
                    prev->rec_len += de->rec_len;
                } else {
                    de->inode = 0;
                }
                WriteBlock(phys, buf);
                KernelHeap::Free(buf);
                return 0;
            }
            prev = de;
            off += de->rec_len;
        }
        pos += block_size;
    }

    KernelHeap::Free(buf);
    return -1;
}

//  path walking

int Ext4::PathWalk(const char* path, uint32_t* ino_out) {
    if (!path || path[0] != '/') return -1;

    uint32_t cur_ino = EXT4_ROOT_INO;
    Ext4Inode cur;
    if (ReadInode(cur_ino, &cur) != 0) return -1;

    const char* p = path + 1;  // skip leading '/'
    if (*p == 0) {
        *ino_out = cur_ino;
        return 0;
    }

    while (*p) {
        // skip slashes
        while (*p == '/') p++;
        if (*p == 0) break;

        // extract component
        char component[EXT4_NAME_LEN + 1];
        int ci = 0;
        while (*p && *p != '/' && ci < EXT4_NAME_LEN) {
            component[ci++] = *p++;
        }
        component[ci] = 0;

        // current must be a directory
        if ((cur.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return -1;

        // handle symlinks
        if ((cur.i_mode & EXT4_S_IFMT) == EXT4_S_IFLNK) {
            // simplified: skip symlink resolution in path walk
        }

        uint32_t child_ino;
        if (DirLookup(&cur, component, &child_ino) != 0) return -1;

        cur_ino = child_ino;
        if (ReadInode(cur_ino, &cur) != 0) return -1;
    }

    *ino_out = cur_ino;
    return 0;
}

void Ext4::SplitPath(const char* path, char* parent, char* basename) {
    int len = e4_slen(path);
    // find last '/'
    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') { last_slash = i; break; }
    }

    if (last_slash <= 0) {
        parent[0] = '/';
        parent[1] = 0;
        e4_scpy(basename, (last_slash == 0) ? path + 1 : path, EXT4_NAME_LEN);
    } else {
        // copy parent
        for (int i = 0; i < last_slash && i < EXT4_MAX_PATH - 1; i++)
            parent[i] = path[i];
        parent[last_slash] = 0;
        e4_scpy(basename, path + last_slash + 1, EXT4_NAME_LEN);
    }
}

int Ext4::LookupInode(const char* path, uint32_t* ino_out) {
    if (!mounted) return -1;
    return PathWalk(path, ino_out);
}

int Ext4::ReadInodeByPath(const char* path, Ext4Inode* out) {
    uint32_t ino;
    if (LookupInode(path, &ino) != 0) return -1;
    return ReadInode(ino, out);
}

//  block / inode allocation (simplified bitmap-based)

int Ext4::AllocBlock(uint32_t hint_group, uint64_t* block_out) {
    uint8_t* bmp = (uint8_t*)KernelHeap::Alloc(block_size);
    if (!bmp) return -1;

    for (uint32_t g = 0; g < groups_count; g++) {
        uint32_t grp = (hint_group + g) % groups_count;
        Ext4GroupDesc gd;
        memset(&gd, 0, sizeof(gd));
        if (GetGroupDesc(grp, &gd) != 0) continue;
        if (gd.bg_free_blocks_count_lo == 0) continue;

        uint64_t bmp_blk = gd.bg_block_bitmap_lo;
        if (desc_size >= 64) bmp_blk |= ((uint64_t)gd.bg_block_bitmap_hi << 32);
        if (ReadBlock(bmp_blk, bmp) != 0) continue;

        for (uint32_t i = 0; i < sb.s_blocks_per_group; i++) {
            uint32_t byte_idx = i / 8;
            uint8_t bit = 1 << (i % 8);
            if (!(bmp[byte_idx] & bit)) {
                bmp[byte_idx] |= bit;
                WriteBlock(bmp_blk, bmp);
                // update group descriptor
                gd.bg_free_blocks_count_lo--;
                // write gd back  -  simplified
                uint64_t gdt_block = (block_size == 1024) ? 2 : 1;
                uint64_t gd_off = gdt_block * block_size +
                                  (uint64_t)grp * desc_size;
                WriteBytes(gd_off, desc_size, &gd);
                // update superblock free count
                sb.s_free_blocks_count_lo--;
                WriteBytes(1024, sizeof(Ext4Superblock), &sb);

                *block_out = (uint64_t)grp * sb.s_blocks_per_group +
                             sb.s_first_data_block + i;
                KernelHeap::Free(bmp);
                return 0;
            }
        }
    }

    KernelHeap::Free(bmp);
    return -1;  // no free blocks
}

int Ext4::AllocInode(uint32_t hint_group, uint32_t* ino_out) {
    uint8_t* bmp = (uint8_t*)KernelHeap::Alloc(block_size);
    if (!bmp) return -1;

    for (uint32_t g = 0; g < groups_count; g++) {
        uint32_t grp = (hint_group + g) % groups_count;
        Ext4GroupDesc gd;
        memset(&gd, 0, sizeof(gd));
        if (GetGroupDesc(grp, &gd) != 0) continue;
        if (gd.bg_free_inodes_count_lo == 0) continue;

        uint64_t bmp_blk = gd.bg_inode_bitmap_lo;
        if (desc_size >= 64) bmp_blk |= ((uint64_t)gd.bg_inode_bitmap_hi << 32);
        if (ReadBlock(bmp_blk, bmp) != 0) continue;

        for (uint32_t i = 0; i < inodes_per_group; i++) {
            uint32_t byte_idx = i / 8;
            uint8_t bit = 1 << (i % 8);
            if (!(bmp[byte_idx] & bit)) {
                bmp[byte_idx] |= bit;
                WriteBlock(bmp_blk, bmp);
                gd.bg_free_inodes_count_lo--;
                uint64_t gdt_block = (block_size == 1024) ? 2 : 1;
                uint64_t gd_off = gdt_block * block_size +
                                  (uint64_t)grp * desc_size;
                WriteBytes(gd_off, desc_size, &gd);
                sb.s_free_inodes_count--;
                WriteBytes(1024, sizeof(Ext4Superblock), &sb);

                *ino_out = grp * inodes_per_group + i + 1;
                KernelHeap::Free(bmp);
                return 0;
            }
        }
    }

    KernelHeap::Free(bmp);
    return -1;
}

void Ext4::FreeBlock(uint64_t block) {
    uint32_t grp = (uint32_t)((block - sb.s_first_data_block) /
                               sb.s_blocks_per_group);
    uint32_t idx = (uint32_t)((block - sb.s_first_data_block) %
                               sb.s_blocks_per_group);

    Ext4GroupDesc gd;
    memset(&gd, 0, sizeof(gd));
    if (GetGroupDesc(grp, &gd) != 0) return;

    uint64_t bmp_blk = gd.bg_block_bitmap_lo;
    if (desc_size >= 64) bmp_blk |= ((uint64_t)gd.bg_block_bitmap_hi << 32);

    uint8_t* bmp = (uint8_t*)KernelHeap::Alloc(block_size);
    if (!bmp) return;

    if (ReadBlock(bmp_blk, bmp) == 0) {
        bmp[idx / 8] &= ~(1 << (idx % 8));
        WriteBlock(bmp_blk, bmp);
        gd.bg_free_blocks_count_lo++;
        uint64_t gdt_block = (block_size == 1024) ? 2 : 1;
        WriteBytes(gdt_block * block_size + (uint64_t)grp * desc_size,
                   desc_size, &gd);
        sb.s_free_blocks_count_lo++;
        WriteBytes(1024, sizeof(Ext4Superblock), &sb);
    }

    KernelHeap::Free(bmp);
}

void Ext4::FreeInode(uint32_t ino) {
    uint32_t grp = (ino - 1) / inodes_per_group;
    uint32_t idx = (ino - 1) % inodes_per_group;

    Ext4GroupDesc gd;
    memset(&gd, 0, sizeof(gd));
    if (GetGroupDesc(grp, &gd) != 0) return;

    uint64_t bmp_blk = gd.bg_inode_bitmap_lo;
    if (desc_size >= 64) bmp_blk |= ((uint64_t)gd.bg_inode_bitmap_hi << 32);

    uint8_t* bmp = (uint8_t*)KernelHeap::Alloc(block_size);
    if (!bmp) return;

    if (ReadBlock(bmp_blk, bmp) == 0) {
        bmp[idx / 8] &= ~(1 << (idx % 8));
        WriteBlock(bmp_blk, bmp);
        gd.bg_free_inodes_count_lo++;
        uint64_t gdt_block = (block_size == 1024) ? 2 : 1;
        WriteBytes(gdt_block * block_size + (uint64_t)grp * desc_size,
                   desc_size, &gd);
        sb.s_free_inodes_count++;
        WriteBytes(1024, sizeof(Ext4Superblock), &sb);
    }

    KernelHeap::Free(bmp);
}

//  file-level operations

int Ext4::Open(const char* path, uint8_t flags) {
    if (!mounted) return -1;

    uint32_t ino;
    if (PathWalk(path, &ino) != 0) return -1;

    // find free fd
    int fd = -1;
    for (int i = 0; i < EXT4_MAX_OPEN_FILES; i++) {
        if (!open_files[i].open) { fd = i; break; }
    }
    if (fd < 0) return -1;

    open_files[fd].inode_num = ino;
    if (ReadInode(ino, &open_files[fd].inode) != 0) return -1;
    open_files[fd].offset = 0;
    open_files[fd].open = true;
    open_files[fd].flags = flags;

    return fd;
}

int Ext4::Read(int fd, void* buf, uint32_t len) {
    if (fd < 0 || fd >= EXT4_MAX_OPEN_FILES || !open_files[fd].open) return -1;

    int r = ReadInodeData(&open_files[fd].inode, open_files[fd].offset,
                          len, buf);
    if (r > 0) open_files[fd].offset += r;
    return r;
}

int Ext4::Write(int fd, const void* buf, uint32_t len) {
    if (fd < 0 || fd >= EXT4_MAX_OPEN_FILES || !open_files[fd].open) return -1;
    if (!(open_files[fd].flags & 2)) return -1;  // not writable

    int r = WriteInodeData(&open_files[fd].inode, open_files[fd].inode_num,
                           open_files[fd].offset, len, buf);
    if (r > 0) open_files[fd].offset += r;
    return r;
}

int Ext4::Seek(int fd, int64_t offset, int whence) {
    if (fd < 0 || fd >= EXT4_MAX_OPEN_FILES || !open_files[fd].open) return -1;

    Ext4Inode* in = &open_files[fd].inode;
    uint64_t sz = in->i_size_lo | ((uint64_t)in->i_size_high << 32);

    int64_t new_pos;
    if (whence == 0) new_pos = offset;
    else if (whence == 1) new_pos = (int64_t)open_files[fd].offset + offset;
    else if (whence == 2) new_pos = (int64_t)sz + offset;
    else return -1;

    if (new_pos < 0) new_pos = 0;
    open_files[fd].offset = (uint64_t)new_pos;
    return 0;
}

int Ext4::Close(int fd) {
    if (fd < 0 || fd >= EXT4_MAX_OPEN_FILES || !open_files[fd].open) return -1;
    open_files[fd].open = false;
    return 0;
}

int64_t Ext4::FileSize(const char* path) {
    Ext4Inode in;
    if (ReadInodeByPath(path, &in) != 0) return -1;
    return (int64_t)(in.i_size_lo | ((uint64_t)in.i_size_high << 32));
}

//  directory listing

int Ext4::ListDir(const char* path, Ext4DirInfo* entries, int max) {
    if (!mounted) return -1;

    uint32_t dir_ino;
    if (PathWalk(path, &dir_ino) != 0) return -1;

    Ext4Inode dir;
    if (ReadInode(dir_ino, &dir) != 0) return -1;
    if ((dir.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return -1;

    uint64_t dir_size = dir.i_size_lo |
                        ((uint64_t)dir.i_size_high << 32);
    int count = 0;

    uint8_t* buf = (uint8_t*)KernelHeap::Alloc(block_size);
    if (!buf) return -1;

    uint64_t pos = 0;
    while (pos < dir_size && count < max) {
        uint32_t logical = (uint32_t)(pos / block_size);
        bool use_extents = (dir.i_flags & EXT4_EXTENTS_FL) != 0;
        uint64_t phys = use_extents
                        ? ExtentLogicalToPhysical(&dir, logical)
                        : BlockMapLogical(&dir, logical);
        if (phys == 0) { pos += block_size; continue; }
        if (ReadBlock(phys, buf) != 0) { pos += block_size; continue; }

        uint32_t off = 0;
        while (off < block_size && count < max) {
            Ext4DirEntry2* de = (Ext4DirEntry2*)(buf + off);
            if (de->rec_len == 0) break;

            if (de->inode != 0) {
                // copy entry info
                int nl = de->name_len;
                if (nl > EXT4_NAME_LEN) nl = EXT4_NAME_LEN;
                memcpy(entries[count].name, de->name, nl);
                entries[count].name[nl] = 0;
                entries[count].inode = de->inode;
                entries[count].file_type = de->file_type;

                // read child inode for size/mode
                Ext4Inode child;
                if (ReadInode(de->inode, &child) == 0) {
                    entries[count].size = child.i_size_lo;
                    entries[count].mode = child.i_mode;
                    entries[count].uid = child.i_uid;
                    entries[count].gid = child.i_gid;
                } else {
                    entries[count].size = 0;
                    entries[count].mode = 0;
                    entries[count].uid = 0;
                    entries[count].gid = 0;
                }
                count++;
            }
            off += de->rec_len;
        }
        pos += block_size;
    }

    KernelHeap::Free(buf);
    return count;
}

//  high-level metadata ops

int Ext4::Stat(const char* path, Ext4Inode* out) {
    return ReadInodeByPath(path, out);
}

bool Ext4::Exists(const char* path) {
    uint32_t ino;
    return PathWalk(path, &ino) == 0;
}

bool Ext4::IsDir(const char* path) {
    Ext4Inode in;
    if (ReadInodeByPath(path, &in) != 0) return false;
    return (in.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR;
}

bool Ext4::IsFile(const char* path) {
    Ext4Inode in;
    if (ReadInodeByPath(path, &in) != 0) return false;
    return (in.i_mode & EXT4_S_IFMT) == EXT4_S_IFREG;
}

int Ext4::ReadLink(const char* path, char* buf, int max) {
    Ext4Inode in;
    if (ReadInodeByPath(path, &in) != 0) return -1;
    if ((in.i_mode & EXT4_S_IFMT) != EXT4_S_IFLNK) return -1;

    uint64_t sz = in.i_size_lo;
    if (sz > (uint64_t)(max - 1)) sz = max - 1;

    // fast symlink: target stored inline in i_block
    if (sz < 60 && !(in.i_flags & EXT4_EXTENTS_FL)) {
        memcpy(buf, in.i_block, (uint32_t)sz);
        buf[sz] = 0;
        return (int)sz;
    }

    // slow symlink: data in blocks
    int r = ReadInodeData(&in, 0, (uint32_t)sz, buf);
    if (r > 0) buf[r] = 0;
    return r;
}

//  write operations (create, unlink, mkdir)

int Ext4::CreateFile(const char* path, uint16_t mode) {
    if (!mounted) return -1;

    char parent_path[EXT4_MAX_PATH];
    char basename[EXT4_NAME_LEN + 1];
    SplitPath(path, parent_path, basename);
    if (basename[0] == 0) return -1;

    uint32_t parent_ino;
    if (PathWalk(parent_path, &parent_ino) != 0) return -1;

    // allocate new inode
    uint32_t grp = (parent_ino - 1) / inodes_per_group;
    uint32_t new_ino;
    if (AllocInode(grp, &new_ino) != 0) return -1;

    // initialize inode
    Ext4Inode new_in;
    memset(&new_in, 0, sizeof(new_in));
    new_in.i_mode = EXT4_S_IFREG | mode;
    new_in.i_links_count = 1;
    new_in.i_flags = EXT4_EXTENTS_FL;

    // write inode
    uint64_t blk;
    uint32_t off;
    if (GetInodeBlock(new_ino, &blk, &off) != 0) {
        FreeInode(new_ino);
        return -1;
    }
    WriteBytes(blk * block_size + off,
               (inode_size < sizeof(Ext4Inode))
               ? inode_size : sizeof(Ext4Inode), &new_in);

    // add directory entry
    if (DirAddEntry(parent_ino, new_ino, basename, EXT4_FT_REG_FILE) != 0) {
        FreeInode(new_ino);
        return -1;
    }

    return 0;
}

int Ext4::WriteFile(const char* path, const void* data, uint32_t len) {
    if (!mounted) return -1;

    // check if file exists, create if not
    uint32_t ino;
    if (PathWalk(path, &ino) != 0) {
        if (CreateFile(path, 0644) != 0) return -1;
        if (PathWalk(path, &ino) != 0) return -1;
    }

    Ext4Inode in;
    if (ReadInode(ino, &in) != 0) return -1;

    return WriteInodeData(&in, ino, 0, len, data);
}

int Ext4::Mkdir(const char* path, uint16_t mode) {
    if (!mounted) return -1;

    char parent_path[EXT4_MAX_PATH];
    char basename[EXT4_NAME_LEN + 1];
    SplitPath(path, parent_path, basename);
    if (basename[0] == 0) return -1;

    uint32_t parent_ino;
    if (PathWalk(parent_path, &parent_ino) != 0) return -1;

    // check doesn't already exist
    uint32_t existing;
    if (DirLookup(nullptr, basename, &existing) == 0) return -1;

    uint32_t grp = (parent_ino - 1) / inodes_per_group;
    uint32_t new_ino;
    if (AllocInode(grp, &new_ino) != 0) return -1;

    // initialize directory inode
    Ext4Inode new_in;
    memset(&new_in, 0, sizeof(new_in));
    new_in.i_mode = EXT4_S_IFDIR | mode;
    new_in.i_links_count = 2;  // . and parent's link
    new_in.i_size_lo = block_size;

    // allocate a block for directory entries
    uint64_t dir_block;
    if (AllocBlock(grp, &dir_block) != 0) {
        FreeInode(new_ino);
        return -1;
    }

    // set up extent for the directory data block
    new_in.i_flags = EXT4_EXTENTS_FL;
    Ext4ExtentHeader* eh = (Ext4ExtentHeader*)new_in.i_block;
    eh->eh_magic = 0xF30A;
    eh->eh_entries = 1;
    eh->eh_max = 4;
    eh->eh_depth = 0;
    eh->eh_generation = 0;
    Ext4Extent* ext = (Ext4Extent*)(eh + 1);
    ext->ee_block = 0;
    ext->ee_len = 1;
    ext->ee_start_lo = (uint32_t)(dir_block & 0xFFFFFFFF);
    ext->ee_start_hi = (uint16_t)(dir_block >> 32);

    new_in.i_blocks_lo = block_size / 512;

    // write inode
    uint64_t iblk;
    uint32_t ioff;
    if (GetInodeBlock(new_ino, &iblk, &ioff) != 0) {
        FreeBlock(dir_block);
        FreeInode(new_ino);
        return -1;
    }
    WriteBytes(iblk * block_size + ioff,
               (inode_size < sizeof(Ext4Inode))
               ? inode_size : sizeof(Ext4Inode), &new_in);

    // create . and .. entries in the new dir block
    uint8_t* dbuf = (uint8_t*)KernelHeap::Alloc(block_size);
    if (dbuf) {
        memset(dbuf, 0, block_size);
        Ext4DirEntry2* dot = (Ext4DirEntry2*)dbuf;
        dot->inode = new_ino;
        dot->rec_len = 12;
        dot->name_len = 1;
        dot->file_type = EXT4_FT_DIR;
        dot->name[0] = '.';

        Ext4DirEntry2* dotdot = (Ext4DirEntry2*)(dbuf + 12);
        dotdot->inode = parent_ino;
        dotdot->rec_len = (uint16_t)(block_size - 12);
        dotdot->name_len = 2;
        dotdot->file_type = EXT4_FT_DIR;
        dotdot->name[0] = '.';
        dotdot->name[1] = '.';

        WriteBlock(dir_block, dbuf);
        KernelHeap::Free(dbuf);
    }

    // add to parent directory
    if (DirAddEntry(parent_ino, new_ino, basename, EXT4_FT_DIR) != 0) {
        FreeBlock(dir_block);
        FreeInode(new_ino);
        return -1;
    }

    // increment parent link count
    Ext4Inode parent_in;
    if (ReadInode(parent_ino, &parent_in) == 0) {
        parent_in.i_links_count++;
        uint64_t pblk;
        uint32_t poff;
        if (GetInodeBlock(parent_ino, &pblk, &poff) == 0) {
            WriteBytes(pblk * block_size + poff,
                       (inode_size < sizeof(Ext4Inode))
                       ? inode_size : sizeof(Ext4Inode), &parent_in);
        }
    }

    return 0;
}

int Ext4::Unlink(const char* path) {
    if (!mounted) return -1;

    char parent_path[EXT4_MAX_PATH];
    char basename[EXT4_NAME_LEN + 1];
    SplitPath(path, parent_path, basename);

    uint32_t parent_ino;
    if (PathWalk(parent_path, &parent_ino) != 0) return -1;

    uint32_t file_ino;
    Ext4Inode parent_in;
    if (ReadInode(parent_ino, &parent_in) != 0) return -1;
    if (DirLookup(&parent_in, basename, &file_ino) != 0) return -1;

    Ext4Inode file_in;
    if (ReadInode(file_ino, &file_in) != 0) return -1;
    if ((file_in.i_mode & EXT4_S_IFMT) == EXT4_S_IFDIR) return -1;

    DirRemoveEntry(parent_ino, basename);

    file_in.i_links_count--;
    if (file_in.i_links_count == 0) {
        // free inode and blocks
        FreeInode(file_ino);
        // todo: free data blocks  -  complex for extent/blockmap
    } else {
        uint64_t blk;
        uint32_t off;
        if (GetInodeBlock(file_ino, &blk, &off) == 0) {
            WriteBytes(blk * block_size + off,
                       (inode_size < sizeof(Ext4Inode))
                       ? inode_size : sizeof(Ext4Inode), &file_in);
        }
    }

    return 0;
}

int Ext4::Rmdir(const char* path) {
    if (!mounted) return -1;

    uint32_t dir_ino;
    if (PathWalk(path, &dir_ino) != 0) return -1;
    if (dir_ino == EXT4_ROOT_INO) return -1;

    Ext4Inode dir;
    if (ReadInode(dir_ino, &dir) != 0) return -1;
    if ((dir.i_mode & EXT4_S_IFMT) != EXT4_S_IFDIR) return -1;

    // check empty (only . and .. entries)
    Ext4DirInfo entries[4];
    int n = ListDir(path, entries, 4);
    int real_entries = 0;
    for (int i = 0; i < n; i++) {
        if (!e4_seq(entries[i].name, ".") && !e4_seq(entries[i].name, ".."))
            real_entries++;
    }
    if (real_entries > 0) return -1;  // not empty

    char parent_path[EXT4_MAX_PATH];
    char basename[EXT4_NAME_LEN + 1];
    SplitPath(path, parent_path, basename);

    uint32_t parent_ino;
    if (PathWalk(parent_path, &parent_ino) != 0) return -1;

    DirRemoveEntry(parent_ino, basename);
    FreeInode(dir_ino);

    // decrement parent link count
    Ext4Inode parent_in;
    if (ReadInode(parent_ino, &parent_in) == 0) {
        if (parent_in.i_links_count > 0) parent_in.i_links_count--;
        uint64_t blk;
        uint32_t off;
        if (GetInodeBlock(parent_ino, &blk, &off) == 0) {
            WriteBytes(blk * block_size + off,
                       (inode_size < sizeof(Ext4Inode))
                       ? inode_size : sizeof(Ext4Inode), &parent_in);
        }
    }

    return 0;
}

//  convenience helpers

int Ext4::ReadWholeFile(const char* path, void* buf, uint32_t max_len) {
    Ext4Inode in;
    if (ReadInodeByPath(path, &in) != 0) return -1;
    uint64_t sz = in.i_size_lo | ((uint64_t)in.i_size_high << 32);
    if (sz > max_len) sz = max_len;
    return ReadInodeData(&in, 0, (uint32_t)sz, buf);
}

int Ext4::ReadString(const char* path, char* buf, int max_len) {
    int r = ReadWholeFile(path, buf, max_len - 1);
    if (r >= 0) buf[r] = 0;
    return r;
}

int Ext4::WriteString(const char* path, const char* str) {
    int len = e4_slen(str);
    return WriteFile(path, str, len);
}

uint64_t Ext4::TotalBlocks() {
    if (!mounted) return 0;
    uint64_t t = sb.s_blocks_count_lo;
    if (sb.s_feature_incompat & 0x80) t |= ((uint64_t)sb.s_blocks_count_hi << 32);
    return t;
}

uint64_t Ext4::FreeBlocks() {
    if (!mounted) return 0;
    uint64_t f = sb.s_free_blocks_count_lo;
    if (sb.s_feature_incompat & 0x80) f |= ((uint64_t)sb.s_free_blocks_count_hi << 32);
    return f;
}

uint32_t Ext4::BlockSize() { return mounted ? block_size : 0; }

const char* Ext4::VolumeName() {
    return mounted ? sb.s_volume_name : "";
}

//  durability / flush
//
//  this driver is write-through: every mutation (inode, block + inode bitmaps,
//  group descriptors, superblock) is pushed to the block layer via WriteBytes
//  the moment it happens. so a sync only needs to drive a device-level cache
//  flush to guarantee those writes have actually reached stable storage. these
//  are free functions (not Ext4 members) so the public header doesn't change;
//  callers forward-declare and invoke them. (satoru)

// flush the whole device write cache (general sync). returns 0 on success,
// -1 if nothing is mounted, -2 if the device flush command failed. (satoru)
int Ext4Sync() {
    if (!Ext4::IsMounted()) return -1;
    return NVMe::Flush() ? 0 : -2;
}

// force durability for a single file's data + metadata. since all writes are
// already write-through to the block layer, this is a device-cache barrier.
// the inode number is accepted for api symmetry/logging. returns 0 on success,
// -1 if not mounted, -2 if the device flush failed. (satoru)
int Ext4Fsync(uint32_t inode_num) {
    (void)inode_num;
    if (!Ext4::IsMounted()) return -1;
    return NVMe::Flush() ? 0 : -2;
}

// end (satoru)
