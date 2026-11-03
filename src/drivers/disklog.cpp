#include "disklog.h"
#include "nvme.h"
#include "serial.h"
#include "../kernel/pmm.h"
#include "../proc/scheduler.h"
#include "hwinfo.h"

// see disklog.h. everything here reads/writes through one pmm bounce buffer,
// page aligned and identity mapped, like persist.cpp. all offsets inside the
// partition are kept in BYTES and only turned into device lbas at the edge,
// so a 4k-lba ssd and a 512-byte fat sector size mix correctly. (satoru)

namespace {
    const uint32_t BOUNCE = 256u * 1024u;

    uint8_t*  g_buf      = nullptr;
    uint32_t  g_lbasz    = 512;
    uint64_t  g_part_lo  = 0;      // partition first lba (satoru)
    uint64_t  g_part_end = 0;      // partition end lba, exclusive (satoru)
    bool      g_window   = false;
    bool      g_narrowed = false;  // Init has cut the write window down; never widen it again (satoru)

    bool      g_ready    = false;
    uint64_t  g_file_lba = 0;      // device lba of KURONO.LOG's first byte (satoru)
    uint32_t  g_file_len = 0;      // bytes, whole lbas only (satoru)
    uint32_t  g_done     = 0;      // capture bytes already on disk in full lbas (satoru)
    volatile uint32_t g_busy = 0;  // a flush is in flight (worker vs panic) (satoru)
    // why the log is or is not running, shown on screen at boot. (satoru)
    const char* g_status = "not started";
    char g_status_buf[64];

    uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
    uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
    uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

    bool ensure_buf() {
        if (!g_buf) g_buf = (uint8_t*)PMM::AllocBytes(BOUNCE);
        return g_buf != nullptr;
    }

    // read len bytes at byte offset off from the start of the DISK into out
    // (out may alias g_buf only when it is g_buf itself). (satoru)
    bool disk_read(uint64_t off, uint32_t len, uint8_t* out) {
        uint64_t lba  = off / g_lbasz;
        uint32_t skip = (uint32_t)(off % g_lbasz);
        uint32_t n    = (skip + len + g_lbasz - 1) / g_lbasz;
        if ((uint64_t)n * g_lbasz > BOUNCE) return false;
        if (!NVMe::Read(lba, n, g_buf)) return false;
        if (out != g_buf || skip) {
            for (uint32_t i = 0; i < len; i++) out[i] = g_buf[skip + i];
        }
        return true;
    }

    // fat32 geometry of the KURONO partition, in bytes from the disk start. (satoru)
    uint64_t f_fat_off  = 0;
    uint64_t f_data_off = 0;
    uint32_t f_clus     = 0;       // bytes per cluster (satoru)
    uint32_t f_root     = 0;

    // one 4k window of the fat is cached so walking a multi-gb file's chain
    // costs one read per 1024 entries instead of one per entry. (satoru)
    uint8_t  g_fat_win[4096];
    uint64_t g_fat_win_off = ~0ull;

    uint32_t fat_next(uint32_t c) {
        uint64_t eoff = f_fat_off + (uint64_t)c * 4u;
        uint64_t woff = eoff & ~4095ull;
        if (woff != g_fat_win_off) {
            if (!disk_read(woff, 4096, g_fat_win)) return 0x0FFFFFFFu;
            g_fat_win_off = woff;
        }
        return rd32(g_fat_win + (eoff - woff)) & 0x0FFFFFFFu;
    }
    uint64_t clus_off(uint32_t c) { return f_data_off + (uint64_t)(c - 2) * f_clus; }

    // the kfs backing file, found by Init (device lba, bytes, whole lbas). (satoru)
    uint64_t  g_kfs_lba    = 0;
    uint64_t  g_kfs_len    = 0;
    const char* g_kfs_status = "KURONO partition not opened";

    // look up a named 8.3 file in the KURONO root, require it to be one
    // contiguous cluster run and return its device lba + whole-cluster length.
    // 0 ok, 1 missing/empty, 2 fragmented, 3 misaligned or outside the partition.
    // reads only. needs the f_* geometry set up by Init. (satoru)
    int find_root_file(const char* name83, uint64_t* out_lba, uint64_t* out_len) {
        uint32_t first = 0, size = 0;
        uint32_t c = f_root;
        for (int guard = 0; guard < 4096 && c >= 2 && c < 0x0FFFFFF8u && !first; guard++) {
            uint8_t* d = g_buf;
            if (!disk_read(clus_off(c), f_clus, d)) return 1;
            bool end = false;
            for (uint32_t o = 0; o + 32 <= f_clus; o += 32) {
                uint8_t* ent = d + o;
                if (ent[0] == 0) { end = true; break; }
                if (ent[0] == 0xE5 || ent[11] == 0x0F || (ent[11] & 0x18)) continue;
                bool m = true;
                for (int k = 0; k < 11; k++) if (ent[k] != (uint8_t)name83[k]) { m = false; break; }
                if (!m) continue;
                first = ((uint32_t)rd16(ent + 20) << 16) | rd16(ent + 26);
                size  = rd32(ent + 28);
                break;
            }
            if (first || end) break;
            c = fat_next(c);
        }
        if (first < 2 || size < f_clus) return 1;
        // a file must be one contiguous run so the writer can treat it as a
        // flat lba range. a fresh fsutil createnew on an empty volume always is. (satoru)
        uint32_t nclus = size / f_clus;
        for (uint32_t i = 0; i + 1 < nclus; i++) {
            if (fat_next(first + i) != first + i + 1) return 2;
        }
        uint64_t foff = clus_off(first);
        uint64_t flen = (uint64_t)nclus * f_clus;
        if (foff % g_lbasz || flen % g_lbasz) return 3;
        uint64_t flba = foff / g_lbasz;
        if (flba < g_part_lo || flba + flen / g_lbasz > g_part_end) return 3;
        *out_lba = flba; *out_len = flen;
        return 0;
    }

    // is the fat32 volume at partition lba plo labelled KURONO? checks the
    // boot sector label AND the root directory's volume-label entry: windows
    // writes "NO NAME" into the boot sector and keeps the real label only in
    // the root dir, so the boot-sector check alone never matched a partition
    // formatted by windows. (satoru)
    bool fat32_label_is_kurono(uint64_t plo) {
        uint8_t bs[90];
        if (!disk_read(plo * g_lbasz, 90, bs)) return false;
        const char fs[8] = { 'F','A','T','3','2',' ',' ',' ' };
        for (int k = 0; k < 8; k++) if (bs[82 + k] != (uint8_t)fs[k]) return false;
        static const char lab[11] = { 'K','U','R','O','N','O',' ',' ',' ',' ',' ' };
        bool bpb = true;
        for (int k = 0; k < 11; k++) if (bs[71 + k] != (uint8_t)lab[k]) { bpb = false; break; }
        if (bpb) return true;
        uint32_t bps = rd16(bs + 11), spc = bs[13], rsvd = rd16(bs + 14), nfats = bs[16];
        uint32_t fatsz = rd32(bs + 36), root = rd32(bs + 44);
        if (bps < 512 || bps > 4096 || spc == 0 || nfats == 0 || fatsz == 0 || root < 2) return false;
        uint32_t clus = bps * spc;
        if (clus > BOUNCE) return false;
        uint64_t data = plo * g_lbasz + ((uint64_t)rsvd + (uint64_t)nfats * fatsz) * bps;
        // the label entry lives in the root dir's first cluster on a windows format (satoru)
        if (!disk_read(data + (uint64_t)(root - 2) * clus, clus, g_buf)) return false;
        for (uint32_t o = 0; o + 32 <= clus; o += 32) {
            const uint8_t* ent = g_buf + o;
            if (ent[0] == 0) break;
            if (ent[0] == 0xE5 || ent[11] == 0x0F || (ent[11] & 0x08) == 0) continue;
            bool m = true;
            for (int k = 0; k < 11; k++) if (ent[k] != (uint8_t)lab[k]) { m = false; break; }
            if (m) return true;
        }
        return false;
    }
}

bool DiskLog::FindKuronoWindow() {
    if (!NVMe::IsDetected()) { g_status = "nvme not detected"; return false; }
    if (!ensure_buf()) { g_status = "no dma buffer"; return false; }
    g_lbasz = NVMe::GetLBASize();
    if (g_lbasz < 512 || g_lbasz > 4096) g_lbasz = 512;

    uint8_t hdr[92];
    if (!disk_read((uint64_t)g_lbasz, 92, hdr)) { g_status = "nvme READ FAILED (gpt header)"; return false; }
    const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
    for (int i = 0; i < 8; i++) if (hdr[i] != (uint8_t)sig[i]) {
        SerialLogger::Log("[disklog] nvme has no gpt, staying read-only\r\n");
        g_status = "no gpt signature at lba 1 (wrong lba size or read garbage)";
        return false;
    }
    uint64_t ent_lba = rd64(hdr + 72);
    uint32_t nent    = rd32(hdr + 80);
    uint32_t esz     = rd32(hdr + 84);
    if (esz < 128 || esz > 512 || nent == 0 || nent > 256) { g_status = "bad gpt entry table"; return false; }

    static const char want[] = "KURONO";
    int hits = 0;
    uint64_t lo = 0, end = 0;
    for (uint32_t i = 0; i < nent; i++) {
        uint8_t e[128];
        if (!disk_read(ent_lba * g_lbasz + (uint64_t)i * esz, 128, e)) { g_status = "nvme READ FAILED (gpt entries)"; return false; }
        bool used = false;
        for (int k = 0; k < 16; k++) if (e[k]) { used = true; break; }
        if (!used) continue;
        // name is utf-16le at +56, 36 code units, zero padded. exact match only. (satoru)
        bool match = true;
        for (int k = 0; k < 36; k++) {
            uint16_t ch = rd16(e + 56 + k * 2);
            uint16_t w  = (k < 6) ? (uint16_t)want[k] : 0;
            if (ch != w) { match = false; break; }
        }
        if (!match) {
            // windows cannot set a gpt partition name, but it can format fat32
            // with a volume label. so a fat32 partition labelled KURONO counts
            // too. still exactly one match overall, or nothing is writable. (satoru)
            uint64_t plo = rd64(e + 32);
            if (!fat32_label_is_kurono(plo)) continue;
        }
        hits++;
        lo  = rd64(e + 32);
        end = rd64(e + 40) + 1;
    }
    if (hits != 1 || end <= lo || end > NVMe::GetCapacityLBA()) {
        SerialLogger::Log("[disklog] no single gpt partition named KURONO, staying read-only\r\n");
        {
            const char* m = "KURONO partitions found: ";
            int n = 0;
            while (m[n]) { g_status_buf[n] = m[n]; n++; }
            g_status_buf[n++] = (char)('0' + (hits > 9 ? 9 : hits));
            g_status_buf[n] = 0;
            g_status = g_status_buf;
        }
        return false;
    }
    g_part_lo = lo; g_part_end = end; g_window = true;
    g_status = "partition found, log file not opened yet";
    // a second call (the boot retry) must not re-widen a window Init already cut down to the
    // two files; the partition-wide window only exists until the first Init narrows it. (satoru)
    if (!g_narrowed) {
        NVMe::SetWriteWindow(lo, end);
        SerialLogger::Log("[disklog] write window = KURONO partition lba ");
        SerialLogger::LogHex64(lo); SerialLogger::Log(" - "); SerialLogger::LogHex64(end);
        SerialLogger::Log("\r\n");
    }
    return true;
}

bool DiskLog::Init() {
    if (!g_window) return false;
    // start from nothing writable on EVERY exit path, including the allocation failure
    // below: the partition-wide window FindKuronoWindow set must not outlive a failed Init.
    // window 2 (KURONO.KFS) is NOT zeroed up front - a retry would briefly refuse a KFS save
    // already in flight; it is replaced with the real extent (or cleared) once known. (satoru)
    NVMe::SetWriteWindow(0, 0);
    g_narrowed = true;
    if (!ensure_buf()) return false;
    uint64_t base = g_part_lo * g_lbasz;
    uint8_t bs[90];
    if (!disk_read(base, 90, bs)) { g_status = "nvme READ FAILED (kurono boot sector)"; return false; }
    uint32_t bps   = rd16(bs + 11);
    uint32_t spc   = bs[13];
    uint32_t rsvd  = rd16(bs + 14);
    uint32_t nfats = bs[16];
    uint32_t fatsz = rd32(bs + 36);
    f_root         = rd32(bs + 44);
    const char fs[8] = { 'F','A','T','3','2',' ',' ',' ' };
    bool is32 = true;
    for (int i = 0; i < 8; i++) if (bs[82 + i] != (uint8_t)fs[i]) is32 = false;
    if (!is32 || bps < 512 || bps > 4096 || spc == 0 || nfats == 0 || fatsz == 0 || f_root < 2) {
        SerialLogger::Log("[disklog] KURONO partition is not fat32, no log file\r\n");
        g_status = "KURONO partition is not fat32";
        return false;
    }
    f_clus     = bps * spc;
    f_fat_off  = base + (uint64_t)rsvd * bps;
    f_data_off = f_fat_off + (uint64_t)nfats * fatsz * bps;
    if (f_clus > BOUNCE) return false;

    g_fat_win_off = ~0ull;
    // both writable extents are located first, then the guard window is cut down
    // to exactly those two files (no fat, no loader files). (satoru)
    g_kfs_lba = 0; g_kfs_len = 0;
    static const char kfs83[11] = { 'K','U','R','O','N','O',' ',' ','K','F','S' };
    {
        uint64_t klba = 0, klen = 0;
        int kr = find_root_file(kfs83, &klba, &klen);
        if (kr == 0) {
            g_kfs_lba = klba; g_kfs_len = klen; g_kfs_status = "ok";
            NVMe::SetWriteWindow2(klba, klba + klen / g_lbasz);
            SerialLogger::Log("[disklog] KURONO.KFS extent lba ");
            SerialLogger::LogHex64(klba); SerialLogger::Log(" size ");
            SerialLogger::LogDec((int)(klen >> 20)); SerialLogger::Log(" MB\r\n");
        } else {
            NVMe::SetWriteWindow2(0, 0);   // no usable extent: nothing writable (satoru)
            if (kr == 1) g_kfs_status = "KURONO.KFS not found in the KURONO partition root";
            else if (kr == 2) g_kfs_status = "KURONO.KFS is fragmented";
            else g_kfs_status = "KURONO.KFS misaligned or outside the partition";
        }
    }

    static const char name83[11] = { 'K','U','R','O','N','O',' ',' ','L','O','G' };
    uint64_t flba = 0, flen = 0;
    int lr = find_root_file(name83, &flba, &flen);
    if (lr == 1) {
        SerialLogger::Log("[disklog] KURONO.LOG not found (or empty) in the KURONO root\r\n");
        g_status = "KURONO.LOG not found in the partition root";
        return false;
    }
    if (lr == 2) {
        SerialLogger::Log("[disklog] KURONO.LOG is fragmented, not writing to it\r\n");
        g_status = "KURONO.LOG is fragmented";
        return false;
    }
    if (lr != 0) { g_status = "KURONO.LOG misaligned or outside the partition"; return false; }
    if (flen > 0xFFFFF000ull) flen = 0xFFFFF000ull;
    g_file_lba = flba;
    g_file_len = (uint32_t)flen;
    NVMe::SetWriteWindow(g_file_lba, g_file_lba + g_file_len / g_lbasz);

    // blank the old contents so this boot's log never mixes with the last one. (satoru)
    for (uint32_t i = 0; i < BOUNCE; i++) g_buf[i] = 0;
    for (uint32_t off = 0; off < g_file_len; off += BOUNCE) {
        uint32_t n = g_file_len - off; if (n > BOUNCE) n = BOUNCE;
        if (!NVMe::Write(g_file_lba + off / g_lbasz, n / g_lbasz, g_buf)) {
            SerialLogger::Log("[disklog] blanking KURONO.LOG failed\r\n");
            g_status = NVMe::GuardDenied() ? "nvme WRITE REFUSED by the guard" : "nvme WRITE FAILED (blanking KURONO.LOG)";
            return false;
        }
    }
    // no NVMe::Flush() anywhere in here: on the batched io queues it times out
    // (the poll misses a completion SubmitIOBatch already consumed) and spins
    // ~10M iterations each call - a stutter every second from the worker. the
    // writes themselves are complete when they return, and a warm reboot keeps
    // the drive's cache. (satoru)
    g_done = 0;
    g_ready = true;
    g_status = "OK, streaming";
    SerialLogger::Log("[disklog] streaming boot log to KURONO.LOG (");
    SerialLogger::LogDec((int)(g_file_len / 1024)); SerialLogger::Log(" KB)\r\n");
    Flush();
    return true;
}

// ---- read-only root file access (satoru) ----
// everything below uses its own bounce buffer and fat window, never g_buf, so
// it can run from any task while the log worker is flushing. reads only. (satoru)
namespace {
    struct RootRd {
        uint8_t* buf;          // BOUNCE_RD + one lba of slack (satoru)
        uint8_t  fat[4096];
        uint64_t fat_off;
    };
    const uint32_t BOUNCE_RD = 1024u * 1024u;

    bool rr_read(RootRd* r, uint64_t off, uint32_t len, uint8_t* out) {
        uint64_t lba  = off / g_lbasz;
        uint32_t skip = (uint32_t)(off % g_lbasz);
        uint32_t n    = (skip + len + g_lbasz - 1) / g_lbasz;
        if ((uint64_t)n * g_lbasz > BOUNCE_RD + 4096) return false;
        // an occasional queue hiccup (seen once at boot beside the log worker) must not
        // fail a 300 MB load: retry a few times. (satoru)
        bool okr = false;
        for (int t = 0; t < 4 && !okr; t++) okr = NVMe::Read(lba, n, r->buf);
        if (!okr) return false;
        memcpy(out, r->buf + skip, len);
        return true;
    }
    uint32_t rr_next(RootRd* r, uint32_t c) {
        uint64_t eoff = f_fat_off + (uint64_t)c * 4u;
        uint64_t woff = eoff & ~4095ull;
        if (woff != r->fat_off) {
            if (!rr_read(r, woff, 4096, r->fat)) return 0x0FFFFFFFu;
            r->fat_off = woff;
        }
        return rd32(r->fat + (eoff - woff)) & 0x0FFFFFFFu;
    }
    // locate the root dir entry. 0 ok, 1 no partition/geometry, 2 missing, 4 read error. (satoru)
    int rr_find(RootRd* r, const char* name83, uint32_t* first, uint32_t* size) {
        if (!g_window || f_clus == 0 || f_root < 2 || f_clus > BOUNCE_RD) return 1;
        uint32_t c = f_root;
        static uint8_t dirbuf[256 * 1024];     // one cluster, f_clus <= 256k here (satoru)
        if (f_clus > sizeof(dirbuf)) return 1;
        for (int guard = 0; guard < 4096 && c >= 2 && c < 0x0FFFFFF8u; guard++) {
            if (!rr_read(r, clus_off(c), f_clus, dirbuf)) return 4;
            for (uint32_t o = 0; o + 32 <= f_clus; o += 32) {
                uint8_t* ent = dirbuf + o;
                if (ent[0] == 0) return 2;
                if (ent[0] == 0xE5 || ent[11] == 0x0F || (ent[11] & 0x18)) continue;
                bool m = true;
                for (int k = 0; k < 11; k++) if (ent[k] != (uint8_t)name83[k]) { m = false; break; }
                if (!m) continue;
                *first = ((uint32_t)rd16(ent + 20) << 16) | rd16(ent + 26);
                *size  = rd32(ent + 28);
                if (*first < 2 || *size == 0) return 2;
                return 0;
            }
            c = rr_next(r, c);
        }
        return 2;
    }
    RootRd* rr_open() {
        // the log worker and this reader share the nvme io queue; two tasks inside a batched
        // submit at once corrupt it (seen as an lba-out-of-range error mid load). g_busy is the
        // log flush's own gate, so holding it keeps the worker out for the duration. (satoru)
        bool got = false;
        for (uint32_t i = 0; i < 20000000u && !got; i++) {
            uint32_t e = 0;
            got = __atomic_compare_exchange_n(&g_busy, &e, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
            if (!got) __asm__ __volatile__("pause" ::: "memory");
        }
        if (!got) return nullptr;
        RootRd* r = (RootRd*)PMM::AllocBytes(sizeof(RootRd) + 0);
        if (!r) { __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE); return nullptr; }
        r->buf = (uint8_t*)PMM::AllocBytes(BOUNCE_RD + 4096);
        if (!r->buf) { PMM::FreeBytes(r, sizeof(RootRd)); __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE); return nullptr; }
        r->fat_off = ~0ull;
        return r;
    }
    void rr_close(RootRd* r) {
        if (!r) return;
        PMM::FreeBytes(r->buf, BOUNCE_RD + 4096);
        PMM::FreeBytes(r, sizeof(RootRd));
        __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
    }
}

const char* DiskLog::RootFileMsg(int code) {
    switch (code) {
        case 0: return "ok";
        case 1: return "the KURONO partition is not open (no single fat32 partition labelled KURONO)";
        case 2: return "file not found in the KURONO partition root";
        case 3: return "file is larger than the memory reserved for it";
        case 4: return "disk read error";
        case 5: return "broken fat chain (file damaged or truncated)";
        default: return "unknown error";
    }
}

int DiskLog::StatRootFile(const char* name83, uint64_t* size) {
    RootRd* r = rr_open();
    if (!r) return 4;
    uint32_t first = 0, sz = 0;
    int rc = rr_find(r, name83, &first, &sz);
    if (rc == 0 && size) *size = sz;
    rr_close(r);
    return rc;
}

int DiskLog::ReadRootFile(const char* name83, void* dst, uint64_t cap, RootFileProgress prog) {
    RootRd* r = rr_open();
    if (!r) return 4;
    uint32_t first = 0, sz = 0;
    int rc = rr_find(r, name83, &first, &sz);
    if (rc != 0) { rr_close(r); return rc; }
    if ((uint64_t)sz > cap) { rr_close(r); return 3; }
    uint8_t* out = (uint8_t*)dst;
    uint64_t done = 0, next_report = 0;
    uint32_t c = first;
    uint64_t nclus_total = ((uint64_t)sz + f_clus - 1) / f_clus;
    uint64_t seen = 0;
    while (done < sz) {
        if (c < 2 || c >= 0x0FFFFFF8u || seen >= nclus_total) {
            // say exactly where a load stopped, a bad read looks like a broken chain otherwise (satoru)
            SerialLogger::Log("[rootfile] stop c=");  SerialLogger::LogDec((int)c);
            SerialLogger::Log(" seen=");  SerialLogger::LogDec((int)seen);
            SerialLogger::Log(" of ");    SerialLogger::LogDec((int)nclus_total);
            SerialLogger::Log(" fat_off="); SerialLogger::LogDec((int)(r->fat_off >> 12));
            SerialLogger::Log(" first="); SerialLogger::LogDec((int)first);
            SerialLogger::Log("\r\n");
            rc = 5; break;
        }
        // coalesce a run of consecutive clusters into one sequential read (satoru)
        uint32_t run = 1;
        uint32_t cur = c, nxt = rr_next(r, cur);
        while (seen + run < nclus_total && nxt == cur + 1) { run++; cur = nxt; nxt = rr_next(r, cur); }
        uint64_t roff = clus_off(c);
        uint64_t rlen = (uint64_t)run * f_clus;
        uint64_t rpos = 0;
        while (rpos < rlen && done < sz) {
            uint32_t n = (rlen - rpos > BOUNCE_RD) ? BOUNCE_RD : (uint32_t)(rlen - rpos);
            // never read past the file size: the last cluster is only partly
            // used and dst is exactly file sized (an overrun trashed the gdt). (satoru)
            if ((uint64_t)n > sz - done) n = (uint32_t)(sz - done);
            if (!rr_read(r, roff + rpos, n, out + done)) { rc = 4; break; }
            done += n;
            rpos += n;
            if (prog && done >= next_report) { prog(done, sz); next_report = done + (16ull << 20); }
        }
        if (rc) break;
        seen += run;
        c = nxt;
        if (done < sz && seen >= nclus_total) { rc = 5; break; }
    }
    if (rc == 0 && prog) prog(sz, sz);
    rr_close(r);
    return rc;
}

bool DiskLog::KfsFile(uint64_t* lba, uint64_t* bytes) {
    if (g_kfs_len == 0) return false;
    if (lba) *lba = g_kfs_lba;
    if (bytes) *bytes = g_kfs_len;
    return true;
}
const char* DiskLog::KfsStatus() { return g_kfs_status; }

bool DiskLog::Ready() { return g_ready; }
const char* DiskLog::Status() { return g_status; }

// eager mode: the serial line hook flushes after every line (cpu 0 only, and
// never while an nvme command is in flight - that line just rides along with
// the next flush). (satoru)
static void eager_hook() {
    if (!g_ready || NVMe::Busy()) return;
    DiskLog::Flush();
}
void DiskLog::SetEager(bool on) { SerialLogger::SetLineHook(on ? eager_hook : nullptr); }

void DiskLog::Flush() {
    if (!g_ready) return;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&g_busy, &expected, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    const char* cap = SerialLogger::CaptureData();
    uint32_t len = SerialLogger::CaptureLen();
    if (len > g_file_len) len = g_file_len;
    // rewrite from the first not-yet-complete lba up to the current end; the
    // trailing partial lba is zero padded and rewritten next time. (satoru)
    uint32_t start = g_done;
    while (start < len) {
        uint32_t n = len - start;
        if (n > BOUNCE) n = BOUNCE;
        uint32_t nl = (n + g_lbasz - 1) / g_lbasz;
        for (uint32_t i = 0; i < nl * g_lbasz; i++) g_buf[i] = (i < n) ? (uint8_t)cap[start + i] : 0;
        if (!NVMe::Write(g_file_lba + start / g_lbasz, nl, g_buf)) { g_status = "nvme WRITE FAILED while streaming"; break; }
        uint32_t full = (n / g_lbasz) * g_lbasz;
        start += full;
        g_done = start;
        if (full < n) break;
    }
    __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
}

void DiskLog::PanicFlush() {
    static volatile uint32_t once = 0;
    if (__atomic_exchange_n(&once, 1u, __ATOMIC_ACQ_REL)) return;
    if (!g_ready) return;
    SerialLogger::Log("[disklog] panic: flushing log to KURONO.LOG\r\n");
    Flush();
}

[[noreturn]] void DiskLog::WorkerEntry() {
    // heartbeat every 5 s (uptime + input counters) and a second full
    // inventory at ~20 s for usb devices that enumerate late. (satoru)
    for (uint32_t tick = 1; ; tick++) {
        if (tick % 5 == 0) HwInfo::Heartbeat();
        if (tick == 20) HwInfo::DumpAll();
        if (tick == 22) HwInfo::KbDiag(1);
        if (tick == 180) HwInfo::KbDiag(2);
        Flush();
        Scheduler::SleepMs(1000);
    }
}

// end (satoru)
