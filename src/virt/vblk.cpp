//  kurono os - virtio-blk host device (see vblk.h)
#include "vblk.h"
#include "virtio_dev.h"
#include "../drivers/nvme.h"
#include "../drivers/disklog.h"
#include "../drivers/serial.h"
#include "../kernel/pmm.h"
#include "../kernel/types.h"
#include "guest_mem.h"

namespace {
    VirtioDev g_dev;
    uint64_t g_base_lba = 0;      // device lba of sector 0 of the image
    uint64_t g_sectors = 0;       // 512 byte sectors
    uint32_t g_lbasz = 512;
    bool     g_have = false;
    uint8_t* g_bounce = nullptr;  // one bounce buffer, BOUNCE bytes
    const uint32_t BOUNCE = 256 * 1024;
    uint32_t g_reads, g_writes, g_flushes, g_errors;
    uint64_t g_bytes_r, g_bytes_w;
    char     g_id[20];

    // chunked disk (satoru): virtual sectors [0, g_img_sectors) are the image, the rest are
    // chunk files NAME.000 ... of g_chunk_sec sectors each, created on their first write; a
    // chunk that does not exist reads as zeros.
    const uint32_t MAX_CHUNKS = 1000;
    uint64_t g_img_sectors = 0, g_chunk_sec = 0;
    uint32_t g_chunk_bytes = 0, g_nchunks = 0, g_chunks_present = 0;
    uint64_t* g_chunk_lba = nullptr;
    char     g_chunk_prefix[8], g_img83[11];
    bool     g_chunk_warned = false;

    // read/write `n` 512 byte sectors at sector `s` of the extent that starts at device lba
    // `base` through the bounce buffer; handles lba sizes larger than 512 with
    // read-modify-write. chunk extents are written under guard window 4.
    bool raw(bool wr, uint64_t base, uint64_t s, uint32_t n, uint8_t* buf, bool win4) {
        uint64_t byte_off = s * 512, bytes = (uint64_t)n * 512;
        uint64_t lba0 = base + byte_off / g_lbasz;
        uint32_t skip = (uint32_t)(byte_off % g_lbasz);
        uint32_t nl = (uint32_t)((skip + bytes + g_lbasz - 1) / g_lbasz);
        if ((uint64_t)nl * g_lbasz > BOUNCE) return false;
        if (!wr) {
            bool ok = false;
            for (int t = 0; t < 3 && !ok; t++) ok = NVMe::Read(lba0, nl, g_bounce);
            if (!ok) return false;
            memcpy(buf, g_bounce + skip, (size_t)bytes);
            return true;
        }
        if (skip != 0 || (bytes % g_lbasz) != 0) {      // partial lba: read first
            if (!NVMe::Read(lba0, nl, g_bounce)) return false;
        }
        memcpy(g_bounce + skip, buf, (size_t)bytes);
        bool ok = false;
        if (win4) NVMe::SetWriteWindow4(lba0, lba0 + nl);
        for (int t = 0; t < 3 && !ok; t++) ok = NVMe::Write(lba0, nl, g_bounce);
        if (win4) NVMe::SetWriteWindow4(0, 0);
        return ok;
    }

    bool make_chunk(uint32_t c) {
        char nm[11];
        for (int i = 0; i < 8; i++) nm[i] = g_chunk_prefix[i];
        nm[8] = (char)('0' + c / 100); nm[9] = (char)('0' + (c / 10) % 10); nm[10] = (char)('0' + c % 10);
        uint64_t lba = 0;
        int rc = DiskLog::CreateGuestChunk(nm, g_chunk_bytes, &lba);
        DiskLog::ArmGuestImage(g_img83, nullptr, nullptr);          // creating a chunk cleared window 3
        if (rc == 7) {                                               // exists but was not found at start: look again
            DiskLog::GuestChunkScan(g_chunk_prefix, g_chunk_bytes, g_chunk_lba, (int)MAX_CHUNKS);
            if (g_chunk_lba[c]) return true;
        }
        if (rc != 0) {
            if (!g_chunk_warned) {
                g_chunk_warned = true;
                SerialLogger::Log("[vblk] cannot create guest disk chunk "); SerialLogger::LogDec((int)c);
                SerialLogger::Log(": rc="); SerialLogger::LogDec(rc);
                SerialLogger::Log(rc == 6 ? " (partition full or no contiguous run)\r\n" : "\r\n");
            }
            return false;
        }
        g_chunk_lba[c] = lba;
        g_chunks_present++;
        return true;
    }

    // virtual sector range -> image / chunks (satoru)
    bool rw(bool wr, uint64_t s, uint32_t n, uint8_t* buf) {
        if (s + n > g_sectors || s + n < s) return false;
        while (n) {
            uint32_t piece;
            if (s < g_img_sectors) {
                piece = (uint32_t)((g_img_sectors - s) < n ? (g_img_sectors - s) : n);
                if (!raw(wr, g_base_lba, s, piece, buf, false)) return false;
            } else {
                uint64_t rel = s - g_img_sectors;
                uint32_t c = (uint32_t)(rel / g_chunk_sec);
                uint64_t off = rel % g_chunk_sec;
                piece = (uint32_t)((g_chunk_sec - off) < n ? (g_chunk_sec - off) : n);
                if (c >= g_nchunks) return false;
                if (!g_chunk_lba[c]) {
                    if (!wr) memset(buf, 0, (size_t)piece * 512);
                    else if (!make_chunk(c)) return false;
                }
                if (g_chunk_lba[c] && !raw(wr, g_chunk_lba[c], off, piece, buf, true)) return false;
            }
            s += piece; n -= piece; buf += (size_t)piece * 512;
        }
        return true;
    }

    // move `len` bytes between the image at sector `sec` and guest memory at
    // `gpa`, splitting at host chunk boundaries and the bounce size (satoru)
    bool xfer(bool wr, uint64_t& sec, uint64_t gpa, uint32_t len) {
        if (len % 512) return false;
        uint32_t pos = 0;
        while (pos < len) {
            uint8_t* gm = GuestMemoryManager::GuestPhysToHost(gpa + pos);
            uint32_t run = GuestMemoryManager::ContigBytes(gpa + pos);
            if (!gm || run < 512) {
                // a sector straddling two host chunks: stage it whole (satoru)
                if (!gm) return false;
                uint8_t tmp[512];
                if (wr) {
                    if (!GuestMemoryManager::ReadGuestPhys(gpa + pos, tmp, 512)) return false;
                    if (!rw(true, sec, 1, tmp)) return false;
                } else {
                    if (!rw(false, sec, 1, tmp)) return false;
                    if (!GuestMemoryManager::WriteGuestPhys(gpa + pos, tmp, 512)) return false;
                }
                sec += 1; pos += 512;
                continue;
            }
            uint32_t chunk = len - pos;
            if (chunk > run) chunk = run;
            if (chunk > BOUNCE - 4096) chunk = BOUNCE - 4096;
            chunk &= ~511u;
            if (!rw(wr, sec, chunk / 512, gm)) return false;
            sec += chunk / 512; pos += chunk;
        }
        return true;
    }

    void handle(VirtioDev* d, VirtioChain* c) {
        uint8_t status = 0;     // VIRTIO_BLK_S_OK
        uint32_t written = 0;
        struct { uint32_t type, resv; uint64_t sector; } h;
        if (c->n < 2 || c->truncated || VirtioReadChain(c, 0, &h, 16) != 16) { status = 1; goto done; }
        if (h.type == 0 || h.type == 1) {                 // IN / OUT
            bool wr = (h.type == 1);
            uint64_t sec = h.sector;
            uint32_t moved = 0;
            // data descriptors: everything but the header bytes and the status byte
            uint32_t hdr_left = 16;
            for (int i = 0; i < c->n; i++) {
                uint64_t a = c->addr[i]; uint32_t l = c->len[i];
                if (i == 0 || hdr_left) {                 // header may share a descriptor
                    uint32_t k = l < hdr_left ? l : hdr_left;
                    a += k; l -= k; hdr_left -= k;
                }
                if (i == c->n - 1) { if (l == 0) break; l -= 1; }   // status byte
                if (l == 0) continue;
                if (c->write[i] != !wr) { status = 1; goto done; }
                if (!xfer(wr, sec, a, l)) { status = 1; g_errors++; goto done; }
                moved += l;
            }
            if (wr) { g_writes++; g_bytes_w += moved; }
            else { g_reads++; g_bytes_r += moved; written = moved; }
        } else if (h.type == 4) {                         // FLUSH
            g_flushes++;                                  // writes are complete when NVMe::Write returns
        } else if (h.type == 8) {                         // GET_ID
            written = VirtioWriteChain(c, 0, g_id, 20);
        } else {
            status = 2;                                   // UNSUPP
        }
    done:
        // the status byte is the last byte of the last descriptor
        {
            uint64_t sa = c->addr[c->n - 1] + c->len[c->n - 1] - 1;
            uint8_t* sp = (c->n >= 1 && c->len[c->n - 1]) ? GuestMemoryManager::GuestPhysToHost(sa) : nullptr;
            if (sp) *sp = status;
            written += 1;
        }
        VirtioPush(d, 0, c->head, written);
    }

    void on_notify(VirtioDev* d, int qid) {
        if (qid != 0) return;
        VirtioChain c;
        int n = 0;
        while (VirtioPop(d, 0, &c)) { if (c.n >= 1) handle(d, &c); n++; }
        if (n) VirtioRaise(d);
    }
}

bool VBlk::Init(uint64_t lba, uint64_t bytes, uint8_t irq, const char* id,
                const char* chunk_prefix8, const char* img83, uint64_t virt_bytes) {
    g_have = false;
    if (!bytes) return false;
    g_lbasz = NVMe::GetLBASize();
    if (g_lbasz < 512) g_lbasz = 512;
    if (!g_bounce) g_bounce = (uint8_t*)PMM::AllocBytes(BOUNCE + 4096);
    if (!g_bounce) return false;
    g_base_lba = lba;
    g_sectors = bytes / 512;
    g_img_sectors = g_sectors; g_nchunks = 0; g_chunks_present = 0; g_chunk_warned = false;
    // an ext4 that was grown into the chunked tail must never see a smaller disk (turning the
    // setting off, or deleting the chunk files, would make the guest refuse to mount): the disk
    // is at least as big as the filesystem says it is (satoru)
    if (chunk_prefix8 && img83) {
        uint32_t rn = (4096u + g_lbasz - 1) / g_lbasz;
        if (rn * g_lbasz <= BOUNCE && NVMe::Read(lba, rn, g_bounce)) {
            const uint8_t* sb = g_bounce + 1024;
            if (sb[56] == 0x53 && sb[57] == 0xEF) {
                uint64_t blocks = (uint64_t)sb[4] | ((uint64_t)sb[5] << 8) | ((uint64_t)sb[6] << 16) | ((uint64_t)sb[7] << 24);
                uint32_t incompat = sb[0x60] | (sb[0x61] << 8) | (sb[0x62] << 16) | ((uint32_t)sb[0x63] << 24);
                if (incompat & 0x80) blocks |= ((uint64_t)sb[0x150] | ((uint64_t)sb[0x151] << 8) | ((uint64_t)sb[0x152] << 16) | ((uint64_t)sb[0x153] << 24)) << 32;
                uint32_t bs = 1024u << (sb[24] & 15);
                uint64_t fsb = blocks * bs;
                if (fsb > virt_bytes) virt_bytes = fsb;
            }
        }
    }
    if (chunk_prefix8 && img83 && virt_bytes > bytes + (64ull << 20)) {
        // chunked disk: 64 mb chunks, as many as the virtual size asks for (satoru)
        g_chunk_bytes = 64u << 20;
        g_chunk_sec = g_chunk_bytes / 512;
        if (!g_chunk_lba) g_chunk_lba = (uint64_t*)PMM::AllocBytes(MAX_CHUNKS * sizeof(uint64_t));
        if (g_chunk_lba) {
            for (int i = 0; i < 8; i++) g_chunk_prefix[i] = chunk_prefix8[i];
            for (int i = 0; i < 11; i++) g_img83[i] = img83[i];
            uint64_t n = (virt_bytes - bytes + g_chunk_bytes - 1) / g_chunk_bytes;
            if (n > MAX_CHUNKS) n = MAX_CHUNKS;
            g_nchunks = (uint32_t)n;
            int f = DiskLog::GuestChunkScan(g_chunk_prefix, g_chunk_bytes, g_chunk_lba, (int)MAX_CHUNKS);
            g_chunks_present = f > 0 ? (uint32_t)f : 0;
            // a chunk past the configured size stays out of reach but is not touched (satoru)
            g_sectors = g_img_sectors + (uint64_t)g_nchunks * g_chunk_sec;
        }
    }
    g_reads = g_writes = g_flushes = g_errors = 0;
    g_bytes_r = g_bytes_w = 0;
    for (int i = 0; i < 20; i++) g_id[i] = 0;
    for (int i = 0; id && id[i] && i < 20; i++) g_id[i] = id[i];
    g_dev = {};
    g_dev.name = "virtio-blk";
    g_dev.devtype = 2;
    g_dev.pci_class = 0x01; g_dev.pci_subclass = 0x00;
    g_dev.irq = irq;
    g_dev.nqueues = 1;
    g_dev.queue_max = 128;
    g_dev.features = (1ULL << 9) | (1ULL << 2);        // FLUSH, SEG_MAX
    uint64_t cap = g_sectors;
    for (int i = 0; i < 8; i++) g_dev.devcfg[i] = (uint8_t)(cap >> (8 * i));
    g_dev.devcfg[12] = VIRTIO_CHAIN_MAX - 2;            // seg_max
    g_dev.devcfg_len = 24;
    g_dev.notify = on_notify;
    g_have = VirtioRegister(&g_dev);
    SerialLogger::Log("[vblk] guest disk ");
    SerialLogger::LogDec((int)(bytes >> 20));
    if (g_nchunks) { SerialLogger::Log(" MB image + "); SerialLogger::LogDec((int)g_nchunks); SerialLogger::Log(" chunks of 64 MB ("); SerialLogger::LogDec((int)g_chunks_present); SerialLogger::Log(" present), virtual "); SerialLogger::LogDec((int)(g_sectors >> 11)); }
    SerialLogger::Log(" MB at nvme lba "); SerialLogger::LogHex64(lba);
    SerialLogger::Log(g_have ? "\r\n" : " (register failed)\r\n");
    return g_have;
}

bool VBlk::HasBacking() { return g_have; }
void VBlk::ChunkStats(uint32_t* present, uint32_t* total) { if (present) *present = g_chunks_present; if (total) *total = g_nchunks; }
uint64_t VBlk::Sectors() { return g_sectors; }
uint64_t VBlk::BytesRead() { return g_bytes_r; }
uint64_t VBlk::BytesWritten() { return g_bytes_w; }
void VBlk::Stats(uint32_t* r, uint32_t* w, uint32_t* f, uint32_t* e) {
    if (r) *r = g_reads;
    if (w) *w = g_writes;
    if (f) *f = g_flushes;
    if (e) *e = g_errors;
}

// end (satoru)
