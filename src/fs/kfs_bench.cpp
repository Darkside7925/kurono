#include "kfs_bench.h"
#include "kfs.h"
#include "../drivers/nvme.h"
#include "../drivers/serial.h"
#include "../drivers/cpu_detect.h"
#include "../kernel/pmm.h"
#include "../kernel/heap.h"
#include "../hal/hal.h"

//  kfs_bench - see kfs_bench.h. all timing is rdtsc-based for sub-ms precision;
//  MB/s + IOPS are computed in integer math (no fp - kernel is -mno-sse and the
//  compiler must not emit sse). (satoru)

namespace {
    //  block i/o backend wiring KFS to the nvme data disk (4 KB block -> LBA). a
    //  private copy so the bench doesn't depend on persist.cpp internals. (satoru)
    bool bench_rd(uint64_t block, uint32_t count, void* buf, void*) {
        uint32_t per = 4096u / NVMe::GetLBASize();
        return NVMe::Read(block * per, count * per, buf);
    }
    bool bench_wr(uint64_t block, uint32_t count, const void* buf, void*) {
        uint32_t per = 4096u / NVMe::GetLBASize();
        return NVMe::Write(block * per, count * per, buf);
    }

    uint64_t rdtsc() {
        uint32_t lo, hi;
        __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t)hi << 32) | lo;
    }
    uint64_t tsc_hz() {
        uint64_t f = CPUDetect::GetInfo().frequency.tsc_frequency;
        return f >= 1000000ull ? f : 1000000000ull;   // fall back to 1 GHz if uncalibrated (satoru)
    }

    //  log "<label>: <whole>.<frac3> <unit>" from a fixed-point value scaled x1000. (satoru)
    void log_fixed3(const char* label, uint64_t val_x1000, const char* unit) {
        SerialLogger::Log(label);
        SerialLogger::LogDec((int)(val_x1000 / 1000));
        SerialLogger::Log(".");
        int frac = (int)(val_x1000 % 1000);
        if (frac < 100) SerialLogger::Log("0");
        if (frac < 10)  SerialLogger::Log("0");
        SerialLogger::LogDec(frac);
        SerialLogger::Log(" "); SerialLogger::Log(unit); SerialLogger::Log("\r\n");
    }

    //  MB/s x1000 from bytes + elapsed tsc cycles. MB/s = bytes / (1e6) / seconds
    //  = bytes * hz / (1e6 * cycles). scale by 1000 for 3 decimals. all 64-bit. (satoru)
    uint64_t mbps_x1000(uint64_t bytes, uint64_t cycles, uint64_t hz) {
        if (cycles == 0) cycles = 1;
        // bytes * hz can overflow for big files; divide hz path carefully. use
        // microseconds: us = cycles * 1e6 / hz; MB/s = bytes / us. x1000 -> *1000. (satoru)
        uint64_t us = (cycles / (hz / 1000000ull ? hz / 1000000ull : 1));
        if (us == 0) us = 1;
        return (bytes * 1000ull) / us;   // (bytes/us) = MB/s; *1000 for 3 dp (satoru)
    }

    //  ms x1000 from cycles. (satoru)
    uint64_t ms_x1000(uint64_t cycles, uint64_t hz) {
        // ms = cycles * 1000 / hz; x1000 -> cycles * 1e6 / hz. (satoru)
        uint64_t per_ms = hz / 1000ull; if (!per_ms) per_ms = 1;
        return (cycles * 1000ull) / per_ms;
    }
}

bool KfsBench::Run() {
    if (!NVMe::IsDetected()) {
        SerialLogger::Log("[KFSBENCH] FAIL: no nvme data disk\r\n");
        return false;
    }
    uint64_t hz = tsc_hz();
    SerialLogger::Log("[KFSBENCH] begin (tsc hz=");
    SerialLogger::LogDec((int)(hz / 1000000ull)); SerialLogger::Log(" MHz)\r\n");

    KFS::SetBackend(bench_rd, bench_wr, nullptr);

    // size the bench volume to the data disk (cap so the metadata cache is sane). (satoru)
    uint64_t disk_blocks = NVMe::GetCapacityLBA() * (uint64_t)NVMe::GetLBASize() / KFS_BLOCK_SIZE;
    if (disk_blocks > 262144) disk_blocks = 262144;   // 1 GB of bench volume is plenty (satoru)
    if (disk_blocks < 64) { SerialLogger::Log("[KFSBENCH] FAIL: disk too small\r\n"); return false; }

    uint64_t t0 = rdtsc();
    bool fmt = KFS::Format((uint32_t)disk_blocks);
    uint64_t t1 = rdtsc();
    if (!fmt) { SerialLogger::Log("[KFSBENCH] FAIL: format\r\n"); return false; }
    log_fixed3("[KFSBENCH] format: ", ms_x1000(t1 - t0, hz), "ms");

    // ── sequential write: one large contiguous file through KFS ──────────────
    // 32 MB test payload (8192 blocks). built once in a heap buffer with a cheap
    // pattern so we can verify the read back. (satoru)
    const uint32_t SEQ_BYTES = 32u * 1024u * 1024u;
    uint8_t* payload = (uint8_t*)KernelHeap::Alloc(SEQ_BYTES);
    if (!payload) { SerialLogger::Log("[KFSBENCH] FAIL: alloc payload\r\n"); return false; }
    for (uint32_t i = 0; i < SEQ_BYTES; i++) payload[i] = (uint8_t)(i * 2654435761u >> 24);

    KFS::ResetStats();
    uint64_t wc0 = NVMe::GetWriteCount();
    uint64_t sw0 = rdtsc();
    bool wok = KFS::WriteFile("/home/bench/seq.bin", payload, SEQ_BYTES);
    bool sok = KFS::Sync();
    uint64_t sw1 = rdtsc();
    uint64_t seq_wr_cmds = NVMe::GetWriteCount() - wc0;
    if (!wok || !sok) { SerialLogger::Log("[KFSBENCH] FAIL: seq write\r\n"); KernelHeap::Free(payload); return false; }
    log_fixed3("[KFSBENCH] seq write MB/s: ", mbps_x1000(SEQ_BYTES, sw1 - sw0, hz), "MB/s");
    SerialLogger::Log("[KFSBENCH] seq write: "); SerialLogger::LogDec((int)(SEQ_BYTES / (1024*1024)));
    SerialLogger::Log(" MB in "); SerialLogger::LogDec((int)seq_wr_cmds);
    SerialLogger::Log(" nvme write cmds, "); SerialLogger::LogDec((int)KFS::Stats().extents_used);
    SerialLogger::Log(" extent(s) (ideal=1)\r\n");

    // ── sequential read: read the file back + verify ─────────────────────────
    uint8_t* rbuf = (uint8_t*)KernelHeap::Alloc(SEQ_BYTES);
    if (!rbuf) { SerialLogger::Log("[KFSBENCH] FAIL: alloc rbuf\r\n"); KernelHeap::Free(payload); return false; }
    uint64_t rc0 = NVMe::GetReadCount();
    uint64_t sr0 = rdtsc();
    int64_t got = KFS::ReadFile("/home/bench/seq.bin", rbuf, SEQ_BYTES);
    uint64_t sr1 = rdtsc();
    uint64_t seq_rd_cmds = NVMe::GetReadCount() - rc0;
    bool rverify = (got == (int64_t)SEQ_BYTES);
    if (rverify) for (uint32_t i = 0; i < SEQ_BYTES; i++) if (rbuf[i] != payload[i]) { rverify = false; break; }
    log_fixed3("[KFSBENCH] seq read MB/s: ", mbps_x1000(SEQ_BYTES, sr1 - sr0, hz), "MB/s");
    SerialLogger::Log("[KFSBENCH] seq read: "); SerialLogger::LogDec((int)(SEQ_BYTES / (1024*1024)));
    SerialLogger::Log(" MB in "); SerialLogger::LogDec((int)seq_rd_cmds);
    SerialLogger::Log(" nvme read cmds, verify="); SerialLogger::Log(rverify ? "OK" : "MISMATCH");
    SerialLogger::Log("\r\n");

    // ── random 4 KB write IOPS: raw nvme single-block writes at random LBAs ────
    // this is the primitive the write-back cache + log-structured layers batch.
    // measure it raw so we have an honest "before" for those layers. (satoru)
    const int RAND_OPS = 2000;
    uint8_t* blk = (uint8_t*)PMM::AllocBytes(4096);
    if (!blk) { SerialLogger::Log("[KFSBENCH] FAIL: alloc blk\r\n"); KernelHeap::Free(payload); KernelHeap::Free(rbuf); return false; }
    for (int i = 0; i < 4096; i++) blk[i] = (uint8_t)i;
    uint32_t lba_per_blk = 4096u / NVMe::GetLBASize();
    uint64_t cap_blocks = (NVMe::GetCapacityLBA() / lba_per_blk);
    if (cap_blocks < 1024) cap_blocks = 1024;
    uint32_t rng = 0x12345678u;
    uint64_t rw0 = rdtsc();
    for (int i = 0; i < RAND_OPS; i++) {
        rng = rng * 1103515245u + 12345u;
        uint64_t b = (rng % (cap_blocks - 1));
        NVMe::Write(b * lba_per_blk, lba_per_blk, blk);
    }
    NVMe::Flush();
    uint64_t rw1 = rdtsc();
    // IOPS = ops / seconds = ops * hz / cycles. (satoru)
    uint64_t cyc = rw1 - rw0; if (!cyc) cyc = 1;
    uint64_t rand_us = cyc / (hz / 1000000ull ? hz / 1000000ull : 1); if (!rand_us) rand_us = 1;
    uint64_t iops = ((uint64_t)RAND_OPS * 1000000ull) / rand_us;
    SerialLogger::Log("[KFSBENCH] random 4K write IOPS: "); SerialLogger::LogDec((int)iops);
    SerialLogger::Log(" ("); SerialLogger::LogDec(RAND_OPS); SerialLogger::Log(" ops in ");
    SerialLogger::LogDec((int)(rand_us / 1000)); SerialLogger::Log(" ms)\r\n");

    // ── inline tiny-file efficiency: 512 small files, expect 0 data blocks ────
    KFS::ResetStats();
    uint64_t iwc0 = NVMe::GetWriteCount();
    const char* tiny = "small file content under 184 bytes -> inline in inode, no data block";
    uint64_t tlen = 0; while (tiny[tlen]) tlen++;
    char* tinypath = (char*)KernelHeap::Alloc(64);
    for (int i = 0; i < 512; i++) {
        // build /home/bench/tiny/fNNN (satoru)
        int p = 0; const char* pre = "/home/bench/tiny/f";
        for (int k = 0; pre[k]; k++) tinypath[p++] = pre[k];
        int v = i; char d[8]; int dl = 0; if (v == 0) d[dl++] = '0'; while (v) { d[dl++] = (char)('0' + v % 10); v /= 10; }
        for (int k = dl - 1; k >= 0; k--) tinypath[p++] = d[k];
        tinypath[p] = 0;
        KFS::WriteFile(tinypath, tiny, tlen);
    }
    KFS::Sync();
    uint64_t inline_cmds = NVMe::GetWriteCount() - iwc0;
    SerialLogger::Log("[KFSBENCH] inline tiny files: 512 written, ");
    SerialLogger::LogDec((int)KFS::Stats().inline_files); SerialLogger::Log(" stored inline (no data block), ");
    SerialLogger::LogDec((int)KFS::Stats().extents_used); SerialLogger::Log(" data extents, ");
    SerialLogger::LogDec((int)inline_cmds); SerialLogger::Log(" nvme cmds total\r\n");
    KernelHeap::Free(tinypath);

    // ── snapshot save (full): format + mirror a representative tree ───────────
    // mirror the 32 MB file + the tiny files we already wrote by re-formatting and
    // re-writing them (simulating PersistStore::SaveTree). (satoru)
    uint64_t fwc0 = NVMe::GetWriteCount();
    uint64_t fs0 = rdtsc();
    KFS::Format((uint32_t)disk_blocks);
    KFS::WriteFile("/home/bench/seq.bin", payload, SEQ_BYTES);
    KFS::Sync();
    uint64_t fs1 = rdtsc();
    uint64_t full_cmds = NVMe::GetWriteCount() - fwc0;
    log_fixed3("[KFSBENCH] snapshot save (full, 32MB): ", ms_x1000(fs1 - fs0, hz), "ms");
    SerialLogger::Log("[KFSBENCH] full save nvme cmds: "); SerialLogger::LogDec((int)full_cmds); SerialLogger::Log("\r\n");

    // ── snapshot save (incremental ~ no change): just a metadata Sync ─────────
    uint64_t iwc1 = NVMe::GetWriteCount();
    uint64_t is0 = rdtsc();
    KFS::Sync();
    uint64_t is1 = rdtsc();
    uint64_t inc_cmds = NVMe::GetWriteCount() - iwc1;
    log_fixed3("[KFSBENCH] snapshot save (incremental/sync only): ", ms_x1000(is1 - is0, hz), "ms");
    SerialLogger::Log("[KFSBENCH] incremental nvme cmds: "); SerialLogger::LogDec((int)inc_cmds); SerialLogger::Log("\r\n");

    // ── boot restore time: mount + read the file back (the restore hot path) ──
    uint64_t br0 = rdtsc();
    bool mok = KFS::Mount();
    int64_t rgot = mok ? KFS::ReadFile("/home/bench/seq.bin", rbuf, SEQ_BYTES) : -1;
    uint64_t br1 = rdtsc();
    log_fixed3("[KFSBENCH] boot restore (mount+read 32MB): ", ms_x1000(br1 - br0, hz), "ms");
    SerialLogger::Log("[KFSBENCH] restore verify="); SerialLogger::Log(rgot == (int64_t)SEQ_BYTES ? "OK" : "FAIL"); SerialLogger::Log("\r\n");

    KernelHeap::Free(payload);
    KernelHeap::Free(rbuf);
    PMM::FreeBytes(blk, 4096);
    SerialLogger::Log("[KFSBENCH] end\r\n");
    return true;
}
// end (satoru)
