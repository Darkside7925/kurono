#pragma once
#include "types.h"

//  KLD drm userspace interface, broker side (stage D5). The Linux ABI half is
//  src/linux/dri_dev.cpp; the wire format is kld/lx_emul/kimpl/lx_dri.h.
//
//  One container (the drm driver) serves /dev/dri for every process. A caller's
//  ioctl/open/mmap becomes a request in one of 32 slots of a shared ring; the
//  caller is parked (restartable, like poll) and a container worker runs the
//  upstream handler. When that handler touches the caller's memory the shim
//  posts a UACCESS op: the PARKED CALLER is woken and does the copy itself,
//  in its own address space, between its memory and the slot's bounce area.
//  The broker never walks another address space. (satoru)
//
//  end (satoru)

struct Process;

namespace KldDri {

// request outcomes seen by the caller thread
enum PollResult { POLL_WAIT = 0, POLL_DONE = 1, POLL_UACC = 2 };

struct UaReq {
    uint64_t ptr;
    uint32_t len;
    uint32_t dir;            // LXDRI_UA_*
    uint64_t bounce_phys;    // 4 pages, references held until UaFinish
};

struct Reply {
    int64_t  result;
    uint32_t nseg;
    uint32_t flags;
    uint32_t file;           // the request's file id (satoru)
    struct { uint32_t obj, first, npages, flags; } seg[64];
};

bool Attached();
void SetNoFlush(bool on);   // rig: skip the shootdown broadcast of the object path (satoru)

// ---- caller side ------------------------------------------------------------
// files: allocate/free a kernel file id for a minor; DEAD after container death.
int  FileAlloc(uint32_t minor);            // id or -errno
void FileFree(uint32_t id);                // before the open succeeded
void FileClose(uint32_t id);               // after: queues a release behind in-flight requests
bool FileAlive(uint32_t id);
uint32_t FileEvBytes(uint32_t id);
uint32_t FileMinor(uint32_t id);
bool MinorAvailable(uint32_t minor);

// requests. a task has at most one pending request.
int  FindByTask(Process* task);
int  FindBound(Process* task, uint32_t kind, uint32_t file, uint32_t cmd, uint64_t arg, uint64_t mm_off, uint64_t mm_len);
uint32_t ReqFile(int slot);
int  Submit(Process* task, uint64_t as, uint32_t kind, uint32_t file, uint32_t cmd,
            uint64_t arg, uint32_t pid, uint32_t tgid, uint32_t euid, uint32_t egid,
            uint32_t flags, uint64_t mm_off, uint64_t mm_len, uint32_t mm_prot, uint32_t mm_flags);
bool ReqReady(uint32_t slot);
int  Poll(int slot, UaReq* ua, Reply* rep);
void UaFinish(int slot, const UaReq* ua, int64_t not_copied);
void Abandon(int slot);
void Release(int slot);
void TaskGone(Process* task);

// ---- container side (broker ops) -------------------------------------------
int64_t Call(Process* cur, uint32_t op, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
void ContainerGone(uint64_t as);
void AsDestroyed(uint64_t as);
bool RangeIsContainerMapping(uint64_t as, uint64_t va, uint64_t len);

// ---- object frames (D5.3) ---------------------------------------------------
// resolve a segment list against live objects; takes one reference per frame
// for the caller's mapping. frames[] receives physical addresses.
int  ObjResolve(const Reply* rep, uint64_t max_pages, uint64_t* frames, uint64_t* n);
void FramesRelease(const uint64_t* frames, uint64_t n);

int  Status(char* out, int mx);

}  // namespace KldDri

// end (satoru)
