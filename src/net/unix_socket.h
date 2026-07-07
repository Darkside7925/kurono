#ifndef KURONO_NET_UNIX_SOCKET_H
#define KURONO_NET_UNIX_SOCKET_H

#include "../kernel/types.h"

// AF_UNIX (a.k.a. AF_LOCAL) socket subsystem.
//
// Supports:
//   * SOCK_STREAM, SOCK_DGRAM, SOCK_SEQPACKET
//   * Pathname sockets (bind to /system/run/user/1000/wayland-0 etc.)
//   * Abstract namespace sockets (sun_path[0] == '\0'; len-prefixed name)
//   * socketpair()  -  pre-connected pair, used by every fork()ed process
//   * SCM_RIGHTS  -  file descriptor passing in cmsg control buffers
//   * SCM_CREDENTIALS  -  peer pid/uid/gid passing
//
// Usage from the syscall layer:
//   int sd = UnixSocket::Create(SOCK_STREAM);
//   UnixSocket::Bind(sd, "/system/run/user/1000/wayland-0");
//   UnixSocket::Listen(sd, 16);
//   int cd = UnixSocket::Accept(sd, peer_path, sizeof(peer_path));
//   UnixSocket::Send(cd, buf, len, 0, fds_to_pass, n_fds);
//   UnixSocket::Recv(cd, buf, len, 0, fds_received, &n_fds);
//
// A kernel-side "auto-handler" callback may be attached to a listening
// socket so the Wayland/PulseAudio/DBus servers can dispatch incoming
// bytes inside the kernel without a user-space process.

namespace UnixSocket {

    enum SockType : uint8_t {
        UNIX_SOCK_STREAM    = 1,
        UNIX_SOCK_DGRAM     = 2,
        UNIX_SOCK_SEQPACKET = 5,
    };

    static const int UNIX_MAX_SOCKETS   = 64;
    static const int UNIX_MAX_BACKLOG   = 16;
    static const int UNIX_RING_BYTES    = 65536;
    static const int UNIX_MAX_PASSED_FD = 32;
    static const int UNIX_PATH_MAX      = 108;

    struct Credentials {
        uint32_t pid;
        uint32_t uid;
        uint32_t gid;
    };

    // Inline cmsg-style ancillary data attached to one send/recv call.
    struct ControlMsg {
        int      passed_fds[UNIX_MAX_PASSED_FD];
        // when a passed fd is a memfd/shm object, the sender resolves it to the
        // backing here so an in-kernel server (wayland wl_shm) can use the pages
        // directly  -  a raw client fd number is meaningless to the kernel. (satoru)
        uint64_t passed_shm_base[UNIX_MAX_PASSED_FD];
        uint64_t passed_shm_size[UNIX_MAX_PASSED_FD];
        // when a passed fd is an AF_UNIX socket (firefox's e10s ipc channel sent
        // to the fork server via SCM_RIGHTS), the sender records its global socket
        // sd here so the receiver installs a real refcounted alias of the SAME
        // socket instead of a /dev/null placeholder -- without this the passed ipc
        // channel was dead and the parent busy-spun on a reply. (satoru)
        int      passed_sd[UNIX_MAX_PASSED_FD];
        bool     passed_is_socket[UNIX_MAX_PASSED_FD];
        int      passed_fd_count;
        Credentials peer_creds;
        bool     creds_valid;
    };

    typedef void (*ConnectionCallback)(int server_sd, int new_client_sd,
                                       void* user);
    typedef void (*DataCallback)(int sd, const uint8_t* data, int len,
                                 void* user);

    void Init();

    int  Create(SockType type);
    int  Bind(int sd, const char* path);          // pathname or abstract
    int  Listen(int sd, int backlog);
    int  Accept(int sd, char* peer_path, int peer_path_len);
    int  Connect(int sd, const char* path);
    int  Pair(SockType type, int* sd0, int* sd1);

    // Read / write with optional ancillary data.
    int  Send(int sd, const void* buf, int len, int flags,
              const int* pass_fds = nullptr, int n_fds = 0);
    // Send with a fully-built ControlMsg (carries resolved shm backings for
    // memfd fds passed via SCM_RIGHTS). Used by the sendmsg syscall path.
    int  SendMsg(int sd, const void* buf, int len, int flags, const ControlMsg* cm);
    int  Recv(int sd, void* buf, int len, int flags,
              ControlMsg* cmsg = nullptr);

    // An in-kernel server (wayland) calls this from inside its on_data handler
    // to retrieve the ancillary data (passed fds / shm backings) that arrived
    // with the message it is currently parsing. Returns true + fills *out, and
    // clears the pending slot. (satoru)
    bool TakePendingControl(int sd, ControlMsg* out);
    int  Shutdown(int sd, int how);
    int  Close(int sd);
    // bump the open-fd refcount. the fork fd-table copy calls this for an
    // inherited socket so a later close of one fd does not tear down the peer
    // while another fd (the child's) still references the same socket. (satoru)
    int  Retain(int sd);

    int  GetSockName(int sd, char* path, int path_len);
    int  GetPeerName(int sd, char* path, int path_len);
    int  GetPeerCred(int sd, Credentials* out);

    // In-kernel server hooks  -  Wayland/PulseAudio/DBus register here so
    // they can dispatch protocol traffic without a user-space process.
    void RegisterServer(int sd,
                        ConnectionCallback on_connect,
                        DataCallback       on_data,
                        void*              user);
    int  PendingBytes(int sd);
    bool HasPendingConnection(int sd);   // listen fd has a backlog conn -> POLLIN (satoru)
    int  KernelInject(int sd, const void* buf, int len);  // kernel→client
    // kernel→client with ancillary data (SCM_RIGHTS out of an in-kernel
    // server; the wl_keyboard.keymap fd path). (satoru)
    int  KernelInjectMsg(int sd, const void* buf, int len, const ControlMsg* cm);

    // Resolve a socket path back to a listening sd, or -1.
    int  Lookup(const char* path);

    // Statistics for /proc/net/unix.
    int  ActiveCount();
    void DumpProcNetUnix(char* buf, int max_len);
}

#endif
