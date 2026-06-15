#pragma once
#include "types.h"

//  kurono os: udf, the user driver framework (kurono's answer to windows umdf).
//
//  the ring-3 tier of the hybrid-kernel architecture: a driver that does NOT need
//  ring-0 (wifi control plane, usb-hid, usb-storage, printer) runs as an ordinary
//  kinit-managed linux user process and reaches the hardware ONLY through a thin
//  kernel proxy. the user driver calls udf_call(op, params) -> the SYS_UDF_CALL
//  syscall -> the kernel UDFProxy marshals the request, performs the privileged
//  hardware op on the driver's behalf, and returns a result. if the ring-3 driver
//  dies, nothing in the kernel is corrupted: kinit's crash monitor restarts the
//  process and the proxy re-registers. a fully crashed driver just makes its
//  udf_call return -UDF_EDEAD until it is back.
//
//  this is the strongest isolation tier kurono offers: a udf driver is a real
//  separate address space at cpl 3, so a wild pointer or a crash cannot touch
//  kernel memory at all (unlike a kdf driver, which shares the kernel page tables
//  and is only fenced by guard pages). the tradeoff is a syscall round-trip per
//  call (~10-15us); perf-critical drivers stay in ring 0 / kdf. (satoru)

namespace UDF {

constexpr int UDF_MAX_PROXIES = 16;
constexpr int UDF_NAME_LEN    = 24;

// ── operation codes carried by SYS_UDF_CALL (satoru) ─────────────────────────
// op space is partitioned by device class. a proxy registers the class it serves
// and only ops in its class are dispatched to it. generic ops (0x00xx) are
// handled by the framework itself (register / ping / shared-buffer setup). (satoru)
enum UDFOp : uint32_t {
    // generic framework ops (handled in udf.cpp directly) (satoru)
    UDF_OP_REGISTER   = 0x0001,   // a user driver announces itself: a0=class, a1=name ptr (satoru)
    UDF_OP_UNREGISTER = 0x0002,   // graceful detach (satoru)
    UDF_OP_PING       = 0x0003,   // liveness heartbeat (satoru)
    UDF_OP_POLL       = 0x0004,   // pull the next queued request for this proxy (satoru)
    UDF_OP_COMPLETE   = 0x0005,   // post a completion for a previously-polled request (satoru)

    // wifi control-plane class (0x01xx). the data path stays in the kernel; the
    // user driver does scan/assoc/wpa handshakes and pushes results down. (satoru)
    UDF_OP_WIFI_SCAN     = 0x0101,
    UDF_OP_WIFI_CONNECT  = 0x0102,
    UDF_OP_WIFI_STATUS   = 0x0103,

    // usb-hid class (0x02xx): the user driver parses report descriptors + decodes
    // input reports, then pushes normalized events to the kernel input layer. (satoru)
    UDF_OP_HID_REPORT    = 0x0201,   // push a decoded hid input event (satoru)
    UDF_OP_HID_DESC      = 0x0202    // hand the kernel a parsed descriptor summary (satoru)
};

// device classes a proxy can serve. (satoru)
enum UDFClass : uint32_t {
    UDF_CLASS_NONE = 0,
    UDF_CLASS_WIFI = 1,
    UDF_CLASS_HID  = 2,
    UDF_CLASS_STORAGE = 3,
    UDF_CLASS_PRINTER = 4
};

// error codes returned in the syscall result (negative). (satoru)
enum UDFError : int64_t {
    UDF_OK        =  0,
    UDF_EDEAD     = -1,   // the target user driver is not registered / has died (satoru)
    UDF_EINVAL    = -2,   // bad op / class / args (satoru)
    UDF_ENOPROXY  = -3,   // no proxy serves this op's class (satoru)
    UDF_EAGAIN    = -4    // nothing to poll right now (satoru)
};

// ── lifecycle ────────────────────────────────────────────────────────────────
void Init();

// the kernel-side entry the SYS_UDF_CALL syscall routes to. `op` selects the
// operation; a0..a3 are the marshalled args (user pointers are validated before
// any dereference). returns a UDFError (<=0) or an op-specific non-negative
// result. runs at cpl 0 on behalf of the calling user process. (satoru)
int64_t Call(uint32_t op, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);

// ── proxy registry / introspection (satoru) ──────────────────────────────────
struct UDFProxyInfo {
    char     name[UDF_NAME_LEN];
    UDFClass cls;
    int      pid;            // owning ring-3 process pid (satoru)
    bool     alive;          // cleared when the process dies (satoru)
    uint32_t calls;          // total Call()s dispatched to it (satoru)
    uint32_t last_ping_ms;   // last UDF_OP_PING (satoru)
};

int  GetProxyCount();
bool GetProxyInfo(int idx, UDFProxyInfo* out);
// find the proxy serving a class, or -1. (satoru)
int  FindProxyByClass(UDFClass cls);

// kinit calls this when a udf-driver process exits so the proxy is marked dead
// (subsequent Call()s for its class return UDF_EDEAD until it re-registers).
// (satoru)
void NotifyProxyDied(int pid);

// render a status table (proxy, class, pid, alive, calls) into out. (satoru)
int  Status(char* out, int mx);

}  // namespace UDF

// end (satoru)
