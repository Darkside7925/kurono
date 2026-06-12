#include "persist.h"
#include "../drivers/nvme.h"
#include "../kernel/pmm.h"
#include "../drivers/serial.h"

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

    const uint32_t per   = lbas_per_page();
    const uint64_t data_lba = KPS_DATA_OFF / NVMe::GetLBASize();  // 4096/512 = 8 (satoru)

    uint8_t* bounce = (uint8_t*)PMM::AllocBytes(4096);
    if (!bounce) return false;

    // write the blob in page-sized chunks; the final short chunk is zero-padded
    // out to a full page so the trailing sector is clean. (satoru)
    bool ok = true;
    uint32_t off = 0;
    uint64_t lba = data_lba;
    while (off < len) {
        uint32_t chunk = len - off;
        if (chunk > 4096) chunk = 4096;
        memset(bounce, 0, 4096);
        memcpy(bounce, blob + off, chunk);
        if (!NVMe::Write(lba, per, bounce)) { ok = false; break; }
        off += chunk;
        lba += per;
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

    PMM::FreeBytes(bounce, 4096);
    if (ok) {
        SerialLogger::Log("[persist] saved "); SerialLogger::LogDec((int)len);
        SerialLogger::Log(" bytes to raw nvme store\r\n");
    }
    return ok;
}

bool PersistStore::Load(uint8_t* buf, uint32_t maxlen, uint32_t* out_len) {
    if (out_len) *out_len = 0;
    if (!NVMe::IsDetected() || !buf) return false;

    const uint32_t per   = lbas_per_page();
    const uint64_t data_lba = KPS_DATA_OFF / NVMe::GetLBASize();

    uint8_t* bounce = (uint8_t*)PMM::AllocBytes(4096);
    if (!bounce) return false;

    // read + validate the header. a fresh / foreign disk fails the magic check. (satoru)
    if (!NVMe::Read(KPS_HDR_LBA, per, bounce)) { PMM::FreeBytes(bounce, 4096); return false; }
    KpsHeader hdr;
    memcpy(&hdr, bounce, sizeof(hdr));
    if (hdr.magic != KPS_MAGIC || hdr.length == 0 || hdr.length > maxlen) {
        PMM::FreeBytes(bounce, 4096);
        return false;
    }

    // read the blob back in page-sized chunks. (satoru)
    bool ok = true;
    uint32_t off = 0;
    uint64_t lba = data_lba;
    while (off < hdr.length) {
        if (!NVMe::Read(lba, per, bounce)) { ok = false; break; }
        uint32_t chunk = hdr.length - off;
        if (chunk > 4096) chunk = 4096;
        memcpy(buf + off, bounce, chunk);
        off += chunk;
        lba += per;
    }
    PMM::FreeBytes(bounce, 4096);
    if (!ok) return false;

    // reject a torn / corrupt blob rather than feeding garbage to deserialize. (satoru)
    if (crc32(buf, hdr.length) != hdr.crc) return false;

    if (out_len) *out_len = hdr.length;
    SerialLogger::Log("[persist] loaded "); SerialLogger::LogDec((int)hdr.length);
    SerialLogger::Log(" bytes from raw nvme store\r\n");
    return true;
}
// end (satoru)
