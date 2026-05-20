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

    static void int_to_str(int value, char* out, int out_len) {
        if (!out || out_len < 2) return;
        if (value < 0) {
            out[0] = '-';
            int_to_str(-value, out + 1, out_len - 1);
            return;
        }

        char tmp[16];
        int len = 0;
        do {
            tmp[len++] = (char)('0' + (value % 10));
            value /= 10;
        } while (value && len < (int)sizeof(tmp));

        int i = 0;
        while (len > 0 && i < out_len - 1) {
            out[i++] = tmp[--len];
        }
        out[i] = 0;
    }

    static bool is_safe_log_char(char ch) {
        return (ch >= 'a' && ch <= 'z') ||
               (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') ||
               ch == '_' || ch == '-' || ch == '.';
    }

    static void sanitize_log_name(char* dst, const char* src, int max_len) {
        if (!dst || max_len < 2) return;

        int out = 0;
        if (src) {
            for (int i = 0; src[i] && out < max_len - 1; i++) {
                dst[out++] = is_safe_log_char(src[i]) ? src[i] : '_';
            }
        }

        if (out == 0) {
            str_cpy(dst, "process", max_len);
            return;
        }
        dst[out] = 0;
    }

    static void build_process_log_path(char* out, int out_len, const char* root, const char* process_name, int pid) {
        char safe_name[64];
        char pid_text[16];

        sanitize_log_name(safe_name, process_name, sizeof(safe_name));
        int_to_str(pid, pid_text, sizeof(pid_text));

        out[0] = 0;
        str_cat(out, root, out_len);
        str_cat(out, "/", out_len);
        str_cat(out, pid_text, out_len);
        str_cat(out, "_", out_len);
        str_cat(out, safe_name, out_len);
        str_cat(out, ".log", out_len);
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

    static void ensure_core_layout() {
        KVFS::Mkdirs("/system/logs");
        KVFS::Mkdirs("/system/logs/processes");
        KVFS::Mkdirs("/system/boot");
        KVFS::Mkdirs("/system/kurono");
        KVFS::Mkdirs("/kurono/logs");
        KVFS::Mkdirs("/kurono/logs/apps");
        KVFS::Mkdirs("/kurono/logs/processes");

        ensure_file("/system/logs/serial.log");
        ensure_file("/system/logs/system.log");
        ensure_file("/system/boot/boot.log");
        ensure_file("/kurono/logs/serial.log");
        ensure_file("/kurono/logs/system.log");
        ensure_file("/kurono/logs/boot.log");
    }

    static void ensure_app_log_paths(const char* app_id) {
        if (!app_id || !*app_id) return;

        char base[128];
        char logs[160];
        char data[160];
        char runtime[160];
        char mirror_dir[192];
        char mirror_runtime[224];

        base[0] = 0;
        str_cat(base, "/apps/", sizeof(base));
        str_cat(base, app_id, sizeof(base));

        str_cpy(logs, base, sizeof(logs));
        str_cat(logs, "/logs", sizeof(logs));

        str_cpy(data, base, sizeof(data));
        str_cat(data, "/data", sizeof(data));

        str_cpy(runtime, logs, sizeof(runtime));
        str_cat(runtime, "/runtime.log", sizeof(runtime));

        mirror_dir[0] = 0;
        str_cat(mirror_dir, "/kurono/logs/apps/", sizeof(mirror_dir));
        str_cat(mirror_dir, app_id, sizeof(mirror_dir));

        str_cpy(mirror_runtime, mirror_dir, sizeof(mirror_runtime));
        str_cat(mirror_runtime, "/runtime.log", sizeof(mirror_runtime));

        KVFS::Mkdirs(base);
        KVFS::Mkdirs(logs);
        KVFS::Mkdirs(data);
        KVFS::Mkdirs(mirror_dir);
        ensure_file(runtime);
        ensure_file(mirror_runtime);
    }

    static void ensure_process_log_paths() {
        KVFS::Mkdirs("/system/logs/processes");
        KVFS::Mkdirs("/kurono/logs/processes");
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

    ensure_core_layout();
    KVFS::Mkdirs("/boot");
    KVFS::Mkdirs("/apps");
    KVFS::WriteString("/system/logs/README.txt",
        "Kurono runtime logs\n"
        "- serial.log: mirrored serial/driver/kernel output\n"
        "- system.log: structured runtime events\n"
        "- processes/: per-process lifecycle logs\n"
        "- /system/boot/boot.log: boot milestones\n");
    KVFS::WriteString("/kurono/logs/README.txt",
        "Kurono in-OS logs\n"
        "- serial.log: mirrored serial/driver/kernel output\n"
        "- system.log: structured runtime events\n"
        "- boot.log: boot milestones\n"
        "- processes/: mirrored per-process lifecycle logs\n"
        "- apps/<app>/runtime.log: per-app activity logs\n");
    KVFS::WriteString("/system/logs/processes/README.txt",
        "Per-process logs\n"
        "- One file per native or Linux process\n"
        "- Filenames are <pid>_<name>.log\n");
    KVFS::WriteString("/kurono/logs/processes/README.txt",
        "Mirrored per-process logs\n"
        "- One file per native or Linux process\n"
        "- Filenames are <pid>_<name>.log\n");

    create_app_layout("terminal", "Terminal");
    create_app_layout("files", "File Manager");
    create_app_layout("calculator", "Calculator");
    create_app_layout("editor", "Text Editor");
    create_app_layout("settings", "Settings");
    create_app_layout("tasks", "Task Manager");
    create_app_layout("browser", "Browser");
    create_app_layout("media", "Media Player");

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
        ensure_core_layout();
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
        ensure_core_layout();
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
        ensure_core_layout();
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
        ensure_core_layout();
        ensure_app_log_paths(app);
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

void RuntimeLog::LogProcessEvent(const char* process_name, int pid, const char* event, const char* detail) {
    if (!process_name || !*process_name || !event || !*event) return;

    char path[224];
    char mirror_path[224];
    char line[512];
    char prefix[128];
    char pid_text[16];

    build_process_log_path(path, sizeof(path), "/system/logs/processes", process_name, pid);
    build_process_log_path(mirror_path, sizeof(mirror_path), "/kurono/logs/processes", process_name, pid);

    int_to_str(pid, pid_text, sizeof(pid_text));
    prefix[0] = 0;
    str_cat(prefix, "[pid ", sizeof(prefix));
    str_cat(prefix, pid_text, sizeof(prefix));
    str_cat(prefix, " ", sizeof(prefix));
    str_cat(prefix, process_name, sizeof(prefix));
    str_cat(prefix, "] ", sizeof(prefix));
    format_line(line, sizeof(line), prefix, event, detail);

    if (g_fs_ready) {
        ensure_core_layout();
        ensure_process_log_paths();
        append_file_text(path, line);
        append_file_text(mirror_path, line);
        return;
    }

    append_buffer(g_system_pending, g_system_pending_len, sizeof(g_system_pending), line);
}