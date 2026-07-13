#include "kfs.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"
#include "../drivers/serial.h"

//  KFS implementation - see kfs.h. the persistence layer formats a fresh volume
//  each save and writes the user-data tree as real files + dirs, so a bump-style
//  allocator hands out CONTIGUOUS block runs and a whole file goes to disk in one
//  multi-page nvme command and is described by a SINGLE extent. the bitmap is
//  still maintained on disk (it's part of the format spec a fuse driver reads),
//  allocation just never has to search it on a fresh volume. metadata (superblock
//  + bitmap + inode table) is cached in ram and flushed in Sync().
//
//  EXTENT-BASED (KFS v2): files/dirs are lists of extents {start,len}, NOT
//  per-block pointers - 23 inline in the inode + an unbounded overflow chain. no
//  ~4 MB file cap, no max-dir-entry cap, and a 174 MB binary is one extent. tiny
//  files (<= KFS_INLINE_MAX) live inline in the inode with no data block. (satoru)

namespace {
    KFSReadFn  g_rd  = nullptr;
    KFSWriteFn g_wr  = nullptr;
    void*      g_ctx = nullptr;

    KFSSuper   g_sb;
    bool       g_mounted = false;

    uint8_t*   g_bitmap = nullptr;   // in-ram free-block bitmap (pmm-backed) (satoru)
    uint8_t*   g_inodes = nullptr;   // in-ram inode table (pmm-backed) (satoru)
    uint32_t   g_bitmap_bytes = 0;   // pmm allocation sizes for FreeBytes (satoru)
    uint32_t   g_inodes_bytes = 0;
    uint32_t   g_next   = 0;         // bump hint for contiguous allocation (satoru)

    KFSStats   g_stats = {};

    //  a page-aligned, physically-contiguous bounce for multi-page block i/o. one
    //  command moves at most this many blocks (matches the nvme prp-list cap of
    //  512 pages / ~2 MB); bigger files are written in successive runs. (satoru)
    uint8_t*   g_bounce = nullptr;
    uint32_t   g_bounce_cap = 0;
    static const uint32_t KFS_MAX_RUN_BLOCKS = 512;

    uint32_t crc32(const uint8_t* p, uint32_t n) {
        uint32_t c = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1))); }
        return ~c;
    }

    bool rd_block(uint64_t b, uint32_t n, void* buf)        { return g_rd && g_rd(b, n, buf, g_ctx); }
    bool wr_block(uint64_t b, uint32_t n, const void* buf)  { return g_wr && g_wr(b, n, buf, g_ctx); }

    KFSInode* inode_ptr(uint32_t ino) {
        if (ino == 0 || ino >= g_sb.inode_count || !g_inodes) return nullptr;
        return (KFSInode*)(g_inodes + (uint64_t)ino * KFS_INODE_SIZE);
    }

    void bit_set(uint32_t b) { if (g_bitmap) g_bitmap[b >> 3] |= (uint8_t)(1u << (b & 7)); }
    bool bit_get(uint32_t b) { return g_bitmap && ((g_bitmap[b >> 3] >> (b & 7)) & 1u); }

    //  bump-allocate `n` contiguous data blocks; returns the first, or 0 if full.
    //  on a fresh volume the hint walks straight up, so runs are contiguous and a
    //  whole file is a single extent. when the bump space is exhausted, fall back
    //  to a first-fit scan of the bitmap: the bump path never REUSES free space,
    //  so without the fallback a volume can report "full" while mostly free and a
    //  big file then silently fails to write (the zombie-inode source). (satoru)
    uint32_t alloc_run(uint32_t n) {
        if (n == 0) return 0;
        if (g_next < g_sb.data_start) g_next = g_sb.data_start;
        if ((uint64_t)g_next + n <= g_sb.total_blocks) {
            uint32_t start = g_next;
            for (uint32_t i = 0; i < n; i++) bit_set(start + i);
            g_next += n;
            g_sb.free_blocks -= n;
            return start;
        }
        // first-fit bitmap scan for a free run of n. safe because every alloc
        // path maintains the bitmap (bit_set above) and Mount loads it from
        // disk, so a clear bit really is free space. (satoru)
        uint32_t run = 0, s = 0;
        for (uint32_t b = g_sb.data_start; b < g_sb.total_blocks; b++) {
            if (bit_get(b)) { run = 0; continue; }
            if (run == 0) s = b;
            if (++run == n) {
                for (uint32_t i = 0; i < n; i++) bit_set(s + i);
                g_sb.free_blocks -= n;
                return s;
            }
        }
        return 0;
    }
    uint32_t alloc_block() { return alloc_run(1); }

    //  ensure the bounce buffer holds at least `bytes`. (satoru)
    bool ensure_bounce(uint32_t bytes) {
        if (g_bounce && g_bounce_cap >= bytes) return true;
        if (g_bounce) PMM::FreeBytes(g_bounce, g_bounce_cap);
        uint32_t cap = (bytes + KFS_BLOCK_SIZE - 1) & ~(KFS_BLOCK_SIZE - 1);
        g_bounce = (uint8_t*)PMM::AllocBytes(cap);
        g_bounce_cap = g_bounce ? cap : 0;
        return g_bounce != nullptr;
    }

    //  ── extent helpers ──────────────────────────────────────────────────────
    //  append one extent {start,len} to an inode, auto-merging with the previous
    //  extent if it is physically adjacent (so the snapshot writer's contiguous
    //  runs collapse to a single extent). spills to an overflow-block chain when
    //  the inline slots fill, so the extent list is unbounded. (satoru)
    bool ext_append(KFSInode* n, uint32_t start, uint32_t len) {
        if (len == 0) return true;
        // try to merge with the last extent (inline or last overflow). (satoru)
        if (n->ext_count > 0) {
            if (n->ext_count <= KFS_INLINE_EXTENTS) {
                KFSExtent* last = &n->extent[n->ext_count - 1];
                if (last->start + last->len == start) { last->len += len; n->blocks += len; return true; }
            } else if (n->ext_overflow) {
                uint8_t blk[KFS_BLOCK_SIZE];
                uint32_t cur = n->ext_overflow;
                while (cur) {
                    if (!rd_block(cur, 1, blk)) return false;
                    KFSExtOverflow* o = (KFSExtOverflow*)blk;
                    if (o->next == 0) {
                        if (o->count > 0) {
                            KFSExtent* last = &o->ext[o->count - 1];
                            if (last->start + last->len == start) {
                                last->len += len; n->blocks += len;
                                return wr_block(cur, 1, blk);
                            }
                        }
                        break;
                    }
                    cur = o->next;
                }
            }
        }
        // store inline while there is room. (satoru)
        if (n->ext_count < KFS_INLINE_EXTENTS) {
            n->extent[n->ext_count].start = start;
            n->extent[n->ext_count].len   = len;
            n->ext_count++; n->blocks += len; g_stats.extents_used++;
            return true;
        }
        // spill to an overflow block: walk to the last block with room, else add. (satoru)
        uint8_t blk[KFS_BLOCK_SIZE];
        uint32_t cur = n->ext_overflow, last_blk = 0;
        while (cur) {
            if (!rd_block(cur, 1, blk)) return false;
            KFSExtOverflow* o = (KFSExtOverflow*)blk;
            last_blk = cur;
            if (o->count < KFS_EXT_PER_OVF) {
                o->ext[o->count].start = start; o->ext[o->count].len = len;
                o->count++;
                if (!wr_block(cur, 1, blk)) return false;
                n->ext_count++; n->blocks += len; g_stats.extents_used++;
                return true;
            }
            cur = o->next;
        }
        // allocate a new overflow block. (satoru)
        uint32_t nb = alloc_block();
        if (!nb) return false;
        for (uint32_t k = 0; k < KFS_BLOCK_SIZE; k++) blk[k] = 0;
        KFSExtOverflow* o = (KFSExtOverflow*)blk;
        o->next = 0; o->count = 1; o->ext[0].start = start; o->ext[0].len = len;
        if (!wr_block(nb, 1, blk)) return false;
        if (last_blk == 0) {
            n->ext_overflow = nb;
        } else {
            // patch the previous tail's next pointer. (satoru)
            uint8_t pblk[KFS_BLOCK_SIZE];
            if (!rd_block(last_blk, 1, pblk)) return false;
            ((KFSExtOverflow*)pblk)->next = nb;
            if (!wr_block(last_blk, 1, pblk)) return false;
        }
        n->ext_count++; n->blocks += len; g_stats.extents_used++;
        return true;
    }

    //  resolve the i'th logical data block of an inode to its physical block by
    //  walking the extents. linear in the extent count, which is tiny (≈1 for a
    //  contiguous file). (satoru)
    uint32_t logical_to_phys(KFSInode* n, uint32_t lblk) {
        uint32_t base = 0;
        uint32_t inline_n = n->ext_count < KFS_INLINE_EXTENTS ? n->ext_count : KFS_INLINE_EXTENTS;
        for (uint32_t i = 0; i < inline_n; i++) {
            if (lblk < base + n->extent[i].len) return n->extent[i].start + (lblk - base);
            base += n->extent[i].len;
        }
        uint8_t blk[KFS_BLOCK_SIZE];
        uint32_t cur = (n->ext_count > KFS_INLINE_EXTENTS) ? n->ext_overflow : 0;
        while (cur) {
            if (!rd_block(cur, 1, blk)) return 0;
            KFSExtOverflow* o = (KFSExtOverflow*)blk;
            for (uint32_t i = 0; i < o->count; i++) {
                if (lblk < base + o->ext[i].len) return o->ext[i].start + (lblk - base);
                base += o->ext[i].len;
            }
            cur = o->next;
        }
        return 0;
    }

    //  path helpers - split into components, no allocation. (satoru)
    bool comp_eq(const char* a, const char* b, int blen) {
        for (int i = 0; i < blen; i++) if (a[i] != b[i]) return false;
        return a[blen] == 0;
    }

    //  ── directory machinery (extent-backed, unbounded entries) ───────────────
    //  the directory's data is a run of dir-entry blocks described by the inode's
    //  extents (64 entries/block). lookup/add/list walk those blocks. (satoru)
    uint32_t dir_lookup(KFSInode* dir, const char* name, int nlen) {
        if (!dir || dir->type != KFS_DIR) return 0;
        uint8_t blk[KFS_BLOCK_SIZE];
        uint32_t nblocks = (uint32_t)((dir->size + KFS_BLOCK_SIZE - 1) / KFS_BLOCK_SIZE);
        for (uint32_t i = 0; i < nblocks; i++) {
            uint32_t pb = logical_to_phys(dir, i);
            if (!pb) continue;
            if (!rd_block(pb, 1, blk)) return 0;
            KFSDirEnt* e = (KFSDirEnt*)blk;
            for (uint32_t j = 0; j < KFS_BLOCK_SIZE / sizeof(KFSDirEnt); j++) {
                if (e[j].inode && e[j].name_len == (uint16_t)nlen &&
                    comp_eq(e[j].name, name, nlen))
                    return e[j].inode;
            }
        }
        return 0;
    }

    //  add (name -> child) to directory `dir`, growing it a block at a time. no
    //  cap on entries - growth chains extents like a file. (satoru)
    bool dir_add(KFSInode* dir, const char* name, int nlen, uint32_t child, KFSType t) {
        if (!dir || nlen <= 0 || nlen > KFS_NAME_MAX) return false;
        uint8_t blk[KFS_BLOCK_SIZE];
        uint32_t per = KFS_BLOCK_SIZE / sizeof(KFSDirEnt);
        uint32_t nblocks = (uint32_t)((dir->size + KFS_BLOCK_SIZE - 1) / KFS_BLOCK_SIZE);
        // try to reuse a free slot in an existing block. (satoru)
        for (uint32_t i = 0; i < nblocks; i++) {
            uint32_t pb = logical_to_phys(dir, i);
            if (!pb) continue;
            if (!rd_block(pb, 1, blk)) return false;
            KFSDirEnt* e = (KFSDirEnt*)blk;
            for (uint32_t j = 0; j < per; j++) {
                if (!e[j].inode) {
                    e[j].inode = child; e[j].name_len = (uint16_t)nlen; e[j].type = (uint16_t)t;
                    for (int k = 0; k < nlen; k++) e[j].name[k] = name[k];
                    e[j].name[nlen] = 0;
                    return wr_block(pb, 1, blk);
                }
            }
        }
        // grow: allocate a fresh dir block and record it as an extent. (satoru)
        uint32_t nb = alloc_block();
        if (!nb) return false;
        for (uint32_t k = 0; k < KFS_BLOCK_SIZE; k++) blk[k] = 0;
        KFSDirEnt* e = (KFSDirEnt*)blk;
        e[0].inode = child; e[0].name_len = (uint16_t)nlen; e[0].type = (uint16_t)t;
        for (int k = 0; k < nlen; k++) e[0].name[k] = name[k];
        e[0].name[nlen] = 0;
        if (!wr_block(nb, 1, blk)) return false;
        if (!ext_append(dir, nb, 1)) return false;
        dir->size = (uint64_t)(nblocks + 1) * KFS_BLOCK_SIZE;
        return true;
    }

    //  resolve an absolute path to its inode number; 0 if any component is
    //  missing. (satoru)
    uint32_t resolve(const char* path) {
        if (!path || path[0] != '/') return 0;
        uint32_t cur = KFS_ROOT_INODE;
        const char* p = path + 1;
        while (*p) {
            const char* s = p;
            while (*p && *p != '/') p++;
            int nlen = (int)(p - s);
            if (nlen > 0) {
                KFSInode* d = inode_ptr(cur);
                cur = dir_lookup(d, s, nlen);
                if (!cur) return 0;
            }
            while (*p == '/') p++;
        }
        return cur;
    }

    //  allocate + initialize a fresh inode of `type`; returns its number or 0. (satoru)
    uint32_t new_inode(KFSType type, uint16_t mode) {
        for (uint32_t i = 2; i < g_sb.inode_count; i++) {
            KFSInode* n = inode_ptr(i);
            if (n && n->type == KFS_FREE) {
                for (uint32_t k = 0; k < KFS_INODE_SIZE; k++) ((uint8_t*)n)[k] = 0;
                n->type = type; n->mode = mode; n->nlink = (type == KFS_DIR) ? 2 : 1;
                return i;
            }
        }
        return 0;
    }

    //  make `path` a directory (creating parents); returns its inode or 0. (satoru)
    uint32_t mkdirs_ino(const char* path) {
        if (!path || path[0] != '/') return 0;
        uint32_t cur = KFS_ROOT_INODE;
        const char* p = path + 1;
        while (*p) {
            const char* s = p;
            while (*p && *p != '/') p++;
            int nlen = (int)(p - s);
            if (nlen > 0) {
                KFSInode* d = inode_ptr(cur);
                uint32_t child = dir_lookup(d, s, nlen);
                if (!child) {
                    child = new_inode(KFS_DIR, 0755);
                    if (!child) return 0;
                    if (!dir_add(d, s, nlen, child, KFS_DIR)) return 0;
                    g_stats.dirs_made++;
                }
                cur = child;
            }
            while (*p == '/') p++;
        }
        return cur;
    }

    //  split "/a/b/c" -> parent inode of "c" + the leaf name. parent_path is a
    //  4096-byte buffer (linux PATH_MAX) so deep canonical paths fit; it is on
    //  the heap because the recursion makes a 4 KB stack buffer here unsafe. (satoru)
    bool split_parent(const char* path, uint32_t* parent, const char** leaf, int* leaf_len, bool make) {
        if (!path || path[0] != '/') return false;
        const char* last = path + 1;
        const char* p = path + 1;
        const char* lstart = path + 1;
        while (*p) {
            if (*p == '/') { if (p[1]) lstart = p + 1; }
            p++;
        }
        last = lstart;
        int llen = 0; while (last[llen] && last[llen] != '/') llen++;
        if (llen == 0) return false;
        int plen = (int)(last - path);
        if (plen <= 1) { *parent = KFS_ROOT_INODE; }
        else {
            char* parent_path = (char*)KernelHeap::Alloc(4096);
            if (!parent_path) return false;
            if (plen >= 4096) { KernelHeap::Free(parent_path); return false; }
            for (int i = 0; i < plen; i++) parent_path[i] = path[i];
            parent_path[plen] = 0;
            *parent = make ? mkdirs_ino(parent_path) : resolve(parent_path);
            KernelHeap::Free(parent_path);
            if (!*parent) return false;
        }
        *leaf = last; *leaf_len = llen;
        return true;
    }

    //  read file inode `n` into buf (up to max). returns bytes or -1. reads each
    //  contiguous physical run through the page-aligned bounce (one multi-page
    //  command per ≤2 MB chunk). inline files copy straight out of the inode. (satoru)
    int64_t read_file(KFSInode* n, void* buf, uint64_t max) {
        if (!n || (n->type != KFS_FILE && n->type != KFS_SYMLINK)) return -1;
        uint64_t len = n->size; if (len > max) len = max;
        if (len == 0) return 0;
        uint8_t* out = (uint8_t*)buf;
        if (n->flags & KFS_FLAG_INLINE) {
            for (uint64_t i = 0; i < len; i++) out[i] = n->inline_data[i];
            return (int64_t)len;
        }
        uint64_t done = 0;
        uint32_t lblk = 0;
        uint32_t total_blocks = (uint32_t)((n->size + KFS_BLOCK_SIZE - 1) / KFS_BLOCK_SIZE);
        while (done < len && lblk < total_blocks) {
            // find the longest contiguous physical run starting at lblk. (satoru)
            uint32_t pstart = logical_to_phys(n, lblk);
            if (!pstart) return -1;
            uint32_t run = 1;
            while (lblk + run < total_blocks && run < KFS_MAX_RUN_BLOCKS) {
                if (logical_to_phys(n, lblk + run) == pstart + run) run++;
                else break;
            }
            uint32_t run_bytes = run * KFS_BLOCK_SIZE;
            if (!ensure_bounce(run_bytes)) return -1;
            if (!rd_block(pstart, run, g_bounce)) return -1;
            uint64_t copy = len - done; if (copy > run_bytes) copy = run_bytes;
            for (uint64_t i = 0; i < copy; i++) out[done + i] = g_bounce[i];
            done += copy; lblk += run;
        }
        return (int64_t)done;
    }

    //  common writer for files + symlinks: store small payloads inline, large ones
    //  as one (or a few) extents. (satoru)
    bool write_payload(KFSInode* f, const void* data, uint64_t len, KFSType t) {
        f->type = t;
        // stamp size LAST: stamping it up-front meant any failure below left a
        // ZOMBIE inode (size > 0 with no covering extents) - FileSize() then
        // reports the full size forever while ReadFile() returns -1 (the
        // firefox cursor "short read got=-1" restore failures). (satoru)
        f->size = 0;
        f->flags = 0; f->ext_count = 0; f->ext_overflow = 0; f->blocks = 0;
        if (len == 0) return true;

        // tiny payload -> store inline, no data block. (satoru)
        if (len <= KFS_INLINE_MAX) {
            const uint8_t* src = (const uint8_t*)data;
            for (uint64_t i = 0; i < len; i++) f->inline_data[i] = src[i];
            for (uint64_t i = len; i < KFS_INLINE_MAX; i++) f->inline_data[i] = 0;
            f->flags |= KFS_FLAG_INLINE;
            f->size = len;
            g_stats.inline_files++;
            return true;
        }

        // one contiguous run for the whole file -> a single extent + a few
        // multi-page commands. on any failure roll the inode back to an empty
        // (size 0) file so it can never restore as a zombie. (satoru)
        uint32_t nblocks = (uint32_t)((len + KFS_BLOCK_SIZE - 1) / KFS_BLOCK_SIZE);
        uint32_t start = alloc_run(nblocks);
        if (!start) return false;
        if (!ext_append(f, start, nblocks)) {
            f->ext_count = 0; f->ext_overflow = 0; f->blocks = 0;
            return false;
        }

        const uint8_t* src = (const uint8_t*)data;
        uint32_t per = KFS_MAX_RUN_BLOCKS;
        uint64_t off = 0;
        uint32_t blk_off = 0;
        bool ok = true;
        while (off < len) {
            uint32_t run_blocks = nblocks - blk_off; if (run_blocks > per) run_blocks = per;
            uint32_t run_bytes  = run_blocks * KFS_BLOCK_SIZE;
            if (!ensure_bounce(run_bytes)) { ok = false; break; }
            uint64_t copy = len - off; if (copy > run_bytes) copy = run_bytes;
            for (uint64_t i = 0; i < copy; i++) g_bounce[i] = src[off + i];
            for (uint64_t i = copy; i < run_bytes; i++) g_bounce[i] = 0;   // zero-pad tail (satoru)
            if (!wr_block(start + blk_off, run_blocks, g_bounce)) { ok = false; break; }
            off     += copy;
            blk_off += run_blocks;
        }
        if (!ok) {
            f->ext_count = 0; f->ext_overflow = 0; f->blocks = 0;   // rollback (blocks leak until next format) (satoru)
            return false;
        }
        f->size = len;
        return true;
    }
}

void KFS::SetBackend(KFSReadFn rd, KFSWriteFn wr, void* ctx) { g_rd = rd; g_wr = wr; g_ctx = ctx; }
bool KFS::IsMounted() { return g_mounted; }
const KFSStats& KFS::Stats() { return g_stats; }
void KFS::ResetStats() { for (uint32_t i = 0; i < sizeof(g_stats); i++) ((uint8_t*)&g_stats)[i] = 0; }

//  layer 6: the 64-bit user-data fingerprint lives in superblock reserved[0..1]
//  (lo, hi). it survives reboot because Sync() writes the superblock. (satoru)
uint64_t KFS::MountedFingerprint() {
    if (!g_mounted) return 0;
    return ((uint64_t)g_sb.reserved[1] << 32) | g_sb.reserved[0];
}
void KFS::SetFingerprint(uint64_t fp) {
    g_sb.reserved[0] = (uint32_t)(fp & 0xFFFFFFFFu);
    g_sb.reserved[1] = (uint32_t)(fp >> 32);
}

bool KFS::Format(uint32_t total_blocks) {
    if (!g_wr || total_blocks < 64) return false;

    KFSSuper sb;
    for (uint32_t i = 0; i < sizeof(sb); i++) ((uint8_t*)&sb)[i] = 0;
    sb.magic = KFS_MAGIC; sb.version = KFS_VERSION; sb.block_size = KFS_BLOCK_SIZE;
    sb.inode_size    = KFS_INODE_SIZE;
    sb.total_blocks  = total_blocks;
    sb.bitmap_start  = 1;
    sb.bitmap_blocks = (total_blocks + (KFS_BLOCK_SIZE * 8 - 1)) / (KFS_BLOCK_SIZE * 8);
    sb.inode_count   = total_blocks / 32; if (sb.inode_count < 256) sb.inode_count = 256;
    sb.inode_start   = sb.bitmap_start + sb.bitmap_blocks;
    sb.inode_blocks  = (sb.inode_count + KFS_INODES_PER_BLOCK - 1) / KFS_INODES_PER_BLOCK;
    sb.data_start    = sb.inode_start + sb.inode_blocks;
    if (sb.data_start >= total_blocks) return false;
    sb.free_blocks   = total_blocks - sb.data_start;

    // (re)allocate the in-ram metadata caches. pmm-backed, NOT kernel-heap: the
    // ~36 mb inode table + bitmap were prime targets for the documented stale-
    // writer class (a use-after-free write landing inside the inode table reads
    // back as garbage sizes/extents); pmm blocks live off the heap freelist
    // entirely, out of that blast radius. (satoru)
    if (g_bitmap) PMM::FreeBytes(g_bitmap, g_bitmap_bytes);
    if (g_inodes) PMM::FreeBytes(g_inodes, g_inodes_bytes);
    g_bitmap_bytes = sb.bitmap_blocks * KFS_BLOCK_SIZE;
    g_inodes_bytes = sb.inode_blocks  * KFS_BLOCK_SIZE;
    g_bitmap = (uint8_t*)PMM::AllocBytes(g_bitmap_bytes);
    g_inodes = (uint8_t*)PMM::AllocBytes(g_inodes_bytes);
    if (!g_bitmap || !g_inodes) return false;
    for (uint32_t i = 0; i < sb.bitmap_blocks * KFS_BLOCK_SIZE; i++) g_bitmap[i] = 0;
    for (uint32_t i = 0; i < sb.inode_blocks  * KFS_BLOCK_SIZE; i++) g_inodes[i] = 0;

    g_sb = sb;
    g_next = sb.data_start;
    ResetStats();
    // mark all metadata blocks used. (satoru)
    for (uint32_t b = 0; b < sb.data_start; b++) bit_set(b);
    // root directory. (satoru)
    KFSInode* root = inode_ptr(KFS_ROOT_INODE);
    root->type = KFS_DIR; root->mode = 0755; root->nlink = 2; root->size = 0;

    g_mounted = true;
    return Sync();
}

//  validate an on-disk superblock before we trust ANY of its size/count fields
//  to size an allocation or a read. the crc only proves the block is internally
//  consistent - it does NOT stop a malicious image (an attacker recomputes the
//  crc), so every field is bounded here independently. all arithmetic is 64-bit
//  so the u32 fields can't overflow a size calc (e.g. bitmap_blocks * 4096
//  wrapping a u32 to allocate a tiny buffer that a large read then overflows).
//  returns true iff the layout is self-consistent and inside sane maxima. (satoru)
static bool kfs_super_sane(const KFSSuper* sb) {
    // hard ceiling on the volume: the in-ram metadata caches scale with it
    // (bitmap = total/8 bytes, inode table = total*8 bytes), so persist.cpp caps
    // a real volume at ~59M blocks. mirror that here as the trust boundary. (satoru)
    const uint64_t MAX_TOTAL_BLOCKS = 59000000ull;

    uint64_t total   = sb->total_blocks;
    uint64_t bm_strt = sb->bitmap_start;
    uint64_t bm_blks = sb->bitmap_blocks;
    uint64_t in_strt = sb->inode_start;
    uint64_t in_blks = sb->inode_blocks;
    uint64_t in_cnt  = sb->inode_count;
    uint64_t dt_strt = sb->data_start;

    // basic ranges. (satoru)
    if (total < 64 || total > MAX_TOTAL_BLOCKS) return false;
    if (bm_strt != 1) return false;                 // bitmap always follows the superblock (satoru)
    if (bm_blks == 0 || in_blks == 0 || in_cnt < 2) return false;

    // the bitmap must be big enough to cover every block, and not absurdly big.
    // (satoru)
    uint64_t need_bm = (total + (KFS_BLOCK_SIZE * 8 - 1)) / (KFS_BLOCK_SIZE * 8);
    if (bm_blks < need_bm || bm_blks > need_bm + 1) return false;

    // the inode table must exactly cover inode_count inodes (16 per block). (satoru)
    uint64_t need_in = (in_cnt + KFS_INODES_PER_BLOCK - 1) / KFS_INODES_PER_BLOCK;
    if (in_blks != need_in) return false;
    if (in_cnt > total) return false;               // can't have more inodes than blocks (satoru)

    // the in-ram caches are allocated as (blocks * KFS_BLOCK_SIZE) BYTES with a
    // u32 multiply; even within the block ceiling above, inode_blocks*4096 can
    // overflow a u32. bound both byte sizes in 64-bit so the allocation size is
    // exactly what the subsequent read fills (no wrap -> tiny-buffer overflow).
    // cap at 512 MB each (matches persist.cpp's metadata-cache budget). (satoru)
    const uint64_t MAX_CACHE_BYTES = 512ull * 1024 * 1024;
    if (bm_blks * (uint64_t)KFS_BLOCK_SIZE > MAX_CACHE_BYTES) return false;
    if (in_blks * (uint64_t)KFS_BLOCK_SIZE > MAX_CACHE_BYTES) return false;

    // the regions must be laid out in order, non-overlapping, and entirely inside
    // the volume: [sb][bitmap][inodes][data...]. (satoru)
    if (in_strt != bm_strt + bm_blks) return false;
    if (dt_strt != in_strt + in_blks) return false;
    if (dt_strt >= total) return false;             // at least one data block (satoru)
    if (sb->free_blocks > total) return false;

    return true;
}

bool KFS::Mount() {
    if (!g_rd) return false;
    uint8_t blk[KFS_BLOCK_SIZE];
    if (!rd_block(0, 1, blk)) return false;
    KFSSuper* sb = (KFSSuper*)blk;
    if (sb->magic != KFS_MAGIC || sb->version != KFS_VERSION || sb->block_size != KFS_BLOCK_SIZE)
        return false;
    if (crc32((uint8_t*)sb, __builtin_offsetof(KFSSuper, crc)) != sb->crc) return false;
    // gate on a fully validated layout BEFORE any field is used to size an
    // allocation or a read - a corrupt/malicious superblock that passes magic +
    // crc must not be able to overflow g_bitmap / g_inodes. (satoru)
    if (!kfs_super_sane(sb)) {
        SerialLogger::Log("[KFS] superblock failed sanity check; refusing mount\r\n");
        return false;
    }
    g_sb = *sb;

    if (g_bitmap) PMM::FreeBytes(g_bitmap, g_bitmap_bytes);
    if (g_inodes) PMM::FreeBytes(g_inodes, g_inodes_bytes);
    g_bitmap_bytes = g_sb.bitmap_blocks * KFS_BLOCK_SIZE;
    g_inodes_bytes = g_sb.inode_blocks  * KFS_BLOCK_SIZE;
    g_bitmap = (uint8_t*)PMM::AllocBytes(g_bitmap_bytes);
    g_inodes = (uint8_t*)PMM::AllocBytes(g_inodes_bytes);
    if (!g_bitmap || !g_inodes) return false;
    if (!rd_block(g_sb.bitmap_start, g_sb.bitmap_blocks, g_bitmap)) return false;
    if (!rd_block(g_sb.inode_start,  g_sb.inode_blocks,  g_inodes)) return false;
    // start the bump hint PAST the highest used block. it used to reset to
    // data_start here, and since the bump allocator never consults the bitmap,
    // ANY write to a mounted (not freshly formatted) volume re-allocated blocks
    // from data_start - silently overwriting the data of every existing file on
    // disk. scan the just-loaded bitmap for the high-water mark instead. (satoru)
    {
        uint32_t hi = g_sb.data_start;
        uint32_t bytes = (g_sb.total_blocks + 7u) / 8u;
        for (uint32_t i = bytes; i > 0; i--) {
            if (g_bitmap[i - 1] == 0) continue;
            uint32_t bit = 7;
            while (bit > 0 && !((g_bitmap[i - 1] >> bit) & 1u)) bit--;
            hi = (i - 1) * 8u + bit + 1u;
            break;
        }
        if (hi < g_sb.data_start)   hi = g_sb.data_start;
        if (hi > g_sb.total_blocks) hi = g_sb.total_blocks;
        g_next = hi;
    }
    g_mounted = true;
    return true;
}

void KFS::Unmount() {
    // drop the in-ram metadata caches - the bitmap + inode table together run
    // ~36 mb on the 4 gb data disk and were held for the whole session after a
    // boot restore. callers that WROTE must Sync() first; this only frees.
    // the next Mount() re-reads everything from disk. (satoru)
    if (!g_mounted) return;
    if (g_bitmap) { PMM::FreeBytes(g_bitmap, g_bitmap_bytes); g_bitmap = nullptr; g_bitmap_bytes = 0; }
    if (g_inodes) { PMM::FreeBytes(g_inodes, g_inodes_bytes); g_inodes = nullptr; g_inodes_bytes = 0; }
    if (g_bounce) { PMM::FreeBytes(g_bounce, g_bounce_cap); g_bounce = nullptr; g_bounce_cap = 0; }
    g_mounted = false;
}

bool KFS::Sync() {
    if (!g_mounted) return false;
    // superblock (re-checksum). (satoru)
    uint8_t blk[KFS_BLOCK_SIZE];
    for (uint32_t i = 0; i < KFS_BLOCK_SIZE; i++) blk[i] = 0;
    g_sb.crc = crc32((uint8_t*)&g_sb, __builtin_offsetof(KFSSuper, crc));
    for (uint32_t i = 0; i < sizeof(KFSSuper); i++) blk[i] = ((uint8_t*)&g_sb)[i];
    if (!wr_block(0, 1, blk)) return false;
    if (!wr_block(g_sb.bitmap_start, g_sb.bitmap_blocks, g_bitmap)) return false;
    if (!wr_block(g_sb.inode_start,  g_sb.inode_blocks,  g_inodes)) return false;
    return true;
}

bool KFS::Mkdirs(const char* path) {
    if (!g_mounted) return false;
    return mkdirs_ino(path) != 0;
}

bool KFS::WriteFile(const char* path, const void* data, uint64_t len) {
    if (!g_mounted || !path) return false;

    uint32_t parent; const char* leaf; int llen;
    if (!split_parent(path, &parent, &leaf, &llen, true)) return false;
    KFSInode* pdir = inode_ptr(parent);
    if (!pdir) return false;

    // fresh file inode (snapshot model: paths are unique per format). (satoru)
    uint32_t fino = new_inode(KFS_FILE, 0644);
    if (!fino) return false;
    if (!dir_add(pdir, leaf, llen, fino, KFS_FILE)) return false;
    KFSInode* f = inode_ptr(fino);
    if (!write_payload(f, data, len, KFS_FILE)) return false;
    g_stats.files_written++; g_stats.bytes_written += len;
    return true;
}

int64_t KFS::ReadFile(const char* path, void* buf, uint64_t max) {
    if (!g_mounted) return -1;
    KFSInode* n = inode_ptr(resolve(path));
    if (n && n->type == KFS_SYMLINK) return -1;  // symlinks read via ReadLink (satoru)
    return read_file(n, buf, max);
}

bool KFS::Symlink(const char* path, const char* target) {
    if (!g_mounted || !path || !target) return false;
    uint32_t parent; const char* leaf; int llen;
    if (!split_parent(path, &parent, &leaf, &llen, true)) return false;
    KFSInode* pdir = inode_ptr(parent);
    if (!pdir) return false;
    uint32_t sino = new_inode(KFS_SYMLINK, 0777);
    if (!sino) return false;
    if (!dir_add(pdir, leaf, llen, sino, KFS_SYMLINK)) return false;
    KFSInode* s = inode_ptr(sino);
    uint64_t tlen = 0; while (target[tlen]) tlen++;
    return write_payload(s, target, tlen, KFS_SYMLINK);
}

int KFS::ReadLink(const char* path, char* buf, int max) {
    if (!g_mounted || !buf || max <= 0) return -1;
    KFSInode* n = inode_ptr(resolve(path));
    if (!n || n->type != KFS_SYMLINK) return -1;
    int64_t got = read_file(n, buf, (uint64_t)(max - 1));
    if (got < 0) return -1;
    buf[got] = 0;
    return (int)got;
}

bool KFS::Exists(const char* path)  { return g_mounted && resolve(path) != 0; }
bool KFS::IsDir(const char* path)   { KFSInode* n = g_mounted ? inode_ptr(resolve(path)) : nullptr; return n && n->type == KFS_DIR; }
bool KFS::IsSymlink(const char* path){ KFSInode* n = g_mounted ? inode_ptr(resolve(path)) : nullptr; return n && n->type == KFS_SYMLINK; }
int64_t KFS::FileSize(const char* path){ KFSInode* n = g_mounted ? inode_ptr(resolve(path)) : nullptr; return (n && n->type == KFS_FILE) ? (int64_t)n->size : -1; }

int KFS::List(const char* path, ListCb cb, void* ctx) {
    if (!g_mounted || !cb) return -1;
    KFSInode* d = inode_ptr(resolve(path));
    if (!d || d->type != KFS_DIR) return -1;
    uint8_t blk[KFS_BLOCK_SIZE];
    uint32_t nblocks = (uint32_t)((d->size + KFS_BLOCK_SIZE - 1) / KFS_BLOCK_SIZE);
    int count = 0;
    for (uint32_t i = 0; i < nblocks; i++) {
        uint32_t pb = logical_to_phys(d, i);
        if (!pb) continue;
        if (!rd_block(pb, 1, blk)) return -1;
        KFSDirEnt* e = (KFSDirEnt*)blk;
        for (uint32_t j = 0; j < KFS_BLOCK_SIZE / sizeof(KFSDirEnt); j++) {
            if (e[j].inode) { cb(e[j].name, e[j].type == KFS_DIR, ctx); count++; }
        }
    }
    return count;
}
// end (satoru)
