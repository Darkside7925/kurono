#include "unix_socket.h"
#include "../drivers/serial.h"
#include "../proc/scheduler.h"

namespace {

using namespace UnixSocket;

struct Socket;

struct Ring {
    uint8_t  data[UNIX_RING_BYTES];
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

Socket g_socks[UNIX_MAX_SOCKETS];

int alloc_sd() {
    for (int i = 0; i < UNIX_MAX_SOCKETS; i++) {
        if (!g_socks[i].in_use) {
            Socket* s = &g_socks[i];
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
            s->refs = 1;
            s->creds = {0, 0, 0};
            return i;
        }
    }
    return -1;
}

inline bool valid(int sd) {
    return sd >= 0 && sd < UNIX_MAX_SOCKETS && g_socks[sd].in_use;
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

int ring_write(Ring& r, const uint8_t* data, int len, const ControlMsg* cm,
               SockType type) {
    if (len <= 0) return 0;
    // Datagram / seqpacket: if no frame slot is available, fail rather
    // than silently merging into the previous record.
    if (type != UNIX_SOCK_STREAM) {
        if (r.frame_head - r.frame_tail >= Ring::FRAME_RING) return 0;
    }
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
    } else if (cm && (cm->passed_fd_count > 0 || cm->creds_valid)) {
        // Stream: attach cmsg only when there is something to deliver.
        if (r.frame_head - r.frame_tail < Ring::FRAME_RING) {
            uint32_t fi = r.frame_head % Ring::FRAME_RING;
            r.frames[fi].offset = off;
            r.frames[fi].length = (uint32_t)len;
            r.frames[fi].cmsg = *cm;
            r.frame_head++;
        }
    }
    r.head += (uint32_t)len;
    return len;
}

int ring_read(Ring& r, uint8_t* out, int max, ControlMsg* cm, SockType type) {
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
        consume = n;
        if (cm && r.frame_head != r.frame_tail) {
            uint32_t fi = r.frame_tail % Ring::FRAME_RING;
            *cm = r.frames[fi].cmsg;
            r.frame_tail++;
        } else if (cm) {
            *cm = {};
        }
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

}  // namespace

namespace UnixSocket {

void Init() {
    for (int i = 0; i < UNIX_MAX_SOCKETS; i++) g_socks[i].in_use = false;
    SerialLogger::Log("UnixSocket: AF_UNIX subsystem ready, ");
    SerialLogger::LogDec(UNIX_MAX_SOCKETS);
    SerialLogger::Log(" sd slots\r\n");
}

int Create(SockType type) {
    int sd = alloc_sd();
    if (sd < 0) return -1;
    g_socks[sd].type = type;
    return sd;
}

int Bind(int sd, const char* path) {
    if (!valid(sd) || !path) return -1;
    Socket& s = g_socks[sd];
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
    if (!valid(sd)) return -1;
    if (!g_socks[sd].bound) return -1;
    g_socks[sd].listening = true;
    return 0;
}

int Lookup(const char* path) {
    for (int i = 0; i < UNIX_MAX_SOCKETS; i++) {
        if (g_socks[i].in_use && g_socks[i].listening &&
            path_eq(g_socks[i].path, path)) return i;
    }
    return -1;
}

int Connect(int sd, const char* path) {
    if (!valid(sd) || !path) return -1;
    int srv = Lookup(path);
    if (srv < 0) return -111;          // ECONNREFUSED
    Socket& server = g_socks[srv];

    // Reject early if the backlog is full so we don't leak an accept slot.
    int next = (server.backlog_head + 1) % UNIX_MAX_BACKLOG;
    if (next == server.backlog_tail) return -11;       // EAGAIN

    int accept_sd = alloc_sd();
    if (accept_sd < 0) return -1;
    Socket& as = g_socks[accept_sd];
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
    {
        Process* cur = Scheduler::GetCurrentProcess();
        as.creds.pid = cur ? cur->pid : 0;
        as.creds.uid = 1000;
        as.creds.gid = 1000;
    }

    Socket& cs = g_socks[sd];
    cs.connected = true;
    cs.peer_sd = accept_sd;
    cs.type    = server.type;

    server.backlog[server.backlog_head] = accept_sd;
    server.backlog_head = next;

    if (server.on_connect) {
        server.on_connect(srv, accept_sd, server.user);
    }
    return 0;
}

int Accept(int sd, char* peer_path, int peer_path_len) {
    if (!valid(sd) || !g_socks[sd].listening) return -1;
    Socket& s = g_socks[sd];
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
    int a = alloc_sd();
    if (a < 0) return -1;
    int b = alloc_sd();
    if (b < 0) { g_socks[a].in_use = false; return -1; }
    g_socks[a].type = type; g_socks[b].type = type;
    g_socks[a].connected = g_socks[b].connected = true;
    g_socks[a].peer_sd = b;
    g_socks[b].peer_sd = a;
    g_socks[a].creds = {0, 0, 0};
    g_socks[b].creds = {0, 0, 0};
    *sd0 = a; *sd1 = b;
    return 0;
}

// pending ancillary for in-kernel servers: the message a server's on_data is
// parsing right now may have carried SCM_RIGHTS fds (e.g. a wl_shm pool fd). We
// stash the ControlMsg here so the handler can pull it via TakePendingControl()
// during the synchronous on_data call. (satoru)
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

// shared write core: enqueue to the peer ring and, for an in-kernel server peer,
// drive its on_data handler with the ancillary made available for this delivery.
static int send_core(int sd, const void* buf, int len, const ControlMsg& cm) {
    if (!valid(sd) || !g_socks[sd].connected) {
        // (satoru) TEMP: this EPIPE is silent (no [usend] below). catch a firefox
        // parent->content send that fails because the inherited channel lost its
        // `connected` flag - distinguishes a kernel socket bug from "never sent".
        SerialLogger::Log("[usend] sd="); SerialLogger::LogDec(sd);
        SerialLogger::Log(" EPIPE valid="); SerialLogger::LogDec(valid(sd) ? 1 : 0);
        SerialLogger::Log(" conn="); SerialLogger::LogDec((valid(sd) && g_socks[sd].connected) ? 1 : 0);
        SerialLogger::Log(" len="); SerialLogger::LogDec(len);
        SerialLogger::Log("\r\n");
        return -32;   // EPIPE
    }
    Socket& s = g_socks[sd];
    if (s.shutdown_wr) return -32;
    int peer = s.peer_sd;
    if (!valid(peer)) {
        SerialLogger::Log("[usend] sd="); SerialLogger::LogDec(sd);
        SerialLogger::Log(" NOPEER len="); SerialLogger::LogDec(len);
        SerialLogger::Log("\r\n");
        return -32;
    }
    Socket& ps = g_socks[peer];
    if (false && !ps.is_kernel_server) {  // (satoru) TEMP gated off: user<->user (firefox ipc) send trace
        SerialLogger::Log("[usend] sd="); SerialLogger::LogDec(sd);
        SerialLogger::Log(" -> peer="); SerialLogger::LogDec(peer);
        SerialLogger::Log(" len="); SerialLogger::LogDec(len);
        SerialLogger::Log("\r\n");
    }
    if (ps.shutdown_rd) return -32;
    int w = ring_write(ps.rx, (const uint8_t*)buf, len, &cm, s.type);
    if (w == 0 && len > 0) return -11;  // EAGAIN
    if (ps.is_kernel_server && ps.on_data && w > 0) {
        // (satoru) TEMP [sc] guard+diag for the firefox sibling-sendmsg #UD: a
        // kernel-server peer whose on_data is a garbage pointer (e.g. 3) means this
        // indirect call jumps into nothing -> #UD RIP=3. log the socket identity +
        // skip the insane handler so we don't execute address 0x3. debug.
        if ((uint64_t)(uintptr_t)ps.on_data < 0x1000ULL) {
            SerialLogger::Log("[sc] BAD on_data sd="); SerialLogger::LogDec(sd);
            SerialLogger::Log(" peer="); SerialLogger::LogDec(peer);
            SerialLogger::Log(" od="); SerialLogger::LogHex((uint32_t)((uint64_t)(uintptr_t)ps.on_data >> 32));
            SerialLogger::LogHex((uint32_t)(uintptr_t)ps.on_data);
            SerialLogger::Log(" type="); SerialLogger::LogDec((int)ps.type);
            SerialLogger::Log(" ks="); SerialLogger::LogDec(ps.is_kernel_server ? 1 : 0);
            SerialLogger::Log(" refs="); SerialLogger::LogDec(ps.refs);
            SerialLogger::Log(" inuse="); SerialLogger::LogDec(ps.in_use ? 1 : 0);
            SerialLogger::Log("\r\n");
            return w;   // bytes are queued in the ring; skip the corrupt handler (satoru)
        }
        // expose ancillary (passed fds / resolved shm) to the handler. (satoru)
        if (cm.passed_fd_count > 0) {
            g_pending_ctrl[peer]       = cm;
            g_pending_ctrl_valid[peer] = true;
            g_pending_ctrl_taken[peer] = 0;   // hand out fds one at a time (satoru)
        }
        // Drain into the kernel-side handler with no extra copy where possible:
        // for streams, walk the ring in contiguous spans and hand each span
        // directly to the callback.
        if (ps.type == UNIX_SOCK_STREAM) {
            while (ps.rx.avail() > 0) {
                uint32_t off = ps.rx.tail % UNIX_RING_BYTES;
                int span = ps.rx.avail();
                int contig = (int)(UNIX_RING_BYTES - off);
                if (span > contig) span = contig;
                ps.on_data(peer, ps.rx.data + off, span, ps.user);
                ps.rx.tail += (uint32_t)span;
            }
            // Drain stale frame metadata so it doesn't accumulate.
            ps.rx.frame_tail = ps.rx.frame_head;
        } else {
            uint8_t scratch[2048];
            int got = ring_read(ps.rx, scratch, sizeof(scratch), nullptr, ps.type);
            if (got > 0) ps.on_data(peer, scratch, got, ps.user);
        }
        g_pending_ctrl_valid[peer] = false;
    }
    return w;
}

int Send(int sd, const void* buf, int len, int flags,
         const int* pass_fds, int n_fds) {
    (void)flags;
    if (!valid(sd)) return -32;
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
    if (!valid(sd)) return -32;
    ControlMsg cm = cmin ? *cmin : ControlMsg{};
    cm.creds_valid = true;
    cm.peer_creds  = g_socks[sd].creds;
    return send_core(sd, buf, len, cm);
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

int Recv(int sd, void* buf, int len, int flags, ControlMsg* cmsg) {
    (void)flags;
    if (!valid(sd)) return -1;
    Socket& s = g_socks[sd];
    if (s.shutdown_rd) return 0;
    return ring_read(s.rx, (uint8_t*)buf, len, cmsg, s.type);
}

int Shutdown(int sd, int how) {
    if (!valid(sd)) return -1;
    if (how == 0 || how == 2) g_socks[sd].shutdown_rd = true;
    if (how == 1 || how == 2) g_socks[sd].shutdown_wr = true;
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
    if (!valid(sd)) return false;
    Socket& s = g_socks[sd];
    return s.connected && s.peer_sd < 0;
}

int Retain(int sd) {
    if (!valid(sd)) return -1;
    g_socks[sd].refs++;
    return 0;
}

int Close(int sd) {
    if (!valid(sd)) return -1;
    Socket& s = g_socks[sd];
    // a socketpair end inherited across fork is referenced by both the parent's
    // and the child's fd tables (same global sd). only the LAST close severs the
    // peer + frees the slot; an earlier close just drops this fd's reference, so
    // the still-live peer keeps working. (satoru)
    if (s.refs > 1) { s.refs--; return 0; }
    s.refs = 0;
    if (s.peer_sd >= 0 && valid(s.peer_sd)) {
        Socket& ps = g_socks[s.peer_sd];
        ps.shutdown_wr = true;     // peer's writes will start failing
        ps.peer_sd = -1;
        // Leave any buffered RX data on the peer so it can drain after
        // our close - connection-style EOF semantics.
    }
    s.in_use = false;
    return 0;
}

int GetSockName(int sd, char* path, int path_len) {
    if (!valid(sd) || !path || path_len <= 0) return -1;
    Socket& s = g_socks[sd];
    int i = 0;
    while (i < path_len - 1 && s.path[i]) { path[i] = s.path[i]; i++; }
    path[i] = 0;
    return i;
}

int GetPeerName(int sd, char* path, int path_len) {
    if (!valid(sd) || !valid(g_socks[sd].peer_sd) || !path) return -1;
    return GetSockName(g_socks[sd].peer_sd, path, path_len);
}

int GetPeerCred(int sd, Credentials* out) {
    if (!valid(sd) || !out) return -1;
    *out = g_socks[sd].creds;
    return 0;
}

void RegisterServer(int sd, ConnectionCallback on_conn, DataCallback on_data,
                    void* user) {
    if (!valid(sd)) return;
    Socket& s = g_socks[sd];
    s.on_connect = on_conn;
    s.on_data    = on_data;
    s.user       = user;
    s.is_kernel_server = true;
}

int PendingBytes(int sd) {
    if (!valid(sd)) return 0;
    return g_socks[sd].rx.avail();
}

// (satoru) TEMP diag: bytes waiting in this socket's PEER rx ring (what a write
// to `sd` delivers). lets the poll probe tell "polled fd has no data because the
// signaler wrote the read end and it went to the peer" apart from "nobody wrote
// it". returns -1 if unpaired. remove before commit.
int PeerPendingBytes(int sd) {
    if (!valid(sd)) return -1;
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
    if (!valid(sd)) return false;
    const Socket& s = g_socks[sd];
    return s.listening && s.backlog_head != s.backlog_tail;
}

int KernelInject(int sd, const void* buf, int len) {
    if (!valid(sd)) return -1;
    Socket& s = g_socks[sd];
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
    int peer = s.peer_sd;
    if (s.connected && valid(peer)) {
        Socket& ps = g_socks[peer];
        return ring_write(ps.rx, (const uint8_t*)buf, len, nullptr, ps.type);
    }
    return ring_write(s.rx, (const uint8_t*)buf, len, nullptr, s.type);
}

// kernel→client with ancillary data: same peer routing as KernelInject but
// the ControlMsg rides the ring frame, so the client's recvmsg installs the
// passed backing as a real memfd (SCM_RIGHTS out of an in-kernel server - 
// the wl_keyboard.keymap fd is the first user). (satoru)
int KernelInjectMsg(int sd, const void* buf, int len, const ControlMsg* cm) {
    if (!valid(sd)) return -1;
    Socket& s = g_socks[sd];
    int peer = s.peer_sd;
    if (s.connected && valid(peer)) {
        Socket& ps = g_socks[peer];
        return ring_write(ps.rx, (const uint8_t*)buf, len, cm, ps.type);
    }
    return ring_write(s.rx, (const uint8_t*)buf, len, cm, s.type);
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
