#pragma once
#include "types.h"

//  kurono os: irp, the i/o request packet system (kurono's answer to windows'
//  IRP / IoCallDriver model).
//
//  instead of a driver calling another driver's read/write entry directly, an
//  i/o request is packaged into an IRP and posted to the executive dispatch
//  queue. the executive routes it to the target device's registered dispatch
//  routine; a device can forward the irp DOWN a driver stack (e.g. filesystem ->
//  volume -> disk) by re-posting it to the next device, and completion bubbles
//  back UP. an irp can be SYNCHRONOUS (the poster blocks until completion) or
//  ASYNC (the poster gets back an irp handle + polls / gets a callback).
//
//  this gives kurono the structured, stackable, async i/o path windows uses,
//  decoupling drivers from each other. HONEST SCOPE: the dispatch + stacking +
//  sync/async completion are real; it is a cooperative in-kernel queue (no
//  separate i/o worker threads beyond the existing scheduler), and only the
//  drivers that opt in (register a device + route through PostIrp) use it. the
//  legacy direct-call driver entries still work unchanged. (satoru)

namespace IRP {

constexpr int IRP_MAX_DEVICES = 32;
constexpr int IRP_MAX_INFLIGHT = 64;
constexpr int IRP_NAME_LEN    = 24;

// the major function codes (windows-style IRP_MJ_*). (satoru)
enum IrpMajor : uint8_t {
    IRP_MJ_READ = 0,
    IRP_MJ_WRITE,
    IRP_MJ_IOCTL,
    IRP_MJ_FLUSH,
    IRP_MJ_PNP,        // start/stop/query device (satoru)
    IRP_MJ_COUNT
};

// completion status. (satoru)
enum IrpStatus : int32_t {
    IRP_PENDING   = 1,     // queued / in flight (satoru)
    IRP_SUCCESS   = 0,
    IRP_EINVAL    = -2,
    IRP_ENODEV    = -3,    // no device / dispatch routine (satoru)
    IRP_EIO       = -5,    // device i/o error (satoru)
    IRP_EBUSY     = -11    // queue full (satoru)
};

struct Irp;

// a device's dispatch routine: handles one irp for `major`. it sets irp->status
// + irp->info (bytes transferred) and returns. to forward down the stack it calls
// IRP::Forward(irp, next_device) and returns IRP_PENDING. returns the resulting
// status (or IRP_PENDING if it forwarded / will complete later). (satoru)
typedef int32_t (*IrpDispatch)(Irp* irp);

// completion callback for an async irp. (satoru)
typedef void (*IrpCompletion)(Irp* irp, void* ctx);

// the i/o request packet. (satoru)
struct Irp {
    bool      in_use;
    int       id;
    IrpMajor  major;
    int       device;        // target device index (satoru)
    uint64_t  lba;           // read/write start block (or ioctl code) (satoru)
    uint32_t  count;         // block count (or ioctl arg) (satoru)
    void*     buffer;        // data buffer (caller-owned) (satoru)
    int32_t   status;        // IrpStatus; IRP_PENDING until completed (satoru)
    uint32_t  info;          // bytes transferred on completion (satoru)
    bool      async;         // false = poster blocks in PostIrp (satoru)
    IrpCompletion completion;
    void*     completion_ctx;
    int       stack_depth;   // how many times it has been forwarded (loop guard) (satoru)
};

// ── lifecycle ────────────────────────────────────────────────────────────────
void Init();

// register a device (a driver's dispatch target). returns a device index, or -1.
// (satoru)
int  RegisterDevice(const char* name, IrpDispatch dispatch);
int  FindDevice(const char* name);

// allocate + post an irp to a device. SYNC form blocks (cooperatively) until the
// irp completes and returns its final status; ASYNC form returns an irp id
// immediately (>=0) and the caller polls Completed()/Status() or gets a callback.
// (satoru)
int32_t PostSync(int device, IrpMajor major, uint64_t lba, uint32_t count, void* buffer);
int     PostAsync(int device, IrpMajor major, uint64_t lba, uint32_t count, void* buffer,
                  IrpCompletion cb, void* ctx);

// a dispatch routine calls this to forward an irp DOWN to the next device in the
// stack; the executive re-dispatches it there. returns the next device's status.
// (satoru)
int32_t Forward(Irp* irp, int next_device);

// a driver completes an irp it took ownership of (async path): sets status+info
// and fires the completion callback / unblocks a sync waiter. (satoru)
void Complete(Irp* irp, int32_t status, uint32_t info);

// async-irp introspection by id. (satoru)
bool    Completed(int irp_id);
int32_t StatusOf(int irp_id);
uint32_t InfoOf(int irp_id);
// release a completed async irp slot. (satoru)
void    Release(int irp_id);

// ── status (satoru) ───────────────────────────────────────────────────────────
int GetDeviceCount();
int InFlightCount();
int Status(char* out, int mx);

}  // namespace IRP

// end (satoru)
