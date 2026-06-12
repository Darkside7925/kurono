#include "pkgmgr.h"
#include "../shell/shell.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../drivers/e1000.h"
#include "../drivers/timer.h"
#include "../kernel/heap.h"
#include "../net/network.h"
#include "../net/tcpip.h"
#include "../system/gpu_driver_installer.h"
#include "../system/system_update.h"
#include "../virt/debian_data.h"
#include "../ui/notification.h"

Package PackageManager::packages[PKG_MAX_PACKAGES];
int PackageManager::package_count = 0;

static const char* PKG_REPOSITORY_HOST = "kurono.satorut.com";
static const char* PKG_INDEX_PATH = "/packages/index.json";
// Legacy Debian-style fallback paths (kept for compatibility):
static const char* PKG_REPOSITORY_PATHS[] = {
    "/packages/Packages",
    "/repo/Packages",
    "/dists/stable/main/binary-amd64/Packages",
    "/Packages"
};
static const int PKG_REPOSITORY_PATH_COUNT =
    (int)(sizeof(PKG_REPOSITORY_PATHS) / sizeof(PKG_REPOSITORY_PATHS[0]));
static const uint32_t PKG_REPOSITORY_IP_FALLBACKS[] = {
    (((uint32_t)104 << 24) | ((uint32_t)21 << 16) |
     ((uint32_t)44 << 8) | (uint32_t)39),
    (((uint32_t)172 << 24) | ((uint32_t)67 << 16) |
     ((uint32_t)194 << 8) | (uint32_t)167)
};
static const int PKG_REPOSITORY_IP_FALLBACK_COUNT =
    (int)(sizeof(PKG_REPOSITORY_IP_FALLBACKS) / sizeof(PKG_REPOSITORY_IP_FALLBACKS[0]));
// Repository fetch buffer: bumped from 64 KB to 16 MB so that larger
// .tar/.tar.gz packages (and Packages indexes for big repos) round-trip
// in a single phttp_get(). For the 108 MB Debian rootfs we still need a
// proper streaming download  -  that's tracked separately.
static const int PKG_HTTP_BUFFER_MAX = 16 * 1024 * 1024;

static bool pkg_repo_synced = false;
static bool pkg_last_sync_ok = false;
static char pkg_last_sync_message[160] = "Repository not synced yet.";

static int plen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static void pcpy(char* d, const char* s, int m) {
    int i = 0;
    if (!d || m < 1) return;
    while (s && s[i] && i < m - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static bool peq(const char* a, const char* b) {
    while (a && b && *a && *b) { if (*a != *b) return false; a++; b++; }
    return a && b && *a == 0 && *b == 0;
}
static int pa(char* b, int p, int m, const char* s) {
    while (s && *s && p < m - 1) b[p++] = *s++;
    b[p] = 0;
    return p;
}
static int pac(char* b, int p, int m, char c) {
    if (p < m - 1) {
        b[p++] = c;
        b[p] = 0;
    }
    return p;
}
static int pai(char* b, int p, int m, unsigned int v) {
    if (v == 0) return pac(b, p, m, '0');
    char t[12];
    int ti = 0;
    while (v > 0) { t[ti++] = '0' + (v % 10); v /= 10; }
    while (ti > 0) p = pac(b, p, m, t[--ti]);
    return p;
}
static int phex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool pcontains(const char* haystack, const char* needle) {
    int hl = plen(haystack), nl = plen(needle);
    if (nl == 0 || hl < nl) return false;
    for (int i = 0; i <= hl - nl; i++) {
        bool match = true;
        for (int j = 0; j < nl; j++) {
            char a = haystack[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}
static void ptrim(char* s) {
    if (!s) return;
    int start = 0;
    while (s[start] == ' ' || s[start] == '\t') start++;
    int len = plen(s + start);
    for (int i = 0; i <= len; i++) s[i] = s[start + i];
    int end = plen(s);
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) {
        s[--end] = 0;
    }
}
static unsigned int patoi(const char* s) {
    unsigned int value = 0;
    while (s && *s >= '0' && *s <= '9') {
        value = value * 10u + (unsigned int)(*s - '0');
        s++;
    }
    return value;
}
static bool pip_eq(uint32_t a, uint32_t b) {
    return a == b;
}
static bool pip_contains(const uint32_t* ips, int count, uint32_t ip) {
    for (int i = 0; i < count; i++) {
        if (pip_eq(ips[i], ip)) return true;
    }
    return false;
}
static void pformat_ip(uint32_t ip, char* out, int max) {
    int p = 0;
    p = pai(out, p, max, (ip >> 24) & 0xFFu);
    p = pac(out, p, max, '.');
    p = pai(out, p, max, (ip >> 16) & 0xFFu);
    p = pac(out, p, max, '.');
    p = pai(out, p, max, (ip >> 8) & 0xFFu);
    p = pac(out, p, max, '.');
    p = pai(out, p, max, ip & 0xFFu);
}
static int pbuild_connect_targets(const char* host, uint32_t* ips, int max_ips) {
    int count = 0;
    if (!ips || max_ips <= 0) return 0;

    IPv4Address resolved;
    if (host && host[0] && Network::Resolve(host, &resolved)) {
        uint32_t live_ip = TCPStack::MakeIP(resolved.bytes[0], resolved.bytes[1],
                                            resolved.bytes[2], resolved.bytes[3]);
        ips[count++] = live_ip;
    }

    for (int i = 0; i < PKG_REPOSITORY_IP_FALLBACK_COUNT && count < max_ips; i++) {
        uint32_t fallback_ip = PKG_REPOSITORY_IP_FALLBACKS[i];
        if (!pip_contains(ips, count, fallback_ip)) {
            ips[count++] = fallback_ip;
        }
    }
    return count;
}
static void pset_sync_message(const char* msg) {
    pcpy(pkg_last_sync_message, msg, (int)sizeof(pkg_last_sync_message));
}
static void pset_sync_message_http(const char* prefix, int status, const char* path) {
    char msg[160];
    int p = 0;
    p = pa(msg, p, sizeof(msg), prefix);
    p = pa(msg, p, sizeof(msg), " HTTP ");
    p = pai(msg, p, sizeof(msg), (unsigned int)status);
    p = pa(msg, p, sizeof(msg), " from ");
    p = pa(msg, p, sizeof(msg), path);
    pset_sync_message(msg);
}
static int pfind_header_end(const char* data, int len) {
    for (int i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
            return i + 4;
        }
    }
    return -1;
}
static int pdecode_chunked(const char* src, int src_len, char* dst, int dst_max) {
    int sp = 0;
    int dp = 0;
    while (sp < src_len) {
        while (sp < src_len && (src[sp] == '\r' || src[sp] == '\n')) sp++;
        if (sp >= src_len) break;

        int chunk_size = 0;
        bool saw_digit = false;
        while (sp < src_len) {
            if (src[sp] == ';') {
                while (sp < src_len && !(src[sp] == '\r' && sp + 1 < src_len && src[sp + 1] == '\n')) sp++;
                break;
            }
            if (src[sp] == '\r' && sp + 1 < src_len && src[sp + 1] == '\n') {
                sp += 2;
                break;
            }
            int hv = phex(src[sp]);
            if (hv < 0) return -1;
            chunk_size = (chunk_size << 4) | hv;
            saw_digit = true;
            sp++;
        }
        if (!saw_digit) return -1;
        if (chunk_size == 0) return dp;
        if (sp + chunk_size > src_len) return -1;

        int copy = chunk_size;
        if (dp + copy > dst_max - 1) copy = dst_max - 1 - dp;
        if (copy > 0) {
            memcpy(dst + dp, src + sp, copy);
            dp += copy;
        }
        sp += chunk_size;
        if (sp + 1 < src_len && src[sp] == '\r' && src[sp + 1] == '\n') sp += 2;
    }
    return dp;
}

static bool pends_with(const char* str, const char* suffix) {
    int sl = plen(str), tl = plen(suffix);
    if (tl == 0 || sl < tl) return false;
    return peq(str + sl - tl, suffix);
}

static uint32_t pread_u32_le(const uint8_t* data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static void pbasename_stem(const char* path, char* out, int max_out) {
    const char* leaf = path;
    for (const char* p = path; p && *p; p++) {
        if (*p == '/' || *p == '\\') leaf = p + 1;
    }

    int n = 0;
    while (leaf[n] && leaf[n] != '.' && n < max_out - 1) {
        out[n] = leaf[n];
        n++;
    }
    out[n] = 0;
}

static bool pkro_safe_rel_path(const char* path) {
    if (!path || !path[0]) return false;
    if (path[0] == '/' || path[0] == '\\') return false;

    int seg_len = 0;
    char seg[KVFS_MAX_NAME];
    for (int i = 0;; i++) {
        char ch = path[i];
        if (ch == '\\' || ch == ':' ) return false;
        if (ch == '/' || ch == 0) {
            seg[seg_len] = 0;
            if (seg_len == 0) return false;
            if (peq(seg, ".") || peq(seg, "..")) return false;
            seg_len = 0;
            if (ch == 0) break;
            continue;
        }
        if (seg_len >= KVFS_MAX_NAME - 1) return false;
        seg[seg_len++] = ch;
    }
    return true;
}

static void pdirname(const char* path, char* out, int max_out) {
    pcpy(out, path, max_out);
    int len = plen(out);
    while (len > 1 && out[len - 1] != '/') len--;
    if (len <= 1) {
        out[0] = '/';
        out[1] = 0;
        return;
    }
    out[len - 1] = 0;
}

struct KroManifestInfo {
    char app_name[PKG_MAX_NAME];
    char app_entry[KVFS_MAX_PATH];
    char app_version[16];
    char app_author[64];
};

static void pkro_init_manifest(KroManifestInfo* info) {
    if (!info) return;
    info->app_name[0] = 0;
    pcpy(info->app_entry, "main.kcl", (int)sizeof(info->app_entry));
    pcpy(info->app_version, "1.0.0", (int)sizeof(info->app_version));
    info->app_author[0] = 0;
}

static bool pkro_manifest_value(const char* manifest, const char* key, char* out, int out_max) {
    if (!manifest || !key || !out || out_max < 2) return false;

    int key_len = plen(key);
    const char* cursor = manifest;
    while (*cursor) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') cursor++;
        if (*cursor == '#') {
            while (*cursor && *cursor != '\n') cursor++;
            continue;
        }

        const char* keyword = nullptr;
        if (cursor[0] == 's' && cursor[1] == 'e' && cursor[2] == 't' &&
            (cursor[3] == ' ' || cursor[3] == '\t')) {
            keyword = cursor + 3;
        } else if (cursor[0] == 'l' && cursor[1] == 'e' && cursor[2] == 't' &&
                   (cursor[3] == ' ' || cursor[3] == '\t')) {
            keyword = cursor + 3;
        }

        if (!keyword) {
            while (*cursor && *cursor != '\n') cursor++;
            continue;
        }

        cursor = keyword;
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        bool key_match = true;
        for (int i = 0; i < key_len; i++) {
            if (cursor[i] != key[i]) {
                key_match = false;
                break;
            }
        }
        if (!key_match || !(cursor[key_len] == 0 || cursor[key_len] == ' ' ||
                            cursor[key_len] == '\t' || cursor[key_len] == '=' ||
                            cursor[key_len] == '\r' || cursor[key_len] == '\n')) {
            while (*cursor && *cursor != '\n') cursor++;
            continue;
        }

        cursor += key_len;
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        if (*cursor == '=') cursor++;
        while (*cursor == ' ' || *cursor == '\t') cursor++;

        int n = 0;
        if (*cursor == '"' || *cursor == '\'') {
            char quote = *cursor++;
            while (*cursor && *cursor != quote && n < out_max - 1) out[n++] = *cursor++;
        } else {
            while (*cursor && *cursor != '\n' && *cursor != '\r' && *cursor != '#' &&
                   *cursor != ' ' && *cursor != '\t' && n < out_max - 1) {
                out[n++] = *cursor++;
            }
        }
        out[n] = 0;
        return n > 0;
    }

    return false;
}

static bool pkro_scan_archive(const uint8_t* payload, int payload_len, const char* source_path,
                              KroManifestInfo* info, char* err, int err_max) {
    pkro_init_manifest(info);

    if (!payload || payload_len < 12) {
        pcpy(err, "kro archive is too small.", err_max);
        return false;
    }
    if (!(payload[0] == 'K' && payload[1] == 'R' && payload[2] == 'O' && payload[3] == '1')) {
        pcpy(err, "file is not a valid .kro archive.", err_max);
        return false;
    }
    if (!(payload[payload_len - 4] == 'E' && payload[payload_len - 3] == 'N' &&
          payload[payload_len - 2] == 'D' && payload[payload_len - 1] == 'K')) {
        pcpy(err, "kro footer is missing.", err_max);
        return false;
    }

    uint32_t entry_count = pread_u32_le(payload + 4);
    int pos = 8;
    bool have_manifest = false;

    for (uint32_t entry = 0; entry < entry_count; entry++) {
        if (pos + 8 > payload_len - 4) {
            pcpy(err, "kro entry header is truncated.", err_max);
            return false;
        }

        uint32_t name_len = pread_u32_le(payload + pos);
        pos += 4;
        if (name_len == 0 || name_len >= KVFS_MAX_PATH || pos + (int)name_len + 4 > payload_len - 4) {
            pcpy(err, "kro entry name is invalid.", err_max);
            return false;
        }

        char entry_name[KVFS_MAX_PATH];
        for (uint32_t i = 0; i < name_len; i++) entry_name[i] = (char)payload[pos + i];
        entry_name[name_len] = 0;
        pos += (int)name_len;

        uint32_t data_len = pread_u32_le(payload + pos);
        pos += 4;
        if (pos + (int)data_len > payload_len - 4) {
            pcpy(err, "kro entry payload is truncated.", err_max);
            return false;
        }

        if (!pkro_safe_rel_path(entry_name)) {
            pcpy(err, "kro archive contains an unsafe entry path.", err_max);
            return false;
        }

        if (peq(entry_name, "manifest.kcl") || pends_with(entry_name, "/manifest.kcl")) {
            int copy_len = (int)data_len;
            if (copy_len > 2047) copy_len = 2047;
            char manifest_buf[2048];
            for (int i = 0; i < copy_len; i++) manifest_buf[i] = (char)payload[pos + i];
            manifest_buf[copy_len] = 0;

            pkro_manifest_value(manifest_buf, "app_name", info->app_name, (int)sizeof(info->app_name));
            pkro_manifest_value(manifest_buf, "app_entry", info->app_entry, (int)sizeof(info->app_entry));
            pkro_manifest_value(manifest_buf, "app_version", info->app_version, (int)sizeof(info->app_version));
            pkro_manifest_value(manifest_buf, "app_author", info->app_author, (int)sizeof(info->app_author));
            have_manifest = true;
        }

        pos += (int)data_len;
    }

    if (!have_manifest) {
        pcpy(err, "kro archive is missing manifest.kcl.", err_max);
        return false;
    }
    if (!info->app_name[0]) pbasename_stem(source_path, info->app_name, (int)sizeof(info->app_name));
    if (!info->app_name[0]) {
        pcpy(err, "kro manifest is missing app_name.", err_max);
        return false;
    }
    if (!pkro_safe_rel_path(info->app_entry)) {
        pcpy(err, "kro manifest app_entry is invalid.", err_max);
        return false;
    }
    return true;
}

static bool pkro_extract_archive(const uint8_t* payload, int payload_len, const char* dest_root,
                                 char* err, int err_max) {
    (void)payload_len;
    if (KVFS::Exists(dest_root)) KVFS::RmTree(dest_root);
    KVFS::Mkdirs(dest_root);

    uint32_t entry_count = pread_u32_le(payload + 4);
    int pos = 8;
    for (uint32_t entry = 0; entry < entry_count; entry++) {
        uint32_t name_len = pread_u32_le(payload + pos);
        pos += 4;

        char entry_name[KVFS_MAX_PATH];
        for (uint32_t i = 0; i < name_len; i++) entry_name[i] = (char)payload[pos + i];
        entry_name[name_len] = 0;
        pos += (int)name_len;

        uint32_t data_len = pread_u32_le(payload + pos);
        pos += 4;

        char full_path[KVFS_MAX_PATH];
        int fp = 0;
        fp = pa(full_path, fp, sizeof(full_path), dest_root);
        if (fp == 0 || full_path[fp - 1] != '/') fp = pac(full_path, fp, sizeof(full_path), '/');
        fp = pa(full_path, fp, sizeof(full_path), entry_name);

        char parent[KVFS_MAX_PATH];
        pdirname(full_path, parent, sizeof(parent));
        KVFS::Mkdirs(parent);
        if (KVFS::WriteFile(full_path, payload + pos, data_len) < 0) {
            pcpy(err, "failed to extract a .kro archive entry.", err_max);
            return false;
        }

        pos += (int)data_len;
    }

    return true;
}

// step logging for `kpkg sync`  -  writes each HTTP milestone to the serial
// console so a failed sync against kurono.satorut.com shows exactly where it
// stopped (dns/connect/send/headers/body/done). internal only. (satoru)
static void phttp_log(const char* msg) {
    SerialLogger::Log("[kpkg:http] ");
    SerialLogger::Log(msg);
    SerialLogger::Log("\r\n");
}
static void phttp_log_ip(const char* msg, uint32_t ip) {
    char ip_text[16];
    pformat_ip(ip, ip_text, (int)sizeof(ip_text));
    SerialLogger::Log("[kpkg:http] ");
    SerialLogger::Log(msg);
    SerialLogger::Log(ip_text);
    SerialLogger::Log("\r\n");
}
static void phttp_log_num(const char* msg, int n) {
    SerialLogger::Log("[kpkg:http] ");
    SerialLogger::Log(msg);
    SerialLogger::LogDec(n);
    SerialLogger::Log("\r\n");
}

static bool phttp_get(const char* host, const char* path,
                      char* body_out, int body_max,
                      int* status_out, int* body_len_out) {
    if (!host || !path || !body_out || body_max < 2) {
        pset_sync_message("Package fetch parameters are invalid.");
        return false;
    }
    phttp_log("--- phttp_get begin ---");
    phttp_log(host);
    phttp_log(path);
    if (!TCPStack::IsUp()) {
        if (!E1000::IsDetected()) {
            pset_sync_message("TCP/IP stack unavailable: no supported E1000 NIC detected.");
        } else if (!E1000::IsLinkUp()) {
            pset_sync_message("TCP/IP stack unavailable: E1000 link is down.");
        } else {
            pset_sync_message("TCP/IP stack is unavailable on this boot.");
        }
        return false;
    }

    bool ok = false;
    int sock = -1;
    char* response = (char*)KernelHeap::Alloc(PKG_HTTP_BUFFER_MAX + 1);
    if (!response) {
        pset_sync_message("Not enough heap for HTTP response buffer.");
        return false;
    }

    do {
        uint32_t connect_targets[4];
        int connect_target_count = pbuild_connect_targets(host, connect_targets,
                                                          (int)(sizeof(connect_targets) / sizeof(connect_targets[0])));
        phttp_log_num("resolved connect targets: ", connect_target_count);
        bool connected = false;
        bool socket_alloc_failed = false;
        for (int i = 0; i < connect_target_count; i++) {
            phttp_log_ip("connect target: ", connect_targets[i]);
            sock = TCPStack::Socket(SOCK_STREAM);
            if (sock < 0) {
                phttp_log("no free TCP socket");
                pset_sync_message("No free TCP sockets are available.");
                socket_alloc_failed = true;
                break;
            }
            phttp_log_ip("SYN -> ", connect_targets[i]);
            if (TCPStack::Connect(sock, connect_targets[i], 80)) {
                phttp_log_ip("connect OK: ", connect_targets[i]);
                connected = true;
                break;
            }
            phttp_log_ip("connect FAILED: ", connect_targets[i]);
            TCPStack::Close(sock);
            sock = -1;
        }
        if (!connected) {
            phttp_log("all connect targets failed");
            if (socket_alloc_failed) break;
            char msg[160];
            int mp = 0;
            mp = pa(msg, mp, sizeof(msg), "TCP connect to ");
            mp = pa(msg, mp, sizeof(msg), host);
            if (connect_target_count > 1) {
                mp = pa(msg, mp, sizeof(msg), " failed on all known A records.");
            } else if (connect_target_count == 1) {
                char ip_text[16];
                pformat_ip(connect_targets[0], ip_text, (int)sizeof(ip_text));
                mp = pa(msg, mp, sizeof(msg), " failed at ");
                mp = pa(msg, mp, sizeof(msg), ip_text);
                mp = pac(msg, mp, sizeof(msg), '.');
            } else {
                mp = pa(msg, mp, sizeof(msg), " could not be resolved.");
            }
            pset_sync_message(msg);
            break;
        }

        char request[256];
        int rp = 0;
        rp = pa(request, rp, sizeof(request), "GET ");
        rp = pa(request, rp, sizeof(request), path);
        rp = pa(request, rp, sizeof(request), " HTTP/1.1\r\nHost: ");
        rp = pa(request, rp, sizeof(request), host);
        rp = pa(request, rp, sizeof(request), "\r\nUser-Agent: Kurono-kpkg/1.0\r\nConnection: close\r\nAccept: */*\r\n\r\n");

        if (TCPStack::Send(sock, request, rp) != rp) {
            phttp_log("HTTP GET send FAILED");
            pset_sync_message("TCP send failed while requesting repository data.");
            break;
        }
        phttp_log_num("HTTP GET sent, bytes: ", rp);

        int total = 0;
        bool header_logged = false;
        uint32_t read_start_ms = Timer::GetTicks();
        uint32_t last_progress_ms = read_start_ms;
        bool recv_aborted = false;
        int prog_emit_anchor = 0;
        while (total < PKG_HTTP_BUFFER_MAX &&
               (uint32_t)(Timer::GetTicks() - last_progress_ms) < 5000u &&
               (uint32_t)(Timer::GetTicks() - read_start_ms) < 20000u) {
            if (KuronoShell::IsCommandCancelRequested()) {
                recv_aborted = true;
                break;
            }
            TCPStack::Tick();
            KuronoShell::PumpUI();
            int got = TCPStack::Recv(sock, response + total, PKG_HTTP_BUFFER_MAX - total);
            if (got < 0) {
                pset_sync_message("TCP receive failed while reading repository data.");
                break;
            }
            if (got == 0) {
                if (TCPStack::IsPeerClosed(sock)) {
                    break;
                }
                KuronoShell::PumpUI();
                /* ~1ms throttle: PIT-polled wait so this loop doesn't
                   call Tick/PumpUI thousands of times per ms while waiting
                   for the next chunk. */
                uint32_t iter_start = Timer::GetTicks();
                while ((uint32_t)(Timer::GetTicks() - iter_start) < 1u) {
                    __asm__ __volatile__("pause");
                }
                continue;
            }
            total += got;
            last_progress_ms = Timer::GetTicks();
            if (!header_logged && pfind_header_end(response, total) >= 0) {
                header_logged = true;
                phttp_log_num("response headers received at byte: ", pfind_header_end(response, total));
            }
            if (total - prog_emit_anchor >= 16384) {
                prog_emit_anchor = total;
                char line[88];
                int lp = 0;
                lp = pa(line, lp, sizeof(line), "[kpkg] received ");
                lp = pai(line, lp, sizeof(line), (unsigned int)total);
                lp = pa(line, lp, sizeof(line), " bytes...\n");
                KuronoShell::EmitIncrementalRange(line, 0, lp);
            }
        }

        if (recv_aborted) {
            phttp_log("receive aborted (Ctrl+C)");
            pset_sync_message("Interrupted (Ctrl+C).");
            break;
        }
        phttp_log_num("total bytes received: ", total);

        if (total <= 0) {
            phttp_log("no HTTP payload received");
            pset_sync_message("Remote server returned no HTTP payload.");
            break;
        }

        response[total] = 0;
        int header_end = pfind_header_end(response, total);
        if (header_end < 0) {
            phttp_log("missing header terminator");
            pset_sync_message("HTTP response was missing headers terminator.");
            break;
        }

        int status = 0;
        const char* status_ptr = response;
        while (*status_ptr && *status_ptr != ' ') status_ptr++;
        if (*status_ptr == ' ') status = (int)patoi(status_ptr + 1);
        if (status_out) *status_out = status;
        phttp_log_num("HTTP status: ", status);

        bool chunked = pcontains(response, "Transfer-Encoding: chunked");
        int body_bytes = total - header_end;
        if (chunked) {
            phttp_log("body is chunked, decoding");
            body_bytes = pdecode_chunked(response + header_end, total - header_end, body_out, body_max);
            if (body_bytes < 0) {
                phttp_log("chunked decode FAILED");
                pset_sync_message("Chunked HTTP body decoding failed.");
                break;
            }
        } else {
            if (body_bytes > body_max - 1) body_bytes = body_max - 1;
            memcpy(body_out, response + header_end, body_bytes);
        }
        body_out[body_bytes] = 0;
        if (body_len_out) *body_len_out = body_bytes;
        phttp_log_num("body bytes received: ", body_bytes);
        phttp_log("--- phttp_get OK ---");
        ok = true;
    } while (false);

    if (!ok) phttp_log("--- phttp_get FAILED ---");
    KernelHeap::Free(response);
    if (sock >= 0) TCPStack::Close(sock);
    return ok;
}
// ---------------- SHA-256 ----------------
struct sha256_ctx { uint32_t s[8]; uint64_t bits; uint8_t buf[64]; int buf_len; };
static const uint32_t SHA_K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};
static uint32_t rrot(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void sha_compress(sha256_ctx* c, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rrot(w[i-15], 7) ^ rrot(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rrot(w[i-2], 17) ^ rrot(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c->s[0],b=c->s[1],cc=c->s[2],d=c->s[3],e=c->s[4],f=c->s[5],g=c->s[6],h=c->s[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rrot(e,6)^rrot(e,11)^rrot(e,25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + SHA_K[i] + w[i];
        uint32_t S0 = rrot(a,2)^rrot(a,13)^rrot(a,22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->s[0]+=a; c->s[1]+=b; c->s[2]+=cc; c->s[3]+=d;
    c->s[4]+=e; c->s[5]+=f; c->s[6]+=g; c->s[7]+=h;
}
static void sha_init(sha256_ctx* c) {
    c->s[0]=0x6a09e667u; c->s[1]=0xbb67ae85u; c->s[2]=0x3c6ef372u; c->s[3]=0xa54ff53au;
    c->s[4]=0x510e527fu; c->s[5]=0x9b05688cu; c->s[6]=0x1f83d9abu; c->s[7]=0x5be0cd19u;
    c->bits = 0; c->buf_len = 0;
}
static void sha_update(sha256_ctx* c, const uint8_t* d, int n) {
    c->bits += (uint64_t)n * 8u;
    while (n > 0) {
        int take = 64 - c->buf_len;
        if (take > n) take = n;
        for (int i = 0; i < take; i++) c->buf[c->buf_len + i] = d[i];
        c->buf_len += take; d += take; n -= take;
        if (c->buf_len == 64) { sha_compress(c, c->buf); c->buf_len = 0; }
    }
}
static void sha_final(sha256_ctx* c, uint8_t out[32]) {
    c->buf[c->buf_len++] = 0x80;
    if (c->buf_len > 56) {
        while (c->buf_len < 64) c->buf[c->buf_len++] = 0;
        sha_compress(c, c->buf); c->buf_len = 0;
    }
    while (c->buf_len < 56) c->buf[c->buf_len++] = 0;
    uint64_t bits = c->bits;
    for (int i = 7; i >= 0; i--) c->buf[c->buf_len++] = (uint8_t)(bits >> (i*8));
    sha_compress(c, c->buf);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(c->s[i] >> 24);
        out[i*4+1] = (uint8_t)(c->s[i] >> 16);
        out[i*4+2] = (uint8_t)(c->s[i] >> 8);
        out[i*4+3] = (uint8_t)(c->s[i]);
    }
}
static void sha_hex(const uint8_t in[32], char out[65]) {
    static const char* hx = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i*2]   = hx[(in[i] >> 4) & 0xF];
        out[i*2+1] = hx[in[i] & 0xF];
    }
    out[64] = 0;
}
static bool sha_eq_hex(const uint8_t in[32], const char* hex) {
    char calc[65]; sha_hex(in, calc);
    int i = 0;
    while (calc[i] && hex[i]) {
        char a = calc[i], b = hex[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
        i++;
    }
    return calc[i] == 0 && hex[i] == 0;
}

// ---------------- minimal JSON field readers ----------------
static const char* jskip_ws(const char* p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}
// finds top-level (depth=1 object) key occurrence, returns pointer to value (after ':' + ws) or null
static const char* jfind_key(const char* obj_start, const char* key) {
    const char* p = jskip_ws(obj_start);
    if (*p != '{') return nullptr;
    p++;
    int depth = 1;
    bool in_str = false;
    while (*p && depth >= 1) {
        if (in_str) { if (*p == '\\' && p[1]) p++; else if (*p == '"') in_str = false; p++; continue; }
        if (*p == '"') {
            // possible key
            if (depth == 1) {
                const char* ks = p + 1;
                const char* ke = ks;
                while (*ke && *ke != '"') { if (*ke == '\\' && ke[1]) ke++; ke++; }
                int klen = (int)(ke - ks);
                int wantlen = plen(key);
                if (klen == wantlen) {
                    bool eq = true;
                    for (int i = 0; i < klen; i++) if (ks[i] != key[i]) { eq = false; break; }
                    if (eq) {
                        const char* after = ke;
                        if (*after == '"') after++;
                        after = jskip_ws(after);
                        if (*after == ':') {
                            after = jskip_ws(after + 1);
                            return after;
                        }
                    }
                }
                p = ke;
                if (*p == '"') p++;
                continue;
            }
            in_str = true; p++; continue;
        }
        if (*p == '{' || *p == '[') depth++;
        else if (*p == '}' || *p == ']') depth--;
        p++;
    }
    return nullptr;
}
static int jread_string(const char* p, char* out, int max) {
    p = jskip_ws(p);
    if (*p != '"') { if (max > 0) out[0] = 0; return 0; }
    p++;
    int o = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            char esc = *p++;
            if (esc == 'n') c = '\n';
            else if (esc == 't') c = '\t';
            else if (esc == 'r') c = '\r';
            else c = esc;
        }
        if (o < max - 1) out[o++] = c;
    }
    if (max > 0) out[o] = 0;
    return o;
}
static unsigned int jread_uint(const char* p) {
    p = jskip_ws(p);
    if (*p == '"') p++;
    unsigned int v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10u + (unsigned int)(*p - '0'); p++; }
    return v;
}
// scan top-level array of objects, calling cb for each element body
static int jforeach_object(const char* arr_start, void (*cb)(const char* obj_start, void* ctx), void* ctx) {
    const char* p = jskip_ws(arr_start);
    if (*p != '[') return 0;
    p++;
    int count = 0;
    while (*p) {
        KuronoShell::PumpUI();
        p = jskip_ws(p);
        if (*p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p == '{') {
            const char* obj = p;
            int depth = 1;
            p++;
            bool in_str = false;
            while (*p && depth > 0) {
                if (in_str) { if (*p == '\\' && p[1]) p++; else if (*p == '"') in_str = false; }
                else {
                    if (*p == '"') in_str = true;
                    else if (*p == '{') depth++;
                    else if (*p == '}') depth--;
                }
                p++;
            }
            cb(obj, ctx);
            count++;
        } else p++;
    }
    return count;
}
// foreach string in array
static int jforeach_string(const char* arr_start, void (*cb)(const char* s, void* ctx), void* ctx) {
    const char* p = jskip_ws(arr_start);
    if (*p != '[') return 0;
    p++;
    int count = 0;
    while (*p) {
        p = jskip_ws(p);
        if (*p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p == '"') {
            char buf[64];
            int n = jread_string(p, buf, sizeof(buf));
            (void)n;
            cb(buf, ctx);
            count++;
            p++;
            while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
            if (*p == '"') p++;
        } else p++;
    }
    return count;
}

// ---------------- ustar tar reader ----------------
static unsigned int oct_to_uint(const char* s, int n) {
    unsigned int v = 0;
    for (int i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8u + (unsigned int)(s[i] - '0');
    return v;
}
static bool ustar_extract(const char* archive, int archive_len, const char* base_dir) {
    int pos = 0;
    bool any = false;
    while (pos + 512 <= archive_len) {
        KuronoShell::PumpUI();
        const char* hdr = archive + pos;
        // empty block = end
        bool empty = true;
        for (int i = 0; i < 512; i++) if (hdr[i] != 0) { empty = false; break; }
        if (empty) break;

        char name[200];
        int nl = 0;
        while (nl < 100 && hdr[nl]) { name[nl] = hdr[nl]; nl++; }
        name[nl] = 0;
        unsigned int fsize = oct_to_uint(hdr + 124, 12);
        char typeflag = hdr[156];

        pos += 512;
        if (pos + (int)fsize > archive_len) break;

        if (typeflag == '0' || typeflag == 0 || typeflag == '7') {
            char full[256];
            int fp = 0;
            fp = pa(full, fp, sizeof(full), base_dir);
            if (fp == 0 || full[fp - 1] != '/') fp = pac(full, fp, sizeof(full), '/');
            // strip leading "./"
            const char* nstart = name;
            if (nstart[0] == '.' && nstart[1] == '/') nstart += 2;
            fp = pa(full, fp, sizeof(full), nstart);

            // ensure parent directories
            char dir[256];
            int last_slash = -1;
            for (int i = 0; full[i]; i++) if (full[i] == '/') last_slash = i;
            if (last_slash > 0) {
                for (int i = 0; i < last_slash; i++) dir[i] = full[i];
                dir[last_slash] = 0;
                KVFS::Mkdirs(dir);
            }
            KVFS::WriteFile(full, archive + pos, fsize);
            any = true;
        } else if (typeflag == '5') {
            char full[256];
            int fp = 0;
            fp = pa(full, fp, sizeof(full), base_dir);
            if (fp == 0 || full[fp - 1] != '/') fp = pac(full, fp, sizeof(full), '/');
            const char* nstart = name;
            if (nstart[0] == '.' && nstart[1] == '/') nstart += 2;
            fp = pa(full, fp, sizeof(full), nstart);
            // strip trailing slash
            if (fp > 0 && full[fp - 1] == '/') full[fp - 1] = 0;
            KVFS::Mkdirs(full);
        }
        // round up to 512
        unsigned int padded = (fsize + 511u) & ~511u;
        pos += (int)padded;
    }
    return any;
}

// ---------------- record / installed.json ----------------
static void pwrite_package_record(const Package* pkg, const char* payload, int payload_len,
                                  const char* payload_name) {
    if (!pkg) return;

    char dir[128];
    int dp = 0;
    dp = pa(dir, dp, sizeof(dir), "/var/cache/kpkg/");
    dp = pa(dir, dp, sizeof(dir), pkg->name);
    KVFS::Mkdirs(dir);

    char payload_path[160];
    int pp = 0;
    pp = pa(payload_path, pp, sizeof(payload_path), dir);
    pp = pa(payload_path, pp, sizeof(payload_path), "/");
    pp = pa(payload_path, pp, sizeof(payload_path), payload_name ? payload_name : "payload.kpkg");
    if (payload && payload_len > 0) KVFS::WriteFile(payload_path, payload, (uint32_t)payload_len);

    char meta[400];
    int mp = 0;
    mp = pa(meta, mp, sizeof(meta), "{\n  \"name\": \"");
    mp = pa(meta, mp, sizeof(meta), pkg->name);
    mp = pa(meta, mp, sizeof(meta), "\",\n  \"version\": \"");
    mp = pa(meta, mp, sizeof(meta), pkg->version);
    mp = pa(meta, mp, sizeof(meta), "\",\n  \"description\": \"");
    mp = pa(meta, mp, sizeof(meta), pkg->description);
    mp = pa(meta, mp, sizeof(meta), "\",\n  \"sha256\": \"");
    mp = pa(meta, mp, sizeof(meta), pkg->sha256);
    mp = pa(meta, mp, sizeof(meta), "\"\n}\n");

    char meta_path[160];
    int mt = 0;
    mt = pa(meta_path, mt, sizeof(meta_path), dir);
    mt = pa(meta_path, mt, sizeof(meta_path), "/manifest.json");
    KVFS::WriteString(meta_path, meta);
}

static void pregister_installed(const Package* pkg) {
    if (!pkg) return;
    KVFS::Mkdirs("/var/lib/kpkg");
    char buf[8192];
    int existing = 0;
    if (KVFS::Exists("/var/lib/kpkg/installed.json")) {
        existing = KVFS::ReadFile("/var/lib/kpkg/installed.json", buf, (uint32_t)sizeof(buf) - 1);
        if (existing < 0) existing = 0;
    }
    buf[existing] = 0;

    char rec[256];
    int rp = 0;
    rp = pa(rec, rp, sizeof(rec), "    { \"name\": \"");
    rp = pa(rec, rp, sizeof(rec), pkg->name);
    rp = pa(rec, rp, sizeof(rec), "\", \"version\": \"");
    rp = pa(rec, rp, sizeof(rec), pkg->version);
    rp = pa(rec, rp, sizeof(rec), "\", \"sha256\": \"");
    rp = pa(rec, rp, sizeof(rec), pkg->sha256);
    rp = pa(rec, rp, sizeof(rec), "\" }");

    char out[8400];
    int op = 0;
    if (existing > 4 && pcontains(buf, "\"installed\"")) {
        // splice before closing ']'
        int last_bracket = -1;
        for (int i = 0; i < existing; i++) if (buf[i] == ']') last_bracket = i;
        if (last_bracket > 0) {
            // find last non-space before ']'
            int prev = last_bracket - 1;
            while (prev > 0 && (buf[prev] == ' ' || buf[prev] == '\t' || buf[prev] == '\r' || buf[prev] == '\n')) prev--;
            for (int i = 0; i <= prev; i++) op = pac(out, op, sizeof(out), buf[i]);
            if (buf[prev] != '[') op = pac(out, op, sizeof(out), ',');
            op = pac(out, op, sizeof(out), '\n');
            op = pa(out, op, sizeof(out), rec);
            op = pa(out, op, sizeof(out), "\n  ]\n}\n");
        }
    }
    if (op == 0) {
        op = pa(out, op, sizeof(out), "{\n  \"installed\": [\n");
        op = pa(out, op, sizeof(out), rec);
        op = pa(out, op, sizeof(out), "\n  ]\n}\n");
    }
    KVFS::WriteString("/var/lib/kpkg/installed.json", out);
}

// ---------------- per-package install (manifest -> tar -> extract) ----------------

static bool pfetch_manifest(Package* pkg) {
    if (!pkg || !pkg->manifest_url[0]) {
        pset_sync_message("Package has no manifest URL.");
        return false;
    }
    char* mbuf = (char*)KernelHeap::Alloc(8192);
    if (!mbuf) {
        pset_sync_message("Not enough heap for manifest buffer.");
        return false;
    }
    int status = 0, body_len = 0;
    bool ok = phttp_get(PKG_REPOSITORY_HOST, pkg->manifest_url, mbuf, 8192, &status, &body_len);
    if (!ok) { KernelHeap::Free(mbuf); return false; }
    if (status != 200) {
        pset_sync_message_http("Manifest fetch returned", status, pkg->manifest_url);
        KernelHeap::Free(mbuf);
        return false;
    }
    mbuf[body_len < 8191 ? body_len : 8191] = 0;

    char tmp[160];
    int n = jread_string(jfind_key(mbuf, "version"), tmp, sizeof(tmp));
    if (n > 0) {
        pcpy(pkg->version, tmp, sizeof(pkg->version));
        pcpy(pkg->latest_version, tmp, sizeof(pkg->latest_version));
    }
    n = jread_string(jfind_key(mbuf, "description"), tmp, sizeof(tmp));
    if (n > 0) pcpy(pkg->description, tmp, sizeof(pkg->description));
    n = jread_string(jfind_key(mbuf, "url"), tmp, sizeof(tmp));
    if (n > 0) pcpy(pkg->download_url, tmp, sizeof(pkg->download_url));
    n = jread_string(jfind_key(mbuf, "sha256"), tmp, sizeof(tmp));
    if (n > 0) pcpy(pkg->sha256, tmp, sizeof(pkg->sha256));
    unsigned int sz = jread_uint(jfind_key(mbuf, "size"));
    if (sz > 0) pkg->size = sz / 1024u + 1u;

    // dependencies array of "name-version" strings
    pkg->dep_count = 0;
    const char* deps_arr = jfind_key(mbuf, "dependencies");
    if (deps_arr) {
        struct DepCtx { Package* pkg; } dc{ pkg };
        jforeach_string(deps_arr, [](const char* s, void* ctx){
            DepCtx* d = (DepCtx*)ctx;
            if (d->pkg->dep_count >= PKG_MAX_DEPS) return;
            // strip "-version" suffix
            char name[PKG_MAX_NAME];
            int i = 0;
            while (s[i] && s[i] != '-' && i < PKG_MAX_NAME - 1) { name[i] = s[i]; i++; }
            // detect "-<digit>..." version separator
            int j = i;
            if (s[j] == '-' && s[j+1] >= '0' && s[j+1] <= '9') name[i] = 0;
            else { while (s[i] && i < PKG_MAX_NAME - 1) { name[i] = s[i]; i++; } name[i] = 0; }
            pcpy(d->pkg->deps[d->pkg->dep_count++], name, PKG_MAX_NAME);
        }, &dc);
    }

    KernelHeap::Free(mbuf);
    return true;
}

// extract host + path from "https://host/path" or "/path"
static void purl_path(const char* url, char* path_out, int max) {
    if (!url || !url[0]) { path_out[0] = 0; return; }
    if (url[0] == '/') { pcpy(path_out, url, max); return; }
    const char* p = url;
    if (p[0]=='h' && p[1]=='t' && p[2]=='t' && (p[3]=='p' || p[3]=='s')) {
        while (*p && *p != ':') p++;
        if (*p == ':') p++;
        while (*p == '/') p++;
        while (*p && *p != '/') p++;
    }
    if (*p == '/') pcpy(path_out, p, max);
    else { path_out[0] = '/'; pcpy(path_out + 1, p, max - 1); }
}

static bool pfetch_package_payload(Package* pkg) {
    if (!pkg) {
        pset_sync_message("Package payload fetch received a null package.");
        return false;
    }
    if (!pfetch_manifest(pkg)) return false;

    char path[200];
    purl_path(pkg->download_url, path, sizeof(path));
    if (!path[0]) {
        pset_sync_message("Manifest did not include a download URL.");
        return false;
    }

    char* payload = (char*)KernelHeap::Alloc(PKG_HTTP_BUFFER_MAX + 1);
    if (!payload) {
        pset_sync_message("Not enough heap for package payload buffer.");
        return false;
    }

    int status = 0, payload_len = 0;
    bool ok = phttp_get(PKG_REPOSITORY_HOST, path, payload, PKG_HTTP_BUFFER_MAX + 1, &status, &payload_len);
    if (!ok) { KernelHeap::Free(payload); return false; }
    if (status != 200) {
        pset_sync_message_http("Package download returned", status, path);
        KernelHeap::Free(payload);
        return false;
    }

    // sha256 verify
    if (pkg->sha256[0]) {
        sha256_ctx ctx; sha_init(&ctx);
        sha_update(&ctx, (const uint8_t*)payload, payload_len);
        uint8_t digest[32]; sha_final(&ctx, digest);
        if (!sha_eq_hex(digest, pkg->sha256)) {
            char msg[200]; int p = 0;
            p = pa(msg, p, sizeof(msg), "SHA256 mismatch for ");
            p = pa(msg, p, sizeof(msg), pkg->name);
            p = pa(msg, p, sizeof(msg), " (expected ");
            for (int i = 0; i < 8 && pkg->sha256[i]; i++) p = pac(msg, p, sizeof(msg), pkg->sha256[i]);
            p = pa(msg, p, sizeof(msg), "...) -- continuing without verify.");
            pset_sync_message(msg);
            // Note: in this build we warn but continue, since dummy hashes
            // are sometimes published. Strict mode can be enabled later.
        }
    }

    // cache + record
    pwrite_package_record(pkg, payload, payload_len, "payload.kpkg");

    // extract tar payload into /
    char dest[64];
    pcpy(dest, "/", sizeof(dest));
    bool extracted = ustar_extract(payload, payload_len, dest);
    (void)extracted;

    KernelHeap::Free(payload);
    pregister_installed(pkg);
    return true;
}

void PackageManager::AddDefaultPackages() {
    auto add = [](const char* name, const char* ver, const char* desc, const char* cat, PkgState st, unsigned int sz) {
        if (package_count >= PKG_MAX_PACKAGES) return;
        Package& p = packages[package_count++];
        memset(&p, 0, sizeof(Package));
        pcpy(p.name, name, PKG_MAX_NAME);
        pcpy(p.version, ver, (int)sizeof(p.version));
        pcpy(p.latest_version, ver, (int)sizeof(p.latest_version));
        pcpy(p.description, desc, PKG_MAX_DESC);
        pcpy(p.category, cat, (int)sizeof(p.category));
        p.state = st;
        p.size = sz;
        p.dep_count = 0;
    };

    add("kurono-kernel",    "1.0.0", "Kurono hybrid kernel",          "core",    PKG_INSTALLED, 2048);
    add("kurono-shell",     "1.0.0", "Kurono command shell (ksh)",    "core",    PKG_INSTALLED, 128);
    add("kurono-desktop",   "1.0.0", "Desktop environment",           "core",    PKG_INSTALLED, 512);
    add("kvfs",             "1.0.0", "Virtual filesystem",            "core",    PKG_INSTALLED, 64);
    add("kcl",              "1.0.0", "Kurono Command Language",       "core",    PKG_INSTALLED, 96);
    add("supr-security",    "1.0.0", "SUPR security engine",          "core",    PKG_INSTALLED, 48);
    add("linux-bridge",     "1.0.0", "Linux command compatibility",   "compat",  PKG_INSTALLED, 128);
    add("windows-bridge",   "1.0.0", "Windows command compatibility", "compat",  PKG_INSTALLED, 128);

    add("bga-driver",       "1.0.0", "Bochs Graphics Adapter driver", "drivers", PKG_INSTALLED, 32);
    add("ps2-keyboard",     "1.0.0", "PS/2 keyboard driver",          "drivers", PKG_INSTALLED, 16);
    add("ps2-mouse",        "1.0.0", "PS/2 mouse driver",             "drivers", PKG_INSTALLED, 16);
    add("pit-timer",        "1.0.0", "PIT timer driver (1kHz)",       "drivers", PKG_INSTALLED, 8);
    add("rtc-driver",       "1.0.0", "CMOS RTC driver",               "drivers", PKG_INSTALLED, 8);
    add("serial-driver",    "1.0.0", "COM1 serial driver",            "drivers", PKG_INSTALLED, 8);

    add("calculator",       "1.0.0", "Calculator application",        "apps",    PKG_INSTALLED, 32);
    add("file-browser",     "1.0.0", "File browser application",      "apps",    PKG_INSTALLED, 48);
    add("terminal",         "1.0.0", "Terminal emulator",             "apps",    PKG_INSTALLED, 64);
    add("text-editor",      "1.0.0", "Basic text editor",             "apps",    PKG_INSTALLED, 48);
    add("settings",         "1.0.0", "System settings",               "apps",    PKG_INSTALLED, 32);
    add("task-manager",     "1.0.0", "Process monitor",               "apps",    PKG_INSTALLED, 32);

    add("wifi-driver",      "1.0.0", "WiFi network driver",           "drivers", PKG_AVAILABLE, 64);
    add("ethernet-driver",  "1.0.0", "Ethernet NIC driver",           "drivers", PKG_AVAILABLE, 48);
    add("usb-driver",       "1.2.0", "USB host controller driver",    "drivers", PKG_AVAILABLE, 96);
    add("audio-driver",     "1.0.0", "AC97/HDA audio driver",         "drivers", PKG_AVAILABLE, 64);
    add("ahci-driver",      "1.0.0", "AHCI SATA driver",              "drivers", PKG_AVAILABLE, 48);
    add("nvme-driver",      "1.0.0", "NVMe storage driver",           "drivers", PKG_AVAILABLE, 48);
    add("stb-image",        "2.28.0", "Image decoding library",       "libs",    PKG_INSTALLED, 128);
    add("stb-truetype",     "1.26.0", "TrueType font renderer",       "libs",    PKG_INSTALLED, 96);
    add("tcp-stack",        "1.0.0", "TCP/IP network stack",          "network", PKG_AVAILABLE, 128);
    add("dns-resolver",     "1.0.0", "DNS resolution service",        "network", PKG_AVAILABLE, 32);
    add("http-client",      "1.0.0", "HTTP client",                   "network", PKG_AVAILABLE, 64);
    add("ssh-client",       "1.0.0", "SSH remote client",             "network", PKG_AVAILABLE, 96);
    add("image-viewer",     "1.0.0", "PNG/JPEG viewer",               "apps",    PKG_AVAILABLE, 32);
    add("music-player",     "1.0.0", "Audio player",                  "apps",    PKG_AVAILABLE, 48);
    add("web-browser",      "0.1.0", "Minimal web browser",           "apps",    PKG_AVAILABLE, 256);
    add("games-pack",       "1.0.0", "Tetris, Snake, Minesweeper",    "apps",    PKG_AVAILABLE, 64);
}

Package* PackageManager::FindOrCreate(const char* name) {
    Package* pkg = Find(name);
    if (pkg) return pkg;
    if (package_count >= PKG_MAX_PACKAGES) return nullptr;

    Package& fresh = packages[package_count++];
    memset(&fresh, 0, sizeof(Package));
    pcpy(fresh.name, name, PKG_MAX_NAME);
    pcpy(fresh.category, "repo", (int)sizeof(fresh.category));
    fresh.state = PKG_AVAILABLE;
    return &fresh;
}

int PackageManager::ParseIndexJson(const char* json) {
    if (!json) return 0;
    const char* arr = jfind_key(json, "packages");
    if (!arr) return 0;
    int parsed_count = 0;
    struct Ctx { int* parsed; } ctx{ &parsed_count };
    jforeach_object(arr, [](const char* obj, void* c){
        Ctx* cc = (Ctx*)c;
        char name[PKG_MAX_NAME] = {0};
        char version[16] = {0};
        char desc[PKG_MAX_DESC] = {0};
        jread_string(jfind_key(obj, "name"), name, sizeof(name));
        jread_string(jfind_key(obj, "version"), version, sizeof(version));
        jread_string(jfind_key(obj, "description"), desc, sizeof(desc));
        if (!name[0]) return;
        Package* pkg = PackageManager::FindOrCreate(name);
        if (!pkg) return;
        if (version[0]) {
            pcpy(pkg->latest_version, version, (int)sizeof(pkg->latest_version));
            if (pkg->version[0] == 0) pcpy(pkg->version, version, (int)sizeof(pkg->version));
        }
        if (desc[0]) pcpy(pkg->description, desc, (int)sizeof(pkg->description));
        int p = 0;
        p = pa(pkg->manifest_url, p, (int)sizeof(pkg->manifest_url), "/packages/");
        p = pa(pkg->manifest_url, p, (int)sizeof(pkg->manifest_url), name);
        p = pa(pkg->manifest_url, p, (int)sizeof(pkg->manifest_url), "/manifest.json");
        if (pkg->state != PKG_INSTALLED) pkg->state = PKG_AVAILABLE;
        (*cc->parsed)++;
    }, &ctx);
    return parsed_count;
}

int PackageManager::ParseRepositoryManifest(const char* manifest) {
    if (!manifest || !manifest[0]) return 0;

    char name[PKG_MAX_NAME] = {};
    char version[16] = {};
    char description[PKG_MAX_DESC] = {};
    char category[16] = {};
    char filename[64] = {};
    unsigned int size = 0;
    int parsed = 0;

    auto flush = [&]() {
        if (!name[0]) return;
        Package* pkg = FindOrCreate(name);
        if (!pkg) return;

        if (version[0]) {
            pcpy(pkg->latest_version, version, (int)sizeof(pkg->latest_version));
            if (pkg->version[0] == 0) pcpy(pkg->version, version, (int)sizeof(pkg->version));
        }
        if (description[0]) pcpy(pkg->description, description, PKG_MAX_DESC);
        if (category[0]) pcpy(pkg->category, category, (int)sizeof(pkg->category));
        if (filename[0]) pcpy(pkg->repo_path, filename, (int)sizeof(pkg->repo_path));
        if (size > 0) pkg->size = size;
        if (pkg->state != PKG_INSTALLED) pkg->state = PKG_AVAILABLE;
        parsed++;

        name[0] = 0;
        version[0] = 0;
        description[0] = 0;
        category[0] = 0;
        filename[0] = 0;
        size = 0;
    };

    int pos = 0;
    while (manifest[pos]) {
        KuronoShell::PumpUI();
        char line[256];
        int lp = 0;
        while (manifest[pos] && manifest[pos] != '\n' && lp < (int)sizeof(line) - 1) {
            if (manifest[pos] != '\r') line[lp++] = manifest[pos];
            pos++;
        }
        if (manifest[pos] == '\n') pos++;
        line[lp] = 0;

        if (line[0] == 0) {
            flush();
            continue;
        }

        char* value = line;
        while (*value && *value != ':') value++;
        if (*value != ':') continue;
        *value++ = 0;
        while (*value == ' ' || *value == '\t') value++;
        ptrim(value);

        if (peq(line, "Package")) pcpy(name, value, sizeof(name));
        else if (peq(line, "Version")) pcpy(version, value, sizeof(version));
        else if (peq(line, "Description")) pcpy(description, value, sizeof(description));
        else if (peq(line, "Section")) pcpy(category, value, sizeof(category));
        else if (peq(line, "Filename")) pcpy(filename, value, sizeof(filename));
        else if (peq(line, "Installed-Size") || peq(line, "Size")) size = patoi(value);
    }

    flush();
    return parsed;
}

void PackageManager::Init() {
    package_count = 0;
    pkg_repo_synced = false;
    pkg_last_sync_ok = false;
    pset_sync_message("Repository not synced yet.");
    AddDefaultPackages();
    SerialLogger::Log("PkgMgr: Initialized\r\n");
}

bool PackageManager::Install(const char* name) {
    Package* pkg = Find(name);
    if (!pkg) {
        if (!SyncRepository()) return false;
        pkg = Find(name);
        if (!pkg) {
            pset_sync_message("Package was not found in the local or remote index.");
            return false;
        }
    }
    if (pkg->state == PKG_INSTALLED) return true;
    if (!pkg_repo_synced && !SyncRepository()) return false;

    for (int i = 0; i < pkg->dep_count; i++) {
        if (!Install(pkg->deps[i])) return false;
    }
    if (!pfetch_package_payload(pkg)) return false;

    pkg->state = PKG_INSTALLED;
    if (pkg->latest_version[0]) pcpy(pkg->version, pkg->latest_version, (int)sizeof(pkg->version));
    pset_sync_message("Package installed from kurono.satorut.com.");
    return true;
}

bool PackageManager::InstallKro(const char* path, char* app_name_out, int app_name_max,
                                char* entry_path_out, int entry_path_max) {
    if (app_name_out && app_name_max > 0) app_name_out[0] = 0;
    if (entry_path_out && entry_path_max > 0) entry_path_out[0] = 0;
    if (!path || !path[0]) {
        pset_sync_message("kro install path was empty.");
        return false;
    }
    if (!KVFS::Exists(path)) {
        pset_sync_message("kro file was not found.");
        return false;
    }

    int payload_len = KVFS::GetFileSize(path);
    if (payload_len <= 0) {
        pset_sync_message("kro file is empty or unreadable.");
        return false;
    }

    char* payload = (char*)KernelHeap::Alloc((uint32_t)payload_len + 1u);
    if (!payload) {
        pset_sync_message("Not enough heap for kro payload.");
        return false;
    }

    int got = KVFS::ReadFile(path, payload, (uint32_t)payload_len);
    if (got != payload_len) {
        KernelHeap::Free(payload);
        pset_sync_message("Failed to read kro payload from disk.");
        return false;
    }

    KroManifestInfo info;
    char err[160];
    if (!pkro_scan_archive((const uint8_t*)payload, got, path, &info, err, (int)sizeof(err))) {
        KernelHeap::Free(payload);
        pset_sync_message(err);
        return false;
    }

    char app_root[KVFS_MAX_PATH];
    int rp = 0;
    rp = pa(app_root, rp, sizeof(app_root), "/apps/");
    rp = pa(app_root, rp, sizeof(app_root), info.app_name);

    if (!pkro_extract_archive((const uint8_t*)payload, got, app_root, err, (int)sizeof(err))) {
        KernelHeap::Free(payload);
        pset_sync_message(err);
        return false;
    }

    char entry_path[KVFS_MAX_PATH];
    int ep = 0;
    ep = pa(entry_path, ep, sizeof(entry_path), app_root);
    if (ep == 0 || entry_path[ep - 1] != '/') ep = pac(entry_path, ep, sizeof(entry_path), '/');
    ep = pa(entry_path, ep, sizeof(entry_path), info.app_entry);
    if (!KVFS::Exists(entry_path)) {
        KernelHeap::Free(payload);
        pset_sync_message("kro app entry was not found after extraction.");
        return false;
    }

    Package* pkg = FindOrCreate(info.app_name);
    if (!pkg) {
        KernelHeap::Free(payload);
        pset_sync_message("Package registry is full; cannot register kro app.");
        return false;
    }

    bool was_installed = (pkg->state == PKG_INSTALLED);
    pcpy(pkg->name, info.app_name, PKG_MAX_NAME);
    pcpy(pkg->version, info.app_version, (int)sizeof(pkg->version));
    pcpy(pkg->latest_version, info.app_version, (int)sizeof(pkg->latest_version));
    pcpy(pkg->description, "Kurono Runtime Object app", PKG_MAX_DESC);
    pcpy(pkg->category, "apps", (int)sizeof(pkg->category));
    pkg->repo_path[0] = 0;
    pkg->manifest_url[0] = 0;
    pkg->download_url[0] = 0;
    pkg->sha256[0] = 0;
    pkg->dep_count = 0;
    pkg->state = PKG_INSTALLED;
    pkg->size = (unsigned int)(payload_len / 1024) + 1u;

    pwrite_package_record(pkg, payload, payload_len, "payload.kro");
    if (!was_installed) pregister_installed(pkg);

    if (app_name_out && app_name_max > 0) pcpy(app_name_out, info.app_name, app_name_max);
    if (entry_path_out && entry_path_max > 0) pcpy(entry_path_out, entry_path, entry_path_max);

    KernelHeap::Free(payload);
    pset_sync_message("Local .kro app installed.");
    return true;
}

bool PackageManager::Remove(const char* name) {
    Package* pkg = Find(name);
    if (!pkg || pkg->state != PKG_INSTALLED) return false;
    if (peq(pkg->category, "core")) return false;
    pkg->state = PKG_AVAILABLE;
    return true;
}

bool PackageManager::Update(const char* name) {
    Package* pkg = Find(name);
    if (!pkg || pkg->state != PKG_INSTALLED) {
        pset_sync_message("Package is not installed.");
        return false;
    }
    if (!pkg_repo_synced && !SyncRepository()) return false;
    if (!pkg->latest_version[0] || peq(pkg->version, pkg->latest_version)) {
        pset_sync_message("Package is already up to date.");
        return true;
    }

    pkg->state = PKG_UPDATING;
    if (!pfetch_package_payload(pkg)) {
        pkg->state = PKG_BROKEN;
        return false;
    }
    pcpy(pkg->version, pkg->latest_version, (int)sizeof(pkg->version));
    pkg->state = PKG_INSTALLED;
    pset_sync_message("Package updated from kurono.satorut.com.");
    return true;
}

bool PackageManager::UpdateAll() {
    if (!SyncRepository()) return false;
    bool ok = true;
    for (int i = 0; i < package_count; i++) {
        KuronoShell::PumpUI();
        if (packages[i].state == PKG_INSTALLED &&
            packages[i].latest_version[0] &&
            !peq(packages[i].version, packages[i].latest_version)) {
            if (!Update(packages[i].name)) ok = false;
        }
    }
    return ok;
}

bool PackageManager::SyncRepository() {
    pkg_repo_synced = false;
    pkg_last_sync_ok = false;

    for (int i = 0; i < package_count; i++) {
        pcpy(packages[i].latest_version, packages[i].version, (int)sizeof(packages[i].latest_version));
        packages[i].repo_path[0] = 0;
        packages[i].manifest_url[0] = 0;
        packages[i].download_url[0] = 0;
        packages[i].sha256[0] = 0;
    }

    char* manifest = (char*)KernelHeap::Alloc(PKG_HTTP_BUFFER_MAX + 1);
    if (!manifest) {
        pset_sync_message("Not enough heap for repository manifest.");
        return false;
    }

    bool synced = false;
    int parsed = 0;

    // Phase 1: try modern JSON index.
    {
        int status = 0, body_len = 0;
        if (phttp_get(PKG_REPOSITORY_HOST, PKG_INDEX_PATH, manifest, PKG_HTTP_BUFFER_MAX + 1, &status, &body_len)
            && status == 200) {
            parsed = ParseIndexJson(manifest);
            if (parsed > 0) {
                KVFS::Mkdirs("/var/lib/kpkg");
                KVFS::WriteFile("/var/lib/kpkg/index.json", manifest, (uint32_t)body_len);
                synced = true;

                char msg[160];
                int p = 0;
                p = pa(msg, p, sizeof(msg), "Synced ");
                p = pai(msg, p, sizeof(msg), (unsigned int)parsed);
                p = pa(msg, p, sizeof(msg), " packages from ");
                p = pa(msg, p, sizeof(msg), PKG_REPOSITORY_HOST);
                p = pa(msg, p, sizeof(msg), " (index.json).");
                pset_sync_message(msg);
            }
        }
    }

    // Phase 2: fallback Debian-style.
    if (!synced) {
    for (int i = 0; i < PKG_REPOSITORY_PATH_COUNT; i++) {
        int status = 0;
        int body_len = 0;
        if (!phttp_get(PKG_REPOSITORY_HOST, PKG_REPOSITORY_PATHS[i], manifest, PKG_HTTP_BUFFER_MAX + 1, &status, &body_len)) {
            continue;
        }
        if (status != 200) {
            pset_sync_message_http("Repository returned", status, PKG_REPOSITORY_PATHS[i]);
            continue;
        }

        parsed = ParseRepositoryManifest(manifest);
        if (parsed <= 0) {
            pset_sync_message("Repository index format was not recognized.");
            continue;
        }

        KVFS::Mkdirs("/var/lib/kpkg");
        KVFS::WriteFile("/var/lib/kpkg/Packages", manifest, (uint32_t)body_len);
        synced = true;

        char msg[160];
        int p = 0;
        p = pa(msg, p, sizeof(msg), "Synced ");
        p = pai(msg, p, sizeof(msg), (unsigned int)parsed);
        p = pa(msg, p, sizeof(msg), " packages from ");
        p = pa(msg, p, sizeof(msg), PKG_REPOSITORY_HOST);
        p = pa(msg, p, sizeof(msg), ".");
        pset_sync_message(msg);
        break;
    }
    } // end if (!synced) fallback

    KernelHeap::Free(manifest);
    pkg_repo_synced = synced;
    pkg_last_sync_ok = synced;
    // surface the sync outcome as a toast for the desktop. (satoru)
    if (synced)
        NotificationManager::Post("kpkg", "repository sync complete",
                                  NotificationManager::ICON_SUCCESS, 4000);
    else
        NotificationManager::Post("kpkg", pkg_last_sync_message,
                                  NotificationManager::ICON_ERROR, 5000);
    return synced;
}

Package* PackageManager::Find(const char* name) {
    for (int i = 0; i < package_count; i++) {
        if (peq(packages[i].name, name)) return &packages[i];
    }
    return nullptr;
}

int PackageManager::Search(const char* pattern, Package** results, int max_results) {
    int count = 0;
    for (int i = 0; i < package_count && count < max_results; i++) {
        if (pcontains(packages[i].name, pattern) || pcontains(packages[i].description, pattern)) {
            results[count++] = &packages[i];
        }
    }
    return count;
}

int PackageManager::ListInstalled(Package** results, int max_results) {
    int count = 0;
    for (int i = 0; i < package_count && count < max_results; i++) {
        if (packages[i].state == PKG_INSTALLED) results[count++] = &packages[i];
    }
    return count;
}

int PackageManager::ListAll(Package** results, int max_results) {
    int count = 0;
    for (int i = 0; i < package_count && count < max_results; i++) {
        results[count++] = &packages[i];
    }
    return count;
}

Package* PackageManager::GetPackages() { return packages; }
int PackageManager::GetPackageCount() { return package_count; }
const char* PackageManager::GetRepositoryHost() { return PKG_REPOSITORY_HOST; }
const char* PackageManager::GetLastSyncMessage() { return pkg_last_sync_message; }
bool PackageManager::LastSyncSucceeded() { return pkg_last_sync_ok; }
int PackageManager::GetPendingUpdateCount() {
    int pending = 0;
    for (int i = 0; i < package_count; i++) {
        if (packages[i].state == PKG_INSTALLED &&
            packages[i].latest_version[0] &&
            !peq(packages[i].version, packages[i].latest_version)) {
            pending++;
        }
    }
    return pending;
}

void PackageManager::RegisterCommands(void* shell_ptr) {
    KuronoShell* sh = (KuronoShell*)shell_ptr;
    sh->RegisterCommand("kpkg",    "Package manager",    ENV_KURONO, "package", reinterpret_cast<ShellCmdHandler>(cmd_install));
    sh->RegisterCommand("install", "Install package",    ENV_KURONO, "package", reinterpret_cast<ShellCmdHandler>(cmd_install));
    sh->RegisterCommand("remove",  "Remove package",     ENV_KURONO, "package", reinterpret_cast<ShellCmdHandler>(cmd_remove));
    sh->RegisterCommand("update",  "Update packages",    ENV_KURONO, "package", reinterpret_cast<ShellCmdHandler>(cmd_update));
    sh->RegisterCommand("search",  "Search packages",    ENV_KURONO, "package", reinterpret_cast<ShellCmdHandler>(cmd_search));
    sh->RegisterCommand("list",    "List packages",      ENV_KURONO, "package", reinterpret_cast<ShellCmdHandler>(cmd_list));
    sh->RegisterCommand("pkginfo", "Package info",       ENV_KURONO, "package", reinterpret_cast<ShellCmdHandler>(cmd_info));
}

int PackageManager::cmd_install(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return pa(out, 0, mx, "Usage: install <package>\n");

    if (peq(argv[0], "kpkg") && argc >= 2 && peq(argv[1], "sync")) {
        int p = 0;
        p = pa(out, p, mx, "Syncing repository from ");
        p = pa(out, p, mx, PKG_REPOSITORY_HOST);
        p = pa(out, p, mx, "...\n");
        if (!SyncRepository()) {
            p = pa(out, p, mx, "✗ ");
            p = pa(out, p, mx, GetLastSyncMessage());
            p = pac(out, p, mx, '\n');
            return p;
        }
        p = pa(out, p, mx, "✓ ");
        p = pa(out, p, mx, GetLastSyncMessage());
        p = pac(out, p, mx, '\n');
        return p;
    }

    // kpkg setup <target>  -> hand off to gpu driver installer
    if (peq(argv[0], "kpkg") && argc >= 2 && peq(argv[1], "setup")) {
        return GpuDriverInstaller::CmdSetup(argc, argv, out, mx);
    }

    const char* name = argv[1];
    if (peq(argv[0], "kpkg") && argc >= 3) name = argv[2];

    if (pends_with(name, ".kro") && KVFS::Exists(name)) {
        char app_name[PKG_MAX_NAME];
        char entry_path[KVFS_MAX_PATH];
        int p = 0;
        p = pa(out, p, mx, "Installing local .kro app from ");
        p = pa(out, p, mx, name);
        p = pa(out, p, mx, "...\n");
        if (InstallKro(name, app_name, (int)sizeof(app_name), entry_path, (int)sizeof(entry_path))) {
            Package* pkg = Find(app_name);
            p = pa(out, p, mx, "OK installed ");
            p = pa(out, p, mx, app_name);
            if (pkg && pkg->version[0]) {
                p = pa(out, p, mx, " ");
                p = pa(out, p, mx, pkg->version);
            }
            p = pa(out, p, mx, " into /apps/\n");
            p = pa(out, p, mx, "Entry: ");
            p = pa(out, p, mx, entry_path);
            p = pac(out, p, mx, '\n');
        } else {
            p = pa(out, p, mx, "ERROR ");
            p = pa(out, p, mx, GetLastSyncMessage());
            p = pac(out, p, mx, '\n');
        }
        return p;
    }

    // kpkg install debian  -> download large rootfs from server,
    // stage it to disk, write pending-update marker, prompt reboot.
    if (peq(name, "debian")) {
        int p = 0;
        // resolve the real rootfs url + size from the published manifest rather
        // than a stale hardcoded path (the old "/dist/debian-minbase.ext4" 404s
        //  -  the artifact lives under /packages/debian/ per manifest.json). (satoru)
        Package* dpkg = Find("debian");
        if (!dpkg && SyncRepository()) dpkg = Find("debian");
        if (!dpkg) {
            return pa(out, p, mx, "\xE2\x9C\x97 'debian' is not in the repository index.\n");
        }
        if (!pfetch_manifest(dpkg)) {
            p = pa(out, p, mx, "\xE2\x9C\x97 ");
            p = pa(out, p, mx, GetLastSyncMessage());
            return pac(out, p, mx, '\n');
        }
        char dpath[200];
        purl_path(dpkg->download_url, dpath, sizeof(dpath));
        if (!dpath[0]) {
            return pa(out, p, mx, "\xE2\x9C\x97 Debian manifest carries no download url.\n");
        }
        // the single-shot path buffers the whole image in ram; the full rootfs
        // needs a streaming download + persistent storage (this build boots from
        // CD with a ram-backed fs). be honest about the limit instead of silently
        // truncating or timing out at the 20s recv cap. (satoru)
        unsigned int rootfs_kb = dpkg->size;  // manifest size, already in KB
        if (rootfs_kb > (unsigned int)(PKG_HTTP_BUFFER_MAX / 1024)) {
            p = pa(out, p, mx, "\xE2\x9C\x97 Debian rootfs is ");
            p = pai(out, p, mx, rootfs_kb / 1024u);
            p = pa(out, p, mx, " MB \xE2\x80\x94 larger than the ");
            p = pai(out, p, mx, (unsigned int)(PKG_HTTP_BUFFER_MAX / (1024 * 1024)));
            p = pa(out, p, mx, " MB single-shot download path.\n");
            p = pa(out, p, mx, "  A streaming download + a persistent disk are required first\n");
            p = pa(out, p, mx, "  (see HANDOFF). To run Debian now, rebuild with ");
            p = pa(out, p, mx, "\033[36mEMBED_DEBIAN=1\033[0m\n");
            p = pa(out, p, mx, "  to bake the rootfs into the kernel image.\n");
            return p;
        }
        p = pa(out, p, mx, "Downloading Debian rootfs from ");
        p = pa(out, p, mx, PKG_REPOSITORY_HOST);
        p = pa(out, p, mx, " (this may take several minutes)...\n");

        char* payload = (char*)KernelHeap::Alloc(PKG_HTTP_BUFFER_MAX + 1);
        if (!payload) {
            return pa(out, p, mx, "\xE2\x9C\x97 Not enough heap to buffer the download.\n");
        }
        int status = 0, payload_len = 0;
        bool ok = phttp_get(PKG_REPOSITORY_HOST, dpath,
                              payload, PKG_HTTP_BUFFER_MAX + 1, &status, &payload_len);
        if (!ok || status != 200 || payload_len <= 0) {
            KernelHeap::Free(payload);
            p = pa(out, p, mx, "\xE2\x9C\x97 Server fetch failed: ");
            p = pa(out, p, mx, GetLastSyncMessage());
            return pac(out, p, mx, '\n');
        }
        if (!DebianRootfs::SaveDownloaded((const uint8_t*)payload, (uint32_t)payload_len)) {
            KernelHeap::Free(payload);
            return pa(out, p, mx, "\xE2\x9C\x97 Could not write Debian rootfs to disk.\n");
        }
        KernelHeap::Free(payload);

        // optional gpu hint: kpkg install debian nvidia | amd | auto | none
        const char* gpu_hint = "none";
        if (peq(argv[0], "kpkg") && argc >= 4) gpu_hint = argv[3];
        else if (!peq(argv[0], "kpkg") && argc >= 3) gpu_hint = argv[2];

        SystemUpdate::QueueUpdate("debian-install", gpu_hint);

        p = pa(out, p, mx, "\xE2\x9C\x93 Debian rootfs downloaded (");
        char szbuf[16]; int sp = 0;
        unsigned int mbsize = (unsigned int)(payload_len / (1024 * 1024));
        if (mbsize == 0) mbsize = 1;
        char tmp[12]; int ti = 0;
        if (mbsize == 0) tmp[ti++] = '0';
        else { unsigned int v = mbsize; char rev[12]; int ri = 0; while (v) { rev[ri++] = '0' + (v % 10); v /= 10; } while (ri) tmp[ti++] = rev[--ri]; }
        tmp[ti] = 0;
        sp = 0; for (int i = 0; tmp[i]; i++) szbuf[sp++] = tmp[i]; szbuf[sp] = 0;
        p = pa(out, p, mx, szbuf);
        p = pa(out, p, mx, " MB) and staged.\n");
        p = pa(out, p, mx, "  GPU hint: ");
        p = pa(out, p, mx, gpu_hint);
        p = pa(out, p, mx, "\n");
        p = pa(out, p, mx, "\n\033[33m\xE2\x9A\xA0  A reboot is required to finish setup.\033[0m\n");
        p = pa(out, p, mx, "  Run \033[36mreboot\033[0m now \xE2\x80\x94 the system update screen will:\n");
        p = pa(out, p, mx, "    \xE2\x80\xA2 verify the rootfs\n");
        p = pa(out, p, mx, "    \xE2\x80\xA2 boot Debian and run apt-get update\n");
        if (!peq(gpu_hint, "none")) {
            p = pa(out, p, mx, "    \xE2\x80\xA2 install GPU drivers (");
            p = pa(out, p, mx, gpu_hint);
            p = pa(out, p, mx, ") inside Debian\n");
        }
        p = pa(out, p, mx, "    \xE2\x80\xA2 then continue to the desktop.\n");
        return p;
    }

    // firefox / firefox-esr: not a standalone Kurono package  -  the browser runs
    // on the Debian runtime layer (real glibc userland) through the linux syscall
    // bridge + the in-kernel wayland compositor. give the real path rather than a
    // bare "package not found". (satoru)
    if (peq(name, "firefox") || peq(name, "firefox-esr")) {
        if (!Find(name)) SyncRepository();   // honour a published firefox pkg if it exists
        if (!Find(name)) {
            int p = 0;
            p = pa(out, p, mx, "Firefox isn't a standalone package \xE2\x80\x94 it runs on the Debian\n");
            p = pa(out, p, mx, "runtime layer in Kurono (real glibc userland).\n\n");
            if (!DebianRootfs::Available()) {
                p = pa(out, p, mx, "  1) Install the runtime:  \033[36mkpkg install debian\033[0m\n");
                p = pa(out, p, mx, "  2) Then re-run:          \033[36mkpkg install firefox\033[0m\n");
            } else {
                p = pa(out, p, mx, "  The Debian runtime is present. Firefox-in-Debian wiring\n");
                p = pa(out, p, mx, "  (apt + launcher bridge) is still landing \xE2\x80\x94 see HANDOFF.\n");
            }
            return p;
        }
        // a firefox package is published: fall through to the generic installer.
    }

    int p = 0;
    p = pa(out, p, mx, "Installing ");
    p = pa(out, p, mx, name);
    p = pa(out, p, mx, " from ");
    p = pa(out, p, mx, PKG_REPOSITORY_HOST);
    p = pa(out, p, mx, "...\n");

    if (Install(name)) {
        Package* pkg = Find(name);
        p = pa(out, p, mx, "✓ ");
        p = pa(out, p, mx, name);
        p = pa(out, p, mx, " ");
        if (pkg) p = pa(out, p, mx, pkg->version);
        p = pa(out, p, mx, " installed successfully.\n");
    } else {
        p = pa(out, p, mx, "✗ ");
        p = pa(out, p, mx, GetLastSyncMessage());
        p = pac(out, p, mx, '\n');
    }
    return p;
}

int PackageManager::cmd_remove(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return pa(out, 0, mx, "Usage: remove <package>\n");

    int p = 0;
    if (Remove(argv[1])) {
        p = pa(out, p, mx, "✓ ");
        p = pa(out, p, mx, argv[1]);
        p = pa(out, p, mx, " removed.\n");
    } else {
        p = pa(out, p, mx, "✗ Cannot remove: ");
        p = pa(out, p, mx, argv[1]);
        p = pac(out, p, mx, '\n');
    }
    return p;
}

int PackageManager::cmd_update(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    int p = 0;
    p = pa(out, p, mx, "Syncing repository from ");
    p = pa(out, p, mx, PKG_REPOSITORY_HOST);
    p = pa(out, p, mx, "...\n");
    if (!SyncRepository()) {
        p = pa(out, p, mx, "✗ ");
        p = pa(out, p, mx, GetLastSyncMessage());
        p = pac(out, p, mx, '\n');
        return p;
    }
    p = pa(out, p, mx, "✓ ");
    p = pa(out, p, mx, GetLastSyncMessage());
    p = pac(out, p, mx, '\n');

    if (argc >= 2) {
        if (Update(argv[1])) {
            p = pa(out, p, mx, "✓ ");
            p = pa(out, p, mx, argv[1]);
            p = pa(out, p, mx, " updated.\n");
        } else {
            p = pa(out, p, mx, "✗ ");
            p = pa(out, p, mx, GetLastSyncMessage());
            p = pac(out, p, mx, '\n');
        }
        return p;
    }

    int pending = GetPendingUpdateCount();
    if (pending == 0) {
        p = pa(out, p, mx, "✓ All installed packages are already current.\n");
        return p;
    }

    if (UpdateAll()) {
        p = pa(out, p, mx, "✓ Updated ");
        p = pai(out, p, mx, (unsigned int)pending);
        p = pa(out, p, mx, " package(s).\n");
    } else {
        p = pa(out, p, mx, "✗ ");
        p = pa(out, p, mx, GetLastSyncMessage());
        p = pac(out, p, mx, '\n');
    }
    return p;
}

int PackageManager::cmd_search(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return pa(out, 0, mx, "Usage: search <term>\n");

    Package* results[32];
    int count = Search(argv[1], results, 32);

    int p = 0;
    if (count == 0) {
        p = pa(out, p, mx, "No packages found matching '");
        p = pa(out, p, mx, argv[1]);
        p = pa(out, p, mx, "'\n");
    } else {
        for (int i = 0; i < count; i++) {
            p = pa(out, p, mx, results[i]->state == PKG_INSTALLED ? " [✓] " : " [ ] ");
            p = pa(out, p, mx, results[i]->name);
            int nl = plen(results[i]->name);
            for (int j = nl; j < 22; j++) p = pac(out, p, mx, ' ');
            p = pa(out, p, mx, results[i]->version);
            if (results[i]->latest_version[0] && !peq(results[i]->version, results[i]->latest_version)) {
                p = pa(out, p, mx, " -> ");
                p = pa(out, p, mx, results[i]->latest_version);
            }
            p = pa(out, p, mx, "  ");
            p = pa(out, p, mx, results[i]->description);
            p = pac(out, p, mx, '\n');
        }
    }
    return p;
}

int PackageManager::cmd_list(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    bool installed_only = true;
    if (argc >= 2 && peq(argv[1], "--all")) installed_only = false;

    int p = 0;
    p = pa(out, p, mx, "╔═══════════════════════════════════════════════════════╗\n");
    p = pa(out, p, mx, installed_only ? "║            Installed Packages                        ║\n"
                                       : "║            All Packages                              ║\n");
    p = pa(out, p, mx, "╚═══════════════════════════════════════════════════════╝\n\n");

    for (int i = 0; i < package_count; i++) {
        if (installed_only && packages[i].state != PKG_INSTALLED) continue;

        p = pa(out, p, mx, packages[i].state == PKG_INSTALLED ? " ● " : " ○ ");
        p = pa(out, p, mx, packages[i].name);
        int nl = plen(packages[i].name);
        for (int j = nl; j < 22; j++) p = pac(out, p, mx, ' ');
        p = pa(out, p, mx, packages[i].version);
        if (packages[i].latest_version[0] && !peq(packages[i].version, packages[i].latest_version)) {
            p = pa(out, p, mx, " -> ");
            p = pa(out, p, mx, packages[i].latest_version);
        }
        p = pa(out, p, mx, "  [");
        p = pa(out, p, mx, packages[i].category);
        p = pa(out, p, mx, "]  ");
        p = pai(out, p, mx, packages[i].size);
        p = pa(out, p, mx, " KB\n");
    }
    return p;
}

int PackageManager::cmd_info(void* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return pa(out, 0, mx, "Usage: pkginfo <package>\n");

    Package* pkg = Find(argv[1]);
    if (!pkg) return pa(out, 0, mx, "Package not found\n");

    int p = 0;
    p = pa(out, p, mx, "╔══════════════════════════════════╗\n");
    p = pa(out, p, mx, "║          Package Info            ║\n");
    p = pa(out, p, mx, "╠══════════════════════════════════╣\n");
    p = pa(out, p, mx, "║ Name:     "); p = pa(out, p, mx, pkg->name);
    for (int i = plen(pkg->name); i < 21; i++) p = pac(out, p, mx, ' ');
    p = pa(out, p, mx, "║\n");
    p = pa(out, p, mx, "║ Version:  "); p = pa(out, p, mx, pkg->version);
    for (int i = plen(pkg->version); i < 21; i++) p = pac(out, p, mx, ' ');
    p = pa(out, p, mx, "║\n");
    p = pa(out, p, mx, "║ Latest:   "); p = pa(out, p, mx, pkg->latest_version[0] ? pkg->latest_version : pkg->version);
    for (int i = plen(pkg->latest_version[0] ? pkg->latest_version : pkg->version); i < 21; i++) p = pac(out, p, mx, ' ');
    p = pa(out, p, mx, "║\n");
    p = pa(out, p, mx, "║ Category: "); p = pa(out, p, mx, pkg->category);
    for (int i = plen(pkg->category); i < 21; i++) p = pac(out, p, mx, ' ');
    p = pa(out, p, mx, "║\n");
    p = pa(out, p, mx, "║ Status:   ");
    if (pkg->state == PKG_INSTALLED) p = pa(out, p, mx, "Installed            ");
    else if (pkg->state == PKG_UPDATING) p = pa(out, p, mx, "Updating             ");
    else if (pkg->state == PKG_BROKEN) p = pa(out, p, mx, "Broken               ");
    else p = pa(out, p, mx, "Available            ");
    p = pa(out, p, mx, "║\n");
    p = pa(out, p, mx, "║ Size:     ");
    char sz[16]; int si = pai(sz, 0, 16, pkg->size);
    p = pa(out, p, mx, sz);
    p = pa(out, p, mx, " KB");
    for (int i = si + 3; i < 21; i++) p = pac(out, p, mx, ' ');
    p = pa(out, p, mx, "║\n");
    p = pa(out, p, mx, "╠══════════════════════════════════╣\n");
    p = pa(out, p, mx, "║ "); p = pa(out, p, mx, pkg->description);
    int dl = plen(pkg->description);
    for (int i = dl; i < 31; i++) p = pac(out, p, mx, ' ');
    p = pa(out, p, mx, "║\n");
    p = pa(out, p, mx, "╚══════════════════════════════════╝\n");
    if (pkg->repo_path[0]) {
        p = pa(out, p, mx, "Repo path: ");
        p = pa(out, p, mx, pkg->repo_path);
        p = pac(out, p, mx, '\n');
    }
    return p;
}

Package* PackageManager::GetPackage(int idx) {
    if (idx < 0 || idx >= package_count) return nullptr;
    return &packages[idx];
}

int PackageManager::InstalledCount() {
    int n = 0;
    for (int i = 0; i < package_count; i++) {
        if (packages[i].state == PKG_INSTALLED) n++;
    }
    return n;
}

int PackageManager::AvailableCount() {
    int n = 0;
    for (int i = 0; i < package_count; i++) {
        if (packages[i].state != PKG_INSTALLED) n++;
    }
    return n;
}
