#include "persist.h"
#include "../drivers/nvme.h"
#include "../drivers/timer.h"
#include "../kernel/pmm.h"
#include "../kernel/heap.h"
#include "../drivers/serial.h"
#include "kfs.h"
#include "kvfs.h"

//  raw-sector persistence - see persist.h. all i/o goes through a single page
//  aligned bounce buffer (PMM::AllocBytes hands out page-aligned, identity-mapped
//  frames) because nvme read/write here use prp1 only, so one command moves at
//  most one 4096-byte page. (satoru)

namespace {
    const uint32_t KPS_MAGIC    = 0x4B505331u;  // "KPS1" (satoru)
    const uint32_t KPS_VERSION  = 1u;
    const uint64_t KPS_HDR_LBA  = 0;            // header lives in the first sector (satoru)
    const uint64_t KPS_DATA_OFF = 4096;         // blob starts at byte offset 4096 (satoru)

    struct KpsHeader {
        uint32_t magic;
        uint32_t version;
        uint32_t length;    // blob byte length (satoru)
        uint32_t crc;       // crc32 of the blob (satoru)
    };

    // standard crc32 (poly 0xedb88320), table-free to keep the footprint tiny. (satoru)
    uint32_t crc32(const uint8_t* p, uint32_t n) {
        uint32_t c = 0xFFFFFFFFu;
        for (uint32_t i = 0; i < n; i++) {
            c ^= p[i];
            for (int k = 0; k < 8; k++)
                c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
        }
        return ~c;
    }

    // lbas spanned by one 4096-byte page on this controller. (satoru)
    uint32_t lbas_per_page() {
        uint32_t sz = NVMe::GetLBASize();
        if (!sz) sz = 512;
        uint32_t n = 4096u / sz;
        return n ? n : 1;
    }
}

bool PersistStore::Available() {
    return NVMe::IsDetected();
}

bool PersistStore::Save(const uint8_t* blob, uint32_t len) {
    if (!NVMe::IsDetected() || !blob || len == 0) return false;

    const uint32_t per      = lbas_per_page();
    const uint64_t data_lba  = KPS_DATA_OFF / NVMe::GetLBASize();  // 4096/512 = 8 (satoru)
    const uint32_t MAXCHUNK  = NVMe::MaxTransferBytes();           // ~2mb per command via the prp list (satoru)

    // one big page-aligned, physically-contiguous bounce so a whole multi-mb blob
    // goes out in a handful of commands instead of hundreds of 4kb ones. (satoru)
    uint8_t* bounce = (uint8_t*)PMM::AllocBytes(MAXCHUNK);
    if (!bounce) return false;

    bool ok = true;
    uint32_t off = 0;
    uint64_t lba = data_lba;
    while (off < len) {
        uint32_t chunk = len - off;
        if (chunk > MAXCHUNK) chunk = MAXCHUNK;
        uint32_t pages  = (chunk + 4095) / 4096;       // round up to whole pages (satoru)
        memset(bounce, 0, pages * 4096);               // zero-pad the trailing partial page (satoru)
        memcpy(bounce, blob + off, chunk);
        if (!NVMe::Write(lba, pages * per, bounce)) { ok = false; break; }
        off += chunk;
        lba += (uint64_t)pages * per;
    }

    if (ok) {
        // commit the header LAST: a torn blob write then fails the crc check on
        // load (falls back to the default tree) instead of advertising garbage. (satoru)
        KpsHeader hdr;
        hdr.magic   = KPS_MAGIC;
        hdr.version = KPS_VERSION;
        hdr.length  = len;
        hdr.crc     = crc32(blob, len);
        memset(bounce, 0, 4096);
        memcpy(bounce, &hdr, sizeof(hdr));
        ok = NVMe::Write(KPS_HDR_LBA, per, bounce);
        NVMe::Flush();
    }

    PMM::FreeBytes(bounce, MAXCHUNK);
    if (ok) {
        SerialLogger::Log("[persist] saved "); SerialLogger::LogDec((int)len);
        SerialLogger::Log(" bytes to raw nvme store\r\n");
    }
    return ok;
}

bool PersistStore::Load(uint8_t* buf, uint32_t maxlen, uint32_t* out_len) {
    if (out_len) *out_len = 0;
    if (!NVMe::IsDetected() || !buf) return false;

    const uint32_t per      = lbas_per_page();
    const uint64_t data_lba  = KPS_DATA_OFF / NVMe::GetLBASize();
    const uint32_t MAXCHUNK  = NVMe::MaxTransferBytes();

    uint8_t* bounce = (uint8_t*)PMM::AllocBytes(MAXCHUNK);
    if (!bounce) return false;

    // read + validate the header. a fresh / foreign disk fails the magic check. (satoru)
    if (!NVMe::Read(KPS_HDR_LBA, per, bounce)) { PMM::FreeBytes(bounce, MAXCHUNK); return false; }
    KpsHeader hdr;
    memcpy(&hdr, bounce, sizeof(hdr));
    if (hdr.magic != KPS_MAGIC || hdr.length == 0 || hdr.length > maxlen) {
        PMM::FreeBytes(bounce, MAXCHUNK);
        return false;
    }

    // read the blob back in big (multi-page) chunks. (satoru)
    bool ok = true;
    uint32_t off = 0;
    uint64_t lba = data_lba;
    while (off < hdr.length) {
        uint32_t chunk = hdr.length - off;
        if (chunk > MAXCHUNK) chunk = MAXCHUNK;
        uint32_t pages = (chunk + 4095) / 4096;
        if (!NVMe::Read(lba, pages * per, bounce)) { ok = false; break; }
        memcpy(buf + off, bounce, chunk);
        off += chunk;
        lba += (uint64_t)pages * per;
    }
    PMM::FreeBytes(bounce, MAXCHUNK);
    if (!ok) return false;

    // reject a torn / corrupt blob rather than feeding garbage to deserialize. (satoru)
    if (crc32(buf, hdr.length) != hdr.crc) return false;

    if (out_len) *out_len = hdr.length;
    SerialLogger::Log("[persist] loaded "); SerialLogger::LogDec((int)hdr.length);
    SerialLogger::Log(" bytes from raw nvme store\r\n");
    return true;
}

//  ── KFS-backed persistence (the real-filesystem replacement for the raw blob) ──
//  the on-disk store is now a real Kurono filesystem: the kvfs user-data subtrees
//  are mirrored to it as ACTUAL files + dirs (browsable, fuse-mountable later)
//  instead of one opaque blob. (satoru)
namespace {
    bool kfs_rd(uint64_t block, uint32_t count, void* buf, void*) {
        uint32_t per = 4096u / NVMe::GetLBASize();
        return NVMe::Read(block * per, count * per, buf);
    }
    bool kfs_wr(uint64_t block, uint32_t count, const void* buf, void*) {
        uint32_t per = 4096u / NVMe::GetLBASize();
        return NVMe::Write(block * per, count * per, buf);
    }

    void path_join(char* out, int cap, const char* dir, const char* name) {
        int p = 0;
        for (int i = 0; dir[i] && p < cap - 1; i++) out[p++] = dir[i];
        if (p == 0 || out[p - 1] != '/') { if (p < cap - 1) out[p++] = '/'; }
        for (int i = 0; name[i] && p < cap - 1; i++) out[p++] = name[i];
        out[p] = 0;
    }

    //  mirror a kvfs subtree into the mounted kfs volume. the child list is sized
    //  to the directory's actual child_count (heap, not a fixed 512 cap) so a
    //  directory of ANY size is fully mirrored - no max-dir-entry limit. (satoru)
    void save_subtree(const char* path) {
        KVFSNode* node = KVFS::Resolve(path);
        if (node && node->type == KVFS_SYMLINK) {
            // store the symlink target so the compat overlay survives reboots even
            // if InstallCanonicalLayout were ever skipped (defence in depth). (satoru)
            KFS::Symlink(path, node->link_target);
            return;
        }
        if (KVFS::IsDir(path)) {
            KFS::Mkdirs(path);
            int cap = node ? node->child_count : 0;
            if (cap <= 0) return;
            KVFSNode** kids = (KVFSNode**)KernelHeap::Alloc((uint32_t)cap * sizeof(KVFSNode*));
            if (!kids) return;
            int n = KVFS::Listdir(path, kids, cap);
            for (int i = 0; i < n; i++) {
                if (!kids[i]) continue;
                // a path component can be up to 4 KB (linux PATH_MAX); heap-alloc
                // the join buffer so deep trees don't blow the recursion stack. (satoru)
                char* child = (char*)KernelHeap::Alloc(4096);
                if (!child) continue;
                path_join(child, 4096, path, kids[i]->name);
                save_subtree(child);
                KernelHeap::Free(child);
            }
            KernelHeap::Free(kids);
        } else if (KVFS::IsFile(path)) {
            int sz = KVFS::GetFileSize(path);
            // KFS v2 extents have NO ~4 MB file cap (files are limited only by
            // free disk space), so the old KFS_MAX_FILE skip is gone - mirror
            // whatever KVFS holds. (satoru)
            if (sz < 0) return;
            if (sz == 0) { KFS::WriteFile(path, "", 0); return; }
            uint8_t* buf = (uint8_t*)KernelHeap::Alloc((uint32_t)sz);
            if (!buf) return;
            if (KVFS::ReadFile(path, buf, (uint32_t)sz) == sz)
                KFS::WriteFile(path, buf, (uint64_t)(uint32_t)sz);
            KernelHeap::Free(buf);
        }
    }

    //  layer 6 - content fingerprint of a kvfs subtree. accumulates a 64-bit hash
    //  over (path, type, size, content) for every node so two different trees
    //  almost never collide. order matters (depth-first, dir children in listdir
    //  order), which is deterministic for the same tree. used to detect "nothing
    //  changed since the last save" and skip the whole reformat+rewrite. (satoru)
    void fp_mix(uint64_t* h, const uint8_t* p, uint32_t n) {
        uint64_t x = *h;
        for (uint32_t i = 0; i < n; i++) { x ^= p[i]; x *= 1099511628211ull; }   // fnv-1a-ish (satoru)
        *h = x;
    }
    void fp_str(uint64_t* h, const char* s) { uint32_t n = 0; while (s[n]) n++; fp_mix(h, (const uint8_t*)s, n); }

    void fingerprint_subtree(const char* path, uint64_t* h) {
        KVFSNode* node = KVFS::Resolve(path);
        if (!node) return;
        fp_str(h, path);
        uint8_t t = (uint8_t)node->type; fp_mix(h, &t, 1);
        if (node->type == KVFS_SYMLINK) { fp_str(h, node->link_target); return; }
        if (KVFS::IsDir(path)) {
            int cap = node->child_count;
            if (cap <= 0) return;
            KVFSNode** kids = (KVFSNode**)KernelHeap::Alloc((uint32_t)cap * sizeof(KVFSNode*));
            if (!kids) return;
            int n = KVFS::Listdir(path, kids, cap);
            for (int i = 0; i < n; i++) {
                if (!kids[i]) continue;
                char* child = (char*)KernelHeap::Alloc(4096);
                if (!child) continue;
                path_join(child, 4096, path, kids[i]->name);
                fingerprint_subtree(child, h);
                KernelHeap::Free(child);
            }
            KernelHeap::Free(kids);
        } else if (KVFS::IsFile(path)) {
            int sz = KVFS::GetFileSize(path);
            if (sz < 0) return;
            uint32_t usz = (uint32_t)sz;
            fp_mix(h, (const uint8_t*)&usz, sizeof(usz));
            if (sz == 0) return;
            uint8_t* buf = (uint8_t*)KernelHeap::Alloc(usz);
            if (!buf) return;
            if (KVFS::ReadFile(path, buf, usz) == sz) fp_mix(h, buf, usz);
            KernelHeap::Free(buf);
        }
    }

    //  fingerprint the whole persisted user-data set (the same roots SaveTree
    //  mirrors). a non-zero base so an empty tree isn't 0 (which means "unset"). (satoru)
    uint64_t fingerprint_userdata() {
        uint64_t h = 1469598103934665603ull;   // fnv offset basis (satoru)
        const char* roots[3] = { "/home", "/etc", "/root" };
        for (int i = 0; i < 3; i++) if (KVFS::Exists(roots[i])) fingerprint_subtree(roots[i], &h);
        if (h == 0) h = 1;   // 0 is the "unset" sentinel (satoru)
        return h;
    }

    //  one dir's children collected off the kfs List callback, so the List scan's
    //  block buffer is released before we recurse (keeps the stack shallow). the
    //  collection grows on the heap with NO fixed cap, so a directory of any size
    //  restores in full. (satoru)
    struct Collected {
        char (*names)[KFS_NAME_MAX + 1];
        bool* isdir;
        int   count;
        int   cap;
    };
    void collect_cb(const char* name, bool is_dir, void* ctxv) {
        Collected* c = (Collected*)ctxv;
        if (c->count >= c->cap) {
            int ncap = c->cap ? c->cap * 2 : 64;
            char (*nn)[KFS_NAME_MAX + 1] = (char(*)[KFS_NAME_MAX + 1])KernelHeap::Alloc((uint32_t)ncap * (KFS_NAME_MAX + 1));
            bool* nd = (bool*)KernelHeap::Alloc((uint32_t)ncap * sizeof(bool));
            if (!nn || !nd) { if (nn) KernelHeap::Free(nn); if (nd) KernelHeap::Free(nd); return; }
            for (int i = 0; i < c->count; i++) {
                for (int k = 0; k <= KFS_NAME_MAX; k++) nn[i][k] = c->names[i][k];
                nd[i] = c->isdir[i];
            }
            if (c->names) KernelHeap::Free(c->names);
            if (c->isdir) KernelHeap::Free(c->isdir);
            c->names = nn; c->isdir = nd; c->cap = ncap;
        }
        int i = 0; for (; name[i] && i < KFS_NAME_MAX; i++) c->names[c->count][i] = name[i];
        c->names[c->count][i] = 0;
        c->isdir[c->count] = is_dir;
        c->count++;
    }

    void restore_subtree(const char* path) {
        Collected c; c.names = nullptr; c.isdir = nullptr; c.count = 0; c.cap = 0;
        KFS::List(path, collect_cb, &c);
        for (int i = 0; i < c.count; i++) {
            char* child = (char*)KernelHeap::Alloc(4096);
            if (!child) continue;
            path_join(child, 4096, path, c.names[i]);
            if (c.isdir[i]) {
                KVFS::Mkdirs(child);
                restore_subtree(child);
            } else if (KFS::IsSymlink(child)) {
                char tgt[1024]; tgt[0] = 0;
                if (KFS::ReadLink(child, tgt, (int)sizeof(tgt)) > 0)
                    KVFS::Symlink(child, tgt);
            } else {
                int64_t sz = KFS::FileSize(child);
                if (sz < 0) { KernelHeap::Free(child); continue; }
                if (sz == 0) { KVFS::WriteFile(child, "", 0); KernelHeap::Free(child); continue; }
                uint8_t* buf = (uint8_t*)KernelHeap::Alloc((uint32_t)sz);
                if (!buf) {   // a silent skip here leaves a restored install incomplete (satoru)
                    SerialLogger::Log("[persist] RESTORE alloc FAIL "); SerialLogger::Log(child);
                    SerialLogger::Log(" sz="); SerialLogger::LogDec((int)sz); SerialLogger::Log("\r\n");
                    KernelHeap::Free(child); continue;
                }
                int64_t got = KFS::ReadFile(child, buf, (uint64_t)sz);
                if (got == sz) {
                    KVFS::WriteFile(child, buf, (uint32_t)sz);
                } else {   // a truncated restore is silent corruption -- surface it (satoru)
                    SerialLogger::Log("[persist] RESTORE short read "); SerialLogger::Log(child);
                    SerialLogger::Log(" sz="); SerialLogger::LogDec((int)sz);
                    SerialLogger::Log(" got="); SerialLogger::LogDec((int)got); SerialLogger::Log("\r\n");
                }
                KernelHeap::Free(buf);
            }
            KernelHeap::Free(child);
        }
        if (c.names) KernelHeap::Free(c.names);
        if (c.isdir) KernelHeap::Free(c.isdir);
    }

    //  size the KFS volume to the FULL nvme capacity (no 256 MB cap). the in-ram
    //  metadata caches scale with the volume - bitmap = total_blocks/8 bytes and
    //  the inode table = total_blocks*8 bytes (1 inode per 32 blocks) - so a 4 GB
    //  disk costs ~36 MB of cache, a 16 GB disk ~144 MB. cap the cache budget at
    //  512 MB (=> ~57 GB volume) so a pathologically huge disk can't exhaust ram;
    //  beyond that we still address the whole disk for data, we just cap the
    //  metadata region. (satoru)
    uint32_t kfs_disk_blocks() {
        uint64_t blocks = NVMe::GetCapacityLBA() * (uint64_t)NVMe::GetLBASize() / KFS_BLOCK_SIZE;
        // budget ~9 bytes of metadata cache per block (bitmap 1/8 B + inode 8 B);
        // 512 MB / 9 ≈ 59.6 M blocks ≈ 228 GB addressable before we clamp. (satoru)
        const uint64_t MAX_BLOCKS = 59000000ull;
        if (blocks > MAX_BLOCKS) blocks = MAX_BLOCKS;
        if (blocks > 0xFFFFFFFEull) blocks = 0xFFFFFFFEull;  // block numbers are u32 (satoru)
        if (blocks < 64) return 0;
        return (uint32_t)blocks;
    }
}

bool PersistStore::SaveTree() {
    if (!NVMe::IsDetected()) return false;
    KFS::SetBackend(kfs_rd, kfs_wr, nullptr);

    // layer 6 - INCREMENTAL save. fingerprint the current user-data set; if a
    // valid volume is already on disk with the SAME fingerprint, nothing changed
    // since the last save, so skip the whole reformat+rewrite. this makes a save
    // with no changes ≈ a single mount (a few ms) instead of a full 30-40 ms
    // format + rewrite. on a real change (or a fresh/foreign disk) we fall through
    // to the full snapshot. (satoru)
    uint64_t cur_fp = fingerprint_userdata();
    if (KFS::Mount()) {
        uint64_t prev_fp = KFS::MountedFingerprint();
        if (prev_fp != 0 && prev_fp == cur_fp) {
            SerialLogger::Log("[persist] incremental save: user data unchanged, skipped full rewrite\r\n");
            return true;
        }
    }

    uint32_t blocks = kfs_disk_blocks();
    if (!blocks || !KFS::Format(blocks)) return false;
    // /apps persists too so a large installed package (e.g. firefox in
    // /apps/firefox) survives reboot and isn't re-downloaded every boot. (satoru)
    const char* roots[4] = { "/home", "/etc", "/root", "/apps" };
    for (int i = 0; i < 4; i++) if (KVFS::Exists(roots[i])) save_subtree(roots[i]);
    KFS::SetFingerprint(cur_fp);   // stamp so the next save can short-circuit (satoru)
    bool ok = KFS::Sync();
    if (ok) SerialLogger::Log("[persist] saved kvfs user data to KFS volume (full snapshot)\r\n");
    return ok;
}

bool PersistStore::LoadTree() {
    if (!NVMe::IsDetected()) return false;
    KFS::SetBackend(kfs_rd, kfs_wr, nullptr);
    if (!KFS::Mount()) return false;
    // restore the small user-data subtrees at boot. /apps is DELIBERATELY skipped
    // here: a large installed package (firefox is ~288 mb) bulk-read into the heap
    // this early (before the desktop is up) recreated the heap+timing state that
    // stalled firefox's threads before its wayland connect. /apps is restored
    // lazily by RestoreApps() at launch time instead, matching the install path
    // where /apps only fills once firefox is actually run. (satoru)
    const char* roots[3] = { "/home", "/etc", "/root" };
    for (int i = 0; i < 3; i++) if (KFS::Exists(roots[i])) restore_subtree(roots[i]);
    SerialLogger::Log("[persist] restored kvfs user data from KFS volume (apps deferred)\r\n");
    return true;
}

// mount the KFS volume only if it is not already mounted. each KFS::Mount()
// FREES and re-allocates the ~36 mb in-ram bitmap+inode caches; calling it
// repeatedly at launch time (HasPersistedFirefox then RestoreApps) churned those
// large blocks on the live, fragmented desktop heap. the boot-time LoadTree
// already left the volume mounted, so reuse that state. (satoru)
static bool kfs_ensure_mounted() {
    KFS::SetBackend(kfs_rd, kfs_wr, nullptr);
    if (KFS::IsMounted()) return true;
    return KFS::Mount();
}

bool PersistStore::RestoreApps() {
    if (!NVMe::IsDetected()) return false;
    if (!kfs_ensure_mounted()) return false;
    if (!KFS::Exists("/apps")) return false;
    // already restored this boot? a populated /apps/firefox means RestoreApps ran
    // (or the install path created it), so don't re-read 288 mb. (satoru)
    if (KVFS::IsDir("/apps") && KVFS::Exists("/apps/firefox/firefox")) return true;
    uint64_t t0 = Timer::GetRealMs64();
    restore_subtree("/apps");
    uint64_t t1 = Timer::GetRealMs64();
    SerialLogger::Log("[persist] restored /apps from KFS volume in ");
    SerialLogger::LogDec((int)(t1 - t0)); SerialLogger::Log(" ms\r\n");
    return true;
}

bool PersistStore::HasPersistedFirefox() {
    if (!NVMe::IsDetected()) return false;
    if (!kfs_ensure_mounted()) return false;
    // a non-empty firefox binary in the on-disk volume == a reusable install. (satoru)
    return KFS::Exists("/apps/firefox/firefox") &&
           KFS::FileSize("/apps/firefox/firefox") > 0;
}
// end (satoru)
