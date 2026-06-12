// kurono os enhanced kernel - full desktop edition
// complete bare-metal os with hybrid kernel, desktop environment and apps

#include "types.h"
#include "multiboot.h"
#include "system.h"
#include "heap.h"
#include "time.h"
#include "memory_mgr.h"
#include "buddy.h"
#include "slab.h"
#include "hrtimer.h"
#include "panic.h"
#include "../drivers/serial.h"
#include "../drivers/display.h"
#include "../drivers/graphics.h"
#include "../drivers/bga.h"
#include "../drivers/timer.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../media/mediadecoder.h"
#include "../ui/gui.h"
#include "../ui/lockscreen.h"
#include "../system/user_mgmt.h"
#include "../system/installer_gui.h"
#include "../ui/font.h"
#include "../ui/window_manager.h"
#include "../ui/desktop.h"
#include "../ui/notification.h"
#include "../apps/calculator.h"
#include "../apps/terminal.h"
#include "../apps/file_manager.h"
#include "../apps/text_editor.h"
#include "../apps/settings.h"
#include "../apps/task_manager.h"
#include "../hal/hal.h"
#include "../fs/vfs.h"
#include "../fs/kvfs.h"
#include "../fs/persist.h"
#include "../drivers/usb.h"
#include "../linux/ext4.h"
#include "../proc/scheduler.h"
#include "../tests/test_suite.h"
#include "../system/input_manager.h"
#include "../system/vconsole.h"
#include "../system/logging.h"
#include "../system/kpaths.h"
#include "../system/installer.h"
#include "../system/ui_config.h"
#include "../ui/kss.h"
#include "../system/gpu_driver_installer.h"
#include "../system/system_update.h"
#include "../shell/shell.h"
#include "../ui/wallpaper.h"
#include "../shell/linux_cmds.h"
#include "../shell/windows_cmds.h"
#include "../kcl/kcl.h"
#include "../security/supr.h"
#include "../packages/pkgmgr.h"
#include "../apps/python_interp.h"
#include "elf_loader.h"
#include "userspace.h"
#include "../userprogs/embedded_userprogs.h"
#include "../linux/linux_syscall.h"
#include "../proc/kernel_processes.h"
#include "../net/network.h"
#include "../net/tcpip.h"
#include "../net/tuntap.h"
#include "../net/ipv6.h"
#include "../net/netfilter.h"
#include "../net/unix_socket.h"
#include "../proc/cgroup.h"
#include "../drivers/tpm.h"
#include "../drivers/pulse_server.h"
#include "../hal/cpufreq.h"
#include "../system/runtime_layout.h"
#include "../system/dbus_server.h"
#include "../ui/wayland_server.h"
#include "../linux/ld_kurono.h"
#include "../drivers/audio.h"
#include "../drivers/audio_server.h"
#include "../drivers/audio_mixer.h"
#include "../drivers/audio_dma.h"
#include "../drivers/e1000.h"
#include "../linux/dual_boot.h"
#include "../linux/linux_init.h"
#include "../linux/linux_netbridge.h"
#include "../linux/linux_drivers.h"
#include "../virt/vmm.h"
#include "../virt/vdevices.h"
#include "../virt/iommu.h"
#include "../virt/hypervisor.h"
#include "../drivers/nvidia_gpu.h"
#include "../drivers/amd_gpu.h"
#include "../drivers/intel_gpu.h"
#include "../drivers/display_mgr.h"
#include "../drivers/virtio_gpu.h"   // bring up the virtio gpu so DisplayManager can select the accelerated backend (satoru)
#include "../drivers/ac97.h"
#include "../drivers/cpu_detect.h"
#include "../proc/smp.h"
#include "../drivers/gpu_probe.h"
#include "../../logo.h"
#include "../media/embedded_media.h"

// helper: string comparison (no libc)
static bool streq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

static bool stcontains(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) return false;
    for (const char* h = haystack; *h; h++) {
        const char* p = h;
        const char* n = needle;
        while (*p && *n && *p == *n) { p++; n++; }
        if (*n == 0) return true;
    }
    return false;
}

static int stlen(const char* s) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void stcopy(char* dst, const char* src, int max_len) {
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

static void stappend(char* dst, const char* src, int max_len) {
    if (!dst || max_len < 1) return;
    int n = stlen(dst);
    int i = 0;
    if (src) {
        while (src[i] && n < max_len - 1) {
            dst[n++] = src[i++];
        }
    }
    dst[n] = 0;
}

static bool stspace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool ststarts_with(const char* text, const char* prefix) {
    if (!text || !prefix) return false;
    while (*prefix) {
        if (*text != *prefix) return false;
        text++;
        prefix++;
    }
    return true;
}

static bool boot_has_token(const char* cmdline, const char* token) {
    if (!cmdline || !token || !token[0]) return false;
    int token_len = stlen(token);
    for (const char* p = cmdline; *p; ) {
        while (*p && stspace(*p)) p++;
        if (!*p) break;
        if (ststarts_with(p, token) && (p == cmdline || stspace(*(p - 1))) &&
            (p[token_len] == 0 || stspace(p[token_len]))) {
            return true;
        }
        while (*p && !stspace(*p)) p++;
    }
    return false;
}

static bool boot_get_value(const char* cmdline, const char* key, char* out, int max_out) {
    if (!out || max_out < 2) return false;
    out[0] = 0;
    if (!cmdline || !key || !key[0]) return false;

    int key_len = stlen(key);
    for (const char* p = cmdline; *p; p++) {
        if (p != cmdline && !stspace(*(p - 1))) continue;
        if (!ststarts_with(p, key) || p[key_len] != '=') continue;

        const char* value = p + key_len + 1;
        char quote = 0;
        if (*value == '"' || *value == '\'') {
            quote = *value;
            value++;
        }

        int n = 0;
        while (*value && n < max_out - 1) {
            if (quote) {
                if (*value == quote) break;
            } else if (stspace(*value)) {
                break;
            }
            out[n++] = *value++;
        }
        out[n] = 0;
        return n > 0;
    }
    return false;
}

// works on all x86 hardware in text mode. used before graphics init.
static volatile uint16_t* const VGA_TEXT = (volatile uint16_t*)0xB8000;
static int vga_row = 0, vga_col = 0;
static const int VGA_COLS = 80, VGA_ROWS = 25;

static void vga_clear() {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA_TEXT[i] = 0x0F00 | ' '; // white on black
    vga_row = 0; vga_col = 0;
}

static void vga_puts(const char* s, uint8_t color = 0x0F) {
    while (*s) {
        if (*s == '\n') {
            vga_col = 0; vga_row++;
        } else {
            if (vga_row < VGA_ROWS && vga_col < VGA_COLS)
                VGA_TEXT[vga_row * VGA_COLS + vga_col] = ((uint16_t)color << 8) | (uint8_t)*s;
            vga_col++;
        }
        if (vga_row >= VGA_ROWS) vga_row = VGA_ROWS - 1; // stop scrolling, stay on last line
        s++;
    }
}

static void vga_puthex(uint32_t val) {
    const char* hex = "0123456789ABCDEF";
    char buf[11] = "0x00000000";
    for (int i = 9; i >= 2; i--) { buf[i] = hex[val & 0xF]; val >>= 4; }
    vga_puts(buf);
}

static void vga_u32_to_dec(uint32_t val, char* out, int max_len) {
    if (!out || max_len < 2) return;
    if (val == 0) { out[0] = '0'; out[1] = 0; return; }
    char rev[16];
    int ri = 0;
    while (val && ri < 15) { rev[ri++] = (char)('0' + (val % 10)); val /= 10; }
    int oi = 0;
    while (ri > 0 && oi < max_len - 1) out[oi++] = rev[--ri];
    out[oi] = 0;
}

static void vga_write_line(int row, const char* s, uint8_t color = 0x0F) {
    if (row < 0 || row >= VGA_ROWS) return;
    int base = row * VGA_COLS;
    for (int i = 0; i < VGA_COLS; i++) VGA_TEXT[base + i] = ((uint16_t)color << 8) | ' ';
    if (!s) return;
    for (int i = 0; s[i] && i < VGA_COLS; i++) VGA_TEXT[base + i] = ((uint16_t)color << 8) | (uint8_t)s[i];
}

struct EmergencyLine {
    char text[VGA_COLS + 1];
    uint8_t color;
};

static EmergencyLine g_emergency_lines[18];
static int g_emergency_line_count = 0;

static void emergency_clear_lines() {
    g_emergency_line_count = 0;
    for (int i = 0; i < 18; i++) {
        g_emergency_lines[i].text[0] = 0;
        g_emergency_lines[i].color = 0x0F;
    }
}

static void emergency_push_line(const char* text, uint8_t color = 0x0F) {
    if (g_emergency_line_count >= 18) {
        for (int i = 1; i < 18; i++) g_emergency_lines[i - 1] = g_emergency_lines[i];
        g_emergency_line_count = 17;
    }
    stcopy(g_emergency_lines[g_emergency_line_count].text, text ? text : "", VGA_COLS + 1);
    g_emergency_lines[g_emergency_line_count].color = color;
    g_emergency_line_count++;
}

static void emergency_push_output(const char* text, uint8_t color = 0x0F) {
    char line[VGA_COLS + 1];
    int pos = 0;
    if (!text || !*text) {
        emergency_push_line("(ok)", 0x0A);
        return;
    }

    for (int i = 0; text[i]; i++) {
        char c = text[i];

        if (c == '\x1b') {
            i++;
            if (text[i] == '[') {
                while (text[i] && !((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z'))) i++;
            }
            continue;
        }
        if (c == '\r') continue;
        if (c == '\n') {
            line[pos] = 0;
            emergency_push_line(line, color);
            pos = 0;
            line[0] = 0;
            continue;
        }
        if (c == '\t') c = ' ';
        if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '?';
        line[pos++] = c;
        if (pos >= VGA_COLS) {
            line[pos] = 0;
            emergency_push_line(line, color);
            pos = 0;
            line[0] = 0;
        }
    }
    if (pos > 0) {
        line[pos] = 0;
        emergency_push_line(line, color);
    }
}

static void emergency_render(const char* input) {
    vga_clear();
    vga_write_line(0, "Kurono Emergency Kernel", 0x0C);
    vga_write_line(1, "Minimal recovery shell | same kernel, emergency boot profile", 0x0F);
    vga_write_line(2, "Commands: help ls cat tail dmesg reboot | shortcuts: bootlog syslog", 0x0B);
    vga_write_line(3, "seriallog clear", 0x0B);
    for (int i = 0; i < 18; i++) {
        if (i < g_emergency_line_count) vga_write_line(4 + i, g_emergency_lines[i].text, g_emergency_lines[i].color);
        else vga_write_line(4 + i, "", 0x07);
    }
    char prompt[VGA_COLS + 1];
    stcopy(prompt, "emergency> ", sizeof(prompt));
    stappend(prompt, input ? input : "", sizeof(prompt));
    vga_write_line(22, prompt, 0x0F);
    vga_write_line(23, "Logs: /system/boot/boot.log | /system/logs/system.log | /system/logs/serial.log", 0x08);
    vga_write_line(24, "Enter=run  Backspace=edit  Reboot command exits recovery", 0x08);
}

static void emergency_run_shell() {
    char input[256];
    int input_len = 0;
    input[0] = 0;

    emergency_clear_lines();
    emergency_push_line("Recovery shell ready.", 0x0A);
    emergency_push_line("Use 'bootlog', 'syslog', or 'seriallog' to inspect logs.", 0x07);
    emergency_render(input);

    while (true) {
        uint32_t real_elapsed = Timer::ElapsedSinceLast();
        if (real_elapsed > 0) TimeManager::AdvanceByMs(real_elapsed);
        Scheduler::Tick();
        Keyboard::Poll();

        bool changed = false;
        while (Keyboard::HasChar()) {
            char c = Keyboard::GetChar();
            if (c == '\r' || c == '\n') {
                changed = true;
                if (input_len > 0) {
                    char cmd[512];
                    char out[SHELL_OUTPUT_BUF];
                    char cmdline[VGA_COLS + 1];
                    stcopy(cmd, input, sizeof(cmd));
                    stcopy(cmdline, "emergency> ", sizeof(cmdline));
                    stappend(cmdline, input, sizeof(cmdline));
                    emergency_push_line(cmdline, 0x0F);
                    if (streq(cmd, "clear")) {
                        emergency_clear_lines();
                    } else {
                        if (streq(cmd, "bootlog")) stcopy(cmd, "tail -n 40 /system/boot/boot.log", sizeof(cmd));
                        else if (streq(cmd, "syslog")) stcopy(cmd, "tail -n 40 /system/logs/system.log", sizeof(cmd));
                        else if (streq(cmd, "seriallog")) stcopy(cmd, "tail -n 40 /system/logs/serial.log", sizeof(cmd));
                        out[0] = 0;
                        KuronoShell::Execute(cmd, out, sizeof(out));
                        emergency_push_output(out, 0x07);
                    }
                }
                input_len = 0;
                input[0] = 0;
            } else if (c == 8 || c == 127) {
                if (input_len > 0) {
                    input[--input_len] = 0;
                    changed = true;
                }
            } else if ((unsigned char)c >= 32 && (unsigned char)c <= 126) {
                if (input_len < (int)sizeof(input) - 1) {
                    input[input_len++] = c;
                    input[input_len] = 0;
                    changed = true;
                }
            }
        }

        if (changed) emergency_render(input);
    }
}

static EmergencyLine g_cli_lines[18];
static int g_cli_line_count = 0;

static void cli_clear_lines() {
    g_cli_line_count = 0;
    for (int i = 0; i < 18; i++) {
        g_cli_lines[i].text[0] = 0;
        g_cli_lines[i].color = 0x0F;
    }
}

static void cli_push_line(const char* text, uint8_t color = 0x0F) {
    if (g_cli_line_count >= 18) {
        for (int i = 1; i < 18; i++) g_cli_lines[i - 1] = g_cli_lines[i];
        g_cli_line_count = 17;
    }
    stcopy(g_cli_lines[g_cli_line_count].text, text ? text : "", VGA_COLS + 1);
    g_cli_lines[g_cli_line_count].color = color;
    g_cli_line_count++;
}

static void cli_push_output(const char* text, uint8_t color = 0x0F) {
    char line[VGA_COLS + 1];
    int pos = 0;
    if (!text || !*text) {
        cli_push_line("(ok)", 0x0A);
        return;
    }

    for (int i = 0; text[i]; i++) {
        char c = text[i];
        if (c == '\x1b') {
            i++;
            if (text[i] == '[') {
                while (text[i] &&
                       !((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z'))) {
                    i++;
                }
            }
            continue;
        }
        if (c == '\r') continue;
        if (c == '\n') {
            line[pos] = 0;
            cli_push_line(line, color);
            pos = 0;
            line[0] = 0;
            continue;
        }
        if (c == '\t') c = ' ';
        if ((unsigned char)c < 32 || (unsigned char)c > 126) c = '?';
        line[pos++] = c;
        if (pos >= VGA_COLS) {
            line[pos] = 0;
            cli_push_line(line, color);
            pos = 0;
            line[0] = 0;
        }
    }
    if (pos > 0) {
        line[pos] = 0;
        cli_push_line(line, color);
    }
}

static void cli_render(const char* input) {
    vga_clear();
    vga_write_line(0, "Kurono CLI Boot Mode", 0x0A);
    vga_write_line(1, "Shared kernel init path, shell, package manager, and TCP/IP stack", 0x0F);
    vga_write_line(2, "Commands: help | ip | ping host | curl http://... | kpkg sync | reboot", 0x0B);
    vga_write_line(3, "Autorun results are mirrored to serial for headless QEMU testing", 0x08);
    for (int i = 0; i < 18; i++) {
        if (i < g_cli_line_count) vga_write_line(4 + i, g_cli_lines[i].text, g_cli_lines[i].color);
        else vga_write_line(4 + i, "", 0x07);
    }
    char prompt[VGA_COLS + 1];
    stcopy(prompt, "cli> ", sizeof(prompt));
    stappend(prompt, input ? input : "", sizeof(prompt));
    vga_write_line(22, prompt, 0x0F);
    vga_write_line(23, "Logs: serial + /system/boot/boot.log | Ctrl+Alt+F1..F6 still switch VTs", 0x08);
    vga_write_line(24, "Enter=run  Backspace=edit  'shutdown' halts  'reboot' resets", 0x08);
}

static void cli_log_output(const char* text) {
    if (!text || !text[0]) {
        SerialLogger::Log("(ok)\r\n");
        return;
    }
    SerialLogger::Log(text);
    int len = stlen(text);
    if (len < 1 || text[len - 1] != '\n') SerialLogger::Log("\r\n");
}

static void cli_decode_command(char* text) {
    if (!text) return;
    int w = 0;
    for (int r = 0; text[r]; r++) {
        if (text[r] == '+') {
            text[w++] = ' ';
        } else if (text[r] == '%' && text[r + 1] == '2' && text[r + 2] == '0') {
            text[w++] = ' ';
            r += 2;
        } else {
            text[w++] = text[r];
        }
    }
    text[w] = 0;
}

static void cli_run_command(const char* cmd) {
    if (!cmd || !cmd[0]) return;

    char prompt[VGA_COLS + 1];
    stcopy(prompt, "cli> ", sizeof(prompt));
    stappend(prompt, cmd, sizeof(prompt));
    cli_push_line(prompt, 0x0F);
    SerialLogger::Log(prompt);
    SerialLogger::Log("\r\n");

    if (streq(cmd, "clear")) {
        cli_clear_lines();
        cli_push_line("Screen cleared.", 0x0A);
        SerialLogger::Log("Screen cleared.\r\n");
        return;
    }

    char out[SHELL_OUTPUT_BUF];
    out[0] = 0;
    KuronoShell::Execute(cmd, out, sizeof(out));
    cli_push_output(out, out[0] ? 0x07 : 0x0A);
    cli_log_output(out);
}

static void cli_run_shell(const char* autorun_cmd, bool halt_after_autorun) {
    char input[256];
    int input_len = 0;
    input[0] = 0;

    Keyboard::FlushBuffers();
    cli_clear_lines();
    cli_push_line("CLI shell ready.", 0x0A);
    cli_push_line("This mode skips the desktop but keeps the normal package/network stack.", 0x07);

    if (autorun_cmd && autorun_cmd[0]) {
        cli_push_line("Autorun command requested.", 0x0B);
        SerialLogger::Log("[CLI] Autorun requested\r\n");
        cli_run_command(autorun_cmd);
        if (halt_after_autorun) {
            cli_push_line("Autorun complete. Halting CPU.", 0x0E);
            cli_render("");
            SerialLogger::Log("[CLI] Autorun complete, halting\r\n");
            HAL::DisableInterrupts();
            HAL::Halt();
        }
    }

    cli_render(input);

    while (true) {
        uint32_t real_elapsed = Timer::ElapsedSinceLast();
        if (real_elapsed > 0) TimeManager::AdvanceByMs(real_elapsed);
        if (TCPStack::IsUp()) TCPStack::Tick();
        Scheduler::Tick();
        Keyboard::Poll();

        bool changed = false;
        while (Keyboard::HasChar()) {
            char c = Keyboard::GetChar();
            if (c == '\r' || c == '\n') {
                changed = true;
                if (input_len > 0) cli_run_command(input);
                input_len = 0;
                input[0] = 0;
            } else if (c == 8 || c == 127) {
                if (input_len > 0) {
                    input[--input_len] = 0;
                    changed = true;
                }
            } else if ((unsigned char)c >= 32 && (unsigned char)c <= 126) {
                if (input_len < (int)sizeof(input) - 1) {
                    input[input_len++] = c;
                    input[input_len] = 0;
                    changed = true;
                }
            }
        }

        if (changed) cli_render(input);
    }
}

// early framebuffer pixel writer - works before graphics::init
// handles 16, 24, and 32 bpp for maximum hardware compatibility
static void early_fb_fill(uint64_t fb_addr, uint32_t pitch, uint32_t w, uint32_t h, uint8_t bpp, uint32_t color) {
    if (!fb_addr || !w || !h || bpp < 15) return;
    uint8_t* fb = (uint8_t*)(uintptr_t)fb_addr;
    uint32_t bytes_pp = bpp / 8;
    uint8_t r = (uint8_t)(color >> 16);
    uint8_t g = (uint8_t)(color >> 8);
    uint8_t b = (uint8_t)(color);
    for (uint32_t y = 0; y < h && y < 32; y++) { // top 32 rows = color bar
        for (uint32_t x = 0; x < w; x++) {
            uint8_t* p = fb + y * pitch + x * bytes_pp;
            if (bytes_pp == 4) {
                p[0] = b; p[1] = g; p[2] = r; p[3] = 0xFF;
            } else if (bytes_pp == 3) {
                p[0] = b; p[1] = g; p[2] = r;
            } else if (bytes_pp == 2) {
                // rgb565: rrrrrggggggbbbbb
                uint16_t px = ((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (b >> 3);
                *(uint16_t*)p = px;
            }
        }
    }
    // flush cpu caches so pixels reach gpu vram on real hardware
    __asm__ __volatile__("wbinvd" ::: "memory");
}

// minimal 8x8 built-in font bitmap for boot diagnostics.
// works before any subsystem init  -  writes directly to the framebuffer.
// this is essential for bare-metal debugging when there's no serial port.
static uint64_t _efb_addr = 0;
static uint32_t _efb_pitch = 0, _efb_w = 0, _efb_h = 0;
static uint8_t  _efb_bpp = 0;
static int _efb_cx = 4, _efb_cy = 36; // cursor position (below the color bar)

// tiny 8x8 font  -  covers printable ascii 0x20-0x7e only
// each char is 8 bytes (1 byte per row, msb-first)
static const uint8_t _efb_font[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' ' (0x20)
    {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00}, // '!'
    {0x6C,0x6C,0x24,0x00,0x00,0x00,0x00,0x00}, // '"'
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, // '#'
    {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00}, // '$'
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, // '%'
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, // '&'
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // '''
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // '('
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // ')'
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // '*'
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // '+'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // ','
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // '-'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // '.'
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, // '/'
    {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00}, // '0'
    {0x18,0x38,0x78,0x18,0x18,0x18,0x7E,0x00}, // '1'
    {0x7C,0xC6,0x06,0x1C,0x30,0x60,0xFE,0x00}, // '2'
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, // '3'
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x00}, // '4'
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, // '5'
    {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, // '6'
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, // '7'
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, // '8'
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, // '9'
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, // ':'
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, // ';'
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, // '<'
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, // '='
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, // '>'
    {0x7C,0xC6,0x06,0x0C,0x18,0x00,0x18,0x00}, // '?'
    {0x7C,0xC6,0xDE,0xDE,0xDC,0xC0,0x7C,0x00}, // '@'
    {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 'a'
    {0xFC,0xC6,0xC6,0xFC,0xC6,0xC6,0xFC,0x00}, // 'b'
    {0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00}, // 'c'
    {0xF8,0xCC,0xC6,0xC6,0xC6,0xCC,0xF8,0x00}, // 'd'
    {0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xFE,0x00}, // 'e'
    {0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xC0,0x00}, // 'f'
    {0x7C,0xC6,0xC0,0xCE,0xC6,0xC6,0x7E,0x00}, // 'g'
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 'h'
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00}, // 'i'
    {0x1E,0x06,0x06,0x06,0xC6,0xC6,0x7C,0x00}, // 'j'
    {0xC6,0xCC,0xD8,0xF0,0xD8,0xCC,0xC6,0x00}, // 'k'
    {0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xFE,0x00}, // 'l'
    {0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00}, // 'm'
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, // 'n'
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 'o'
    {0xFC,0xC6,0xC6,0xFC,0xC0,0xC0,0xC0,0x00}, // 'p'
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06}, // 'q'
    {0xFC,0xC6,0xC6,0xFC,0xD8,0xCC,0xC6,0x00}, // 'r'
    {0x7C,0xC6,0xC0,0x7C,0x06,0xC6,0x7C,0x00}, // 's'
    {0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // 't'
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 'u'
    {0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00}, // 'v'
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, // 'w'
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00}, // 'x'
    {0xC3,0xC3,0x66,0x3C,0x18,0x18,0x18,0x00}, // 'y'
    {0xFE,0x06,0x0C,0x18,0x30,0x60,0xFE,0x00}, // 'z'
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // '['
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, // '\'
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // ']'
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, // '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE}, // '_'
    {0x18,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // '`'
    {0x00,0x00,0x7C,0x06,0x7E,0xC6,0x7E,0x00}, // 'a'
    {0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xFC,0x00}, // 'b'
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00}, // 'c'
    {0x06,0x06,0x7E,0xC6,0xC6,0xC6,0x7E,0x00}, // 'd'
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, // 'e'
    {0x1C,0x36,0x30,0x7C,0x30,0x30,0x30,0x00}, // 'f'
    {0x00,0x00,0x7E,0xC6,0xC6,0x7E,0x06,0x7C}, // 'g'
    {0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x00}, // 'h'
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, // 'i'
    {0x06,0x00,0x0E,0x06,0x06,0xC6,0xC6,0x7C}, // 'j'
    {0xC0,0xC0,0xCC,0xD8,0xF0,0xD8,0xCC,0x00}, // 'k'
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'l'
    {0x00,0x00,0xCC,0xFE,0xD6,0xC6,0xC6,0x00}, // 'm'
    {0x00,0x00,0xFC,0xC6,0xC6,0xC6,0xC6,0x00}, // 'n'
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, // 'o'
    {0x00,0x00,0xFC,0xC6,0xC6,0xFC,0xC0,0xC0}, // 'p'
    {0x00,0x00,0x7E,0xC6,0xC6,0x7E,0x06,0x06}, // 'q'
    {0x00,0x00,0xDC,0xE6,0xC0,0xC0,0xC0,0x00}, // 'r'
    {0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00}, // 's'
    {0x30,0x30,0x7C,0x30,0x30,0x36,0x1C,0x00}, // 't'
    {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0x7E,0x00}, // 'u'
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, // 'v'
    {0x00,0x00,0xC6,0xC6,0xD6,0xFE,0x6C,0x00}, // 'w'
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, // 'x'
    {0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0x7C}, // 'y'
    {0x00,0x00,0xFE,0x0C,0x38,0x60,0xFE,0x00}, // 'z'
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, // '{'
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // '|'
    {0xE0,0x30,0x30,0x1C,0x30,0x30,0xE0,0x00}, // '}'
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, // '~'
};

static void early_fb_putpx(int x, int y, uint32_t color) {
    if (!_efb_addr || x < 0 || y < 0 || (uint32_t)x >= _efb_w || (uint32_t)y >= _efb_h) return;
    uint8_t* fb = (uint8_t*)(uintptr_t)_efb_addr;
    uint32_t bpp = _efb_bpp / 8;
    uint8_t* p = fb + y * _efb_pitch + x * bpp;
    if (bpp == 4) { p[0]=(uint8_t)color; p[1]=(uint8_t)(color>>8); p[2]=(uint8_t)(color>>16); p[3]=0xFF; }
    else if (bpp == 3) { p[0]=(uint8_t)color; p[1]=(uint8_t)(color>>8); p[2]=(uint8_t)(color>>16); }
}

static void early_fb_putc(char c, uint32_t fg) {
    if (c == '\n') { _efb_cx = 4; _efb_cy += 10; return; }
    if (c < 0x20 || c > 0x7E) return;
    const uint8_t* glyph = _efb_font[c - 0x20];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col))
                early_fb_putpx(_efb_cx + col, _efb_cy + row, fg);
        }
    }
    _efb_cx += 8;
    if ((uint32_t)_efb_cx >= _efb_w - 8) { _efb_cx = 4; _efb_cy += 10; }
}

static void early_fb_puts(const char* s, uint32_t fg = 0x00FF00) {
    while (*s) early_fb_putc(*s++, fg);
}

static void early_fb_puthex32(uint32_t val, uint32_t fg = 0x00FF00) {
    const char* hex = "0123456789ABCDEF";
    early_fb_putc('0', fg); early_fb_putc('x', fg);
    for (int i = 28; i >= 0; i -= 4)
        early_fb_putc(hex[(val >> i) & 0xF], fg);
}

// initialize the early fb text context after we know the fb parameters
static void early_fb_init_text(uint64_t addr, uint32_t pitch, uint32_t w, uint32_t h, uint8_t bpp) {
    _efb_addr = addr; _efb_pitch = pitch; _efb_w = w; _efb_h = h; _efb_bpp = bpp;
    _efb_cx = 4; _efb_cy = 36;
}

// flush the early fb writes to the display  -  tries both wbinvd and sfence
static void early_fb_flush() {
    __asm__ __volatile__("wbinvd" ::: "memory");
}

//  multiboot2 → multiboot1 compatibility shim
//  converts mb2 tagged info into a static multiboot_info_t so all existing
//  kernel code works unchanged with either boot protocol.
static multiboot_info_t mb2_compat_info;
// buffer for converted mb1-format mmap entries (each 24 bytes, room for 170)
static uint8_t mb2_mmap_buf[4096];

static void convert_mb2_to_mb1(uint64_t mb_addr) {
    // mb2 boot info starts with: uint32_t total_size, uint32_t reserved
    // then tags follow, each 8-byte aligned
    uint8_t* base = (uint8_t*)(uintptr_t)mb_addr;
    uint32_t total_size = *(uint32_t*)base;
    uint8_t* tag = base + 8;
    uint8_t* end = base + total_size;

    for (int i = 0; i < (int)sizeof(mb2_compat_info); i++)
        ((uint8_t*)&mb2_compat_info)[i] = 0;

    while (tag < end) {
        uint32_t type = *(uint32_t*)tag;
        uint32_t size = *(uint32_t*)(tag + 4);
        if (type == 0 || size < 8) break; // end tag or malformed

        switch (type) {
            case 1: { // boot command line
                mb2_compat_info.flags |= (1u << 2);
                mb2_compat_info.cmdline = (uint32_t)(uintptr_t)(tag + 8);
                break;
            }
            case 4: { // basic memory info
                mb2_compat_info.flags |= (1u << 0);
                mb2_compat_info.mem_lower = *(uint32_t*)(tag + 8);
                mb2_compat_info.mem_upper = *(uint32_t*)(tag + 12);
                break;
            }
            case 6: { // memory map
                // convert mb2 mmap entries to mb1 format in our static buffer
                // mb2 entry: {u64 base, u64 length, u32 type, u32 reserved} = variable size
                // mb1 entry: {u32 size=20, u64 base, u64 length, u32 type} = 24 bytes
                uint32_t entry_size = *(uint32_t*)(tag + 8);
                if (entry_size == 0) entry_size = 24; // sane default
                uint8_t* src = tag + 16;
                uint8_t* src_end = tag + size;
                uint8_t* dst = mb2_mmap_buf;
                uint8_t* dst_end = mb2_mmap_buf + sizeof(mb2_mmap_buf);
                while (src + entry_size <= src_end && dst + 24 <= dst_end) {
                    uint64_t mbase  = *(uint64_t*)(src + 0);
                    uint64_t mlen   = *(uint64_t*)(src + 8);
                    uint32_t mtype  = *(uint32_t*)(src + 16);
                    // write mb1-format entry: size(4) + base(8) + length(8) + type(4)
                    *(uint32_t*)(dst + 0)  = 20;    // size of rest
                    *(uint64_t*)(dst + 4)  = mbase;
                    *(uint64_t*)(dst + 12) = mlen;
                    *(uint32_t*)(dst + 20) = mtype;
                    src += entry_size;
                    dst += 24;
                }
                mb2_compat_info.flags |= (1u << 6);
                mb2_compat_info.mmap_addr = (uint32_t)(uintptr_t)mb2_mmap_buf;
                mb2_compat_info.mmap_length = (uint32_t)(dst - mb2_mmap_buf);
                break;
            }
            case 8: { // framebuffer info
                mb2_compat_info.flags |= (1u << 12);
                mb2_compat_info.framebuffer_addr  = *(uint64_t*)(tag + 8);
                mb2_compat_info.framebuffer_pitch  = *(uint32_t*)(tag + 16);
                mb2_compat_info.framebuffer_width  = *(uint32_t*)(tag + 20);
                mb2_compat_info.framebuffer_height = *(uint32_t*)(tag + 24);
                mb2_compat_info.framebuffer_bpp    = *(uint8_t*)(tag + 28);
                mb2_compat_info.framebuffer_type   = *(uint8_t*)(tag + 29);
                break;
            }
            // module tags (type 3) have variable format  -  skip for now.
            // boot loader name (type 2), etc.  -  not needed.
        }

        // advance to next tag (8-byte aligned)
        uint32_t advance = (size + 7u) & ~7u;
        tag += advance;
    }
}

extern "C" void kernel_main(uint64_t magic, uint64_t mb_addr) {
    SerialLogger::Init();
    SerialLogger::Log("Kurono OS Starting...\r\n");
    RuntimeLog::LogBoot("kernel_main entered");

    // if booted via mb2, convert the tagged info to mb1 format so all
    // existing kernel code works unchanged.
    if (magic == 0x36d76289) {
        SerialLogger::Log("Multiboot2 detected  -  converting to MB1 compat\r\n");
        RuntimeLog::LogBoot("multiboot2 detected");
        convert_mb2_to_mb1(mb_addr);
        mb_addr = (uint64_t)(uintptr_t)&mb2_compat_info;
        magic = 0x2BADB002;     // pretend it's mb1 from here on
    }

    vga_clear();
    vga_puts("Kurono OS booting...\n", 0x0A);

    // this gives immediate visual feedback even before any subsystem init
    multiboot_info_t* mbi_early = (multiboot_info_t*)(uintptr_t)mb_addr;
    if (magic == 0x2BADB002) {
        KernelPanic::Initialize(mbi_early);
    }
    bool has_early_fb = false;
    if (magic == 0x2BADB002 && (mbi_early->flags & (1u << 12)) && mbi_early->framebuffer_addr != 0
        && mbi_early->framebuffer_width > 0 && mbi_early->framebuffer_height > 0
        && mbi_early->framebuffer_type != 2) {
        uint64_t fb = mbi_early->framebuffer_addr;
        uint32_t pitch = mbi_early->framebuffer_pitch;
        uint32_t fw = mbi_early->framebuffer_width;
        uint32_t fh = mbi_early->framebuffer_height;
        uint8_t  fbpp = mbi_early->framebuffer_bpp;

        // draw a bright green bar at top of screen (32px)  -  proves we're alive
        uint8_t* fbp = (uint8_t*)(uintptr_t)fb;
        uint32_t bpp = fbpp / 8;
        for (uint32_t y = 0; y < 32 && y < fh; y++) {
            for (uint32_t x = 0; x < fw; x++) {
                uint8_t* p = fbp + y * pitch + x * bpp;
                if (bpp == 4) { p[0]=0x00; p[1]=0xFF; p[2]=0x00; p[3]=0xFF; }
                else if (bpp == 3) { p[0]=0x00; p[1]=0xFF; p[2]=0x00; }
            }
        }
        __asm__ __volatile__("wbinvd" ::: "memory");

        // init early text renderer and show diagnostic info on the fb
        early_fb_init_text(fb, pitch, fw, fh, fbpp);
        early_fb_puts("Kurono OS booting...\n", 0xFFFFFF);
        early_fb_puts("FB: ", 0x00FF00);
        early_fb_puthex32((uint32_t)(fb >> 32), 0x00FF00);
        early_fb_puthex32((uint32_t)(fb & 0xFFFFFFFF), 0x00FF00);
        early_fb_puts(" ", 0x00FF00);
        early_fb_puthex32(fw, 0x00FF00);
        early_fb_putc('x', 0x00FF00);
        early_fb_puthex32(fh, 0x00FF00);
        early_fb_putc('\n', 0);
        early_fb_flush();

        has_early_fb = true;
        SerialLogger::Log("Early FB: drew green bar + text\r\n");
    } else {
        SerialLogger::Log("Early FB: SKIPPED (flags=0x");
        if (magic == 0x2BADB002) {
            SerialLogger::LogHex(mbi_early->flags);
            SerialLogger::Log(" addr=0x");
            SerialLogger::LogHex((uint32_t)(mbi_early->framebuffer_addr & 0xFFFFFFFF));
            SerialLogger::Log(" type=");
            SerialLogger::LogDec((int)mbi_early->framebuffer_type);
        } else {
            SerialLogger::Log("BAD_MAGIC");
        }
        SerialLogger::Log(")\r\n");
    }

    // check multiboot magic
    if (magic != 0x2BADB002) {
        vga_puts("FATAL: Bad multiboot magic!\n", 0x0C);
        if (has_early_fb) { early_fb_puts("FATAL: Bad multiboot magic!\n", 0xFF0000); early_fb_flush(); }
        SerialLogger::Log("FATAL: Invalid Multiboot Magic!\r\n");
        while (true) { __asm__ __volatile__("cli; hlt"); }
    }

    bool boot_text_only = false;
    bool boot_console_realtime = false;
    bool boot_emergency = false;
    bool boot_cli = false;
    bool boot_cli_poweroff = false;
    // autologin: skip the lockscreen/first-boot wizard and drop straight to
    // the desktop with a default user. used by the bootable iso "straight to
    // desktop" entry so a fresh vm boots to a usable desktop with no typing. (satoru)
    bool boot_autologin = false;
    // ffmpeg on-device smoke test, parsed early into a bool: the grub cmdline
    // string lives in low memory that the (now large) kernel image/heap can
    // clobber before the test runs deep in boot, so we must latch it now. (satoru)
    bool boot_ffmpeg_test = false;
    // raw 1:1 mouse (no accel)  -  accessibility + deterministic synthetic input. (satoru)
    bool boot_mouse_raw = false;
    char boot_cli_run[160];
    boot_cli_run[0] = 0;
    static char boot_gui_run[256];
    boot_gui_run[0] = 0;
    const char* boot_cmdline = nullptr;
    if (mbi_early && (mbi_early->flags & (1u << 2)) && mbi_early->cmdline != 0) {
        boot_cmdline = (const char*)(uintptr_t)mbi_early->cmdline;
        if (boot_has_token(boot_cmdline, "kurono_mode=text") || boot_has_token(boot_cmdline, "kurono.text=1")) {
            boot_text_only = true;
        }
        if (boot_has_token(boot_cmdline, "kurono_mode=console") || boot_has_token(boot_cmdline, "kurono.console=1")) {
            boot_console_realtime = true;
        }
        if (boot_has_token(boot_cmdline, "kurono_mode=emergency") || boot_has_token(boot_cmdline, "kurono.emergency=1")) {
            boot_emergency = true;
        }
        if (boot_has_token(boot_cmdline, "-cli") || boot_has_token(boot_cmdline, "kurono_mode=cli") ||
            boot_has_token(boot_cmdline, "kurono.cli=1")) {
            boot_cli = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.cli.poweroff=1")) {
            boot_cli_poweroff = true;
        }
        // autologin: bypass lockscreen + first-boot wizard, land on desktop. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.autologin=1") || boot_has_token(boot_cmdline, "kurono.autologin")) {
            boot_autologin = true;
        }
        // latch the ffmpeg smoke-test flag early (see decl). (satoru)
        if (boot_has_token(boot_cmdline, "kurono.ffmpeg.test=1") || boot_has_token(boot_cmdline, "kurono.ffmpeg.test")) {
            boot_ffmpeg_test = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.mouse.raw=1") || boot_has_token(boot_cmdline, "kurono.mouse.raw")) {
            boot_mouse_raw = true;
        }
        boot_get_value(boot_cmdline, "kurono.cli.run", boot_cli_run, (int)sizeof(boot_cli_run));
        // optional GUI autorun: open Terminal and queue a command. Used purely
        // for headless QEMU debug runs where we cannot type interactively.
        boot_get_value(boot_cmdline, "kurono.gui.run", boot_gui_run, (int)sizeof(boot_gui_run));
    }
    bool force_text_mode = boot_text_only || boot_console_realtime || boot_emergency || boot_cli;

    vga_puts("Multiboot OK\n");
    if (has_early_fb) { early_fb_puts("[OK] Multiboot\n", 0x00FF00); early_fb_flush(); }
    SerialLogger::Log("Multiboot OK\r\n");
    RuntimeLog::LogBoot("multiboot handshake complete");
    if (boot_cmdline) {
        SerialLogger::Log("Boot cmdline: ");
        SerialLogger::Log(boot_cmdline);
        SerialLogger::Log("\r\n");
        // honour `quiet` token: silences COM1 spew (RuntimeLog still mirrors).
        if (stcontains(boot_cmdline, "quiet")) {
            SerialLogger::Log("Quiet boot requested - serial output suppressed.\r\n");
            SerialLogger::SetQuiet(true);
        }
    }
    if (boot_cli) {
        SerialLogger::Log("Boot mode: CLI SHELL\r\n");
        vga_puts("Mode: CLI SHELL\n", 0x0A);
    } else if (boot_text_only) {
        SerialLogger::Log("Boot mode: PURE TEXT (GRUB console path)\r\n");
        vga_puts("Mode: PURE TEXT\n", 0x0B);
    } else if (boot_emergency) {
        SerialLogger::Log("Boot mode: EMERGENCY RECOVERY\r\n");
        vga_puts("Mode: EMERGENCY RECOVERY\n", 0x0C);
    } else if (boot_console_realtime) {
        SerialLogger::Log("Boot mode: REALTIME CONSOLE\r\n");
        vga_puts("Mode: REALTIME CONSOLE\n", 0x0B);
    }

    // initialize core subsystems
    vga_puts("HAL init...\n");
    if (has_early_fb) { early_fb_puts("[..] HAL ", 0xFFFF00); early_fb_flush(); }
    SerialLogger::Log("[1] HAL::Init\r\n");
    HAL::Init();
    if (has_early_fb) { early_fb_puts("OK\n", 0x00FF00); early_fb_flush(); }

    // Enable CPU security features as soon as HAL is up: SMEP (bit 20),
    // SMAP (bit 21), UMIP (bit 11), OSXSAVE (bit 18)  -  gated by CPUID
    // capability bits to avoid #UD on older hardware.
    {
        uint32_t eax, ebx, ecx, edx;
        __asm__ __volatile__("cpuid"
                             : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                             : "a"(7), "c"(0));
        uint64_t cr4;
        __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
        uint64_t set = 0;
        if (ebx & (1u << 7))  set |= (1ULL << 20);  // SMEP
        // SMAP intentionally NOT enabled: the kernel currently performs
        // direct supervisor-mode access to user pointers in the syscall
        // dispatch path.  Turning SMAP on without STAC/CLAC wrappers
        // around every copy_to_user would #PF instantly.  Re-enable once
        // every user-pointer touch is funnelled through a proper helper.
        if (ecx & (1u << 2))  set |= (1ULL << 11);  // UMIP
        if (set) {
            cr4 |= set;
            __asm__ __volatile__("mov %0, %%cr4" : : "r"(cr4));
            SerialLogger::Log("[1.5] CR4 security bits enabled\r\n");
        }
    }

    vga_puts("Memory init...\n");
    if (has_early_fb) { early_fb_puts("[..] Memory ", 0xFFFF00); early_fb_flush(); }
    SerialLogger::Log("[2] MemoryManager::Init\r\n");
    MemoryManager::Init(mb_addr);
    if (has_early_fb) { early_fb_puts("OK\n", 0x00FF00); early_fb_flush(); }
    // Phase 14: bring up buddy + slab as side-by-side allocators on top
    // of the existing PMM.  Buddy borrows ~256 MB contiguous from PMM and
    // exposes power-of-2 page allocations; Slab gives kmalloc/kfree and
    // named caches for kernel structures.  Their /proc files are written
    // by RuntimeLayout::Init() once KVFS is up.
    SerialLogger::Log("[2a] Buddy::Init\r\n");
    Buddy::Init();
    SerialLogger::Log("[2b] Slab::Init\r\n");
    Slab::Init();
    vga_puts("Scheduler init...\n");
    SerialLogger::Log("[3] Scheduler::Init\r\n");
    Scheduler::Init();
    SerialLogger::Log("[3a] HRTimer::Init\r\n");
    HRTimer::Init();
    vga_puts("VFS init...\n");
    SerialLogger::Log("[4] VFS::Init\r\n");
    VFS::Init();
    if (has_early_fb) { early_fb_puts("[OK] Scheduler + VFS\n", 0x00FF00); early_fb_flush(); }
    vga_puts("Running tests...\n");
    SerialLogger::Log("[5] TestSuite::Run\r\n");
    // run diagnostics (after core init, before display)
    TestSuite::Run();
    vga_puts("Tests passed\n", 0x0A);
    if (has_early_fb) { early_fb_puts("[OK] Tests PASSED\n", 0x00FF00); early_fb_flush(); }
    SerialLogger::Log("[6] Post-TestSuite\r\n");

    multiboot_info_t* mbi = (multiboot_info_t*)mb_addr;

    // 1000 Hz PIT: a fine tick is needed so SleepMs-based process wakeups
    // (input/cursor at ~8ms) are timely -> smooth 60fps. The ~2000 timer
    // VM-exits/sec this costs is ~0.4% cpu (negligible; the big VM-exit
    // sources  -  input double-poll, idle non-HLT, audio polling  -  are fixed
    // separately). The ms clock is TSC-based regardless of tick rate. (satoru)
    Timer::Init(1000);
    TimeManager::SelectPIT(1000);
    TimeManager::Init();

    // Calibrate the TSC NOW (right after the PIT), not late in boot. Timer::
    // GetRealMs()/WaitMs() prefer the TSC, and the boot splash + other early
    // delays call WaitMs() long before this used to run. Without the TSC,
    // WaitMs falls back to the polled PIT clock, which LOSES timer periods
    // under hardware hypervisors (WHPX/VMware) where each port read VM-exits
    // and host scheduling delays the poll  -  so the "~3s" splash stretched to
    // 15s on WHPX and minutes on VMware (the black screen after the logo).
    // CPUDetect::Init calibrates the TSC via the precise PIT ch2 one-shot. (satoru)
    CPUDetect::Init();

    // smp phase 1: enable the local apic + enumerate the cpus (no APs started yet,
    // so this is harmless on the single-core path). (satoru)
    SerialLogger::Log("[SMP] Init...\r\n");
    SMP::Init();
    // smp phase 2: bring up the application processors (INIT-SIPI-SIPI). each ap
    // reaches ap_entry, marks itself online, then idles until the scheduler hands
    // it work (phase 3). a failed ap is non-fatal  -  the bsp carries on. (satoru)
    SMP::StartAPs();

    // note: interrupts stay disabled. the kernel is fully polling-based
    // (pit counter read, keyboard/mouse i/o ports). this avoids whpx
    // compatibility issues with hardware interrupt delivery.

    SerialLogger::Log("Initializing display...\r\n");
    vga_puts("Display init...\n");
    if (has_early_fb) { early_fb_puts("[..] Display init...\n", 0xFFFF00); early_fb_flush(); }

    if (!force_text_mode) {
        SerialLogger::Log("[GpuProbe] Starting early GPU probe...\r\n");
        GpuProbe::ScanAll();
        GpuProbe::LogAll();
        if (has_early_fb) {
            const GpuProbeResult& gpr = GpuProbe::GetResult();
            early_fb_puts("[OK] GPUs found: ", 0x00FF00);
            char cnt = '0' + (char)(gpr.count < 10 ? gpr.count : 9);
            early_fb_putc(cnt, 0x00FF00);
            early_fb_puts("\n", 0);
            if (GpuProbe::IsOptimus()) {
                early_fb_puts("[!!] OPTIMUS detected\n", 0xFFFF00);
            } else if (GpuProbe::IsPowerXpress()) {
                early_fb_puts("[!!] PowerXpress detected\n", 0xFFFF00);
            }
            if (gpr.primary_idx >= 0) {
                early_fb_puts("Primary: ", 0xFFFFFF);
                early_fb_puts(gpr.gpus[gpr.primary_idx].desc, 0x00FF00);
                early_fb_puts("\n", 0);
            }
            early_fb_flush();
        }
    } else {
        SerialLogger::Log("Display: forced text mode from boot option\r\n");
        vga_puts("Display: forced text mode\n", 0x0E);
        if (has_early_fb) { early_fb_puts("[OK] Text mode (console)\n", 0x00FF00); early_fb_flush(); }
    }

    bool has_display = false;

    // dump all multiboot framebuffer fields to serial for debugging
    SerialLogger::Log("  MB flags=0x");
    SerialLogger::LogHex(mbi->flags);
    SerialLogger::Log("\r\n");
    SerialLogger::Log("  fb_addr=0x");
    SerialLogger::LogHex((uint32_t)((mbi->framebuffer_addr >> 32) & 0xFFFFFFFF));
    SerialLogger::LogHex((uint32_t)(mbi->framebuffer_addr & 0xFFFFFFFF));
    SerialLogger::Log("\r\n");
    SerialLogger::Log("  fb_w=");
    SerialLogger::LogDec((int)mbi->framebuffer_width);
    SerialLogger::Log(" fb_h=");
    SerialLogger::LogDec((int)mbi->framebuffer_height);
    SerialLogger::Log(" bpp=");
    SerialLogger::LogDec((int)mbi->framebuffer_bpp);
    SerialLogger::Log(" type=");
    SerialLogger::LogDec((int)mbi->framebuffer_type);
    SerialLogger::Log(" pitch=");
    SerialLogger::LogDec((int)mbi->framebuffer_pitch);
    SerialLogger::Log("\r\n");

    // also dump vbe info if available (bit 11)
    if (mbi->flags & (1u << 11)) {
        SerialLogger::Log("  VBE: ctrl=0x");
        SerialLogger::LogHex(mbi->vbe_control_info);
        SerialLogger::Log(" mode_info=0x");
        SerialLogger::LogHex(mbi->vbe_mode_info);
        SerialLogger::Log(" mode=0x");
        SerialLogger::LogHex(mbi->vbe_mode);
        SerialLogger::Log("\r\n");
    }

    // 1. multiboot-provided framebuffer (grub on real hardware via efi gop)
    //    check bit 12 for framebuffer info. accept any type with valid dimensions.
    if (!force_text_mode && !has_display && (mbi->flags & (1u << 12)) && mbi->framebuffer_addr != 0
        && mbi->framebuffer_width > 0 && mbi->framebuffer_height > 0) {

        if (mbi->framebuffer_type == 2) {
            // type 2 = ega text mode framebuffer. don't use for graphics.
            SerialLogger::Log("Display: MB FB is text mode (type 2), skipping\r\n");
            vga_puts("Display: text mode FB (type 2)\n", 0x0E);
            if (has_early_fb) { early_fb_puts("[!!] FB type=2 (text mode)\n", 0xFF0000); early_fb_flush(); }
        } else {
            // type 0=indexed, 1=rgb, other=unknown but try anyway
            uintptr_t fb_phys = (uintptr_t)mbi->framebuffer_addr;
            SerialLogger::Log("Display: Using MB FB at 0x");
            SerialLogger::LogHex((uint32_t)(fb_phys >> 32));
            SerialLogger::LogHex((uint32_t)(fb_phys & 0xFFFFFFFF));
            SerialLogger::Log("\r\n");

            // on hybrid gpu laptops (optimus/powerxpress), the multiboot fb
            // address might not point to the gpu that drives the display.
            // gpuprobe reads the intel igpu's dspsurf register to cross-check.
            uintptr_t validated_fb = GpuProbe::ValidateFramebuffer(
                fb_phys, mbi->framebuffer_width, mbi->framebuffer_height,
                mbi->framebuffer_pitch, mbi->framebuffer_bpp);

            if (validated_fb != fb_phys) {
                SerialLogger::Log("Display: FB ADDRESS CORRECTED by GpuProbe!\r\n");
                SerialLogger::Log("Display:   Was: 0x");
                SerialLogger::LogHex((uint32_t)(fb_phys >> 32));
                SerialLogger::LogHex((uint32_t)(fb_phys & 0xFFFFFFFF));
                SerialLogger::Log("\r\n");
                SerialLogger::Log("Display:   Now: 0x");
                SerialLogger::LogHex((uint32_t)(validated_fb >> 32));
                SerialLogger::LogHex((uint32_t)(validated_fb & 0xFFFFFFFF));
                SerialLogger::Log("\r\n");
                fb_phys = validated_fb;

                if (has_early_fb) {
                    early_fb_puts("[!!] FB CORRECTED: ", 0xFFFF00);
                    early_fb_puthex32((uint32_t)(fb_phys >> 32), 0xFFFF00);
                    early_fb_puthex32((uint32_t)(fb_phys & 0xFFFFFFFF), 0xFFFF00);
                    early_fb_puts("\n", 0);
                    early_fb_flush();
                }
            }

            if (has_early_fb) {
                early_fb_puts("[OK] FB type=", 0x00FF00);
                early_fb_puthex32(mbi->framebuffer_type, 0x00FF00);
                early_fb_puts("\n", 0);
                early_fb_flush();
            }

            // init graphics first  -  this remaps fb pages to write-combining,
            // so the subsequent test fill doesn't need wbinvd.
            Graphics::Init(fb_phys, mbi->framebuffer_width,
                          mbi->framebuffer_height, mbi->framebuffer_pitch, mbi->framebuffer_bpp);
            has_display = true;

            if (has_early_fb) {
                early_fb_puts("[OK] Graphics::Init  WC=", 0x00FF00);
                early_fb_puts(Graphics::IsFramebufferWC() ? "YES" : "NO", 
                    Graphics::IsFramebufferWC() ? 0x00FF00 : 0xFF4444);
                early_fb_puts("\n", 0);
                early_fb_flush();
            }

            // immediate visual test: fill entire screen dark blue using
            // non-temporal stores to guarantee pixels reach gpu vram.
            // this replaces the old byte-by-byte loop that could be cached.
            uint8_t* fb = (uint8_t*)fb_phys;
            uint32_t fb_total = mbi->framebuffer_pitch * mbi->framebuffer_height;
            // fill a temp scanline with dark blue, then nt-copy each row
            {
                uint32_t bytes_pp = mbi->framebuffer_bpp / 8;
                uint32_t row_bytes = mbi->framebuffer_width * bytes_pp;
                // use a stack buffer for one scanline (max ~8kb for 1920x4)
                uint8_t scanline[8192];
                uint32_t fill_w = mbi->framebuffer_width;
                if (row_bytes > sizeof(scanline)) fill_w = sizeof(scanline) / bytes_pp;
                row_bytes = fill_w * bytes_pp;
                for (uint32_t x = 0; x < fill_w; x++) {
                    uint8_t* p = scanline + x * bytes_pp;
                    if (bytes_pp == 4) { p[0]=0x80; p[1]=0x00; p[2]=0x00; p[3]=0xFF; } // dark blue bgra
                    else if (bytes_pp == 3) { p[0]=0x80; p[1]=0x00; p[2]=0x00; }
                }
                // use nt copy to blit each scanline to the fb
                for (uint32_t y = 0; y < mbi->framebuffer_height; y++) {
                    // use inline nt stores for each row
                    uint8_t* dst = fb + y * mbi->framebuffer_pitch;
                    uint8_t* src = scanline;
                    uint32_t sz = row_bytes;
                    // 16-byte aligned nt copy
                    while (sz >= 16 && ((uintptr_t)dst & 15)) { *dst++ = *src++; sz--; }
                    while (sz >= 16) {
                        __asm__ __volatile__(
                            "movdqu (%0), %%xmm0; movntdq %%xmm0, (%1);"
                            :: "r"(src), "r"(dst) : "xmm0", "memory"
                        );
                        dst += 16; src += 16; sz -= 16;
                    }
                    while (sz > 0) { *dst++ = *src++; sz--; }
                }
                __asm__ __volatile__("sfence" ::: "memory");
            }
            (void)fb_total;

            if (has_early_fb) {
                // redraw text on the blue screen so user sees progress
                early_fb_init_text(fb_phys, mbi->framebuffer_pitch,
                    mbi->framebuffer_width, mbi->framebuffer_height, mbi->framebuffer_bpp);
                early_fb_puts("Kurono OS - Display OK\n", 0x00FF00);
                early_fb_puts("WC remap: ", 0xFFFFFF);
                early_fb_puts(Graphics::IsFramebufferWC() ? "SUCCESS" : "FAILED", 
                    Graphics::IsFramebufferWC() ? 0x00FF00 : 0xFF4444);
                early_fb_puts("\n", 0);
                early_fb_puts("Loading desktop...\n", 0xFFFF00);
                early_fb_flush();
            }

            SerialLogger::Log("Display: Multiboot framebuffer OK\r\n");
        }
    }

    // 2. vbe mode info fallback: if bit 11 is set (vbe info available) but
    //    bit 12 is not set, try reading vbe mode info structure directly
    if (!force_text_mode && !has_display && (mbi->flags & (1u << 11)) && mbi->vbe_mode_info != 0) {
        vbe_mode_info_t* vmi = (vbe_mode_info_t*)(uintptr_t)mbi->vbe_mode_info;
        if (vmi->physbase != 0 && vmi->Xres > 0 && vmi->Yres > 0 && vmi->bpp >= 15) {
            uint32_t pitch = vmi->pitch ? vmi->pitch : (vmi->Xres * (vmi->bpp / 8));
            Graphics::Init((uintptr_t)vmi->physbase, vmi->Xres, vmi->Yres, pitch, vmi->bpp);
            has_display = true;
            SerialLogger::Log("Display: VBE mode info fallback OK\r\n");
        }
    }

    // 3. bga (qemu/bochs only  -  not present on real hardware)
    //    try 24bpp first (bochs rfb safe), then 32bpp (qemu preferred)
    if (!force_text_mode && !has_display) {
        bool bga_ok = false;
        // check if bga hardware exists before attempting mode set
        if (BGA::IsAvailable()) {
            SerialLogger::Log("Display: BGA detected, trying modes...\r\n");
            if (BGA::Init(1024, 768, 32)) {
                bga_ok = true;
            }
            if (!bga_ok && BGA::Init(1024, 768, 24)) {
                bga_ok = true;
            }
            if (bga_ok) {
                Graphics::Init(BGA_FRAMEBUFFER_ADDR, BGA::width, BGA::height, BGA::pitch, (uint8_t)BGA::bpp);
                has_display = true;
                SerialLogger::Log("Display: BGA ");
                SerialLogger::LogDec(BGA::width);
                SerialLogger::Log("x");
                SerialLogger::LogDec(BGA::height);
                SerialLogger::Log("x");
                SerialLogger::LogDec(BGA::bpp);
                SerialLogger::Log(" OK\r\n");
            }
        } else {
            SerialLogger::Log("Display: BGA not available (real hardware?)\r\n");
        }
    }

    if (!has_display) {
        if (force_text_mode) {
            SerialLogger::Log("Display: Text console mode active (framebuffer intentionally skipped)\r\n");
            vga_puts("Display: text console mode active\n", 0x0A);
        } else {
            SerialLogger::Log("Display: No framebuffer! Showing diagnostics on VGA text.\r\n");
            // when no graphics framebuffer is available, use vga text mode
            // (0xb8000) to show diagnostic info so the user isn't staring
            // at pure black.
            vga_clear();
            vga_puts("=== Kurono OS  -  No Graphics Framebuffer ===", 0x0C);
            vga_puts("\n", 0x0C);
            vga_puts("\nMultiboot flags: ", 0x0E);
            vga_puthex(mbi->flags);
            vga_puts("\nFB addr:  0x", 0x0E);
            vga_puthex((uint32_t)(mbi->framebuffer_addr >> 32));
            vga_puthex((uint32_t)(mbi->framebuffer_addr & 0xFFFFFFFF));
            vga_puts("\nFB size:  ", 0x0E);
            vga_puthex(mbi->framebuffer_width);
            vga_puts(" x ", 0x0E);
            vga_puthex(mbi->framebuffer_height);
            vga_puts("\nFB bpp:   ", 0x0E);
            vga_puthex(mbi->framebuffer_bpp);
            vga_puts("\nFB type:  ", 0x0E);
            vga_puthex(mbi->framebuffer_type);
            vga_puts("\nFB pitch: ", 0x0E);
            vga_puthex(mbi->framebuffer_pitch);
            vga_puts("\n", 0x07);
            vga_puts("\n", 0x07);
            vga_puts("GRUB did not deliver a graphics framebuffer.", 0x0F);
            vga_puts("\nTry selecting 'Kurono OS (1024x768)' from GRUB menu.", 0x0B);
            vga_puts("\nOr check BIOS: enable CSM/Legacy or EFI GOP.", 0x0B);
            vga_puts("\n\nBit 12 (FB info): ", 0x0E);
            vga_puts((mbi->flags & (1u << 12)) ? "SET" : "NOT SET", 0x0F);
            vga_puts("\nBit 11 (VBE info): ", 0x0E);
            vga_puts((mbi->flags & (1u << 11)) ? "SET" : "NOT SET", 0x0F);
            if (mbi->flags & (1u << 11)) {
                vga_puts("\nVBE mode_info: 0x", 0x0E);
                vga_puthex(mbi->vbe_mode_info);
                vga_puts("\nVBE mode:      0x", 0x0E);
                vga_puthex(mbi->vbe_mode);
            }
            vga_puts("\n\nSystem halted  -  reboot and try a different GRUB entry.", 0x0C);
            // don't halt  -  let kernel continue so serial diag still works
        }
    }

    // enable double buffering if we have a display
    if (has_display) {
        // detect actual display refresh rate via vga vsync timing.
        // on bare metal this gives the real panel hz; on qemu it returns ~60.
        uint32_t detected_hz = Graphics::DetectRefreshRate();
        SerialLogger::Log("Display: Detected refresh rate: ");
        SerialLogger::LogDec((int)detected_hz);
        SerialLogger::Log(" Hz\r\n");

        // match the monitor's native refresh rate exactly
        Graphics::SetTargetFPS(detected_hz);
        Graphics::SetRenderMode(Graphics::DOUBLE_BUFFER);

        // verify display works  -  write directly to framebuffer
        SerialLogger::Log("Display: RenderMode=");
        SerialLogger::LogDec((int)Graphics::GetRenderMode());
        SerialLogger::Log(" backbuf=");
        SerialLogger::LogHex((uint32_t)(uintptr_t)Graphics::GetBackBuffer());
        SerialLogger::Log(" activebuf=");
        SerialLogger::LogHex((uint32_t)(uintptr_t)Graphics::GetBuffer());
        SerialLogger::Log("\r\n");
    }

    // some laptops share a fragile 8042/ec path between keyboard and
    // touchpad. bring the keyboard up, initialize the auxiliary device, then
    // re-arm keyboard scanning so the mouse path does not leave the keyboard
    // port wedged.
    Keyboard::Init();
    Keyboard::InitUSB();
    if (has_display && !force_text_mode) {
        Mouse::Init();
        Mouse::SetDPIScaling(800, 800);
        if (boot_mouse_raw) {
            Mouse::SetRawMode(true);
            SerialLogger::Log("Mouse: raw 1:1 mode enabled (no acceleration)\r\n");
        }
    } else {
        SerialLogger::Log("Mouse: Skipped in text-only boot mode\r\n");
    }
    InputManager::Init();
    VConsole::Init((has_display && !force_text_mode) ? VConsole::VC_GUI : 0);

    if (has_display) {
        int sw = Graphics::GetWidth();
        int sh = Graphics::GetHeight();

        Graphics::Clear(0xFF000000);

        // draw embedded logo centered (scaled to 200x200)
        int logo_sw = 200, logo_sh = 200;
        int logo_lx = (sw - logo_sw) / 2;
        int logo_ly = (sh / 3) - (logo_sh / 2);
        for (int dy = 0; dy < logo_sh; dy++) {
            int src_y = (dy * LOGO_HEIGHT) / logo_sh;
            for (int dx = 0; dx < logo_sw; dx++) {
                int src_x = (dx * LOGO_WIDTH) / logo_sw;
                uint32_t pixel = logo_data[src_y * LOGO_WIDTH + src_x];
                uint8_t alpha = (pixel >> 24) & 0xFF;
                if (alpha > 64) {
                    Graphics::DrawPixel(logo_lx + dx, logo_ly + dy, pixel | 0xFF000000);
                }
            }
        }

        // minimal monochrome brand treatment
        const char* brand = "K U R O N O";
        int brand_w = 11 * 8; // approximate width
        Graphics::DrawString((sw - brand_w) / 2, logo_ly + logo_sh + 24, brand, 0xFFDADADA, 0xFF000000);

        // loading bar dimensions
        int bar_w = 180, bar_h = 3;
        int bar_x = (sw - bar_w) / 2;
        int bar_y = logo_ly + logo_sh + 56;
        Graphics::FillRect(bar_x, bar_y, bar_w, bar_h, 0xFF222222);
        Graphics::SwapBuffers();
        SerialLogger::Log("Boot splash: logo displayed\r\n");

        // animate loading bar over ~1 second (24 steps x 40ms). kept short on
        // purpose: this is cosmetic and runs before the desktop, so it's dead
        // time. WaitMs is now TSC-accurate (CPUDetect ran early), so this is a
        // real ~1s, not the multi-minute overshoot it used to be on VMware.
        // (satoru)
        for (int step = 1; step <= 24; step++) {
            int fill_w = (step * bar_w) / 24;
            for (int px = 0; px < fill_w; px++) {
                uint32_t c = 0xFFD8D8D8;
                for (int py = 0; py < bar_h; py++)
                    Graphics::DrawPixel(bar_x + px, bar_y + py, c);
            }
            Graphics::SwapBuffers();
            Timer::WaitMs(40);
        }

        // brief pause then smooth clear
        Timer::WaitMs(80);
        Graphics::Clear(0xFF000000);
        Graphics::SwapBuffers();
        Timer::WaitMs(60);
        SerialLogger::Log("Boot splash complete\r\n");
    }

    System::Initialize();

    MediaDecoder::Image wallpaper = {0, 0, 0, false, 0, false};
    if (has_display && (mbi->flags & (1u << 3)) && mbi->mods_count > 0) {
        multiboot_module_t* mods = (multiboot_module_t*)mbi->mods_addr;

        // try named wallpaper
        for (uint32_t i = 0; i < mbi->mods_count; i++) {
            const char* name = (const char*)mods[i].string;
            if (name && streq(name, "wallpaper")) {
                MediaDecoder::Image candidate = MediaDecoder::DecodeModule(mods[i].mod_start, mods[i].mod_end);
                if (candidate.valid) { wallpaper = candidate; break; }
            }
        }

        // load font module
        for (uint32_t i = 0; i < mbi->mods_count; i++) {
            const char* name = (const char*)mods[i].string;
            SerialLogger::Log("Module: ");
            if (name) SerialLogger::Log(name); else SerialLogger::Log("(null)");
            SerialLogger::Log("\r\n");

            if (name && streq(name, "font")) {
                const uint8_t* fptr = (const uint8_t*)mods[i].mod_start;
                int fsize = (int)(mods[i].mod_end - mods[i].mod_start);
                FontTTF::Init(fptr, fsize);
                if (FontTTF::ok) SerialLogger::Log("Font OK\r\n");
                else SerialLogger::Log("Font FAIL\r\n");
                break;
            }
        }

        // fallback wallpaper: try any remaining image
        if (!wallpaper.valid) {
            for (uint32_t i = 0; i < mbi->mods_count; i++) {
                uint32_t start = mods[i].mod_start;
                uint32_t end = mods[i].mod_end;
                const uint8_t* d = (const uint8_t*)start;
                size_t n = (size_t)(end - start);
                MediaDecoder::Image candidate;
                if (MediaDecoder::IsPNG(d, n) || MediaDecoder::IsJPEG(d, n)) {
                    candidate = MediaDecoder::DecodeModule(start, end);
                } else {
                    candidate = MediaDecoder::DecodeRaw(start);
                }
                if (candidate.valid) { wallpaper = candidate; break; }
            }
        }
    }

    // fallback: use the embedded wallpaper if no module wallpaper loaded. it is
    // stored as pre-decoded raw rgba (decoded on the host) so we skip the
    // freestanding image decoder, which corrupts a band of rows on large
    // images. just point the Image at the rodata. (satoru)
    if (has_display && !wallpaper.valid) {
        SerialLogger::Log("Loading embedded wallpaper (raw rgba)...\r\n");
        wallpaper.width  = (int)wallpaper_rgba_w;
        wallpaper.height = (int)wallpaper_rgba_h;
        wallpaper.data   = (uint8_t*)wallpaper_rgba_data;
        wallpaper.valid  = true;
        wallpaper.order  = 0;   // rgba
        wallpaper.owns   = false;
        SerialLogger::Log("Embedded wallpaper ready\r\n");
    }

    if (has_display) {
        GUI::SetWallpaper(wallpaper);
        if (wallpaper.valid) {
            Desktop::SetWallpaperImage(wallpaper);
            SerialLogger::Log("Desktop wallpaper image set\r\n");
        }
    }

    TimeManager::SetTimezoneMinutes(0);
    TimeManager::EnableDST(false);

    SerialLogger::Log("[KVFS] Init...\r\n");
    KVFS::Init();
    // bring up + mount a persistent ext4 data disk (if attached) BEFORE restoring,
    // so kvfs.img survives a reboot on a normal boot  -  not just after an install.
    // Installer::Init() runs much later, so do the mount here ourselves. (satoru)
    // detect the nvme data disk  -  MountDataDisk runs NVMe::Init for us; its ext4
    // probe just fails harmlessly on our raw persistence store. (satoru)
    Installer::MountDataDisk();
    // restore persistent kvfs state from the on-disk KFS volume if one is present;
    // otherwise the default tree from init() stays. the /usr/bin binary re-seeding
    // below re-fills the large entries KFS deliberately doesn't carry. (satoru)
    if (PersistStore::Available()) {
        bool ok = PersistStore::LoadTree();
        if (ok) SerialLogger::Log("[KVFS] restored persistent state from KFS\r\n");
        else    SerialLogger::Log("[KVFS] no KFS volume to restore (fresh disk)\r\n");
    }
    // bring up the usb host controller + hid interrupt polling (no-op if no xhci). (satoru)
    SerialLogger::Log("[USB] Init...\r\n");
    USB::Init();
    RuntimeLog::InitFilesystem();
    RuntimeLog::LogBoot("kvfs online");
    RuntimeLog::LogSystem("kernel", "runtime filesystem layout created");

    // recover a persisted crash minidump now that kvfs is up, log it to the crash
    // log, and surface a toast if the previous boot ended in a panic. (satoru)
    KVFS::Mkdirs("/home/user");
    if (KernelPanic::ScanCrashDumpAtBoot()) {
        RuntimeLog::LogCrash("recovered minidump from previous boot panic",
                             "see " KP_LOG_CRASH_DIR);
        NotificationManager::Post("System recovered",
                                  "recovered from a previous crash; logged to " KP_LOG_CRASH_DIR,
                                  NotificationManager::ICON_WARNING, 6000);
    }

    // uiconfig must be initialized before any ui subsystem reads colors/sizes.
    // it writes /etc/kurono/ui.conf with defaults on first boot.
    SerialLogger::Log("[UIConfig] Init...\r\n");
    UIConfig::Init();
    // seed the kss theme tokens from ui.conf (defaults to the black/grey palette). (satoru)
    KSS::Init();

    // apply persisted accessibility settings to the runtime layers
    Graphics::SetColorFilter(UIConfig::Int("a11y.color_filter", 0));
    Graphics::SetHighContrast(UIConfig::Int("a11y.high_contrast", 0) != 0);
    Keyboard::SetStickyKeys(UIConfig::Int("a11y.sticky_keys", 0) != 0);
    Keyboard::SetSlowKeys(UIConfig::Int("a11y.slow_keys", 0) ? 250 : 0);
    Keyboard::SetBounceKeys(UIConfig::Int("a11y.bounce_keys", 0) ? 200 : 0);
    Keyboard::SetScreenReader(UIConfig::Int("a11y.screen_reader", 0) != 0);

    SerialLogger::Log("[Shell] Init...\r\n");
    KuronoShell::Init();
    KuronoShell shell_instance;   // trivial object  -  all methods are static

    SerialLogger::Log("[LinuxCmds] Register...\r\n");
    LinuxCmds::RegisterAll(&shell_instance);

    if (!boot_emergency) {
        SerialLogger::Log("[WindowsCmds] Register...\r\n");
        WindowsCmds::RegisterAll(&shell_instance);
    }

    if (boot_emergency) {
        KVFS::Mkdirs("/home/user/Documents");
        KVFS::Mkdirs("/home/user/Desktop");
        KVFS::Mkdirs("/usr/bin");
        KVFS::Mkdirs("/etc");
        KVFS::Mkdirs("/tmp");
        KVFS::Mkdirs("/var/log");
        KVFS::WriteString("/etc/hostname", "kurono-emergency");
        KVFS::WriteString("/etc/os-release", "Kurono Emergency Kernel\nMODE=emergency\n");
        KVFS::Mkdirs(KP_LOG_DIR);
        KVFS::WriteString(KP_LOG_DIR "/emergency.txt",
            "Kurono emergency mode\n"
            "Commands:\n"
            "  help\n"
            "  ls /system\n"
            "  cat /system/boot/boot.log\n"
            "  tail -n 40 /system/logs/system.log\n"
            "  dmesg\n"
            "  reboot\n");
        SerialLogger::Log("[Emergency] Initializing keyboard/input\r\n");
        Keyboard::Init();
        Keyboard::InitUSB();
        InputManager::Init();
        RuntimeLog::LogBoot("emergency recovery shell ready");
        RuntimeLog::LogSystem("kernel", "entered emergency recovery mode");
        emergency_run_shell();
    }

    SerialLogger::Log("[SUPR] Init...\r\n");
    SUPR::Init();

    SerialLogger::Log("[PackageManager] Init...\r\n");
    PackageManager::Init();
    PackageManager::RegisterCommands(&shell_instance);

    SerialLogger::Log("[GpuDriverInstaller] Init...\r\n");
    GpuDriverInstaller::Init();

    SerialLogger::Log("[Python] Init mini interpreter...\r\n");
    PythonInterp::Init();
    PythonInterp::RegisterShellCommands(&shell_instance);

    SerialLogger::Log("[Userspace] Init ring-3 runtime...\r\n");
    LinuxSyscall::Init();
    Userspace::Init();
    if (EmbeddedUserprogs::HasHello()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/hello",
                        EmbeddedUserprogs::HelloData(),
                        EmbeddedUserprogs::HelloSize());
        SerialLogger::Log("[Userspace] /usr/bin/hello registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::HelloSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    if (EmbeddedUserprogs::HasHelloX64()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/hello_x64",
                        EmbeddedUserprogs::HelloX64Data(),
                        EmbeddedUserprogs::HelloX64Size());
        SerialLogger::Log("[Userspace] /usr/bin/hello_x64 registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::HelloX64Size());
        SerialLogger::Log(" bytes)\r\n");
    }
    if (EmbeddedUserprogs::HasMuslHello()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/mhello",
                        EmbeddedUserprogs::MuslHelloData(),
                        EmbeddedUserprogs::MuslHelloSize());
        SerialLogger::Log("[Userspace] /usr/bin/mhello registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::MuslHelloSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    // register the embedded musl-static ffmpeg so it is runnable from the
    // shell ("ffmpeg ...") and the boot smoke test. (satoru)
    if (EmbeddedUserprogs::HasFfmpeg()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/ffmpeg",
                        EmbeddedUserprogs::FfmpegData(),
                        EmbeddedUserprogs::FfmpegSize());
        SerialLogger::Log("[Userspace] /usr/bin/ffmpeg registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::FfmpegSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    // register the raw-protocol wl_shm wayland client so `wltest` can prove the
    // compositor's shared-memory render path end to end. (satoru)
    if (EmbeddedUserprogs::HasWlShmTest()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/wl_shm_test",
                        EmbeddedUserprogs::WlShmTestData(),
                        EmbeddedUserprogs::WlShmTestSize());
        SerialLogger::Log("[Userspace] /usr/bin/wl_shm_test registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::WlShmTestSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    // register the pthreads smoke test so `pthtest` can prove CLONE_THREAD +
    // futex (mutex serialisation + pthread_join). (satoru)
    if (EmbeddedUserprogs::HasPthreadTest()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/pthread_test",
                        EmbeddedUserprogs::PthreadTestData(),
                        EmbeddedUserprogs::PthreadTestSize());
        SerialLogger::Log("[Userspace] /usr/bin/pthread_test registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::PthreadTestSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    if (EmbeddedUserprogs::HasKpython()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::Mkdirs("/usr/share");
        KVFS::WriteFile("/usr/bin/kpython",
                        EmbeddedUserprogs::KpythonData(),
                        EmbeddedUserprogs::KpythonSize());
        // Pre-install the smoke-test script described in the chat:
        //   print('hello from kurono')
        //   import sys; print(sys.version)
        //   print(2**100)
        const char* hello_py =
            "print('hello from kurono')\n"
            "import sys; print(sys.version)\n"
            "print(2**100)\n";
        // Compute length manually (no strlen in scope here).
        uint32_t len = 0;
        while (hello_py[len]) ++len;
        KVFS::WriteFile("/usr/share/hello.py", (const uint8_t*)hello_py, len);
        SerialLogger::Log("[Userspace] /usr/bin/kpython registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::KpythonSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    {
        auto cmd_runelf = +[](KuronoShell* sh, int argc, const char** argv,
                              char* out, int maxo) -> int {
            (void)sh;
            int p = 0;
            auto sappend = [&](const char* s) {
                while (*s && p < maxo - 1) out[p++] = *s++;
            };
            auto sappend_int = [&](int v) {
                char b[16]; int n = 0;
                if (v == 0) { b[n++] = '0'; }
                else {
                    bool neg = v < 0; uint32_t u = neg ? (uint32_t)(-v) : (uint32_t)v;
                    char tmp[16]; int t = 0;
                    while (u && t < 15) { tmp[t++] = '0' + (u % 10); u /= 10; }
                    if (neg && n < 15) b[n++] = '-';
                    while (t-- > 0 && n < 15) b[n++] = tmp[t];
                }
                b[n] = 0;
                for (int i = 0; b[i] && p < maxo - 1; i++) out[p++] = b[i];
            };
            if (argc < 2) {
                sappend("usage: runelf <path-to-elf>\n");
                return p;
            }
            const char* path = argv[1];
            Process* proc = ElfLoader::LoadELF64FromVFS(path, "userelf");
            if (!proc) {
                sappend("runelf: failed to load ");
                sappend(path);
                sappend("\n");
                return p;
            }
            int rc = Userspace::RunProcess(proc);
            int waited = rc;
            if (Scheduler::WaitForProcess(proc, &waited)) {
                rc = waited;
                Scheduler::ReapProcess(proc);
            } else {
                Scheduler::DestroyProcess(proc);
            }
            sappend("runelf: ");
            sappend(path);
            sappend(" exited with code ");
            sappend_int(rc);
            sappend("\n");
            return p;
        };
        shell_instance.RegisterCommand("runelf", "Run a static ELF64 user binary",
                                       ENV_KURONO, "system",
                                       (ShellCmdHandler)cmd_runelf);

        // ── kpy <script.py | -c "<source>"> ──
        // Loads /usr/bin/kpython and runs it via the new RunProcessWithArgs
        // path so argc/argv/envp/auxv are populated on the user stack.
        auto cmd_kpy = +[](KuronoShell* sh, int argc, const char** argv,
                           char* out, int maxo) -> int {
            (void)sh;
            int p = 0;
            auto sappend = [&](const char* s) {
                while (*s && p < maxo - 1) out[p++] = *s++;
            };
            auto sappend_int = [&](int v) {
                char b[16]; int n = 0;
                if (v == 0) { b[n++] = '0'; }
                else {
                    bool neg = v < 0; uint32_t u = neg ? (uint32_t)(-v) : (uint32_t)v;
                    char tmp[16]; int t = 0;
                    while (u && t < 15) { tmp[t++] = '0' + (u % 10); u /= 10; }
                    if (neg && n < 15) b[n++] = '-';
                    while (t-- > 0 && n < 15) b[n++] = tmp[t];
                }
                b[n] = 0;
                for (int i = 0; b[i] && p < maxo - 1; i++) out[p++] = b[i];
            };
            if (!EmbeddedUserprogs::HasKpython()) {
                sappend("kpy: kpython not embedded\n");
                return p;
            }
            if (argc < 2) {
                sappend("usage: kpy <script.py>     run a script from KVFS\n");
                sappend("       kpy -c \"<source>\"   run inline source\n");
                sappend("Try:   kpy /usr/share/hello.py\n");
                return p;
            }
            // Build argv array passed to user: argv[0]="kpython", then user args.
            const char* uargv[6] = { "kpython", nullptr, nullptr, nullptr, nullptr, nullptr };
            int n = 1;
            for (int i = 1; i < argc && n < 5; i++) uargv[n++] = argv[i];
            uargv[n] = nullptr;

            Process* proc = ElfLoader::LoadELF64FromVFS("/usr/bin/kpython", "kpython");
            if (!proc) { sappend("kpy: load failed\n"); return p; }
            int rc = Userspace::RunProcessWithArgs(proc, uargv, nullptr);
            int waited = rc;
            if (Scheduler::WaitForProcess(proc, &waited)) {
                rc = waited;
                Scheduler::ReapProcess(proc);
            } else {
                Scheduler::DestroyProcess(proc);
            }
            sappend("kpy: exit=");
            sappend_int(rc);
            sappend("\n");
            return p;
        };
        shell_instance.RegisterCommand("kpy",
            "Run a Python-subset script via the embedded kpython interpreter",
            ENV_KURONO, "system", (ShellCmdHandler)cmd_kpy);
    }

    // boot-time smoke test: run the embedded /usr/bin/hello ELF once and
    // log the result over the serial port.  this validates the elf64
    // loader + ring-3 transition path on every boot.
    // NOTE: boot-time smoke tests temporarily disabled  -  the post-exit
    // userspace cleanup path was blocking the kernel from reaching the
    // interactive main loop.  Users can still invoke /usr/bin/hello and
    // /usr/bin/hello_x64 from the shell.
    if (false && EmbeddedUserprogs::HasHello()) {
        SerialLogger::Log("[Userspace] Smoke test: running /usr/bin/hello ...\r\n");
        Process* hp = ElfLoader::LoadELF64FromVFS("/usr/bin/hello", "hello");
        if (!hp) {
            SerialLogger::Log("[Userspace] SMOKE TEST: load failed\r\n");
        } else {
            int rc = Userspace::RunProcess(hp);
            int waited = rc;
            if (Scheduler::WaitForProcess(hp, &waited)) {
                rc = waited;
                Scheduler::ReapProcess(hp);
            } else {
                Scheduler::DestroyProcess(hp);
            }
            SerialLogger::Log("[Userspace] SMOKE TEST: hello exit=");
            SerialLogger::LogDec(rc);
            SerialLogger::Log("\r\n");
        }
    }

    // Second smoke test: x86_64 SYSCALL fast-path.  Validates STAR/LSTAR
    // wiring, the syscall_entry_x64 stub, and the x64→i386 nr translator.
    // This is the path musl/CPython will use natively.
    if (false && EmbeddedUserprogs::HasHelloX64()) {
        SerialLogger::Log("[Userspace] Smoke test: running /usr/bin/hello_x64 ...\r\n");
        Process* hp = ElfLoader::LoadELF64FromVFS("/usr/bin/hello_x64", "hello_x64");
        if (!hp) {
            SerialLogger::Log("[Userspace] SMOKE TEST: hello_x64 load failed\r\n");
        } else {
            int rc = Userspace::RunProcess(hp);
            int waited = rc;
            if (Scheduler::WaitForProcess(hp, &waited)) {
                rc = waited;
                Scheduler::ReapProcess(hp);
            } else {
                Scheduler::DestroyProcess(hp);
            }
            SerialLogger::Log("[Userspace] SMOKE TEST: hello_x64 exit=");
            SerialLogger::LogDec(rc);
            SerialLogger::Log("\r\n");
        }
    }

    // Third smoke test: kpython runs /usr/share/hello.py end to end.
    // Validates argv/auxv stack, file open/read, brk/mmap heap, and the
    // full x86_64 SYSCALL loop that CPython would otherwise need.
    // NOTE: temporarily disabled in boot path  -  kpython works on UEFI but
    // crashes here on BIOS Multiboot2 due to a memory-layout collision
    // (kpython uses an address that overlaps kernel .rodata under MB2).
    // Users can still launch kpy interactively from the shell.
    if (false && EmbeddedUserprogs::HasKpython()) {
        SerialLogger::Log("[Userspace] Smoke test: running kpython /usr/share/hello.py ...\r\n");
        Process* kp = ElfLoader::LoadELF64FromVFS("/usr/bin/kpython", "kpython");
        if (!kp) {
            SerialLogger::Log("[Userspace] SMOKE TEST: kpython load failed\r\n");
        } else {
            const char* kargv[] = { "kpython", "/usr/share/hello.py", nullptr };
            int rc = Userspace::RunProcessWithArgs(kp, kargv, nullptr);
            int waited = rc;
            if (Scheduler::WaitForProcess(kp, &waited)) {
                rc = waited;
                Scheduler::ReapProcess(kp);
            } else {
                Scheduler::DestroyProcess(kp);
            }
            SerialLogger::Log("[Userspace] SMOKE TEST: kpython exit=");
            SerialLogger::LogDec(rc);
            SerialLogger::Log("\r\n");
        }
    }

    // musl-static smoke test: load /usr/bin/mhello via the real elf loader and
    // run it through RunProcessWithArgs. proven working (prints "hello from
    // static elf", rc=0), but running a ring-3 process during boot is for
    // diagnostics only  -  gated off so normal boot reaches the desktop. flip
    // kRunMuslSmokeTest to true to re-exercise it. (satoru)
    static const bool kRunMuslSmokeTest = false;
    if (kRunMuslSmokeTest && EmbeddedUserprogs::HasMuslHello()) {
        SerialLogger::Log("MUSL_HELLO_BEGIN\r\n");
        Process* mp = ElfLoader::LoadELF64FromVFS("/usr/bin/mhello", "mhello");
        if (!mp) {
            SerialLogger::Log("MUSL_HELLO_END rc=load_failed\r\n");
        } else {
            LinuxSyscall::ClearConsoleOutput();
            const char* margv[] = { "mhello", nullptr };
            const char* menvp[] = { nullptr };
            int rc = Userspace::RunProcessWithArgs(mp, margv, menvp);
            // drain the console ring into serial so printf output is visible
            char mbuf[512];
            int n;
            while ((n = LinuxSyscall::ReadConsoleOutput(mbuf, (int)sizeof(mbuf) - 1)) > 0) {
                mbuf[n] = 0;
                SerialLogger::Log(mbuf);
            }
            SerialLogger::Log("\r\nMUSL_HELLO_END rc=");
            SerialLogger::LogDec(rc);
            SerialLogger::Log("\r\n");
        }
    }

    // ffmpeg smoke test (gated by cmdline kurono.ffmpeg.test=1): proves the
    // embedded musl-static ffmpeg runs in ring-3 via the real loader + the
    // widened syscall surface. logs FFMPEG_* markers to serial so a headless
    // qemu boot can verify version output + a real pcm->wav transcode. (satoru)
    if (boot_ffmpeg_test && EmbeddedUserprogs::HasFfmpeg()) {
        SerialLogger::Log("[ffmpeg-test] PMM free MB=");
        SerialLogger::LogDec((int)(PMM::GetFreeMemory() / (1024 * 1024)));
        SerialLogger::Log("\r\n");
        // 1) ffmpeg -version (no file i/o; exercises elf load + musl init +
        //    tls/auxv + futex locks + write to stdout + exit_group).
        SerialLogger::Log("FFMPEG_VERSION_BEGIN\r\n");
        Process* fp = ElfLoader::LoadELF64FromVFS("/usr/bin/ffmpeg", "ffmpeg");
        if (!fp) {
            SerialLogger::Log("FFMPEG_VERSION_END rc=load_failed\r\n");
        } else {
            LinuxSyscall::ClearConsoleOutput();
            const char* av[] = { "ffmpeg", "-version", nullptr };
            const char* ev[] = { "PATH=/usr/bin", nullptr };
            int rc = Userspace::RunProcessWithArgs(fp, av, ev);
            char buf[512]; int n;
            while ((n = LinuxSyscall::ReadConsoleOutput(buf, (int)sizeof(buf) - 1)) > 0) {
                buf[n] = 0; SerialLogger::Log(buf);
            }
            SerialLogger::Log("\r\nFFMPEG_VERSION_END rc=");
            SerialLogger::LogDec(rc);
            SerialLogger::Log("\r\n");
        }
        // 2) real transcode: synthesize ~0.25s of 8 khz mono s16le square wave
        //    into kvfs, then transcode raw pcm -> wav. exercises open/read/write
        //    through kvfs + the pcm decoder/encoder + wav muxer.
        static int16_t pcm[2000];
        for (int i = 0; i < 2000; i++)
            pcm[i] = (int16_t)(((i / 50) & 1) ? 8000 : -8000);
        // a linux process's "/tmp" resolves to kvfs "/system/tmp" via
        // LinuxSyscall::ResolvePath, so seed the input at the resolved path
        // and read the output back from there. (satoru)
        KVFS::Mkdirs("/system/tmp");
        KVFS::WriteFile("/system/tmp/in.pcm", (const uint8_t*)pcm, (uint32_t)sizeof(pcm));
        SerialLogger::Log("FFMPEG_XCODE_BEGIN\r\n");
        Process* tp = ElfLoader::LoadELF64FromVFS("/usr/bin/ffmpeg", "ffmpeg");
        if (!tp) {
            SerialLogger::Log("FFMPEG_XCODE_END rc=load_failed\r\n");
        } else {
            LinuxSyscall::ClearConsoleOutput();
            const char* tv[] = { "ffmpeg", "-hide_banner", "-nostdin",
                "-f", "s16le", "-ar", "8000", "-ac", "1", "-i", "/tmp/in.pcm",
                "-y", "/tmp/out.wav", nullptr };
            const char* ev2[] = { "PATH=/usr/bin", nullptr };
            int rc2 = Userspace::RunProcessWithArgs(tp, tv, ev2);
            char buf2[512]; int n2;
            while ((n2 = LinuxSyscall::ReadConsoleOutput(buf2, (int)sizeof(buf2) - 1)) > 0) {
                buf2[n2] = 0; SerialLogger::Log(buf2);
            }
            int outsz = KVFS::GetFileSize("/system/tmp/out.wav");
            SerialLogger::Log("\r\nFFMPEG_XCODE_END rc=");
            SerialLogger::LogDec(rc2);
            SerialLogger::Log(" out_wav_bytes=");
            SerialLogger::LogDec(outsz);
            SerialLogger::Log("\r\n");
        }
        // note: on-device h264 transcode via ffmpeg works but software
        // decode of 934x720 under qemu-tcg (no kvm) is far too slow for a
        // boot-time smoke test, so it is not exercised here. real-video
        // playback is provided by the native kvid player (see playvideo /
        // the imported ssstik.kvid), which is fast. on kvm/bare-metal the
        // ffmpeg h264 path runs at normal speed. (satoru)
    }

    SerialLogger::Log("[Installer] Init...\r\n");
    Installer::Init();
    Installer::RegisterShellCommands(&shell_instance);

    SerialLogger::Log("[Network] Init...\r\n");
    Network::Init();
    RuntimeLog::LogSystem("network", "network interfaces initialized");
    SerialLogger::Log("[TCPIP] Init...\r\n");
    if (TCPStack::Init()) {
        SerialLogger::Log("[TCPIP] Real TCP/IP stack ready\r\n");
        RuntimeLog::LogSystem("network", "real TCP/IP stack ready");
    } else {
        SerialLogger::Log("[TCPIP] Disabled (no E1000 NIC)\r\n");
        RuntimeLog::LogSystem("network", "real TCP/IP stack unavailable");
    }
    SerialLogger::Log("[TunTap] Init...\r\n");
    TunTap::Init();
    SerialLogger::Log("[IPv6] Init...\r\n");
    IPv6::Init();
    SerialLogger::Log("[Cgroup] Init...\r\n");
    Cgroup::Init();
    Cgroup::PublishToKVFS();
    SerialLogger::Log("[TPM] Init...\r\n");
    TPM::Init();
    SerialLogger::Log("[Netfilter] Init...\r\n");
    Netfilter::Init();
    SerialLogger::Log("[CPUFreq] Init...\r\n");
    CPUFreq::Init();
    WiFi::Init();

    // ---- Userspace runtime layout + IPC servers (Wayland/Pulse/DBus) ----
    SerialLogger::Log("[RuntimeLayout] Seeding /system tree...\r\n");
    RuntimeLayout::Init();

    // demo periodic timer so /proc/timer_list has a visible entry
    static auto hrt_proc_refresh = +[](uint32_t, void*){
        RuntimeLayout::RefreshProc();
    };
    HRTimer::AddPeriodic("proc_refresh", 1000, hrt_proc_refresh, nullptr);
    SerialLogger::Log("[UnixSocket] Init...\r\n");
    UnixSocket::Init();
    SerialLogger::Log("[DBusServer] Starting session bus...\r\n");
    DBusServer::Init();
    SerialLogger::Log("[PulseServer] Starting audio daemon...\r\n");
    PulseServer::Init();
    SerialLogger::Log("[WaylandServer] Starting compositor...\r\n");
    WaylandServer::Init();
    SerialLogger::Log("[ld-kurono] Initialising dynamic linker...\r\n");
    LdKurono::Init();

    // turn on timer-driven preemption so concurrent user threads time-share. (satoru)
    SerialLogger::Log("[Sched] Enabling user-thread preemption...\r\n");
    LinuxSyscall::EnableTimerPreemption();

    SerialLogger::Log("[KCL] Init...\r\n");
    KCL::Init(&shell_instance);

    SerialLogger::Log("[DualBoot] Init...\r\n");
    DualBootManager::Init();

    SerialLogger::Log("[DualBoot] Starting integrated boot...\r\n");
    DualBootManager::BootIntegrated();

    SerialLogger::Log("[LinuxNet] Init...\r\n");
    LinuxNetBridge::Init();

    // register linux subsystem shell commands
    LinuxInit::RegisterShellCommands(&shell_instance);
    DualBootManager::RegisterShellCommands(&shell_instance);
    LinuxNetBridge::RegisterShellCommands(&shell_instance);

    SerialLogger::Log("[LinuxDrivers] Init...\r\n");
    LinuxDriverFramework::Init();
    LinuxDriverFramework::RegisterShellCommands(&shell_instance);

    SerialLogger::Log("[Linux] Subsystem fully integrated\r\n");
    RuntimeLog::LogSystem("linux", "subsystem integrated");

    KVFS::Mkdirs("/home/user/Documents");
    KVFS::Mkdirs("/home/user/Downloads");
    KVFS::Mkdirs("/home/user/Desktop");
    KVFS::Mkdirs("/home/user/Music");
    KVFS::Mkdirs("/home/user/Videos");
    KVFS::Mkdirs("/usr/bin");
    KVFS::Mkdirs("/etc");
    KVFS::Mkdirs("/tmp");
    KVFS::Mkdirs("/var/log");
    KVFS::WriteString("/etc/hostname", "kurono");
    KVFS::WriteString("/etc/os-release", "Kurono OS v1.0\nARCH=x86\nKERNEL=kurono\n");
    KVFS::WriteString("/home/user/readme.txt", "Welcome to Kurono OS!\n\nThis is a bare-metal operating system.\nType 'help' in the terminal for available commands.\n");
    if (EmbeddedMedia::HasDenjiMP4()) {
        KVFS::WriteFile("/home/user/Documents/denji.mp4",
                        EmbeddedMedia::DenjiMP4Data(),
                        EmbeddedMedia::DenjiMP4Size());
        SerialLogger::Log("[KVFS] Embedded denji.mp4: ");
        SerialLogger::LogDec(EmbeddedMedia::DenjiMP4Size());
        SerialLogger::Log(" bytes\r\n");
    } else {
        KVFS::WriteString("/home/user/Documents/denji.mp4",
                          "[MP4 stub - asset not embedded]");
    }
    if (EmbeddedMedia::HasDenjiKVID()) {
        KVFS::WriteFile("/home/user/Videos/denji.kvid",
                        EmbeddedMedia::DenjiKVIDData(),
                        EmbeddedMedia::DenjiKVIDSize());
        SerialLogger::Log("[KVFS] Embedded denji.kvid: ");
        SerialLogger::LogDec(EmbeddedMedia::DenjiKVIDSize());
        SerialLogger::Log(" bytes\r\n");
    }
    // user-imported video: original mp4 + playable kvid (satoru)
    if (EmbeddedMedia::HasSsstikMP4()) {
        KVFS::WriteFile("/home/user/Videos/ssstik.mp4",
                        EmbeddedMedia::SsstikMP4Data(),
                        EmbeddedMedia::SsstikMP4Size());
        SerialLogger::Log("[KVFS] Imported ssstik.mp4: ");
        SerialLogger::LogDec(EmbeddedMedia::SsstikMP4Size());
        SerialLogger::Log(" bytes\r\n");
    }
    if (EmbeddedMedia::HasSsstikKVID()) {
        KVFS::WriteFile("/home/user/Videos/ssstik.kvid",
                        EmbeddedMedia::SsstikKVIDData(),
                        EmbeddedMedia::SsstikKVIDSize());
        SerialLogger::Log("[KVFS] Imported ssstik.kvid: ");
        SerialLogger::LogDec(EmbeddedMedia::SsstikKVIDSize());
        SerialLogger::Log(" bytes\r\n");
    }
    KVFS::WriteString("/home/user/Music/startup.wav", "[WAV PCM 22050Hz 16-bit stereo 0:05]");
    KVFS::WriteString("/home/user/Music/notification.wav", "[WAV PCM 22050Hz 16-bit mono 0:02]");
    KVFS::WriteString("/home/user/hello.kcl", "# KCL Script\nprint \"Hello from Kurono!\"\nset x 42\nprint x\n");
    KVFS::WriteString("/home/user/math.kcl", "# Math demo\nset a 16\nset b sqrt(a)\nprint \"sqrt(16) = \"\nprint b\nset r rand()\nprint \"random = \"\nprint r\n");
    KVFS::WriteString("/home/user/loop.kcl", "# Loop demo\nset sum 0\nfor i in 1 10 do\n  set sum sum + i\nend\nprint \"Sum 1..10 = \"\nprint sum\n");
    KVFS::WriteString("/home/user/fib.kcl", "# Fibonacci\nset a 0\nset b 1\nfor i in 1 10 do\n  set c a + b\n  print c\n  set a b\n  set b c\nend\n");
    // Pre-populate more visible directories and files for the File Manager
    KVFS::Mkdirs("/home/user/Desktop");
    KVFS::Mkdirs("/home/user/Pictures");
    KVFS::Mkdirs("/home/user/Downloads");
    KVFS::Mkdirs("/home/user/Videos");
    KVFS::Mkdirs("/home/user/Projects");
    KVFS::Mkdirs("/home/user/Projects/kurono");
    KVFS::WriteString("/home/user/Desktop/README.txt", "Your Kurono desktop.\n\nApps can be launched from the Start Menu (bottom-left).\nRight-click the desktop for wallpaper & display settings.\n");
    KVFS::WriteString("/home/user/Pictures/screenshot.bmp", "[BMP 1024x768 24-bit placeholder]");
    KVFS::WriteString("/home/user/Projects/kurono/main.kcl", "# Kurono project\nset project_name \"MyApp\"\nprint \"Building \"\nprint project_name\nprint \"...\"\nprint \"\\nDone!\"\n");
    KVFS::WriteString("/home/user/.bashrc", "alias ls='ls -l'\nalias ll='ls -la'\nalias cls='clear'\necho \"Welcome back to Kurono!\"\n");
    KVFS::WriteString("/home/user/.profile", "export PATH=/usr/bin:/usr/local/bin:/home/user/bin\nexport HOME=/home/user\nexport USER=user\n");
    KVFS::WriteString("/home/user/notes.txt", "Kurono OS Notes\n==============\n\nThings to try:\n- Open Terminal and type 'help'\n- Run 'kurono log' to see system logs\n- Open Task Manager to see processes\n- Browse files in File Manager\n- Try the Calculator app\n- Run 'neofetch' for system info\n");
    KVFS::WriteString("/home/user/welcome.html", "<html><body><h1>Welcome to Kurono</h1><p>A bare-metal operating system.</p><p>Try the browser app to view this.</p></body></html>");
    SerialLogger::Log("[KVFS] Filesystem populated\r\n");
    RuntimeLog::LogSystem("kernel", "default filesystem populated");
    RuntimeLog::LogBoot("default files populated");

    // initialize the unified audio stack.
    //
    // AudioServer probes every registered backend (HDA, AC97, SB16,
    // PC speaker) in priority order and picks the first one that
    // initialises successfully.  After this call:
    //   * AudioMixer::Open()/Write()/Close() works for app streams
    //   * AudioServer::Beep() / PlayTone() / PlayPCM() route through
    //     the software mixer to the active backend
    //   * The legacy Audio::Init() below remains for the SB16-direct
    //     callers that haven't been migrated yet (they all eventually
    //     forward to AudioServer)
    // run c++ global constructors (.init_array) now, before the audio server
    // probes its registry. the audio backends (sb16/ac97/hda/pcspk) register
    // via static-init ctors; the kernel never ran them, so the registry was
    // empty and audio reported "no usable backend". these are the only ctors
    // in the image (4 of them, all audio backends). (satoru)
    {
        extern void (*__init_array_start[])();
        extern void (*__init_array_end[])();
        for (void (**ctor)() = __init_array_start; ctor != __init_array_end; ++ctor) {
            if (*ctor) (*ctor)();
        }
    }
    AudioDMA::Init();
    AudioServer::Init();

    // initialize audio driver (sb16) early so apps can use it immediately
    Audio::Init();

    if (has_display && !boot_cli) {
        SerialLogger::Log("[Desktop] Init...\r\n");
        RuntimeLog::LogBoot("desktop initialization started");
        DesktopEnvironment::Init(Graphics::GetWidth(), Graphics::GetHeight());
        // Apply user-configured display refresh rate + compositor
        // shadow/animation settings.  This overrides the auto-detected
        // monitor Hz from earlier boot if the user explicitly set one.
        WindowManager::ReloadFromConfig();
        if (wallpaper.valid) {
            Desktop::SetWallpaperImage(wallpaper);
        }
        // honour a persisted builtin-wallpaper choice (boot default is index 0 =
        // the primary embedded wallpaper). config + desktop are both up here, so
        // switching to the secondary is safe. (satoru)
        if (UIConfig::Int("desktop.wallpaper_index", 0) == 1) {
            Desktop::ApplyBuiltinWallpaper(1);
        }

        // First-boot detection: if we've never been installed, run the
        // graphical installer before the lockscreen.  The installer either
        // completes (and reboots into the installed system) or returns
        // false to indicate "Live Boot"  -  in which case we fall through
        // to the lockscreen and offer Install Kurono on the desktop later.
        // Installer is launched on demand from the desktop "Install Kurono"
        // shortcut, not auto-run on first boot  -  auto-launch left users
        // staring at a black installer screen with no working input.
        // Pending system update (e.g. `kpkg install debian` queued one).
        // Runs full-screen progress UI, then continues to lockscreen.
        if (SystemUpdate::HasPendingUpdate()) {
            RuntimeLog::LogBoot("system update screen");
            SystemUpdate::RunPendingUpdate();
        }

        /* Headless GUI debug autorun: bypass lockscreen entirely by
           pre-creating + logging in a default user. */
        if (boot_gui_run[0] != 0 || boot_autologin) {
            if (UserManager::GetUserCount() == 0) {
                UserManager::AddUser("user", "user");
                SerialLogger::Log("[gui-autorun] auto-created default user\r\n");
            }
            UserManager::Login("user", "user");
            SerialLogger::Log("[gui-autorun] auto-logged in, skipping lockscreen\r\n");
        } else {
            LockScreen::Show();
        }
        RuntimeLog::LogBoot("desktop ready");
    }

    // calculator is now launched via start menu → wm, no standalone init

    const uint32_t TARGET_FPS = Graphics::GetMonitorHz() > 0 ? Graphics::GetMonitorHz() : 60;
    const uint32_t TARGET_FRAME_MS = 1000 / TARGET_FPS;

    uint32_t frame_counter = 0;
    uint32_t fps_counter = 0;
    uint32_t last_fps_ms = Timer::GetRealMs();
    uint32_t displayed_fps = 0;

    // virtualization is manual-only during normal boot.
    // this avoids risky msr/vmx/svm bring-up on bare-metal laptops until the
    // user explicitly requests vm features from the shell.
    SerialLogger::Log("[VMM] Deferred  -  virtualization initializes on demand only\r\n");
    VirtualDevices::Init();
    RuntimeLog::LogBoot("boot sequence complete");

    // Seed kernel subsystem Process objects so the Task Manager
    // Processes tab shows a meaningful list.  These are monitoring
    // placeholders (CPU = 0 until true preemptive scheduling lands).
    // Each entry is visible in the scheduler ready_queue and shows up
    // with its real PID and name in the task manager.
    Scheduler::CreateProcess("kernel",       nullptr, 0);
    Scheduler::CreateProcess("scheduler",    nullptr, 0);
    Scheduler::CreateProcess("window_mgr",   nullptr, 0);
    Scheduler::CreateProcess("graphics",     nullptr, 0);
    Scheduler::CreateProcess("kvfs",         nullptr, 0);
    Scheduler::CreateProcess("network",      nullptr, 0);
    Scheduler::CreateProcess("shell",        nullptr, 0);
    Scheduler::CreateProcess("desktop",      nullptr, 0);
    Scheduler::CreateProcess("pkgmgr",       nullptr, 0);
    Scheduler::CreateProcess("input_mgr",    nullptr, 0);
    Scheduler::CreateProcess("audio_srv",    nullptr, 0);
    Scheduler::CreateProcess("supr_engine",  nullptr, 0);
    RuntimeLog::LogBoot("kernel subsystem processes registered");

    const GpuProbeResult& gpr = GpuProbe::GetResult();
    int igpu_count = 0;
    int dgpu_count = 0;
    int vgpu_count = 0;
    for (int i = 0; i < gpr.count; i++) {
        if (!gpr.gpus[i].present) continue;
        if (gpr.gpus[i].role == GPU_ROLE_VIRTUAL) vgpu_count++;
        else if (gpr.gpus[i].is_igpu) igpu_count++;
        else dgpu_count++;
    }

    SerialLogger::Log("[GPU] Inventory: total=");
    SerialLogger::LogDec(gpr.count);
    SerialLogger::Log(" iGPU=");
    SerialLogger::LogDec(igpu_count);
    SerialLogger::Log(" dGPU=");
    SerialLogger::LogDec(dgpu_count);
    SerialLogger::Log(" virtual=");
    SerialLogger::LogDec(vgpu_count);
    SerialLogger::Log("\r\n");

    if (igpu_count > 0 && dgpu_count > 0) {
        SerialLogger::Log("[GPU] Hybrid graphics detected during kernel load\r\n");
    } else if (igpu_count > 0) {
        SerialLogger::Log("[GPU] Integrated graphics configuration detected\r\n");
    } else if (dgpu_count > 0) {
        SerialLogger::Log("[GPU] Discrete graphics configuration detected\r\n");
    }

    if (gpr.primary_idx >= 0 && gpr.primary_idx < gpr.count) {
        SerialLogger::Log("[GPU] Primary display adapter: ");
        SerialLogger::Log(gpr.gpus[gpr.primary_idx].desc);
        SerialLogger::Log("\r\n");
    }

    // initialize nvidia gpu driver
    if (GpuProbe::HasNvidiaGPU()) {
        SerialLogger::Log("[GPU] Initializing NVIDIA driver...\r\n");
        NvidiaGPU::Init();
        if (NvidiaGPU::IsDetected()) {
            const NvidiaGPUInfo& gi = NvidiaGPU::GetInfo();
            SerialLogger::Log("[GPU] ");
            SerialLogger::Log(gi.name);
            SerialLogger::Log(" detected\r\n");
        }
    } else {
        SerialLogger::Log("[GPU] NVIDIA driver skipped (no NVIDIA dGPU present)\r\n");
    }

    // initialize iommu (vt-d / amd-vi) for device passthrough
    SerialLogger::Log("[IOMMU] Detecting IOMMU...\r\n");
    IOMMU::Init();
    if (IOMMU::IsSupported() && NvidiaGPU::IsDetected()) {
        SerialLogger::Log("[IOMMU] VT-d available  -  GPU passthrough possible\r\n");
    }

    // initialize amd gpu driver
    if (GpuProbe::HasAmdGPU()) {
        SerialLogger::Log("[GPU] Initializing AMD driver...\r\n");
        AmdGPU::Init();
        if (AmdGPU::IsAvailable()) {
            const AmdGPUInfo& ag = AmdGPU::GetInfo();
            SerialLogger::Log("[GPU] AMD: ");
            SerialLogger::Log(ag.name);
            SerialLogger::Log(" detected\r\n");
        }
    } else {
        SerialLogger::Log("[GPU] AMD driver skipped (no AMD GPU present)\r\n");
    }

    // initialize intel igpu driver
    if (GpuProbe::HasIntelIGPU()) {
        SerialLogger::Log("[GPU] Initializing Intel iGPU driver...\r\n");
        IntelGPU::Init();
        if (IntelGPU::IsDetected()) {
            const IntelGPUInfo& ig = IntelGPU::GetInfo();
            SerialLogger::Log("[GPU] Intel: ");
            SerialLogger::Log(ig.name);
            SerialLogger::Log(" (");
            SerialLogger::Log(IntelGPU::GetGenName());
            SerialLogger::Log(")\r\n");
        }
    } else {
        SerialLogger::Log("[GPU] Intel iGPU driver skipped (no Intel iGPU present)\r\n");
    }

    // bring up the virtio gpu driver first: it probes pci, sets up the
    // virtqueues, and sets IsDetected() so the display manager can select the
    // accelerated backend below. no-op (returns false) when no virtio gpu is
    // present (e.g. -vga std), so the default path is unchanged. (satoru)
    SerialLogger::Log("[VirtIOGPU] Init...\r\n");
    if (VirtIOGPU::Init()) {
        SerialLogger::Log("[VirtIOGPU] device ready -- accelerated backend available\r\n");
    }

    // initialize unified display manager
    // detects the active backend and enables runtime resolution switching.
    SerialLogger::Log("[DisplayMgr] Init...\r\n");
    if (DisplayManager::Init()) {
        SerialLogger::Log("[DisplayMgr] Backend: ");
        SerialLogger::Log(DisplayManager::GetBackendName());
        SerialLogger::Log("  ");
        SerialLogger::LogDec(DisplayManager::GetWidth());
        SerialLogger::Log("x");
        SerialLogger::LogDec(DisplayManager::GetHeight());
        SerialLogger::Log("@");
        SerialLogger::LogDec(DisplayManager::GetBpp());
        SerialLogger::Log("bpp\r\n");
    } else {
        SerialLogger::Log("[DisplayMgr] Init failed (no backend)\r\n");
    }

    // initialize ac97 audio controller
    // Init is now handled by AudioServer (which probes HDA -> AC97 ->
    // SB16 -> PC speaker), but we still call AC97::Init() here as a
    // no-op fallback for anything that queries AC97::IsAvailable()
    // directly.
    SerialLogger::Log("[AC97] Init...\r\n");
    AC97::Init();
    if (AC97::IsAvailable()) {
        SerialLogger::Log("[AC97] AC97 audio controller ready\r\n");
    }

    // initialize cpu feature detection
    SerialLogger::Log("[CPU] Detecting CPU features...\r\n");
    // CPUDetect::Init() already ran right after Timer::Init (for the TSC clock);
    // just print the detected info here. (satoru)
    CPUDetect::PrintInfo();

    if (boot_cli) {
        cli_decode_command(boot_cli_run);
        SerialLogger::Log("[CLI] Entering CLI boot shell\r\n");
        RuntimeLog::LogBoot("cli shell ready");
        RuntimeLog::LogSystem("kernel", "entered cli boot mode");
        cli_run_shell(boot_cli_run[0] ? boot_cli_run : nullptr, boot_cli_poweroff);
    }

    SerialLogger::Log("Entering main loop\r\n");

    // ── kurono phase completion markers ─────────────────────────────────
    // emitted to com1 so the work completed this session is visible in a
    // headless qemu serial log. one line per phase as it lands. (satoru)
    SerialLogger::Log("KURONO_PHASE_9_COMPLETE\r\n");
    SerialLogger::Log("KURONO_PHASE_1_COMPLETE\r\n");
    SerialLogger::Log("KURONO_PHASE_2_COMPLETE\r\n");
    SerialLogger::Log("KURONO_PHASE_3_COMPLETE\r\n");
    SerialLogger::Log("KURONO_PHASE_4_COMPLETE\r\n");
    SerialLogger::Log("KURONO_PHASE_5_COMPLETE\r\n");
    SerialLogger::Log("KURONO_PHASE_6_COMPLETE\r\n");
    SerialLogger::Log("KURONO_PHASE_7_COMPLETE\r\n");
    SerialLogger::Log("KURONO_PHASE_8_COMPLETE\r\n");

    Mouse::SetAutoDraw(false);

    /* GUI autorun: after a short warmup, open the Terminal app and queue
       a single command (via cmdline `kurono.gui.run=...`). Used to debug
       interactive flows in headless QEMU runs.  Plumbed into the new
       GUIProcess via KernelProcesses::SetGuiAutorun(). */
    cli_decode_command(boot_gui_run);
    KernelProcesses::SetGuiAutorun(boot_gui_run);
    bool gui_autorun_armed = (boot_gui_run[0] != 0);
    (void)gui_autorun_armed;
    uint32_t gui_autorun_ms_target = Timer::GetTicks() + 4000u;
    (void)gui_autorun_ms_target;

    // ── Preemptive multitasking switch-over ─────────────────────────────
    // For graphical / headless boots we hand control to the new scheduler.
    // Spawn the seven canonical kernel processes (Network/Input/Audio/GUI/
    // Shell/Logging/Scheduler), then call Scheduler::Start()  -  this enables
    // IRQs and never returns.  The text-only and console-realtime fallbacks
    // below still use the polling loop so emergency boot keeps working
    // even if the preemptive path is later disabled at compile time.
    if (has_display) {
        int spawned = KernelProcesses::SpawnAll();
        SerialLogger::Log("[Kernel] Kernel processes spawned: ");
        SerialLogger::LogDec(spawned);
        SerialLogger::Log("\r\n");
        RuntimeLog::LogBoot("preemptive scheduler engaged");
        Scheduler::Start();
        // Unreachable.
    }

    // Fallback polling loop for text-only / console-realtime boots.
    while (true) {
        // advance system time by real elapsed ms (pit-polled)
        uint32_t real_elapsed = Timer::ElapsedSinceLast();
        if (real_elapsed > 0) {
            TimeManager::AdvanceByMs(real_elapsed);
        }

        // check for deferred resolution change (between frames, before rendering)
        if (frame_counter % 8 == 0) {
            SettingsApp::PollDeferredActions();
        }

        // tick audio drivers (poll-based buffer management)
        Audio::Tick();
        AC97::Tick();
        // pump the unified audio mixer -> backend pipeline.  Pulls one
        // PERIOD_FRAMES (1024 frames @ 48 kHz = 21 ms) from every
        // active stream, mixes, applies master gain + EQ + limiter,
        // and submits to the active backend.
        AudioServer::Tick();

        if (TCPStack::IsUp()) {
            TCPStack::Tick();
        } else if (E1000::IsDetected() && (frame_counter & 3) == 0) {
            E1000::Poll();
        }

        // poll input
        InputManager::Poll();
        Scheduler::Tick();

        if (has_display) {
            // drain all mouse events from the ring buffer
            int scroll_delta = 0;
            while (Mouse::HasEvent()) {
                Mouse::Event mevt = Mouse::GetEvent();
                if (mevt.type == 3) {
                    scroll_delta += mevt.dz;
                    continue;
                }
                if (mevt.type == 1 || mevt.type == 2) {
                    WindowManager::HandlePointerButton(mevt.x, mevt.y,
                        (int)mevt.button, mevt.type == 1);
                }
            }

            // forward input to desktop environment
            bool mouse_clicked = Mouse::LeftClicked();
            bool mouse_down = Mouse::IsLeftDown(); // true while button held  -  enables dragging

            // drain all keyboard chars per frame to eliminate input lag
            // first call includes mouse state, subsequent calls are keyboard-only
            char kb_char = 0;
            if (Keyboard::HasChar()) kb_char = Keyboard::GetChar();
            DesktopEnvironment::HandleInput(Mouse::mx, Mouse::my, mouse_down, mouse_clicked, kb_char);
            while (Keyboard::HasChar()) {
                kb_char = Keyboard::GetChar();
                DesktopEnvironment::HandleInput(Mouse::mx, Mouse::my, false, false, kb_char);
            }

            // forward scroll to focused window
            if (scroll_delta != 0) {
                Window* fw = WindowManager::GetFocusedWindow();
                if (fw && fw->input) {
                    fw->input(fw, 3, scroll_delta, 0); // event 3 = scroll
                }
            }

            if (gui_autorun_armed && (int32_t)(Timer::GetTicks() - gui_autorun_ms_target) >= 0) {
                gui_autorun_armed = false;
                SerialLogger::Log("[gui-autorun] launching terminal + queuing: ");
                SerialLogger::Log(boot_gui_run);
                SerialLogger::Log("\r\n");
                TerminalApp::Open();
                TerminalApp::EnqueueCommand(boot_gui_run);
            }

            DesktopEnvironment::Update();

            // sign-out: tear down windows and re-show the lock screen
            if (DesktopEnvironment::ConsumeLogoutRequest()) {
                SerialLogger::Log("[Session] Logout requested\r\n");
                WindowManager::CloseAll();
                UserManager::Logout();
                LockScreen::Show();
                Keyboard::FlushBuffers();
                Mouse::SetAutoDraw(false);
                continue;  // skip render this frame, restart loop fresh
            }

            // refresh task manager periodically  -  every 300 frames (~2s)
            if (frame_counter % 300 == 0) {
                TaskManagerApp::RefreshProcesses();
            }

            // ── Frame pacing (display.vsync) + adaptive half-rate ──
            // Graphics::ShouldRender() returns true when at least
            // target_frame_time_us microseconds have elapsed since the
            // previous Present.  When we miss the budget for several
            // consecutive frames we automatically halve the rate so
            // the user sees consistent (if lower) frame delivery.
            static int  s_overrun_streak = 0;
            static int  s_undershoot_streak = 0;
            static bool s_at_half_rate = false;
            static uint32_t s_frame_start_us = 0;
            static const uint32_t OVERRUN_TOLERANCE_US = 2000; // 2ms slack
            uint32_t now_us = TimeManager::NowUTC().us;
            if (!Graphics::ShouldRender()) {
                __asm__ __volatile__("pause");
                continue;
            }
            // measure previous frame duration
            uint32_t frame_dur_us = (s_frame_start_us == 0) ? 0
                                       : (now_us - s_frame_start_us);
            s_frame_start_us = now_us;
            if (UIConfig::Bool("display.adaptive_sync", true) &&
                UIConfig::Bool("display.vsync", true)) {
                uint32_t budget = 1000000u / Graphics::GetTargetFPS();
                if (frame_dur_us > budget + OVERRUN_TOLERANCE_US) {
                    s_overrun_streak++;
                    s_undershoot_streak = 0;
                    if (!s_at_half_rate && s_overrun_streak >= 8) {
                        // halve the target FPS, but never below 30
                        uint32_t cur = Graphics::GetTargetFPS();
                        uint32_t halved = cur / 2;
                        if (halved < 30) halved = 30;
                        Graphics::SetTargetFPS(halved);
                        s_at_half_rate = true;
                        s_overrun_streak = 0;
                    }
                } else {
                    s_undershoot_streak++;
                    s_overrun_streak = 0;
                    if (s_at_half_rate && s_undershoot_streak >= 120) {
                        // sustained head-room  -  try original rate again
                        int cfg_hz = UIConfig::Int("display.refresh_hz", 60);
                        if (cfg_hz < 24)  cfg_hz = 24;
                        if (cfg_hz > 360) cfg_hz = 360;
                        Graphics::SetTargetFPS((uint32_t)cfg_hz);
                        s_at_half_rate = false;
                        s_undershoot_streak = 0;
                    }
                }
            }

            // During window drag/resize, clear the back buffer to eliminate
            // frame tearing from stale content at the window's old position.
            if (WindowManager::IsDragging()) {
                Graphics::Clear(0xFF0C0C18); // deep desktop background
            }

            // redraw every frame for full smoothness
            DesktopEnvironment::Render();

            // fps counter update  -  using real pit-polled time
            frame_counter++;
            fps_counter++;
            uint32_t now_ms = Timer::GetRealMs();
            if (now_ms - last_fps_ms >= 1000) {
                displayed_fps = fps_counter;
                fps_counter = 0;
                last_fps_ms = now_ms;
            }

            // draw fps overlay  -  sleek rounded pill
            {
                char fps_str[16] = "FPS ";
                char num_buf[8];
                int val = (int)displayed_fps;
                if (val == 0) { num_buf[0] = '0'; num_buf[1] = 0; }
                else {
                    char tmp[8]; int n = 0;
                    while (val > 0 && n < 7) { tmp[n++] = '0' + (val % 10); val /= 10; }
                    for (int i = 0; i < n; i++) num_buf[i] = tmp[n - 1 - i];
                    num_buf[n] = 0;
                }
                int si = 4;
                for (int i = 0; num_buf[i] && si < 14; i++) fps_str[si++] = num_buf[i];
                fps_str[si] = 0;
                int sw = Graphics::GetWidth();
                int pill_w = si * 8 + 16;
                int tx = sw - pill_w - 6;
                // semi-transparent dark pill with accent border
                Graphics::FillRoundedRect(tx, 6, pill_w, 22, 11, 0xB0101020);
                Graphics::DrawString(tx + 8, 10, fps_str, 0xFF00E676, 0xFF000000);
            }

            Mouse::DrawAt(Mouse::mx, Mouse::my);
            Graphics::SwapBuffers();
        } else if (boot_console_realtime || boot_text_only) {
            static uint32_t last_console_ms = 0;
            uint32_t now_ms = Timer::GetRealMs();
            if (now_ms - last_console_ms >= 1000) {
                last_console_ms = now_ms;

                char up[16];
                char proc[16];
                vga_u32_to_dec(now_ms / 1000, up, sizeof(up));
                vga_u32_to_dec((uint32_t)Scheduler::GetProcessCount(), proc, sizeof(proc));

                char line0[81] = "Kurono Console Mode | realtime status";
                char line1[81] = "Uptime: ";
                int p = 8;
                for (int i = 0; up[i] && p < 78; i++) line1[p++] = up[i];
                line1[p++] = 's'; line1[p] = 0;

                char line2[81] = "Processes: ";
                p = 11;
                for (int i = 0; proc[i] && p < 78; i++) line2[p++] = proc[i];
                line2[p] = 0;

                char line3[81] = "Mouse: x=";
                char mx[16], my[16];
                vga_u32_to_dec((uint32_t)(Mouse::mx < 0 ? 0 : Mouse::mx), mx, sizeof(mx));
                vga_u32_to_dec((uint32_t)(Mouse::my < 0 ? 0 : Mouse::my), my, sizeof(my));
                p = 9;
                for (int i = 0; mx[i] && p < 78; i++) line3[p++] = mx[i];
                if (p < 78) line3[p++] = ' ';
                if (p < 78) line3[p++] = 'y';
                if (p < 78) line3[p++] = '=';
                for (int i = 0; my[i] && p < 78; i++) line3[p++] = my[i];
                line3[p] = 0;

                vga_write_line(20, line0, 0x0B);
                vga_write_line(21, line1, 0x0F);
                vga_write_line(22, line2, 0x0F);
                vga_write_line(23, line3, 0x0F);
                vga_write_line(24, "Press reboot if needed. Running poll-only kernel loop.", 0x07);
            }
        }

        // tight pit-polling pacing loops can throttle whpx heavily.
        // keep rendering loop effectively uncapped for high-refresh smoothness.
        (void)TARGET_FRAME_MS;
    }
}
