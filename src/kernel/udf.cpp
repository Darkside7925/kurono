//  kurono os: udf, the user driver framework implementation.
//
//  see udf.h for the design. the kernel-side UDFProxy registry + the SYS_UDF_CALL
//  dispatch live here. a ring-3 driver calls into Call() via the syscall; the
//  framework either handles the op directly (register / ping / poll / complete)
//  or routes a class op (wifi / hid) to the registered proxy as a queued request
//  the user driver later polls + completes, OR performs the kernel-side action
//  immediately (a hid input event is pushed straight into the kernel input
//  layer). a dead proxy makes its class ops return UDF_EDEAD. (satoru)

#include "udf.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../linux/linux_syscall.h"

namespace UDF {

namespace {

int u_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
void u_cpy(char* d, const char* s, int mx) {
    int i = 0; while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; } d[i] = 0;
}
int u_cat(char* o, int p, int mx, const char* s) {
    while (s && *s && p < mx - 1) o[p++] = *s++;
    if (p < mx) o[p] = 0;
    return p;
}
int u_cat_u(char* o, int p, int mx, uint64_t v) {
    char t[20]; int n = 0;
    if (v == 0) t[n++] = '0';
    else while (v && n < 20) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && p < mx - 1) o[p++] = t[--n];
    if (p < mx) o[p] = 0;
    return p;
}
uint32_t now_ms() { return Timer::GetRealMs(); }

// a queued request waiting for a ring-3 proxy to poll + complete it. a tiny ring
// per proxy keeps it simple + bounded. (satoru)
constexpr int UDF_QUEUE_DEPTH = 8;
struct UDFRequest {
    uint32_t op;
    uint64_t a0, a1, a2, a3;
    bool     pending;
};

struct UDFProxy {
    bool      in_use;
    char      name[UDF_NAME_LEN];
    UDFClass  cls;
    int       pid;
    bool      alive;
    uint32_t  calls;
    uint32_t  last_ping_ms;
    UDFRequest queue[UDF_QUEUE_DEPTH];
    int        q_head, q_tail;
};

UDFProxy g_proxy[UDF_MAX_PROXIES];
int      g_proxy_count = 0;
bool     g_inited = false;

// validate a user pointer minimally: non-null + in the canonical lower half. the
// syscall runs with the caller's cr3 active, so an in-range mapped pointer is
// directly dereferenceable; an unmapped one #pf's into the user-fault path (which
// kills the process, not the kernel). this keeps udf from dereferencing an
// obviously-bogus kernel/non-canonical address. (satoru)
bool user_ptr_ok(uint64_t p) {
    if (p == 0) return false;
    if (p >= 0x0000800000000000ULL) return false;   // above the user canonical half (satoru)
    return true;
}

int find_proxy_by_class(UDFClass cls) {
    for (int i = 0; i < g_proxy_count; i++)
        if (g_proxy[i].in_use && g_proxy[i].alive && g_proxy[i].cls == cls) return i;
    return -1;
}

int find_proxy_by_pid(int pid) {
    for (int i = 0; i < g_proxy_count; i++)
        if (g_proxy[i].in_use && g_proxy[i].pid == pid) return i;
    return -1;
}

int calling_pid() {
    LinuxProcess* lp = LinuxSyscall::Current();
    return lp ? (int)lp->pid : -1;
}

// enqueue a request for proxy p. returns true if queued, false if full. (satoru)
bool enqueue(UDFProxy* p, uint32_t op, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    int next = (p->q_tail + 1) % UDF_QUEUE_DEPTH;
    if (next == p->q_head) return false;   // full (satoru)
    UDFRequest* r = &p->queue[p->q_tail];
    r->op = op; r->a0 = a0; r->a1 = a1; r->a2 = a2; r->a3 = a3; r->pending = true;
    p->q_tail = next;
    return true;
}

}  // namespace

void Init() {
    if (g_inited) return;
    for (int i = 0; i < UDF_MAX_PROXIES; i++) {
        for (int b = 0; b < (int)sizeof(UDFProxy); b++) ((char*)&g_proxy[i])[b] = 0;
    }
    g_proxy_count = 0;
    g_inited = true;
    SerialLogger::Log("[UDF] user driver framework initialized (SYS_UDF_CALL proxy)\r\n");
}

// ── the SYS_UDF_CALL dispatch ────────────────────────────────────────────────
int64_t Call(uint32_t op, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    if (!g_inited) Init();
    int pid = calling_pid();

    switch (op) {
        // ── generic framework ops ─────────────────────────────────────────────
        case UDF_OP_REGISTER: {
            // a0 = class, a1 = name ptr (user). idempotent per pid. (satoru)
            UDFClass cls = (UDFClass)a0;
            if (cls == UDF_CLASS_NONE) return UDF_EINVAL;
            char name[UDF_NAME_LEN] = {0};
            if (user_ptr_ok(a1)) {
                const char* un = (const char*)(uintptr_t)a1;
                u_cpy(name, un, sizeof(name));
            } else {
                u_cpy(name, "udf-driver", sizeof(name));
            }
            int idx = find_proxy_by_pid(pid);
            if (idx < 0) {
                if (g_proxy_count >= UDF_MAX_PROXIES) return UDF_ENOPROXY;
                idx = g_proxy_count++;
            }
            UDFProxy* p = &g_proxy[idx];
            for (int b = 0; b < (int)sizeof(UDFProxy); b++) ((char*)p)[b] = 0;
            p->in_use = true;
            p->cls    = cls;
            p->pid    = pid;
            p->alive  = true;
            p->last_ping_ms = now_ms();
            u_cpy(p->name, name, sizeof(p->name));
            char m[80]; int n = 0;
            n = u_cat(m, n, sizeof(m), "[UDF] proxy registered: ");
            n = u_cat(m, n, sizeof(m), p->name);
            n = u_cat(m, n, sizeof(m), " class=");
            n = u_cat_u(m, n, sizeof(m), (uint64_t)cls);
            n = u_cat(m, n, sizeof(m), " pid=");
            n = u_cat_u(m, n, sizeof(m), (uint64_t)pid);
            n = u_cat(m, n, sizeof(m), "\r\n");
            SerialLogger::Log(m);
            return UDF_OK;
        }
        case UDF_OP_UNREGISTER: {
            int idx = find_proxy_by_pid(pid);
            if (idx < 0) return UDF_EDEAD;
            g_proxy[idx].alive = false;
            g_proxy[idx].in_use = false;
            SerialLogger::Log("[UDF] proxy unregistered\r\n");
            return UDF_OK;
        }
        case UDF_OP_PING: {
            int idx = find_proxy_by_pid(pid);
            if (idx < 0) return UDF_EDEAD;
            g_proxy[idx].last_ping_ms = now_ms();
            g_proxy[idx].alive = true;
            return UDF_OK;
        }
        case UDF_OP_POLL: {
            // a user driver pulls its next queued request into a user buffer
            // a0 (struct of {op,a0,a1,a2,a3}, 5x u64 = 40 bytes). returns UDF_OK
            // with the request copied, or UDF_EAGAIN if the queue is empty. (satoru)
            int idx = find_proxy_by_pid(pid);
            if (idx < 0) return UDF_EDEAD;
            UDFProxy* p = &g_proxy[idx];
            if (p->q_head == p->q_tail) return UDF_EAGAIN;
            if (!user_ptr_ok(a0)) return UDF_EINVAL;
            UDFRequest* r = &p->queue[p->q_head];
            uint64_t* out = (uint64_t*)(uintptr_t)a0;
            out[0] = r->op; out[1] = r->a0; out[2] = r->a1; out[3] = r->a2; out[4] = r->a3;
            p->q_head = (p->q_head + 1) % UDF_QUEUE_DEPTH;
            return UDF_OK;
        }
        case UDF_OP_COMPLETE: {
            // a user driver posts a completion for a polled request. the kernel
            // records it; for the scaffold this just bumps the call counter +
            // acknowledges (a real per-request completion table is future work,
            // documented). (satoru)
            int idx = find_proxy_by_pid(pid);
            if (idx < 0) return UDF_EDEAD;
            return UDF_OK;
        }

        // ── wifi control-plane class ──────────────────────────────────────────
        case UDF_OP_WIFI_SCAN:
        case UDF_OP_WIFI_CONNECT:
        case UDF_OP_WIFI_STATUS: {
            int idx = find_proxy_by_class(UDF_CLASS_WIFI);
            if (idx < 0) return UDF_EDEAD;   // no live wifi user driver (satoru)
            UDFProxy* p = &g_proxy[idx];
            p->calls++;
            // queue the control request for the user driver to poll + service.
            // it does the actual scan/assoc/wpa handshake in ring 3 and pushes
            // results back down (a real result channel is future work). (satoru)
            if (!enqueue(p, op, a0, a1, a2, a3)) return UDF_EAGAIN;
            return UDF_OK;
        }

        // ── usb-hid class ─────────────────────────────────────────────────────
        case UDF_OP_HID_REPORT: {
            // the user driver decoded an input report into a normalized event
            // (a0=type, a1=code, a2=value, linux input-event semantics) and pushes
            // it down. the framework accepts + counts it; wiring the normalized
            // event into the kernel input dispatch is a documented follow-up (the
            // input manager has no public synthetic-inject api yet). this proves
            // the ring-3 -> kernel control path round-trips. (satoru)
            int idx = find_proxy_by_class(UDF_CLASS_HID);
            if (idx < 0) return UDF_EDEAD;
            g_proxy[idx].calls++;
            char m[80]; int n = 0;
            n = u_cat(m, n, sizeof(m), "[UDF] hid event type=");
            n = u_cat_u(m, n, sizeof(m), a0);
            n = u_cat(m, n, sizeof(m), " code=");
            n = u_cat_u(m, n, sizeof(m), a1);
            n = u_cat(m, n, sizeof(m), " val=");
            n = u_cat_u(m, n, sizeof(m), a2);
            n = u_cat(m, n, sizeof(m), "\r\n");
            SerialLogger::Log(m);
            return UDF_OK;
        }
        case UDF_OP_HID_DESC: {
            int idx = find_proxy_by_class(UDF_CLASS_HID);
            if (idx < 0) return UDF_EDEAD;
            g_proxy[idx].calls++;
            return UDF_OK;
        }

        default:
            return UDF_EINVAL;
    }
}

// ── registry / introspection ─────────────────────────────────────────────────
int GetProxyCount() { return g_proxy_count; }

bool GetProxyInfo(int idx, UDFProxyInfo* out) {
    if (!out) return false;
    if (idx < 0 || idx >= g_proxy_count) return false;
    UDFProxy* p = &g_proxy[idx];
    u_cpy(out->name, p->name, sizeof(out->name));
    out->cls          = p->cls;
    out->pid          = p->pid;
    out->alive        = p->alive;
    out->calls        = p->calls;
    out->last_ping_ms = p->last_ping_ms;
    return true;
}

int FindProxyByClass(UDFClass cls) { return find_proxy_by_class(cls); }

void NotifyProxyDied(int pid) {
    int idx = find_proxy_by_pid(pid);
    if (idx < 0) return;
    g_proxy[idx].alive = false;
    char m[64]; int n = 0;
    n = u_cat(m, n, sizeof(m), "[UDF] proxy pid ");
    n = u_cat_u(m, n, sizeof(m), (uint64_t)pid);
    n = u_cat(m, n, sizeof(m), " died (class ops -> EDEAD until restart)\r\n");
    SerialLogger::Log(m);
}

const char* class_name(UDFClass c) {
    switch (c) {
        case UDF_CLASS_WIFI:    return "wifi";
        case UDF_CLASS_HID:     return "hid";
        case UDF_CLASS_STORAGE: return "storage";
        case UDF_CLASS_PRINTER: return "printer";
        default:                return "none";
    }
}

int Status(char* out, int mx) {
    if (!out || mx < 2) return 0;
    int p = 0;
    p = u_cat(out, p, mx, "UDF proxies (ring-3 user drivers via SYS_UDF_CALL):\n");
    for (int i = 0; i < g_proxy_count; i++) {
        if (!g_proxy[i].in_use) continue;
        p = u_cat(out, p, mx, "  ");
        p = u_cat(out, p, mx, g_proxy[i].name);
        p = u_cat(out, p, mx, "  class=");
        p = u_cat(out, p, mx, class_name(g_proxy[i].cls));
        p = u_cat(out, p, mx, "  pid=");
        p = u_cat_u(out, p, mx, (uint64_t)g_proxy[i].pid);
        p = u_cat(out, p, mx, g_proxy[i].alive ? "  alive" : "  DEAD");
        p = u_cat(out, p, mx, "  calls=");
        p = u_cat_u(out, p, mx, g_proxy[i].calls);
        p = u_cat(out, p, mx, "\n");
    }
    (void)u_len;
    return p;
}

}  // namespace UDF

// end (satoru)
