// kui_r3_audio.cpp: kui::Audio for ring 3, a small pulseaudio native-protocol client (satoru)
// talks to the in-kernel pulse compatible server (drivers/pulse_server.cpp) over the unix socket
// at /system/run/user/1000/pulse/native. interleaved s16, paced from the sink's own queue depth,
// CORK for pause, DELETE on close and GET_PLAYBACK_LATENCY for Queued. (satoru)
#include "../kui.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace kui {
namespace Audio {
namespace {

const char* kSockPath = "/system/run/user/1000/pulse/native";
constexpr uint32_t PA_VERSION = 12;

// pa command codes (the upstream numbering the kernel server speaks) (satoru)
enum {
    CMD_ERROR   = 0,
    CMD_REPLY   = 2,
    CMD_CREATE  = 3,
    CMD_DELETE  = 4,
    CMD_AUTH    = 8,
    CMD_SETNAME = 9,
    CMD_LATENCY = 14,
    CMD_CORK    = 41,
    CMD_REQUEST = 61,
};

// pa tagstruct tags (satoru)
constexpr uint8_t T_STRING = 't';
constexpr uint8_t T_STRING_NULL = 'N';
constexpr uint8_t T_U32 = 'L';
constexpr uint8_t T_U64 = 'R';
constexpr uint8_t T_S64 = 'r';
constexpr uint8_t T_SAMPLE = 'a';
constexpr uint8_t T_TRUE = '1';
constexpr uint8_t T_FALSE = '0';
constexpr uint8_t T_TIMEVAL = 'T';
constexpr uint8_t T_USEC = 'U';
constexpr uint8_t T_CHMAP = 'm';
constexpr uint8_t T_CVOL = 'v';

constexpr uint8_t PA_SAMPLE_S16LE = 3;
const int MAX_STREAMS = 8;
const size_t RING_BYTES = 262144;   // ~2.7 s of 48 khz stereo s16 (satoru)

struct Stream {
    bool     used;
    uint32_t channel;
    int      rate;
    int      channels;
    int      frame_bytes;
    uint8_t* ring;
    size_t   cap;
    size_t   wr;              // bytes queued by the app (satoru)
    size_t   rd;              // bytes already sent to the server (satoru)
    int      tlength;         // bytes the server wants kept queued (satoru)
    uint32_t q_ms;            // when the sink queue was last measured (satoru)
    uint64_t server_queued;   // source frames the sink has buffered (satoru)
    bool     paused;
    int      vol;
};

Stream g_st[MAX_STREAMS];
int    g_fd = -1;
bool   g_dead = false;
uint32_t g_next_tag = 1;
uint8_t g_rx[65536];
int     g_rx_len = 0;
uint8_t g_tx[2048];
uint8_t g_data[16384];

// synchronous reply capture for one command tag (satoru)
uint32_t g_wait_tag = 0;
bool     g_wait_done = false;
uint32_t g_wait_cmd = 0;
uint8_t  g_wait_buf[1024];
int      g_wait_len = 0;

inline uint32_t rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
inline uint64_t rd64(const uint8_t* p) {
    return ((uint64_t)rd32(p) << 32) | (uint64_t)rd32(p + 4);
}
inline void wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
inline int put_u32(uint8_t* p, uint32_t v) { p[0] = T_U32; wr32(p + 1, v); return 5; }
inline int put_bool(uint8_t* p, bool v) { p[0] = v ? T_TRUE : T_FALSE; return 1; }
inline int put_str_null(uint8_t* p) { p[0] = T_STRING_NULL; return 1; }
inline int put_string(uint8_t* p, const char* s) {
    p[0] = T_STRING;
    int n = 0; while (s[n]) n++;
    for (int i = 0; i < n; i++) p[1 + i] = (uint8_t)s[i];
    p[1 + n] = 0;
    return 2 + n;
}
inline int put_sample_spec(uint8_t* p, uint8_t fmt, uint8_t ch, uint32_t rate) {
    p[0] = T_SAMPLE; p[1] = fmt; p[2] = ch; wr32(p + 3, rate); return 7;
}
inline int put_chmap(uint8_t* p, uint8_t n) {
    p[0] = T_CHMAP; p[1] = n;
    if (n == 1) { p[2] = 0; return 3; }
    for (uint8_t i = 0; i < n; i++) p[2 + i] = (uint8_t)(1 + i);
    return 2 + n;
}
inline int put_cvolume(uint8_t* p, uint8_t n) {
    p[0] = T_CVOL; p[1] = n;
    for (uint8_t i = 0; i < n; i++) wr32(p + 2 + i * 4, 0x10000u);
    return 2 + n * 4;
}
inline int put_timeval(uint8_t* p, uint32_t sec, uint32_t usec) {
    p[0] = T_TIMEVAL; wr32(p + 1, sec); wr32(p + 5, usec); return 9;
}

bool send_all(const uint8_t* p, int n) {
    int off = 0;
    while (off < n && !g_dead && g_fd >= 0) {
        int r = (int)send(g_fd, p + off, (size_t)(n - off), 0);
        if (r > 0) { off += r; continue; }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pf = { g_fd, POLLOUT, 0 };
            poll(&pf, 1, 20);
            continue;
        }
        if (r < 0 && errno == EINTR) continue;
        g_dead = true;
        break;
    }
    return off == n;
}

bool send_cmd(uint32_t cmd, uint32_t tag, const uint8_t* args, int alen) {
    if (g_fd < 0 || g_dead) return false;
    int plen = 10 + alen;
    if (20 + plen > (int)sizeof(g_tx)) return false;
    wr32(g_tx + 0, (uint32_t)plen);
    wr32(g_tx + 4, 0xFFFFFFFFu);   // command channel (satoru)
    wr32(g_tx + 8, 0);
    wr32(g_tx + 12, 0);
    wr32(g_tx + 16, 0);
    int p = 20;
    p += put_u32(g_tx + p, cmd);
    p += put_u32(g_tx + p, tag);
    for (int i = 0; i < alen; i++) g_tx[p++] = args[i];
    return send_all(g_tx, p);
}

void apply_gain(int16_t* s, int n, int vol) {
    if (vol >= 100) return;
    for (int i = 0; i < n; i++) {
        int v = ((int)s[i] * vol) / 100;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        s[i] = (int16_t)v;
    }
}

void service(Stream& s);

void handle_command(uint32_t cmd, uint32_t tag, const uint8_t* a, int n) {
    if (cmd == CMD_REQUEST) return;   // we pace from the sink queue, not the server's requests (satoru)
    if (g_wait_tag && tag == g_wait_tag) {
        g_wait_cmd = cmd;
        int copy = n;
        if (copy > (int)sizeof(g_wait_buf)) copy = (int)sizeof(g_wait_buf);
        for (int i = 0; i < copy; i++) g_wait_buf[i] = a[i];
        g_wait_len = copy;
        g_wait_done = true;
    }
}

void pulse_pump() {
    if (g_fd < 0 || g_dead) return;
    for (;;) {
        int room = (int)sizeof(g_rx) - g_rx_len;
        if (room <= 0) break;
        int r = (int)recv(g_fd, g_rx + g_rx_len, (size_t)room, 0);
        if (r > 0) { g_rx_len += r; continue; }
        if (r == 0) { g_dead = true; break; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        g_dead = true;
        break;
    }
    int off = 0;
    while (g_rx_len - off >= 20) {
        uint32_t plen = rd32(g_rx + off);
        if ((int)plen > (int)sizeof(g_rx) - 20) { g_rx_len = 0; return; }
        if (g_rx_len - off < (int)(20 + plen)) break;
        uint32_t channel = rd32(g_rx + off + 4);
        const uint8_t* p = g_rx + off + 20;
        if (channel == 0xFFFFFFFFu && plen >= 10 && p[0] == T_U32 && p[5] == T_U32)
            handle_command(rd32(p + 1), rd32(p + 6), p + 10, (int)plen - 10);
        off += 20 + plen;
    }
    if (off > 0) {
        int rem = g_rx_len - off;
        if (rem > 0) memmove(g_rx, g_rx + off, (size_t)rem);
        g_rx_len = rem;
    }
}

bool wait_reply(uint32_t tag, int timeout_ms) {
    g_wait_tag = tag;
    g_wait_done = false;
    g_wait_cmd = 0xFFFFFFFFu;
    g_wait_len = 0;
    uint32_t t0 = NowMs();
    while (!g_wait_done && !g_dead) {
        pulse_pump();
        if (g_wait_done) break;
        uint32_t el = NowMs() - t0;
        if (el >= (uint32_t)timeout_ms) break;
        int to = timeout_ms - (int)el;
        if (to > 50) to = 50;
        struct pollfd pf = { g_fd, POLLIN, 0 };
        poll(&pf, 1, to);
    }
    g_wait_tag = 0;
    bool ok = g_wait_done && g_wait_cmd == CMD_REPLY;
    g_wait_done = false;
    return ok;
}

bool connect_server() {
    if (g_fd >= 0) {
        if (!g_dead) return true;
        close(g_fd);
        g_fd = -1;
    }
    g_dead = false;
    g_rx_len = 0;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    int i = 0;
    while (kSockPath[i] && i < (int)sizeof(sa.sun_path) - 1) { sa.sun_path[i] = kSockPath[i]; i++; }
    sa.sun_path[i] = 0;
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) { close(fd); return false; }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    g_fd = fd;

    uint8_t arg[64];
    int n = put_u32(arg, PA_VERSION);
    uint32_t tag = g_next_tag++;
    if (!send_cmd(CMD_AUTH, tag, arg, n) || !wait_reply(tag, 1000)) goto fail;

    tag = g_next_tag++;
    n = put_string(arg, "kurono-kui");
    if (!send_cmd(CMD_SETNAME, tag, arg, n) || !wait_reply(tag, 1000)) goto fail;
    return true;

fail:
    close(g_fd);
    g_fd = -1;
    g_dead = false;
    g_rx_len = 0;
    return false;
}

void poll_latency(Stream& s) {
    if (s.paused || s.channel == 0) return;
    uint32_t now = NowMs();
    if (s.q_ms && now - s.q_ms < 10u) return;
    s.q_ms = now;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint8_t a[16];
    int n = put_u32(a, s.channel);
    n += put_timeval(a + n, (uint32_t)ts.tv_sec, (uint32_t)(ts.tv_nsec / 1000));
    uint32_t tag = g_next_tag++;
    if (send_cmd(CMD_LATENCY, tag, a, n) && wait_reply(tag, 60)) {
        // the reply's write and read indices are this stream's own client bytes; the
        // sink usec also counts the backend's always-full silence ring, which is not
        // ours and never drains, so pace from wr - rd instead. (satoru)
        if (g_wait_len >= 55 && g_wait_buf[37] == T_S64 && g_wait_buf[46] == T_S64) {
            uint64_t wr = rd64(g_wait_buf + 38);
            uint64_t rd = rd64(g_wait_buf + 47);
            uint64_t bytes = wr > rd ? wr - rd : 0;
            s.server_queued = bytes / (uint64_t)s.frame_bytes;
        }
    }
}

void service(Stream& s) {
    if (!s.used || g_fd < 0 || g_dead || s.paused) return;
    // keep about tlength bytes in the mixer ring. this does not depend on the server's
    // pacing pass, which only runs in the audio process. (satoru)
    poll_latency(s);
    uint64_t queued_bytes = s.server_queued * (uint64_t)s.frame_bytes;
    uint64_t target = (uint64_t)(s.tlength > 4096 ? s.tlength : 4096);
    if (queued_bytes >= target) return;
    uint64_t budget = target - queued_bytes;
    while (budget >= (uint64_t)s.frame_bytes && s.wr > s.rd) {
        size_t avail = s.wr - s.rd;
        size_t nb = avail;
        if (nb > budget) nb = (size_t)budget;
        if (nb > 8192) nb = 8192;   // the server's receive parser drops packets larger than 8 kb (satoru)
        if (nb > sizeof(g_data)) nb = sizeof(g_data);
        nb -= nb % (size_t)s.frame_bytes;
        if (nb == 0) break;
        uint8_t hdr[20];
        wr32(hdr + 0, (uint32_t)nb);
        wr32(hdr + 4, s.channel);
        wr32(hdr + 8, 0);
        wr32(hdr + 12, 0);
        wr32(hdr + 16, 0);
        size_t pos = s.rd % s.cap;
        size_t first = nb;
        if (first > s.cap - pos) first = s.cap - pos;
        memcpy(g_data, s.ring + pos, first);
        if (nb > first) memcpy(g_data + first, s.ring, nb - first);
        if (s.vol < 100) apply_gain((int16_t*)g_data, (int)(nb / 2), s.vol);
        if (!send_all(hdr, 20) || !send_all(g_data, (int)nb)) { g_dead = true; return; }
        s.rd += nb;
        budget -= nb;
        s.server_queued += nb / (size_t)s.frame_bytes;
    }
}

}  // namespace

int Open(int rate, int channels) {
    if (rate < 4000 || rate > 192000) return -1;
    if (channels < 1) channels = 1;
    if (channels > 2) channels = 2;
    int slot = -1;
    for (int i = 0; i < MAX_STREAMS; i++) if (!g_st[i].used) { slot = i; break; }
    if (slot < 0) return -1;
    if (!connect_server()) return -1;

    Stream& s = g_st[slot];
    s.ring = (uint8_t*)malloc(RING_BYTES);
    if (!s.ring) return -1;
    s.used = true;
    s.channel = 0;
    s.rate = rate;
    s.channels = channels;
    s.frame_bytes = channels * 2;
    s.cap = RING_BYTES;
    s.wr = s.rd = 0;
    s.tlength = 8192;
    s.q_ms = 0;
    s.server_queued = 0;
    s.paused = false;
    s.vol = 100;

    uint8_t a[256];
    int n = 0;
    n += put_sample_spec(a + n, PA_SAMPLE_S16LE, (uint8_t)channels, (uint32_t)rate);
    n += put_chmap(a + n, (uint8_t)channels);
    n += put_u32(a + n, 0xFFFFFFFFu);   // sink index: default (satoru)
    n += put_str_null(a + n);           // sink name
    n += put_u32(a + n, 0xFFFFFFFFu);   // maxlength
    n += put_bool(a + n, false);        // corked
    n += put_u32(a + n, 0xFFFFFFFFu);   // tlength
    n += put_u32(a + n, 0xFFFFFFFFu);   // prebuf
    n += put_u32(a + n, 0xFFFFFFFFu);   // minreq
    n += put_u32(a + n, 0xFFFFFFFFu);   // syncid
    n += put_cvolume(a + n, (uint8_t)channels);

    uint32_t tag = g_next_tag++;
    bool ok = send_cmd(CMD_CREATE, tag, a, n) && wait_reply(tag, 2000);
    if (ok && g_wait_len >= 25 && g_wait_buf[0] == T_U32 && g_wait_buf[5] == T_U32) {
        s.channel = rd32(g_wait_buf + 1);
        s.tlength = (int)rd32(g_wait_buf + 21);
        if (s.tlength < 4096) s.tlength = 4096;
        if (s.tlength > 65536) s.tlength = 65536;
    } else {
        ok = false;
    }
    if (!ok || s.channel == 0) {
        free(s.ring);
        s.ring = nullptr;
        s.used = false;
        return -1;
    }
    return slot + 1;
}

int Write(int id, const int16_t* frames, int nframes) {
    if (id <= 0 || id > MAX_STREAMS || !frames || nframes <= 0) return 0;
    Stream& s = g_st[id - 1];
    if (!s.used || !s.ring) return 0;
    size_t bytes = (size_t)nframes * (size_t)s.frame_bytes;
    size_t used = s.wr - s.rd;
    if (used >= s.cap) bytes = 0;
    else if (bytes > s.cap - used) bytes = s.cap - used;
    bytes -= bytes % (size_t)s.frame_bytes;
    if (bytes > 0) {
        const uint8_t* src = (const uint8_t*)frames;
        size_t pos = s.wr % s.cap;
        size_t first = bytes;
        if (first > s.cap - pos) first = s.cap - pos;
        memcpy(s.ring + pos, src, first);
        if (bytes > first) memcpy(s.ring, src + first, bytes - first);
        s.wr += bytes;
    }
    pulse_pump();
    service(s);
    return (int)(bytes / (size_t)s.frame_bytes);
}

int Queued(int id) {
    if (id <= 0 || id > MAX_STREAMS) return 0;
    Stream& s = g_st[id - 1];
    if (!s.used) return 0;
    pulse_pump();
    service(s);
    uint64_t pending = (uint64_t)(s.wr - s.rd);
    uint64_t frames = pending / (uint64_t)s.frame_bytes + s.server_queued;
    return frames > 0x7fffffffull ? 0x7fffffff : (int)frames;
}

void Pause(int id, bool on) {
    if (id <= 0 || id > MAX_STREAMS) return;
    Stream& s = g_st[id - 1];
    if (!s.used || s.paused == on) return;
    uint8_t a[8];
    int n = put_u32(a, s.channel);
    n += put_bool(a + n, on);
    uint32_t tag = g_next_tag++;
    if (send_cmd(CMD_CORK, tag, a, n)) wait_reply(tag, 400);
    s.paused = on;
}

void SetVolume(int id, int pct) {
    if (id <= 0 || id > MAX_STREAMS) return;
    Stream& s = g_st[id - 1];
    if (!s.used) return;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s.vol = pct;
}

void Close(int id) {
    if (id <= 0 || id > MAX_STREAMS) return;
    Stream& s = g_st[id - 1];
    if (!s.used) return;
    if (s.channel && g_fd >= 0 && !g_dead) {
        uint8_t a[8];
        int n = put_u32(a, s.channel);
        uint32_t tag = g_next_tag++;
        if (send_cmd(CMD_DELETE, tag, a, n)) wait_reply(tag, 400);
    }
    free(s.ring);
    s.ring = nullptr;
    s.cap = 0;
    s.wr = s.rd = 0;
    s.tlength = 0;
    s.q_ms = 0;
    s.server_queued = 0;
    s.channel = 0;
    s.used = false;
}

const char* BackendName() {
    return (g_fd >= 0 && !g_dead) ? "pulse" : "none";
}

}  // namespace Audio
}  // namespace kui

// end (satoru)
