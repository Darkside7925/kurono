#include "pulse_server.h"
#include "../net/unix_socket.h"
#include "serial.h"
#include "timer.h"
#include "../fs/kvfs.h"
#include "audio_mixer.h"
#include "audio_format.h"
#include "audio_server.h"
#include "audio_backend.h"
#include "../proc/spinlock.h"

// pulseaudio native-protocol server, rewritten against the REAL upstream
// command numbering and tagstruct layouts (negotiated protocol version 12).
// the previous revision had shifted command codes (CREATE_PLAYBACK_STREAM
// as 13, which is upstream STAT; sink list as 27, upstream GET_CLIENT_INFO)
// and advertised sample format 2 - upstream ULAW, not S16LE - so a real
// libpulse client (firefox cubeb) could connect but never actually create
// a working stream. it also never sent PA_COMMAND_REQUEST, without which
// libpulse writes its initial buffer once and then waits forever. (satoru)

namespace {

struct Stream {
    bool     in_use;
    uint32_t channel;
    uint32_t sample_rate;    // client-side rate (satoru)
    uint8_t  channels;
    uint8_t  format;         // pa sample format code (satoru)
    int      mixer_id;       // AudioMixer::StreamID, -1 if not bound
    int      owner_sd;       // socket to send REQUEST/timing packets to (satoru)
    uint32_t frame_bytes;    // client-side bytes per frame (satoru)
    uint32_t tlength;        // negotiated target buffer bytes (satoru)
    uint32_t minreq;         // negotiated minimum request bytes (satoru)
    // pacing: total client bytes we have asked for so far vs the mixer's
    // consumed-frame counter converted back into client bytes. (satoru)
    uint64_t requested_bytes;
    uint64_t written_bytes;  // client bytes received on the data channel (satoru)
};

struct Client {
    bool   in_use;
    int    sd;
    bool   authed;
    uint32_t client_index;
    Stream streams[PulseServer::PA_MAX_STREAMS_PER_CLIENT];
    uint8_t rx[16384];
    int    rx_len;
};

Client g_clients[PulseServer::PA_MAX_CLIENTS];
int    g_listen_sd = -1;
uint32_t g_next_client_idx = 100;
uint32_t g_next_channel    = 1;

// the protocol version we negotiate. 12 keeps every reply layout at its
// simplest complete form (client_index in SET_CLIENT_NAME, proplists in
// sink records and the extra latency counters all arrived at 13+). (satoru)
constexpr uint32_t PA_VERSION = 12;
// pa sample format codes (upstream sample_spec.h). (satoru)
constexpr uint8_t PA_SAMPLE_U8        = 0;
constexpr uint8_t PA_SAMPLE_S16LE     = 3;
constexpr uint8_t PA_SAMPLE_FLOAT32LE = 5;
constexpr uint8_t PA_SAMPLE_S32LE     = 7;

inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
inline void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
inline void put_be64(uint8_t* p, uint64_t v) {
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

// PA tag bytes (subset).
enum {
    PA_TAG_STRING = 't', PA_TAG_STRING_NULL = 'N',
    PA_TAG_U32 = 'L', PA_TAG_U8 = 'B', PA_TAG_U64 = 'R',
    PA_TAG_S64 = 'r', PA_TAG_SAMPLE_SPEC = 'a',
    PA_TAG_ARBITRARY = 'x', PA_TAG_BOOLEAN_TRUE = '1',
    PA_TAG_BOOLEAN_FALSE = '0', PA_TAG_TIMEVAL = 'T',
    PA_TAG_USEC = 'U', PA_TAG_CHANNEL_MAP = 'm',
    PA_TAG_CVOLUME = 'v', PA_TAG_PROPLIST = 'P',
    PA_TAG_VOLUME = 'V', PA_TAG_FORMAT_INFO = 'f',
};

// PA command codes - the REAL upstream numbering. (satoru)
enum {
    PA_COMMAND_ERROR                    = 0,
    PA_COMMAND_TIMEOUT                  = 1,
    PA_COMMAND_REPLY                    = 2,
    PA_COMMAND_CREATE_PLAYBACK_STREAM   = 3,
    PA_COMMAND_DELETE_PLAYBACK_STREAM   = 4,
    PA_COMMAND_CREATE_RECORD_STREAM     = 5,
    PA_COMMAND_DELETE_RECORD_STREAM     = 6,
    PA_COMMAND_EXIT                     = 7,
    PA_COMMAND_AUTH                     = 8,
    PA_COMMAND_SET_CLIENT_NAME          = 9,
    PA_COMMAND_DRAIN_PLAYBACK_STREAM    = 12,
    PA_COMMAND_STAT                     = 13,
    PA_COMMAND_GET_PLAYBACK_LATENCY     = 14,
    PA_COMMAND_GET_SERVER_INFO          = 20,
    PA_COMMAND_GET_SINK_INFO            = 21,
    PA_COMMAND_GET_SINK_INFO_LIST       = 22,
    PA_COMMAND_GET_SOURCE_INFO          = 23,
    PA_COMMAND_GET_SOURCE_INFO_LIST     = 24,
    PA_COMMAND_SUBSCRIBE                = 35,
    PA_COMMAND_CORK_PLAYBACK_STREAM     = 41,
    PA_COMMAND_FLUSH_PLAYBACK_STREAM    = 42,
    PA_COMMAND_TRIGGER_PLAYBACK_STREAM  = 43,
    PA_COMMAND_SET_PLAYBACK_STREAM_NAME = 46,
    PA_COMMAND_REQUEST                  = 61,   // server -> client (satoru)
};
constexpr uint32_t PA_ERR_NOTSUPPORTED = 19;
constexpr uint32_t PA_ERR_NORESOURCE   = 5;
constexpr uint32_t PA_ERR_INVALID      = 2;

Client* find_client(int sd) {
    for (int i = 0; i < PulseServer::PA_MAX_CLIENTS; i++) {
        if (g_clients[i].in_use && g_clients[i].sd == sd) return &g_clients[i];
    }
    return nullptr;
}

Client* alloc_client(int sd) {
    for (int i = 0; i < PulseServer::PA_MAX_CLIENTS; i++) {
        if (!g_clients[i].in_use) {
            Client* c = &g_clients[i];
            c->in_use = true;
            c->sd = sd;
            c->authed = false;
            c->client_index = g_next_client_idx++;
            c->rx_len = 0;
            for (int j = 0; j < PulseServer::PA_MAX_STREAMS_PER_CLIENT; j++) {
                c->streams[j].in_use = false;
                c->streams[j].mixer_id = -1;
            }
            return c;
        }
    }
    return nullptr;
}

static AudioFormat::SampleFormat pa_to_fmt(uint8_t pa) {
    switch (pa) {
        case PA_SAMPLE_U8:        return AudioFormat::FMT_U8;
        case PA_SAMPLE_S16LE:     return AudioFormat::FMT_S16_LE;
        case PA_SAMPLE_FLOAT32LE: return AudioFormat::FMT_F32_LE;
        case PA_SAMPLE_S32LE:     return AudioFormat::FMT_S32_LE;
        default:                  return AudioFormat::FMT_S16_LE;
    }
}
static bool pa_fmt_supported(uint8_t pa) {
    return pa == PA_SAMPLE_U8 || pa == PA_SAMPLE_S16LE ||
           pa == PA_SAMPLE_FLOAT32LE || pa == PA_SAMPLE_S32LE;
}

static Stream* find_stream_by_channel(uint32_t channel) {
    for (int i = 0; i < PulseServer::PA_MAX_CLIENTS; i++) {
        if (!g_clients[i].in_use) continue;
        for (int j = 0; j < PulseServer::PA_MAX_STREAMS_PER_CLIENT; j++) {
            Stream* s = &g_clients[i].streams[j];
            if (s->in_use && s->channel == channel) return s;
        }
    }
    return nullptr;
}

void send_packet(int sd, const uint8_t* payload, int payload_len) {
    uint8_t hdr[20];
    put_be32(hdr + 0,  (uint32_t)payload_len);
    put_be32(hdr + 4,  0xFFFFFFFFu);   // command channel
    put_be32(hdr + 8,  0);
    put_be32(hdr + 12, 0);
    put_be32(hdr + 16, 0);
    UnixSocket::KernelInject(sd, hdr, 20);
    UnixSocket::KernelInject(sd, payload, payload_len);
}

// ── tagstruct writers ────────────────────────────────────────────────────
inline int put_u32(uint8_t* p, uint32_t v) {
    p[0] = PA_TAG_U32;
    put_be32(p + 1, v);
    return 5;
}
inline int put_string(uint8_t* p, const char* s) {
    p[0] = PA_TAG_STRING;
    int n = 0; while (s[n]) n++;
    for (int i = 0; i < n; i++) p[1 + i] = (uint8_t)s[i];
    p[1 + n] = 0;
    return 2 + n;
}
inline int put_str_null(uint8_t* p) { p[0] = PA_TAG_STRING_NULL; return 1; }
inline int put_bool(uint8_t* p, bool v) { p[0] = v ? PA_TAG_BOOLEAN_TRUE : PA_TAG_BOOLEAN_FALSE; return 1; }
inline int put_sample_spec(uint8_t* p, uint8_t fmt, uint8_t ch, uint32_t rate) {
    p[0] = PA_TAG_SAMPLE_SPEC;
    p[1] = fmt; p[2] = ch;
    put_be32(p + 3, rate);
    return 7;
}
inline int put_channel_map_stereo(uint8_t* p) {
    p[0] = PA_TAG_CHANNEL_MAP;
    p[1] = 2;
    p[2] = 1;   // PA_CHANNEL_POSITION_FRONT_LEFT (satoru)
    p[3] = 2;   // PA_CHANNEL_POSITION_FRONT_RIGHT (satoru)
    return 4;
}
inline int put_channel_map_n(uint8_t* p, uint8_t n) {
    // echo back a plausible map for n channels: mono=center, else L,R,... (satoru)
    p[0] = PA_TAG_CHANNEL_MAP;
    p[1] = n;
    if (n == 1) { p[2] = 0; return 3; }   // MONO (satoru)
    for (uint8_t i = 0; i < n; i++) p[2 + i] = (uint8_t)(1 + i);
    return 2 + n;
}
inline int put_cvolume_norm(uint8_t* p, uint8_t n) {
    p[0] = PA_TAG_CVOLUME;
    p[1] = n;
    for (uint8_t i = 0; i < n; i++) put_be32(p + 2 + i * 4, 0x10000u);  // PA_VOLUME_NORM (satoru)
    return 2 + n * 4;
}
inline int put_usec(uint8_t* p, uint64_t usec) {
    p[0] = PA_TAG_USEC;
    put_be64(p + 1, usec);
    return 9;
}
inline int put_s64(uint8_t* p, int64_t v) {
    p[0] = PA_TAG_S64;
    put_be64(p + 1, (uint64_t)v);
    return 9;
}
inline int put_timeval(uint8_t* p, uint32_t sec, uint32_t usec) {
    p[0] = PA_TAG_TIMEVAL;
    put_be32(p + 1, sec);
    put_be32(p + 5, usec);
    return 9;
}

// ── tagstruct reader ─────────────────────────────────────────────────────
struct TagReader {
    const uint8_t* p;
    int len;
    int pos;
    bool ok;
};
static bool rd_u32(TagReader& r, uint32_t* out) {
    if (!r.ok || r.pos + 5 > r.len || r.p[r.pos] != PA_TAG_U32) { r.ok = false; return false; }
    *out = be32(r.p + r.pos + 1);
    r.pos += 5;
    return true;
}
static bool rd_bool(TagReader& r, bool* out) {
    if (!r.ok || r.pos + 1 > r.len) { r.ok = false; return false; }
    uint8_t t = r.p[r.pos];
    if (t != PA_TAG_BOOLEAN_TRUE && t != PA_TAG_BOOLEAN_FALSE) { r.ok = false; return false; }
    *out = (t == PA_TAG_BOOLEAN_TRUE);
    r.pos += 1;
    return true;
}
static bool rd_string_skip(TagReader& r) {
    if (!r.ok || r.pos >= r.len) { r.ok = false; return false; }
    if (r.p[r.pos] == PA_TAG_STRING_NULL) { r.pos += 1; return true; }
    if (r.p[r.pos] != PA_TAG_STRING) { r.ok = false; return false; }
    int q = r.pos + 1;
    while (q < r.len && r.p[q]) q++;
    if (q >= r.len) { r.ok = false; return false; }
    r.pos = q + 1;
    return true;
}
static bool rd_sample_spec(TagReader& r, uint8_t* fmt, uint8_t* ch, uint32_t* rate) {
    if (!r.ok || r.pos + 7 > r.len || r.p[r.pos] != PA_TAG_SAMPLE_SPEC) { r.ok = false; return false; }
    *fmt  = r.p[r.pos + 1];
    *ch   = r.p[r.pos + 2];
    *rate = be32(r.p + r.pos + 3);
    r.pos += 7;
    return true;
}
static bool rd_channel_map_skip(TagReader& r) {
    if (!r.ok || r.pos + 2 > r.len || r.p[r.pos] != PA_TAG_CHANNEL_MAP) { r.ok = false; return false; }
    int n = r.p[r.pos + 1];
    if (r.pos + 2 + n > r.len) { r.ok = false; return false; }
    r.pos += 2 + n;
    return true;
}
static bool rd_cvolume_skip(TagReader& r) {
    if (!r.ok || r.pos + 2 > r.len || r.p[r.pos] != PA_TAG_CVOLUME) { r.ok = false; return false; }
    int n = r.p[r.pos + 1];
    if (r.pos + 2 + n * 4 > r.len) { r.ok = false; return false; }
    r.pos += 2 + n * 4;
    return true;
}
static bool rd_timeval(TagReader& r, uint32_t* sec, uint32_t* usec) {
    if (!r.ok || r.pos + 9 > r.len || r.p[r.pos] != PA_TAG_TIMEVAL) { r.ok = false; return false; }
    *sec  = be32(r.p + r.pos + 1);
    *usec = be32(r.p + r.pos + 5);
    r.pos += 9;
    return true;
}

// Reply header for any successful command: [u32 PA_COMMAND_REPLY, u32 tag]
int reply_header(uint8_t* buf, uint32_t tag) {
    int p = 0;
    p += put_u32(buf + p, PA_COMMAND_REPLY);
    p += put_u32(buf + p, tag);
    return p;
}

void send_error(int sd, uint32_t tag, uint32_t code) {
    uint8_t buf[32];
    int p = 0;
    p += put_u32(buf + p, PA_COMMAND_ERROR);
    p += put_u32(buf + p, tag);
    p += put_u32(buf + p, code);
    send_packet(sd, buf, p);
}

// server-initiated REQUEST: ask the client for `bytes` more of stream data.
// tag is the server-message sentinel 0xFFFFFFFF, exactly like upstream. (satoru)
void send_request(int sd, uint32_t channel, uint32_t bytes) {
    uint8_t buf[24];
    int p = 0;
    p += put_u32(buf + p, PA_COMMAND_REQUEST);
    p += put_u32(buf + p, 0xFFFFFFFFu);
    p += put_u32(buf + p, channel);
    p += put_u32(buf + p, bytes);
    send_packet(sd, buf, p);
}

// one full v12 sink/source record (shared shape). (satoru)
int fill_sink_record(uint8_t* buf, bool is_source) {
    int p = 0;
    uint64_t latency_us = 0;
    {
        AudioBackend* be = AudioServer::ActiveBackend();
        if (be) latency_us = (uint64_t)be->QueuedFrames() * 1000000ull / 48000ull;
    }
    p += put_u32(buf + p, 0);                                       // index
    p += put_string(buf + p, is_source ? "alsa_input.kurono"
                                       : "alsa_output.kurono");     // name
    p += put_string(buf + p, is_source ? "Kurono Built-in Mic"
                                       : "Kurono Built-in Audio");  // description
    p += put_sample_spec(buf + p, PA_SAMPLE_S16LE, 2, 48000);
    p += put_channel_map_stereo(buf + p);
    p += put_u32(buf + p, 0xFFFFFFFFu);                             // owner_module
    p += put_cvolume_norm(buf + p, 2);
    p += put_bool(buf + p, false);                                  // muted
    p += put_u32(buf + p, 0xFFFFFFFFu);                             // monitor idx
    p += put_str_null(buf + p);                                     // monitor name
    p += put_usec(buf + p, latency_us);                             // latency
    p += put_string(buf + p, "kurono");                             // driver
    p += put_u32(buf + p, 0);                                       // flags
    return p;
}

void handle_request(Client* c, uint32_t cmd, uint32_t tag, const uint8_t* args,
                    int args_len) {
    uint8_t buf[1024];
    int p = reply_header(buf, tag);
    TagReader r{args, args_len, 0, true};
    switch (cmd) {
        case PA_COMMAND_AUTH: {
            // reply: u32 negotiated protocol version. (satoru)
            p += put_u32(buf + p, PA_VERSION);
            c->authed = true;
            send_packet(c->sd, buf, p);
            break;
        }
        case PA_COMMAND_SET_CLIENT_NAME: {
            // v12 reply is EMPTY - the client_index field only exists from
            // v13, and sending it anyway desyncs libpulse's tag reader. (satoru)
            send_packet(c->sd, buf, p);
            break;
        }
        case PA_COMMAND_GET_SERVER_INFO: {
            p += put_string(buf + p, "kurono-pulse");     // server name
            p += put_string(buf + p, "12.0");             // server version
            p += put_string(buf + p, "user");             // username
            p += put_string(buf + p, "kurono");           // hostname
            p += put_sample_spec(buf + p, PA_SAMPLE_S16LE, 2, 48000);
            p += put_string(buf + p, "alsa_output.kurono");
            p += put_string(buf + p, "alsa_input.kurono");
            p += put_u32(buf + p, 1);                     // cookie
            send_packet(c->sd, buf, p);
            break;
        }
        case PA_COMMAND_GET_SINK_INFO:
        case PA_COMMAND_GET_SINK_INFO_LIST: {
            p += fill_sink_record(buf + p, false);
            send_packet(c->sd, buf, p);
            break;
        }
        case PA_COMMAND_GET_SOURCE_INFO:
        case PA_COMMAND_GET_SOURCE_INFO_LIST: {
            p += fill_sink_record(buf + p, true);
            send_packet(c->sd, buf, p);
            break;
        }
        case PA_COMMAND_STAT: {
            p += put_u32(buf + p, 0);   // memblock count
            p += put_u32(buf + p, 0);   // memblock bytes
            p += put_u32(buf + p, 0);   // accumulated count
            p += put_u32(buf + p, 0);   // accumulated bytes
            p += put_u32(buf + p, 0);   // sample cache bytes
            send_packet(c->sd, buf, p);
            break;
        }
        case PA_COMMAND_CREATE_PLAYBACK_STREAM: {
            // v12 request layout: sample_spec, channel_map, sink_index,
            // sink_name, maxlength, corked, tlength, prebuf, minreq,
            // syncid, cvolume. (satoru)
            uint8_t  fmt = PA_SAMPLE_S16LE, ch = 2;
            uint32_t rate = 48000;
            uint32_t sink_index = 0, maxlength = 0, tlength = 0, prebuf = 0,
                     minreq = 0, syncid = 0;
            bool corked = false;
            rd_sample_spec(r, &fmt, &ch, &rate);
            rd_channel_map_skip(r);
            rd_u32(r, &sink_index);
            rd_string_skip(r);
            rd_u32(r, &maxlength);
            rd_bool(r, &corked);
            rd_u32(r, &tlength);
            rd_u32(r, &prebuf);
            rd_u32(r, &minreq);
            rd_u32(r, &syncid);
            rd_cvolume_skip(r);
            if (!r.ok || !pa_fmt_supported(fmt) || ch < 1 || ch > 6 ||
                rate < 4000 || rate > 192000) {
                send_error(c->sd, tag, PA_ERR_INVALID);
                break;
            }
            int slot = -1;
            for (int i = 0; i < PulseServer::PA_MAX_STREAMS_PER_CLIENT; i++) {
                if (!c->streams[i].in_use) { slot = i; break; }
            }
            if (slot < 0) {
                send_error(c->sd, tag, PA_ERR_NORESOURCE);
                break;
            }
            Stream* s = &c->streams[slot];
            s->in_use      = true;
            s->channel     = g_next_channel++;
            s->sample_rate = rate;
            s->channels    = ch;
            s->format      = fmt;
            s->owner_sd    = c->sd;
            s->frame_bytes = AudioFormat::FrameSize(pa_to_fmt(fmt), ch);
            if (s->frame_bytes == 0) s->frame_bytes = 4;
            // sane negotiated attrs: honor the client's asks where present,
            // else default to ~85ms target with ~21ms requests. (satoru)
            uint32_t period_bytes = (rate * s->frame_bytes * 21) / 1000;
            if (tlength == 0 || tlength == 0xFFFFFFFFu) tlength = period_bytes * 4;
            if (minreq  == 0 || minreq  == 0xFFFFFFFFu) minreq  = period_bytes;
            if (prebuf  == 0xFFFFFFFFu) prebuf = tlength / 2;
            if (prebuf  > tlength) prebuf = tlength;
            if (maxlength == 0 || maxlength == 0xFFFFFFFFu) maxlength = tlength * 8;
            if (maxlength < tlength) maxlength = tlength;
            s->tlength = tlength;
            s->minreq  = minreq;
            s->requested_bytes = tlength;   // the initial `missing` grant (satoru)
            s->written_bytes   = 0;
            s->mixer_id = AudioServer::OpenStream("pulse", pa_to_fmt(fmt), rate, ch);
            if (s->mixer_id == AudioMixer::INVALID_STREAM) {
                s->in_use = false;
                send_error(c->sd, tag, PA_ERR_NORESOURCE);
                break;
            }
            if (corked) AudioServer::PauseStream(s->mixer_id, true);
            // v12 reply: channel, stream_index, missing, maxlength, tlength,
            // prebuf, minreq, sample_spec, channel_map, dev_index, dev_name,
            // suspended. sample_spec echoes the CLIENT's negotiated spec. (satoru)
            p += put_u32(buf + p, s->channel);
            p += put_u32(buf + p, s->channel);
            p += put_u32(buf + p, tlength);          // missing = fill me up (satoru)
            p += put_u32(buf + p, maxlength);
            p += put_u32(buf + p, tlength);
            p += put_u32(buf + p, prebuf);
            p += put_u32(buf + p, minreq);
            p += put_sample_spec(buf + p, fmt, ch, rate);
            p += put_channel_map_n(buf + p, ch);
            p += put_u32(buf + p, 0);                // device index
            p += put_string(buf + p, "alsa_output.kurono");
            p += put_bool(buf + p, false);           // suspended
            send_packet(c->sd, buf, p);
            SerialLogger::Log("Pulse: stream open fmt=");
            SerialLogger::LogDec((int)fmt);
            SerialLogger::Log(" rate=");
            SerialLogger::LogDec((int)rate);
            SerialLogger::Log(" ch=");
            SerialLogger::LogDec((int)ch);
            SerialLogger::Log("\r\n");
            break;
        }
        case PA_COMMAND_DELETE_PLAYBACK_STREAM: {
            uint32_t chn = 0;
            if (rd_u32(r, &chn)) {
                for (int j = 0; j < PulseServer::PA_MAX_STREAMS_PER_CLIENT; j++) {
                    Stream* s = &c->streams[j];
                    if (s->in_use && s->channel == chn) {
                        if (s->mixer_id >= 0) AudioServer::CloseStream(s->mixer_id);
                        s->in_use = false;
                        s->mixer_id = -1;
                    }
                }
            }
            send_packet(c->sd, buf, p);
            break;
        }
        case PA_COMMAND_DRAIN_PLAYBACK_STREAM: {
            uint32_t chn = 0;
            if (rd_u32(r, &chn)) {
                Stream* s = find_stream_by_channel(chn);
                if (s && s->mixer_id >= 0) AudioServer::DrainStream(s->mixer_id);
            }
            send_packet(c->sd, buf, p);
            break;
        }
        case PA_COMMAND_CORK_PLAYBACK_STREAM: {
            uint32_t chn = 0;
            bool cork = false;
            if (rd_u32(r, &chn) && rd_bool(r, &cork)) {
                Stream* s = find_stream_by_channel(chn);
                if (s && s->mixer_id >= 0) AudioServer::PauseStream(s->mixer_id, cork);
            }
            send_packet(c->sd, buf, p);
            break;
        }
        case PA_COMMAND_GET_PLAYBACK_LATENCY: {
            // request: channel, client timeval. reply: sink usec, source
            // usec, playing, local timeval (echo), remote timeval, write
            // index, read index - everything cubeb needs to keep a/v sync. (satoru)
            uint32_t chn = 0, tsec = 0, tusec = 0;
            rd_u32(r, &chn);
            rd_timeval(r, &tsec, &tusec);
            Stream* s = find_stream_by_channel(chn);
            if (!s || s->mixer_id < 0) {
                send_error(c->sd, tag, PA_ERR_INVALID);
                break;
            }
            AudioMixer::StreamStats st = AudioMixer::GetStats(s->mixer_id);
            uint64_t backend_us = 0;
            {
                AudioBackend* be = AudioServer::ActiveBackend();
                if (be) backend_us = (uint64_t)be->QueuedFrames() * 1000000ull / 48000ull;
            }
            // ring depth in usec at the internal rate + the dma queue. (satoru)
            uint64_t ring_frames = st.frames_written - st.frames_consumed;
            uint64_t sink_us = backend_us + ring_frames * 1000000ull / 48000ull;
            // client-byte read/write indices: internal frames scaled back to
            // the client's rate and frame size. (satoru)
            uint64_t wr_bytes = s->written_bytes;
            uint64_t rd_frames_client = st.frames_consumed * (uint64_t)s->sample_rate / 48000ull;
            uint64_t rd_bytes = rd_frames_client * s->frame_bytes;
            if (rd_bytes > wr_bytes) rd_bytes = wr_bytes;
            bool playing = AudioMixer::GetState(s->mixer_id) == AudioMixer::STREAM_PLAYING ||
                           AudioMixer::GetState(s->mixer_id) == AudioMixer::STREAM_DRAINING;
            p += put_usec(buf + p, sink_us);
            p += put_usec(buf + p, 0);
            p += put_bool(buf + p, playing);
            p += put_timeval(buf + p, tsec, tusec);      // echo local (satoru)
            p += put_timeval(buf + p, tsec, tusec);      // remote ~= local (satoru)
            p += put_s64(buf + p, (int64_t)wr_bytes);
            p += put_s64(buf + p, (int64_t)rd_bytes);
            send_packet(c->sd, buf, p);
            break;
        }
        case PA_COMMAND_FLUSH_PLAYBACK_STREAM:
        case PA_COMMAND_TRIGGER_PLAYBACK_STREAM:
        case PA_COMMAND_SET_PLAYBACK_STREAM_NAME:
        case PA_COMMAND_SUBSCRIBE:
        case PA_COMMAND_EXIT: {
            send_packet(c->sd, buf, p);
            break;
        }
        default: {
            // honest NOTSUPPORTED: a fake empty success desyncs the client's
            // tag reader when it expects reply fields (the old behavior). (satoru)
            send_error(c->sd, tag, PA_ERR_NOTSUPPORTED);
            break;
        }
    }
}

void on_data(int sd, const uint8_t* data, int len, void* user) {
    (void)user;
    Client* c = find_client(sd);
    if (!c) return;
    int copy = len;
    if (c->rx_len + copy > (int)sizeof(c->rx)) copy = (int)sizeof(c->rx) - c->rx_len;
    for (int i = 0; i < copy; i++) c->rx[c->rx_len + i] = data[i];
    c->rx_len += copy;

    int p = 0;
    while (c->rx_len - p >= 20) {
        uint32_t plen = be32(c->rx + p);
        if (plen > 8192) { c->rx_len = 0; return; }
        if (c->rx_len - p < (int)(20 + plen)) break;
        uint32_t channel = be32(c->rx + p + 4);
        const uint8_t* payload = c->rx + p + 20;
        if (channel == 0xFFFFFFFFu) {
            // Command packet - payload is tagstruct(u32 cmd, u32 tag, ...)
            if (plen >= 10 && payload[0] == PA_TAG_U32 && payload[5] == PA_TAG_U32) {
                uint32_t cmd = be32(payload + 1);
                uint32_t tag = be32(payload + 6);
                handle_request(c, cmd, tag, payload + 10, (int)plen - 10);
            }
        } else {
            // Stream audio data - forward to the mixer through the LOCKED
            // server wrapper (this runs in the client's syscall context on
            // any cpu; unlocked mixer writes raced the pump). (satoru)
            Stream* s = find_stream_by_channel(channel);
            if (s && s->mixer_id >= 0 && s->frame_bytes > 0) {
                uint32_t frames = plen / s->frame_bytes;
                if (frames > 0) {
                    AudioServer::WriteStream(s->mixer_id, payload, frames);
                    s->written_bytes += (uint64_t)frames * s->frame_bytes;
                }
            }
        }
        p += 20 + plen;
    }
    if (p > 0) {
        int rem = c->rx_len - p;
        for (int i = 0; i < rem; i++) c->rx[i] = c->rx[p + i];
        c->rx_len = rem;
    }
}

void on_connect(int server_sd, int new_sd, void* user) {
    (void)server_sd; (void)user;
    if (!alloc_client(new_sd)) {
        UnixSocket::Close(new_sd);
        return;
    }
    SerialLogger::Log("Pulse: client connected sd=");
    SerialLogger::LogDec(new_sd);
    SerialLogger::Log("\r\n");
}

}  // namespace

namespace PulseServer {

void Init() {
    for (int i = 0; i < PA_MAX_CLIENTS; i++) {
        g_clients[i].in_use = false;
        for (int j = 0; j < PA_MAX_STREAMS_PER_CLIENT; j++) {
            g_clients[i].streams[j].in_use   = false;
            g_clients[i].streams[j].mixer_id = -1;
        }
    }

    g_listen_sd = UnixSocket::Create(UnixSocket::UNIX_SOCK_STREAM);
    if (g_listen_sd < 0) return;
    if (UnixSocket::Bind(g_listen_sd,
                         "/system/run/user/1000/pulse/native") < 0) return;
    UnixSocket::Listen(g_listen_sd, 16);
    UnixSocket::RegisterServer(g_listen_sd, on_connect, on_data, nullptr);

    KVFS::WriteString("/system/run/user/1000/pulse/cookie",
        "kurono-pulse-cookie-32bytes-of-padding-data\n");
    KVFS::WriteString("/system/run/user/1000/pulse/native.info",
        "kurono pulseaudio-compatible server v1\n"
        "default sink: alsa_output.kurono\n"
        "default source: alsa_input.kurono\n"
        "sample format: S16LE 48000Hz stereo\n");
    SerialLogger::Log("Pulse: listening on /system/run/user/1000/pulse/native\r\n");
}

void Pace() {
    // for each live stream: how many client bytes has the mixer consumed
    // beyond what we already granted? request them in minreq-sized bites so
    // the client keeps ~tlength queued - this is real pulseaudio's pacing
    // model, and without it libpulse writes once and waits forever. (satoru)
    for (int i = 0; i < PA_MAX_CLIENTS; i++) {
        Client* c = &g_clients[i];
        if (!c->in_use) continue;
        for (int j = 0; j < PA_MAX_STREAMS_PER_CLIENT; j++) {
            Stream* s = &c->streams[j];
            // hold the audio pump lock across the whole per-stream body:
            // create/close/data-write run in client syscall context on other
            // cpus under this same lock (via the AudioServer wrappers), while
            // this pass runs unlocked on the bsp audio process. without it a
            // close between the in_use check and GetStats read a recycled
            // mixer slot and mis-paced REQUESTs at the wrong stream. per
            // stream, not around the whole pass, to keep writers moving. (satoru)
            SpinLockCpuGuard guard(AudioServer::PumpLock());
            if (!s->in_use || s->mixer_id < 0) continue;
            AudioMixer::StreamStats st = AudioMixer::GetStats(s->mixer_id);
            uint64_t consumed_client =
                st.frames_consumed * (uint64_t)s->sample_rate / 48000ull * s->frame_bytes;
            uint64_t target = consumed_client + s->tlength;
            if (target > s->requested_bytes &&
                target - s->requested_bytes >= s->minreq) {
                uint32_t ask = (uint32_t)(target - s->requested_bytes);
                send_request(s->owner_sd, s->channel, ask);
                s->requested_bytes = target;
            }
        }
    }
}

int ListenSd() { return g_listen_sd; }

int ClientCount() {
    int n = 0;
    for (int i = 0; i < PA_MAX_CLIENTS; i++) if (g_clients[i].in_use) n++;
    return n;
}
int StreamCount() {
    int n = 0;
    for (int i = 0; i < PA_MAX_CLIENTS; i++) {
        if (!g_clients[i].in_use) continue;
        for (int j = 0; j < PA_MAX_STREAMS_PER_CLIENT; j++) {
            if (g_clients[i].streams[j].in_use) n++;
        }
    }
    return n;
}

}  // namespace PulseServer
// end (satoru)
