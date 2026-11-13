// audiod - Kurono's ring-3 audio daemon (kurono.r3audio=1).
//
// Phase 3 of "separate everything": the pulseaudio-compatible server and the
// per-client mixing used to live in the kernel (drivers/pulse_server.cpp +
// the per-stream part of audio_mixer.cpp). Here they run as an ordinary musl
// process. Linux clients (Firefox, GTK apps) connect to the same unix socket
// as before; audiod converts + resamples each client stream to 48 kHz s16
// stereo, sums them, and hands the result to the kernel as ONE stream of the
// kernel mixer through SYS_KAUD_CALL. The kernel keeps the hardware (HDA/AC97/
// USB backend, DMA ring) plus master volume / EQ / limiter, so those and the
// in-kernel clients (Media Player, beeps) are untouched.
//
// Protocol: PulseAudio native, negotiated version 12, same command subset and
// reply layouts as the in-kernel server it replaces.

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <pthread.h>

#define SYS_KAUD_CALL 0x4B415544L
enum { OP_OPEN = 1, OP_WRITE, OP_QUEUED, OP_CONSUMED, OP_LATENCY, OP_CLOSE, OP_PING };

#define SOCK_PATH "/system/run/user/1000/pulse/native"
#define MAX_CLIENTS 16
#define MAX_STREAMS 8
#define RING_FRAMES 32768u
#define OUT_RATE 48000u
#define KQ_TARGET 4096u      // keep ~85 ms queued in the kernel stream
#define IDLE_CLOSE_MS 1500

static void logs(const char* s) { (void)!write(2, s, strlen(s)); }
static void logd(const char* tag, long v) {
    char b[96]; int n = snprintf(b, sizeof b, "%s%ld\r\n", tag, v);
    (void)!write(2, b, (size_t)n);
}

static long kaud(unsigned op, unsigned long a0, unsigned long a1) {
    return syscall(SYS_KAUD_CALL, op, a0, a1);
}

static uint64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

// ---- pa constants ---------------------------------------------------------
enum {
    TAG_STRING = 't', TAG_STRING_NULL = 'N', TAG_U32 = 'L', TAG_S64 = 'r',
    TAG_SAMPLE_SPEC = 'a', TAG_TRUE = '1', TAG_FALSE = '0', TAG_TIMEVAL = 'T',
    TAG_USEC = 'U', TAG_CHANNEL_MAP = 'm', TAG_CVOLUME = 'v',
};
enum {
    CMD_ERROR = 0, CMD_REPLY = 2, CMD_CREATE_PLAYBACK = 3, CMD_DELETE_PLAYBACK = 4,
    CMD_EXIT = 7, CMD_AUTH = 8, CMD_SET_CLIENT_NAME = 9, CMD_DRAIN = 12, CMD_STAT = 13,
    CMD_GET_PLAYBACK_LATENCY = 14, CMD_GET_SERVER_INFO = 20, CMD_GET_SINK_INFO = 21,
    CMD_GET_SINK_INFO_LIST = 22, CMD_GET_SOURCE_INFO = 23, CMD_GET_SOURCE_INFO_LIST = 24,
    CMD_SUBSCRIBE = 35, CMD_CORK = 41, CMD_FLUSH = 42, CMD_TRIGGER = 43,
    CMD_SET_STREAM_NAME = 46, CMD_REQUEST = 61,
};
#define PA_VERSION 12
#define PA_ERR_NOTSUPPORTED 19
#define PA_ERR_NORESOURCE 5
#define PA_ERR_INVALID 2
enum { F_U8 = 0, F_S16 = 3, F_F32 = 5, F_S32 = 7 };

typedef struct {
    int in_use;
    uint32_t channel, rate;
    uint8_t ch, fmt;
    uint32_t frame_bytes, tlength, minreq;
    uint64_t requested_bytes, written_bytes, pulled_frames;
    int corked;
    int16_t* ring;                 // stereo s16 at OUT_RATE
    uint32_t rd, wr, count;
    uint32_t step, pos;            // resampler: 16.16 input frames per output frame
    int16_t prev[2];
    int have_prev;
} Stream;

typedef struct {
    int in_use, fd;
    uint8_t rx[16384];
    int rx_len;
    Stream st[MAX_STREAMS];
} Client;

static Client g_cl[MAX_CLIENTS];
static uint32_t g_next_channel = 1;
static int g_kopen = 0;            // kernel stream open
static uint64_t g_last_data_ms = 0;
static uint64_t g_out_frames = 0;  // frames handed to the kernel in total

static inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void put_be32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static inline void put_be64(uint8_t* p, uint64_t v) { put_be32(p, v >> 32); put_be32(p + 4, (uint32_t)v); }

// ---- socket tx -------------------------------------------------------------
static int send_all(Client* c, const uint8_t* b, int len) {
    int off = 0, spins = 0;
    while (off < len) {
        ssize_t n = send(c->fd, b + off, (size_t)(len - off), MSG_NOSIGNAL);
        if (n > 0) { off += (int)n; spins = 0; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EINTR) && ++spins < 200) { sched_yield(); continue; }
        return -1;
    }
    return 0;
}
static void send_packet(Client* c, const uint8_t* payload, int plen) {
    uint8_t buf[20 + 1100];
    if (plen > 1100) return;
    put_be32(buf, (uint32_t)plen);
    put_be32(buf + 4, 0xFFFFFFFFu);
    memset(buf + 8, 0, 12);
    memcpy(buf + 20, payload, (size_t)plen);
    send_all(c, buf, 20 + plen);
}

// ---- tagstruct writers ----------------------------------------------------
static int put_u32(uint8_t* p, uint32_t v) { p[0] = TAG_U32; put_be32(p + 1, v); return 5; }
static int put_string(uint8_t* p, const char* s) {
    int n = (int)strlen(s); p[0] = TAG_STRING; memcpy(p + 1, s, (size_t)n + 1); return 2 + n;
}
static int put_str_null(uint8_t* p) { p[0] = TAG_STRING_NULL; return 1; }
static int put_bool(uint8_t* p, int v) { p[0] = v ? TAG_TRUE : TAG_FALSE; return 1; }
static int put_sample_spec(uint8_t* p, uint8_t f, uint8_t ch, uint32_t rate) {
    p[0] = TAG_SAMPLE_SPEC; p[1] = f; p[2] = ch; put_be32(p + 3, rate); return 7;
}
static int put_cmap_stereo(uint8_t* p) { p[0] = TAG_CHANNEL_MAP; p[1] = 2; p[2] = 1; p[3] = 2; return 4; }
static int put_cmap_n(uint8_t* p, uint8_t n) {
    p[0] = TAG_CHANNEL_MAP; p[1] = n;
    if (n == 1) { p[2] = 0; return 3; }
    for (uint8_t i = 0; i < n; i++) p[2 + i] = (uint8_t)(1 + i);
    return 2 + n;
}
static int put_cvol(uint8_t* p, uint8_t n) {
    p[0] = TAG_CVOLUME; p[1] = n;
    for (uint8_t i = 0; i < n; i++) put_be32(p + 2 + i * 4, 0x10000u);
    return 2 + n * 4;
}
static int put_usec(uint8_t* p, uint64_t u) { p[0] = TAG_USEC; put_be64(p + 1, u); return 9; }
static int put_s64(uint8_t* p, int64_t v) { p[0] = TAG_S64; put_be64(p + 1, (uint64_t)v); return 9; }
static int put_timeval(uint8_t* p, uint32_t s, uint32_t u) {
    p[0] = TAG_TIMEVAL; put_be32(p + 1, s); put_be32(p + 5, u); return 9;
}

// ---- tagstruct reader -----------------------------------------------------
typedef struct { const uint8_t* p; int len, pos, ok; } Rd;
static int rd_u32(Rd* r, uint32_t* o) {
    if (!r->ok || r->pos + 5 > r->len || r->p[r->pos] != TAG_U32) { r->ok = 0; return 0; }
    *o = be32(r->p + r->pos + 1); r->pos += 5; return 1;
}
static int rd_bool(Rd* r, int* o) {
    if (!r->ok || r->pos + 1 > r->len) { r->ok = 0; return 0; }
    uint8_t t = r->p[r->pos];
    if (t != TAG_TRUE && t != TAG_FALSE) { r->ok = 0; return 0; }
    *o = t == TAG_TRUE; r->pos++; return 1;
}
static int rd_string_skip(Rd* r) {
    if (!r->ok || r->pos >= r->len) { r->ok = 0; return 0; }
    if (r->p[r->pos] == TAG_STRING_NULL) { r->pos++; return 1; }
    if (r->p[r->pos] != TAG_STRING) { r->ok = 0; return 0; }
    int q = r->pos + 1;
    while (q < r->len && r->p[q]) q++;
    if (q >= r->len) { r->ok = 0; return 0; }
    r->pos = q + 1; return 1;
}
static int rd_spec(Rd* r, uint8_t* f, uint8_t* ch, uint32_t* rate) {
    if (!r->ok || r->pos + 7 > r->len || r->p[r->pos] != TAG_SAMPLE_SPEC) { r->ok = 0; return 0; }
    *f = r->p[r->pos + 1]; *ch = r->p[r->pos + 2]; *rate = be32(r->p + r->pos + 3);
    r->pos += 7; return 1;
}
static int rd_cmap_skip(Rd* r) {
    if (!r->ok || r->pos + 2 > r->len || r->p[r->pos] != TAG_CHANNEL_MAP) { r->ok = 0; return 0; }
    int n = r->p[r->pos + 1];
    if (r->pos + 2 + n > r->len) { r->ok = 0; return 0; }
    r->pos += 2 + n; return 1;
}
static int rd_cvol_skip(Rd* r) {
    if (!r->ok || r->pos + 2 > r->len || r->p[r->pos] != TAG_CVOLUME) { r->ok = 0; return 0; }
    int n = r->p[r->pos + 1];
    if (r->pos + 2 + n * 4 > r->len) { r->ok = 0; return 0; }
    r->pos += 2 + n * 4; return 1;
}
static int rd_timeval(Rd* r, uint32_t* s, uint32_t* u) {
    if (!r->ok || r->pos + 9 > r->len || r->p[r->pos] != TAG_TIMEVAL) { r->ok = 0; return 0; }
    *s = be32(r->p + r->pos + 1); *u = be32(r->p + r->pos + 5); r->pos += 9; return 1;
}

static int reply_header(uint8_t* b, uint32_t tag) {
    int p = put_u32(b, CMD_REPLY); p += put_u32(b + p, tag); return p;
}
static void send_error(Client* c, uint32_t tag, uint32_t code) {
    uint8_t b[32]; int p = put_u32(b, CMD_ERROR);
    p += put_u32(b + p, tag); p += put_u32(b + p, code);
    send_packet(c, b, p);
}
static void send_request(Client* c, uint32_t channel, uint32_t bytes) {
    uint8_t b[32]; int p = put_u32(b, CMD_REQUEST);
    p += put_u32(b + p, 0xFFFFFFFFu); p += put_u32(b + p, channel); p += put_u32(b + p, bytes);
    send_packet(c, b, p);
}

// ---- mixing ---------------------------------------------------------------
static uint32_t fmt_bytes(uint8_t f) {
    switch (f) { case F_U8: return 1; case F_S16: return 2; case F_F32: return 4; case F_S32: return 4; }
    return 0;
}
static int fmt_ok(uint8_t f) { return f == F_U8 || f == F_S16 || f == F_F32 || f == F_S32; }

static int16_t clip16(int32_t v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v); }

static int16_t conv_sample(const uint8_t* p, uint8_t f) {
    switch (f) {
        case F_U8: return (int16_t)(((int)p[0] - 128) << 8);
        case F_S16: { int16_t v; memcpy(&v, p, 2); return v; }
        case F_S32: { int32_t v; memcpy(&v, p, 4); return (int16_t)(v >> 16); }
        case F_F32: {
            float v; memcpy(&v, p, 4);
            if (!(v == v)) return 0;
            if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
            return clip16((int32_t)(v * 32767.0f));
        }
    }
    return 0;
}

static void ring_push(Stream* s, int16_t l, int16_t r) {
    if (s->count >= RING_FRAMES) return;           // client overran its tlength: drop, like the kernel ring
    s->ring[s->wr * 2] = l; s->ring[s->wr * 2 + 1] = r;
    s->wr = (s->wr + 1) % RING_FRAMES; s->count++;
}

// convert + resample `frames` client frames into the stream ring
static void stream_write(Stream* s, const uint8_t* data, uint32_t frames) {
    uint32_t sb = fmt_bytes(s->fmt);
    for (uint32_t i = 0; i < frames; i++) {
        const uint8_t* fp = data + (size_t)i * s->frame_bytes;
        int16_t l = conv_sample(fp, s->fmt);
        int16_t r = s->ch >= 2 ? conv_sample(fp + sb, s->fmt) : l;
        if (s->rate == OUT_RATE) { ring_push(s, l, r); continue; }
        if (!s->have_prev) { s->prev[0] = l; s->prev[1] = r; s->have_prev = 1; s->pos = 0; }
        while (s->pos < 65536u) {
            int32_t fr = (int32_t)s->pos;
            ring_push(s, (int16_t)(s->prev[0] + (((int32_t)l - s->prev[0]) * fr >> 16)),
                         (int16_t)(s->prev[1] + (((int32_t)r - s->prev[1]) * fr >> 16)));
            s->pos += s->step;
        }
        s->pos -= 65536u;
        s->prev[0] = l; s->prev[1] = r;
    }
}

static int any_active(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) if (g_cl[i].in_use)
        for (int j = 0; j < MAX_STREAMS; j++) if (g_cl[i].st[j].in_use) return 1;
    return 0;
}

// mix `n` output frames into out (stereo s16)
static int mix_frames(int16_t* out, uint32_t n) {
    static int32_t acc[2048 * 2];
    int got = 0;
    memset(acc, 0, (size_t)n * 8);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_cl[i].in_use) continue;
        for (int j = 0; j < MAX_STREAMS; j++) {
            Stream* s = &g_cl[i].st[j];
            if (!s->in_use || s->corked || s->count == 0) continue;
            uint32_t take = s->count < n ? s->count : n;
            for (uint32_t k = 0; k < take; k++) {
                acc[k * 2]     += s->ring[s->rd * 2];
                acc[k * 2 + 1] += s->ring[s->rd * 2 + 1];
                s->rd = (s->rd + 1) % RING_FRAMES;
            }
            s->count -= take; s->pulled_frames += take; got = 1;
        }
    }
    for (uint32_t k = 0; k < n * 2; k++) out[k] = clip16(acc[k]);
    return got;
}

// ---- stream/client lifecycle ----------------------------------------------
static void stream_free(Stream* s) {
    if (s->ring) free(s->ring);
    memset(s, 0, sizeof *s);
}
static void client_close(Client* c) {
    for (int j = 0; j < MAX_STREAMS; j++) if (c->st[j].in_use) stream_free(&c->st[j]);
    if (c->fd >= 0) close(c->fd);
    c->in_use = 0; c->fd = -1; c->rx_len = 0;
}
static Stream* find_stream(uint32_t chn) {
    for (int i = 0; i < MAX_CLIENTS; i++) if (g_cl[i].in_use)
        for (int j = 0; j < MAX_STREAMS; j++)
            if (g_cl[i].st[j].in_use && g_cl[i].st[j].channel == chn) return &g_cl[i].st[j];
    return NULL;
}

static int sink_record(uint8_t* b, int is_source) {
    int p = 0;
    long lat = kaud(OP_LATENCY, 0, 0); if (lat < 0) lat = 0;
    p += put_u32(b + p, 0);
    p += put_string(b + p, is_source ? "alsa_input.kurono" : "alsa_output.kurono");
    p += put_string(b + p, is_source ? "Kurono Built-in Mic" : "Kurono Built-in Audio");
    p += put_sample_spec(b + p, F_S16, 2, OUT_RATE);
    p += put_cmap_stereo(b + p);
    p += put_u32(b + p, 0xFFFFFFFFu);
    p += put_cvol(b + p, 2);
    p += put_bool(b + p, 0);
    p += put_u32(b + p, 0xFFFFFFFFu);
    p += put_str_null(b + p);
    p += put_usec(b + p, (uint64_t)lat * 1000000ull / OUT_RATE);
    p += put_string(b + p, "kurono");
    p += put_u32(b + p, 0);
    return p;
}

static void handle_cmd(Client* c, uint32_t cmd, uint32_t tag, const uint8_t* args, int alen) {
    uint8_t buf[1024];
    int p = reply_header(buf, tag);
    Rd r = { args, alen, 0, 1 };
    switch (cmd) {
    case CMD_AUTH:
        p += put_u32(buf + p, PA_VERSION); send_packet(c, buf, p); break;
    case CMD_SET_CLIENT_NAME:
        send_packet(c, buf, p); break;
    case CMD_GET_SERVER_INFO:
        p += put_string(buf + p, "kurono-pulse");
        p += put_string(buf + p, "12.0");
        p += put_string(buf + p, "user");
        p += put_string(buf + p, "kurono");
        p += put_sample_spec(buf + p, F_S16, 2, OUT_RATE);
        p += put_string(buf + p, "alsa_output.kurono");
        p += put_string(buf + p, "alsa_input.kurono");
        p += put_u32(buf + p, 1);
        send_packet(c, buf, p); break;
    case CMD_GET_SINK_INFO: case CMD_GET_SINK_INFO_LIST:
        p += sink_record(buf + p, 0); send_packet(c, buf, p); break;
    case CMD_GET_SOURCE_INFO: case CMD_GET_SOURCE_INFO_LIST:
        p += sink_record(buf + p, 1); send_packet(c, buf, p); break;
    case CMD_STAT:
        for (int i = 0; i < 5; i++) p += put_u32(buf + p, 0);
        send_packet(c, buf, p); break;
    case CMD_CREATE_PLAYBACK: {
        uint8_t fmt = F_S16, ch = 2; uint32_t rate = 48000;
        uint32_t sink_index = 0, maxlength = 0, tlength = 0, prebuf = 0, minreq = 0, syncid = 0;
        int corked = 0;
        rd_spec(&r, &fmt, &ch, &rate); rd_cmap_skip(&r); rd_u32(&r, &sink_index);
        rd_string_skip(&r); rd_u32(&r, &maxlength); rd_bool(&r, &corked);
        rd_u32(&r, &tlength); rd_u32(&r, &prebuf); rd_u32(&r, &minreq);
        rd_u32(&r, &syncid); rd_cvol_skip(&r);
        if (!r.ok || !fmt_ok(fmt) || ch < 1 || ch > 6 || rate < 4000 || rate > 192000) {
            send_error(c, tag, PA_ERR_INVALID); break;
        }
        int slot = -1;
        for (int i = 0; i < MAX_STREAMS; i++) if (!c->st[i].in_use) { slot = i; break; }
        if (slot < 0) { send_error(c, tag, PA_ERR_NORESOURCE); break; }
        Stream* s = &c->st[slot];
        memset(s, 0, sizeof *s);
        s->ring = (int16_t*)malloc((size_t)RING_FRAMES * 4);
        if (!s->ring) { send_error(c, tag, PA_ERR_NORESOURCE); break; }
        s->in_use = 1; s->channel = g_next_channel++; s->rate = rate; s->ch = ch; s->fmt = fmt;
        s->frame_bytes = fmt_bytes(fmt) * ch;
        s->step = (uint32_t)(((uint64_t)rate << 16) / OUT_RATE);
        s->corked = corked;
        uint32_t period = (rate * s->frame_bytes * 21) / 1000;
        if (tlength == 0 || tlength == 0xFFFFFFFFu) tlength = period * 4;
        if (minreq == 0 || minreq == 0xFFFFFFFFu) minreq = period;
        if (prebuf == 0xFFFFFFFFu) prebuf = tlength / 2;
        if (prebuf > tlength) prebuf = tlength;
        if (maxlength == 0 || maxlength == 0xFFFFFFFFu) maxlength = tlength * 8;
        if (maxlength < tlength) maxlength = tlength;
        s->tlength = tlength; s->minreq = minreq; s->requested_bytes = tlength;
        p += put_u32(buf + p, s->channel);
        p += put_u32(buf + p, s->channel);
        p += put_u32(buf + p, tlength);
        p += put_u32(buf + p, maxlength);
        p += put_u32(buf + p, tlength);
        p += put_u32(buf + p, prebuf);
        p += put_u32(buf + p, minreq);
        p += put_sample_spec(buf + p, fmt, ch, rate);
        p += put_cmap_n(buf + p, ch);
        p += put_u32(buf + p, 0);
        p += put_string(buf + p, "alsa_output.kurono");
        p += put_bool(buf + p, 0);
        send_packet(c, buf, p);
        char m[80]; int n = snprintf(m, sizeof m, "audiod: stream open fmt=%d rate=%u ch=%d\r\n", fmt, rate, ch);
        (void)!write(2, m, (size_t)n);
        break;
    }
    case CMD_DELETE_PLAYBACK: {
        uint32_t chn;
        if (rd_u32(&r, &chn))
            for (int j = 0; j < MAX_STREAMS; j++)
                if (c->st[j].in_use && c->st[j].channel == chn) stream_free(&c->st[j]);
        send_packet(c, buf, p); break;
    }
    case CMD_CORK: {
        uint32_t chn; int cork = 0;
        if (rd_u32(&r, &chn) && rd_bool(&r, &cork)) { Stream* s = find_stream(chn); if (s) s->corked = cork; }
        send_packet(c, buf, p); break;
    }
    case CMD_GET_PLAYBACK_LATENCY: {
        uint32_t chn = 0, ts = 0, tu = 0;
        rd_u32(&r, &chn); rd_timeval(&r, &ts, &tu);
        Stream* s = find_stream(chn);
        if (!s) { send_error(c, tag, PA_ERR_INVALID); break; }
        long hw = kaud(OP_LATENCY, 0, 0), kq = kaud(OP_QUEUED, 0, 0);
        if (hw < 0) hw = 0;
        if (kq < 0) kq = 0;
        uint64_t sink_us = ((uint64_t)hw + (uint64_t)kq + s->count) * 1000000ull / OUT_RATE;
        uint64_t rd_bytes = (s->pulled_frames * s->rate / OUT_RATE) * s->frame_bytes;
        if (rd_bytes > s->written_bytes) rd_bytes = s->written_bytes;
        p += put_usec(buf + p, sink_us);
        p += put_usec(buf + p, 0);
        p += put_bool(buf + p, !s->corked);
        p += put_timeval(buf + p, ts, tu);
        p += put_timeval(buf + p, ts, tu);
        p += put_s64(buf + p, (int64_t)s->written_bytes);
        p += put_s64(buf + p, (int64_t)rd_bytes);
        send_packet(c, buf, p); break;
    }
    case CMD_DRAIN: case CMD_FLUSH: case CMD_TRIGGER: case CMD_SET_STREAM_NAME:
    case CMD_SUBSCRIBE: case CMD_EXIT:
        send_packet(c, buf, p); break;
    default:
        send_error(c, tag, PA_ERR_NOTSUPPORTED); break;
    }
}

static void client_data(Client* c) {
    int p = 0;
    while (c->rx_len - p >= 20) {
        uint32_t plen = be32(c->rx + p);
        if (plen > 8192) { c->rx_len = 0; return; }
        if (c->rx_len - p < (int)(20 + plen)) break;
        uint32_t channel = be32(c->rx + p + 4);
        const uint8_t* payload = c->rx + p + 20;
        if (channel == 0xFFFFFFFFu) {
            if (plen >= 10 && payload[0] == TAG_U32 && payload[5] == TAG_U32)
                handle_cmd(c, be32(payload + 1), be32(payload + 6), payload + 10, (int)plen - 10);
        } else {
            Stream* s = find_stream(channel);
            if (s && s->frame_bytes) {
                uint32_t frames = plen / s->frame_bytes;
                if (frames) {
                    stream_write(s, payload, frames);
                    s->written_bytes += (uint64_t)frames * s->frame_bytes;
                    g_last_data_ms = now_ms();
                }
            }
        }
        p += 20 + (int)plen;
    }
    if (p > 0) {
        int rem = c->rx_len - p;
        memmove(c->rx, c->rx + p, (size_t)rem);
        c->rx_len = rem;
    }
}

// ask each live stream for the bytes the mix consumed since the last pass
static void pace(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client* c = &g_cl[i];
        if (!c->in_use) continue;
        for (int j = 0; j < MAX_STREAMS; j++) {
            Stream* s = &c->st[j];
            if (!s->in_use || s->corked) continue;
            uint64_t consumed = (s->pulled_frames * s->rate / OUT_RATE) * s->frame_bytes;
            uint64_t target = consumed + s->tlength;
            if (target > s->requested_bytes && target - s->requested_bytes >= s->minreq) {
                send_request(c, s->channel, (uint32_t)(target - s->requested_bytes));
                s->requested_bytes = target;
            }
        }
    }
}

// keep the kernel stream topped up; open/close it as streams come and go
static void pump(void) {
    uint64_t t = now_ms();
    static uint64_t last_ping;
    if (t - last_ping > 1000) { kaud(OP_PING, 0, 0); last_ping = t; }   // heartbeat: the kernel supervisor restarts us only if this stops
    if (!g_kopen) {
        if (!any_active() || t - g_last_data_ms > IDLE_CLOSE_MS) return;
        long rate = kaud(OP_OPEN, 0, 0);
        if (rate < 0) return;
        g_kopen = 1;
        logs("audiod: kernel stream open\r\n");
    }
    long q = kaud(OP_QUEUED, 0, 0);
    if (q < 0) { g_kopen = 0; return; }
    static int16_t out[1024 * 2];
    int guard = 0;
    while ((uint32_t)q < KQ_TARGET && guard++ < 8) {
        uint32_t n = KQ_TARGET - (uint32_t)q;
        if (n > 1024) n = 1024;
        mix_frames(out, n);
        long acc = kaud(OP_WRITE, (unsigned long)out, n);
        if (acc < 0) { g_kopen = 0; break; }
        g_out_frames += (uint64_t)acc;
        q += acc;
        if ((uint32_t)acc < n) break;
    }
    pace();
    if (g_kopen && (!any_active() || t - g_last_data_ms > IDLE_CLOSE_MS)) {
        kaud(OP_CLOSE, 0, 0);
        g_kopen = 0;
        logs("audiod: idle, kernel stream closed\r\n");
    }
}

static int make_listener(void) {
    mkdir("/system/run", 0755); mkdir("/system/run/user", 0755);
    mkdir("/system/run/user/1000", 0755); mkdir("/system/run/user/1000/pulse", 0755);
    unlink(SOCK_PATH);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) { logd("audiod: socket failed errno=", errno); return -1; }
    struct sockaddr_un sa; memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX; strcpy(sa.sun_path, SOCK_PATH);
    if (bind(fd, (struct sockaddr*)&sa, sizeof sa) < 0) { logd("audiod: bind failed errno=", errno); close(fd); return -1; }
    if (listen(fd, 16) < 0) { logd("audiod: listen failed errno=", errno); close(fd); return -1; }
    return fd;
}

static void* serve(void* arg);
int pulsetest_main(int argc, char** argv);   // pulsetest.c, linked in

// "audiod selftest": once the server is up, play a 44.1 kHz and a 48 kHz tone
// through our own socket with /usr/bin/pulsetest (a stand-in for a libpulse app).
static void* selftest(void* arg) {
    (void)arg;
    sleep(4);
    static char* const a1[] = { "pulsetest", "44100", "3", "440", NULL };
    static char* const a2[] = { "pulsetest", "48000", "3", "880", NULL };
    logd("audiod: selftest 44.1k rc=", pulsetest_main(4, a1));
    logd("audiod: selftest 48k rc=", pulsetest_main(4, a2));
    logs("audiod: selftest done\r\n");
    return NULL;
}

// The server loop runs on a worker thread, not on the process's first thread.
// Kurono keeps the first thread of a ring-3 process on the bsp, where a waiting
// syscall is a cooperative busy-wait that competes with the kernel processes
// (the audio pump among them); a worker thread is placed on an AP, where a
// blocking wait really parks the core. The first thread just joins.
int main(int argc, char** argv) {
    pthread_t th, tt;
    if (argc > 1 && strcmp(argv[1], "selftest") == 0) pthread_create(&tt, NULL, selftest, NULL);
    if (pthread_create(&th, NULL, serve, NULL) != 0) { logs("audiod: pthread_create failed\r\n"); return 1; }
    if (getenv("AUDIOD_JOIN")) { pthread_join(th, NULL); return 0; }
    syscall(SYS_exit, 0);       // end only this thread; the server thread keeps the process alive
    return 0;
}

static void* serve(void* arg) {
    (void)arg;
    logs("audiod: starting (ring-3 pulse server + mixer)\r\n");
    long ping = kaud(OP_PING, 0, 0);
    if (ping < 0) { logd("audiod: broker refused us rc=", ping); return NULL; }
    logd("audiod: backend ready=", ping);
    for (int i = 0; i < MAX_CLIENTS; i++) g_cl[i].fd = -1;
    int lfd = make_listener();
    if (lfd < 0) return NULL;
    logs("audiod: listening on " SOCK_PATH "\r\n");

    struct pollfd pf[MAX_CLIENTS + 1];
    int idx[MAX_CLIENTS + 1];
    for (;;) {
        int n = 0;
        pf[n].fd = lfd; pf[n].events = POLLIN; idx[n] = -1; n++;
        for (int i = 0; i < MAX_CLIENTS; i++) if (g_cl[i].in_use) {
            pf[n].fd = g_cl[i].fd; pf[n].events = POLLIN; idx[n] = i; n++;
        }
        int busy = g_kopen;
        int rc = poll(pf, (nfds_t)n, busy ? 4 : 100);
        if (rc > 0) {
            for (int k = 0; k < n; k++) {
                if (!pf[k].revents) continue;
                if (idx[k] < 0) {
                    for (;;) {
                        int nfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK);
                        if (nfd < 0) break;
                        int slot = -1;
                        for (int i = 0; i < MAX_CLIENTS; i++) if (!g_cl[i].in_use) { slot = i; break; }
                        if (slot < 0) { close(nfd); continue; }
                        memset(&g_cl[slot], 0, sizeof g_cl[slot]);
                        g_cl[slot].in_use = 1; g_cl[slot].fd = nfd;
                        logd("audiod: client connected fd=", nfd);
                    }
                    continue;
                }
                Client* c = &g_cl[idx[k]];
                for (;;) {
                    int room = (int)sizeof c->rx - c->rx_len;
                    if (room <= 0) { c->rx_len = 0; room = (int)sizeof c->rx; }
                    ssize_t got = recv(c->fd, c->rx + c->rx_len, (size_t)room, 0);
                    if (got > 0) { c->rx_len += (int)got; client_data(c); if (got < room) break; continue; }
                    if (got == 0 || (errno != EAGAIN && errno != EINTR)) { client_close(c); break; }
                    break;
                }
            }
        }
        pump();
    }
}
