// minimal raw-protocol wayland wl_shm client - proves the kurono compositor's
// shared-memory render path end to end (memfd -> mmap -> SCM_RIGHTS -> wl_shm
// pool -> buffer -> commit). no libwayland: the wire protocol is hand-rolled so
// it builds as a tiny static-musl binary the kls runtime can exec. (satoru)
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <fcntl.h>

#define W 320
#define H 240
#define STRIDE (W * 4)
#define POOL (STRIDE * H)

// wayland object ids we hand out (1 = wl_display, fixed). (satoru)
enum { ID_DISPLAY = 1, ID_REGISTRY = 2, ID_SYNC = 3, ID_COMPOSITOR = 4,
       ID_XDGBASE = 5, ID_SHM = 6, ID_SURFACE = 7, ID_XDGSURF = 8,
       ID_TOPLEVEL = 9, ID_POOL = 10, ID_BUFFER = 11 };

static int g_fd;
static uint32_t g_name_compositor, g_name_xdgbase, g_name_shm;

static void logs(const char* s) { write(2, s, strlen(s)); }

// build + send one wayland message: header (obj, (size<<16)|op) + arg words.
static void wl_send(uint32_t obj, uint16_t op, const uint32_t* args, int nargs) {
    uint32_t m[64];
    int size = 8 + nargs * 4;
    m[0] = obj;
    m[1] = ((uint32_t)size << 16) | op;
    for (int i = 0; i < nargs; i++) m[2 + i] = args[i];
    write(g_fd, m, size);
}

// wl_registry.bind(name, "iface", version, new_id) - the new_id is encoded as
// (iface_string, version, id) on the wire. (satoru)
static void wl_bind(uint32_t name, const char* iface, uint32_t version, uint32_t newid) {
    uint32_t m[64];
    int p = 0;
    m[p++] = ID_REGISTRY;          // header filled below
    m[p++] = 0;
    m[p++] = name;
    int ilen = strlen(iface) + 1;  // include NUL
    m[p++] = ilen;
    // copy string padded to 4 bytes
    int words = (ilen + 3) / 4;
    char* dst = (char*)&m[p];
    memset(dst, 0, words * 4);
    memcpy(dst, iface, ilen - 1);
    p += words;
    m[p++] = version;
    m[p++] = newid;
    int size = p * 4;
    m[1] = ((uint32_t)size << 16) | 0;   // opcode 0 = bind
    write(g_fd, m, size);
}

// send wl_shm.create_pool(new_id, fd, size) with the fd in SCM_RIGHTS. (satoru)
static void shm_create_pool(int memfd) {
    uint32_t body[4];
    body[0] = ID_SHM;
    body[1] = ((uint32_t)(8 + 8) << 16) | 0;   // op 0, size 16: new_id + size
    body[2] = ID_POOL;
    body[3] = POOL;

    struct iovec iov = { body, sizeof(body) };
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof(cbuf));
    struct msghdr msg = {0};
    msg.msg_iov = &iov; msg.msg_iovlen = 1;
    msg.msg_control = cbuf; msg.msg_controllen = sizeof(cbuf);
    struct cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &memfd, sizeof(int));
    sendmsg(g_fd, &msg, 0);
}

int main(void) {
    // 1) connect to the compositor socket. (satoru)
    g_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_fd < 0) { logs("wl_shm_test: socket failed\n"); return 1; }
    struct sockaddr_un a; memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, "/system/run/user/1000/wayland-0");
    if (connect(g_fd, (struct sockaddr*)&a, sizeof(a)) < 0) {
        logs("wl_shm_test: connect failed\n"); return 1;
    }
    logs("wl_shm_test: connected\n");

    // 2) get_registry + sync; read the global advertisements. (satoru)
    uint32_t arg = ID_REGISTRY; wl_send(ID_DISPLAY, 1, &arg, 1);   // get_registry
    arg = ID_SYNC;             wl_send(ID_DISPLAY, 0, &arg, 1);    // sync

    uint8_t buf[8192];
    int have = 0, synced = 0, tries = 0;
    while (!synced && tries++ < 2000) {
        int n = read(g_fd, buf + have, sizeof(buf) - have);
        if (n <= 0) { usleep(1000); continue; }
        have += n;
        int off = 0;
        while (have - off >= 8) {
            uint32_t obj = *(uint32_t*)(buf + off);
            uint32_t hw  = *(uint32_t*)(buf + off + 4);
            int sz = hw >> 16, op = hw & 0xffff;
            if (sz < 8 || off + sz > have) break;
            if (obj == ID_REGISTRY && op == 0) {            // global
                uint32_t name = *(uint32_t*)(buf + off + 8);
                uint32_t ilen = *(uint32_t*)(buf + off + 12);
                const char* iface = (const char*)(buf + off + 16);
                if (ilen && ilen < 64) {
                    if (!strcmp(iface, "wl_compositor")) g_name_compositor = name;
                    else if (!strcmp(iface, "xdg_wm_base")) g_name_xdgbase = name;
                    else if (!strcmp(iface, "wl_shm")) g_name_shm = name;
                }
            } else if (obj == ID_SYNC && op == 0) {         // wl_callback.done
                synced = 1;
            }
            off += sz;
        }
        if (off) { memmove(buf, buf + off, have - off); have -= off; }
    }
    logs("wl_shm_test: registry synced\n");

    // 3) bind the globals. (satoru)
    wl_bind(g_name_compositor, "wl_compositor", 4, ID_COMPOSITOR);
    wl_bind(g_name_xdgbase,    "xdg_wm_base",   1, ID_XDGBASE);
    wl_bind(g_name_shm,        "wl_shm",        1, ID_SHM);

    // 4) surface -> xdg_surface -> xdg_toplevel, initial commit. (satoru)
    uint32_t a1 = ID_SURFACE; wl_send(ID_COMPOSITOR, 0, &a1, 1);   // create_surface
    uint32_t xa[2] = { ID_XDGSURF, ID_SURFACE };
    wl_send(ID_XDGBASE, 2, xa, 2);                                  // get_xdg_surface
    uint32_t ta = ID_TOPLEVEL; wl_send(ID_XDGSURF, 1, &ta, 1);      // get_toplevel
    wl_send(ID_SURFACE, 6, 0, 0);                                   // commit

    // 5) make a memfd, size it, map it, paint a solid color. (satoru)
    int memfd = syscall(SYS_memfd_create, "wl_shm", 0);
    if (memfd < 0) { logs("wl_shm_test: memfd_create failed\n"); return 1; }
    if (ftruncate(memfd, POOL) < 0) { logs("wl_shm_test: ftruncate failed\n"); return 1; }
    uint32_t* px = mmap(0, POOL, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (px == MAP_FAILED) { logs("wl_shm_test: mmap failed\n"); return 1; }
    for (int i = 0; i < W * H; i++) px[i] = 0xFF1E90FF;   // opaque dodger blue
    logs("wl_shm_test: painted buffer\n");

    // 6) create_pool (fd via SCM_RIGHTS) -> create_buffer -> attach -> commit.
    shm_create_pool(memfd);
    uint32_t cb[6] = { ID_BUFFER, 0, W, H, STRIDE, 1 };  // offset,w,h,stride,XRGB8888
    wl_send(ID_POOL, 0, cb, 6);                          // wl_shm_pool.create_buffer
    uint32_t at[3] = { ID_BUFFER, 0, 0 };
    wl_send(ID_SURFACE, 1, at, 3);                       // attach
    uint32_t dm[4] = { 0, 0, W, H };
    wl_send(ID_SURFACE, 2, dm, 4);                       // damage
    wl_send(ID_SURFACE, 6, 0, 0);                        // commit
    logs("wl_shm_test: committed frame\n");

    // 7) hold the frame on screen a few seconds, answering configure/ping, then
    // exit cleanly so the launching shell command returns. (satoru)
    for (int loop = 0; loop < 3000; loop++) {
        int n = read(g_fd, buf, sizeof(buf));
        if (n <= 0) { usleep(2000); continue; }
        int off = 0;
        while (n - off >= 8) {
            uint32_t obj = *(uint32_t*)(buf + off);
            uint32_t hw  = *(uint32_t*)(buf + off + 4);
            int sz = hw >> 16, op = hw & 0xffff;
            if (sz < 8 || off + sz > n) break;
            if (obj == ID_XDGSURF && op == 0) {                 // xdg_surface.configure
                uint32_t serial = *(uint32_t*)(buf + off + 8);
                wl_send(ID_XDGSURF, 4, &serial, 1);             // ack_configure
                wl_send(ID_SURFACE, 6, 0, 0);                   // commit
            } else if (obj == ID_XDGBASE && op == 0) {          // xdg_wm_base.ping
                uint32_t serial = *(uint32_t*)(buf + off + 8);
                wl_send(ID_XDGBASE, 3, &serial, 1);             // pong
            }
            off += sz;
        }
    }
    return 0;
}
// end (satoru)
