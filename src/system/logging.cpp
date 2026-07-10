#include "logging.h"
#include "kpaths.h"
#include "../fs/kvfs.h"

//  kurono runtime logging - lightweight, no daemon. one canonical root
//  (/kurono/var/log, see kpaths.h); the old triple-homing across /system/logs,
//  /kurono/logs and /var/log is gone. (satoru)

namespace {
    static bool g_fs_ready = false;

    // serial mirror staging: EVERY serial line lands here first (any context),
    // and only the LoggingProcess moves it into kvfs. 32k of headroom; when full
    // the mirror drops lines (the real serial console always has everything). (satoru)
    static char g_serial_pending[32768];
    static int  g_serial_pending_len = 0;

    static char g_boot_pending[8192];
    static int  g_boot_pending_len = 0;

    static char g_system_pending[8192];
    static int  g_system_pending_len = 0;

    static int str_len(const char* s) {
        int n = 0;
        if (!s) return 0;
        while (s[n]) n++;
        return n;
    }

    static void str_cpy(char* dst, const char* src, int max_len) {
        if (!dst || max_len < 1) return;
        int i = 0;
        if (src) {
            while (src[i] && i < max_len - 1) { dst[i] = src[i]; i++; }
        }
        dst[i] = 0;
    }

    static void str_cat(char* dst, const char* src, int max_len) {
        if (!dst || max_len < 1) return;
        int n = str_len(dst);
        int i = 0;
        if (src) {
            while (src[i] && n < max_len - 1) { dst[n++] = src[i++]; }
        }
        dst[n] = 0;
    }

    static void int_to_str(int value, char* out, int out_len) {
        if (!out || out_len < 2) return;
        if (value < 0) { out[0] = '-'; int_to_str(-value, out + 1, out_len - 1); return; }
        char tmp[16];
        int len = 0;
        do { tmp[len++] = (char)('0' + (value % 10)); value /= 10; } while (value && len < (int)sizeof(tmp));
        int i = 0;
        while (len > 0 && i < out_len - 1) out[i++] = tmp[--len];
        out[i] = 0;
    }

    static bool is_safe_log_char(char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.';
    }

    static void sanitize_log_name(char* dst, const char* src, int max_len) {
        if (!dst || max_len < 2) return;
        int out = 0;
        if (src) {
            for (int i = 0; src[i] && out < max_len - 1; i++)
                dst[out++] = is_safe_log_char(src[i]) ? src[i] : '_';
        }
        if (out == 0) { str_cpy(dst, "process", max_len); return; }
        dst[out] = 0;
    }

    static void append_buffer(char* buffer, int& len, int cap, const char* text) {
        if (!buffer || cap <= 1 || !text) return;
        for (int i = 0; text[i] && len < cap - 1; i++) buffer[len++] = text[i];
        buffer[len] = 0;
    }

    static void ensure_file(const char* path) {
        if (!KVFS::Exists(path)) KVFS::CreateFile(path);
    }

    static void append_file_text(const char* path, const char* text) {
        if (!path || !text || !*text) return;
        ensure_file(path);
        KVFS::AppendFile(path, text, (uint32_t)str_len(text));
    }

    static void format_line(char* out, int out_len, const char* prefix, const char* message, const char* detail) {
        if (!out || out_len < 2) return;
        out[0] = 0;
        if (prefix && *prefix) str_cat(out, prefix, out_len);
        if (message && *message) str_cat(out, message, out_len);
        if (detail && *detail) { str_cat(out, ": ", out_len); str_cat(out, detail, out_len); }
        str_cat(out, "\n", out_len);
    }

    //  create the single canonical log tree ONCE (the dirs/files persist, so a
    //  per-line storm of Mkdirs would needlessly take the vfs lock). (satoru)
    static void ensure_core_layout() {
        static bool done = false;
        if (done) return;
        done = true;
        KVFS::Mkdirs(KP_LOG_DIR);
        KVFS::Mkdirs(KP_LOG_CRASH_DIR);
        KVFS::Mkdirs(KP_LOG_APPS_DIR);
        KVFS::Mkdirs(KP_LOG_PROC_DIR);
        ensure_file(KP_LOG_SERIAL);
        ensure_file(KP_LOG_SYSTEM);
        ensure_file(KP_LOG_BOOT);
        ensure_file(KP_LOG_NETWORK);
        ensure_file(KP_LOG_SECURITY);
    }

    //  /kurono/var/log/<dir>/<pid>_<name>.log (satoru)
    static void build_sub_log_path(char* out, int out_len, const char* dir, const char* name, int pid) {
        char safe_name[64];
        char pid_text[16];
        sanitize_log_name(safe_name, name, sizeof(safe_name));
        int_to_str(pid, pid_text, sizeof(pid_text));
        out[0] = 0;
        str_cat(out, dir, out_len);
        str_cat(out, "/", out_len);
        str_cat(out, pid_text, out_len);
        str_cat(out, "_", out_len);
        str_cat(out, safe_name, out_len);
        str_cat(out, ".log", out_len);
    }

    static void flush_pending_logs() {
        if (!g_fs_ready) return;
        append_file_text(KP_LOG_SERIAL, g_serial_pending);
        append_file_text(KP_LOG_BOOT,   g_boot_pending);
        append_file_text(KP_LOG_SYSTEM, g_system_pending);
        g_serial_pending_len = 0; g_serial_pending[0] = 0;
        g_boot_pending_len   = 0; g_boot_pending[0]   = 0;
        g_system_pending_len = 0; g_system_pending[0] = 0;
    }
}

// ── cross-core log guard (smp thread dispatch) ──────────────────────────────
// every serial line mirrors into kvfs (append_file_text -> kvfs node realloc):
// with multiple cores logging concurrently that ran UNSYNCHRONIZED kernel-heap
// / kvfs-tree mutation - kernel structs (incl. Process/user_frame) got clobbered
// and resumed user threads crashed on garbled registers. owner-recursive so a
// log emitted from inside a locked section on the same cpu (kvfs warning paths)
// flows through instead of self-deadlocking. (satoru)
#include "../proc/smp.h"
static volatile uint32_t g_rtlog_word  = 0;
static volatile int      g_rtlog_owner = -1;
struct RtLogGuard {
    bool nested;
    RtLogGuard() {
        int cpu = (int)SMP::CpuIndex();
        if (g_rtlog_owner == cpu) { nested = true; return; }
        nested = false;
        for (;;) {
            uint32_t expected = 0;
            if (__atomic_compare_exchange_n(&g_rtlog_word, &expected, 1u, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) break;
            do { __asm__ __volatile__("pause" ::: "memory"); }
            while (__atomic_load_n(&g_rtlog_word, __ATOMIC_RELAXED) != 0);
        }
        g_rtlog_owner = cpu;
    }
    ~RtLogGuard() {
        if (nested) return;
        g_rtlog_owner = -1;
        __atomic_store_n(&g_rtlog_word, 0u, __ATOMIC_RELEASE);
    }
};

void RuntimeLog::InitFilesystem() {
    if (g_fs_ready) return;
    ensure_core_layout();
    KVFS::WriteString(KP_LOG_DIR "/README.txt",
        "Kurono system logs (minimal, no daemon)\n"
        "  boot.log     - boot milestones\n"
        "  system.log   - general runtime events\n"
        "  serial.log   - mirrored serial/driver/kernel console\n"
        "  network.log  - connect/disconnect/link/errors\n"
        "  security.log - supr escalations, ksa prompts, grants/denials\n"
        "  crash/       - kernel panics + minidumps\n"
        "  apps/<app>.log         - per-app activity\n"
        "  processes/<pid>_<name>.log - per-process lifecycle\n");
    g_fs_ready = true;
    flush_pending_logs();
    RuntimeLog::LogSystem("logging", "log layout initialized at " KP_LOG_DIR);
    RuntimeLog::LogBoot("boot log online");
}

void RuntimeLog::MirrorSerial(const char* text) {
    RtLogGuard _rtg;   // cross-core kvfs/log serialization (satoru)
    if (!text || !*text) return;
    // ALWAYS defer - never touch kvfs/heap from here. this used to append into
    // kvfs synchronously, which runs from ANY context (timer irq, #pf/#gp dump
    // paths, even heap-corruption warnings inside a heap op). every lock on
    // that path (RtLogGuard, g_vfs_lock, the heap guard) is cpu-owner-RECURSIVE,
    // so an exception on the owning cpu re-entered a HALF-MUTATED kvfs tree /
    // heap freelist and wrote log text over live blocks - the recurring
    // corruption whose frames are full of "serial.log"/"/kurono/" path bytes,
    // and the #pf-dump -> nested-fault -> triple-fault cascades. the pending
    // ring is plain static memory: safe from every context. the LoggingProcess
    // kernel process flushes it into kvfs from process context every 500ms
    // (FlushSerialMirror). when the ring is full the MIRROR drops lines - the
    // real serial console still carries everything. (satoru)
    if (g_serial_pending_len >= (int)sizeof(g_serial_pending) - 1) return;
    append_buffer(g_serial_pending, g_serial_pending_len, sizeof(g_serial_pending), text);
}

void RuntimeLog::FlushSerialMirror() {
    if (!g_fs_ready) return;
    // stage the pending text out under the log lock, then write to kvfs with
    // the lock RELEASED - an exception mid-append then finds no half-held log
    // state to recurse into (its MirrorSerial just stages into the ring). (satoru)
    static char staged[sizeof(g_serial_pending)];
    int n;
    {
        RtLogGuard _rtg;
        n = g_serial_pending_len;
        if (n <= 0) return;
        for (int i = 0; i < n; i++) staged[i] = g_serial_pending[i];
        staged[n] = 0;
        g_serial_pending_len = 0;
        g_serial_pending[0]  = 0;
    }
    ensure_core_layout();
    append_file_text(KP_LOG_SERIAL, staged);
}

void RuntimeLog::LogSystem(const char* component, const char* message) {
    RtLogGuard _rtg;   // cross-core kvfs/log serialization (satoru)
    char line[512];
    char prefix[96];
    prefix[0] = 0;
    str_cat(prefix, "[", 96);
    str_cat(prefix, component ? component : "system", 96);
    str_cat(prefix, "] ", 96);
    format_line(line, sizeof(line), prefix, message, nullptr);
    if (g_fs_ready) { ensure_core_layout(); append_file_text(KP_LOG_SYSTEM, line); return; }
    append_buffer(g_system_pending, g_system_pending_len, sizeof(g_system_pending), line);
}

void RuntimeLog::LogBoot(const char* message) {
    RtLogGuard _rtg;   // cross-core kvfs/log serialization (satoru)
    char line[512];
    format_line(line, sizeof(line), "[boot] ", message, nullptr);
    if (g_fs_ready) { ensure_core_layout(); append_file_text(KP_LOG_BOOT, line); return; }
    append_buffer(g_boot_pending, g_boot_pending_len, sizeof(g_boot_pending), line);
}

void RuntimeLog::LogNetwork(const char* event, const char* detail) {
    RtLogGuard _rtg;   // cross-core kvfs/log serialization (satoru)
    if (!event || !*event) return;
    char line[512];
    format_line(line, sizeof(line), "[net] ", event, detail);
    if (g_fs_ready) { ensure_core_layout(); append_file_text(KP_LOG_NETWORK, line); return; }
    append_buffer(g_system_pending, g_system_pending_len, sizeof(g_system_pending), line);
}

void RuntimeLog::LogSecurity(const char* event, const char* detail) {
    RtLogGuard _rtg;   // cross-core kvfs/log serialization (satoru)
    if (!event || !*event) return;
    char line[512];
    format_line(line, sizeof(line), "[sec] ", event, detail);
    if (g_fs_ready) { ensure_core_layout(); append_file_text(KP_LOG_SECURITY, line); return; }
    append_buffer(g_system_pending, g_system_pending_len, sizeof(g_system_pending), line);
}

void RuntimeLog::LogCrash(const char* summary, const char* detail) {
    RtLogGuard _rtg;   // cross-core kvfs/log serialization (satoru)
    if (!summary || !*summary) return;
    char line[512];
    format_line(line, sizeof(line), "[crash] ", summary, detail);
    if (g_fs_ready) {
        ensure_core_layout();
        append_file_text(KP_LOG_CRASH_DIR "/crash.log", line);
        return;
    }
    append_buffer(g_system_pending, g_system_pending_len, sizeof(g_system_pending), line);
}

void RuntimeLog::LogAppEvent(const char* app, const char* event, const char* detail) {
    RtLogGuard _rtg;   // cross-core kvfs/log serialization (satoru)
    if (!app || !*app || !event || !*event) return;
    char path[192];
    char line[512];
    char prefix[96];
    char safe[64];
    sanitize_log_name(safe, app, sizeof(safe));
    path[0] = 0;
    str_cat(path, KP_LOG_APPS_DIR "/", sizeof(path));
    str_cat(path, safe, sizeof(path));
    str_cat(path, ".log", sizeof(path));
    prefix[0] = 0;
    str_cat(prefix, "[", 96);
    str_cat(prefix, app, 96);
    str_cat(prefix, "] ", 96);
    format_line(line, sizeof(line), prefix, event, detail);
    if (g_fs_ready) { ensure_core_layout(); append_file_text(path, line); return; }
    RuntimeLog::LogSystem("apps", line);
}

void RuntimeLog::LogProcessEvent(const char* process_name, int pid, const char* event, const char* detail) {
    RtLogGuard _rtg;   // cross-core kvfs/log serialization (satoru)
    if (!process_name || !*process_name || !event || !*event) return;
    char path[224];
    char line[512];
    char prefix[128];
    char pid_text[16];
    build_sub_log_path(path, sizeof(path), KP_LOG_PROC_DIR, process_name, pid);
    int_to_str(pid, pid_text, sizeof(pid_text));
    prefix[0] = 0;
    str_cat(prefix, "[pid ", sizeof(prefix));
    str_cat(prefix, pid_text, sizeof(prefix));
    str_cat(prefix, " ", sizeof(prefix));
    str_cat(prefix, process_name, sizeof(prefix));
    str_cat(prefix, "] ", sizeof(prefix));
    format_line(line, sizeof(line), prefix, event, detail);
    if (g_fs_ready) { ensure_core_layout(); append_file_text(path, line); return; }
    append_buffer(g_system_pending, g_system_pending_len, sizeof(g_system_pending), line);
}
