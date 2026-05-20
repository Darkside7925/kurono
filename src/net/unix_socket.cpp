#include "unix_socket.h"
#include "../drivers/serial.h"

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

int ring_write(Ring& r, const uint8_t* data, int len, const ControlMsg* cm,
               SockType type) {
    if (len <= 0) return 0;
    int can = r.free();
    if (can <= 0) return 0;
    if (len > can) len = can;
    for (int i = 0; i < len; i++) {
        r.data[(r.head + i) % UNIX_RING_BYTES] = data[i];
    }
    if (type != UNIX_SOCK_STREAM) {
        uint32_t fi = r.frame_head % Ring::FRAME_RING;
        r.frames[fi].offset = r.head % UNIX_RING_BYTES;
        r.frames[fi].length = (uint32_t)len;
        if (cm) r.frames[fi].cmsg = *cm;
        else    r.frames[fi].cmsg = {};
        r.frame_head++;
    } else if (cm && cm->passed_fd_count > 0) {
        // Stream: attach cmsg to *first* frame slot anyway so SCM_RIGHTS
        // arrives with the next read.
        uint32_t fi = r.frame_head % Ring::FRAME_RING;
        r.frames[fi].offset = r.head % UNIX_RING_BYTES;
        r.frames[fi].length = (uint32_t)len;
        r.frames[fi].cmsg = *cm;
        r.frame_head++;
    }
    r.head += (uint32_t)len;
    return len;
}

int ring_read(Ring& r, uint8_t* out, int max, ControlMsg* cm, SockType type) {
    int avail = r.avail();
    if (avail <= 0) return 0;
    int n = avail;
    if (type == UNIX_SOCK_DGRAM || type == UNIX_SOCK_SEQPACKET) {
        if (r.frame_head == r.frame_tail) return 0;
        uint32_t fi = r.frame_tail % Ring::FRAME_RING;
        n = (int)r.frames[fi].length;
        if (n > max) n = max;
        if (cm) *cm = r.frames[fi].cmsg;
        r.frame_tail++;
    } else {
        if (n > max) n = max;
        if (cm && r.frame_head != r.frame_tail) {
            uint32_t fi = r.frame_tail % Ring::FRAME_RING;
            *cm = r.frames[fi].cmsg;
            r.frame_tail++;
        } else if (cm) {
            *cm = {};
        }
    }
    for (int i = 0; i < n; i++) {
        out[i] = r.data[(r.tail + i) % UNIX_RING_BYTES];
    }
    r.tail += (uint32_t)n;
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

    // Build the server-side accept slot now.
    int accept_sd = alloc_sd();
    if (accept_sd < 0) return -1;
    Socket& as = g_socks[accept_sd];
    as.type    = server.type;
    as.connected = true;
    as.peer_sd = sd;
    for (int i = 0; i < UNIX_PATH_MAX; i++) as.path[i] = server.path[i];

    Socket& cs = g_socks[sd];
    cs.connected = true;
    cs.peer_sd = accept_sd;
    cs.type    = server.type;

    // Push to backlog.
    int next = (server.backlog_head + 1) % UNIX_MAX_BACKLOG;
    if (next == server.backlog_tail) return -11;       // EAGAIN, full
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
    *sd0 = a; *sd1 = b;
    return 0;
}

int Send(int sd, const void* buf, int len, int flags,
         const int* pass_fds, int n_fds) {
    (void)flags;
    if (!valid(sd) || !g_socks[sd].connected) return -32;   // EPIPE
    Socket& s = g_socks[sd];
    if (s.shutdown_wr) return -32;
    int peer = s.peer_sd;
    if (!valid(peer)) return -32;
    Socket& ps = g_socks[peer];
    ControlMsg cm = {};
    if (pass_fds && n_fds > 0) {
        if (n_fds > UNIX_MAX_PASSED_FD) n_fds = UNIX_MAX_PASSED_FD;
        for (int i = 0; i < n_fds; i++) cm.passed_fds[i] = pass_fds[i];
        cm.passed_fd_count = n_fds;
    }
    cm.creds_valid = true;
    cm.peer_creds  = s.creds;
    int w = ring_write(ps.rx, (const uint8_t*)buf, len, &cm, s.type);
    if (ps.is_kernel_server && ps.on_data && w > 0) {
        // Drain immediately into the kernel-side handler.
        uint8_t scratch[2048];
        int total = 0;
        while (true) {
            int got = ring_read(ps.rx, scratch, sizeof(scratch), nullptr,
                                ps.type);
            if (got <= 0) break;
            ps.on_data(peer, scratch, got, ps.user);
            total += got;
            if (ps.type != UNIX_SOCK_STREAM) break;
        }
        (void)total;
    }
    return w;
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

int Close(int sd) {
    if (!valid(sd)) return -1;
    Socket& s = g_socks[sd];
    if (s.peer_sd >= 0 && valid(s.peer_sd)) {
        g_socks[s.peer_sd].shutdown_rd = true;
        g_socks[s.peer_sd].peer_sd = -1;
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

int KernelInject(int sd, const void* buf, int len) {
    if (!valid(sd)) return -1;
    Socket& s = g_socks[sd];
    return ring_write(s.rx, (const uint8_t*)buf, len, nullptr, s.type);
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
