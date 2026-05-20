#include "pulse_server.h"
#include "../net/unix_socket.h"
#include "serial.h"
#include "../fs/kvfs.h"

namespace {

struct Stream {
    bool     in_use;
    uint32_t channel;
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  format;
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

inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
inline void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
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

// PA command codes (only the ones we handle).
enum {
    PA_COMMAND_ERROR        = 0,
    PA_COMMAND_TIMEOUT      = 1,
    PA_COMMAND_REPLY        = 2,
    PA_COMMAND_AUTH         = 8,
    PA_COMMAND_SET_CLIENT_NAME = 9,
    PA_COMMAND_LOOKUP_SINK  = 10,
    PA_COMMAND_LOOKUP_SOURCE = 11,
    PA_COMMAND_CREATE_PLAYBACK_STREAM = 13,
    PA_COMMAND_DELETE_PLAYBACK_STREAM = 14,
    PA_COMMAND_DRAIN_PLAYBACK_STREAM  = 15,
    PA_COMMAND_GET_SERVER_INFO = 20,
    PA_COMMAND_GET_SINK_INFO_LIST = 27,
    PA_COMMAND_GET_SOURCE_INFO_LIST = 28,
    PA_COMMAND_SUBSCRIBE    = 33,
    PA_COMMAND_FLUSH_PLAYBACK_STREAM = 49,
};

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
            }
            return c;
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

// Tagstruct writers.
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

// Reply header for any successful command: [u32 PA_COMMAND_REPLY, u32 tag]
int reply_header(uint8_t* buf, uint32_t tag) {
    int p = 0;
    p += put_u32(buf + p, PA_COMMAND_REPLY);
    p += put_u32(buf + p, tag);
    return p;
}

void handle_request(Client* c, uint32_t cmd, uint32_t tag, const uint8_t* args,
                    int args_len) {
    (void)args; (void)args_len;
    uint8_t buf[1024];
    int p = reply_header(buf, tag);
    switch (cmd) {
        case PA_COMMAND_AUTH: {
            // reply: u32 protocol_version (32 supported)
            p += put_u32(buf + p, 32);
            c->authed = true;
            send_packet(c->sd, buf, p);
            break;
        }
        case PA_COMMAND_SET_CLIENT_NAME: {
            // reply: u32 client_index
            p += put_u32(buf + p, c->client_index);
            send_packet(c->sd, buf, p);
            break;
        }
        case PA_COMMAND_GET_SERVER_INFO: {
            p += put_string(buf + p, "kurono-pulse");
            p += put_string(buf + p, "1.0.0");
            p += put_string(buf + p, "kurono");
            p += put_string(buf + p, "kurono-host");
            // sample_spec: u8 format(2=S16LE), u8 ch(2), u32 rate(48000)
            buf[p++] = PA_TAG_SAMPLE_SPEC;
            buf[p++] = 2;
            buf[p++] = 2;
            put_be32(buf + p, 48000); p += 4;
            p += put_string(buf + p, "alsa_output.kurono");        // default sink
            p += put_string(buf + p, "alsa_input.kurono");         // default source
            p += put_u32(buf + p, 0);                              // cookie
            send_packet(c->sd, buf, p);
            break;
        }
        case PA_COMMAND_GET_SINK_INFO_LIST: {
            // One sink record.
            p += put_u32(buf + p, 0);                              // index
            p += put_string(buf + p, "alsa_output.kurono");        // name
            p += put_string(buf + p, "Kurono Built-in Audio");     // description
            buf[p++] = PA_TAG_SAMPLE_SPEC;
            buf[p++] = 2; buf[p++] = 2; put_be32(buf + p, 48000); p += 4;
            p += put_u32(buf + p, 0);                              // owner_module
            p += put_string(buf + p, "Front");                     // monitor_source
            p += put_u32(buf + p, 0xFFFFFFFFu);                    // monitor_source idx
            p += put_str_null(buf + p);                            // driver
            p += put_u32(buf + p, 0);                              // flags
            send_packet(c->sd, buf, p);
            // Empty terminator reply.
            p = reply_header(buf, tag);
            send_packet(c->sd, buf, p);
            break;
        }
        case PA_COMMAND_GET_SOURCE_INFO_LIST: {
            p += put_u32(buf + p, 0);
            p += put_string(buf + p, "alsa_input.kurono");
            p += put_string(buf + p, "Kurono Built-in Mic");
            buf[p++] = PA_TAG_SAMPLE_SPEC;
            buf[p++] = 2; buf[p++] = 2; put_be32(buf + p, 48000); p += 4;
            send_packet(c->sd, buf, p);
            p = reply_header(buf, tag);
            send_packet(c->sd, buf, p);
            break;
        }
        case PA_COMMAND_CREATE_PLAYBACK_STREAM: {
            // Allocate a stream slot.
            int slot = -1;
            for (int i = 0; i < PulseServer::PA_MAX_STREAMS_PER_CLIENT; i++) {
                if (!c->streams[i].in_use) { slot = i; break; }
            }
            if (slot < 0) {
                p = 0;
                p += put_u32(buf + p, PA_COMMAND_ERROR);
                p += put_u32(buf + p, tag);
                p += put_u32(buf + p, 5);                          // PA_ERR_NORESOURCE
                send_packet(c->sd, buf, p);
                break;
            }
            c->streams[slot].in_use     = true;
            c->streams[slot].channel    = g_next_channel++;
            c->streams[slot].sample_rate = 48000;
            c->streams[slot].channels   = 2;
            c->streams[slot].format     = 2;
            // reply: u32 channel, u32 stream_index, u32 missing(prebuf)
            p += put_u32(buf + p, c->streams[slot].channel);
            p += put_u32(buf + p, c->streams[slot].channel);
            p += put_u32(buf + p, 4096);                           // missing bytes
            // bufer_attr: maxlength, tlength, prebuf, minreq
            p += put_u32(buf + p, 65536);
            p += put_u32(buf + p, 16384);
            p += put_u32(buf + p, 4096);
            p += put_u32(buf + p, 1024);
            buf[p++] = PA_TAG_SAMPLE_SPEC;
            buf[p++] = 2; buf[p++] = 2; put_be32(buf + p, 48000); p += 4;
            p += put_string(buf + p, "alsa_output.kurono");        // dev_name
            p += put_u32(buf + p, 0);                              // dev_index
            p += put_bool(buf + p, false);                         // suspended
            send_packet(c->sd, buf, p);
            break;
        }
        case PA_COMMAND_DELETE_PLAYBACK_STREAM:
        case PA_COMMAND_DRAIN_PLAYBACK_STREAM:
        case PA_COMMAND_FLUSH_PLAYBACK_STREAM:
        case PA_COMMAND_SUBSCRIBE: {
            send_packet(c->sd, buf, p);
            break;
        }
        default: {
            // Always reply success so the client doesn't error out.
            send_packet(c->sd, buf, p);
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
            // Command packet  -  payload is tagstruct(u32 cmd, u32 tag, ...)
            if (plen >= 10 && payload[0] == PA_TAG_U32 && payload[5] == PA_TAG_U32) {
                uint32_t cmd = be32(payload + 1);
                uint32_t tag = be32(payload + 6);
                handle_request(c, cmd, tag, payload + 10, (int)plen - 10);
            }
        } else {
            // Stream audio data  -  would forward to Audio::PlayPCM.
            // For now we acknowledge by consuming the bytes silently.
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
    for (int i = 0; i < PA_MAX_CLIENTS; i++) g_clients[i].in_use = false;

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
