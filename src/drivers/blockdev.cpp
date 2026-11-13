#include "blockdev.h"
#include "serial.h"
#include "../kernel/heap.h"

// generic block-device registry - see blockdev.h for why this exists. (satoru)

BlockDevice BlockDev::s_devs[BLOCKDEV_MAX] = {};
int         BlockDev::s_count = 0;
uint32_t    BlockDev::s_gen = 1;
// statistics side table (BlockDevice layout is unchanged). (satoru)
static uint64_t s_reads[BLOCKDEV_MAX], s_sectors_read[BLOCKDEV_MAX];
static uint64_t s_writes[BLOCKDEV_MAX], s_sectors_written[BLOCKDEV_MAX];

namespace {

void bd_cpy(char* dst, const char* src, int cap) {
    int i = 0;
    if (!dst || cap <= 0) return;
    if (src) {
        for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    }
    dst[i] = 0;
}

bool bd_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

// append a decimal number to out at *pos, bounded. (satoru)
void bd_num(char* out, int* pos, int max, uint64_t v) {
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && *pos < max - 1) out[(*pos)++] = tmp[--n];
}

void bd_str(char* out, int* pos, int max, const char* s) {
    while (s && *s && *pos < max - 1) out[(*pos)++] = *s++;
}

}  // namespace

void BlockDev::Init() {
    for (int i = 0; i < BLOCKDEV_MAX; i++) s_devs[i].present = false;
    s_count = 0;
    s_gen++;
}

uint32_t BlockDev::Generation() { return s_gen; }

void BlockDev::Unregister(const char* name) {
    for (int i = 0; i < s_count; i++) {
        if (!s_devs[i].present || !bd_eq(s_devs[i].name, name)) continue;
        for (int j = i; j + 1 < s_count; j++) s_devs[j] = s_devs[j + 1];
        s_count--;
        s_devs[s_count].present = false;
        s_gen++;
        SerialLogger::Log("[blockdev] -");
        SerialLogger::Log(name);
        SerialLogger::Log("\r\n");
        return;
    }
}

int BlockDev::Register(const BlockDevice& dev) {
    // reject entries that would fault or divide-by-zero later. a driver that
    // half-initialised a port must not land in the table. (satoru)
    if (!dev.read || dev.sector_size == 0) return -1;
    if (s_count >= BLOCKDEV_MAX) {
        SerialLogger::Log("[blockdev] table full, dropping ");
        SerialLogger::Log(dev.name);
        SerialLogger::Log("\r\n");
        return -1;
    }
    int idx = s_count++;
    s_devs[idx] = dev;
    s_devs[idx].present = true;
    s_gen++;

    SerialLogger::Log("[blockdev] +");
    SerialLogger::LogDec(idx);
    SerialLogger::Log(" ");
    SerialLogger::Log(dev.name);
    SerialLogger::Log(" driver=");
    SerialLogger::Log(dev.driver);
    SerialLogger::Log(" sectors=");
    // capacity_lba is 64-bit; printing it through int() wrapped large disks to
    // a negative count in the boot log. (satoru)
    {
        char capbuf[24];
        int cp = 0;
        bd_num(capbuf, &cp, sizeof(capbuf), dev.capacity_lba);
        SerialLogger::Log(capbuf);
    }
    SerialLogger::Log(" ssz=");
    SerialLogger::LogDec((int)dev.sector_size);
    SerialLogger::Log("\r\n");
    return idx;
}

int BlockDev::Count() { return s_count; }

BlockDevice* BlockDev::Get(int idx) {
    if (idx < 0 || idx >= s_count) return nullptr;
    if (!s_devs[idx].present) return nullptr;
    return &s_devs[idx];
}

BlockDevice* BlockDev::FindByName(const char* name) {
    for (int i = 0; i < s_count; i++) {
        if (s_devs[i].present && bd_eq(s_devs[i].name, name)) return &s_devs[i];
    }
    return nullptr;
}

bool BlockDev::Read(int idx, uint64_t lba, uint32_t count, void* buf) {
    BlockDevice* d = Get(idx);
    if (!d || !buf || count == 0) return false;
    // a gpt scanner walking a truncated/garbage table will run off the end of
    // the disk; return a clean false rather than letting the controller decide.
    // (satoru)
    if (lba >= d->capacity_lba) return false;
    if (lba + count > d->capacity_lba) return false;
    bool ok = d->read(d->ctx, lba, count, buf);
    if (ok) {
        s_reads[idx]++;
        s_sectors_read[idx] += count * (d->sector_size ? d->sector_size / 512u : 1u);
    }
    return ok;
}

bool BlockDev::Write(int idx, uint64_t lba, uint32_t count, const void* buf) {
    BlockDevice* d = Get(idx);
    if (!d || !buf || count == 0) return false;
    if (!d->write) return false;                  // read-only device (satoru)
    if (lba >= d->capacity_lba) return false;
    if (lba + count > d->capacity_lba) return false;
    bool ok = d->write(d->ctx, lba, count, buf);
    if (ok) {
        s_writes[idx]++;
        s_sectors_written[idx] += count * (d->sector_size ? d->sector_size / 512u : 1u);
    }
    return ok;
}

bool BlockDev::Flush(int idx) {
    BlockDevice* d = Get(idx);
    if (!d) return false;
    if (!d->flush) return true;                   // nothing to do (satoru)
    return d->flush(d->ctx);
}

bool BlockDev::ReadBytes(int idx, uint64_t byte_off, uint32_t len, void* buf) {
    BlockDevice* d = Get(idx);
    if (!d || !buf || len == 0) return false;
    const uint32_t ssz = d->sector_size;

    uint64_t first = byte_off / ssz;
    uint64_t last  = (byte_off + len - 1) / ssz;
    uint32_t nsec  = (uint32_t)(last - first + 1);

    // fast path: already sector aligned and a whole number of sectors, so read
    // straight into the caller's buffer with no bounce. (satoru)
    if ((byte_off % ssz) == 0 && (len % ssz) == 0) {
        return Read(idx, first, len / ssz, buf);
    }

    uint8_t* bounce = (uint8_t*)KernelHeap::Alloc((uint64_t)nsec * ssz);
    if (!bounce) return false;
    bool ok = Read(idx, first, nsec, bounce);
    if (ok) {
        uint32_t skew = (uint32_t)(byte_off - first * ssz);
        uint8_t* dst = (uint8_t*)buf;
        for (uint32_t i = 0; i < len; i++) dst[i] = bounce[skew + i];
    }
    KernelHeap::Free(bounce);
    return ok;
}

bool BlockDev::WriteBytes(int idx, uint64_t byte_off, uint32_t len, const void* buf) {
    BlockDevice* d = Get(idx);
    if (!d || !buf || len == 0) return false;
    if (!d->write) return false;
    const uint32_t ssz = d->sector_size;

    uint64_t first = byte_off / ssz;
    uint64_t last  = (byte_off + len - 1) / ssz;
    uint32_t nsec  = (uint32_t)(last - first + 1);

    if ((byte_off % ssz) == 0 && (len % ssz) == 0) {
        return Write(idx, first, len / ssz, buf);
    }

    // unaligned write is read-modify-write: pull the straddled sectors, patch
    // the middle, put them back. skipping the read would zero the bytes either
    // side of the range, which on a partition table or a superblock is exactly
    // the kind of damage there is no recovering from. (satoru)
    uint8_t* bounce = (uint8_t*)KernelHeap::Alloc((uint64_t)nsec * ssz);
    if (!bounce) return false;
    bool ok = Read(idx, first, nsec, bounce);
    if (ok) {
        uint32_t skew = (uint32_t)(byte_off - first * ssz);
        const uint8_t* src = (const uint8_t*)buf;
        for (uint32_t i = 0; i < len; i++) bounce[skew + i] = src[i];
        ok = Write(idx, first, nsec, bounce);
    }
    KernelHeap::Free(bounce);
    return ok;
}

void BlockDev::GetStats(int idx, uint64_t* reads, uint64_t* sectors_read,
                        uint64_t* writes, uint64_t* sectors_written) {
    if (reads) *reads = (idx >= 0 && idx < BLOCKDEV_MAX) ? s_reads[idx] : 0;
    if (sectors_read) *sectors_read = (idx >= 0 && idx < BLOCKDEV_MAX) ? s_sectors_read[idx] : 0;
    if (writes) *writes = (idx >= 0 && idx < BLOCKDEV_MAX) ? s_writes[idx] : 0;
    if (sectors_written) *sectors_written = (idx >= 0 && idx < BLOCKDEV_MAX) ? s_sectors_written[idx] : 0;
}

void BlockDev::Describe(int idx, char* out, int max_len) {
    if (!out || max_len <= 0) return;
    out[0] = 0;
    BlockDevice* d = Get(idx);
    if (!d) return;

    int p = 0;
    bd_str(out, &p, max_len, d->name);
    bd_str(out, &p, max_len, "  ");
    bd_str(out, &p, max_len, d->driver);
    bd_str(out, &p, max_len, "  ");

    // size in MiB - integer only, the kernel is built -mno-sse. (satoru)
    uint64_t mib = (d->capacity_lba * d->sector_size) / (1024ULL * 1024ULL);
    if (mib >= 1024) {
        bd_num(out, &p, max_len, mib / 1024);
        bd_str(out, &p, max_len, " GiB");
    } else {
        bd_num(out, &p, max_len, mib);
        bd_str(out, &p, max_len, " MiB");
    }

    bd_str(out, &p, max_len, "  ");
    bd_str(out, &p, max_len, d->model[0] ? d->model : "(no model)");
    if (d->removable) bd_str(out, &p, max_len, " [removable]");
    out[p < max_len ? p : max_len - 1] = 0;
}

// keep bd_cpy reachable for drivers that include this TU pattern later; it is
// the only helper they need and duplicating it per driver invites drift. (satoru)
extern "C" void blockdev_name_copy(char* dst, const char* src, int cap) {
    bd_cpy(dst, src, cap);
}

// end (satoru)
