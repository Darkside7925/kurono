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
namespace DiskLog {
    bool FindKuronoWindow();
    bool Init();
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
    const char* KfsStatus();
    // read-only access to a named file in the KURONO fat32 root (8.3 name, 11
    // chars space padded). follows the fat chain, so a fragmented file works.
    // never writes. StatRootFile returns the exact byte size (an empty file
    // is present with size 0, callers that need data check sz > 0); ReadRootFile
    // streams the whole file into dst (identity mapped ram, cap bytes). both
    // return 0 on success; RootFileMsg(code) is a human readable reason. (satoru)
    typedef void (*RootFileProgress)(uint64_t done, uint64_t total);
    int StatRootFile(const char* name83, uint64_t* size);
    int ReadRootFile(const char* name83, void* dst, uint64_t cap, RootFileProgress prog);
    const char* RootFileMsg(int code);
    // short reason the log is / is not running (for the boot screen). (satoru)
    const char* Status();
    void Flush();
    // eager: write every log line to disk as it is printed (early boot). (satoru)
    void SetEager(bool on);
    // one bounded best-effort flush from the panic path, before the reboot. (satoru)
    void PanicFlush();
    [[noreturn]] void WorkerEntry();
}

// end (satoru)
