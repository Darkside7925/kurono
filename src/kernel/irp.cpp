//  kurono os: irp, the i/o request packet system implementation.
//
//  see irp.h. the executive keeps a device table (name + dispatch routine) and a
//  pool of irps. PostSync packages a request, dispatches it to the device's
//  routine, and (if the routine returned IRP_PENDING) cooperatively waits for
//  completion. a routine forwards DOWN the stack via Forward (re-dispatch to the
//  next device); Complete bubbles the result back UP, firing callbacks /
//  unblocking the sync waiter. (satoru)

#include "irp.h"
#include "../drivers/serial.h"
#include "../proc/scheduler.h"

namespace IRP {

namespace {

int i_cat(char* o, int p, int mx, const char* s) {
    while (s && *s && p < mx - 1) o[p++] = *s++;
    if (p < mx) o[p] = 0;
    return p;
}
int i_cat_u(char* o, int p, int mx, uint64_t v) {
    char t[20]; int n = 0;
    if (v == 0) t[n++] = '0';
    else while (v && n < 20) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p < mx - 1) o[p++] = t[--n];
    if (p < mx) o[p] = 0;
    return p;
}
bool i_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}
void i_cpy(char* d, const char* s, int mx) {
    int i = 0; while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; } d[i] = 0;
}

struct Device {
    bool        in_use;
    char        name[IRP_NAME_LEN];
    IrpDispatch dispatch;
};

Device g_dev[IRP_MAX_DEVICES];
int    g_dev_count = 0;
Irp    g_irp[IRP_MAX_INFLIGHT];
int    g_next_id = 1;
bool   g_inited = false;

constexpr int IRP_MAX_STACK = 8;   // forward-depth loop guard (satoru)

Irp* alloc_irp() {
    for (int i = 0; i < IRP_MAX_INFLIGHT; i++) {
        if (!g_irp[i].in_use) {
            Irp* p = &g_irp[i];
            for (int b = 0; b < (int)sizeof(Irp); b++) ((char*)p)[b] = 0;
            p->in_use = true;
            p->id = g_next_id++;
            p->status = IRP_PENDING;
            return p;
        }
    }
    return nullptr;
}

Irp* find_irp(int id) {
    for (int i = 0; i < IRP_MAX_INFLIGHT; i++)
        if (g_irp[i].in_use && g_irp[i].id == id) return &g_irp[i];
    return nullptr;
}

// dispatch an irp to its current target device's routine. (satoru)
int32_t dispatch_to_device(Irp* irp) {
    if (irp->device < 0 || irp->device >= g_dev_count) { irp->status = IRP_ENODEV; return IRP_ENODEV; }
    Device* d = &g_dev[irp->device];
    if (!d->in_use || !d->dispatch) { irp->status = IRP_ENODEV; return IRP_ENODEV; }
    return d->dispatch(irp);
}

}  // namespace

void Init() {
    if (g_inited) return;
    for (int i = 0; i < IRP_MAX_DEVICES; i++)
        for (int b = 0; b < (int)sizeof(Device); b++) ((char*)&g_dev[i])[b] = 0;
    for (int i = 0; i < IRP_MAX_INFLIGHT; i++) g_irp[i].in_use = false;
    g_dev_count = 0;
    g_next_id = 1;
    g_inited = true;
    SerialLogger::Log("[IRP] i/o request packet executive initialized\r\n");
}

int RegisterDevice(const char* name, IrpDispatch dispatch) {
    if (!g_inited) Init();
    if (!name || !dispatch) return -1;
    if (FindDevice(name) >= 0) return FindDevice(name);
    if (g_dev_count >= IRP_MAX_DEVICES) return -1;
    int id = g_dev_count++;
    g_dev[id].in_use = true;
    i_cpy(g_dev[id].name, name, sizeof(g_dev[id].name));
    g_dev[id].dispatch = dispatch;
    char m[64]; int n = 0;
    n = i_cat(m, n, sizeof(m), "[IRP] device registered: ");
    n = i_cat(m, n, sizeof(m), name);
    n = i_cat(m, n, sizeof(m), "\r\n");
    SerialLogger::Log(m);
    return id;
}

int FindDevice(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < g_dev_count; i++)
        if (g_dev[i].in_use && i_eq(g_dev[i].name, name)) return i;
    return -1;
}

int32_t PostSync(int device, IrpMajor major, uint64_t lba, uint32_t count, void* buffer) {
    if (!g_inited) Init();
    Irp* irp = alloc_irp();
    if (!irp) return IRP_EBUSY;
    irp->major  = major;
    irp->device = device;
    irp->lba    = lba;
    irp->count  = count;
    irp->buffer = buffer;
    irp->async  = false;

    int32_t st = dispatch_to_device(irp);
    if (st != IRP_PENDING) {
        // the routine completed synchronously. (satoru)
        if (irp->status == IRP_PENDING) irp->status = st;
        int32_t result = irp->status;
        irp->in_use = false;
        return result;
    }
    // pending: cooperatively wait for Complete() to set the status. bounded so a
    // stuck driver can never wedge the caller forever. (satoru)
    for (int i = 0; i < 50000 && irp->status == IRP_PENDING; i++)
        Scheduler::SleepMs(1);
    int32_t result = (irp->status == IRP_PENDING) ? IRP_EIO : irp->status;
    irp->in_use = false;
    return result;
}

int PostAsync(int device, IrpMajor major, uint64_t lba, uint32_t count, void* buffer,
              IrpCompletion cb, void* ctx) {
    if (!g_inited) Init();
    Irp* irp = alloc_irp();
    if (!irp) return -1;
    irp->major  = major;
    irp->device = device;
    irp->lba    = lba;
    irp->count  = count;
    irp->buffer = buffer;
    irp->async  = true;
    irp->completion = cb;
    irp->completion_ctx = ctx;
    int id = irp->id;

    int32_t st = dispatch_to_device(irp);
    if (st != IRP_PENDING && irp->status == IRP_PENDING) {
        // completed synchronously inside dispatch: fire the callback now. (satoru)
        Complete(irp, st, irp->info);
    }
    return id;
}

int32_t Forward(Irp* irp, int next_device) {
    if (!irp) return IRP_EINVAL;
    if (irp->stack_depth >= IRP_MAX_STACK) { irp->status = IRP_EIO; return IRP_EIO; }
    irp->stack_depth++;
    irp->device = next_device;
    return dispatch_to_device(irp);
}

void Complete(Irp* irp, int32_t status, uint32_t info) {
    if (!irp) return;
    irp->status = status;
    irp->info   = info;
    if (irp->async && irp->completion) {
        irp->completion(irp, irp->completion_ctx);
        // async completed irps stay in_use until Release() so the poster can read
        // status/info by id; the callback may also Release it. (satoru)
    }
    // a sync waiter sees irp->status flip out of IRP_PENDING and returns. (satoru)
}

bool Completed(int irp_id) {
    Irp* p = find_irp(irp_id);
    return p ? (p->status != IRP_PENDING) : true;   // unknown id = treat as done (satoru)
}
int32_t StatusOf(int irp_id) {
    Irp* p = find_irp(irp_id);
    return p ? p->status : IRP_EINVAL;
}
uint32_t InfoOf(int irp_id) {
    Irp* p = find_irp(irp_id);
    return p ? p->info : 0;
}
void Release(int irp_id) {
    Irp* p = find_irp(irp_id);
    if (p) p->in_use = false;
}

int GetDeviceCount() { return g_dev_count; }
int InFlightCount() {
    int n = 0;
    for (int i = 0; i < IRP_MAX_INFLIGHT; i++) if (g_irp[i].in_use) n++;
    return n;
}

int Status(char* out, int mx) {
    if (!out || mx < 2) return 0;
    int p = 0;
    p = i_cat(out, p, mx, "IRP executive (stackable async i/o):\n  devices: ");
    for (int i = 0; i < g_dev_count; i++) {
        if (!g_dev[i].in_use) continue;
        p = i_cat(out, p, mx, g_dev[i].name);
        p = i_cat(out, p, mx, " ");
    }
    p = i_cat(out, p, mx, "\n  in-flight irps: ");
    p = i_cat_u(out, p, mx, (uint64_t)InFlightCount());
    p = i_cat(out, p, mx, "\n");
    return p;
}

}  // namespace IRP

// end (satoru)
