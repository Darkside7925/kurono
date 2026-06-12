#include "persist.h"
#include "../drivers/nvme.h"
#include "../kernel/pmm.h"
#include "../kernel/heap.h"
#include "../drivers/serial.h"
#include "kfs.h"
#include "kvfs.h"

//  raw-sector persistence  -  see persist.h. all i/o goes through a single page
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

    //  mirror a kvfs subtree into the mounted kfs volume. (satoru)
    void save_subtree(const char* path) {
        if (KVFS::IsDir(path)) {
            KFS::Mkdirs(path);
            KVFSNode** kids = (KVFSNode**)KernelHeap::Alloc(512 * sizeof(KVFSNode*));
            if (!kids) return;
            int n = KVFS::Listdir(path, kids, 512);
            for (int i = 0; i < n; i++) {
                if (!kids[i]) continue;
                char child[256];
                path_join(child, sizeof(child), path, kids[i]->name);
                save_subtree(child);
            }
            KernelHeap::Free(kids);
        } else if (KVFS::IsFile(path)) {
            int sz = KVFS::GetFileSize(path);
            if (sz < 0 || (uint32_t)sz > KFS_MAX_FILE) return;  // skip oversized re-seeded media (satoru)
            if (sz == 0) { KFS::WriteFile(path, "", 0); return; }
            uint8_t* buf = (uint8_t*)KernelHeap::Alloc((uint32_t)sz);
            if (!buf) return;
            if (KVFS::ReadFile(path, buf, (uint32_t)sz) == sz)
                KFS::WriteFile(path, buf, (uint32_t)sz);
            KernelHeap::Free(buf);
        }
    }

    //  one dir's children collected off the kfs List callback, so the List scan's
    //  block buffer is released before we recurse (keeps the stack shallow). (satoru)
    struct Collected {
        static const int MAXN = 256;
        char names[MAXN][KFS_NAME_MAX + 1];
        bool isdir[MAXN];
        int  count;
    };
    void collect_cb(const char* name, bool is_dir, void* ctxv) {
        Collected* c = (Collected*)ctxv;
        if (c->count >= Collected::MAXN) return;
        int i = 0; for (; name[i] && i < KFS_NAME_MAX; i++) c->names[c->count][i] = name[i];
        c->names[c->count][i] = 0;
        c->isdir[c->count] = is_dir;
        c->count++;
    }

    void restore_subtree(const char* path) {
        Collected* c = (Collected*)KernelHeap::Alloc(sizeof(Collected));
        if (!c) return;
        c->count = 0;
        KFS::List(path, collect_cb, c);
        for (int i = 0; i < c->count; i++) {
            char child[256];
            path_join(child, sizeof(child), path, c->names[i]);
            if (c->isdir[i]) {
                KVFS::Mkdirs(child);
                restore_subtree(child);
            } else {
                int sz = KFS::FileSize(child);
                if (sz < 0) continue;
                if (sz == 0) { KVFS::WriteFile(child, "", 0); continue; }
                uint8_t* buf = (uint8_t*)KernelHeap::Alloc((uint32_t)sz);
                if (!buf) continue;
                if (KFS::ReadFile(child, buf, (uint32_t)sz) == sz)
                    KVFS::WriteFile(child, buf, (uint32_t)sz);
                KernelHeap::Free(buf);
            }
        }
        KernelHeap::Free(c);
    }

    uint32_t kfs_disk_blocks() {
        uint64_t blocks = NVMe::GetCapacityLBA() * NVMe::GetLBASize() / KFS_BLOCK_SIZE;
        if (blocks > 65536) blocks = 65536;   // cap at 256mb so the metadata caches stay ~1mb (satoru)
        if (blocks < 64) return 0;
        return (uint32_t)blocks;
    }
}

bool PersistStore::SaveTree() {
    if (!NVMe::IsDetected()) return false;
    KFS::SetBackend(kfs_rd, kfs_wr, nullptr);
    uint32_t blocks = kfs_disk_blocks();
    if (!blocks || !KFS::Format(blocks)) return false;
    const char* roots[3] = { "/home", "/etc", "/root" };
    for (int i = 0; i < 3; i++) if (KVFS::Exists(roots[i])) save_subtree(roots[i]);
    bool ok = KFS::Sync();
    if (ok) SerialLogger::Log("[persist] saved kvfs user data to KFS volume\r\n");
    return ok;
}

bool PersistStore::LoadTree() {
    if (!NVMe::IsDetected()) return false;
    KFS::SetBackend(kfs_rd, kfs_wr, nullptr);
    if (!KFS::Mount()) return false;
    const char* roots[3] = { "/home", "/etc", "/root" };
    for (int i = 0; i < 3; i++) if (KFS::Exists(roots[i])) restore_subtree(roots[i]);
    SerialLogger::Log("[persist] restored kvfs user data from KFS volume\r\n");
    return true;
}
// end (satoru)
