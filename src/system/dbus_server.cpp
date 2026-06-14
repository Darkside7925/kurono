#include "dbus_server.h"
#include "../net/unix_socket.h"
#include "../drivers/serial.h"
#include "../fs/kvfs.h"

namespace {

struct Client {
    bool in_use;
    int  sd;
    bool authed;
    bool sent_hello;
    char unique_name[16];                      // ":1.42"
    uint32_t serial;
    uint8_t rx[16384];
    int rx_len;
};

struct NameEntry {
    bool in_use;
    char name[DBusServer::DBUS_NAME_LEN];
    int  owner_sd;
};

Client    g_clients[DBusServer::DBUS_MAX_CLIENTS];
NameEntry g_names[DBusServer::DBUS_MAX_NAMES];
int       g_listen_sd = -1;
int       g_next_unique = 1;

inline uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0]       | ((uint32_t)p[1] << 8) |
          ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline void put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

bool str_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}
int str_len(const char* s) { int n = 0; while (s[n]) n++; return n; }
void str_cpy(char* d, const char* s, int max) {
    int i; for (i = 0; i < max - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

Client* find_client(int sd) {
    for (int i = 0; i < DBusServer::DBUS_MAX_CLIENTS; i++) {
        if (g_clients[i].in_use && g_clients[i].sd == sd) return &g_clients[i];
    }
    return nullptr;
}

Client* alloc_client(int sd) {
    for (int i = 0; i < DBusServer::DBUS_MAX_CLIENTS; i++) {
        if (!g_clients[i].in_use) {
            Client* c = &g_clients[i];
            c->in_use = true;
            c->sd = sd;
            c->authed = false;
            c->sent_hello = false;
            c->serial = 1;
            c->rx_len = 0;
            // unique name like ":1.<n>"
            int n = g_next_unique++;
            char tmp[12]; int ti = 0;
            if (n == 0) tmp[ti++] = '0';
            else { char r[12]; int ri = 0;
                   while (n) { r[ri++] = '0' + (n % 10); n /= 10; }
                   while (ri) tmp[ti++] = r[--ri]; }
            tmp[ti] = 0;
            c->unique_name[0] = ':'; c->unique_name[1] = '1'; c->unique_name[2] = '.';
            int o = 3;
            for (int i2 = 0; tmp[i2] && o < (int)sizeof(c->unique_name) - 1; i2++)
                c->unique_name[o++] = tmp[i2];
            c->unique_name[o] = 0;
            return c;
        }
    }
    return nullptr;
}

// Pad helper.
inline int pad_to(int p, int align) { return (p + (align - 1)) & ~(align - 1); }

// Build a method-return message.  Body is a tiny payload containing one
// optional string.
//
// Header fields we set:
//   PATH(1)             -- not used in replies
//   REPLY_SERIAL(5)     u32  serial of the request
//   DESTINATION(6)      string  client unique name
//   SIGNATURE(8)        signature  body signature ("s" or "" or "as")
//   SENDER(7)           string  ":1.0" (the bus daemon)
//
// Header layout (DBus 1 spec):
//   byte    endian ('l')
//   byte    type   (1=method_call, 2=method_return, 3=error, 4=signal)
//   byte    flags
//   byte    proto version (1)
//   uint32  body length
//   uint32  serial
//   array of (struct (byte field_code, variant value))
//
// Header is then padded to 8-byte alignment before the body.

// cap = bytes available at p; every write is bounded so a client-controlled
// val_len can't run past the caller's buffer. returns bytes written. (satoru)
int put_header_field(uint8_t* p, int cap, uint8_t field_code, char sig,
                     const void* val, int val_len) {
    int s = 0;
    if (cap < 4) return 0;        // need at least the variant preamble (satoru)
    p[s++] = field_code;
    // variant: signature(1 char) then value
    p[s++] = 1;          // signature length
    p[s++] = (uint8_t)sig;
    p[s++] = 0;
    if (sig == 's' || sig == 'o') {
        // string: u32 length + bytes + nul
        s = pad_to(s, 4);
        if (s + 4 > cap) return s;
        put_le32(p + s, (uint32_t)val_len); s += 4;
        for (int i = 0; i < val_len && s < cap; i++) p[s++] = ((const uint8_t*)val)[i];
        if (s < cap) p[s++] = 0;
    } else if (sig == 'g') {
        // signature: u8 length + bytes + nul
        if (s < cap) p[s++] = (uint8_t)val_len;
        for (int i = 0; i < val_len && s < cap; i++) p[s++] = ((const uint8_t*)val)[i];
        if (s < cap) p[s++] = 0;
    } else if (sig == 'u') {
        s = pad_to(s, 4);
        if (s + 4 > cap) return s;
        put_le32(p + s, *(const uint32_t*)val); s += 4;
    }
    return s;
}

void send_method_return(Client* c, uint32_t reply_serial, const char* body_sig,
                        const uint8_t* body, int body_len) {
    uint8_t buf[1024];
    int p = 0;
    buf[p++] = 'l';            // little endian
    buf[p++] = 2;              // method_return
    buf[p++] = 0;              // flags
    buf[p++] = 1;              // protocol
    put_le32(buf + p, (uint32_t)body_len); p += 4;
    put_le32(buf + p, c->serial++); p += 4;

    // Header field array: u32 length, then padded fields.
    int array_len_off = p;
    put_le32(buf + p, 0); p += 4;
    int fields_start = p;

    // every pad_to/put_header_field below is bounded against sizeof(buf) so a
    // client-controlled name length can't overflow the stack buffer. (satoru)
    const int cap = (int)sizeof(buf);
    // REPLY_SERIAL (5) u32
    p = pad_to(p, 8);
    if (p <= cap) p += put_header_field(buf + p, cap - p, 5, 'u', &reply_serial, 0);
    // DESTINATION (6) string
    p = pad_to(p, 8);
    int un_len = str_len(c->unique_name);
    if (p <= cap) p += put_header_field(buf + p, cap - p, 6, 's', c->unique_name, un_len);
    // SENDER (7) string ":1.0"
    p = pad_to(p, 8);
    if (p <= cap) p += put_header_field(buf + p, cap - p, 7, 's', "org.freedesktop.DBus", 20);
    // SIGNATURE (8)
    if (body_sig && body_sig[0]) {
        p = pad_to(p, 8);
        if (p <= cap) p += put_header_field(buf + p, cap - p, 8, 'g', body_sig, str_len(body_sig));
    }

    int fields_len = p - fields_start;
    put_le32(buf + array_len_off, (uint32_t)fields_len);

    // Pad header to 8 bytes.
    p = pad_to(p, 8);
    if (p > cap) p = cap;
    // Append body, bounded by remaining capacity. (satoru)
    for (int i = 0; i < body_len && p < cap; i++) buf[p++] = body[i];

    UnixSocket::KernelInject(c->sd, buf, p);
}

// Tiny string-builder body for "s" signature.
int build_string_body(uint8_t* buf, const char* s) {
    int slen = str_len(s);
    put_le32(buf, (uint32_t)slen);
    for (int i = 0; i < slen; i++) buf[4 + i] = (uint8_t)s[i];
    buf[4 + slen] = 0;
    return 4 + slen + 1;
}

// Walk the header field array of an incoming message and pluck out the
// fields we need: PATH(1), MEMBER(3), INTERFACE(2), DESTINATION(6).
struct ParsedMsg {
    uint8_t  type;
    uint32_t body_len;
    uint32_t serial;
    char path[128];
    char iface[128];
    char member[64];
    char dest[64];
    int  body_off;
};

bool parse_message(const uint8_t* msg, int len, ParsedMsg* out) {
    if (len < 16) return false;
    if (msg[0] != 'l') return false;     // we only accept little-endian
    out->type     = msg[1];
    out->body_len = le32(msg + 4);
    out->serial   = le32(msg + 8);
    uint32_t flen = le32(msg + 12);
    out->path[0] = out->iface[0] = out->member[0] = out->dest[0] = 0;
    int p = 16;
    int end = p + (int)flen;
    if (end > len) return false;
    while (p < end) {
        p = pad_to(p, 8);
        if (p >= end) break;
        uint8_t code = msg[p++];
        uint8_t siglen = msg[p++];
        if (p + siglen + 1 > len) return false;
        char sig = (char)msg[p];
        p += siglen + 1;        // skip sig bytes + nul
        if (sig == 's' || sig == 'o') {
            p = pad_to(p, 4);
            if (p + 4 > len) return false;
            uint32_t slen = le32(msg + p); p += 4;
            if (p + (int)slen + 1 > len) return false;
            char* dst = nullptr;
            int dst_max = 0;
            if (code == 1) { dst = out->path;   dst_max = sizeof(out->path); }
            if (code == 2) { dst = out->iface;  dst_max = sizeof(out->iface); }
            if (code == 3) { dst = out->member; dst_max = sizeof(out->member); }
            if (code == 6) { dst = out->dest;   dst_max = sizeof(out->dest); }
            if (dst) {
                int n = (int)slen; if (n >= dst_max) n = dst_max - 1;
                for (int i = 0; i < n; i++) dst[i] = (char)msg[p + i];
                dst[n] = 0;
            }
            p += (int)slen + 1;
        } else if (sig == 'g') {
            uint8_t glen = msg[p++];
            p += glen + 1;
        } else if (sig == 'u') {
            p = pad_to(p, 4);
            p += 4;
        } else {
            // Unknown variant  -  bail.
            return false;
        }
    }
    out->body_off = pad_to(end, 8);
    return true;
}

void handle_message(Client* c, const uint8_t* msg, int len) {
    ParsedMsg pm;
    if (!parse_message(msg, len, &pm)) return;
    if (pm.type != 1) return;        // only method_call

    if (str_eq(pm.iface, "org.freedesktop.DBus") &&
        str_eq(pm.member, "Hello")) {
        uint8_t body[64];
        int n = build_string_body(body, c->unique_name);
        send_method_return(c, pm.serial, "s", body, n);
        c->sent_hello = true;
    } else if (str_eq(pm.iface, "org.freedesktop.DBus") &&
               str_eq(pm.member, "RequestName")) {
        // body: string name, u32 flags  -  we honour any well-formed name.
        if (pm.body_len >= 8) {
            uint32_t slen = le32(msg + pm.body_off);
            int boff = pm.body_off + 4;
            if (boff + (int)slen <= len) {
                for (int i = 0; i < DBusServer::DBUS_MAX_NAMES; i++) {
                    if (!g_names[i].in_use) {
                        g_names[i].in_use = true;
                        g_names[i].owner_sd = c->sd;
                        int n = (int)slen;
                        if (n >= DBusServer::DBUS_NAME_LEN)
                            n = DBusServer::DBUS_NAME_LEN - 1;
                        for (int k = 0; k < n; k++) g_names[i].name[k] = (char)msg[boff + k];
                        g_names[i].name[n] = 0;
                        break;
                    }
                }
            }
        }
        // Reply: u32 1 (DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
        uint8_t body[8];
        put_le32(body, 1);
        send_method_return(c, pm.serial, "u", body, 4);
    } else if (str_eq(pm.iface, "org.freedesktop.DBus") &&
               str_eq(pm.member, "ListNames")) {
        // Reply: array of strings.  We return [":1.0", own + every owned name].
        uint8_t body[1024];
        const int bcap = (int)sizeof(body);
        int p = 0;
        // u32 array byte length placeholder
        int len_off = p; p += 4;
        // Each string: u32 len + bytes + nul, padded to 4.  Every entry is
        // bounded against sizeof(body); names are client-controlled so an entry
        // that wouldn't fit is dropped rather than overflowing the stack. (satoru)
        const char* fixed[] = { "org.freedesktop.DBus", c->unique_name, nullptr };
        for (int i = 0; fixed[i]; i++) {
            int sl = str_len(fixed[i]);
            int padded = pad_to(p, 4);
            if (padded + 4 + sl + 1 > bcap) break;
            p = padded;
            put_le32(body + p, (uint32_t)sl); p += 4;
            for (int k = 0; k < sl; k++) body[p++] = (uint8_t)fixed[i][k];
            body[p++] = 0;
        }
        for (int i = 0; i < DBusServer::DBUS_MAX_NAMES; i++) {
            if (!g_names[i].in_use) continue;
            int sl = str_len(g_names[i].name);
            int padded = pad_to(p, 4);
            if (padded + 4 + sl + 1 > bcap) break;
            p = padded;
            put_le32(body + p, (uint32_t)sl); p += 4;
            for (int k = 0; k < sl; k++) body[p++] = (uint8_t)g_names[i].name[k];
            body[p++] = 0;
        }
        put_le32(body + len_off, (uint32_t)(p - 4));
        send_method_return(c, pm.serial, "as", body, p);
    } else if (str_eq(pm.iface, "org.freedesktop.Notifications") &&
               str_eq(pm.member, "GetCapabilities")) {
        uint8_t body[256]; int p = 0;
        int len_off = p; p += 4;
        const char* caps[] = { "actions", "body", "body-markup", "icon-static", nullptr };
        for (int i = 0; caps[i]; i++) {
            int sl = str_len(caps[i]);
            p = pad_to(p, 4);
            put_le32(body + p, (uint32_t)sl); p += 4;
            for (int k = 0; k < sl; k++) body[p++] = (uint8_t)caps[i][k];
            body[p++] = 0;
        }
        put_le32(body + len_off, (uint32_t)(p - 4));
        send_method_return(c, pm.serial, "as", body, p);
    } else if (str_eq(pm.iface, "org.freedesktop.Notifications") &&
               str_eq(pm.member, "Notify")) {
        // Always succeed  -  return a fresh notification id.
        static uint32_t notif_id = 1;
        uint8_t body[8]; put_le32(body, notif_id++);
        send_method_return(c, pm.serial, "u", body, 4);
    } else {
        // Generic empty success reply for everything else.
        send_method_return(c, pm.serial, "", nullptr, 0);
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

    // Auth phase: handle SASL EXTERNAL line by line ending in \r\n.
    if (!c->authed) {
        // Strip leading nul (libdbus sends one byte 0 first).
        if (c->rx_len > 0 && c->rx[0] == 0) {
            for (int i = 1; i < c->rx_len; i++) c->rx[i - 1] = c->rx[i];
            c->rx_len--;
        }
        // Look for "AUTH EXTERNAL ...\r\n"
        int p = 0;
        while (p < c->rx_len - 1) {
            if (c->rx[p] == '\r' && c->rx[p + 1] == '\n') {
                // Line is c->rx[0..p)
                if (p >= 4 && c->rx[0] == 'A' && c->rx[1] == 'U' &&
                    c->rx[2] == 'T' && c->rx[3] == 'H') {
                    const char* ok = "OK 5a4b3c2d1e0f9876543210abcdef0123\r\n";
                    int olen = str_len(ok);
                    UnixSocket::KernelInject(c->sd, (const uint8_t*)ok, olen);
                } else if (p >= 5 && c->rx[0] == 'B' && c->rx[1] == 'E' &&
                           c->rx[2] == 'G' && c->rx[3] == 'I' && c->rx[4] == 'N') {
                    c->authed = true;
                } else if (p >= 11 && c->rx[0] == 'N' && c->rx[1] == 'E' &&
                           c->rx[2] == 'G') {
                    const char* nm = "AGREE_UNIX_FD\r\n";
                    UnixSocket::KernelInject(c->sd, (const uint8_t*)nm, str_len(nm));
                }
                int rem = c->rx_len - (p + 2);
                for (int i = 0; i < rem; i++) c->rx[i] = c->rx[p + 2 + i];
                c->rx_len = rem;
                p = 0;
                if (c->authed) break;
            } else {
                p++;
            }
        }
        if (!c->authed) return;
    }

    // Message phase.
    int p = 0;
    while (c->rx_len - p >= 16) {
        if (c->rx[p] != 'l') { c->rx_len = 0; return; }
        uint32_t blen = le32(c->rx + p + 4);
        uint32_t flen = le32(c->rx + p + 12);
        int total = pad_to(16 + (int)flen, 8) + (int)blen;
        if (c->rx_len - p < total) break;
        handle_message(c, c->rx + p, total);
        p += total;
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
    SerialLogger::Log("DBus: client connected sd=");
    SerialLogger::LogDec(new_sd);
    SerialLogger::Log("\r\n");
}

}  // namespace

namespace DBusServer {

void Init() {
    for (int i = 0; i < DBUS_MAX_CLIENTS; i++) g_clients[i].in_use = false;
    for (int i = 0; i < DBUS_MAX_NAMES; i++)   g_names[i].in_use   = false;

    g_listen_sd = UnixSocket::Create(UnixSocket::UNIX_SOCK_STREAM);
    if (g_listen_sd < 0) return;
    if (UnixSocket::Bind(g_listen_sd, "/system/run/user/1000/bus") < 0) return;
    UnixSocket::Listen(g_listen_sd, 32);
    UnixSocket::RegisterServer(g_listen_sd, on_connect, on_data, nullptr);

    // Drop a session.conf so dbus-launch can verify the daemon.
    KVFS::WriteString("/system/etc/dbus-1/session.conf",
        "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\""
        " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
        "<busconfig>\n"
        "  <type>session</type>\n"
        "  <listen>unix:path=/system/run/user/1000/bus</listen>\n"
        "  <auth>EXTERNAL</auth>\n"
        "  <policy context=\"default\">\n"
        "    <allow send_destination=\"*\"/>\n"
        "    <allow eavesdrop=\"true\"/>\n"
        "    <allow own=\"*\"/>\n"
        "  </policy>\n"
        "</busconfig>\n");
    KVFS::WriteString("/system/run/user/1000/bus.info",
        "kurono dbus session daemon v1\n"
        "auth: EXTERNAL\n"
        "interfaces: org.freedesktop.DBus, .Notifications, .ScreenSaver, .portal.Desktop\n");
    SerialLogger::Log("DBus: listening on /system/run/user/1000/bus\r\n");
}

int ListenSd() { return g_listen_sd; }
int ClientCount() {
    int n = 0;
    for (int i = 0; i < DBUS_MAX_CLIENTS; i++) if (g_clients[i].in_use) n++;
    return n;
}
int RegisteredNameCount() {
    int n = 0;
    for (int i = 0; i < DBUS_MAX_NAMES; i++) if (g_names[i].in_use) n++;
    return n;
}

void EmitSignal(const char* path, const char* iface, const char* member) {
    (void)path; (void)iface; (void)member;
    // TODO: build signal message with type=4, broadcast to AddMatch subscribers.
}

}  // namespace DBusServer
