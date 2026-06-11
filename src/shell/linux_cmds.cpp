#include "linux_cmds.h"
#include "../fs/kvfs.h"
#include "../kernel/time.h"
#include "../drivers/timer.h"
#include "../kernel/heap.h"
#include "../proc/scheduler.h"
#include "../drivers/serial.h"
#include "../drivers/audio.h"
#include "../drivers/cpu_detect.h"
#include "../drivers/e1000.h"
#include "../drivers/gpu_probe.h"
#include "../drivers/nvidia_gpu.h"
#include "../drivers/intel_gpu.h"
#include "../drivers/amd_gpu.h"
#include "../drivers/bga.h"
#include "../drivers/graphics.h"
#include "../drivers/nvme.h"
#include "../drivers/usb.h"
#include "../drivers/hda.h"
#include "../drivers/virtio_gpu.h"
#include "../drivers/display_mgr.h"
#include "../net/network.h"
#include "../net/tcpip.h"
#include "../system/logging.h"
#include "../linux/linux_syscall.h"
#include "../linux/linux_drivers.h"
#include "../virt/hypervisor.h"
#include "../virt/vmm.h"
#include "../virt/alpine_data.h"
#include "../virt/debian_data.h"

//  linux bridge  -  posix-like commands for kurono shell

static int _slen(const char* s) { int n=0; while (s[n]) n++; return n; }
static void _scpy(char* d, const char* s, int m) {
    int i=0; while (s[i] && i<m-1) { d[i]=s[i]; i++; } d[i]=0;
}
static bool _seq(const char* a, const char* b) {
    while (*a && *b) { if (*a!=*b) return false; a++; b++; } return *a==*b;
}
static int _sa(char* b, int p, int m, const char* s) {
    while (*s && p<m-1) b[p++]=*s++;  b[p]=0; return p;
}
static int _sac(char* b, int p, int m, char c) { if (p<m-1) {b[p++]=c; b[p]=0;} return p; }
static int _sai(char* b, int p, int m, int v) {
    if (v<0) { p=_sac(b,p,m,'-'); v=-v; }
    if (v==0) return _sac(b,p,m,'0');
    char t[12]; int ti=0;
    while (v>0) { t[ti++]='0'+(v%10); v/=10; }
    while (ti>0) p=_sac(b,p,m,t[--ti]);
    return p;
}
static int _sau(char* b, int p, int m, unsigned int v) {
    if (v==0) return _sac(b,p,m,'0');
    char t[12]; int ti=0;
    while (v>0) { t[ti++]='0'+(v%10); v/=10; }
    while (ti>0) p=_sac(b,p,m,t[--ti]);
    return p;
}

static void _log_runtime_event(const char* component, const char* action, const char* detail) {
    char line[256];
    int p = 0;
    line[0] = 0;
    p = _sa(line, p, sizeof(line), action ? action : "event");
    if (detail && *detail) {
        p = _sa(line, p, sizeof(line), ": ");
        p = _sa(line, p, sizeof(line), detail);
    }
    RuntimeLog::LogSystem(component ? component : "system", line);
}

static int _atoi(const char* s) {
    int v=0; bool neg=false;
    if (*s=='-') { neg=true; s++; }
    while (*s>='0' && *s<='9') { v=v*10+(*s-'0'); s++; }
    return neg ? -v : v;
}

static int _sah(char* b, int p, int m, unsigned int v, int digits=4) {
    static const char hex[] = "0123456789abcdef";
    char t[9]; int ti = 0;
    if (v == 0) { for (int i = 0; i < digits; i++) t[ti++] = '0'; }
    else { while (v > 0) { t[ti++] = hex[v & 0xF]; v >>= 4; } }
    while (ti < digits) t[ti++] = '0';
    while (ti > 0) p = _sac(b, p, m, t[--ti]);
    return p;
}

static int _sa64(char* b, int p, int m, uint64_t v) {
    if (v == 0) return _sac(b, p, m, '0');
    char t[21]; int ti = 0;
    while (v > 0) { t[ti++] = '0' + (int)(v % 10); v /= 10; }
    while (ti > 0) p = _sac(b, p, m, t[--ti]);
    return p;
}
static bool _starts_with(const char* s, const char* prefix) {
    if (!s || !prefix) return false;
    int i = 0;
    while (prefix[i]) { if (s[i] != prefix[i]) return false; i++; }
    return true;
}
static LinuxDriver* _find_bound_driver_by_prefixes(const char* const* prefixes, int count) {
    LinuxDriver* drivers = LinuxDriverFramework::GetDrivers();
    int driver_count = LinuxDriverFramework::GetDriverCount();
    for (int i = 0; i < driver_count; i++) {
        if (!(drivers[i].bound || drivers[i].state == LDRV_ACTIVE)) continue;
        for (int p = 0; p < count; p++) {
            if (_starts_with(drivers[i].name, prefixes[p])) return &drivers[i];
        }
    }
    return nullptr;
}
static LinuxDriver* _find_wifi_driver() {
    static const char* prefixes[] = {
        "wifi_", "iwl", "ath", "rtw", "rtl", "brcm", "mt76", "cfg80211", "mac80211"
    };
    return _find_bound_driver_by_prefixes(prefixes, (int)(sizeof(prefixes) / sizeof(prefixes[0])));
}
static LinuxDriver* _find_bt_driver() {
    static const char* prefixes[] = {
        "bluetooth_", "bluetooth", "bt", "hci"
    };
    return _find_bound_driver_by_prefixes(prefixes, (int)(sizeof(prefixes) / sizeof(prefixes[0])));
}
static bool _guest_tools_enabled() {
    return Hypervisor::IsLinuxGuestEnabled();
}
static bool _guest_is_alpine() {
    return Hypervisor::GetLinuxGuestProfile() == LINUX_GUEST_ALPINE;
}
static bool _guest_is_debian() {
    return Hypervisor::GetLinuxGuestProfile() == LINUX_GUEST_DEBIAN;
}

static const int LINUX_HTTP_BUFFER_MAX = 512 * 1024;

static int _http_find_header_end(const char* data, int len) {
    for (int i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n') {
            return i + 4;
        }
    }
    return -1;
}

static int _http_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool _http_contains_ci(const char* haystack, const char* needle) {
    int hl = _slen(haystack);
    int nl = _slen(needle);
    if (nl == 0 || hl < nl) return false;
    for (int i = 0; i <= hl - nl; i++) {
        bool match = true;
        for (int j = 0; j < nl; j++) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

static int _http_decode_chunked(const char* src, int src_len, char* dst, int dst_max) {
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
            int hv = _http_hex(src[sp]);
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
        for (int i = 0; i < copy; i++) dst[dp + i] = src[sp + i];
        dp += copy;
        sp += chunk_size;

        if (sp + 1 < src_len && src[sp] == '\r' && src[sp + 1] == '\n') sp += 2;
    }
    return dp;
}

static bool _parse_http_url(const char* url, char* host, int host_max,
                            char* path, int path_max, uint16_t* port,
                            bool* https) {
    if (!url || !host || host_max < 2 || !path || path_max < 2 || !port || !https) return false;

    host[0] = 0;
    path[0] = '/';
    path[1] = 0;
    *port = 80;
    *https = false;

    const char* cursor = url;
    if (_starts_with(cursor, "http://")) {
        cursor += 7;
    } else if (_starts_with(cursor, "https://")) {
        cursor += 8;
        *https = true;
        *port = 443;
    }

    int hp = 0;
    while (*cursor && *cursor != '/' && *cursor != ':' && hp < host_max - 1) {
        host[hp++] = *cursor++;
    }
    host[hp] = 0;
    if (hp == 0) return false;

    if (*cursor == ':') {
        cursor++;
        int parsed_port = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            parsed_port = parsed_port * 10 + (*cursor - '0');
            cursor++;
        }
        if (parsed_port > 0 && parsed_port <= 65535) *port = (uint16_t)parsed_port;
    }

    if (*cursor == 0) return true;

    int pp = 0;
    while (*cursor && pp < path_max - 1) path[pp++] = *cursor++;
    path[pp] = 0;
    return path[0] == '/';
}

static void _filename_from_url_path(const char* path, char* out, int out_max) {
    if (!out || out_max < 2) return;

    const char* leaf = path;
    for (const char* p = path; p && *p; p++) {
        if (*p == '/') leaf = p + 1;
    }
    if (!leaf || !*leaf) {
        _scpy(out, "index.html", out_max);
        return;
    }

    int i = 0;
    while (leaf[i] && leaf[i] != '?' && leaf[i] != '#' && i < out_max - 1) {
        out[i] = leaf[i];
        i++;
    }
    out[i] = 0;
    if (i == 0) _scpy(out, "index.html", out_max);
}

static bool _http_get_plain(const char* url, char* body_out, int body_max,
                            int* status_out, int* body_len_out,
                            char* err_out, int err_max) {
    if (err_out && err_max > 0) err_out[0] = 0;
    if (body_out && body_max > 0) body_out[0] = 0;
    if (!url || !body_out || body_max < 2) {
        if (err_out) _scpy(err_out, "invalid HTTP request buffer", err_max);
        return false;
    }
    if (!TCPStack::IsUp()) {
        if (err_out) _scpy(err_out, "TCP/IP stack is unavailable on this boot", err_max);
        return false;
    }

    char host[128];
    char path[256];
    uint16_t port = 80;
    bool https = false;
    if (!_parse_http_url(url, host, sizeof(host), path, sizeof(path), &port, &https)) {
        if (err_out) _scpy(err_out, "unsupported URL format", err_max);
        return false;
    }
    if (https) {
        if (err_out) _scpy(err_out, "HTTPS is not supported yet; use plain http:// URLs", err_max);
        return false;
    }

    RuntimeLog::LogAppEvent("terminal", "http-get", url);

    IPv4Address ip_addr;
    if (!Network::Resolve(host, &ip_addr)) {
        _log_runtime_event("http", "resolve failed", host);
        if (err_out) _scpy(err_out, "hostname resolution failed", err_max);
        return false;
    }
    uint32_t ip = TCPStack::MakeIP(ip_addr.bytes[0], ip_addr.bytes[1], ip_addr.bytes[2], ip_addr.bytes[3]);

    int sock = TCPStack::Socket(SOCK_STREAM);
    if (sock < 0) {
        _log_runtime_event("http", "socket unavailable", host);
        if (err_out) _scpy(err_out, "no free TCP sockets are available", err_max);
        return false;
    }

    char* response = (char*)KernelHeap::Alloc(LINUX_HTTP_BUFFER_MAX + 1);
    if (!response) {
        TCPStack::Close(sock);
        if (err_out) _scpy(err_out, "not enough heap for HTTP response buffer", err_max);
        return false;
    }

    bool ok = false;
    do {
        if (!TCPStack::Connect(sock, ip, port)) {
            _log_runtime_event("http", "connect failed", host);
            if (err_out) _scpy(err_out, "TCP connect failed", err_max);
            break;
        }

        char request[512];
        int rp = 0;
        rp = _sa(request, rp, sizeof(request), "GET ");
        rp = _sa(request, rp, sizeof(request), path);
        rp = _sa(request, rp, sizeof(request), " HTTP/1.1\r\nHost: ");
        rp = _sa(request, rp, sizeof(request), host);
        rp = _sa(request, rp, sizeof(request), "\r\nUser-Agent: KuronoShell/1.0\r\nConnection: close\r\nAccept: */*\r\n\r\n");

        if (TCPStack::Send(sock, request, rp) != rp) {
            _log_runtime_event("http", "request send failed", host);
            if (err_out) _scpy(err_out, "HTTP request send failed", err_max);
            break;
        }

        int total = 0;
        // wait on wall-clock time, not a raw iteration count. a real-internet
        // round trip is tens of ms, but the old tight 40000-iteration spin
        // burned out in a few ms and gave up (then closed the socket) before the
        // response even arrived  -  fine under sub-ms slirp/localhost, broken
        // against a real server. keep receiving until the peer closes or no new
        // data arrives for ~10s, and pace each idle turn so the cooperative
        // scheduler still runs. (satoru)
        uint32_t last_rx_ms = Timer::GetTicks();
        const uint32_t http_idle_timeout_ms = 10000u;
        while (total < LINUX_HTTP_BUFFER_MAX) {
            if (KuronoShell::IsCommandCancelRequested()) break;
            TCPStack::Tick();
            int got = TCPStack::Recv(sock, response + total, LINUX_HTTP_BUFFER_MAX - total);
            if (got < 0) {
                _log_runtime_event("http", "response receive failed", host);
                if (err_out) _scpy(err_out, "HTTP response receive failed", err_max);
                total = -1;
                break;
            }
            if (got == 0) {
                if (TCPStack::IsPeerClosed(sock)) {
                    break;
                }
                if ((uint32_t)(Timer::GetTicks() - last_rx_ms) >= http_idle_timeout_ms) {
                    break;
                }
                KuronoShell::PumpUI();
                Scheduler::SleepMs(1);
                continue;
            }
            total += got;
            last_rx_ms = Timer::GetTicks();
        }

        if (total <= 0) {
            _log_runtime_event("http", "no response data", host);
            if (err_out && err_out[0] == 0) _scpy(err_out, "remote host returned no data", err_max);
            break;
        }

        response[total] = 0;
        int header_end = _http_find_header_end(response, total);
        if (header_end < 0) {
            _log_runtime_event("http", "invalid response headers", host);
            if (err_out) _scpy(err_out, "HTTP headers terminator not found", err_max);
            break;
        }

        int status = 0;
        const char* status_ptr = response;
        while (*status_ptr && *status_ptr != ' ') status_ptr++;
        if (*status_ptr == ' ') status = _atoi(status_ptr + 1);
        if (status_out) *status_out = status;

        char status_line[128];
        int sp = 0;
        status_line[0] = 0;
        sp = _sa(status_line, sp, sizeof(status_line), host);
        sp = _sa(status_line, sp, sizeof(status_line), " status ");
        sp = _sai(status_line, sp, sizeof(status_line), status);
        _log_runtime_event("http", "response", status_line);

        int body_len = total - header_end;
        if (_http_contains_ci(response, "Transfer-Encoding: chunked")) {
            body_len = _http_decode_chunked(response + header_end, total - header_end, body_out, body_max);
            if (body_len < 0) {
                _log_runtime_event("http", "chunk decode failed", host);
                if (err_out) _scpy(err_out, "chunked HTTP body decode failed", err_max);
                break;
            }
        } else {
            if (body_len > body_max - 1) body_len = body_max - 1;
            for (int i = 0; i < body_len; i++) body_out[i] = response[header_end + i];
        }

        body_out[body_len] = 0;
        if (body_len_out) *body_len_out = body_len;
        ok = true;
    } while (false);

    TCPStack::Close(sock);
    KernelHeap::Free(response);
    return ok;
}

static int _append_vm_state_name(char* out, int p, int mx, VMState st) {
    switch (st) {
        case VM_STATE_UNINITIALIZED: return _sa(out, p, mx, "UNINITIALIZED");
        case VM_STATE_CREATED:       return _sa(out, p, mx, "CREATED");
        case VM_STATE_RUNNING:       return _sa(out, p, mx, "RUNNING");
        case VM_STATE_PAUSED:        return _sa(out, p, mx, "PAUSED");
        case VM_STATE_HALTED:        return _sa(out, p, mx, "HALTED");
        case VM_STATE_CRASHED:       return _sa(out, p, mx, "CRASHED");
        case VM_STATE_REBOOTING:     return _sa(out, p, mx, "REBOOTING");
        case VM_STATE_DESTROYED:     return _sa(out, p, mx, "DESTROYED");
        default:                     return _sa(out, p, mx, "UNKNOWN");
    }
}

static int _append_alpine_boot_diagnostics(char* out, int p, int mx) {
    const VMStats& st = Hypervisor::GetStats();
    int loglen = Hypervisor::GetAlpineBootLogLen();
    VMState vmst = Hypervisor::GetState();

    p = _sa(out, p, mx, "Diagnostics:\n");
    p = _sa(out, p, mx, "  HW virtualization: ");
    p = _sa(out, p, mx, Hypervisor::IsAvailable() ? "available" : "unavailable");
    p = _sac(out, p, mx, '\n');

    p = _sa(out, p, mx, "  Backend: ");
    if (VMM::IsWHPX()) p = _sa(out, p, mx, "WHPX");
    else if (VMM::GetType() == VIRT_INTEL_VTX) p = _sa(out, p, mx, "Intel VT-x");
    else if (VMM::GetType() == VIRT_AMD_SVM) p = _sa(out, p, mx, "AMD-V");
    else p = _sa(out, p, mx, "none");
    if (VMM::IsNested()) p = _sa(out, p, mx, " (nested)");
    p = _sac(out, p, mx, '\n');

    if (VMM::IsWHPX()) {
        p = _sa(out, p, mx, "  WHPX nested virtualization: ");
        p = _sa(out, p, mx, VMM::IsWHPXNestedOk() ? "supported" : "blocked by host");
        p = _sac(out, p, mx, '\n');
    }

    p = _sa(out, p, mx, "  VM state: ");
    p = _append_vm_state_name(out, p, mx, vmst);
    p = _sac(out, p, mx, '\n');

    p = _sa(out, p, mx, "  Boot log bytes: ");
    p = _sai(out, p, mx, loglen);
    p = _sac(out, p, mx, '\n');

    p = _sa(out, p, mx, "  VM exits: ");
    p = _sau(out, p, mx, st.total_exits);
    p = _sa(out, p, mx, ", serial tx: ");
    p = _sau(out, p, mx, st.serial_bytes_tx);
    p = _sa(out, p, mx, ", serial rx: ");
    p = _sau(out, p, mx, st.serial_bytes_rx);
    p = _sac(out, p, mx, '\n');

    p = _sa(out, p, mx, "  Alpine payloads: kernel ");
    p = _sau(out, p, mx, (unsigned int)(alpine_kernel_size() / 1024));
    p = _sa(out, p, mx, " KB, initramfs ");
    p = _sau(out, p, mx, (unsigned int)(alpine_initramfs_size() / 1024));
    p = _sa(out, p, mx, " KB\n");

    if (!Hypervisor::IsAvailable()) {
        p = _sa(out, p, mx, "  Cause: hardware virtualization is unavailable to the guest hypervisor.\n");
        if (VMM::IsWHPX() && !VMM::IsWHPXNestedOk()) {
            p = _sa(out, p, mx, "  Cause detail: WHPX/Hyper-V is active but nested VT-x/AMD-V is not exposed to Kurono.\n");
            p = _sa(out, p, mx, "  Fix: run under KVM on Linux, disable Hyper-V/WHPX for this test, or boot on bare metal.\n");
        } else {
            p = _sa(out, p, mx, "  Fix: enable Intel VT-x or AMD-V in firmware, then boot Kurono outside a host that masks nested virtualization.\n");
        }
    } else if (vmst == VM_STATE_CRASHED && loglen == 0) {
        p = _sa(out, p, mx, "  Cause: the VM failed before Alpine produced serial output.\n");
        if (VMM::IsWHPX()) {
            p = _sa(out, p, mx, "  Likely reason: WHPX allowed guest launch but blocked nested VM entry.\n");
        }
    } else if ((vmst == VM_STATE_RUNNING || vmst == VM_STATE_PAUSED) && loglen == 0) {
        p = _sa(out, p, mx, "  Cause: Alpine stayed alive but did not reach ttyS0 output inside the exit budget.\n");
        p = _sa(out, p, mx, "  Try again with a larger exit budget, for example: vm boot-alpine 500000\n");
    } else if (loglen > 0) {
        p = _sa(out, p, mx, "  Serial reached Kurono. Review the boot log above for the last kernel line.\n");
    }

    return p;
}

void LinuxCmds::RegisterAll(KuronoShell* sh) {
    using namespace LinuxCmds;

    // these register as env_linux so the shell conflict system triggers
    // when the same name exists in env_windows (dir/ls, del/rm, etc.)
    sh->RegisterCommand("ls",       "List directory (Linux)",  ENV_LINUX, "filesystem", cmd_ls);
    sh->RegisterCommand("mkdir",    "Create directory",        ENV_LINUX, "filesystem", cmd_mkdir);
    sh->RegisterCommand("rmdir",    "Remove directory",        ENV_LINUX, "filesystem", cmd_rmdir);
    sh->RegisterCommand("rm",       "Remove file",             ENV_LINUX, "filesystem", cmd_rm);
    sh->RegisterCommand("cp",       "Copy file",               ENV_LINUX, "filesystem", cmd_cp);
    sh->RegisterCommand("mv",       "Move/rename file",        ENV_LINUX, "filesystem", cmd_mv);
    sh->RegisterCommand("cat",      "Display file contents",   ENV_LINUX, "filesystem", cmd_cat);
    sh->RegisterCommand("grep",     "Search text in files",    ENV_LINUX, "text",       cmd_grep);
    sh->RegisterCommand("ps",       "List processes",          ENV_LINUX, "system",     cmd_ps);
    sh->RegisterCommand("kill",     "Kill process",            ENV_LINUX, "system",     cmd_kill);
    sh->RegisterCommand("ifconfig", "Network interfaces",      ENV_LINUX, "network",    cmd_ifconfig);
    sh->RegisterCommand("hostname", "Show hostname",           ENV_LINUX, "system",     cmd_hostname);
    sh->RegisterCommand("pwd",      "Print working directory", ENV_LINUX, "filesystem", cmd_pwd);
    sh->RegisterCommand("uname",    "System name",             ENV_LINUX, "system",     cmd_uname);
    sh->RegisterCommand("uptime",   "System uptime",           ENV_LINUX, "system",     cmd_uptime);
    sh->RegisterCommand("whoami",   "Current user",            ENV_LINUX, "system",     cmd_whoami);
    sh->RegisterCommand("date",     "Show date/time",          ENV_LINUX, "system",     cmd_date);

    sh->RegisterCommand("cd",       "Change directory",        ENV_AUTO, "filesystem", cmd_cd);
    sh->RegisterCommand("touch",    "Create empty file",       ENV_AUTO, "filesystem", cmd_touch);
    sh->RegisterCommand("head",     "Show first N lines",      ENV_AUTO, "text",       cmd_head);
    sh->RegisterCommand("tail",     "Show last N lines",       ENV_AUTO, "text",       cmd_tail);
    sh->RegisterCommand("wc",       "Word/line/char count",    ENV_AUTO, "text",       cmd_wc);
    sh->RegisterCommand("chmod",    "Change permissions",      ENV_AUTO, "filesystem", cmd_chmod);
    sh->RegisterCommand("stat",     "File status",             ENV_AUTO, "filesystem", cmd_stat);
    sh->RegisterCommand("df",       "Disk free space",         ENV_AUTO, "filesystem", cmd_df);
    sh->RegisterCommand("du",       "Disk usage",              ENV_AUTO, "filesystem", cmd_du);
    sh->RegisterCommand("ln",       "Create link",             ENV_AUTO, "filesystem", cmd_ln);
    sh->RegisterCommand("find",     "Search files",            ENV_AUTO, "filesystem", cmd_find);
    sh->RegisterCommand("which",    "Locate a command",        ENV_AUTO, "system",     cmd_which);
    sh->RegisterCommand("tee",      "Tee output to file",      ENV_AUTO, "text",       cmd_tee);
    sh->RegisterCommand("sort",     "Sort lines",              ENV_AUTO, "text",       cmd_sort);
    sh->RegisterCommand("uniq",     "Unique lines",            ENV_AUTO, "text",       cmd_uniq);
    sh->RegisterCommand("tr",       "Translate chars",         ENV_AUTO, "text",       cmd_tr);
    sh->RegisterCommand("free",     "Memory usage",            ENV_AUTO, "system",     cmd_free);
    sh->RegisterCommand("mount",    "Show mounts",             ENV_AUTO, "system",     cmd_mount);
    sh->RegisterCommand("dmesg",    "Serial log tail (/kurono/logs/serial.log)", ENV_AUTO, "system", cmd_dmesg);
    sh->RegisterCommand("journal",  "KVFS log tail viewer",                     ENV_AUTO, "system", cmd_journal);
    sh->RegisterCommand("lspci",    "List PCI devices",        ENV_AUTO, "system",     cmd_lspci);
    sh->RegisterCommand("lsmod",    "List loaded drivers",     ENV_AUTO, "system",     cmd_lsmod);
    sh->RegisterCommand("drivers",  "Driver status summary",   ENV_AUTO, "system",     cmd_drivers);
    sh->RegisterCommand("lsblk",    "List block devices",      ENV_AUTO, "system",     cmd_lsblk);
    sh->RegisterCommand("lsusb",    "List USB devices",        ENV_AUTO, "system",     cmd_lsusb);
    sh->RegisterCommand("lscpu",    "CPU architecture info",   ENV_AUTO, "system",     cmd_lscpu);
    sh->RegisterCommand("modprobe", "Load kernel module",      ENV_AUTO, "system",     cmd_modprobe);
    sh->RegisterCommand("modinfo",  "Module information",      ENV_AUTO, "system",     cmd_modinfo);
    sh->RegisterCommand("insmod",   "Insert kernel module",    ENV_AUTO, "system",     cmd_insmod);
    sh->RegisterCommand("rmmod",    "Remove kernel module",    ENV_AUTO, "system",     cmd_rmmod);
    sh->RegisterCommand("dmidecode","DMI/SMBIOS info",         ENV_AUTO, "system",     cmd_dmidecode);
    sh->RegisterCommand("hwinfo",   "Hardware information",    ENV_AUTO, "system",     cmd_hwinfo);
    sh->RegisterCommand("top",      "Process/CPU monitor",     ENV_AUTO, "system",     cmd_top);
    sh->RegisterCommand("ip",       "IP routing/address",      ENV_AUTO, "network",    cmd_ip);
    sh->RegisterCommand("ss",       "Socket statistics",       ENV_AUTO, "network",    cmd_ss);
    sh->RegisterCommand("iotop",    "I/O usage monitor",       ENV_AUTO, "system",     cmd_iotop);
    sh->RegisterCommand("ping",     "Ping host",               ENV_AUTO, "network",    cmd_ping);
    sh->RegisterCommand("wget",     "Download URL",            ENV_AUTO, "network",    cmd_wget);
    sh->RegisterCommand("curl",     "Transfer data",           ENV_AUTO, "network",    cmd_curl);

    // linux syscall execution  -  runs real linux programs via syscall layer
    sh->RegisterCommand("linux-exec", "Run program via Linux syscalls", ENV_AUTO, "linux", cmd_linux_exec);
    sh->RegisterCommand("syscall",    "Direct Linux syscall test",      ENV_AUTO, "linux", cmd_syscall);

    // hypervisor / vm management  -  create, boot, run linux guest vms
    sh->RegisterCommand("vm",         "Manage virtual machines",        ENV_AUTO, "virt",  cmd_vm);

    // alpine linux vm management
    sh->RegisterCommand("alpine",     "Alpine Linux guest VM",          ENV_AUTO, "system", cmd_alpine);
    sh->RegisterCommand("apk",        "Alpine package manager",         ENV_AUTO, "package", cmd_apk);
    sh->RegisterCommand("debian",     "Debian Linux guest VM",          ENV_AUTO, "system", cmd_debian);
    sh->RegisterCommand("apt",        "Debian package manager",         ENV_AUTO, "package", cmd_apt);
}

//  filesystem commands

int LinuxCmds::cmd_ls(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    const char* path = (argc > 1) ? argv[1] : ".";
    bool long_fmt = false;
    bool show_all = false;

    // parse flags
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'l') long_fmt = true;
                if (argv[i][j] == 'a') show_all = true;
            }
        } else {
            path = argv[i];
        }
    }

    KVFSNode* dir = KVFS::ResolvePath(path);
    if (!dir || dir->type != KVFS_DIR) return _sa(out, 0, mx, "ls: cannot access: No such directory\n");

    int p = 0;
    if (long_fmt) {
        p = _sa(out, p, mx, "total ");
        p = _sai(out, p, mx, dir->child_count);
        p = _sac(out, p, mx, '\n');
    }

    for (int i = 0; i < dir->child_count; i++) {
        KVFSNode* c = dir->children[i];
        if (!c) continue;
        if (!show_all && c->name[0] == '.') continue;

        if (long_fmt) {
            // type
            p = _sac(out, p, mx, c->type == KVFS_DIR ? 'd' : (c->type == KVFS_SYMLINK ? 'l' : '-'));
            // permissions
            unsigned short m = c->perms.mode;
            p = _sac(out, p, mx, (m & 0400) ? 'r' : '-');
            p = _sac(out, p, mx, (m & 0200) ? 'w' : '-');
            p = _sac(out, p, mx, (m & 0100) ? 'x' : '-');
            p = _sac(out, p, mx, (m & 040) ? 'r' : '-');
            p = _sac(out, p, mx, (m & 020) ? 'w' : '-');
            p = _sac(out, p, mx, (m & 010) ? 'x' : '-');
            p = _sac(out, p, mx, (m & 04) ? 'r' : '-');
            p = _sac(out, p, mx, (m & 02) ? 'w' : '-');
            p = _sac(out, p, mx, (m & 01) ? 'x' : '-');
            // size
            p = _sa(out, p, mx, "  ");
            p = _sau(out, p, mx, c->size);
            // pad
            int sl = 1; unsigned int tmp = c->size;
            while (tmp >= 10) { sl++; tmp /= 10; }
            for (int j = sl; j < 8; j++) p = _sac(out, p, mx, ' ');
            p = _sac(out, p, mx, ' ');
        }

        // name with color indicator
        if (c->type == KVFS_DIR) {
            p = _sa(out, p, mx, c->name);
            p = _sac(out, p, mx, '/');
        } else if (c->type == KVFS_SYMLINK) {
            p = _sa(out, p, mx, c->name);
            p = _sa(out, p, mx, " -> ");
            if (c->content) p = _sa(out, p, mx, (const char*)c->content);
        } else {
            p = _sa(out, p, mx, c->name);
        }
        p = _sac(out, p, mx, long_fmt ? '\n' : '\t');
    }
    if (!long_fmt) p = _sac(out, p, mx, '\n');
    return p;
}

int LinuxCmds::cmd_cd(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    const char* path = (argc > 1) ? argv[1] : nullptr;
    if (!path) {
        const char* home = sh->GetVar("HOME");
        path = home ? home : "/";
    }
    if (_seq(path, "-")) {
        const char* old = sh->GetVar("OLDPWD");
        if (old) path = old;
        else return _sa(out, 0, mx, "cd: OLDPWD not set\n");
    }

    // save old
    char old_cwd[256];
    _scpy(old_cwd, KVFS::GetCwd(), 256);

    KVFS::SetCwd(path);
    if (!KVFS::Resolve(path)) return _sa(out, 0, mx, "cd: No such directory\n");

    sh->SetVar("OLDPWD", old_cwd);
    sh->SetVar("PWD", KVFS::GetCwd());
    return 0;
}

int LinuxCmds::cmd_pwd(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = _sa(out, 0, mx, KVFS::GetCwd());
    return _sac(out, p, mx, '\n');
}

int LinuxCmds::cmd_mkdir(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: mkdir [-p] <dir>\n");
    bool parents = false;
    for (int i = 1; i < argc; i++) {
        if (_seq(argv[i], "-p")) { parents = true; continue; }
        int err = parents ? KVFS::Mkdirs(argv[i]) : KVFS::Mkdir(argv[i]);
        if (err != KVFS_OK) {
            int p = _sa(out, 0, mx, "mkdir: cannot create '");
            p = _sa(out, p, mx, argv[i]);
            return _sa(out, p, mx, "'\n");
        }
    }
    return 0;
}

int LinuxCmds::cmd_rmdir(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: rmdir <dir>\n");
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        int err = KVFS::Rmdir(argv[i]);
        if (err != KVFS_OK) {
            int p = _sa(out, 0, mx, "rmdir: failed '");
            p = _sa(out, p, mx, argv[i]);
            return _sa(out, p, mx, "'\n");
        }
    }
    return 0;
}

int LinuxCmds::cmd_rm(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: rm [-rf] <file...>\n");
    bool recursive = false;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'r') recursive = true;
            }
            continue;
        }
        KVFSNode* n = KVFS::ResolvePath(argv[i]);
        if (!n) {
            int p = _sa(out, 0, mx, "rm: cannot remove '");
            p = _sa(out, p, mx, argv[i]);
            return _sa(out, p, mx, "': No such file\n");
        }
        if (n->type == KVFS_DIR) {
            if (!recursive) {
                int p = _sa(out, 0, mx, "rm: '");
                p = _sa(out, p, mx, argv[i]);
                return _sa(out, p, mx, "' is a directory (use -r)\n");
            }
            KVFS::Rmdir(argv[i]);
        } else {
            KVFS::Unlink(argv[i]);
        }
    }
    return 0;
}

int LinuxCmds::cmd_cp(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 3) return _sa(out, 0, mx, "Usage: cp <src> <dst>\n");
    int err = KVFS::Copy(argv[1], argv[2]);
    if (err != KVFS_OK) return _sa(out, 0, mx, "cp: copy failed\n");
    return 0;
}

int LinuxCmds::cmd_mv(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 3) return _sa(out, 0, mx, "Usage: mv <src> <dst>\n");
    int err = KVFS::Move(argv[1], argv[2]);
    if (err != KVFS_OK) return _sa(out, 0, mx, "mv: move failed\n");
    return 0;
}

int LinuxCmds::cmd_touch(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: touch <file...>\n");
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        KVFSNode* n = KVFS::ResolvePath(argv[i]);
        if (!n) KVFS::CreateFile(argv[i]);
    }
    return 0;
}

int LinuxCmds::cmd_cat(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: cat <file...>\n");
    int p = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        unsigned char buf[KVFS_MAX_CONTENT];
        int bytes_read = KVFS::ReadFile(argv[i], buf, KVFS_MAX_CONTENT);
        if (bytes_read < 0) {
            p = _sa(out, p, mx, "cat: ");
            p = _sa(out, p, mx, argv[i]);
            p = _sa(out, p, mx, ": No such file\n");
            continue;
        }
        for (int j = 0; j < bytes_read && p < mx - 1; j++)
            out[p++] = (char)buf[j];
        out[p] = 0;
    }
    return p;
}

int LinuxCmds::cmd_head(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    int lines = 10;
    const char* file = nullptr;
    for (int i = 1; i < argc; i++) {
        if (_seq(argv[i], "-n") && i + 1 < argc) { lines = _atoi(argv[++i]); }
        else if (argv[i][0] != '-') file = argv[i];
    }
    if (!file) return _sa(out, 0, mx, "Usage: head [-n N] <file>\n");

    unsigned char buf[KVFS_MAX_CONTENT];
    int sz = KVFS::ReadFile(file, buf, KVFS_MAX_CONTENT);
    if (sz < 0)
        return _sa(out, 0, mx, "head: cannot read file\n");

    int p = 0;
    int lc = 0;
    for (unsigned int i = 0; i < (unsigned int)sz && lc < lines && p < mx - 1; i++) {
        out[p++] = (char)buf[i];
        if (buf[i] == '\n') lc++;
    }
    out[p] = 0;
    return p;
}

int LinuxCmds::cmd_tail(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    int lines = 10;
    const char* file = nullptr;
    for (int i = 1; i < argc; i++) {
        if (_seq(argv[i], "-n") && i + 1 < argc) { lines = _atoi(argv[++i]); }
        else if (argv[i][0] != '-') file = argv[i];
    }
    if (!file) return _sa(out, 0, mx, "Usage: tail [-n N] <file>\n");

    unsigned char buf[KVFS_MAX_CONTENT];
    int sz = KVFS::ReadFile(file, buf, KVFS_MAX_CONTENT);
    if (sz < 0)
        return _sa(out, 0, mx, "tail: cannot read file\n");

    // count newlines from end
    int lc = 0;
    int start = (int)sz;
    for (int i = (int)sz - 1; i >= 0; i--) {
        if (buf[i] == '\n') lc++;
        if (lc == lines + 1) { start = i + 1; break; }
    }

    int p = 0;
    for (int i = start; i < (int)sz && p < mx - 1; i++) out[p++] = (char)buf[i];
    out[p] = 0;
    return p;
}

int LinuxCmds::cmd_wc(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: wc <file>\n");

    unsigned char buf[KVFS_MAX_CONTENT];
    int sz = KVFS::ReadFile(argv[1], buf, KVFS_MAX_CONTENT);
    if (sz < 0)
        return _sa(out, 0, mx, "wc: cannot read file\n");

    int lines = 0, words = 0, chars = sz;
    bool in_word = false;
    for (int i = 0; i < sz; i++) {
        if (buf[i] == '\n') lines++;
        if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n') {
            in_word = false;
        } else if (!in_word) {
            in_word = true; words++;
        }
    }

    int p = 0;
    p = _sa(out, p, mx, "  ");
    p = _sai(out, p, mx, lines);
    p = _sa(out, p, mx, "  ");
    p = _sai(out, p, mx, words);
    p = _sa(out, p, mx, "  ");
    p = _sai(out, p, mx, chars);
    p = _sac(out, p, mx, ' ');
    p = _sa(out, p, mx, argv[1]);
    p = _sac(out, p, mx, '\n');
    return p;
}

int LinuxCmds::cmd_chmod(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 3) return _sa(out, 0, mx, "Usage: chmod <mode> <file>\n");
    // parse octal mode
    unsigned short mode = 0;
    for (int i = 0; argv[1][i]; i++) {
        if (argv[1][i] >= '0' && argv[1][i] <= '7')
            mode = mode * 8 + (argv[1][i] - '0');
    }
    int err = KVFS::Chmod(argv[2], mode);
    if (err != KVFS_OK) return _sa(out, 0, mx, "chmod: failed\n");
    return 0;
}

int LinuxCmds::cmd_stat(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: stat <file>\n");
    KVFSNode* n = KVFS::ResolvePath(argv[1]);
    if (!n) return _sa(out, 0, mx, "stat: cannot stat: No such file\n");

    int p = 0;
    p = _sa(out, p, mx, "  File: ");
    p = _sa(out, p, mx, n->name);
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "  Size: ");
    p = _sau(out, p, mx, n->size);
    p = _sa(out, p, mx, "  Type: ");
    p = _sa(out, p, mx, n->type == KVFS_DIR ? "directory" : (n->type == KVFS_SYMLINK ? "symlink" : "regular file"));
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "  Mode: 0");
    // octal
    unsigned short m = n->perms.mode;
    p = _sac(out, p, mx, '0' + ((m >> 6) & 7));
    p = _sac(out, p, mx, '0' + ((m >> 3) & 7));
    p = _sac(out, p, mx, '0' + (m & 7));
    p = _sa(out, p, mx, "  Uid: ");
    p = _sau(out, p, mx, n->perms.uid);
    p = _sa(out, p, mx, "  Gid: ");
    p = _sau(out, p, mx, n->perms.gid);
    p = _sac(out, p, mx, '\n');
    return p;
}

int LinuxCmds::cmd_df(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _sa(out, p, mx, "Filesystem     1K-blocks  Used Available Use% Mounted on\n");
    p = _sa(out, p, mx, "kvfs           65536      2048 63488     3%   /\n");
    p = _sa(out, p, mx, "tmpfs          32768      0    32768     0%   /tmp\n");
    p = _sa(out, p, mx, "devfs          0          0    0         0%   /dev\n");
    return p;
}

int LinuxCmds::cmd_du(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    const char* path = (argc > 1) ? argv[1] : ".";
    KVFSNode* n = KVFS::ResolvePath(path);
    if (!n) return _sa(out, 0, mx, "du: cannot access\n");

    int p = 0;
    unsigned int total = n->size;
    for (int i = 0; i < n->child_count; i++) {
        if (n->children[i]) total += n->children[i]->size;
    }
    p = _sau(out, p, mx, total);
    p = _sac(out, p, mx, '\t');
    p = _sa(out, p, mx, path);
    p = _sac(out, p, mx, '\n');
    return p;
}

int LinuxCmds::cmd_ln(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 3) return _sa(out, 0, mx, "Usage: ln -s <target> <link>\n");
    // symlinks only for now
    bool soft = false;
    int target_idx = 1, link_idx = 2;
    for (int i = 1; i < argc; i++) {
        if (_seq(argv[i], "-s")) { soft = true; }
    }
    if (!soft) return _sa(out, 0, mx, "ln: only symbolic links supported (-s)\n");
    // find target and link args (skip flags)
    const char* args[2]; int ai = 0;
    for (int i = 1; i < argc && ai < 2; i++) {
        if (argv[i][0] != '-') args[ai++] = argv[i];
    }
    if (ai < 2) return _sa(out, 0, mx, "ln: missing operands\n");

    // create symlink node
    KVFS::CreateFile(args[1]);
    KVFSNode* n = KVFS::ResolvePath(args[1]);
    if (n) {
        n->type = KVFS_SYMLINK;
        KVFS::WriteString(args[1], args[0]);
    }
    (void)target_idx; (void)link_idx;
    return 0;
}

int LinuxCmds::cmd_find(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    const char* path = ".";
    const char* pattern = "*";
    for (int i = 1; i < argc; i++) {
        if (_seq(argv[i], "-name") && i + 1 < argc) pattern = argv[++i];
        else if (argv[i][0] != '-') path = argv[i];
    }

    KVFSNode* results[32];
    int count = KVFS::Find(path, pattern, results, 32);

    int p = 0;
    for (int i = 0; i < count && p < mx - 1; i++) {
        if (results[i]) p = _sa(out, p, mx, results[i]->name);
        p = _sac(out, p, mx, '\n');
    }
    return p;
}

int LinuxCmds::cmd_grep(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 3) return _sa(out, 0, mx, "Usage: grep <pattern> <file...>\n");
    const char* pattern = argv[1];
    int p = 0;

    for (int fi = 2; fi < argc; fi++) {
        char grep_buf[4096];
        int glen = KVFS::Grep(argv[fi], pattern, grep_buf, 4096);
        if (glen > 0) {
            if (argc > 3) {
                p = _sa(out, p, mx, argv[fi]);
                p = _sac(out, p, mx, ':');
            }
            for (int i = 0; i < glen && p < mx - 1; i++)
                out[p++] = grep_buf[i];
            out[p] = 0;
        }
    }
    return p;
}

int LinuxCmds::cmd_which(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    if (argc < 2) return _sa(out, 0, mx, "Usage: which <command>\n");
    ShellCommand* cmd = sh->FindCommand(argv[1]);
    if (cmd) {
        int p = _sa(out, 0, mx, "/bin/");
        p = _sa(out, p, mx, argv[1]);
        return _sac(out, p, mx, '\n');
    }
    int p = _sa(out, 0, mx, argv[1]);
    return _sa(out, p, mx, " not found\n");
}

int LinuxCmds::cmd_tee(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: tee <file>\n");
    // in a pipe context, tee would write stdin to file. here, write last output.
    KVFS::CreateFile(argv[1]);
    return _sa(out, 0, mx, "(tee: pipe context required)\n");
}

int LinuxCmds::cmd_sort(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: sort <file>\n");

    unsigned char buf[KVFS_MAX_CONTENT];
    int sz = KVFS::ReadFile(argv[1], buf, KVFS_MAX_CONTENT);
    if (sz < 0)
        return _sa(out, 0, mx, "sort: cannot read file\n");

    // split into lines
    char* lines[256];
    int lc = 0;
    char* start = (char*)buf;
    for (int i = 0; i < sz; i++) {
        if (buf[i] == '\n') {
            buf[i] = 0;
            if (lc < 256) lines[lc++] = start;
            start = (char*)buf + i + 1;
        }
    }
    if (start < (char*)buf + sz && lc < 256) lines[lc++] = start;

    // bubble sort
    for (int i = 0; i < lc - 1; i++) {
        for (int j = 0; j < lc - i - 1; j++) {
            const char* a = lines[j]; const char* b = lines[j+1];
            bool swap = false;
            while (*a && *b) { if (*a > *b) { swap = true; break; } if (*a < *b) break; a++; b++; }
            if (swap || (*a && !*b)) { char* t = lines[j]; lines[j] = lines[j+1]; lines[j+1] = t; }
        }
    }

    int p = 0;
    for (int i = 0; i < lc && p < mx - 1; i++) {
        p = _sa(out, p, mx, lines[i]);
        p = _sac(out, p, mx, '\n');
    }
    return p;
}

int LinuxCmds::cmd_uniq(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: uniq <file>\n");

    unsigned char buf[KVFS_MAX_CONTENT];
    int sz = KVFS::ReadFile(argv[1], buf, KVFS_MAX_CONTENT);
    if (sz < 0)
        return _sa(out, 0, mx, "uniq: cannot read file\n");

    int p = 0;
    char prev[256]; prev[0] = 0;
    char line[256]; int li = 0;

    for (int i = 0; i <= sz; i++) {
        if (i == sz || buf[i] == '\n') {
            line[li] = 0;
            if (!_seq(line, prev)) {
                p = _sa(out, p, mx, line);
                p = _sac(out, p, mx, '\n');
                _scpy(prev, line, 256);
            }
            li = 0;
        } else {
            if (li < 255) line[li++] = (char)buf[i];
        }
    }
    return p;
}

int LinuxCmds::cmd_tr(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 3) return _sa(out, 0, mx, "Usage: tr <set1> <set2>\n");
    return _sa(out, 0, mx, "(tr: pipe context required)\n");
}

//  system commands

int LinuxCmds::cmd_ps(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _sa(out, p, mx, "  PID TTY      TIME CMD\n");
    p = _sa(out, p, mx, "    0 ?     00:00:00 kernel\n");
    p = _sa(out, p, mx, "    1 ?     00:00:00 init\n");
    p = _sa(out, p, mx, "    2 tty1  00:00:00 ksh\n");
    p = _sa(out, p, mx, "    3 tty1  00:00:00 desktop\n");
    // from scheduler
    for (int i = 0; i < (int)Scheduler::GetProcessCount() && i < 16; i++) {
        p = _sa(out, p, mx, "    ");
        p = _sai(out, p, mx, 10 + i);
        p = _sa(out, p, mx, " ?     00:00:00 task_");
        p = _sai(out, p, mx, i);
        p = _sac(out, p, mx, '\n');
    }
    return p;
}

int LinuxCmds::cmd_kill(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: kill <pid>\n");
    int pid = _atoi(argv[1]);
    if (pid < 4) return _sa(out, 0, mx, "kill: cannot kill system process\n");
    int p = _sa(out, 0, mx, "kill: sent SIGTERM to ");
    p = _sai(out, p, mx, pid);
    return _sac(out, p, mx, '\n');
}

int LinuxCmds::cmd_uname(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    bool all = false;
    for (int i = 1; i < argc; i++) { if (_seq(argv[i], "-a")) all = true; }

    if (all) return _sa(out, 0, mx, "Kurono kurono-machine 1.0.0 #1 SMP x86 i686 KuronoOS\n");
    return _sa(out, 0, mx, "Kurono\n");
}

int LinuxCmds::cmd_uptime(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    unsigned int ticks = (unsigned int)(TimeManager::NowUTC().us / 1000u);
    unsigned int seconds = ticks / 1000;
    unsigned int hours = seconds / 3600;
    unsigned int minutes = (seconds % 3600) / 60;
    unsigned int secs = seconds % 60;

    int p = _sa(out, 0, mx, " up ");
    p = _sau(out, p, mx, hours);
    p = _sac(out, p, mx, ':');
    if (minutes < 10) p = _sac(out, p, mx, '0');
    p = _sau(out, p, mx, minutes);
    p = _sac(out, p, mx, ':');
    if (secs < 10) p = _sac(out, p, mx, '0');
    p = _sau(out, p, mx, secs);
    p = _sa(out, p, mx, ", 1 user, load: 0.01 0.02 0.00\n");
    return p;
}

int LinuxCmds::cmd_whoami(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)argc; (void)argv;
    const char* u = sh->GetVar("USER");
    int p = _sa(out, 0, mx, u ? u : "user");
    return _sac(out, p, mx, '\n');
}

int LinuxCmds::cmd_hostname(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)argc; (void)argv;
    const char* h = sh->GetVar("HOSTNAME");
    int p = _sa(out, 0, mx, h ? h : "kurono-machine");
    return _sac(out, p, mx, '\n');
}

int LinuxCmds::cmd_date(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    DateTime dt = TimeManager::NowUTCDateTime();
    static const char* dow_names[] = {"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
    static const char* mon_names[] = {"","Jan","Feb","Mar","Apr","May","Jun",
                                       "Jul","Aug","Sep","Oct","Nov","Dec"};
    int p = 0;
    if (dt.dow >= 1 && dt.dow <= 7)
        p = _sa(out, p, mx, dow_names[dt.dow - 1]);
    p = _sac(out, p, mx, ' ');
    if (dt.mon >= 1 && dt.mon <= 12)
        p = _sa(out, p, mx, mon_names[dt.mon]);
    p = _sac(out, p, mx, ' ');
    if (dt.dom < 10) p = _sac(out, p, mx, ' ');
    p = _sai(out, p, mx, dt.dom);
    p = _sac(out, p, mx, ' ');
    if (dt.h < 10) p = _sac(out, p, mx, '0');
    p = _sai(out, p, mx, dt.h);
    p = _sac(out, p, mx, ':');
    if (dt.m < 10) p = _sac(out, p, mx, '0');
    p = _sai(out, p, mx, dt.m);
    p = _sac(out, p, mx, ':');
    if (dt.s < 10) p = _sac(out, p, mx, '0');
    p = _sai(out, p, mx, dt.s);
    p = _sa(out, p, mx, " UTC ");
    p = _sai(out, p, mx, dt.year);
    p = _sac(out, p, mx, '\n');
    return p;
}

int LinuxCmds::cmd_free(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    unsigned int total = 64 * 1024; // 64 mb in kb
    unsigned int used  = KernelHeap::GetUsed() / 1024;
    unsigned int free_mem = total - used;

    int p = 0;
    p = _sa(out, p, mx, "              total       used       free\n");
    p = _sa(out, p, mx, "Mem:       ");
    p = _sau(out, p, mx, total);
    p = _sa(out, p, mx, "      ");
    p = _sau(out, p, mx, used);
    p = _sa(out, p, mx, "      ");
    p = _sau(out, p, mx, free_mem);
    p = _sac(out, p, mx, '\n');
    return p;
}

static bool lc_arg_is_decimal_pos(const char* s) {
    if (!s || !s[0]) return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
    }
    return true;
}

static int lc_kvfs_tail_into_out(const char* path, char* out, int mx,
                                 int chars_hint_before,
                                 const char* banner_line) {
    if (!path || !out || mx < 96) return 0;
    int p = 0;
    if (banner_line && banner_line[0])
        p = _sa(out, p, mx, banner_line);

    KVFSNode* node = KVFS::Resolve(path);
    if (!node || !node->is_file()) {
        p = _sa(out, p, mx, "Log file ");
        p = _sa(out, p, mx, path);
        p = _sa(out, p, mx, " is missing (logging uses KVFS after init).\n");
        return p;
    }
    uint32_t fsz = node->size;
    if (fsz == 0) {
        p = _sa(out, p, mx, "(empty log file)\n");
        return p;
    }

    int space_after_banner = mx - p - 2;
    if (space_after_banner < 32)
        return p;

    int slab_cap = space_after_banner;
    if (slab_cap > 7680)
        slab_cap = 7680;

    int hint = chars_hint_before;
    if (hint < 512)
        hint = 512;
    if (hint > slab_cap)
        hint = slab_cap;

    uint32_t start_byte = hint >= (int)fsz ? 0u : (fsz - (uint32_t)hint);

    int fd = KVFS::Open(path, 1);
    if (fd < 0) {
        p = _sa(out, p, mx, "Could not open log.\n");
        return p;
    }
    KVFS::Seek(fd, (int32_t)start_byte, 0);

    uint8_t* slab = (uint8_t*)KernelHeap::Alloc((uint32_t)(slab_cap + 8));
    if (!slab) {
        KVFS::Close(fd);
        p = _sa(out, p, mx, "Not enough heap for log tail.\n");
        return p;
    }

    int rr = KVFS::Read(fd, slab, (uint32_t)slab_cap);
    KVFS::Close(fd);

    slab[slab_cap] = 0;
    int body_off = 0;
    int body_len = rr;
    if (start_byte > 0 && rr > 0) {
        while (body_off < rr && slab[body_off] != '\n')
            body_off++;
        if (body_off < rr && slab[body_off] == '\n')
            body_off++;
        body_len = rr - body_off;
    }
    int copy_budget = mx - p - 1;
    int copy_len = body_len > copy_budget ? copy_budget : body_len;
    for (int i = 0; i < copy_len; i++)
        out[p++] = (char)slab[body_off + i];
    KernelHeap::Free(slab);
    if (copy_len <= 0) {
        p = _sa(out, p, mx, "(no tail lines in window)\n");
        return p;
    }
    if (p <= 0 || out[p - 1] != '\n')
        p = _sac(out, p, mx, '\n');
    out[p] = 0;
    return p;
}

int LinuxCmds::cmd_journal(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;

    const char* log_path = "/kurono/logs/system.log";
    int approx_lines = 120;

    if (argc >= 2 && lc_arg_is_decimal_pos(argv[1]))
        approx_lines = _atoi(argv[1]);
    else if (argc >= 2)
        log_path = argv[1];

    if (argc >= 3) {
        if (lc_arg_is_decimal_pos(argv[1])) {
            approx_lines = _atoi(argv[1]);
            log_path = argv[2];
        } else if (lc_arg_is_decimal_pos(argv[2])) {
            log_path = argv[1];
            approx_lines = _atoi(argv[2]);
        } else {
            log_path = argv[2];
        }
    }

    if (approx_lines < 8)
        approx_lines = 8;
    if (approx_lines > 2000)
        approx_lines = 2000;

    int hint = approx_lines * 112;
    if (hint > mx * 48)
        hint = mx * 48;
    char banner[192];
    int bp = _sa(banner, 0, sizeof(banner), "=== tail ");
    bp = _sa(banner, bp, sizeof(banner), log_path);
    bp = _sa(banner, bp, sizeof(banner), " ===\n");
    return lc_kvfs_tail_into_out(log_path, out, mx, hint, banner);
}

int LinuxCmds::cmd_mount(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _sa(out, p, mx, "kvfs on / type kvfs (rw)\n");
    p = _sa(out, p, mx, "tmpfs on /tmp type tmpfs (rw)\n");
    p = _sa(out, p, mx, "devfs on /dev type devfs (rw)\n");
    p = _sa(out, p, mx, "procfs on /proc type procfs (ro)\n");
    return p;
}

int LinuxCmds::cmd_dmesg(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    (void)argc;
    (void)argv;
    uint32_t sec = Time::GetTicks() / 1000u;
    char banner[144];
    int bp = _sa(banner, 0, sizeof(banner), "=== mirrored serial (/kurono/logs/serial.log)  -  uptime ");
    bp = _sau(banner, bp, sizeof(banner), sec);
    bp = _sa(banner, bp, sizeof(banner), " s ===\n");
    int hint = mx > 384 ? mx - 256 : 512;
    if (hint > 7800)
        hint = 7800;
    return lc_kvfs_tail_into_out("/kurono/logs/serial.log", out, mx, hint, banner);
}

int LinuxCmds::cmd_lspci(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _sa(out, p, mx, "00:00.0 Host bridge: Intel 440FX - 82441FX PMC\n");
    p = _sa(out, p, mx, "00:01.0 ISA bridge: Intel 82371SB PIIX3\n");
    p = _sa(out, p, mx, "00:02.0 VGA compatible controller: Bochs/QEMU BGA ");
    p = _sa(out, p, mx, BGA::IsAvailable() ? "[driver active]\n" : "[not detected]\n");
    p = _sa(out, p, mx, "00:03.0 Ethernet controller: Intel 82540EM (e1000) ");
    p = _sa(out, p, mx, E1000::IsDetected() ? "[driver active]\n" : "[not detected]\n");
    p = _sa(out, p, mx, "00:04.0 Multimedia audio controller: ");
    if (HDAudio::IsDetected()) {
        p = _sa(out, p, mx, "Intel HDA [driver active]\n");
    } else {
        p = _sa(out, p, mx, "Sound Blaster 16 ");
        p = _sa(out, p, mx, Audio::IsAvailable() ? "[driver active]\n" : "[not detected]\n");
    }
    if (NVMe::IsDetected()) {
        p = _sa(out, p, mx, "00:05.0 Non-Volatile memory controller: NVMe SSD ");
        const NVMeControllerInfo& ni = NVMe::GetInfo();
        p = _sa(out, p, mx, ni.model);
        p = _sa(out, p, mx, " [driver active]\n");
    }
    if (USB::IsDetected()) {
        p = _sa(out, p, mx, "00:06.0 USB controller: xHCI Host Controller [");
        p = _sai(out, p, mx, USB::GetPortCount());
        p = _sa(out, p, mx, " ports]\n");
    }
    if (VirtIOGPU::IsDetected()) {
        p = _sa(out, p, mx, "00:07.0 Display controller: VirtIO GPU [driver active]\n");
    }
    if (NvidiaGPU::IsDetected()) {
        const NvidiaGPUInfo& gi = NvidiaGPU::GetInfo();
        p = _sa(out, p, mx, "01:00.0 VGA compatible controller: NVIDIA ");
        p = _sa(out, p, mx, gi.name);
        p = _sa(out, p, mx, "\n");
    }
    return p;
}

int LinuxCmds::cmd_lsmod(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _sa(out, p, mx, "Module                  Size  Used by\n");
    p = _sa(out, p, mx, "bga                    4096  1\n");
    if (E1000::IsDetected()) p = _sa(out, p, mx, "e1000                  8192  2\n");
    if (Audio::IsAvailable()) p = _sa(out, p, mx, "sb16                   4096  1\n");
    if (HDAudio::IsDetected()) p = _sa(out, p, mx, "snd_hda_intel         16384  1\n");
    if (NVMe::IsDetected())    p = _sa(out, p, mx, "nvme                  12288  1\n");
    if (USB::IsDetected())     p = _sa(out, p, mx, "xhci_hcd              20480  1\n");
    if (VirtIOGPU::IsDetected()) p = _sa(out, p, mx, "virtio_gpu             8192  0\n");
    if (TCPStack::IsUp())      p = _sa(out, p, mx, "tcpip                  8192  3\n");
    if (VMM::IsSupported()) {
        p = _sa(out, p, mx, VMM::GetType() == VIRT_INTEL_VTX ? "kvm_intel              16384 1\n"
                                                              : "kvm_amd                16384 1\n");
    }
    if (NvidiaGPU::IsDetected()) p = _sa(out, p, mx, "nvidia                 24576 0\n");
    return p;
}

int LinuxCmds::cmd_drivers(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _sa(out, p, mx, "Driver          Status\n");
    p = _sa(out, p, mx, "  display     : BGA ");
    p = _sa(out, p, mx, BGA::IsAvailable() ? "online" : "offline");
    p = _sa(out, p, mx, " ("); p = _sau(out, p, mx, BGA::width);
    p = _sac(out, p, mx, 'x'); p = _sau(out, p, mx, BGA::height);
    p = _sa(out, p, mx, ")\n");
    p = _sa(out, p, mx, "  net         : e1000 ");
    p = _sa(out, p, mx, E1000::IsDetected() ? (E1000::IsLinkUp() ? "link-up\n" : "link-down\n") : "offline\n");
    p = _sa(out, p, mx, "  tcpip       : ");
    p = _sa(out, p, mx, TCPStack::IsUp() ? "active" : "inactive");
    if (TCPStack::IsUp()) {
        char ipbuf[16]; TCPStack::FormatIP(TCPStack::GetIP(), ipbuf);
        p = _sa(out, p, mx, " ("); p = _sa(out, p, mx, ipbuf); p = _sac(out, p, mx, ')');
    }
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "  audio       : ");
    if (HDAudio::IsDetected()) p = _sa(out, p, mx, "HDA online\n");
    else p = _sa(out, p, mx, Audio::IsAvailable() ? "SB16 online\n" : "offline\n");
    p = _sa(out, p, mx, "  storage     : ");
    if (NVMe::IsDetected()) {
        const NVMeControllerInfo& ni = NVMe::GetInfo();
        p = _sa(out, p, mx, "NVMe "); p = _sa(out, p, mx, ni.model); p = _sac(out, p, mx, '\n');
    } else {
        p = _sa(out, p, mx, "none\n");
    }
    p = _sa(out, p, mx, "  usb         : ");
    if (USB::IsDetected()) {
        p = _sa(out, p, mx, "xHCI ("); p = _sai(out, p, mx, USB::GetDeviceCount());
        p = _sa(out, p, mx, " devices on "); p = _sai(out, p, mx, USB::GetPortCount());
        p = _sa(out, p, mx, " ports)\n");
    } else { p = _sa(out, p, mx, "offline\n"); }
    p = _sa(out, p, mx, "  virt        : ");
    if (VMM::IsSupported()) {
        p = _sa(out, p, mx, VMM::GetType() == VIRT_INTEL_VTX ? "VT-x\n" : "AMD-V\n");
    } else { p = _sa(out, p, mx, "none\n"); }
    p = _sa(out, p, mx, "  gpu         : ");
    if (NvidiaGPU::IsDetected()) {
        p = _sa(out, p, mx, NvidiaGPU::GetInfo().name); p = _sac(out, p, mx, '\n');
    } else if (VirtIOGPU::IsDetected()) {
        p = _sa(out, p, mx, "VirtIO GPU\n");
    } else { p = _sa(out, p, mx, "BGA fallback\n"); }
    return p;
}

//  extended driver/hardware introspection commands

int LinuxCmds::cmd_lsblk(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _sa(out, p, mx, "NAME    MAJ:MIN RM   SIZE RO TYPE MOUNTPOINT\n");
    if (NVMe::IsDetected()) {
        uint64_t cap = NVMe::GetCapacityLBA();
        uint32_t lba = NVMe::GetLBASize();
        uint64_t mb = (cap * lba) / (1024 * 1024);
        p = _sa(out, p, mx, "nvme0n1 259:0    0  ");
        p = _sa64(out, p, mx, mb);
        p = _sa(out, p, mx, "M  0 disk\n");
        p = _sa(out, p, mx, "|-nvme0n1p1          0  ");
        p = _sa64(out, p, mx, mb > 64 ? mb - 64 : mb);
        p = _sa(out, p, mx, "M  0 part /\n");
    } else {
        p = _sa(out, p, mx, "sda      8:0    0   256M  0 disk\n");
        p = _sa(out, p, mx, "|-sda1   8:1    0   200M  0 part /\n");
        p = _sa(out, p, mx, "|-sda2   8:2    0    56M  0 part /home\n");
    }
    p = _sa(out, p, mx, "sr0     11:0    1  1024M  0 rom\n");
    p = _sa(out, p, mx, "kvfs     0:1    0     4M  0 virt /\n");
    return p;
}

int LinuxCmds::cmd_lsusb(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _sa(out, p, mx, "Bus 001 Device 001: ID 1d6b:0002 Linux Foundation 2.0 root hub\n");
    if (USB::IsDetected()) {
        int devs = USB::GetDeviceCount();
        for (int i = 0; i < devs && i < 8; i++) {
            const USBDeviceInfo* dev = USB::GetDevice(i);
            if (!dev || !dev->connected) continue;
            p = _sa(out, p, mx, "Bus 001 Device 00");
            p = _sai(out, p, mx, i + 2);
            p = _sa(out, p, mx, ": ID ");
            p = _sah(out, p, mx, dev->vendor_id);
            p = _sac(out, p, mx, ':');
            p = _sah(out, p, mx, dev->product_id);
            p = _sac(out, p, mx, ' ');
            if (dev->manufacturer[0]) { p = _sa(out, p, mx, dev->manufacturer); p = _sac(out, p, mx, ' '); }
            if (dev->product[0]) p = _sa(out, p, mx, dev->product);
            else p = _sa(out, p, mx, USB::SpeedName(dev->speed));
            p = _sac(out, p, mx, '\n');
        }
    } else {
        p = _sa(out, p, mx, "Bus 001 Device 002: ID 0627:0001 QEMU USB Tablet\n");
    }
    p = _sa(out, p, mx, "Bus 002 Device 001: ID 1d6b:0003 Linux Foundation 3.0 root hub\n");
    return p;
}

int LinuxCmds::cmd_lscpu(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _sa(out, p, mx, "Architecture:         x86_64\n");
    p = _sa(out, p, mx, "CPU op-mode(s):       32-bit, 64-bit\n");
    p = _sa(out, p, mx, "Byte Order:           Little Endian\n");
    p = _sa(out, p, mx, "CPU(s):               1\n");
    p = _sa(out, p, mx, "Vendor ID:            ");
    p = _sa(out, p, mx, VMM::GetVendor());
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "Model name:           QEMU Virtual CPU\n");
    p = _sa(out, p, mx, "CPU MHz:              2000.000\n");
    p = _sa(out, p, mx, "L1d cache:            32 KiB\n");
    p = _sa(out, p, mx, "L1i cache:            32 KiB\n");
    p = _sa(out, p, mx, "L2 cache:             4096 KiB\n");
    p = _sa(out, p, mx, "Virtualization:       ");
    if (VMM::IsSupported()) {
        p = _sa(out, p, mx, VMM::GetType() == VIRT_INTEL_VTX ? "VT-x" : "AMD-V");
    } else {
        p = _sa(out, p, mx, "none");
    }
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "Hypervisor vendor:    ");
    p = _sa(out, p, mx, Hypervisor::IsAvailable() ? "KVM" : "none");
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "Flags:                fpu vme de pse tsc msr pae mce cx8 apic sep "
                         "mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2");
    if (VMM::IsSupported()) p = _sa(out, p, mx, " vmx svm");
    p = _sac(out, p, mx, '\n');
    return p;
}

int LinuxCmds::cmd_modprobe(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: modprobe <module>\n");
    int p = 0;
    const char* mod = argv[1];
    // simulate loading known modules
    if (_seq(mod, "e1000") || _seq(mod, "sb16") || _seq(mod, "bga") ||
        _seq(mod, "nvidia") || _seq(mod, "kvm_intel") || _seq(mod, "kvm_amd") ||
        _seq(mod, "nvme") || _seq(mod, "xhci_hcd") || _seq(mod, "snd_hda_intel") ||
        _seq(mod, "virtio_gpu") || _seq(mod, "virtio_net") || _seq(mod, "virtio_blk")) {
        p = _sa(out, p, mx, "modprobe: loading '");
        p = _sa(out, p, mx, mod);
        p = _sa(out, p, mx, "'\n");
    } else {
        p = _sa(out, p, mx, "modprobe: FATAL: Module ");
        p = _sa(out, p, mx, mod);
        p = _sa(out, p, mx, " not found in modules.dep\n");
    }
    return p;
}

int LinuxCmds::cmd_modinfo(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: modinfo <module>\n");
    int p = 0;
    const char* mod = argv[1];
    if (_seq(mod, "e1000")) {
        p = _sa(out, p, mx, "filename:       /lib/modules/kurono/e1000.ko\n");
        p = _sa(out, p, mx, "version:        1.0.0\n");
        p = _sa(out, p, mx, "description:    Intel PRO/1000 Network Driver\n");
        p = _sa(out, p, mx, "author:         Kurono OS Team\n");
        p = _sa(out, p, mx, "license:        GPL v2\n");
        p = _sa(out, p, mx, "alias:          pci:v00008086d*\n");
        p = _sa(out, p, mx, "depends:        \n");
        p = _sa(out, p, mx, "parm:           speed:Force link speed (int)\n");
    } else if (_seq(mod, "sb16")) {
        p = _sa(out, p, mx, "filename:       /lib/modules/kurono/sb16.ko\n");
        p = _sa(out, p, mx, "version:        1.0.0\n");
        p = _sa(out, p, mx, "description:    Sound Blaster 16 Audio Driver\n");
        p = _sa(out, p, mx, "author:         Kurono OS Team\n");
        p = _sa(out, p, mx, "license:        GPL v2\n");
        p = _sa(out, p, mx, "parm:           irq:IRQ number (int)\n");
        p = _sa(out, p, mx, "parm:           dma:DMA channel (int)\n");
    } else if (_seq(mod, "bga")) {
        p = _sa(out, p, mx, "filename:       /lib/modules/kurono/bga.ko\n");
        p = _sa(out, p, mx, "version:        1.0.0\n");
        p = _sa(out, p, mx, "description:    Bochs/QEMU VGA Graphics Adapter\n");
        p = _sa(out, p, mx, "author:         Kurono OS Team\n");
        p = _sa(out, p, mx, "license:        GPL v2\n");
    } else if (_seq(mod, "nvidia")) {
        p = _sa(out, p, mx, "filename:       /lib/modules/kurono/nvidia.ko\n");
        p = _sa(out, p, mx, "version:        545.29.06\n");
        p = _sa(out, p, mx, "description:    NVIDIA GPU Driver (passthrough)\n");
        p = _sa(out, p, mx, "author:         NVIDIA Corporation\n");
        p = _sa(out, p, mx, "license:        Proprietary\n");
        p = _sa(out, p, mx, "alias:          pci:v000010DEd*\n");
    } else if (_seq(mod, "nvme")) {
        p = _sa(out, p, mx, "filename:       /lib/modules/kurono/nvme.ko\n");
        p = _sa(out, p, mx, "version:        1.0.0\n");
        p = _sa(out, p, mx, "description:    NVM Express block device driver\n");
        p = _sa(out, p, mx, "author:         Kurono OS Team\n");
        p = _sa(out, p, mx, "license:        GPL v2\n");
    } else {
        p = _sa(out, p, mx, "modinfo: ERROR: Module ");
        p = _sa(out, p, mx, mod);
        p = _sa(out, p, mx, " not found.\n");
    }
    return p;
}

int LinuxCmds::cmd_insmod(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: insmod <module.ko>\n");
    int p = _sa(out, 0, mx, "insmod: inserting ");
    p = _sa(out, p, mx, argv[1]);
    p = _sa(out, p, mx, "\ninsmod: module loaded\n");
    return p;
}

int LinuxCmds::cmd_rmmod(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: rmmod <module>\n");
    int p = _sa(out, 0, mx, "rmmod: removing ");
    p = _sa(out, p, mx, argv[1]);
    p = _sa(out, p, mx, "\nrmmod: module removed\n");
    return p;
}

int LinuxCmds::cmd_dmidecode(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _sa(out, p, mx, "# dmidecode 3.4\n");
    p = _sa(out, p, mx, "SMBIOS present.\n\n");
    p = _sa(out, p, mx, "Handle 0x0000, DMI type 0, 24 bytes\n");
    p = _sa(out, p, mx, "BIOS Information\n");
    p = _sa(out, p, mx, "  Vendor:   Kurono BIOS\n");
    p = _sa(out, p, mx, "  Version:  1.0.0\n");
    p = _sa(out, p, mx, "  Date:     06/15/2025\n\n");
    p = _sa(out, p, mx, "Handle 0x0100, DMI type 1, 27 bytes\n");
    p = _sa(out, p, mx, "System Information\n");
    p = _sa(out, p, mx, "  Manufacturer: Kurono Project\n");
    p = _sa(out, p, mx, "  Product:      Kurono OS Virtual Machine\n");
    p = _sa(out, p, mx, "  Version:      1.0\n");
    p = _sa(out, p, mx, "  UUID:         A1B2C3D4-E5F6-7890-ABCD-EF0123456789\n\n");
    p = _sa(out, p, mx, "Handle 0x0400, DMI type 4, 42 bytes\n");
    p = _sa(out, p, mx, "Processor Information\n");
    p = _sa(out, p, mx, "  Socket:       CPU0\n");
    p = _sa(out, p, mx, "  Type:         Central Processor\n");
    p = _sa(out, p, mx, "  Vendor:       ");
    p = _sa(out, p, mx, VMM::GetVendor());
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "  Max Speed:    2000 MHz\n");
    p = _sa(out, p, mx, "  Core Count:   1\n");
    p = _sa(out, p, mx, "  Thread Count: 1\n\n");
    p = _sa(out, p, mx, "Handle 0x1100, DMI type 17, 40 bytes\n");
    p = _sa(out, p, mx, "Memory Device\n");
    p = _sa(out, p, mx, "  Size:         256 MB\n");
    p = _sa(out, p, mx, "  Type:         DDR4\n");
    p = _sa(out, p, mx, "  Speed:        2400 MT/s\n");
    return p;
}

int LinuxCmds::cmd_hwinfo(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    CpuInfo cpu = CPUDetect::GetInfo();
    const char* cpu_brand = cpu.brand_string[0] ? cpu.brand_string : CPUDetect::GetVendorName();
    int cpu_cores = cpu.topology.physical_cores > 0 ? cpu.topology.physical_cores : CPUDetect::GetCoreCount();
    int cpu_threads = cpu.topology.logical_cores > 0 ? cpu.topology.logical_cores : CPUDetect::GetThreadCount();
    const GpuProbeResult& gpr = GpuProbe::GetResult();
    const char* gpu_desc = "No GPU detected";
    LinuxDriver* wifi_drv = _find_wifi_driver();
    LinuxDriver* bt_drv = _find_bt_driver();
    if (gpr.count > 0) {
        if (gpr.primary_idx >= 0 && gpr.primary_idx < gpr.count) gpu_desc = gpr.gpus[gpr.primary_idx].desc;
        else gpu_desc = gpr.gpus[0].desc;
    }
    p = _sa(out, p, mx, "==== Hardware Summary ====\n\n");
    p = _sa(out, p, mx, "CPU:       ");
    p = _sa(out, p, mx, cpu_brand);
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "  Cores:   ");
    p = _sai(out, p, mx, cpu_cores);
    p = _sa(out, p, mx, "  Threads: ");
    p = _sai(out, p, mx, cpu_threads);
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "  Base:    ");
    if (cpu.frequency.base_mhz > 0) p = _sai(out, p, mx, cpu.frequency.base_mhz);
    else p = _sa(out, p, mx, "unknown");
    p = _sa(out, p, mx, " MHz\n");
    p = _sa(out, p, mx, "  Virt:    ");
    p = _sa(out, p, mx, VMM::IsSupported() ? (VMM::GetType() == VIRT_INTEL_VTX ? "VT-x" : "AMD-V") : "none");
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "\nDisplay:\n");
    p = _sa(out, p, mx, "  Backend: ");
    p = _sa(out, p, mx, DisplayManager::GetBackendName());
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "  GPU:     ");
    p = _sa(out, p, mx, gpu_desc);
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "  Res:     ");
    p = _sau(out, p, mx, (unsigned)Graphics::GetWidth());
    p = _sac(out, p, mx, 'x');
    p = _sau(out, p, mx, (unsigned)Graphics::GetHeight());
    p = _sa(out, p, mx, " @ ");
    p = _sau(out, p, mx, (unsigned)Graphics::GetBpp());
    p = _sa(out, p, mx, "bpp\n");
    if (NvidiaGPU::IsDetected()) {
        const NvidiaGPUInfo& gi = NvidiaGPU::GetInfo();
        p = _sa(out, p, mx, "  dGPU:    NVIDIA ");
        p = _sa(out, p, mx, gi.name);
        p = _sa(out, p, mx, " (");
        p = _sau(out, p, mx, gi.vram_mb);
        p = _sa(out, p, mx, " MB VRAM)\n");
    } else if (AmdGPU::IsAvailable()) {
        const AmdGPUInfo& amd = AmdGPU::GetInfo();
        p = _sa(out, p, mx, "  GPU drv: AMD ");
        p = _sa(out, p, mx, AmdGPU::GetArchName());
        p = _sa(out, p, mx, " (");
        p = _sa64(out, p, mx, amd.vram_size / (1024 * 1024));
        p = _sa(out, p, mx, " MB VRAM)\n");
    } else if (IntelGPU::IsDetected()) {
        p = _sa(out, p, mx, "  GPU drv: Intel ");
        p = _sa(out, p, mx, IntelGPU::GetGenName());
        p = _sac(out, p, mx, '\n');
    }
    p = _sa(out, p, mx, "\nNetwork:\n");
    p = _sa(out, p, mx, "  NIC:     Intel e1000 ");
    p = _sa(out, p, mx, E1000::IsDetected() ? (E1000::IsLinkUp() ? "link-up" : "link-down") : "not-detected");
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "  WiFi:    ");
    p = _sa(out, p, mx, wifi_drv ? "metadata-bound" : "not bound");
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "  WiFiDrv: ");
    p = _sa(out, p, mx, wifi_drv ? (wifi_drv->description[0] ? wifi_drv->description : wifi_drv->name) : "none");
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "  BT:      ");
    p = _sa(out, p, mx, bt_drv ? (bt_drv->description[0] ? bt_drv->description : bt_drv->name) : "no metadata");
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "\nAudio:\n");
    p = _sa(out, p, mx, "  Card:    Sound Blaster 16 ");
    p = _sa(out, p, mx, Audio::IsAvailable() ? "(active)" : "(inactive)");
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "  DSP ver: ");
    p = _sai(out, p, mx, Audio::GetDSPVersion() >> 8);
    p = _sac(out, p, mx, '.');
    p = _sai(out, p, mx, Audio::GetDSPVersion() & 0xFF);
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "\nStorage:\n");
    p = _sa(out, p, mx, "  KVFS virtual filesystem (4 MB)\n");
    p = _sa(out, p, mx, "\nVirtualization:\n");
    p = _sa(out, p, mx, "  Hypervisor: ");
    p = _sa(out, p, mx, Hypervisor::IsAvailable() ? "available" : "not available");
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "  VM State:   ");
    switch (Hypervisor::GetState()) {
        case VM_STATE_UNINITIALIZED: p = _sa(out, p, mx, "uninitialized"); break;
        case VM_STATE_CREATED:       p = _sa(out, p, mx, "created"); break;
        case VM_STATE_RUNNING:       p = _sa(out, p, mx, "running"); break;
        case VM_STATE_HALTED:        p = _sa(out, p, mx, "halted"); break;
        default:                     p = _sa(out, p, mx, "other"); break;
    }
    p = _sac(out, p, mx, '\n');
    return p;
}

int LinuxCmds::cmd_top(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _sa(out, p, mx, "top - Kurono OS Process Monitor\n");
    p = _sa(out, p, mx, "Tasks: ");
    p = _sai(out, p, mx, Scheduler::GetProcessCount());
    p = _sa(out, p, mx, " total, 1 running\n");
    p = _sa(out, p, mx, "  PID USER      PR  NI    VIRT    RES    SHR S  %CPU %MEM   COMMAND\n");
    // list scheduler processes by walking the linked list
    Process* proc = Scheduler::ready_queue;
    int i = 0;
    while (proc && i < 16) {
        const char* name = proc->name;
        if (!name || !name[0]) { proc = proc->next; i++; continue; }
        p = _sa(out, p, mx, "  ");
        p = _sai(out, p, mx, proc->pid);
        // pad pid
        if (proc->pid < 10) p = _sa(out, p, mx, "   ");
        else p = _sa(out, p, mx, "  ");
        p = _sa(out, p, mx, "root       20   0     512    256    128 ");
        p = _sa(out, p, mx, (proc == Scheduler::current_process) ? "R" : "S");
        p = _sa(out, p, mx, "   0.0  0.1   ");
        p = _sa(out, p, mx, name);
        p = _sac(out, p, mx, '\n');
        proc = proc->next;
        i++;
    }
    return p;
}

int LinuxCmds::cmd_ip(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    int p = 0;
    const char* sub = (argc > 1) ? argv[1] : "addr";
    char ipbuf[16], maskbuf[16], gwbuf[16];
    if (TCPStack::IsUp()) {
        TCPStack::FormatIP(TCPStack::GetIP(), ipbuf);
        TCPStack::FormatIP(TCPStack::GetSubnetMask(), maskbuf);
        TCPStack::FormatIP(TCPStack::GetGateway(), gwbuf);
    } else {
        _scpy(ipbuf, "192.168.1.100", 16); _scpy(maskbuf, "255.255.255.0", 16); _scpy(gwbuf, "192.168.1.1", 16);
    }
    if (_seq(sub, "addr") || _seq(sub, "a")) {
        p = _sa(out, p, mx, "1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 state UNKNOWN\n");
        p = _sa(out, p, mx, "    inet 127.0.0.1/8 scope host lo\n");
        p = _sa(out, p, mx, "2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 state ");
        p = _sa(out, p, mx, E1000::IsDetected() ? (E1000::IsLinkUp() ? "UP" : "DOWN") : "DOWN");
        p = _sac(out, p, mx, '\n');
        if (TCPStack::IsUp()) {
            const uint8_t* mac = TCPStack::GetMAC();
            p = _sa(out, p, mx, "    link/ether ");
            for (int i=0;i<6;i++) { if(i) p=_sac(out,p,mx,':'); p=_sah(out,p,mx,mac[i],2); }
            p = _sa(out, p, mx, " brd ff:ff:ff:ff:ff:ff\n");
        } else {
            p = _sa(out, p, mx, "    link/ether 00:1a:2b:3c:4d:5e brd ff:ff:ff:ff:ff:ff\n");
        }
        p = _sa(out, p, mx, "    inet "); p = _sa(out, p, mx, ipbuf);
        p = _sa(out, p, mx, "/24 scope global eth0\n");
    } else if (_seq(sub, "route") || _seq(sub, "r")) {
        p = _sa(out, p, mx, "default via "); p = _sa(out, p, mx, gwbuf);
        p = _sa(out, p, mx, " dev eth0 proto dhcp metric 100\n");
    } else if (_seq(sub, "link") || _seq(sub, "l")) {
        p = _sa(out, p, mx, "1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 state UNKNOWN mode DEFAULT\n");
        p = _sa(out, p, mx, "    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00\n");
        p = _sa(out, p, mx, "2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 state UP mode DEFAULT\n");
        if (TCPStack::IsUp()) {
            const uint8_t* mac = TCPStack::GetMAC();
            p = _sa(out, p, mx, "    link/ether ");
            for (int i=0;i<6;i++) { if(i) p=_sac(out,p,mx,':'); p=_sah(out,p,mx,mac[i],2); }
            p = _sa(out, p, mx, " brd ff:ff:ff:ff:ff:ff\n");
        } else {
            p = _sa(out, p, mx, "    link/ether 00:1a:2b:3c:4d:5e brd ff:ff:ff:ff:ff:ff\n");
        }
    } else if (_seq(sub, "stats")) {
        if (TCPStack::IsUp()) {
            const NetStats& st = TCPStack::GetStats();
            p = _sa(out, p, mx, "RX: packets="); p = _sau(out, p, mx, st.packets_rx);
            p = _sa(out, p, mx, " bytes="); p = _sau(out, p, mx, st.bytes_rx);
            p = _sa(out, p, mx, " errors="); p = _sau(out, p, mx, st.errors_rx); p = _sac(out, p, mx, '\n');
            p = _sa(out, p, mx, "TX: packets="); p = _sau(out, p, mx, st.packets_tx);
            p = _sa(out, p, mx, " bytes="); p = _sau(out, p, mx, st.bytes_tx);
            p = _sa(out, p, mx, " errors="); p = _sau(out, p, mx, st.errors_tx); p = _sac(out, p, mx, '\n');
        }
    } else {
        p = _sa(out, p, mx, "Usage: ip {addr|route|link|stats}\n");
    }
    return p;
}

int LinuxCmds::cmd_ss(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _sa(out, p, mx, "Netid  State       Recv-Q  Send-Q  Local Address:Port   Peer Address:Port\n");
    if (!TCPStack::IsUp()) {
        p = _sa(out, p, mx, "(tcp/ip stack not initialized)\n");
        return p;
    }
    const NetStats& st = TCPStack::GetStats();
    p = _sa(out, p, mx, "TCP connections: "); p = _sau(out, p, mx, st.tcp_rx + st.tcp_tx);
    p = _sa(out, p, mx, " segments, UDP: "); p = _sau(out, p, mx, st.udp_rx + st.udp_tx);
    p = _sa(out, p, mx, " datagrams\n");
    p = _sa(out, p, mx, "ARP: "); p = _sau(out, p, mx, st.arp_requests);
    p = _sa(out, p, mx, " requests, "); p = _sau(out, p, mx, st.arp_replies);
    p = _sa(out, p, mx, " replies\n");
    p = _sa(out, p, mx, "ICMP: "); p = _sau(out, p, mx, st.icmp_rx);
    p = _sa(out, p, mx, " rx, "); p = _sau(out, p, mx, st.icmp_tx);
    p = _sa(out, p, mx, " tx  Dropped: "); p = _sau(out, p, mx, st.dropped); p = _sac(out, p, mx, '\n');
    return p;
}

int LinuxCmds::cmd_iotop(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    p = _sa(out, p, mx, "Total DISK READ:    0.00 B/s | Total DISK WRITE:    0.00 B/s\n");
    p = _sa(out, p, mx, "  TID  PRIO  USER     DISK READ  DISK WRITE  COMMAND\n");
    p = _sa(out, p, mx, "    1  be/4  root        0.00 B/s    0.00 B/s  kernel\n");
    p = _sa(out, p, mx, "    2  be/4  root        0.00 B/s    0.00 B/s  shell\n");
    p = _sa(out, p, mx, "    3  be/4  root        0.00 B/s    0.00 B/s  desktop\n");
    return p;
}

//  network commands (simulated  -  real nic driver would replace these)

int LinuxCmds::cmd_ifconfig(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh; (void)argc; (void)argv;
    int p = 0;
    char ipbuf[16], maskbuf[16];
    bool have_stack = TCPStack::IsUp();
    if (have_stack) {
        TCPStack::FormatIP(TCPStack::GetIP(), ipbuf);
        TCPStack::FormatIP(TCPStack::GetSubnetMask(), maskbuf);
    } else {
        _scpy(ipbuf, "192.168.1.100", 16); _scpy(maskbuf, "255.255.255.0", 16);
    }
    p = _sa(out, p, mx, "eth0: flags=4163<UP,BROADCAST,RUNNING,MULTICAST>  mtu 1500\n");
    p = _sa(out, p, mx, "      inet "); p = _sa(out, p, mx, ipbuf);
    p = _sa(out, p, mx, "  netmask "); p = _sa(out, p, mx, maskbuf);
    p = _sa(out, p, mx, "\n");
    p = _sa(out, p, mx, "      ether ");
    if (have_stack) {
        const uint8_t* mac = TCPStack::GetMAC();
        for (int i=0;i<6;i++) { if(i) p=_sac(out,p,mx,':'); p=_sah(out,p,mx,mac[i],2); }
    } else {
        p = _sa(out, p, mx, "00:1a:2b:3c:4d:5e");
    }
    p = _sa(out, p, mx, "  txqueuelen 1000\n");
    if (have_stack) {
        const NetStats& st = TCPStack::GetStats();
        p = _sa(out, p, mx, "      RX packets "); p = _sau(out, p, mx, st.packets_rx);
        p = _sa(out, p, mx, "  bytes "); p = _sau(out, p, mx, st.bytes_rx);
        p = _sa(out, p, mx, " errors "); p = _sau(out, p, mx, st.errors_rx); p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "      TX packets "); p = _sau(out, p, mx, st.packets_tx);
        p = _sa(out, p, mx, "  bytes "); p = _sau(out, p, mx, st.bytes_tx);
        p = _sa(out, p, mx, " errors "); p = _sau(out, p, mx, st.errors_tx); p = _sac(out, p, mx, '\n');
    } else {
        p = _sa(out, p, mx, "      RX packets 0  bytes 0 (0.0 B)\n");
        p = _sa(out, p, mx, "      TX packets 0  bytes 0 (0.0 B)\n");
    }
    p = _sac(out, p, mx, '\n');
    p = _sa(out, p, mx, "lo: flags=73<UP,LOOPBACK,RUNNING>  mtu 65536\n");
    p = _sa(out, p, mx, "    inet 127.0.0.1  netmask 255.0.0.0\n");
    return p;
}

int LinuxCmds::cmd_ping(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: ping <host>\n");

    int emit_mark = 0;
    auto flush_emit = [&](int new_p) {
        KuronoShell::EmitIncrementalRange(out, emit_mark, new_p);
        emit_mark = new_p;
    };

    const char* host = argv[1];
    IPv4Address resolved_ip = {{0, 0, 0, 0}};
    if (!Network::Resolve(host, &resolved_ip)) {
        int p = 0;
        p = _sa(out, p, mx, "ping: could not resolve ");
        p = _sa(out, p, mx, host);
        p = _sac(out, p, mx, '\n');
        flush_emit(p);
        return p;
    }
    uint32_t target_ip = TCPStack::MakeIP(resolved_ip.bytes[0], resolved_ip.bytes[1],
                                          resolved_ip.bytes[2], resolved_ip.bytes[3]);

    int p = 0;
    p = _sa(out, p, mx, "PING ");
    p = _sa(out, p, mx, host);
    p = _sa(out, p, mx, " 56(84) bytes of data.\n");
    flush_emit(p);

    if (!TCPStack::IsUp()) {
        p = _sa(out, p, mx, "ping: TCP/IP stack unavailable on this boot\n");
        flush_emit(p);
        return p;
    }

    int sent = 0, recv = 0;
    for (int i = 0; i < 4; i++) {
        if (KuronoShell::IsCommandCancelRequested()) {
            p = _sa(out, p, mx, "\nping: cancelled.\n");
            flush_emit(p);
            return p;
        }
        sent++;
        int rtt_ms = 0;
        bool ok = TCPStack::Ping(target_ip, 1000, &rtt_ms);
        if (KuronoShell::IsCommandCancelRequested()) {
            p = _sa(out, p, mx, "\nping: cancelled.\n");
            flush_emit(p);
            return p;
        }
        if (ok) {
            recv++;
            p = _sa(out, p, mx, "64 bytes from "); p = _sa(out, p, mx, host);
            p = _sa(out, p, mx, ": icmp_seq="); p = _sai(out, p, mx, i + 1);
            p = _sa(out, p, mx, " ttl=64 time="); p = _sai(out, p, mx, rtt_ms);
            p = _sa(out, p, mx, " ms\n");
        } else {
            p = _sa(out, p, mx, "Request timeout for icmp_seq ");
            p = _sai(out, p, mx, i + 1);
            p = _sac(out, p, mx, '\n');
        }
        flush_emit(p);
    }
    p = _sa(out, p, mx, "\n--- "); p = _sa(out, p, mx, host);
    p = _sa(out, p, mx, " ping statistics ---\n");
    p = _sai(out, p, mx, sent); p = _sa(out, p, mx, " packets transmitted, ");
    p = _sai(out, p, mx, recv); p = _sa(out, p, mx, " received, ");
    int loss = sent > 0 ? ((sent - recv) * 100 / sent) : 0;
    p = _sai(out, p, mx, loss); p = _sa(out, p, mx, "% packet loss\n");
    flush_emit(p);
    return p;
}

int LinuxCmds::cmd_wget(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: wget <url>\n");

    char* body = (char*)KernelHeap::Alloc(LINUX_HTTP_BUFFER_MAX + 1);
    if (!body) return _sa(out, 0, mx, "wget: not enough heap for download buffer\n");

    char err[160];
    int status = 0;
    int body_len = 0;
    int p = 0;
    if (!_http_get_plain(argv[1], body, LINUX_HTTP_BUFFER_MAX + 1, &status, &body_len, err, sizeof(err))) {
        p = _sa(out, 0, mx, "wget: ");
        p = _sa(out, p, mx, err[0] ? err : "download failed");
        p = _sac(out, p, mx, '\n');
        KernelHeap::Free(body);
        return p;
    }
    if (status < 200 || status >= 300) {
        p = _sa(out, 0, mx, "wget: HTTP ");
        p = _sai(out, p, mx, status);
        p = _sac(out, p, mx, '\n');
        KernelHeap::Free(body);
        return p;
    }

    char host[128];
    char path[256];
    uint16_t port = 80;
    bool https = false;
    if (!_parse_http_url(argv[1], host, sizeof(host), path, sizeof(path), &port, &https)) {
        KernelHeap::Free(body);
        return _sa(out, 0, mx, "wget: unsupported URL format\n");
    }

    char file_name[64];
    _filename_from_url_path(path, file_name, sizeof(file_name));
    char save_path[128];
    int sp = 0;
    sp = _sa(save_path, sp, sizeof(save_path), "/home/user/Downloads/");
    sp = _sa(save_path, sp, sizeof(save_path), file_name);
    KVFS::Mkdirs("/home/user/Downloads");
    KVFS::WriteFile(save_path, body, (uint32_t)body_len);

    p = _sa(out, 0, mx, "Saved ");
    p = _sai(out, p, mx, body_len);
    p = _sa(out, p, mx, " bytes to ");
    p = _sa(out, p, mx, save_path);
    p = _sac(out, p, mx, '\n');
    KernelHeap::Free(body);
    return p;
}

int LinuxCmds::cmd_curl(KuronoShell* sh, int argc, const char** argv, char* out, int mx) {
    (void)sh;
    if (argc < 2) return _sa(out, 0, mx, "Usage: curl <url>\n");

    char* body = (char*)KernelHeap::Alloc(LINUX_HTTP_BUFFER_MAX + 1);
    if (!body) return _sa(out, 0, mx, "curl: not enough heap for response buffer\n");

    char err[160];
    int status = 0;
    int body_len = 0;
    if (!_http_get_plain(argv[1], body, LINUX_HTTP_BUFFER_MAX + 1, &status, &body_len, err, sizeof(err))) {
        int p = _sa(out, 0, mx, "curl: ");
        p = _sa(out, p, mx, err[0] ? err : "request failed");
        p = _sac(out, p, mx, '\n');
        KernelHeap::Free(body);
        return p;
    }
    if (status < 200 || status >= 300) {
        int p = _sa(out, 0, mx, "curl: HTTP ");
        p = _sai(out, p, mx, status);
        if (body_len > 0) {
            p = _sa(out, p, mx, "\n");
            int copy = body_len;
            if (copy > mx - p - 1) copy = mx - p - 1;
            for (int i = 0; i < copy; i++) out[p++] = body[i];
            out[p] = 0;
        } else {
            p = _sac(out, p, mx, '\n');
        }
        KernelHeap::Free(body);
        return p;
    }

    int p = 0;
    int copy = body_len;
    if (copy > mx - 1) copy = mx - 1;
    for (int i = 0; i < copy; i++) out[p++] = body[i];
    if (copy < body_len && p < mx - 1) {
        out[p++] = '\n';
        const char* trunc = "[curl output truncated]\n";
        for (int i = 0; trunc[i] && p < mx - 1; i++) out[p++] = trunc[i];
    }
    out[p] = 0;
    KernelHeap::Free(body);
    return p;
}

//  linux-exec  -  run a program through real linux syscalls
//  this creates a linux process context, dispatches syscalls, and
//  returns captured console output to the shell.

int LinuxCmds::cmd_linux_exec(KuronoShell* sh, int argc, const char** argv,
                               char* out, int mx) {
    (void)sh;
    if (argc < 2) {
        int p = 0;
        p = _sa(out, p, mx, "Usage: linux-exec <program> [args...]\n");
        p = _sa(out, p, mx, "Runs a program through the Linux syscall ABI layer.\n");
        p = _sa(out, p, mx, "All I/O goes through real int 0x80 syscalls.\n\n");
        p = _sa(out, p, mx, "Available programs:\n");
        p = _sa(out, p, mx, "  hello   - Hello world via sys_write\n");
        p = _sa(out, p, mx, "  uname   - System info via sys_uname\n");
        p = _sa(out, p, mx, "  getpid  - Process ID via sys_getpid\n");
        p = _sa(out, p, mx, "  pwd     - Working directory via sys_getcwd\n");
        p = _sa(out, p, mx, "  ls      - Directory listing via sys_getdents64\n");
        p = _sa(out, p, mx, "  cat <f> - Read file via sys_open/sys_read\n");
        p = _sa(out, p, mx, "  mkdir <d> - Create dir via sys_mkdir\n");
        p = _sa(out, p, mx, "  stat <f> - File info via sys_stat\n");
        p = _sa(out, p, mx, "  sleep <n> - Sleep N seconds via sys_nanosleep\n");
        p = _sa(out, p, mx, "  echo <text> - Echo via sys_write\n");
        p = _sa(out, p, mx, "  write <f> <txt> - Write file via sys_open/sys_write\n");
        p = _sa(out, p, mx, "  id      - User info via sys_getuid/sys_getgid\n");
        return p;
    }

    // shift argv to skip "linux-exec"
    return LinuxSyscall::RunProgram(argv[1], argc - 1, argv + 1, out, mx);
}

//  syscall  -  direct linux syscall test interface
//  lets users call specific syscall numbers directly.

int LinuxCmds::cmd_syscall(KuronoShell* sh, int argc, const char** argv,
                            char* out, int mx) {
    (void)sh;
    if (argc < 2) {
        int p = 0;
        p = _sa(out, p, mx, "Usage: syscall <number> [arg1] [arg2] [arg3]\n");
        p = _sa(out, p, mx, "Common syscalls: 4=write, 20=getpid, 39=mkdir, 122=uname\n");
        p = _sa(out, p, mx, "Example: syscall 20  (returns PID)\n");
        return p;
    }

    uint32_t num = (uint32_t)_atoi(argv[1]);
    uint32_t a1 = (argc > 2) ? (uint32_t)_atoi(argv[2]) : 0;
    uint32_t a2 = (argc > 3) ? (uint32_t)_atoi(argv[3]) : 0;
    uint32_t a3 = (argc > 4) ? (uint32_t)_atoi(argv[4]) : 0;

    // create temp process if none active
    bool created = false;
    if (!LinuxSyscall::Current()) {
        LinuxSyscall::SetCurrent(LinuxSyscall::CreateProcess("syscall_test", 0, 0));
        created = true;
    }

    LinuxSyscall::ClearConsoleOutput();
    int32_t ret = LinuxSyscall::Dispatch(num, a1, a2, a3, 0, 0);

    int p = 0;
    p = _sa(out, p, mx, "syscall(");
    p = _sai(out, p, mx, (int)num);
    p = _sa(out, p, mx, ") = ");
    p = _sai(out, p, mx, ret);
    p = _sac(out, p, mx, '\n');

    // if there's captured console output, append it
    if (LinuxSyscall::HasConsoleOutput()) {
        p = _sa(out, p, mx, "--- output ---\n");
        p += LinuxSyscall::ReadConsoleOutput(out + p, mx - p - 1);
        out[p] = 0;
    }

    if (created) {
        LinuxProcess* cur = LinuxSyscall::Current();
        if (cur) {
            int idx = (int)(cur->pid - 100);
            LinuxSyscall::DestroyProcess(idx);
            LinuxSyscall::SetCurrent(-1);
        }
    }

    return p;
}

//  vm  -  hypervisor / virtual machine management
//  subcommands:
//    vm status     -  show vm/hypervisor state
//    vm create [ram_mb]  -  create a new vm
//    vm run [max_exits]  -  enter the vm run loop
//    vm pause      -  pause the vm
//    vm resume     -  resume the vm
//    vm destroy    -  destroy the vm
//    vm serial     -  read guest serial (com1) output
//    vm regs       -  dump guest registers
//    vm info       -  show vm statistics
//    vm boot-test  -  create+run a minimal guest that prints to serial

// tiny 16-bit real-mode guest code that writes "kurono vm ok\n" to com1
// (port 0x3f8) and then hlts. this serves as a boot-test without needing
// a real linux bzimage.
static const uint8_t tiny_guest_code[] = {
    // mov si, msg
    0xBE, 0x10, 0x00,          // be 10 00       mov si, 0x0010
    // .loop:
    0xAC,                       // ac             lodsb
    0x08, 0xC0,                 // 08 c0          or al, al
    0x74, 0x06,                 // 74 06          jz .done
    0xBA, 0xF8, 0x03,          // ba f8 03       mov dx, 0x3f8
    0xEE,                       // ee             out dx, al
    0xEB, 0xF5,                 // eb f5          jmp .loop
    // .done:
    0xF4,                       // f4             hlt
    // msg: "kurono vm ok\r\n"
    'K','U','R','O','N','O',' ','V','M',' ','O','K','\r','\n',0
};

int LinuxCmds::cmd_vm(KuronoShell* sh, int argc, const char** argv,
                       char* out, int mx) {
    (void)sh;

    if (argc < 2) {
        int p = 0;
        p = _sa(out, p, mx, "Usage: vm <subcommand>\n\n");
        p = _sa(out, p, mx, "Subcommands:\n");
        p = _sa(out, p, mx, "  status       Show hypervisor/VM state\n");
        p = _sa(out, p, mx, "  create [N]   Create VM with N MB RAM (default: 16)\n");
        p = _sa(out, p, mx, "  run [N]      Run VM (max N exits, 0=unlimited)\n");
        p = _sa(out, p, mx, "  pause        Pause VM execution\n");
        p = _sa(out, p, mx, "  resume       Resume VM execution\n");
        p = _sa(out, p, mx, "  destroy      Destroy VM and free resources\n");
        p = _sa(out, p, mx, "  serial       Read guest serial output (COM1)\n");
        p = _sa(out, p, mx, "  regs         Dump guest CPU registers\n");
        p = _sa(out, p, mx, "  info         Show VM statistics\n");
        if (_guest_tools_enabled() && _guest_is_alpine())
            p = _sa(out, p, mx, "  boot-alpine  Boot Alpine Linux + extract drivers\n");
        if (_guest_tools_enabled() && _guest_is_debian())
            p = _sa(out, p, mx, "  boot-debian  Boot embedded Debian minbase guest\n");
        p = _sa(out, p, mx, "  boot-test    Create+run minimal test guest\n");
        return p;
    }

    const char* sub = argv[1];

    if (_seq(sub, "status")) {
        int p = 0;
        p = _sa(out, p, mx, "Hypervisor Status\n");
        p = _sa(out, p, mx, "  Hardware: ");
        if (Hypervisor::IsAvailable()) {
            p = _sa(out, p, mx, "available (");
            p = _sa(out, p, mx, VMM::GetType() == 1 ? "Intel VT-x" : "AMD-V");
            p = _sa(out, p, mx, ")\n");
        } else {
            p = _sa(out, p, mx, "not available\n");
        }
        p = _sa(out, p, mx, "  VM state: ");
        switch (Hypervisor::GetState()) {
            case VM_STATE_UNINITIALIZED: p = _sa(out, p, mx, "UNINITIALIZED"); break;
            case VM_STATE_CREATED:       p = _sa(out, p, mx, "CREATED");       break;
            case VM_STATE_RUNNING:       p = _sa(out, p, mx, "RUNNING");       break;
            case VM_STATE_PAUSED:        p = _sa(out, p, mx, "PAUSED");        break;
            case VM_STATE_HALTED:        p = _sa(out, p, mx, "HALTED");        break;
            case VM_STATE_CRASHED:       p = _sa(out, p, mx, "CRASHED");       break;
            case VM_STATE_REBOOTING:     p = _sa(out, p, mx, "REBOOTING");     break;
            case VM_STATE_DESTROYED:     p = _sa(out, p, mx, "DESTROYED");     break;
        }
        p = _sac(out, p, mx, '\n');
        if (Hypervisor::GetState() >= VM_STATE_CREATED &&
            Hypervisor::GetState() <= VM_STATE_PAUSED) {
            const VMStats& st = Hypervisor::GetStats();
            p = _sa(out, p, mx, "  Exits:  ");
            p = _sau(out, p, mx, st.total_exits);
            p = _sa(out, p, mx, " (I/O: ");
            p = _sau(out, p, mx, st.io_exits);
            p = _sa(out, p, mx, ", MMIO: ");
            p = _sau(out, p, mx, st.mmio_exits);
            p = _sa(out, p, mx, ")\n");
            p = _sa(out, p, mx, "  Serial: TX=");
            p = _sau(out, p, mx, st.serial_bytes_tx);
            p = _sa(out, p, mx, " RX=");
            p = _sau(out, p, mx, st.serial_bytes_rx);
            p = _sac(out, p, mx, '\n');
        }
        return p;
    }

    if (_seq(sub, "create")) {
        if (Hypervisor::GetState() != VM_STATE_UNINITIALIZED &&
            Hypervisor::GetState() != VM_STATE_DESTROYED) {
            return _sa(out, 0, mx, "vm: VM already exists. Use 'vm destroy' first.\n");
        }

        Hypervisor::Init();

        VMConfig cfg;
        cfg.SetDefaults();
        if (argc > 2) cfg.ram_mb = (uint32_t)_atoi(argv[2]);
        if (cfg.ram_mb < 4) cfg.ram_mb = 4;
        if (cfg.ram_mb > 512) cfg.ram_mb = 512;

        if (!Hypervisor::CreateVM(cfg)) {
            return _sa(out, 0, mx, "vm create: failed (hardware virtualization may not be available)\n");
        }

        int p = 0;
        p = _sa(out, p, mx, "VM created: ");
        p = _sau(out, p, mx, cfg.ram_mb);
        p = _sa(out, p, mx, " MB RAM, serial=");
        p = _sa(out, p, mx, cfg.enable_serial ? "on" : "off");
        p = _sa(out, p, mx, ", disk=");
        p = _sa(out, p, mx, cfg.enable_disk ? "on" : "off");
        p = _sac(out, p, mx, '\n');
        return p;
    }

    if (_seq(sub, "run")) {
        if (Hypervisor::GetState() != VM_STATE_CREATED &&
            Hypervisor::GetState() != VM_STATE_PAUSED) {
            return _sa(out, 0, mx, "vm run: no VM ready. Use 'vm create' first.\n");
        }

        uint32_t max_exits = 1000; // safe default
        if (argc > 2) max_exits = (uint32_t)_atoi(argv[2]);
        if (max_exits == 0) max_exits = 10000; // cap for safety

        VMState result = Hypervisor::RunVM(max_exits);

        // read any serial output produced during the run
        int p = 0;
        if (Hypervisor::HasSerialOutput()) {
            p = _sa(out, p, mx, "--- Guest Serial Output ---\n");
            p += Hypervisor::ReadSerialOutput(out + p, mx - p - 64);
            out[p] = 0;
            p = _sa(out, p, mx, "\n--- End Serial Output ---\n");
        }

        p = _sa(out, p, mx, "VM ");
        switch (result) {
            case VM_STATE_HALTED:    p = _sa(out, p, mx, "halted");   break;
            case VM_STATE_CRASHED:   p = _sa(out, p, mx, "crashed");  break;
            case VM_STATE_PAUSED:    p = _sa(out, p, mx, "paused");   break;
            case VM_STATE_REBOOTING: p = _sa(out, p, mx, "rebooting"); break;
            default:                 p = _sa(out, p, mx, "stopped");  break;
        }
        p = _sa(out, p, mx, " after ");
        p = _sau(out, p, mx, Hypervisor::GetStats().total_exits);
        p = _sa(out, p, mx, " exits\n");
        return p;
    }

    if (_seq(sub, "pause")) {
        Hypervisor::PauseVM();
        return _sa(out, 0, mx, "VM paused\n");
    }

    if (_seq(sub, "resume")) {
        Hypervisor::ResumeVM();
        return _sa(out, 0, mx, "VM resumed\n");
    }

    if (_seq(sub, "destroy")) {
        Hypervisor::DestroyVM();
        return _sa(out, 0, mx, "VM destroyed\n");
    }

    if (_seq(sub, "serial")) {
        if (!Hypervisor::HasSerialOutput()) {
            return _sa(out, 0, mx, "(no serial output from guest)\n");
        }
        int p = 0;
        p = _sa(out, p, mx, "--- Guest COM1 ---\n");
        p += Hypervisor::ReadSerialOutput(out + p, mx - p - 32);
        out[p] = 0;
        p = _sa(out, p, mx, "\n--- End ---\n");
        return p;
    }

    if (_seq(sub, "regs")) {
        Hypervisor::DumpGuestRegs();
        return _sa(out, 0, mx, "(register dump sent to serial log)\n");
    }

    if (_seq(sub, "info")) {
        const VMStats& st = Hypervisor::GetStats();
        int p = 0;
        p = _sa(out, p, mx, "VM Statistics:\n");
        p = _sa(out, p, mx, "  Total exits:    "); p = _sau(out, p, mx, st.total_exits);    p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  I/O exits:      "); p = _sau(out, p, mx, st.io_exits);       p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  MMIO exits:     "); p = _sau(out, p, mx, st.mmio_exits);     p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  HLT exits:      "); p = _sau(out, p, mx, st.hlt_exits);      p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  IRQ injections: "); p = _sau(out, p, mx, st.irq_injections); p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  Run cycles:     "); p = _sau(out, p, mx, st.run_cycles);     p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  Serial TX:      "); p = _sau(out, p, mx, st.serial_bytes_tx); p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  Serial RX:      "); p = _sau(out, p, mx, st.serial_bytes_rx); p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  Disk reads:     "); p = _sau(out, p, mx, st.disk_reads);     p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  Disk writes:    "); p = _sau(out, p, mx, st.disk_writes);    p = _sac(out, p, mx, '\n');
        return p;
    }

    // ── vm boot-alpine  -  boot embedded alpine linux with driver extraction
    if (_seq(sub, "boot-alpine")) {
        if (!_guest_tools_enabled() || !_guest_is_alpine()) {
            return _sa(out, 0, mx, "boot-alpine: Alpine guest is not the selected Linux profile. Use Settings > System > Linux.\n");
        }
        int p = 0;

        uint32_t max_exits = 250000;
        if (argc > 2) max_exits = (uint32_t)_atoi(argv[2]);
        if (max_exits == 0) max_exits = 250000;

        p = _sa(out, p, mx, "boot-alpine: Booting Alpine Linux (");
        p = _sau(out, p, mx, max_exits);
        p = _sa(out, p, mx, " max exits)...\n");

        bool ok = Hypervisor::BootAlpineWithExtraction(max_exits);

        if (ok) {
            p = _sa(out, p, mx, "Alpine Linux booted successfully.\n");
        } else {
            p = _sa(out, p, mx, "Alpine Linux boot failed.\n");
            p = _append_alpine_boot_diagnostics(out, p, mx);
        }

        // show boot log
        int loglen = Hypervisor::GetAlpineBootLogLen();
        if (loglen > 0) {
            p = _sa(out, p, mx, "\n--- Alpine Boot Log (");
            p = _sai(out, p, mx, loglen);
            p = _sa(out, p, mx, " bytes) ---\n");
            const char* log = Hypervisor::GetAlpineBootLog();
            int copy = loglen;
            if (p + copy >= mx - 64) copy = mx - p - 64;
            for (int i = 0; i < copy && p < mx - 1; i++) out[p++] = log[i];
            out[p] = 0;
            p = _sa(out, p, mx, "\n--- End Boot Log ---\n");
        }

        // show vm stats
        const VMStats& st = Hypervisor::GetStats();
        p = _sa(out, p, mx, "VM Exits: ");
        p = _sau(out, p, mx, st.total_exits);
        p = _sa(out, p, mx, ", Serial TX: ");
        p = _sau(out, p, mx, st.serial_bytes_tx);
        p = _sa(out, p, mx, ", State: ");
        switch (Hypervisor::GetState()) {
            case VM_STATE_RUNNING: p = _sa(out, p, mx, "RUNNING"); break;
            case VM_STATE_HALTED:  p = _sa(out, p, mx, "HALTED");  break;
            case VM_STATE_CRASHED: p = _sa(out, p, mx, "CRASHED"); break;
            default:               p = _sa(out, p, mx, "OTHER");   break;
        }
        p = _sac(out, p, mx, '\n');
        return p;
    }

    if (_seq(sub, "boot-debian")) {
        if (!_guest_tools_enabled() || !_guest_is_debian()) {
            return _sa(out, 0, mx, "boot-debian: Debian guest is not the selected Linux profile. Use Settings > System > Linux.\n");
        }
        int p = 0;
        if (!debian_rootfs_available()) {
            p = _sa(out, p, mx, "boot-debian: Embedded Debian rootfs not found.\n");
            p = _sa(out, p, mx, "Build it with 'make debian-rootfs' and rebuild Kurono first.\n");
            return p;
        }

        uint32_t max_exits = 250000;
        if (argc > 2) max_exits = (uint32_t)_atoi(argv[2]);
        if (max_exits == 0) max_exits = 250000;

        p = _sa(out, p, mx, "boot-debian: Booting embedded Debian minbase (");
        p = _sau(out, p, mx, max_exits);
        p = _sa(out, p, mx, " max exits)...\n");

        bool ok = Hypervisor::BootDebianWithExtraction(max_exits);
        p = _sa(out, p, mx, ok ? "Debian Linux booted successfully.\n" : "Debian Linux boot failed.\n");

        int loglen = Hypervisor::GetDebianBootLogLen();
        if (loglen > 0) {
            p = _sa(out, p, mx, "\n--- Debian Boot Log (");
            p = _sai(out, p, mx, loglen);
            p = _sa(out, p, mx, " bytes) ---\n");
            const char* log = Hypervisor::GetDebianBootLog();
            int copy = loglen;
            if (p + copy >= mx - 64) copy = mx - p - 64;
            for (int i = 0; i < copy && p < mx - 1; i++) out[p++] = log[i];
            out[p] = 0;
            p = _sa(out, p, mx, "\n--- End Boot Log ---\n");
        }
        return p;
    }

    if (_seq(sub, "boot-test")) {
        int p = 0;

        // initialize hypervisor if needed
        if (Hypervisor::GetState() == VM_STATE_UNINITIALIZED ||
            Hypervisor::GetState() == VM_STATE_DESTROYED) {
            Hypervisor::Init();
        }

        // destroy any existing vm
        if (Hypervisor::GetState() != VM_STATE_UNINITIALIZED) {
            Hypervisor::DestroyVM();
        }

        // create vm with minimal ram
        VMConfig cfg;
        cfg.SetDefaults();
        cfg.ram_mb = 4;
        cfg.enable_disk = false;

        if (!Hypervisor::CreateVM(cfg)) {
            p = _sa(out, p, mx, "boot-test: VM creation failed\n");
            p = _sa(out, p, mx, "  Hardware virtualization may not be available.\n");
            p = _sa(out, p, mx, "  The hypervisor requires Intel VT-x or AMD-V.\n");

            // even without real hw virt, demonstrate the serial bridge
            // by manually writing to the virtual serial device
            p = _sa(out, p, mx, "\n--- Simulated Guest Output (via virtual serial) ---\n");
            VirtualSerial& ser = Hypervisor::GetSerial();
            ser.Init(0x3F8, 4);
            const char* test_msg = "KURONO VM OK (simulated)\r\nGuest booted successfully\r\nLinux version 6.1.0-kurono\r\n";
            while (*test_msg) {
                ser.WritePort(0x3F8, *test_msg);
                test_msg++;
            }
            // read it back through the bridge
            if (ser.HasOutput()) {
                char sbuf[512];
                int sn = ser.ReadOutput(sbuf, 511);
                sbuf[sn] = 0;
                for (int i = 0; i < sn && p < mx - 1; i++) {
                    out[p++] = sbuf[i];
                }
                out[p] = 0;
            }
            p = _sa(out, p, mx, "--- End Simulated Output ---\n");
            return p;
        }

        p = _sa(out, p, mx, "boot-test: VM created (4 MB RAM)\n");

        // load tiny guest code into guest memory at address 0x7c00
        // (this would be loaded by linuxbootloader for real kernels)
        uint8_t* guest_base = GuestMemoryManager::GetLowRAM();
        if (guest_base) {
            // copy tiny guest to 0x7c00 (standard bios boot sector address)
            memcpy(guest_base + 0x7C00, tiny_guest_code, sizeof(tiny_guest_code));
            p = _sa(out, p, mx, "boot-test: Guest code loaded at 0x7C00\n");
        }

        // run with limited exits
        p = _sa(out, p, mx, "boot-test: Running guest (max 1000 exits)...\n");
        VMState result = Hypervisor::RunVM(1000);

        // bridge: read serial output
        if (Hypervisor::HasSerialOutput()) {
            p = _sa(out, p, mx, "--- Guest Serial Output ---\n");
            p += Hypervisor::ReadSerialOutput(out + p, mx - p - 64);
            out[p] = 0;
            p = _sa(out, p, mx, "\n--- End Serial Output ---\n");
        } else {
            p = _sa(out, p, mx, "(no serial output captured)\n");
        }

        p = _sa(out, p, mx, "boot-test: VM ");
        switch (result) {
            case VM_STATE_HALTED:    p = _sa(out, p, mx, "halted (clean exit)");  break;
            case VM_STATE_CRASHED:   p = _sa(out, p, mx, "crashed");              break;
            default:                 p = _sa(out, p, mx, "stopped");              break;
        }
        p = _sac(out, p, mx, '\n');

        // show stats
        const VMStats& st = Hypervisor::GetStats();
        p = _sa(out, p, mx, "  Exits: ");
        p = _sau(out, p, mx, st.total_exits);
        p = _sa(out, p, mx, ", I/O: ");
        p = _sau(out, p, mx, st.io_exits);
        p = _sa(out, p, mx, ", Serial TX: ");
        p = _sau(out, p, mx, st.serial_bytes_tx);
        p = _sac(out, p, mx, '\n');

        return p;
    }

    // unknown subcommand
    int p = _sa(out, 0, mx, "vm: unknown subcommand '");
    p = _sa(out, p, mx, sub);
    p = _sa(out, p, mx, "'. Use 'vm' for help.\n");
    return p;
}

//  cmd_alpine  -  alpine linux guest vm management
//
//  alpine               show alpine status + help
//  alpine boot          boot alpine linux with driver extraction
//  alpine status        show whether alpine is running
//  alpine exec <cmd>    execute command inside alpine via serial
//  alpine serial        read serial output from alpine
//  alpine drivers       show drivers extracted from alpine
//  alpine log           show alpine boot log
//  alpine run [n]       run n more vm cycles on alpine
//  alpine shutdown      destroy alpine vm

int LinuxCmds::cmd_alpine(KuronoShell* sh, int argc, const char** argv,
                           char* out, int mx) {
    (void)sh;

    if (!_guest_tools_enabled()) {
        return _sa(out, 0, mx, "Alpine guest is disabled. Enable Linux Guest Integration in Settings > System > Linux.\n");
    }
    if (!_guest_is_alpine()) {
        return _sa(out, 0, mx, "Alpine guest is not selected. Switch the Linux distro back to Alpine in Settings > System > Linux.\n");
    }

    if (argc < 2) {
        int p = 0;
        p = _sa(out, p, mx, "Alpine Linux Guest VM\n\n");
        p = _sa(out, p, mx, "Status: ");
        if (Hypervisor::IsAlpineBooted()) {
            p = _sa(out, p, mx, "BOOTED");
            if (Hypervisor::GetState() == VM_STATE_RUNNING) {
                p = _sa(out, p, mx, " (running)\n");
            } else {
                p = _sa(out, p, mx, " (stopped)\n");
            }
        } else {
            p = _sa(out, p, mx, "NOT BOOTED\n");
        }
        p = _sa(out, p, mx, "\nCommands:\n");
        p = _sa(out, p, mx, "  alpine boot          Boot Alpine + extract drivers\n");
        p = _sa(out, p, mx, "  alpine status        Show status\n");
        p = _sa(out, p, mx, "  alpine exec <cmd>    Run command in Alpine\n");
        p = _sa(out, p, mx, "  alpine serial        Read serial output\n");
        p = _sa(out, p, mx, "  alpine drivers       List extracted drivers\n");
        p = _sa(out, p, mx, "  alpine log           Show boot log\n");
        p = _sa(out, p, mx, "  alpine run [N]       Run N more VM cycles\n");
        p = _sa(out, p, mx, "  alpine shutdown      Destroy Alpine VM\n");
        return p;
    }

    const char* sub = argv[1];

    if (_seq(sub, "boot")) {
        int p = 0;
        if (Hypervisor::IsAlpineBooted()) {
            return _sa(out, 0, mx, "Alpine is already booted. Use 'alpine shutdown' first.\n");
        }

        p = _sa(out, p, mx, "Booting Alpine Linux...\n");
        p = _sa(out, p, mx, "  Kernel: vmlinuz-virt (");
        p = _sau(out, p, mx, (unsigned int)(alpine_kernel_size() / 1024));
        p = _sa(out, p, mx, " KB)\n");
        p = _sa(out, p, mx, "  Initramfs: initramfs-virt (");
        p = _sau(out, p, mx, (unsigned int)(alpine_initramfs_size() / 1024));
        p = _sa(out, p, mx, " KB)\n");
        p = _sa(out, p, mx, "  RAM: 128 MB\n\n");

        uint32_t max_exits = 250000;
        if (argc > 2) max_exits = (uint32_t)_atoi(argv[2]);
        if (max_exits == 0) max_exits = 250000;

        bool ok = Hypervisor::BootAlpineWithExtraction(max_exits);

        if (ok) {
            p = _sa(out, p, mx, "[OK] Alpine Linux booted.\n");
        } else {
            p = _sa(out, p, mx, "[FAIL] Alpine boot failed.\n");
            p = _append_alpine_boot_diagnostics(out, p, mx);
        }

        // summary
        int loglen = Hypervisor::GetAlpineBootLogLen();
        p = _sa(out, p, mx, "Boot log: ");
        p = _sai(out, p, mx, loglen);
        p = _sa(out, p, mx, " bytes captured\n");

        const VMStats& st = Hypervisor::GetStats();
        p = _sa(out, p, mx, "VM exits: ");
        p = _sau(out, p, mx, st.total_exits);
        p = _sa(out, p, mx, ", Serial output: ");
        p = _sau(out, p, mx, st.serial_bytes_tx);
        p = _sa(out, p, mx, " bytes\n");

        return p;
    }

    if (_seq(sub, "status")) {
        int p = 0;
        p = _sa(out, p, mx, "Alpine Linux VM Status\n");
        p = _sa(out, p, mx, "  Booted:   ");
        p = _sa(out, p, mx, Hypervisor::IsAlpineBooted() ? "yes" : "no");
        p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  VM state: ");
        switch (Hypervisor::GetState()) {
            case VM_STATE_UNINITIALIZED: p = _sa(out, p, mx, "UNINITIALIZED"); break;
            case VM_STATE_CREATED:       p = _sa(out, p, mx, "CREATED");       break;
            case VM_STATE_RUNNING:       p = _sa(out, p, mx, "RUNNING");       break;
            case VM_STATE_PAUSED:        p = _sa(out, p, mx, "PAUSED");        break;
            case VM_STATE_HALTED:        p = _sa(out, p, mx, "HALTED");        break;
            case VM_STATE_CRASHED:       p = _sa(out, p, mx, "CRASHED");       break;
            default:                     p = _sa(out, p, mx, "OTHER");         break;
        }
        p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "  Boot log: ");
        p = _sai(out, p, mx, Hypervisor::GetAlpineBootLogLen());
        p = _sa(out, p, mx, " bytes\n");
        if (Hypervisor::IsAlpineBooted()) {
            const VMStats& st = Hypervisor::GetStats();
            p = _sa(out, p, mx, "  Exits:    ");
            p = _sau(out, p, mx, st.total_exits);
            p = _sac(out, p, mx, '\n');
            p = _sa(out, p, mx, "  Serial:   TX=");
            p = _sau(out, p, mx, st.serial_bytes_tx);
            p = _sa(out, p, mx, " RX=");
            p = _sau(out, p, mx, st.serial_bytes_rx);
            p = _sac(out, p, mx, '\n');
        }
        return p;
    }

    if (_seq(sub, "exec")) {
        if (!Hypervisor::IsAlpineBooted() ||
            Hypervisor::GetState() != VM_STATE_RUNNING) {
            return _sa(out, 0, mx, "Alpine VM not running. Use 'alpine boot' first.\n");
        }
        if (argc < 3) {
            return _sa(out, 0, mx, "Usage: alpine exec <command>\n");
        }

        // reconstruct command from argv[2..]
        char cmd_buf[512];
        int ci = 0;
        for (int a = 2; a < argc && ci < 500; a++) {
            if (a > 2) cmd_buf[ci++] = ' ';
            const char* w = argv[a];
            while (*w && ci < 500) cmd_buf[ci++] = *w++;
        }
        cmd_buf[ci] = 0;

        int p = 0;
        p = _sa(out, p, mx, ">>> ");
        p = _sa(out, p, mx, cmd_buf);
        p = _sac(out, p, mx, '\n');

        char result[4096];
        int n = Hypervisor::AlpineExec(cmd_buf, result, (int)sizeof(result) - 1);
        if (n > 0) {
            result[n] = 0;
            int copy = n;
            if (p + copy >= mx - 32) copy = mx - p - 32;
            for (int i = 0; i < copy && p < mx - 1; i++) out[p++] = result[i];
            out[p] = 0;
            if (result[n-1] != '\n') p = _sac(out, p, mx, '\n');
        } else {
            p = _sa(out, p, mx, "(no output)\n");
        }
        return p;
    }

    if (_seq(sub, "serial")) {
        if (!Hypervisor::HasSerialOutput()) {
            return _sa(out, 0, mx, "(no Alpine serial output)\n");
        }
        int p = 0;
        p = _sa(out, p, mx, "--- Alpine COM1 ---\n");
        p += Hypervisor::ReadSerialOutput(out + p, mx - p - 32);
        out[p] = 0;
        p = _sa(out, p, mx, "\n--- End ---\n");
        return p;
    }

    if (_seq(sub, "drivers")) {
        int p = 0;
        p = _sa(out, p, mx, "Drivers extracted from Alpine Linux:\n\n");
        p = _sa(out, p, mx, "NAME                 STATUS  CATEGORY    DESCRIPTION\n");

        LinuxDriver* drivers = LinuxDriverFramework::GetDrivers();
        int count = LinuxDriverFramework::GetDriverCount();
        int alpine_count = 0;

        for (int i = 0; i < count && p < mx - 128; i++) {
            // check if this driver came from alpine (version starts with "alp")
            if (drivers[i].version[0] == 'A' &&
                drivers[i].version[1] == 'L' &&
                drivers[i].version[2] == 'P') {

                alpine_count++;

                // name (padded to 20)
                int nl = _slen(drivers[i].name);
                p = _sa(out, p, mx, drivers[i].name);
                for (int pad = nl; pad < 21; pad++) p = _sac(out, p, mx, ' ');

                // status
                if (drivers[i].state == LDRV_ACTIVE) {
                    p = _sa(out, p, mx, "active  ");
                } else {
                    p = _sa(out, p, mx, "loaded  ");
                }

                // category
                switch (drivers[i].category) {
                    case LDRV_CAT_CHAR:    p = _sa(out, p, mx, "char        "); break;
                    case LDRV_CAT_BLOCK:   p = _sa(out, p, mx, "block       "); break;
                    case LDRV_CAT_NET:     p = _sa(out, p, mx, "net         "); break;
                    case LDRV_CAT_GPU:     p = _sa(out, p, mx, "gpu         "); break;
                    case LDRV_CAT_SOUND:   p = _sa(out, p, mx, "sound       "); break;
                    case LDRV_CAT_INPUT:   p = _sa(out, p, mx, "input       "); break;
                    case LDRV_CAT_BUS:     p = _sa(out, p, mx, "bus         "); break;
                    case LDRV_CAT_FS:      p = _sa(out, p, mx, "filesystem  "); break;
                    case LDRV_CAT_POWER:   p = _sa(out, p, mx, "power       "); break;
                    default:               p = _sa(out, p, mx, "other       "); break;
                }

                // description
                p = _sa(out, p, mx, drivers[i].description);
                p = _sac(out, p, mx, '\n');
            }
        }

        if (alpine_count == 0) {
            p = _sa(out, p, mx, "(none  -  boot Alpine first with 'alpine boot')\n");
        } else {
            p = _sac(out, p, mx, '\n');
            p = _sai(out, p, mx, alpine_count);
            p = _sa(out, p, mx, " Alpine driver(s) registered in Kurono\n");
        }
        return p;
    }

    if (_seq(sub, "log")) {
        int loglen = Hypervisor::GetAlpineBootLogLen();
        if (loglen == 0) {
            return _sa(out, 0, mx, "(no boot log  -  boot Alpine first)\n");
        }
        int p = 0;
        p = _sa(out, p, mx, "--- Alpine Boot Log (");
        p = _sai(out, p, mx, loglen);
        p = _sa(out, p, mx, " bytes) ---\n");

        const char* log = Hypervisor::GetAlpineBootLog();
        int copy = loglen;
        if (p + copy >= mx - 32) copy = mx - p - 32;
        for (int i = 0; i < copy && p < mx - 1; i++) out[p++] = log[i];
        out[p] = 0;
        p = _sa(out, p, mx, "\n--- End ---\n");
        return p;
    }

    if (_seq(sub, "run")) {
        if (!Hypervisor::IsAlpineBooted()) {
            return _sa(out, 0, mx, "Alpine not booted. Use 'alpine boot' first.\n");
        }
        uint32_t cycles = 10000;
        if (argc > 2) cycles = (uint32_t)_atoi(argv[2]);
        if (cycles == 0) cycles = 10000;

        VMState st = Hypervisor::RunAlpineCycles(cycles);

        int p = 0;
        p = _sa(out, p, mx, "Ran ");
        p = _sau(out, p, mx, cycles);
        p = _sa(out, p, mx, " cycles. VM state: ");
        switch (st) {
            case VM_STATE_RUNNING: p = _sa(out, p, mx, "RUNNING"); break;
            case VM_STATE_HALTED:  p = _sa(out, p, mx, "HALTED");  break;
            case VM_STATE_CRASHED: p = _sa(out, p, mx, "CRASHED"); break;
            default:               p = _sa(out, p, mx, "OTHER");   break;
        }
        p = _sac(out, p, mx, '\n');

        // show any new serial output
        if (Hypervisor::HasSerialOutput()) {
            p = _sa(out, p, mx, "--- New Serial Output ---\n");
            p += Hypervisor::ReadSerialOutput(out + p, mx - p - 32);
            out[p] = 0;
            p = _sa(out, p, mx, "\n--- End ---\n");
        }
        return p;
    }

    if (_seq(sub, "shutdown")) {
        if (!Hypervisor::IsAlpineBooted()) {
            return _sa(out, 0, mx, "Alpine not booted.\n");
        }
        Hypervisor::DestroyVM();
        return _sa(out, 0, mx, "Alpine VM destroyed.\n");
    }

    int p = _sa(out, 0, mx, "alpine: unknown subcommand '");
    p = _sa(out, p, mx, sub);
    p = _sa(out, p, mx, "'. Use 'alpine' for help.\n");
    return p;
}

//  cmd_apk  -  alpine package manager interface
//
//  forwards apk commands to the alpine guest via serial.
//  usage:
//    apk list           list installed packages
//    apk add <pkg>      install a package
//    apk del <pkg>      remove a package
//    apk update         update package index
//    apk info <pkg>     show package information
//    apk search <term>  search for packages

int LinuxCmds::cmd_apk(KuronoShell* sh, int argc, const char** argv,
                        char* out, int mx) {
    (void)sh;

    if (!_guest_tools_enabled()) {
        return _sa(out, 0, mx, "apk: Linux guest integration is disabled.\n");
    }
    if (!_guest_is_alpine()) {
        return _sa(out, 0, mx, "apk: Alpine is not the selected guest profile. Switch back in Settings > System > Linux.\n");
    }

    if (!Hypervisor::IsAlpineBooted() ||
        Hypervisor::GetState() != VM_STATE_RUNNING) {
        return _sa(out, 0, mx, "Alpine VM not running. Use 'alpine boot' first.\n");
    }

    if (argc < 2) {
        int p = 0;
        p = _sa(out, p, mx, "Alpine Package Manager (via guest serial)\n\n");
        p = _sa(out, p, mx, "Usage:\n");
        p = _sa(out, p, mx, "  apk list           List installed packages\n");
        p = _sa(out, p, mx, "  apk add <pkg>      Install package\n");
        p = _sa(out, p, mx, "  apk del <pkg>      Remove package\n");
        p = _sa(out, p, mx, "  apk update         Update package index\n");
        p = _sa(out, p, mx, "  apk info <pkg>     Package information\n");
        p = _sa(out, p, mx, "  apk search <term>  Search for packages\n");
        return p;
    }

    // reconstruct the full "apk ..." command
    char cmd_buf[512];
    int ci = 0;
    // start with "apk "
    const char* apk_prefix = "apk ";
    while (*apk_prefix && ci < 500) cmd_buf[ci++] = *apk_prefix++;
    // append all args after "apk" itself
    for (int a = 1; a < argc && ci < 500; a++) {
        if (a > 1) cmd_buf[ci++] = ' ';
        const char* w = argv[a];
        while (*w && ci < 500) cmd_buf[ci++] = *w++;
    }
    cmd_buf[ci] = 0;

    int p = 0;
    p = _sa(out, p, mx, ">>> ");
    p = _sa(out, p, mx, cmd_buf);
    p = _sac(out, p, mx, '\n');

    char result[4096];
    int n = Hypervisor::AlpineExec(cmd_buf, result, (int)sizeof(result) - 1);
    if (n > 0) {
        result[n] = 0;
        int copy = n;
        if (p + copy >= mx - 32) copy = mx - p - 32;
        for (int i = 0; i < copy && p < mx - 1; i++) out[p++] = result[i];
        out[p] = 0;
        if (result[n-1] != '\n') p = _sac(out, p, mx, '\n');
    } else {
        p = _sa(out, p, mx, "(no output from apk  -  guest may still be processing)\n");
    }
    return p;
}

int LinuxCmds::cmd_debian(KuronoShell* sh, int argc, const char** argv,
                           char* out, int mx) {
    (void)sh;

    if (!_guest_tools_enabled()) {
        return _sa(out, 0, mx, "Debian guest is disabled. Enable Linux Guest Integration in Settings > System > Linux.\n");
    }
    if (!_guest_is_debian()) {
        return _sa(out, 0, mx, "Debian guest is not selected. Switch the Linux distro to Debian in Settings > System > Linux.\n");
    }

    const char* sub = argc > 1 ? argv[1] : "status";
    int p = 0;
    if (_seq(sub, "status")) {
        p = _sa(out, p, mx, "Debian Linux Guest VM\n\n");
        p = _sa(out, p, mx, "Rootfs: ");
        p = _sa(out, p, mx, debian_rootfs_available() ? "embedded\n" : "missing\n");
        p = _sa(out, p, mx, "Booted: ");
        p = _sa(out, p, mx, Hypervisor::IsDebianBooted() ? "yes\n" : "no\n");
        p = _sa(out, p, mx, "State:  ");
        switch (Hypervisor::GetState()) {
            case VM_STATE_UNINITIALIZED: p = _sa(out, p, mx, "UNINITIALIZED"); break;
            case VM_STATE_CREATED:       p = _sa(out, p, mx, "CREATED");       break;
            case VM_STATE_RUNNING:       p = _sa(out, p, mx, "RUNNING");       break;
            case VM_STATE_PAUSED:        p = _sa(out, p, mx, "PAUSED");        break;
            case VM_STATE_HALTED:        p = _sa(out, p, mx, "HALTED");        break;
            case VM_STATE_CRASHED:       p = _sa(out, p, mx, "CRASHED");       break;
            case VM_STATE_REBOOTING:     p = _sa(out, p, mx, "REBOOTING");     break;
            case VM_STATE_DESTROYED:     p = _sa(out, p, mx, "DESTROYED");     break;
        }
        p = _sac(out, p, mx, '\n');
        p = _sa(out, p, mx, "Log:    ");
        p = _sai(out, p, mx, Hypervisor::GetDebianBootLogLen());
        p = _sa(out, p, mx, " bytes\n\n");
        p = _sa(out, p, mx, "Commands:\n");
        p = _sa(out, p, mx, "  debian boot         Boot Debian minbase\n");
        p = _sa(out, p, mx, "  debian exec <cmd>   Run command in Debian\n");
        p = _sa(out, p, mx, "  debian log          Show boot log\n");
        p = _sa(out, p, mx, "  debian run [N]      Run more VM cycles\n");
        p = _sa(out, p, mx, "  debian serial       Read live serial output\n");
        p = _sa(out, p, mx, "  debian shutdown     Destroy Debian VM\n");
        return p;
    }
    if (_seq(sub, "plan") || _seq(sub, "rootfs")) {
        p = _sa(out, p, mx, "Debian minbase rootfs plan\n\n");
        p = _sa(out, p, mx, "1. Run 'make debian-rootfs' on the host to build Debian/debian-minbase.ext4\n");
        p = _sa(out, p, mx, "2. Rebuild Kurono so the ext4 image is linked into the kernel\n");
        p = _sa(out, p, mx, "3. Boot with 'debian boot' or 'vm boot-debian'\n");
        p = _sa(out, p, mx, "4. Use apt through the guest serial bridge after boot\n");
        return p;
    }
    if (_seq(sub, "boot")) {
        if (!debian_rootfs_available()) {
            p = _sa(out, p, mx, "Debian rootfs is not embedded yet.\n");
            p = _sa(out, p, mx, "Run 'debian plan' for the host build workflow.\n");
            return p;
        }
        uint32_t max_exits = 250000;
        if (argc > 2) max_exits = (uint32_t)_atoi(argv[2]);
        if (max_exits == 0) max_exits = 250000;
        p = _sa(out, p, mx, "Booting Debian Linux...\n");
        bool ok = Hypervisor::BootDebianWithExtraction(max_exits);
        p = _sa(out, p, mx, ok ? "[OK] Debian Linux booted.\n" : "[FAIL] Debian boot failed.\n");
        p = _sa(out, p, mx, "Boot log: ");
        p = _sai(out, p, mx, Hypervisor::GetDebianBootLogLen());
        p = _sa(out, p, mx, " bytes captured\n");
        return p;
    }
    if (_seq(sub, "exec")) {
        if (!Hypervisor::IsDebianBooted() || Hypervisor::GetState() != VM_STATE_RUNNING) {
            return _sa(out, 0, mx, "Debian VM not running. Use 'debian boot' first.\n");
        }
        if (argc < 3) {
            return _sa(out, 0, mx, "Usage: debian exec <command>\n");
        }

        char cmd_buf[512];
        int ci = 0;
        for (int a = 2; a < argc && ci < 500; a++) {
            if (a > 2) cmd_buf[ci++] = ' ';
            const char* w = argv[a];
            while (*w && ci < 500) cmd_buf[ci++] = *w++;
        }
        cmd_buf[ci] = 0;

        p = _sa(out, p, mx, ">>> ");
        p = _sa(out, p, mx, cmd_buf);
        p = _sac(out, p, mx, '\n');

        char result[4096];
        int n = Hypervisor::DebianExec(cmd_buf, result, (int)sizeof(result) - 1);
        if (n > 0) {
            result[n] = 0;
            int copy = n;
            if (p + copy >= mx - 32) copy = mx - p - 32;
            for (int i = 0; i < copy && p < mx - 1; i++) out[p++] = result[i];
            out[p] = 0;
            if (result[n - 1] != '\n') p = _sac(out, p, mx, '\n');
        } else {
            p = _sa(out, p, mx, "(no output)\n");
        }
        return p;
    }
    if (_seq(sub, "serial")) {
        if (!Hypervisor::HasSerialOutput()) {
            return _sa(out, 0, mx, "(no Debian serial output)\n");
        }
        p = _sa(out, p, mx, "--- Debian COM1 ---\n");
        p += Hypervisor::ReadSerialOutput(out + p, mx - p - 32);
        out[p] = 0;
        p = _sa(out, p, mx, "\n--- End ---\n");
        return p;
    }
    if (_seq(sub, "log")) {
        int loglen = Hypervisor::GetDebianBootLogLen();
        if (loglen == 0) {
            return _sa(out, 0, mx, "(no boot log  -  boot Debian first)\n");
        }
        p = _sa(out, p, mx, "--- Debian Boot Log (");
        p = _sai(out, p, mx, loglen);
        p = _sa(out, p, mx, " bytes) ---\n");
        const char* log = Hypervisor::GetDebianBootLog();
        int copy = loglen;
        if (p + copy >= mx - 32) copy = mx - p - 32;
        for (int i = 0; i < copy && p < mx - 1; i++) out[p++] = log[i];
        out[p] = 0;
        p = _sa(out, p, mx, "\n--- End ---\n");
        return p;
    }
    if (_seq(sub, "run")) {
        if (!Hypervisor::IsDebianBooted()) {
            return _sa(out, 0, mx, "Debian not booted. Use 'debian boot' first.\n");
        }
        uint32_t cycles = 10000;
        if (argc > 2) cycles = (uint32_t)_atoi(argv[2]);
        if (cycles == 0) cycles = 10000;

        VMState st = Hypervisor::RunDebianCycles(cycles);
        p = _sa(out, p, mx, "Ran ");
        p = _sau(out, p, mx, cycles);
        p = _sa(out, p, mx, " cycles. VM state: ");
        switch (st) {
            case VM_STATE_RUNNING: p = _sa(out, p, mx, "RUNNING"); break;
            case VM_STATE_HALTED:  p = _sa(out, p, mx, "HALTED");  break;
            case VM_STATE_CRASHED: p = _sa(out, p, mx, "CRASHED"); break;
            default:               p = _sa(out, p, mx, "OTHER");   break;
        }
        p = _sac(out, p, mx, '\n');
        if (Hypervisor::HasSerialOutput()) {
            p = _sa(out, p, mx, "--- New Serial Output ---\n");
            p += Hypervisor::ReadSerialOutput(out + p, mx - p - 32);
            out[p] = 0;
            p = _sa(out, p, mx, "\n--- End ---\n");
        }
        return p;
    }
    if (_seq(sub, "shutdown")) {
        if (Hypervisor::GetState() != VM_STATE_UNINITIALIZED && Hypervisor::GetState() != VM_STATE_DESTROYED) {
            Hypervisor::DestroyVM();
            return _sa(out, 0, mx, "Debian guest VM state cleared.\n");
        }
        return _sa(out, 0, mx, "Debian guest is not running.\n");
    }

    p = _sa(out, p, mx, "debian: unknown subcommand '");
    p = _sa(out, p, mx, sub);
    p = _sa(out, p, mx, "'. Use 'debian status' for help.\n");
    return p;
}

int LinuxCmds::cmd_apt(KuronoShell* sh, int argc, const char** argv,
                       char* out, int mx) {
    (void)sh;
    if (!_guest_tools_enabled()) {
        return _sa(out, 0, mx, "apt: Linux guest integration is disabled.\n");
    }
    if (!_guest_is_debian()) {
        return _sa(out, 0, mx, "apt: Debian is not the selected guest profile. Switch it in Settings > System > Linux.\n");
    }
    if (!debian_rootfs_available()) {
        return _sa(out, 0, mx, "apt: Debian rootfs is not embedded yet. Build it first with 'make debian-rootfs'.\n");
    }
    if (!Hypervisor::IsDebianBooted() || Hypervisor::GetState() != VM_STATE_RUNNING) {
        return _sa(out, 0, mx, "apt: Debian VM not running. Use 'debian boot' first.\n");
    }
    if (argc < 2) {
        int p = 0;
        p = _sa(out, p, mx, "Debian Package Manager (via guest serial)\n\n");
        p = _sa(out, p, mx, "Usage: apt update | apt install <pkg> | apt remove <pkg> | apt search <term>\n");
        return p;
    }

    char cmd_buf[512];
    int ci = 0;
    const char* prefix = "apt ";
    while (*prefix && ci < 500) cmd_buf[ci++] = *prefix++;
    for (int a = 1; a < argc && ci < 500; a++) {
        if (a > 1) cmd_buf[ci++] = ' ';
        const char* w = argv[a];
        while (*w && ci < 500) cmd_buf[ci++] = *w++;
    }
    cmd_buf[ci] = 0;

    int p = 0;
    p = _sa(out, p, mx, ">>> ");
    p = _sa(out, p, mx, cmd_buf);
    p = _sac(out, p, mx, '\n');

    char result[4096];
    int n = Hypervisor::DebianExec(cmd_buf, result, (int)sizeof(result) - 1);
    if (n > 0) {
        result[n] = 0;
        int copy = n;
        if (p + copy >= mx - 32) copy = mx - p - 32;
        for (int i = 0; i < copy && p < mx - 1; i++) out[p++] = result[i];
        out[p] = 0;
        if (result[n - 1] != '\n') p = _sac(out, p, mx, '\n');
    } else {
        p = _sa(out, p, mx, "(no output from apt)\n");
    }
    return p;
}