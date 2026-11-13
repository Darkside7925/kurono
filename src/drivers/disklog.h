#pragma once
#include "../kernel/types.h"

// bare-metal boot log + write window. (satoru)
//
// FindKuronoWindow() reads the nvme gpt and, if a partition is literally named
// KURONO, makes it the ONLY range NVMe::Write accepts while the write guard is
// on. Init() then looks for a preallocated KURONO.LOG in that partition's
// fat32 root and streams SerialLogger's capture buffer into the file's own
// clusters. no fat, directory or size field is ever written, so the volume
// stays consistent and windows can just open the file. (satoru)
//
// whole session (satoru): the stream comes from SerialLogger's session ring
// (4 MB in memory), not the 256 KB linear capture. a file of 1 MB or more is
// split into a kept boot head (the first quarter) and a ring for the rest;
// once that ring wraps, a "===== [disklog] END OF NEWEST DATA ... head=N"
// line follows the newest byte. tools/disklog_unwrap.py puts it back in
// time order. the 1 s worker writes only the lbas that changed. (satoru)
namespace DiskLog {
    bool FindKuronoWindow();
    bool Init();
    // kurono.logname=NAME: write NAME.LOG (8.3, created ahead of time in the
    // KURONO root) instead of KURONO.LOG, so a test boot keeps its own log.
    // falls back to KURONO.LOG when the file is missing. call before Init. (satoru)
    void SetLogName(const char* base);
    bool Ready();
    // the preallocated contiguous KURONO.KFS extent (device lba, bytes), valid
    // after Init(). false if missing or fragmented; KfsStatus() says why. (satoru)
    bool KfsFile(uint64_t* lba, uint64_t* bytes);
    // guest vm disk images in the partition root (8.3 name, e.g. "ALPINE  IMG"):
    // 0 = found (extent out), 1 missing, 2 fragmented, 3 misaligned. (satoru)
    int  GuestImage(const char* name83, uint64_t* lba, uint64_t* bytes);
    // make that image writable (third nvme guard window) for a starting vm (satoru)
    bool ArmGuestImage(const char* name83, uint64_t* lba, uint64_t* bytes);
    void DisarmGuestImage();
    // chunked guest disk files NAME.000 .. NAME.999 (prefix8 = 8 space padded chars): find the
    // existing ones (lbas[i] = device lba or 0) / create one zero filled contiguous chunk.
    // create: 0 ok, 3 misaligned, 4 io, 6 full or no contiguous run, 7 exists. it clears the
    // third guard window, so the caller re-arms its image afterwards. (satoru)
    int  GuestChunkScan(const char* prefix8, uint32_t chunk_bytes, uint64_t* lbas, int max);
    int  CreateGuestChunk(const char* name83, uint32_t bytes, uint64_t* lba_out);
    uint32_t ClusterBytes();
    bool PartitionRange(uint64_t* lo, uint64_t* end);
    const char* KfsStatus();
    // read-only access to a named file in the KURONO fat32 root (8.3 name, 11
    // chars space padded). follows the fat chain, so a fragmented file works.
    // never writes. StatRootFile returns the exact byte size (an empty file
    // is present with size 0, callers that need data check sz > 0); ReadRootFile
    // streams the whole file into dst (identity mapped ram, cap bytes). both
    // return 0 on success; RootFileMsg(code) is a human readable reason. (satoru)
    typedef void (*RootFileProgress)(uint64_t done, uint64_t total);
    int StatRootFile(const char* name83, uint64_t* size);
    // read-only variant for the boot-time image-window policy: parse the fat32
    // geometry of the already-found KURONO partition and look up one 8.3 name,
    // without Init's blanking writes. safe on a guard-off bios boot. (satoru)
    int ProbeRootFile(const char* name83, uint64_t* size);
    int ReadRootFile(const char* name83, void* dst, uint64_t cap, RootFileProgress prog);
    // create one file in the KURONO fat32 root (8.3 name) from a kernel buffer:
    // allocates clusters + writes both FATs + the root dir entry under the
    // third nvme guard window. 0 ok, 1 no partition, 4 io, 6 full, 7 exists. (satoru)
    int  WriteRootFile(const char* name83, const void* data, uint32_t len);
    const char* RootFileMsg(int code);
    // preallocated in-place files (kcrash store). RootFileExtent looks a root
    // file up (boot time: it allocates its read buffers) and returns its device
    // lba, the whole-cluster byte span and the exact size; 0 ok, 1 no partition,
    // 2 missing, 4 io, 5 fragmented. WriteRootExtent overwrites whole lbas
    // inside such an extent: no allocation, no fat or directory write, so it is
    // safe on a failure path. len <= 64 KiB. true on success; false also when
    // another disk user holds the io gate (short try only: keep the data and
    // retry later). writes through nvme guard window 4, never the vm's. (satoru)
    int  RootFileExtent(const char* name83, uint64_t* lba, uint64_t* bytes, uint32_t* size);
    bool WriteRootExtent(uint64_t lba, const void* data, uint32_t len);
    uint32_t LbaSize();
    // short reason the log is / is not running (for the boot screen). (satoru)
    const char* Status();
    // write the captured log up to its current end. false when another flush
    // owns the log or the write failed, so a reset path can retry. (satoru)
    bool Flush();
    // eager: write every log line to disk as it is printed (early boot). (satoru)
    void SetEager(bool on);
    // a bounded best-effort flush from the panic path; may be called repeatedly
    // (each call writes what was logged since). (satoru)
    void PanicFlush();
    [[noreturn]] void WorkerEntry();
}

// end (satoru)
