#include "disklog.h"
#include "nvme.h"
#include "serial.h"
#include "../kernel/pmm.h"
#include "../proc/scheduler.h"
#include "../system/power.h"   // power actions + autosave are also driven from this worker (satoru)
#include "hwinfo.h"
#include "timer.h"   // panic flush wait (satoru)
#include "../linux/linux_syscall.h"   // kurono.usnap thread snapshots (satoru)

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
    uint64_t  g_done     = 0;      // stream bytes already on disk in full lbas (satoru)
    // session layout of the log file (satoru): stream bytes [0, g_head) sit at
    // the same file offsets and are never overwritten (the boot); everything
    // after goes round a ring of g_ring bytes at file offset g_head, so a
    // long session keeps its boot plus its newest part. g_ring == 0 = no
    // wrap (a small file, or no memory for the ring): stop at the file end
    // like before. (satoru)
    uint32_t  g_head     = 0;
    uint32_t  g_ring     = 0;
    uint64_t  g_lost     = 0;      // stream bytes the ring overran before they reached disk (satoru)
    uint64_t  g_lost_said = 0;
    const uint32_t STREAM_RING = 4u * 1024u * 1024u;   // in-memory session ring (satoru)
    volatile uint32_t g_busy = 0;  // a flush is in flight (worker vs panic) (satoru)
    // why the log is or is not running, shown on screen at boot. (satoru)
    const char* g_status = "not started";
    char g_status_buf[64];

    uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
    uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
    uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
    void wr32le(uint8_t* p, uint32_t v) {
        p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    }

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
    // first cluster to consider when allocating: files are written in
    // increasing order, so rescanning from cluster 2 for every delivered asset
    // walked a million fat entries each time. (satoru)
    uint32_t g_alloc_hint = 2;
    // bpb fields needed to allocate clusters in the root (write path) (satoru)
    uint32_t f_totsec   = 0;
    uint32_t f_nfats    = 0;
    uint32_t f_bps      = 0;
    uint32_t f_spc      = 0;
    uint32_t f_rsvd     = 0;
    uint32_t f_fsinfo   = 0;       // fsinfo sector number, 0 = none (satoru)
    uint32_t f_bkboot   = 0;       // backup boot sector number, 0 = none (satoru)
    uint64_t f_fatsz    = 0;       // bytes per fat copy

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
    // guest vm disk images (ALPINE.IMG, DEBIAN.IMG) found by Init: extent or
    // 0, plus why not (1 missing, 2 fragmented, 3 misaligned) (satoru)
    struct GuestImg { char name83[11]; uint64_t lba, len; int why; };
    GuestImg  g_guest_img[2] = {
        { { 'A','L','P','I','N','E',' ',' ','I','M','G' }, 0, 0, 1 },
        { { 'D','E','B','I','A','N',' ',' ','I','M','G' }, 0, 0, 1 },
    };
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

// 8.3 base name from kurono.logname, blank = KURONO.LOG. (satoru)
static char g_alt_name[11] = { ' ',' ',' ',' ',' ',' ',' ',' ','L','O','G' };
void DiskLog::SetLogName(const char* base) {
    for (int k = 0; k < 8; k++) g_alt_name[k] = ' ';
    if (!base) return;
    for (int k = 0; k < 8 && base[k] && base[k] != ' ' && base[k] != '.'; k++) {
        char c = base[k];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) { for (int j = 0; j < 8; j++) g_alt_name[j] = ' '; return; }
        g_alt_name[k] = c;
    }
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
    f_totsec   = rd32(bs + 32);
    f_nfats    = nfats;
    f_bps      = bps;
    f_spc      = spc;
    f_rsvd     = rsvd;
    f_fsinfo   = rd16(bs + 48);
    if (f_fsinfo >= rsvd) f_fsinfo = 0;
    f_bkboot   = rd16(bs + 50);
    if (f_bkboot == 0 || f_bkboot >= rsvd) f_bkboot = 0;
    f_fatsz    = (uint64_t)fatsz * bps;
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

    // guest vm disks: preallocated contiguous files in the partition root. they
    // are found here (the root walk shares the log buffer) but stay read-only
    // until a vm that uses one starts (ArmGuestImage, a third guard window). (satoru)
    NVMe::SetWriteWindow3(0, 0);
    for (int gi = 0; gi < 2; gi++) {
        GuestImg& im = g_guest_img[gi];
        im.lba = im.len = 0;
        uint64_t dlba = 0, dlen = 0;
        im.why = find_root_file(im.name83, &dlba, &dlen);
        if (im.why == 0) {
            im.lba = dlba; im.len = dlen;
            SerialLogger::Log("[disklog] guest disk ");
            for (int k = 0; k < 8 && im.name83[k] != ' '; k++) { char cc[2] = { im.name83[k], 0 }; SerialLogger::Log(cc); }
            SerialLogger::Log(".IMG extent lba ");
            SerialLogger::LogHex64(dlba); SerialLogger::Log(" size ");
            SerialLogger::LogDec((int)(dlen >> 20)); SerialLogger::Log(" MB\r\n");
        } else if (im.why == 2) {
            SerialLogger::Log("[disklog] a guest disk image is fragmented, not usable\r\n");
        }
    }

    static const char name83[11] = { 'K','U','R','O','N','O',' ',' ','L','O','G' };
    uint64_t flba = 0, flen = 0;
    int lr = 1;
    if (g_alt_name[0] != ' ') {
        lr = find_root_file(g_alt_name, &flba, &flen);
        SerialLogger::Log("[disklog] kurono.logname: ");
        for (int k = 0; k < 8 && g_alt_name[k] != ' '; k++) { char cc[2] = { g_alt_name[k], 0 }; SerialLogger::Log(cc); }
        SerialLogger::Log(lr == 0 ? ".LOG found, logging there\r\n" : ".LOG not usable, falling back to KURONO.LOG\r\n");
    }
    if (lr != 0) lr = find_root_file(name83, &flba, &flen);
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
    // whole-session log (satoru): a heap ring behind the linear capture, and
    // the file split into a kept boot head (1/4) and a wrapping tail. files
    // under 1 MB keep the old fill-once behaviour. (satoru)
    g_head = g_file_len; g_ring = 0;
    if (g_file_len >= (1u << 20) && SerialLogger::EnableStreamRing(STREAM_RING)) {
        uint32_t al = (g_lbasz > 4096u) ? g_lbasz : 4096u;
        g_head = (g_file_len / 4u) / al * al;
        g_ring = (g_file_len - g_head) / al * al;
    }
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
        // one cluster, f_clus <= 256k here. only root-file lookups need it,
        // so it is taken from the pmm on the first lookup. (satoru)
        static uint8_t* dirbuf = nullptr;
        if (!dirbuf) {
            dirbuf = (uint8_t*)PMM::AllocBytes(256 * 1024);
            if (!dirbuf) return 1;
        }
        if (f_clus > 256 * 1024) return 1;
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
                // an empty file is present (size 0, fat32 gives it no cluster):
                // the KUROMODE.OFF kill switch is a zero byte file and used to
                // read as missing. only a non empty file needs a real chain. (satoru)
                if (*size == 0) { *first = 0; return 0; }
                if (*first < 2) return 2;
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

// root access can be asked for on a boot without diskguard (no early Init):
// locate the KURONO partition and parse its fat geometry on first use. the log
// file itself may be absent; only geometry is needed for stat/read/write. (satoru)
static bool ensure_root() {
    if (f_clus != 0 && g_window) return true;
    if (!g_window && !DiskLog::FindKuronoWindow()) return false;
    if (f_clus == 0) DiskLog::Init();
    return f_clus != 0 && g_window;
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

int DiskLog::ProbeRootFile(const char* name83, uint64_t* size) {
    if (!g_window) return 1;
    // geometry parse only (see Init below); nothing here writes. (satoru)
    uint64_t base = g_part_lo * g_lbasz;
    uint8_t bs[90];
    if (!disk_read(base, 90, bs)) return 4;
    uint32_t bps   = rd16(bs + 11);
    uint32_t spc   = bs[13];
    uint32_t rsvd  = rd16(bs + 14);
    uint32_t nfats = bs[16];
    uint32_t fatsz = rd32(bs + 36);
    uint32_t root  = rd32(bs + 44);
    const char fs[8] = { 'F','A','T','3','2',' ',' ',' ' };
    bool is32 = true;
    for (int i = 0; i < 8; i++) if (bs[82 + i] != (uint8_t)fs[i]) is32 = false;
    if (!is32 || bps < 512 || bps > 4096 || spc == 0 || nfats == 0 || fatsz == 0 || root < 2)
        return 1;
    f_clus     = bps * spc;
    f_fat_off  = base + (uint64_t)rsvd * bps;
    f_data_off = f_fat_off + (uint64_t)nfats * fatsz * bps;
    f_root     = root;
    g_fat_win_off = ~0ull;
    return StatRootFile(name83, size);
}

int DiskLog::StatRootFile(const char* name83, uint64_t* size) {
    if (!ensure_root()) return 1;
    RootRd* r = rr_open();
    if (!r) return 4;
    uint32_t first = 0, sz = 0;
    int rc = rr_find(r, name83, &first, &sz);
    if (rc == 0 && size) *size = sz;
    rr_close(r);
    return rc;
}

int DiskLog::ReadRootFile(const char* name83, void* dst, uint64_t cap, RootFileProgress prog) {
    if (!ensure_root()) return 1;
    RootRd* r = rr_open();
    if (!r) return 4;
    uint32_t first = 0, sz = 0;
    int rc = rr_find(r, name83, &first, &sz);
    if (rc != 0) { rr_close(r); return rc; }
    if ((uint64_t)sz > cap) { rr_close(r); return 3; }
    // empty file: nothing to read, no chain to follow. (satoru)
    if (sz == 0) { if (prog) prog(0, 0); rr_close(r); return 0; }
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

// ---- write one new file into the fat32 root (delivery path) ----
// the KURONO partition normally arrives pre-populated, but a fresh stick (or a
// machine whose partition only has FIREFOX.TAR) needs the firefox runtime
// assets written on first launch: allocate clusters, write the two FATs, write
// the data, then the root directory entry. writes go through the third nvme
// guard window, so a guarded boot still only touches this partition. (satoru)
namespace {
    bool dl_wr(uint64_t lba, uint32_t count, const void* buf) {
        NVMe::SetWriteWindow3(lba, lba + count);
        bool ok = NVMe::Write(lba, count, (const uint8_t*)buf);
        NVMe::SetWriteWindow3(0, 0);
        return ok;
    }
    // write an arbitrary byte span through the guard window. the FAT copy inside
    // a partition need not be LBA or 4k aligned (the owner's volume has
    // rsvd=5734 sectors and fatsz=13517 sectors), so when the span is not
    // device-lba aligned do a read-modify-write of the covering sectors. tmp
    // must hold 16 kb, src and tmp must differ. (satoru)
    bool dl_wr_span(uint64_t off, uint32_t len, const uint8_t* src, uint8_t* tmp) {
        if (len == 0) return true;
        uint64_t start = (off / g_lbasz) * g_lbasz;
        uint64_t end = ((off + len + g_lbasz - 1) / g_lbasz) * g_lbasz;
        uint32_t span = (uint32_t)(end - start);
        if (span > 16384) return false;
        const uint8_t* out = src;
        if (start != off || span != len) {
            if (!disk_read(start, span, tmp)) return false;
            for (uint32_t i = 0; i < len; i++) tmp[(off - start) + i] = src[i];
            out = tmp;
        }
        return dl_wr(start / g_lbasz, span / g_lbasz, out);
    }
    // read-modify-write one fat entry in both fat copies. the window is indexed
    // RELATIVE to the fat start (the fat itself is not 4k aligned on many
    // volumes) and its length is clamped to the end of the fat. (satoru)
    bool dl_fat_set(uint32_t clus, uint32_t val, uint8_t* win, uint64_t* win_rel, uint8_t* iobuf) {
        uint64_t eoff = (uint64_t)clus * 4u;                 // relative to fat 0
        uint64_t rel  = eoff & ~4095ull;
        if (rel >= f_fatsz) return false;
        uint32_t span = 4096;
        if (rel + span > f_fatsz) span = (uint32_t)(f_fatsz - rel);
        if ((uint32_t)(eoff - rel) + 4u > span) return false;
        if (rel != *win_rel) {
            if (!disk_read(f_fat_off + rel, span, win)) return false;
            *win_rel = rel;
        }
        uint32_t idx = (uint32_t)(eoff - rel);
        uint32_t cur = rd32(win + idx);
        cur = (cur & 0xF0000000u) | (val & 0x0FFFFFFFu);
        win[idx + 0] = (uint8_t)cur;
        win[idx + 1] = (uint8_t)(cur >> 8);
        win[idx + 2] = (uint8_t)(cur >> 16);
        win[idx + 3] = (uint8_t)(cur >> 24);
        for (uint32_t k = 0; k < f_nfats; k++) {
            if (!dl_wr_span(f_fat_off + (uint64_t)k * f_fatsz + rel, span, win, iobuf))
                return false;
        }
        g_fat_win_off = ~0ull;   // the next fat_next must re-read
        return true;
    }
    // FSInfo free count / next free become unknown (0xffffffff) after we
    // allocate without scanning the whole table. both the primary and the
    // backup copy (through the backup boot sector's fsinfo field) are updated
    // when they carry the fsinfo signatures. (satoru)
    bool dl_fsinfo_unknown(uint8_t* buf, uint8_t* iobuf) {
        if (f_rsvd == 0 || f_bps < 512 || f_bps > 4096) return true;
        for (int copy = 0; copy < 2; copy++) {
            uint32_t sec = f_fsinfo;
            if (copy) {
                if (!f_bkboot) break;
                if (!disk_read(g_part_lo * g_lbasz + (uint64_t)f_bkboot * f_bps, f_bps, buf)) return false;
                uint32_t bs_fs = rd16(buf + 48);
                if (bs_fs == 0 || bs_fs >= f_rsvd) break;
                sec = bs_fs;
            }
            if (!sec || sec >= f_rsvd) continue;
            uint64_t off = g_part_lo * g_lbasz + (uint64_t)sec * f_bps;
            if (!disk_read(off, f_bps, buf)) return false;
            if (rd32(buf) != 0x41615252u || rd32(buf + 484) != 0x61417272u) continue;
            wr32le(buf + 488, 0xFFFFFFFFu);
            wr32le(buf + 492, 0xFFFFFFFFu);
            if (!dl_wr_span(off, f_bps, buf, iobuf)) return false;
        }
        return true;
    }
}

int DiskLog::WriteRootFile(const char* name83, const void* data, uint32_t len) {
    if (!ensure_root() || !name83 || !data || len == 0) return 1;
    if (f_clus == 0 || f_clus > 65536 || f_totsec == 0 || f_fatsz == 0) return 1;
    // one writer at a time, same gate as the log flusher (batched io queues). (satoru)
    bool got = false;
    for (uint32_t i = 0; i < 20000000u && !got; i++) {
        uint32_t e = 0;
        got = __atomic_compare_exchange_n(&g_busy, &e, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
        if (!got) __asm__ __volatile__("pause" ::: "memory");
    }
    if (!got) return 4;
    uint8_t* bounce = (uint8_t*)PMM::AllocBytes(65536);
    if (!bounce) { __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE); return 4; }
    uint32_t* freec = nullptr;
    uint32_t freec_cap = 0;
    uint8_t* fatwin = (uint8_t*)PMM::AllocBytes(4096);
    uint8_t* iobuf = (uint8_t*)PMM::AllocBytes(16384);
    if (!fatwin || !iobuf) {
        if (fatwin) PMM::FreeBytes(fatwin, 4096);
        if (iobuf) PMM::FreeBytes(iobuf, 16384);
        PMM::FreeBytes(bounce, 65536);
        __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE); return 4;
    }
    int rc = 4;

    // 1. walk the root chain: existing file, or a free dir slot.
    uint32_t slot_c = 0, slot_o = 0, last_c = 0, c = f_root;
    bool have_slot = false, exists = false;
    for (int guard = 0; guard < 65536 && c >= 2 && c < 0x0FFFFFF8u; guard++) {
        if (!disk_read(clus_off(c), f_clus, bounce)) goto out;
        for (uint32_t o = 0; o + 32 <= f_clus; o += 32) {
            uint8_t* e = bounce + o;
            if (e[0] == 0) {
                if (!have_slot) { slot_c = c; slot_o = o; have_slot = true; }
                goto root_done;
            }
            if (e[0] == 0xE5) {
                if (!have_slot) { slot_c = c; slot_o = o; have_slot = true; }
                continue;
            }
            if (e[11] == 0x0F || (e[11] & 0x18)) continue;
            bool m = true;
            for (int k = 0; k < 11; k++) if (e[k] != (uint8_t)name83[k]) { m = false; break; }
            if (m) { exists = true; goto root_done; }
        }
        last_c = c;
        c = fat_next(c);
    }
root_done:
    if (exists) { rc = 0; goto out; }

    {
        uint64_t nclus = ((uint64_t)len + f_clus - 1) / f_clus;
        uint32_t need = (uint32_t)nclus + (have_slot ? 0 : 1);
        // bpb arithmetic in bytes: total sectors * 512 is implicit in the bpb
        uint32_t fatsectors = (uint32_t)(f_fatsz / f_bps);
        uint32_t total_clus = (f_totsec - f_rsvd - f_nfats * fatsectors) / f_spc + 2;
        freec = (uint32_t*)PMM::AllocBytes((size_t)need * sizeof(uint32_t));
        freec_cap = need;
        if (!freec) { rc = 6; goto out; }
        uint32_t got_n = 0;
        uint8_t* fw = fatwin; uint64_t fwoff = ~0ull;
        uint32_t scan_from = g_alloc_hint ? g_alloc_hint : 2;
        if (scan_from < 2) scan_from = 2;
        for (uint32_t cl = scan_from; cl < total_clus && got_n < need; cl++) {
            uint64_t rel = ((uint64_t)cl * 4u) & ~4095ull;
            if (rel >= f_fatsz) break;
            uint32_t span = 4096;
            if (rel + span > f_fatsz) span = (uint32_t)(f_fatsz - rel);
            if (rel != fwoff) {
                if (!disk_read(f_fat_off + rel, span, fw)) { rc = 4; goto out; }
                fwoff = rel;
            }
            uint32_t idx = (uint32_t)((uint64_t)cl * 4u - rel);
            if ((rd32(fw + idx) & 0x0FFFFFFFu) == 0) freec[got_n++] = cl;
        }
        if (got_n < need) { rc = 6; goto out; }   // partition full
        g_alloc_hint = freec[got_n - 1] + 1;

        // 2. chain fat entries: file clusters, plus the root-extension cluster.
        uint8_t* setwin = fw; uint64_t setoff = ~0ull;
        for (uint32_t i = 0; i < (uint32_t)nclus; i++) {
            uint32_t val = (i + 1 < (uint32_t)nclus) ? freec[i + 1] : 0x0FFFFFFFu;
            if (!dl_fat_set(freec[i], val, setwin, &setoff, iobuf)) { rc = 4; goto out; }
        }
        if (!have_slot) {
            uint32_t ext = freec[nclus];
            if (!dl_fat_set(ext, 0x0FFFFFFFu, setwin, &setoff, iobuf)) { rc = 4; goto out; }
            if (!dl_fat_set(last_c, ext, setwin, &setoff, iobuf)) { rc = 4; goto out; }
        }

        // 3. data clusters, zero padded in the tail.
        uint32_t written = 0;
        for (uint32_t i = 0; i < (uint32_t)nclus; i++) {
            uint32_t n = len - written;
            if (n > f_clus) n = f_clus;
            for (uint32_t k = 0; k < f_clus; k++) bounce[k] = (k < n) ? ((const uint8_t*)data)[written + k] : 0;
            uint64_t off = clus_off(freec[i]);
            if (!dl_wr(off / g_lbasz, f_clus / g_lbasz, bounce)) { rc = 4; goto out; }
            written += n;
        }

        // 4. directory entry (last, so a torn write never exposes a short file).
        if (!have_slot) {
            slot_c = freec[nclus];
            slot_o = 0;
            for (uint32_t k = 0; k < f_clus; k++) bounce[k] = 0;
            uint64_t off = clus_off(slot_c);
            if (!dl_wr(off / g_lbasz, f_clus / g_lbasz, bounce)) { rc = 4; goto out; }
        }
        if (!disk_read(clus_off(slot_c), f_clus, bounce)) { rc = 4; goto out; }
        uint8_t* e = bounce + slot_o;
        for (int k = 0; k < 32; k++) e[k] = 0;
        for (int k = 0; k < 11; k++) e[k] = (uint8_t)name83[k];
        e[11] = 0x20;                                    // archive
        e[20] = (uint8_t)(freec[0] >> 16);               // cluster high
        e[21] = (uint8_t)(freec[0] >> 24);
        e[26] = (uint8_t)(freec[0] & 0xFF);              // cluster low
        e[27] = (uint8_t)((freec[0] >> 8) & 0xFF);
        e[28] = (uint8_t)(len & 0xFF);
        e[29] = (uint8_t)((len >> 8) & 0xFF);
        e[30] = (uint8_t)((len >> 16) & 0xFF);
        e[31] = (uint8_t)((len >> 24) & 0xFF);
        uint64_t off = clus_off(slot_c);
        if (!dl_wr(off / g_lbasz, f_clus / g_lbasz, bounce)) { rc = 4; goto out; }
        // Windows and fsck read the free count / next-free hint; we allocated
        // without a full-table scan, so mark both unknown. (satoru)
        if (!dl_fsinfo_unknown(bounce, iobuf)) { rc = 4; goto out; }
        rc = 0;
    }
out:
    if (freec) PMM::FreeBytes(freec, (size_t)freec_cap * sizeof(uint32_t));
    PMM::FreeBytes(iobuf, 16384);
    PMM::FreeBytes(fatwin, 4096);
    PMM::FreeBytes(bounce, 65536);
    __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
    return rc;
}

// ---- chunked guest disks (satoru)
// a guest disk can be an image plus chunk files NAME.000, NAME.001 ... of a fixed size, created
// on the first write into their range. every chunk is one contiguous cluster run, zero filled
// before it is linked in, so the vm treats it as a flat lba range like the image itself.

int DiskLog::GuestChunkScan(const char* prefix8, uint32_t chunk_bytes, uint64_t* lbas, int max) {
    if (!prefix8 || !lbas || max <= 0 || chunk_bytes == 0 || !ensure_root()) return -1;
    if (f_clus == 0 || f_clus > 256 * 1024) return -1;
    RootRd* r = rr_open();
    if (!r) return -1;
    uint8_t* dirbuf = (uint8_t*)PMM::AllocBytes(f_clus);
    int found = 0;
    for (int i = 0; i < max; i++) lbas[i] = 0;
    if (dirbuf) {
        uint32_t c = f_root;
        bool end = false;
        for (int guard = 0; guard < 4096 && !end && c >= 2 && c < 0x0FFFFFF8u; guard++) {
            if (!rr_read(r, clus_off(c), f_clus, dirbuf)) break;
            for (uint32_t o = 0; o + 32 <= f_clus; o += 32) {
                uint8_t* e = dirbuf + o;
                if (e[0] == 0) { end = true; break; }
                if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x18)) continue;
                bool m = true;
                for (int k = 0; k < 8; k++) if (e[k] != (uint8_t)prefix8[k]) { m = false; break; }
                if (!m || e[8] < '0' || e[8] > '9' || e[9] < '0' || e[9] > '9' || e[10] < '0' || e[10] > '9') continue;
                int idx = (e[8] - '0') * 100 + (e[9] - '0') * 10 + (e[10] - '0');
                uint32_t first = ((uint32_t)rd16(e + 20) << 16) | rd16(e + 26), size = rd32(e + 28);
                if (idx >= max || first < 2 || size != chunk_bytes || size % f_clus) continue;
                uint32_t nclus = size / f_clus;
                bool ok = true;
                for (uint32_t i = 0; i + 1 < nclus && ok; i++) if (rr_next(r, first + i) != first + i + 1) ok = false;
                uint64_t foff = clus_off(first);
                if (!ok || foff % g_lbasz || (uint64_t)size % g_lbasz) continue;
                uint64_t flba = foff / g_lbasz;
                if (flba < g_part_lo || flba + size / g_lbasz > g_part_end) continue;
                lbas[idx] = flba;
                found++;
            }
            if (!end) c = rr_next(r, c);
        }
        PMM::FreeBytes(dirbuf, f_clus);
    } else found = -1;
    rr_close(r);
    return found;
}

int DiskLog::CreateGuestChunk(const char* name83, uint32_t bytes, uint64_t* lba_out) {
    if (!ensure_root() || !name83 || bytes == 0) return 1;
    if (f_clus == 0 || f_clus > 65536 || f_totsec == 0 || f_fatsz == 0 || bytes % f_clus) return 1;
    if (clus_off(2) % g_lbasz || (uint64_t)f_clus % g_lbasz) return 3;
    bool got = false;
    for (uint32_t i = 0; i < 20000000u && !got; i++) {
        uint32_t e = 0;
        got = __atomic_compare_exchange_n(&g_busy, &e, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
        if (!got) __asm__ __volatile__("pause" ::: "memory");
    }
    if (!got) return 4;
    const uint32_t ZB = 256 * 1024;
    uint8_t* bounce = (uint8_t*)PMM::AllocBytes(65536);
    uint8_t* fatwin = (uint8_t*)PMM::AllocBytes(4096);
    uint8_t* iobuf = (uint8_t*)PMM::AllocBytes(16384);
    uint8_t* zbuf = (uint8_t*)PMM::AllocBytes(ZB);
    int rc = 4;
    if (!bounce || !fatwin || !iobuf || !zbuf) goto out;
    {
        for (uint32_t i = 0; i < ZB; i++) zbuf[i] = 0;
        // 1. existing file / free dir slot
        uint32_t slot_c = 0, slot_o = 0, last_c = 0, c = f_root;
        bool have_slot = false;
        for (int guard = 0; guard < 65536 && c >= 2 && c < 0x0FFFFFF8u; guard++) {
            if (!disk_read(clus_off(c), f_clus, bounce)) goto out;
            for (uint32_t o = 0; o + 32 <= f_clus; o += 32) {
                uint8_t* e = bounce + o;
                if (e[0] == 0) { if (!have_slot) { slot_c = c; slot_o = o; have_slot = true; } goto root_done; }
                if (e[0] == 0xE5) { if (!have_slot) { slot_c = c; slot_o = o; have_slot = true; } continue; }
                if (e[11] == 0x0F || (e[11] & 0x18)) continue;
                bool m = true;
                for (int k = 0; k < 11; k++) if (e[k] != (uint8_t)name83[k]) { m = false; break; }
                if (m) { rc = 7; goto out; }
            }
            last_c = c;
            c = fat_next(c);
        }
    root_done:
        uint32_t nclus = bytes / f_clus;
        uint32_t need = nclus + (have_slot ? 0 : 1);
        uint32_t fatsectors = (uint32_t)(f_fatsz / f_bps);
        uint32_t total_clus = (f_totsec - f_rsvd - f_nfats * fatsectors) / f_spc + 2;
        // 2. first run of `need` free clusters, from the hint and then from the start
        uint32_t run_start = 0, run_len = 0, found_at = 0;
        for (int pass = 0; pass < 2 && !found_at; pass++) {
            uint32_t from = pass == 0 ? (g_alloc_hint > 2 ? g_alloc_hint : 2) : 2;
            uint32_t to = pass == 0 ? total_clus : (g_alloc_hint > 2 ? g_alloc_hint + need : total_clus);
            if (to > total_clus) to = total_clus;
            uint64_t fwoff = ~0ull;
            run_start = 0; run_len = 0;
            for (uint32_t cl = from; cl < to; cl++) {
                uint64_t rel = ((uint64_t)cl * 4u) & ~4095ull;
                if (rel >= f_fatsz) break;
                uint32_t span = 4096;
                if (rel + span > f_fatsz) span = (uint32_t)(f_fatsz - rel);
                if (rel != fwoff) { if (!disk_read(f_fat_off + rel, span, fatwin)) goto out; fwoff = rel; }
                bool fr = (rd32(fatwin + (uint32_t)((uint64_t)cl * 4u - rel)) & 0x0FFFFFFFu) == 0;
                if (fr) { if (!run_len) run_start = cl; if (++run_len >= need) { found_at = run_start; break; } }
                else run_len = 0;
            }
        }
        if (!found_at) { rc = 6; goto out; }          // no contiguous run: full or too fragmented
        uint32_t first = found_at, ext = have_slot ? 0 : first + nclus;
        g_alloc_hint = first + need;
        // 3. chain: first .. first+nclus-1, one fat window at a time, both copies
        {
            uint64_t wrel = ~0ull;
            uint32_t span = 0;
            auto flush = [&]() -> bool {
                if (wrel == ~0ull) return true;
                for (uint32_t k = 0; k < f_nfats; k++)
                    if (!dl_wr_span(f_fat_off + (uint64_t)k * f_fatsz + wrel, span, fatwin, iobuf)) return false;
                return true;
            };
            auto setv = [&](uint32_t cl, uint32_t val) -> bool {
                uint64_t rel = ((uint64_t)cl * 4u) & ~4095ull;
                if (rel != wrel) {
                    if (!flush()) return false;
                    span = 4096; if (rel + span > f_fatsz) span = (uint32_t)(f_fatsz - rel);
                    if (!disk_read(f_fat_off + rel, span, fatwin)) { wrel = ~0ull; return false; }
                    wrel = rel;
                }
                uint32_t idx = (uint32_t)((uint64_t)cl * 4u - rel);
                uint32_t cur = (rd32(fatwin + idx) & 0xF0000000u) | (val & 0x0FFFFFFFu);
                wr32le(fatwin + idx, cur);
                return true;
            };
            for (uint32_t i = 0; i < nclus; i++) if (!setv(first + i, i + 1 < nclus ? first + i + 1 : 0x0FFFFFFFu)) goto out;
            if (!have_slot) {
                if (!setv(ext, 0x0FFFFFFFu) || !setv(last_c, ext)) goto out;
            }
            if (!flush()) goto out;
            g_fat_win_off = ~0ull;
        }
        // 4. zero the data, so nothing a deleted file left behind shows through
        {
            uint64_t base = clus_off(first);
            for (uint64_t done = 0; done < bytes; ) {
                uint32_t n = ZB; if (bytes - done < n) n = (uint32_t)(bytes - done);
                if (!dl_wr((base + done) / g_lbasz, n / g_lbasz, zbuf)) goto out;
                done += n;
            }
        }
        // 5. directory entry last
        if (!have_slot) {
            slot_c = ext; slot_o = 0;
            for (uint32_t k = 0; k < f_clus; k++) bounce[k] = 0;
            if (!dl_wr(clus_off(slot_c) / g_lbasz, f_clus / g_lbasz, bounce)) goto out;
        }
        if (!disk_read(clus_off(slot_c), f_clus, bounce)) goto out;
        uint8_t* e = bounce + slot_o;
        for (int k = 0; k < 32; k++) e[k] = 0;
        for (int k = 0; k < 11; k++) e[k] = (uint8_t)name83[k];
        e[11] = 0x20;
        e[20] = (uint8_t)(first >> 16); e[21] = (uint8_t)(first >> 24);
        e[26] = (uint8_t)(first & 0xFF); e[27] = (uint8_t)((first >> 8) & 0xFF);
        wr32le(e + 28, bytes);
        if (!dl_wr(clus_off(slot_c) / g_lbasz, f_clus / g_lbasz, bounce)) goto out;
        if (!dl_fsinfo_unknown(bounce, iobuf)) goto out;
        if (lba_out) *lba_out = clus_off(first) / g_lbasz;
        rc = 0;
    }
out:
    if (zbuf) PMM::FreeBytes(zbuf, ZB);
    if (iobuf) PMM::FreeBytes(iobuf, 16384);
    if (fatwin) PMM::FreeBytes(fatwin, 4096);
    if (bounce) PMM::FreeBytes(bounce, 65536);
    __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
    return rc;
}

uint32_t DiskLog::ClusterBytes() { return f_clus; }
bool DiskLog::PartitionRange(uint64_t* lo, uint64_t* end) {
    if (!g_window) return false;
    if (lo) *lo = g_part_lo;
    if (end) *end = g_part_end;
    return true;
}

uint32_t DiskLog::LbaSize() { return g_lbasz; }

int DiskLog::RootFileExtent(const char* name83, uint64_t* lba, uint64_t* bytes, uint32_t* size) {
    if (!name83 || !ensure_root()) return 1;
    RootRd* r = rr_open();
    if (!r) return 4;
    uint32_t first = 0, sz = 0;
    int rc = rr_find(r, name83, &first, &sz);
    if (rc == 0 && (sz == 0 || first < 2)) rc = 2;
    if (rc == 0) {
        uint32_t nclus = (sz + f_clus - 1) / f_clus;
        for (uint32_t i = 0; i + 1 < nclus && rc == 0; i++)
            if (rr_next(r, first + i) != first + i + 1) rc = 5;
        uint64_t foff = clus_off(first);
        uint64_t flen = (uint64_t)nclus * f_clus;
        if (rc == 0 && (foff % g_lbasz || flen % g_lbasz)) rc = 5;
        uint64_t flba = foff / g_lbasz;
        if (rc == 0 && (flba < g_part_lo || flba + flen / g_lbasz > g_part_end)) rc = 5;
        if (rc == 0) {
            if (lba) *lba = flba;
            if (bytes) *bytes = flen;
            if (size) *size = sz;
        }
    }
    rr_close(r);
    return rc;
}

bool DiskLog::WriteRootExtent(uint64_t lba, const void* data, uint32_t len) {
    if (!g_window || !g_buf || !data || len == 0 || len > 65536 || len % g_lbasz) return false;
    uint32_t n = len / g_lbasz;
    if (lba < g_part_lo || lba + n > g_part_end) return false;   // inside KURONO only (satoru)
    // a short try only: the holder may be a cooperative proc parked on this very
    // cpu, so a long spin could never succeed. busy = false, the caller keeps
    // the data and retries later. (satoru)
    bool got = false;
    for (uint32_t i = 0; i < 20000u && !got; i++) {
        uint32_t e = 0;
        got = __atomic_compare_exchange_n(&g_busy, &e, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
        if (!got) __asm__ __volatile__("pause" ::: "memory");
    }
    if (!got) return false;
    memcpy(g_buf, data, len);
    // its own guard window (4): the vm guest-image window 3 is never touched. (satoru)
    NVMe::SetWriteWindow4(lba, lba + n);
    bool ok = NVMe::Write(lba, n, g_buf);
    NVMe::SetWriteWindow4(0, 0);
    __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
    return ok;
}

int DiskLog::GuestImage(const char* name83, uint64_t* lba, uint64_t* bytes) {
    for (int gi = 0; gi < 2; gi++) {
        GuestImg& im = g_guest_img[gi];
        bool m = true;
        for (int k = 0; k < 11; k++) if (im.name83[k] != name83[k]) { m = false; break; }
        if (!m) continue;
        if (im.why != 0) return im.why;
        if (lba) *lba = im.lba;
        if (bytes) *bytes = im.len;
        return 0;
    }
    return 1;
}

bool DiskLog::ArmGuestImage(const char* name83, uint64_t* lba, uint64_t* bytes) {
    uint64_t l = 0, b = 0;
    if (GuestImage(name83, &l, &b) != 0) { NVMe::SetWriteWindow3(0, 0); return false; }
    NVMe::SetWriteWindow3(l, l + b / g_lbasz);
    if (lba) *lba = l;
    if (bytes) *bytes = b;
    return true;
}

void DiskLog::DisarmGuestImage() { NVMe::SetWriteWindow3(0, 0); }

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

volatile uint32_t g_usnap_secs = 0;   // kurono.usnap=N (satoru)
static volatile bool g_worker_flush = false;   // Flush called from WorkerEntry (satoru)

// stream offset -> file offset, and how many bytes from there before the
// region (head, or the ring's end) ends. (satoru)
static uint32_t dl_map(uint64_t s, uint32_t* room) {
    if (s < g_head || g_ring == 0) { *room = (s < g_head) ? (uint32_t)(g_head - s) : 0; return (uint32_t)s; }
    uint32_t r = (uint32_t)((s - g_head) % g_ring);
    *room = g_ring - r;
    return g_head + r;
}

bool DiskLog::Flush() {
    if (!g_ready) return true;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&g_busy, &expected, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return false;
    bool ok = true;
    uint64_t len = SerialLogger::StreamTotal();
    if (g_ring == 0 && len > g_file_len) len = g_file_len;
    // the writers lapped the memory ring (a log burst > 4 MB between two
    // flushes): skip to what is still there, keep lba alignment, count it. (satoru)
    uint32_t rs = SerialLogger::StreamRingSize();
    if (rs && len > g_done && len - g_done > rs) {
        uint64_t nd = (len - rs / 2) / g_lbasz * g_lbasz;
        if (nd > g_done) { g_lost += nd - g_done; g_done = nd; }
    }
    // rewrite from the first not-yet-complete lba up to the current end; the
    // trailing partial lba is zero padded and rewritten next time. (satoru)
    uint64_t start = g_done;
    while (start < len) {
        uint32_t room = 0;
        uint32_t fo = dl_map(start, &room);
        if (room == 0) break;   // no-wrap file is full (satoru)
        uint64_t left = len - start;
        uint32_t n = (left > BOUNCE) ? BOUNCE : (uint32_t)left;
        // one lba of slack for the wrap marker below (satoru)
        if (n > BOUNCE - g_lbasz) n = BOUNCE - g_lbasz;
        if (n > room) n = room;
        uint32_t got = SerialLogger::StreamRead(start, (char*)g_buf, n);
        if (got == 0) {
            if (!rs) break;   // linear capture exhausted (no ring) (satoru)
            // lapped while we copied: jump to what the ring still holds (satoru)
            uint64_t nd = (SerialLogger::StreamTotal() - rs / 2) / g_lbasz * g_lbasz;
            if (nd <= start) nd = start + g_lbasz;
            g_lost += nd - start; start = nd; g_done = nd;
            continue;
        }
        n = got;
        uint32_t nl = (n + g_lbasz - 1) / g_lbasz;
        for (uint32_t i = n; i < nl * g_lbasz; i++) g_buf[i] = 0;
        uint32_t full = (n / g_lbasz) * g_lbasz;
        bool last = (start + n >= len);
        // once the ring has wrapped, the newest end is followed by the oldest
        // tail lines: put a marker lba right after the newest data so a reader
        // (and tools/disklog_unwrap.py) knows where the session ends. (satoru)
        uint32_t wl = nl;
        if (last && g_ring && start + n > (uint64_t)g_head + g_ring &&
            fo + (nl + 1) * g_lbasz <= g_head + g_ring) {
            uint8_t* mk = g_buf + nl * g_lbasz;
            for (uint32_t i = 0; i < g_lbasz; i++) mk[i] = 0;
            const char* m1 = "\n===== [disklog] END OF NEWEST DATA. older lines follow; head=";
            uint32_t k = 0;
            for (const char* q = m1; *q && k < g_lbasz - 40; q++) mk[k++] = (uint8_t)*q;
            char d[12]; int nd = 0; uint32_t hv = g_head;
            if (!hv) d[nd++] = '0';
            while (hv && nd < 12) { d[nd++] = (char)('0' + hv % 10); hv /= 10; }
            while (nd) mk[k++] = (uint8_t)d[--nd];
            const char* m2 = " =====\n";
            for (const char* q = m2; *q; q++) mk[k++] = (uint8_t)*q;
            wl = nl + 1;
        }
        if (!NVMe::Write(g_file_lba + fo / g_lbasz, wl, g_buf)) { g_status = "nvme WRITE FAILED while streaming"; ok = false; break; }
        start += full;
        g_done = start;
        if (full < n) break;
        // between chunks, from the worker only: Flush also runs from the eager serial
        // hook after ANY log line, where a yield would land mid-sequence in whatever
        // proc logged (review finding). g_busy keeps a second flush out. (satoru)
        if (g_worker_flush) Scheduler::CondResched();
    }
    __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);
    return ok;
}

// the panic path flushes several times (before each screen, before each reset
// method) so the last line on disk says how far it got. the first call stops
// the per-line eager hook (the panic's own logging must not re-enter nvme) and
// puts nvme in its bounded panic mode. a flush the panic interrupted (on this
// cpu, or on a cpu the panic halted) keeps g_busy forever: wait ~250 ms for it,
// then take it over. one retry when a write fails. (satoru)
void DiskLog::PanicFlush() {
    if (!g_ready) return;
    // a fault inside a panic flush re-enters the panic, which flushes again:
    // never nest, the reset must still be reached. (satoru)
    static volatile uint32_t s_in = 0;
    if (__atomic_exchange_n(&s_in, 1u, __ATOMIC_ACQ_REL)) return;
    static volatile uint32_t s_first = 0;
    if (!__atomic_exchange_n(&s_first, 1u, __ATOMIC_ACQ_REL)) {
        SetEager(false);
        NVMe::SetPanicIo(true);
        SerialLogger::Log("[disklog] panic: flushing log to KURONO.LOG\r\n");
    }
    uint64_t t0 = Timer::GetRealMs64();   // tsc ms clock, fine with interrupts off (satoru)
    for (uint64_t n = 0; __atomic_load_n(&g_busy, __ATOMIC_ACQUIRE) != 0; n++) {
        __asm__ __volatile__("pause");
        if (Timer::GetRealMs64() - t0 > 250u || n > 100000000ull) {
            __atomic_store_n(&g_busy, 0u, __ATOMIC_RELEASE);   // (satoru)
            break;
        }
    }
    if (!Flush()) Flush();
    __atomic_store_n(&s_in, 0u, __ATOMIC_RELEASE);
}

[[noreturn]] void DiskLog::WorkerEntry() {
    // heartbeat every 5 s (uptime + input counters) and a second full
    // inventory at ~20 s for usb devices that enumerate late. (satoru)
    for (uint32_t tick = 1; ; tick++) {
        if (tick % 5 == 0) HwInfo::Heartbeat();
        if (tick == 20) HwInfo::DumpAll();
        if (tick == 22) HwInfo::KbDiag(1);
        if (tick == 180) HwInfo::KbDiag(2);
        // this worker is the one kernel proc that keeps running while firefox
        // loads the bsp, so it also services the power button, queued power
        // actions and the autosave cadence, before a flush that can block for
        // seconds on a busy disk. one caller wins each via CAS. (satoru)
        PowerManager::BackgroundTick();
        if (g_lost != g_lost_said) {   // said here, never from inside Flush (satoru)
            uint64_t d = g_lost - g_lost_said; g_lost_said = g_lost;
            SerialLogger::Log("[disklog] log burst overran the 4 MB memory ring: ");
            SerialLogger::LogDec((int)(d / 1024)); SerialLogger::Log(" KB never reached KURONO.LOG\r\n");
        }
        if (g_usnap_secs && tick % g_usnap_secs == 0) {   // kurono.usnap=N, before the flush, kls dropped for it (satoru)
            LinuxSyscall::LayerLock();
            LinuxSyscall::DumpAllUserThreads("usnap");
            LinuxSyscall::LayerUnlock();
        }
        g_worker_flush = true;    // (satoru)
        Flush();
        g_worker_flush = false;
        Scheduler::SleepMs(1000);
    }
}

// end (satoru)
