#include "logging.h"
#include "../fs/kvfs.h"

namespace {
    static bool g_fs_ready = false;

    static char g_serial_pending[16384];
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
            while (src[i] && i < max_len - 1) {
                dst[i] = src[i];
                i++;
            }
        }
        dst[i] = 0;
    }

    static void str_cat(char* dst, const char* src, int max_len) {
        if (!dst || max_len < 1) return;
        int n = str_len(dst);
        int i = 0;
        if (src) {
            while (src[i] && n < max_len - 1) {
                dst[n++] = src[i++];
            }
        }
        dst[n] = 0;
    }

    static void append_buffer(char* buffer, int& len, int cap, const char* text) {
        if (!buffer || cap <= 1 || !text) return;
        for (int i = 0; text[i] && len < cap - 1; i++) {
            buffer[len++] = text[i];
        }
        buffer[len] = 0;
    }

    static void ensure_file(const char* path) {
        if (!KVFS::Exists(path)) {
            KVFS::CreateFile(path);
        }
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
        if (detail && *detail) {
            str_cat(out, ": ", out_len);
            str_cat(out, detail, out_len);
        }
        str_cat(out, "\n", out_len);
    }

    static void create_app_layout(const char* app_id, const char* app_name) {
        if (!app_id || !*app_id) return;

        char base[128];
        char logs[160];
        char data[160];
        char info[160];
        char info_text[256];
        char klogs[192];
        char kruntime[224];

        base[0] = 0;
        str_cat(base, "/apps/", 128);
        str_cat(base, app_id, 128);

        str_cpy(logs, base, 160);
        str_cat(logs, "/logs", 160);

        str_cpy(data, base, 160);
        str_cat(data, "/data", 160);

        str_cpy(info, base, 160);
        str_cat(info, "/app.info", 160);

        KVFS::Mkdirs(base);
        KVFS::Mkdirs(logs);
        KVFS::Mkdirs(data);

        klogs[0] = 0;
        str_cat(klogs, "/kurono/logs/apps/", 192);
        str_cat(klogs, app_id, 192);
        KVFS::Mkdirs("/kurono/logs");
        KVFS::Mkdirs("/kurono/logs/apps");
        KVFS::Mkdirs(klogs);

        info_text[0] = 0;
        str_cat(info_text, "App: ", 256);
        str_cat(info_text, app_name ? app_name : app_id, 256);
        str_cat(info_text, "\nID: ", 256);
        str_cat(info_text, app_id, 256);
        str_cat(info_text, "\nLog: /apps/", 256);
        str_cat(info_text, app_id, 256);
        str_cat(info_text, "/logs/runtime.log\nData: /apps/", 256);
        str_cat(info_text, app_id, 256);
        str_cat(info_text, "/data\n", 256);
        KVFS::WriteString(info, info_text);

        str_cpy(info, base, 160);
        str_cat(info, "/logs/runtime.log", 160);
        ensure_file(info);

        kruntime[0] = 0;
        str_cat(kruntime, klogs, 224);
        str_cat(kruntime, "/runtime.log", 224);
        ensure_file(kruntime);
    }

    static void flush_pending_logs() {
        if (!g_fs_ready) return;
        append_file_text("/system/logs/serial.log", g_serial_pending);
        append_file_text("/kurono/logs/serial.log", g_serial_pending);
        append_file_text("/system/boot/boot.log", g_boot_pending);
        append_file_text("/kurono/logs/boot.log", g_boot_pending);
        append_file_text("/system/logs/system.log", g_system_pending);
        append_file_text("/kurono/logs/system.log", g_system_pending);
        g_serial_pending_len = 0;
        g_boot_pending_len = 0;
        g_system_pending_len = 0;
        g_serial_pending[0] = 0;
        g_boot_pending[0] = 0;
        g_system_pending[0] = 0;
    }
}

void RuntimeLog::InitFilesystem() {
    if (g_fs_ready) return;

    KVFS::Mkdirs("/system/logs");
    KVFS::Mkdirs("/system/boot");
    KVFS::Mkdirs("/system/kurono");
    KVFS::Mkdirs("/kurono/logs");
    KVFS::Mkdirs("/kurono/logs/apps");
    KVFS::Mkdirs("/boot");
    KVFS::Mkdirs("/apps");

    ensure_file("/system/logs/serial.log");
    ensure_file("/system/logs/system.log");
    ensure_file("/system/boot/boot.log");
    ensure_file("/kurono/logs/serial.log");
    ensure_file("/kurono/logs/system.log");
    ensure_file("/kurono/logs/boot.log");

    create_app_layout("terminal", "Terminal");
    create_app_layout("files", "File Manager");
    create_app_layout("calculator", "Calculator");
    create_app_layout("editor", "Text Editor");
    create_app_layout("settings", "Settings");
    create_app_layout("tasks", "Task Manager");
    create_app_layout("browser", "Browser");
    create_app_layout("media", "Media Player");
    create_app_layout("conduit", "Conduit");

    if (!KVFS::Exists("/system/kurono/secret.kcl")) {
        KVFS::WriteString("/system/kurono/secret.kcl",
            "# kurono secret\n"
            "# line three unlocks conduit\n"
            "echo keep listening\n");
    }

    KVFS::WriteString("/boot/kernel.info",
        "Kurono kernel\n"
        "Logs: /system/logs/system.log\n"
        "Serial: /system/logs/serial.log\n"
        "Boot: /system/boot/boot.log\n");
    KVFS::WriteString("/boot/apps.info",
        "Built-in apps are organized at runtime under /apps\n"
        "Each app has /logs and /data folders.\n");

    g_fs_ready = true;
    flush_pending_logs();
    RuntimeLog::LogSystem("logging", "filesystem log layout initialized");
    RuntimeLog::LogBoot("boot log online");
}

void RuntimeLog::MirrorSerial(const char* text) {
    if (!text || !*text) return;
    if (g_fs_ready) {
        append_file_text("/system/logs/serial.log", text);
        append_file_text("/kurono/logs/serial.log", text);
        return;
    }
    append_buffer(g_serial_pending, g_serial_pending_len, sizeof(g_serial_pending), text);
}

void RuntimeLog::LogSystem(const char* component, const char* message) {
    char line[512];
    char prefix[96];
    prefix[0] = 0;
    str_cat(prefix, "[", 96);
    str_cat(prefix, component ? component : "system", 96);
    str_cat(prefix, "] ", 96);
    format_line(line, sizeof(line), prefix, message, nullptr);

    if (g_fs_ready) {
        append_file_text("/system/logs/system.log", line);
        append_file_text("/kurono/logs/system.log", line);
        return;
    }
    append_buffer(g_system_pending, g_system_pending_len, sizeof(g_system_pending), line);
}

void RuntimeLog::LogBoot(const char* message) {
    char line[512];
    format_line(line, sizeof(line), "[boot] ", message, nullptr);

    if (g_fs_ready) {
        append_file_text("/system/boot/boot.log", line);
        append_file_text("/kurono/logs/boot.log", line);
        return;
    }
    append_buffer(g_boot_pending, g_boot_pending_len, sizeof(g_boot_pending), line);
}

void RuntimeLog::LogAppEvent(const char* app, const char* event, const char* detail) {
    if (!app || !*app || !event || !*event) return;

    char path[160];
    char line[512];
    char prefix[96];

    path[0] = 0;
    str_cat(path, "/apps/", 160);
    str_cat(path, app, 160);
    str_cat(path, "/logs/runtime.log", 160);

    prefix[0] = 0;
    str_cat(prefix, "[", 96);
    str_cat(prefix, app, 96);
    str_cat(prefix, "] ", 96);
    format_line(line, sizeof(line), prefix, event, detail);

    if (g_fs_ready) {
        append_file_text(path, line);
        char kpath[192];
        kpath[0] = 0;
        str_cat(kpath, "/kurono/logs/apps/", 192);
        str_cat(kpath, app, 192);
        str_cat(kpath, "/runtime.log", 192);
        append_file_text(kpath, line);
        return;
    }
    RuntimeLog::LogSystem("apps", line);
}