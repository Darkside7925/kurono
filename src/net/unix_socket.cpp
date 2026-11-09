#include "unix_socket.h"
#include "../kernel/heap.h"   // lazily allocated rx rings (satoru)
#include "../drivers/serial.h"
#include "../proc/scheduler.h"
#include "../proc/spinlock.h"

// task 25: file-scope (visible to both the anon-namespace ring writers and the
// UnixSocket-namespace setter) proactive poll-wakeup hook. (satoru)
static void (*g_data_ready_cb)(int sd) = nullptr;
// does the calling cpu hold the linux layer's global lock (kls)? a send to an in-kernel server
// runs that server's handler on the caller's stack and needs it. (satoru)
static bool (*g_kls_held_cb)() = nullptr;

// ── locking (the kls split, steps 2 and 3) ──────────────────────────────────────────────
// this layer used to rely on the linux syscall layer's one global lock. every socket now has a
// spinlock of its own (Socket::lk) covering its ring, flags and backlog, and one table lock
// (g_glock) covers what changes which slot means what: alloc/free, bind/listen/lookup/connect.
// rules:
//  * order is g_glock -> socket locks in ascending slot index. a data path never takes g_glock.
//  * an op that touches both ends of a connection (send writes the peer's ring, close severs
//    the link) locks the pair, lower index first. the peer index is read first without a
//    lock and re-checked under the locks (peer_sd links are only changed with both locks held),
//    so a slot that was freed and reused in between is never written.
//  * no hook or callback runs with a lock held: the poll-wake hook, an in-kernel server's
//    on_data/on_connect, and the close of an fd passed in flight all happen after the unlock.
//  * ring bytes are only ever copied to or from KERNEL memory here. callers bounce user
//    buffers (a user page fault under an irq-off spinlock would wedge).
//  * a send whose peer is an in-kernel server (wayland, pulse, dbus) runs its on_data handler,
//    which needs the linux layer's global lock; without it the send returns UNIX_NEED_KLS
//    and the caller retries holding kls. recv and everything else never needs it. (satoru)

namespace {

using namespace UnixSocket;

struct Socket;

struct Ring {
    // UNIX_RING_BYTES of ring, allocated the first time the slot is used and kept
    // for reuse: 64 idle slots used to pin ~4 mb at boot. (satoru)
    uint8_t* data;
    uint32_t head;
    uint32_t tail;
    // Inline message-boundary metadata.  For UNIX_SOCK_STREAM we ignore this;
    // for UNIX_SOCK_DGRAM/SEQPACKET each entry is one record.
    struct Frame {
        uint32_t offset;
        uint32_t length;
        ControlMsg cmsg;
    };
    static const int FRAME_RING = 64;
    Frame    frames[FRAME_RING];
    uint32_t frame_head;
    uint32_t frame_tail;

    int avail() const { return (int)(head - tail); }
    int free()  const { return UNIX_RING_BYTES - avail(); }
};

struct Socket {
    Spinlock   lk;               // ring, flags, backlog, peer link of THIS socket (satoru)
    bool       in_use;
    SockType   type;
    bool       listening;
    bool       connected;
    bool       bound;
    bool       shutdown_rd;
    bool       shutdown_wr;
    char       path[UNIX_PATH_MAX];
    int        peer_sd;          // -1 if unconnected
    int        backlog[UNIX_MAX_BACKLOG];
    int        backlog_head, backlog_tail;
    Ring       rx;               // bytes the peer has written to *us*
    Credentials creds;            // our peer's creds, captured at connect
    ConnectionCallback on_connect;
    DataCallback       on_data;
    void*              user;
    bool       is_kernel_server;
    int        refs;             // open-fd refcount. >1 once a socketpair end is
                                 // duplicated across fork (clone_file_descriptors
                                 // calls Retain). Close() only severs the peer +
                                 // frees the slot on the LAST close. without this
                                 // the parent's standard post-fork close of the
                                 // child's socketpair end tore down its own live
                                 // ipc end -> firefox e10s child replies never
                                 // arrived -> parent busy-spun. (satoru)
};

Socket   g_socks[UNIX_MAX_SOCKETS];
Spinlock g_glock;                // table lock, see the block comment above (satoru)

// lock one socket, or two in index order. the first lock saves irq state, the second rides on it. (satoru)
struct PairLock {
    Spinlock* a = nullptr;
    Spinlock* b = nullptr;
    uint64_t  fl = 0;
    void lock(int x, int y) {
        if (y < 0 || y >= UNIX_MAX_SOCKETS || y == x) { a = &g_socks[x].lk; b = nullptr; }
        else if (x < y) { a = &g_socks[x].lk; b = &g_socks[y].lk; }
        else            { a = &g_socks[y].lk; b = &g_socks[x].lk; }
        a->LockIrqSave(&fl);
        if (b) b->Lock();
    }
    void unlock() {
        if (b) b->Unlock();
        a->UnlockIrqRestore(fl);
    }
};

inline bool in_range(int sd) { return sd >= 0 && sd < UNIX_MAX_SOCKETS; }

// slot allocation. g_glock held. the slot comes back in_use with refs 1 and every field reset. (satoru)
int alloc_sd_locked() {
    for (int i = 0; i < UNIX_MAX_SOCKETS; i++) {
        if (!g_socks[i].in_use) {
            Socket* s = &g_socks[i];
            if (!s->rx.data) {
                s->rx.data = (uint8_t*)KernelHeap::Alloc(UNIX_RING_BYTES);
                if (!s->rx.data) return -1;   // out of memory: no socket (satoru)
            }
            SpinLockGuard g(s->lk);
            s->in_use = true;
            s->type = UNIX_SOCK_STREAM;
            s->listening = false;
            s->connected = false;
            s->bound = false;
            s->shutdown_rd = false;
            s->shutdown_wr = false;
            s->path[0] = 0;
            s->peer_sd = -1;
            s->backlog_head = s->backlog_tail = 0;
            s->rx.head = s->rx.tail = 0;
            s->rx.frame_head = s->rx.frame_tail = 0;
            s->on_connect = nullptr;
            s->on_data = nullptr;
            s->user = nullptr;
            s->is_kernel_server = false;
            __atomic_store_n(&s->refs, 1, __ATOMIC_RELEASE);
            s->creds = {0, 0, 0};
            return i;
        }
    }
    return -1;
}

bool path_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    // Abstract namespace: leading 0 byte + bounded length.
    if (a[0] == 0 && b[0] == 0) {
        for (int i = 1; i < UNIX_PATH_MAX; i++) {
            if (a[i] != b[i]) return false;
            if (a[i] == 0) return true;
        }
        return true;
    }
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

inline void block_copy(uint8_t* d, const uint8_t* s, int n) {
    // Word-aligned copy when possible; falls back to byte for the tail.
    int i = 0;
    while (i + 7 < n) {
        ((uint64_t*)(d + i))[0] = ((const uint64_t*)(s + i))[0];
        i += 8;
    }
    while (i < n) { d[i] = s[i]; i++; }
}

// socket lock held (the ring's owner's). data is kernel memory. (satoru)
int ring_write(Ring& r, const uint8_t* data, int len, const ControlMsg* cm,
               SockType type) {
    if (len <= 0) return 0;
    // Datagram / seqpacket: if no frame slot is available, fail rather
    // than silently merging into the previous record.
    if (type != UNIX_SOCK_STREAM) {
        if (r.frame_head - r.frame_tail >= Ring::FRAME_RING) return 0;
    }
    // stream: only fd-carrying writes get a frame, and never drop one. (satoru)
    bool stream_fds = type == UNIX_SOCK_STREAM && cm && cm->passed_fd_count > 0;
    if (stream_fds && r.frame_head - r.frame_tail >= Ring::FRAME_RING) return 0;
    int can = r.free();
    if (can <= 0) return 0;
    if (len > can) len = can;
    uint32_t off = r.head % UNIX_RING_BYTES;
    int first = (int)(UNIX_RING_BYTES - off);
    if (first > len) first = len;
    block_copy(r.data + off, data, first);
    if (len > first) {
        block_copy(r.data, data + first, len - first);
    }
    if (type != UNIX_SOCK_STREAM) {
        uint32_t fi = r.frame_head % Ring::FRAME_RING;
        r.frames[fi].offset = off;
        r.frames[fi].length = (uint32_t)len;
        if (cm) r.frames[fi].cmsg = *cm;
        else    r.frames[fi].cmsg = {};
        r.frame_head++;
    } else if (stream_fds) {
        // stream offset is the absolute byte position: the fds ride with it. (satoru)
        uint32_t fi = r.frame_head % Ring::FRAME_RING;
        r.frames[fi].offset = r.head;
        r.frames[fi].length = (uint32_t)len;
        r.frames[fi].cmsg = *cm;
        r.frame_head++;
    }
    r.head += (uint32_t)len;
    return len;
}

// socket lock held; out is kernel memory. a plain read that discards in-flight fds cannot close
// them here (Close takes locks): their sds are returned in drop[] for the caller to close after
// it unlocks. (satoru)
int ring_read(Ring& r, uint8_t* out, int max, ControlMsg* cm, SockType type,
              int* drop, int* ndrop) {
    int avail = r.avail();
    if (avail <= 0) return 0;
    int n = avail;
    int consume = avail;
    if (type == UNIX_SOCK_DGRAM || type == UNIX_SOCK_SEQPACKET) {
        if (r.frame_head == r.frame_tail) return 0;
        uint32_t fi = r.frame_tail % Ring::FRAME_RING;
        int frame_len = (int)r.frames[fi].length;
        n = frame_len < max ? frame_len : max;
        consume = frame_len;     // datagrams discard the truncated tail
        if (cm) *cm = r.frames[fi].cmsg;
        r.frame_tail++;
    } else {
        if (n > max) n = max;
        if (cm) *cm = {};
        // linux semantics: fds arrive with the read holding their first byte,
        // and a read never runs into the next fd-carrying write. (satoru)
        if (r.frame_head != r.frame_tail) {
            uint32_t fi = r.frame_tail % Ring::FRAME_RING;
            int lead = (int)(r.frames[fi].offset - r.tail);
            if (lead <= 0) {
                const ControlMsg& fc = r.frames[fi].cmsg;
                if (cm) *cm = fc;
                else {
                    // plain read: the fds are discarded - drop their in-flight refs. (satoru)
                    for (int k = 0; k < fc.passed_fd_count; k++)
                        if (fc.passed_is_socket[k] && *ndrop < UNIX_MAX_PASSED_FD) drop[(*ndrop)++] = fc.passed_sd[k];
                }
                r.frame_tail++;
                if (r.frame_head != r.frame_tail) {
                    int next = (int)(r.frames[r.frame_tail % Ring::FRAME_RING].offset - r.tail);
                    if (next > 0 && n > next) n = next;
                }
            } else if (n > lead) {
                n = lead;
            }
        }
        consume = n;
    }
    uint32_t off = r.tail % UNIX_RING_BYTES;
    int first = (int)(UNIX_RING_BYTES - off);
    if (first > n) first = n;
    block_copy(out, r.data + off, first);
    if (n > first) {
        block_copy(out + first, r.data, n - first);
    }
    r.tail += (uint32_t)consume;
    return n;
}

// room for another write: linux-style hysteresis (a quarter ring) so a full
// writer isn't woken for every few bytes the reader drains. (satoru)
inline bool ring_room(const Ring& r) {
    return r.free() >= UNIX_RING_BYTES / 4 &&
           r.frame_head - r.frame_tail < (uint32_t)Ring::FRAME_RING;
}

}  // namespace

namespace UnixSocket {

void Init() {
    for (int i = 0; i < UNIX_MAX_SOCKETS; i++) g_socks[i].in_use = false;
    SerialLogger::Log("UnixSocket: AF_UNIX subsystem ready, ");
    SerialLogger::LogDec(UNIX_MAX_SOCKETS);
    SerialLogger::Log(" sd slots\r\n");
}

// task 25: expose the setter in the UnixSocket namespace (matches the header
// decl); g_data_ready_cb is the file-scope static defined at the top. (satoru)
void SetDataReadyHook(void (*cb)(int sd)) { g_data_ready_cb = cb; }
void SetKlsHeldHook(bool (*cb)()) { g_kls_held_cb = cb; }

// the 64 KB ring of the next free slot is allocated here, before the table lock (which is irq-off)
// is taken; alloc_sd_locked still allocates itself if another cpu took that slot first. (satoru)
static void prealloc_ring(int nth = 0) {
    for (int i = 0; i < UNIX_MAX_SOCKETS; i++) {
        if (g_socks[i].in_use) continue;
        if (nth-- > 0) continue;
        if (g_socks[i].rx.data) return;     // that free slot is ready (satoru)
        uint8_t* d = (uint8_t*)KernelHeap::Alloc(UNIX_RING_BYTES);
        if (!d) return;
        SpinLockGuard gg(g_glock);
        if (!g_socks[i].rx.data) g_socks[i].rx.data = d; else KernelHeap::Free(d);
        return;
    }
}

int Create(SockType type) {
    prealloc_ring();
    SpinLockGuard gg(g_glock);
    int sd = alloc_sd_locked();
    if (sd < 0) return -1;
    g_socks[sd].type = type;
    return sd;
}

int Bind(int sd, const char* path) {
    if (!in_range(sd) || !path) return -1;
    SpinLockGuard gg(g_glock);
    Socket& s = g_socks[sd];
    SpinLockGuard g(s.lk);
    if (!s.in_use) return -1;
    int i = 0;
    while (i < UNIX_PATH_MAX - 1 && (i == 0 || path[i - 1])) {
        s.path[i] = path[i];
        if (path[i] == 0 && i > 0) break;
        i++;
    }
    s.path[UNIX_PATH_MAX - 1] = 0;
    s.bound = true;
    return 0;
}

int Listen(int sd, int backlog) {
    (void)backlog;
    if (!in_range(sd)) return -1;
    SpinLockGuard gg(g_glock);
    Socket& s = g_socks[sd];
    SpinLockGuard g(s.lk);
    if (!s.in_use || !s.bound) return -1;
    s.listening = true;
    return 0;
}

// g_glock held. (satoru)
static int lookup_locked(const char* path) {
    for (int i = 0; i < UNIX_MAX_SOCKETS; i++) {
        if (g_socks[i].in_use && g_socks[i].listening &&
            path_eq(g_socks[i].path, path)) return i;
    }
    return -1;
}

int Lookup(const char* path) {
    SpinLockGuard gg(g_glock);
    return lookup_locked(path);
}

int Connect(int sd, const char* path) {
    if (!in_range(sd) || !path) return -1;
    prealloc_ring();
    int srv, accept_sd;
    ConnectionCallback on_conn; void* user; bool wake_listener = false;
    {
        // the whole connect runs under the table lock: the listener cannot be closed and its
        // slot reused between the lookup and the link. (satoru)
        SpinLockGuard gg(g_glock);
        if (!g_socks[sd].in_use) return -1;
        srv = lookup_locked(path);
        if (srv < 0) return -111;          // ECONNREFUSED
        Socket& server = g_socks[srv];

        // an in-kernel server (pulse, dbus, wayland) takes the connection in its
        // on_connect callback and never calls Accept(), so queueing it in the
        // backlog only filled the 16 slots: after 15 connects per boot every new
        // client got EAGAIN. only a listener that accepts gets the backlog. (satoru)
        const bool takes_in_callback = server.is_kernel_server && server.on_connect;
        // Reject early if the backlog is full so we don't leak an accept slot.
        int next = (server.backlog_head + 1) % UNIX_MAX_BACKLOG;
        if (!takes_in_callback && next == server.backlog_tail) return -11;       // EAGAIN

        accept_sd = alloc_sd_locked();
        if (accept_sd < 0) return -1;
        Socket& as = g_socks[accept_sd];
        Socket& cs = g_socks[sd];
        // the accepted socket is private until it is linked below; the client end and the
        // listener's backlog change under their locks. (satoru)
        {
            SpinLockGuard ag(as.lk);
            as.type    = server.type;
            as.connected = true;
            as.peer_sd = sd;
            for (int i = 0; i < UNIX_PATH_MAX; i++) as.path[i] = server.path[i];
            // an in-kernel server's accepted connection inherits the server's data
            // handler + flag, so a client's writes drive the server synchronously: this
            // is how the wayland / dbus compositors process a connected client's
            // requests. without it the accepted socket has no on_data, so firefox's
            // wl_display.get_registry sits unread and firefox hangs polling its display
            // fd for the registry globals the compositor never sends. (satoru)
            as.is_kernel_server = server.is_kernel_server;
            as.on_data          = server.on_data;
            as.user             = server.user;
            // stamp the connector's identity on the server-side end so an in-kernel
            // server (wayland) can resolve which process owns a client via
            // GetPeerCred - connect runs in the connecting task's syscall context,
            // so the current process IS the peer. (satoru)
            Process* cur = Scheduler::GetCurrentProcess();
            as.creds.pid = cur ? cur->pid : 0;
            as.creds.uid = 1000;
            as.creds.gid = 1000;
        }
        {
            PairLock pl; pl.lock(sd, srv);
            cs.connected = true;
            cs.peer_sd = accept_sd;
            cs.type    = server.type;
            if (!takes_in_callback) {
                server.backlog[server.backlog_head] = accept_sd;
                server.backlog_head = next;
                // sched=2: a user-space listener parked in poll/accept is woken by the
                // connect instead of finding it at its timer backstop (the old mode
                // relied on the backstop; unchanged there). (satoru)
                wake_listener = Scheduler::SchedV2();
            }
            pl.unlock();
        }
        on_conn = server.on_connect; user = server.user;
    }
    if (wake_listener && g_data_ready_cb) g_data_ready_cb(srv);
    if (on_conn) on_conn(srv, accept_sd, user);
    return 0;
}

int Accept(int sd, char* peer_path, int peer_path_len) {
    if (!in_range(sd)) return -1;
    Socket& s = g_socks[sd];
    SpinLockGuard g(s.lk);
    if (!s.in_use || !s.listening) return -1;
    if (s.backlog_head == s.backlog_tail) return -11;   // EAGAIN
    int new_sd = s.backlog[s.backlog_tail];
    s.backlog_tail = (s.backlog_tail + 1) % UNIX_MAX_BACKLOG;
    if (peer_path && peer_path_len > 0) {
        int i = 0;
        while (i < peer_path_len - 1 && s.path[i]) {
            peer_path[i] = s.path[i]; i++;
        }
        peer_path[i] = 0;
    }
    return new_sd;
}

int Pair(SockType type, int* sd0, int* sd1) {
    if (!sd0 || !sd1) return -1;
    prealloc_ring(0); prealloc_ring(1);
    SpinLockGuard gg(g_glock);
    int a = alloc_sd_locked();
    if (a < 0) return -1;
    int b = alloc_sd_locked();
    if (b < 0) {
        SpinLockGuard g(g_socks[a].lk);
        g_socks[a].in_use = false;
        return -1;
    }
    PairLock pl; pl.lock(a, b);
    g_socks[a].type = type; g_socks[b].type = type;
    g_socks[a].connected = g_socks[b].connected = true;
    g_socks[a].peer_sd = b;
    g_socks[b].peer_sd = a;
    g_socks[a].creds = {0, 0, 0};
    g_socks[b].creds = {0, 0, 0};
    pl.unlock();
    *sd0 = a; *sd1 = b;
    return 0;
}

// pending ancillary for in-kernel servers: the message a server's on_data is
// parsing right now may have carried SCM_RIGHTS fds (e.g. a wl_shm pool fd). We
// stash the ControlMsg here so the handler can pull it via TakePendingControl()
// during the synchronous on_data call. only in-kernel servers use these and their
// sends run under the linux layer's global lock. (satoru)
static ControlMsg g_pending_ctrl[UNIX_MAX_SOCKETS];
static bool       g_pending_ctrl_valid[UNIX_MAX_SOCKETS];
// how many of the pending message's passed fds have already been handed out.
// one sendmsg can carry MANY SCM_RIGHTS fds (firefox batches several
// wl_shm.create_pool requests, each with its own memfd, into one send); each
// create_pool must consume the NEXT fd in order. the old code returned the whole
// message on the first TakePendingControl and dropped the rest, so every pool
// after the first got a null backing -> the browser window's large shm buffer
// never mapped -> the window painted black. (satoru)
static int        g_pending_ctrl_taken[UNIX_MAX_SOCKETS];

// hand what a send queued for an in-kernel server to its on_data, outside every socket lock. (satoru)
static void drain_kernel_server(int peer, const ControlMsg& cm) {
    Socket& ps = g_socks[peer];
    DataCallback od; void* user; SockType ty;
    {
        SpinLockGuard g(ps.lk);
        if (!ps.in_use) return;
        od = ps.on_data; user = ps.user; ty = ps.type;
    }
    if (!od) return;
    // (satoru) TEMP [sc] guard for the firefox sibling-sendmsg #UD: a kernel-server peer whose
    // on_data is a garbage pointer (e.g. 3) means this indirect call jumps into nothing. skip an
    // insane handler so we never execute address 0x3.
    if ((uint64_t)(uintptr_t)od < 0x1000ULL) {
        SerialLogger::Log("[sc] BAD on_data peer="); SerialLogger::LogDec(peer);
        SerialLogger::Log("\r\n");
        return;
    }
    // expose ancillary (passed fds / resolved shm) to the handler. (satoru)
    if (cm.passed_fd_count > 0) {
        g_pending_ctrl[peer]       = cm;
        g_pending_ctrl_valid[peer] = true;
        g_pending_ctrl_taken[peer] = 0;   // hand out fds one at a time (satoru)
    }
    // Drain into the kernel-side handler a span at a time: copy out of the ring under the
    // socket lock, call the handler without it. (satoru)
    static const int CHUNK = 8192;
    uint8_t scratch[CHUNK];
    if (ty == UNIX_SOCK_STREAM) {
        for (;;) {
            int n;
            {
                SpinLockGuard g(ps.lk);
                n = ps.rx.avail();
                if (n <= 0) {
                    // Drain stale frame metadata so it doesn't accumulate.
                    ps.rx.frame_tail = ps.rx.frame_head;
                    break;
                }
                if (n > CHUNK) n = CHUNK;
                uint32_t off = ps.rx.tail % UNIX_RING_BYTES;
                int contig = (int)(UNIX_RING_BYTES - off);
                if (n > contig) n = contig;
                block_copy(scratch, ps.rx.data + off, n);
                ps.rx.tail += (uint32_t)n;
            }
            od(peer, scratch, n, user);
        }
    } else {
        int got;
        int dropd[UNIX_MAX_PASSED_FD]; int nd = 0;
        {
            SpinLockGuard g(ps.lk);
            got = ring_read(ps.rx, scratch, 2048, nullptr, ty, dropd, &nd);
        }
        if (got > 0) od(peer, scratch, got, user);
    }
    g_pending_ctrl_valid[peer] = false;
}

// shared write core: enqueue to the peer ring and, for an in-kernel server peer, drive its on_data
// handler with the ancillary made available for this delivery. buf is kernel memory. (satoru)
static int send_core(int sd, const void* buf, int len, const ControlMsg& cm) {
    for (int attempt = 0; attempt < 16; attempt++) {
        if (!in_range(sd)) return -32;   // EPIPE
        Socket& s = g_socks[sd];
        int hint = __atomic_load_n(&s.peer_sd, __ATOMIC_ACQUIRE);
        PairLock pl; pl.lock(sd, hint);
        if (!s.in_use || !s.connected) { pl.unlock(); return -32; }
        if (s.peer_sd != hint) { pl.unlock(); continue; }   // the link changed under us: look again (satoru)
        if (s.shutdown_wr) { pl.unlock(); return -32; }
        if (!in_range(hint) || !g_socks[hint].in_use) { pl.unlock(); return -32; }
        Socket& ps = g_socks[hint];
        if (ps.shutdown_rd) { pl.unlock(); return -32; }
        const bool ksrv = ps.is_kernel_server;
        if (ksrv && ps.on_data && g_kls_held_cb && !g_kls_held_cb()) { pl.unlock(); return UNIX_NEED_KLS; }
        // a passed socket stays referenced while in flight: the sender may close its
        // copy before the receiver installs it. recvmsg adopts the ref. taken BEFORE the
        // bytes become visible, so a receiver on another cpu can never adopt a ref that
        // does not exist yet. (satoru)
        if (!ksrv)
            for (int k = 0; k < cm.passed_fd_count; k++)
                if (cm.passed_is_socket[k]) Retain(cm.passed_sd[k]);
        int w = ring_write(ps.rx, (const uint8_t*)buf, len, &cm, s.type);
        const bool has_handler = ksrv && ps.on_data;
        pl.unlock();
        if (w <= 0) {
            if (!ksrv)
                for (int k = 0; k < cm.passed_fd_count; k++)
                    if (cm.passed_is_socket[k]) Close(cm.passed_sd[k]);   // nothing was queued: undo (satoru)
            if (len > 0) return -11;  // EAGAIN
            return 0;
        }
        if (has_handler && w > 0) {
            drain_kernel_server(hint, cm);
        } else if (w > 0 && !ksrv && g_data_ready_cb) {
            // task 25: a user peer's data now sits in its rx ring for it to poll/recv -
            // wake any poll waiter parked on it NOW (a kernel-server peer drained
            // synchronously above, so it has no poller to wake). (satoru)
            g_data_ready_cb(hint);
        }
        return w;
    }
    return -32;
}

int Send(int sd, const void* buf, int len, int flags,
         const int* pass_fds, int n_fds) {
    (void)flags;
    if (!in_range(sd)) return -32;
    ControlMsg cm = {};
    if (pass_fds && n_fds > 0) {
        if (n_fds > UNIX_MAX_PASSED_FD) n_fds = UNIX_MAX_PASSED_FD;
        for (int i = 0; i < n_fds; i++) cm.passed_fds[i] = pass_fds[i];
        cm.passed_fd_count = n_fds;
    }
    cm.creds_valid = true;
    cm.peer_creds  = g_socks[sd].creds;
    return send_core(sd, buf, len, cm);
}

int SendMsg(int sd, const void* buf, int len, int flags, const ControlMsg* cmin) {
    (void)flags;
    if (!in_range(sd)) return -32;
    ControlMsg cm = cmin ? *cmin : ControlMsg{};
    cm.creds_valid = true;
    cm.peer_creds  = g_socks[sd].creds;
    return send_core(sd, buf, len, cm);
}

int PeerSd(int sd) {
    if (!in_range(sd)) return -1;
    SpinLockGuard g(g_socks[sd].lk);
    return g_socks[sd].in_use ? g_socks[sd].peer_sd : -1;   // (satoru)
}

bool IsStream(int sd) {
    if (!in_range(sd)) return true;
    return g_socks[sd].type == UNIX_SOCK_STREAM;
}

bool NeedsKlsToSend(int sd) {
    if (!in_range(sd)) return false;
    Socket& s = g_socks[sd];
    int hint = __atomic_load_n(&s.peer_sd, __ATOMIC_ACQUIRE);
    if (!in_range(hint)) return false;
    PairLock pl; pl.lock(sd, hint);
    bool r = s.in_use && s.peer_sd == hint && g_socks[hint].is_kernel_server && g_socks[hint].on_data;
    pl.unlock();
    return r;
}

bool TakePendingControl(int sd, ControlMsg* out) {
    if (sd < 0 || sd >= UNIX_MAX_SOCKETS) return false;
    if (!g_pending_ctrl_valid[sd]) return false;
    ControlMsg& p = g_pending_ctrl[sd];
    int t = g_pending_ctrl_taken[sd];
    // a message with fds is consumed one fd per call so consecutive create_pool
    // requests in the same send each get their own memfd. a creds-only message
    // (no fds) is returned once as before. (satoru)
    if (p.passed_fd_count > 0) {
        if (t >= p.passed_fd_count) { g_pending_ctrl_valid[sd] = false; return false; }
        if (out) {
            ControlMsg one = {};
            one.passed_fd_count     = 1;
            one.passed_fds[0]       = p.passed_fds[t];
            one.passed_shm_base[0]  = p.passed_shm_base[t];
            one.passed_shm_size[0]  = p.passed_shm_size[t];
            one.passed_sd[0]        = p.passed_sd[t];
            one.passed_is_socket[0] = p.passed_is_socket[t];
            one.creds_valid         = p.creds_valid;
            one.peer_creds          = p.peer_creds;
            *out = one;
        }
        g_pending_ctrl_taken[sd] = t + 1;
        if (g_pending_ctrl_taken[sd] >= p.passed_fd_count) g_pending_ctrl_valid[sd] = false;
        return true;
    }
    if (out) *out = p;
    g_pending_ctrl_valid[sd] = false;
    return true;
}

// buf is kernel memory (the linux layer bounces user buffers). (satoru)
int Recv(int sd, void* buf, int len, int flags, ControlMsg* cmsg) {
    (void)flags;
    if (!in_range(sd)) return -1;
    Socket& s = g_socks[sd];
    int wake = -1, drop[UNIX_MAX_PASSED_FD], nd = 0, ret;
    {
        SpinLockGuard g(s.lk);
        if (!s.in_use) return -1;
        if (s.shutdown_rd) return 0;
        bool was_full = !ring_room(s.rx);
        int n = ring_read(s.rx, (uint8_t*)buf, len, cmsg, s.type, drop, &nd);
        // drained back below the mark: wake the writer parked on epollout. (satoru)
        if (n > 0 && was_full && ring_room(s.rx) && s.peer_sd >= 0) wake = s.peer_sd;
        ret = n;
        // empty ring is eof only once the peer is gone, else eagain. firefox ipc
        // reads until eagain and took the old 0 as the content process hanging up. (satoru)
        if (n == 0 && len > 0 && s.connected && s.peer_sd >= 0) ret = -11;
    }
    for (int k = 0; k < nd; k++) Close(drop[k]);
    if (wake >= 0 && g_data_ready_cb) g_data_ready_cb(wake);
    return ret;
}

int Shutdown(int sd, int how) {
    if (!in_range(sd)) return -1;
    Socket& s = g_socks[sd];
    SpinLockGuard g(s.lk);
    if (!s.in_use) return -1;
    if (how == 0 || how == 2) s.shutdown_rd = true;
    if (how == 1 || how == 2) s.shutdown_wr = true;
    return 0;
}

// task 22: a stream socket/pipe end whose peer has CLOSED - Close() severed our
// peer_sd (-1) and set shutdown_wr on us. POSIX poll must then report POLLHUP so
// a reader waiting for data-or-eof wakes and reaps the connection instead of
// polling forever (the glxtest child-pipe hang: firefox's parent polled the
// exited glxtest's pipe with neither data nor HUP -> "ManageChildProcess poll
// failed" + the StreamTransport retry storm). connected-then-severed only;
// never-connected + listen fds are not hung up. buffered rx still drains first
// (the caller keeps POLLIN while bytes remain). (satoru)
bool PeerClosed(int sd) {
    if (!in_range(sd)) return false;
    Socket& s = g_socks[sd];
    SpinLockGuard g(s.lk);
    return s.in_use && s.connected && s.peer_sd < 0;
}

int Retain(int sd) {
    if (!in_range(sd)) return -1;
    __atomic_add_fetch(&g_socks[sd].refs, 1, __ATOMIC_ACQ_REL);   // dup runs without kls: atomic (satoru)
    return 0;
}

int Close(int sd) {
    if (!in_range(sd)) return -1;
    Socket& s = g_socks[sd];
    // a socketpair end inherited across fork is referenced by both the parent's
    // and the child's fd tables (same global sd). only the LAST close severs the
    // peer + frees the slot; an earlier close just drops this fd's reference, so
    // the still-live peer keeps working. (satoru)
    {
        int r = __atomic_load_n(&s.refs, __ATOMIC_ACQUIRE);
        while (r > 1)
            if (__atomic_compare_exchange_n(&s.refs, &r, r - 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    }
    int woke = -1;
    {
        // the last reference: unlink and free under the table lock plus both socket locks. a
        // concurrent Close of the peer takes the same g_glock first, so the two never cross. (satoru)
        SpinLockGuard gg(g_glock);
        for (int attempt = 0; attempt < 16; attempt++) {
            int hint = __atomic_load_n(&s.peer_sd, __ATOMIC_ACQUIRE);
            PairLock pl; pl.lock(sd, hint);
            if (!s.in_use) { pl.unlock(); return -1; }
            if (s.peer_sd != hint) { pl.unlock(); continue; }
            s.refs = 0;
            if (hint >= 0 && g_socks[hint].in_use) {
                Socket& ps = g_socks[hint];
                ps.shutdown_wr = true;     // peer's writes will start failing
                ps.peer_sd = -1;
                // Leave any buffered RX data on the peer so it can drain after
                // our close - connection-style EOF semantics.
                woke = hint;
            }
            s.peer_sd = -1;
            s.in_use = false;
            pl.unlock();
            break;
        }
    }
    // task 28: the peer is now hung up (POLLHUP) - wake its parked
    // poller NOW so a reader polling a dead child's pipe sees the eof
    // immediately instead of burning its full poll timeout (the 4s
    // glxtest tax on every firefox boot). (satoru)
    if (woke >= 0 && g_data_ready_cb) g_data_ready_cb(woke);
    return 0;
}

int GetSockName(int sd, char* path, int path_len) {
    if (!in_range(sd) || !path || path_len <= 0) return -1;
    Socket& s = g_socks[sd];
    SpinLockGuard g(s.lk);
    if (!s.in_use) return -1;
    int i = 0;
    while (i < path_len - 1 && s.path[i]) { path[i] = s.path[i]; i++; }
    path[i] = 0;
    return i;
}

int GetPeerName(int sd, char* path, int path_len) {
    if (!in_range(sd) || !path) return -1;
    int peer = __atomic_load_n(&g_socks[sd].peer_sd, __ATOMIC_ACQUIRE);
    if (!in_range(peer)) return -1;
    return GetSockName(peer, path, path_len);
}

int GetPeerCred(int sd, Credentials* out) {
    if (!in_range(sd) || !out) return -1;
    SpinLockGuard g(g_socks[sd].lk);
    if (!g_socks[sd].in_use) return -1;
    *out = g_socks[sd].creds;
    return 0;
}

void RegisterServer(int sd, ConnectionCallback on_conn, DataCallback on_data,
                    void* user) {
    if (!in_range(sd)) return;
    SpinLockGuard gg(g_glock);
    Socket& s = g_socks[sd];
    SpinLockGuard g(s.lk);
    if (!s.in_use) return;
    s.on_connect = on_conn;
    s.on_data    = on_data;
    s.user       = user;
    s.is_kernel_server = true;
}

// census: every live socket with unread bytes (the black-window hunt: is the
// last wayland event sitting unread in some ring?). diagnostic only, reads
// without the locks. (satoru)
void DumpPending() {
    for (int i = 0; i < UNIX_MAX_SOCKETS; i++) {
        Socket& s = g_socks[i];
        if (!s.in_use) continue;
        SerialLogger::Log("[usock] sd="); SerialLogger::LogDec(i);
        SerialLogger::Log(" peer="); SerialLogger::LogDec(s.peer_sd);
        SerialLogger::Log(" unread="); SerialLogger::LogDec((int)s.rx.avail());
        SerialLogger::Log(s.is_kernel_server ? " ksrv" : "");
        SerialLogger::Log(s.listening ? " listen" : "");
        SerialLogger::Log(" pid="); SerialLogger::LogDec((int)s.creds.pid);
        SerialLogger::Log(" "); SerialLogger::Log(s.path);
        SerialLogger::Log("\r\n");
    }
}

int PendingBytes(int sd) {
    if (!in_range(sd)) return 0;
    Socket& s = g_socks[sd];
    SpinLockGuard g(s.lk);
    return s.in_use ? s.rx.avail() : 0;
}

// one pass over the readiness bits poll/epoll need (UN_READY_*), under the pair lock: the answer
// to "room to write" lives in the peer's ring. (satoru)
uint32_t Readiness(int sd) {
    if (!in_range(sd)) return UN_READY_OUT;   // the write itself reports the error (satoru)
    Socket& s = g_socks[sd];
    for (int attempt = 0; attempt < 16; attempt++) {
        int hint = __atomic_load_n(&s.peer_sd, __ATOMIC_ACQUIRE);
        PairLock pl; pl.lock(sd, hint);
        if (!s.in_use) { pl.unlock(); return UN_READY_OUT; }
        if (s.peer_sd != hint) { pl.unlock(); continue; }
        uint32_t r = 0;
        // readable when there are bytes OR (for a listen fd) a pending connection. (satoru)
        if (s.rx.avail() > 0 || (s.listening && s.backlog_head != s.backlog_tail)) r |= UN_READY_IN;
        // a full peer ring makes the write eagain: out only with room. (satoru)
        if (!s.connected || !in_range(hint) || !g_socks[hint].in_use) r |= UN_READY_OUT;
        else if (g_socks[hint].is_kernel_server || ring_room(g_socks[hint].rx)) r |= UN_READY_OUT;
        if (s.connected && s.peer_sd < 0) r |= UN_READY_HUP;
        pl.unlock();
        return r;
    }
    return UN_READY_OUT;
}

bool Writable(int sd) { return (Readiness(sd) & UN_READY_OUT) != 0; }

uint32_t EventSeq(int sd) {
    if (!in_range(sd)) return 0;
    Socket& s = g_socks[sd];
    for (int attempt = 0; attempt < 16; attempt++) {
        int hint = __atomic_load_n(&s.peer_sd, __ATOMIC_ACQUIRE);
        PairLock pl; pl.lock(sd, hint);
        if (!s.in_use) { pl.unlock(); return 0; }
        if (s.peer_sd != hint) { pl.unlock(); continue; }
        uint32_t q = s.rx.head + (uint32_t)s.backlog_head * 0x9E3779B1u;   // data in, connections (satoru)
        if (in_range(hint) && g_socks[hint].in_use) q += g_socks[hint].rx.tail;   // peer drained: writable (satoru)
        else if (s.connected) q ^= 0x80000000u;                            // peer gone (satoru)
        pl.unlock();
        return q;
    }
    return 0;
}

// (satoru) TEMP diag: bytes waiting in this socket's PEER rx ring (what a write
// to `sd` delivers). lets the poll probe tell "polled fd has no data because the
// signaler wrote the read end and it went to the peer" apart from "nobody wrote
// it". returns -1 if unpaired. remove before commit.
int PeerPendingBytes(int sd) {
    if (!in_range(sd) || !g_socks[sd].in_use) return -1;
    int peer = g_socks[sd].peer_sd;
    if (peer < 0 || peer >= UNIX_MAX_SOCKETS || !g_socks[peer].in_use) return -1;
    return g_socks[peer].rx.avail();
}

// true when a listening socket has a connection waiting in its backlog. poll /
// epoll must report POLLIN on a listen fd so an accept loop (e.g. firefox's
// WaylandProxy, which listens on wayland-proxy-<pid> and forwards to wayland-0)
// wakes and accept()s the pending client. without it the proxy never accepts,
// so firefox's get_registry is never forwarded to the compositor. (satoru)
bool HasPendingConnection(int sd) {
    if (!in_range(sd)) return false;
    Socket& s = g_socks[sd];
    SpinLockGuard g(s.lk);
    return s.in_use && s.listening && s.backlog_head != s.backlog_tail;
}

// kernel->client write core shared by KernelInject and KernelInjectMsg. (satoru)
static int inject_core(int sd, const void* buf, int len, const ControlMsg* cm) {
    if (!in_range(sd)) return -1;
    Socket& s = g_socks[sd];
    for (int attempt = 0; attempt < 16; attempt++) {
        int hint = __atomic_load_n(&s.peer_sd, __ATOMIC_ACQUIRE);
        PairLock pl; pl.lock(sd, hint);
        if (!s.in_use) { pl.unlock(); return -1; }
        if (s.peer_sd != hint) { pl.unlock(); continue; }
        // kernel->client: an in-kernel server (wayland/dbus/pulse) holds the ACCEPTED
        // socket `sd` and reads the client's requests off sd.rx (send_core drains sd.rx
        // into the server's on_data). its replies/events must reach the CLIENT, which
        // Recv()s its own fd == the PEER of sd, so they have to land in the peer's rx
        // ring. writing them into sd.rx instead looped them straight back into the
        // server's own on_data: firefox's wl_registry.global + wl_seat/wl_output events
        // re-arrived as phantom requests (the global event payload name+iface+version
        // parses byte-for-byte as a wl_registry.bind), the client received nothing on
        // its fd, and gtk stalled waiting for wl_seat.capabilities. route to the peer
        // when one is connected (an unconnected/self-fed socket keeps own.rx). (satoru)
        int target = sd;
        if (s.connected && in_range(hint) && g_socks[hint].in_use) target = hint;
        Socket& ts = g_socks[target];
        int w = ring_write(ts.rx, (const uint8_t*)buf, len, cm, ts.type);
        pl.unlock();
        if (w > 0 && g_data_ready_cb) g_data_ready_cb(target);   // wake the client poller (satoru)
        return w;
    }
    return -1;
}

int KernelInject(int sd, const void* buf, int len) { return inject_core(sd, buf, len, nullptr); }

// kernel->client with ancillary data: same peer routing as KernelInject but
// the ControlMsg rides the ring frame, so the client's recvmsg installs the
// passed backing as a real memfd (SCM_RIGHTS out of an in-kernel server -
// the wl_keyboard.keymap fd is the first user). (satoru)
int KernelInjectMsg(int sd, const void* buf, int len, const ControlMsg* cm) {
    return inject_core(sd, buf, len, cm);
}

int ActiveCount() {
    int n = 0;
    for (int i = 0; i < UNIX_MAX_SOCKETS; i++) if (g_socks[i].in_use) n++;
    return n;
}

void DumpProcNetUnix(char* buf, int max_len) {
    const char* hdr = "Num       RefCount Protocol Flags    Type St Inode Path\n";
    int p = 0;
    for (int i = 0; hdr[i] && p < max_len - 1; i++) buf[p++] = hdr[i];
    for (int i = 0; i < UNIX_MAX_SOCKETS && p < max_len - 80; i++) {
        if (!g_socks[i].in_use) continue;
        // ffffffff: 00000002 00000000 00010000 0001 03 12345 /path
        const char* s = "ffffffff: 00000002 00000000 ";
        for (int k = 0; s[k]; k++) buf[p++] = s[k];
        const char* fl = g_socks[i].listening ? "00010000 0001 01 " : "00000000 0001 03 ";
        for (int k = 0; fl[k]; k++) buf[p++] = fl[k];
        // skip inode
        buf[p++] = '0'; buf[p++] = ' ';
        for (int k = 0; g_socks[i].path[k] && p < max_len - 2; k++) buf[p++] = g_socks[i].path[k];
        buf[p++] = '\n';
    }
    buf[p] = 0;
}

}  // namespace UnixSocket
// end (satoru)
