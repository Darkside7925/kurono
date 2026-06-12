#include "kfs.h"
#include "../kernel/heap.h"
#include "../kernel/pmm.h"
#include "../drivers/serial.h"

//  KFS implementation  -  see kfs.h. the persistence layer formats a fresh volume
//  each save and writes the user-data tree as real files + dirs, so a bump-style
//  allocator hands out CONTIGUOUS block runs and a whole file goes to disk in one
//  multi-page nvme command. the bitmap is still maintained on disk (it's part of
//  the format spec a fuse driver reads), allocation just never has to search it
//  on a fresh volume. metadata (superblock + bitmap + inode table) is cached in
//  ram and flushed in Sync(). (satoru)

namespace {
    KFSReadFn  g_rd  = nullptr;
    KFSWriteFn g_wr  = nullptr;
    void*      g_ctx = nullptr;

    KFSSuper   g_sb;
    bool       g_mounted = false;

    uint8_t*   g_bitmap = nullptr;   // in-ram free-block bitmap (satoru)
    uint8_t*   g_inodes = nullptr;   // in-ram inode table (satoru)
    uint32_t   g_next   = 0;         // bump hint for contiguous allocation (satoru)

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
    bool bit_get(uint32_t b) { return g_bitmap && ((g_bitmap[b >> 3] >> (b & 7)) & 1); }

    //  bump-allocate `n` contiguous data blocks; returns the first, or 0 if full.
    //  on a fresh volume the hint walks straight up, so runs are contiguous. (satoru)
    uint32_t alloc_run(uint32_t n) {
        if (n == 0) return 0;
        if (g_next < g_sb.data_start) g_next = g_sb.data_start;
        if (g_next + n > g_sb.total_blocks) return 0;
        uint32_t start = g_next;
        for (uint32_t i = 0; i < n; i++) bit_set(start + i);
        g_next += n;
        g_sb.free_blocks -= n;
        return start;
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

    //  path helpers  -  split into components, no allocation. (satoru)
    bool comp_eq(const char* a, const char* b, int blen) {
        for (int i = 0; i < blen; i++) if (a[i] != b[i]) return false;
        return a[blen] == 0;
    }

    //  look up `name` (length nlen) in directory inode `dir`; returns child inode
    //  or 0. (satoru)
    uint32_t dir_lookup(KFSInode* dir, const char* name, int nlen) {
        if (!dir || dir->type != KFS_DIR) return 0;
        uint8_t blk[KFS_BLOCK_SIZE];
        uint32_t nblocks = (dir->size + KFS_BLOCK_SIZE - 1) / KFS_BLOCK_SIZE;
        for (uint32_t i = 0; i < nblocks && i < KFS_DIRECT; i++) {
            if (!dir->direct[i]) continue;
            if (!rd_block(dir->direct[i], 1, blk)) return 0;
            KFSDirEnt* e = (KFSDirEnt*)blk;
            for (uint32_t j = 0; j < KFS_BLOCK_SIZE / sizeof(KFSDirEnt); j++) {
                if (e[j].inode && e[j].name_len == (uint16_t)nlen &&
                    comp_eq(e[j].name, name, nlen))
                    return e[j].inode;
            }
        }
        return 0;
    }

    //  add (name -> child) to directory `dir`, growing it a block at a time.
    //  returns false on i/o or capacity error. (satoru)
    bool dir_add(KFSInode* dir, const char* name, int nlen, uint32_t child, KFSType t) {
        if (!dir || nlen <= 0 || nlen > KFS_NAME_MAX) return false;
        uint8_t blk[KFS_BLOCK_SIZE];
        uint32_t per = KFS_BLOCK_SIZE / sizeof(KFSDirEnt);
        // try to reuse a free slot in an existing block. (satoru)
        uint32_t nblocks = (dir->size + KFS_BLOCK_SIZE - 1) / KFS_BLOCK_SIZE;
        for (uint32_t i = 0; i < nblocks && i < KFS_DIRECT; i++) {
            if (!dir->direct[i]) continue;
            if (!rd_block(dir->direct[i], 1, blk)) return false;
            KFSDirEnt* e = (KFSDirEnt*)blk;
            for (uint32_t j = 0; j < per; j++) {
                if (!e[j].inode) {
                    e[j].inode = child; e[j].name_len = (uint16_t)nlen; e[j].type = (uint16_t)t;
                    for (int k = 0; k < nlen; k++) e[j].name[k] = name[k];
                    e[j].name[nlen] = 0;
                    return wr_block(dir->direct[i], 1, blk);
                }
            }
        }
        // grow: allocate a fresh dir block. (satoru)
        if (nblocks >= KFS_DIRECT) return false;
        uint32_t nb = alloc_block();
        if (!nb) return false;
        for (uint32_t k = 0; k < KFS_BLOCK_SIZE; k++) blk[k] = 0;
        KFSDirEnt* e = (KFSDirEnt*)blk;
        e[0].inode = child; e[0].name_len = (uint16_t)nlen; e[0].type = (uint16_t)t;
        for (int k = 0; k < nlen; k++) e[0].name[k] = name[k];
        e[0].name[nlen] = 0;
        if (!wr_block(nb, 1, blk)) return false;
        dir->direct[nblocks] = nb;
        dir->size = (nblocks + 1) * KFS_BLOCK_SIZE;
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
                }
                cur = child;
            }
            while (*p == '/') p++;
        }
        return cur;
    }

    //  split "/a/b/c" -> parent inode of "c" + the leaf name. (satoru)
    bool split_parent(const char* path, uint32_t* parent, const char** leaf, int* leaf_len, bool make) {
        if (!path || path[0] != '/') return false;
        const char* last = path + 1;
        const char* p = path + 1;
        const char* lstart = path + 1;
        // find the final component. (satoru)
        while (*p) {
            if (*p == '/') { if (p[1]) lstart = p + 1; }
            p++;
        }
        last = lstart;
        int llen = 0; while (last[llen] && last[llen] != '/') llen++;
        if (llen == 0) return false;
        // parent path is everything before the leaf. (satoru)
        char parent_path[256];
        int plen = (int)(last - path);
        if (plen <= 1) { *parent = KFS_ROOT_INODE; }
        else {
            if (plen >= (int)sizeof(parent_path)) return false;
            for (int i = 0; i < plen; i++) parent_path[i] = path[i];
            parent_path[plen] = 0;
            *parent = make ? mkdirs_ino(parent_path) : resolve(parent_path);
            if (!*parent) return false;
        }
        *leaf = last; *leaf_len = llen;
        return true;
    }

    //  read file inode `n` into buf (up to max). returns bytes or -1. files in
    //  the snapshot model occupy one contiguous run starting at direct[0], so we
    //  read run-by-run through the page-aligned bounce (one multi-page command per
    //  run)  -  fast + no large stack buffers. (satoru)
    int read_file(KFSInode* n, void* buf, uint32_t max) {
        if (!n || n->type != KFS_FILE) return -1;
        uint32_t len = n->size; if (len > max) len = max;
        if (len == 0) return 0;
        uint32_t start = n->direct[0];
        if (!start) return -1;
        uint32_t nblocks = (n->size + KFS_BLOCK_SIZE - 1) / KFS_BLOCK_SIZE;
        uint8_t* out = (uint8_t*)buf;
        uint32_t done = 0, blk_off = 0;
        while (done < len && blk_off < nblocks) {
            uint32_t run = nblocks - blk_off; if (run > KFS_MAX_RUN_BLOCKS) run = KFS_MAX_RUN_BLOCKS;
            uint32_t run_bytes = run * KFS_BLOCK_SIZE;
            if (!ensure_bounce(run_bytes)) return -1;
            if (!rd_block(start + blk_off, run, g_bounce)) return -1;
            uint32_t copy = len - done; if (copy > run_bytes) copy = run_bytes;
            for (uint32_t i = 0; i < copy; i++) out[done + i] = g_bounce[i];
            done += copy; blk_off += run;
        }
        return (int)done;
    }
}

void KFS::SetBackend(KFSReadFn rd, KFSWriteFn wr, void* ctx) { g_rd = rd; g_wr = wr; g_ctx = ctx; }
bool KFS::IsMounted() { return g_mounted; }

bool KFS::Format(uint32_t total_blocks) {
    if (!g_wr || total_blocks < 64) return false;

    KFSSuper sb;
    for (uint32_t i = 0; i < sizeof(sb); i++) ((uint8_t*)&sb)[i] = 0;
    sb.magic = KFS_MAGIC; sb.version = KFS_VERSION; sb.block_size = KFS_BLOCK_SIZE;
    sb.total_blocks  = total_blocks;
    sb.bitmap_start  = 1;
    sb.bitmap_blocks = (total_blocks + (KFS_BLOCK_SIZE * 8 - 1)) / (KFS_BLOCK_SIZE * 8);
    sb.inode_count   = total_blocks / 32; if (sb.inode_count < 256) sb.inode_count = 256;  // ~2k inodes / 256kb table for a 256mb disk (satoru)
    sb.inode_start   = sb.bitmap_start + sb.bitmap_blocks;
    sb.inode_blocks  = (sb.inode_count + KFS_INODES_PER_BLOCK - 1) / KFS_INODES_PER_BLOCK;
    sb.data_start    = sb.inode_start + sb.inode_blocks;
    if (sb.data_start >= total_blocks) return false;
    sb.free_blocks   = total_blocks - sb.data_start;

    // (re)allocate the in-ram metadata caches. (satoru)
    if (g_bitmap) KernelHeap::Free(g_bitmap);
    if (g_inodes) KernelHeap::Free(g_inodes);
    g_bitmap = (uint8_t*)KernelHeap::Alloc(sb.bitmap_blocks * KFS_BLOCK_SIZE);
    g_inodes = (uint8_t*)KernelHeap::Alloc(sb.inode_blocks  * KFS_BLOCK_SIZE);
    if (!g_bitmap || !g_inodes) return false;
    for (uint32_t i = 0; i < sb.bitmap_blocks * KFS_BLOCK_SIZE; i++) g_bitmap[i] = 0;
    for (uint32_t i = 0; i < sb.inode_blocks  * KFS_BLOCK_SIZE; i++) g_inodes[i] = 0;

    g_sb = sb;
    g_next = sb.data_start;
    // mark all metadata blocks used. (satoru)
    for (uint32_t b = 0; b < sb.data_start; b++) bit_set(b);
    // root directory. (satoru)
    KFSInode* root = inode_ptr(KFS_ROOT_INODE);
    root->type = KFS_DIR; root->mode = 0755; root->nlink = 2; root->size = 0;

    g_mounted = true;
    return Sync();
}

bool KFS::Mount() {
    if (!g_rd) return false;
    uint8_t blk[KFS_BLOCK_SIZE];
    if (!rd_block(0, 1, blk)) return false;
    KFSSuper* sb = (KFSSuper*)blk;
    if (sb->magic != KFS_MAGIC || sb->version != KFS_VERSION || sb->block_size != KFS_BLOCK_SIZE)
        return false;
    if (crc32((uint8_t*)sb, __builtin_offsetof(KFSSuper, crc)) != sb->crc) return false;
    g_sb = *sb;

    if (g_bitmap) KernelHeap::Free(g_bitmap);
    if (g_inodes) KernelHeap::Free(g_inodes);
    g_bitmap = (uint8_t*)KernelHeap::Alloc(g_sb.bitmap_blocks * KFS_BLOCK_SIZE);
    g_inodes = (uint8_t*)KernelHeap::Alloc(g_sb.inode_blocks  * KFS_BLOCK_SIZE);
    if (!g_bitmap || !g_inodes) return false;
    if (!rd_block(g_sb.bitmap_start, g_sb.bitmap_blocks, g_bitmap)) return false;
    if (!rd_block(g_sb.inode_start,  g_sb.inode_blocks,  g_inodes)) return false;
    g_next = g_sb.data_start;
    g_mounted = true;
    return true;
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

bool KFS::WriteFile(const char* path, const void* data, uint32_t len) {
    if (!g_mounted || !path) return false;
    if (len > KFS_MAX_FILE) return false;

    uint32_t parent; const char* leaf; int llen;
    if (!split_parent(path, &parent, &leaf, &llen, true)) return false;
    KFSInode* pdir = inode_ptr(parent);
    if (!pdir) return false;

    // fresh file inode (snapshot model: paths are unique per format). (satoru)
    uint32_t fino = new_inode(KFS_FILE, 0644);
    if (!fino) return false;
    if (!dir_add(pdir, leaf, llen, fino, KFS_FILE)) return false;
    KFSInode* f = inode_ptr(fino);
    f->size = len;

    if (len == 0) return true;

    // one contiguous run for the whole file -> a few multi-page commands. (satoru)
    uint32_t nblocks = (len + KFS_BLOCK_SIZE - 1) / KFS_BLOCK_SIZE;
    uint32_t start = alloc_run(nblocks);
    if (!start) return false;

    // record the block pointers (contiguous: start..start+nblocks-1). (satoru)
    uint32_t indirect[KFS_PTRS_PER_BLOCK];
    bool need_ind = nblocks > KFS_DIRECT;
    if (need_ind) for (uint32_t i = 0; i < KFS_PTRS_PER_BLOCK; i++) indirect[i] = 0;
    for (uint32_t i = 0; i < nblocks; i++) {
        if (i < KFS_DIRECT) f->direct[i] = start + i;
        else                indirect[i - KFS_DIRECT] = start + i;
    }
    if (need_ind) {
        uint32_t ib = alloc_block();
        if (!ib) return false;
        f->indirect = ib;
        if (!wr_block(ib, 1, indirect)) return false;
    }

    // copy the data into a big page-aligned bounce and write it in MAXCHUNK runs;
    // the run is contiguous, so each write is a single multi-page command. (satoru)
    const uint8_t* src = (const uint8_t*)data;
    uint32_t per = KFS_MAX_RUN_BLOCKS;
    uint32_t off = 0;
    uint32_t blk_off = 0;
    while (off < len) {
        uint32_t run_blocks = nblocks - blk_off; if (run_blocks > per) run_blocks = per;
        uint32_t run_bytes  = run_blocks * KFS_BLOCK_SIZE;
        if (!ensure_bounce(run_bytes)) return false;
        uint32_t copy = len - off; if (copy > run_bytes) copy = run_bytes;
        for (uint32_t i = 0; i < copy; i++) g_bounce[i] = src[off + i];
        for (uint32_t i = copy; i < run_bytes; i++) g_bounce[i] = 0;   // zero-pad tail (satoru)
        if (!wr_block(start + blk_off, run_blocks, g_bounce)) return false;
        off     += copy;
        blk_off += run_blocks;
    }
    return true;
}

int KFS::ReadFile(const char* path, void* buf, uint32_t max) {
    if (!g_mounted) return -1;
    KFSInode* n = inode_ptr(resolve(path));
    return read_file(n, buf, max);
}

bool KFS::Exists(const char* path)  { return g_mounted && resolve(path) != 0; }
bool KFS::IsDir(const char* path)   { KFSInode* n = g_mounted ? inode_ptr(resolve(path)) : nullptr; return n && n->type == KFS_DIR; }
int  KFS::FileSize(const char* path){ KFSInode* n = g_mounted ? inode_ptr(resolve(path)) : nullptr; return (n && n->type == KFS_FILE) ? (int)n->size : -1; }

int KFS::List(const char* path, ListCb cb, void* ctx) {
    if (!g_mounted || !cb) return -1;
    KFSInode* d = inode_ptr(resolve(path));
    if (!d || d->type != KFS_DIR) return -1;
    uint8_t blk[KFS_BLOCK_SIZE];
    uint32_t nblocks = (d->size + KFS_BLOCK_SIZE - 1) / KFS_BLOCK_SIZE;
    int count = 0;
    for (uint32_t i = 0; i < nblocks && i < KFS_DIRECT; i++) {
        if (!d->direct[i]) continue;
        if (!rd_block(d->direct[i], 1, blk)) return -1;
        KFSDirEnt* e = (KFSDirEnt*)blk;
        for (uint32_t j = 0; j < KFS_BLOCK_SIZE / sizeof(KFSDirEnt); j++) {
            if (e[j].inode) { cb(e[j].name, e[j].type == KFS_DIR, ctx); count++; }
        }
    }
    return count;
}
// end (satoru)
