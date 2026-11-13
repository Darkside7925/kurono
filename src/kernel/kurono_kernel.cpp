// kurono os enhanced kernel - full desktop edition
// complete bare-metal os with hybrid kernel, desktop environment and apps

#include "types.h"
#include "../system/kasset.h"   // on-demand assets (satoru)
#include "multiboot.h"
#include "system.h"
#include "heap.h"
#include "time.h"
#include "kvdso.h"
#include "memory_mgr.h"
#include "memcensus.h"   // kurono.memcensus=1 leak census (satoru)
#include "taskmem.h"     // kurono.taskmem=1 per-task kernel memory (satoru)
#include "buddy.h"
#include "slab.h"
#include "hrtimer.h"
#include "panic.h"
#include "kimage.h"   // high-linked image: mb1 32-bit pointer fields need phys (satoru)
#include "../drivers/serial.h"
#include "../drivers/display.h"
#include "../drivers/graphics.h"
#include "../drivers/outputs.h"   // multi-monitor outputs (satoru)
#include "../drivers/bga.h"
#include "../drivers/timer.h"
#include "../drivers/keyboard.h"
#include "../drivers/mouse.h"
#include "../media/mediadecoder.h"
#include "../ui/gui.h"
#include "../ui/lockscreen.h"
#include "../system/user_mgmt.h"
#include "../system/claude_host.h"
#include "../system/opencode_host.h"   // (satoru)
#include "dbgwatch.h"   // kurono.dbgwatch (satoru)
#include "../virt/pci_passthrough.h"   // kurono.vm.pt (satoru)
#include "../system/installer_gui.h"
#include "../system/installer_service.h"
#include "../ui/font.h"
#include "../ui/window_manager.h"
#include "../ui/desktop.h"
#include "../ui/gfx_bench.h"   // kurono.gfxstats / kurono.gfxbench (satoru)
#include "../ui/gfx_oracle.h"   // kurono.gfx.oracle (satoru)
#include "../ui/notification.h"
#include "../apps/calculator.h"
#include "../kui/kui_kernel.h"   // kurono.r3apps (satoru)
#include "../apps/terminal.h"
#include "../apps/file_manager.h"
#include "../apps/settings_display.h"   // re-apply the saved display arrangement (satoru)
#include "../apps/text_editor.h"
#include "../apps/settings.h"
#include "../apps/system_settings.h"   // SystemSettings::RestoreAtBoot (satoru)
#include "../apps/task_manager.h"
#include "../hal/hal.h"
#include "../fs/vfs.h"
#include "../fs/kvfs.h"
#include "../fs/persist.h"
#include "../fs/kfs.h"         // release the boot-restore metadata caches (satoru)
#include "../fs/kfs_bench.h"   // headless storage benchmark (kurono.kfsbench) (satoru)
#include "../drivers/nvme.h"   // command-count stats for the kfstest speed proof (satoru)
#include "../drivers/blockdev.h" // generic block registry - sata/nvme behind one api (satoru)
#include "../drivers/ahci.h"     // sata: the driver gap that made rescue-on-metal impossible (satoru)
#include "../drivers/disklog.h"  // bare-metal write guard window + KURONO.LOG boot log (satoru)
#include "../drivers/hwinfo.h"   // full hardware inventory into the boot log (satoru)
#include "../drivers/usb.h"
#include "../drivers/usb_audio.h"
#include "../linux/ext4.h"
#include "../proc/scheduler.h"
#include "../tests/test_suite.h"
#include "../system/input_manager.h"
#include "../system/vconsole.h"
#include "../system/logging.h"
#include "../system/kinit.h"          // service + init manager (satoru)
#include "../system/power.h"           // one reboot/shutdown/sleep sequence + autosave (satoru)
#include "kdf.h"                       // kernel driver framework (hybrid-kernel ring-0+ sandbox) (satoru)
#include "udf.h"                       // user driver framework (ring-3 drivers via SYS_UDF_CALL) (satoru)
#include "kld_broker.h"
#include "kld_ioapic.h"
#include "kld_input.h"                // kld: pci broker for ring-3 linux driver containers (satoru)
#include "kld_broker.h"                  // kld: pci broker for ring-3 linux driver containers (satoru)
#include "../system/platform_test.h"   // kurono.platformtest (satoru)
#include "kscreen.h"                    // stop codes for the restart notice (satoru)
#include "../system/recovery.h"         // kurono.mode=recovery (satoru)
#include "../system/bootcfg.h"          // boot watch + KBOOT.CFG (satoru)
#include "../system/backlight.h"        // panel backlight probe (satoru)
#include "../system/idle_sleep.h"       // kurono.sleep=idle (satoru)
#include "kld_fwcfg.h"                // kld: QEMU fw_cfg wifi.conf delivery (satoru)
#include "kld_auto.h"                     // kld: driver matched by its own pci id table (satoru)
#include "kld_display.h"                  // kld display bridge: mode table, scanout, damage (satoru)
#include "../linux/dri_dev.h"             // kurono.kld.dri (satoru)
#include "memboot.h"                   // kurono.memboot=1 (satoru)
#include "../linux/uaccess.h"          // uaccess reject counters for the uatest gate (satoru)
#include "irp.h"                       // i/o request packet executive (stackable async i/o) (satoru)
#include "kexec.h"                     // kernel executive (NT-style Mm/Ps/Io/Se/Cm facade) (satoru)
#include "../system/kpkg_daemon.h"    // background package install daemon (satoru)
#include "../system/kdaemons.h"       // kupdate + ksecurity service daemons (satoru)
#include "../system/systemd_compat.h" // systemd compatibility shim (-> kinit) (satoru)
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
#include "../kcl/kcl_test.h"
#include "../net/ieee80211_test.h"  // 802.11i wpa2 crypto self-test (kurono.wifitest) (satoru)
#include "kmemx.h"             // kmemx memory compression engine (satoru)
#include "kmemx_test.h"        // kmemx memory-compression self-test (kurono.kmemxtest) (satoru)
#include "../apps/kj.h"        // kj (kurono javascript) interpreter (satoru)
#include "../apps/kj_test.h"   // kj (kurono javascript) self-test suite (satoru)
#include "../security/ken.h"
#include "../security/ksa.h"
#include "../security/admin.h"
#include "../packages/pkgmgr.h"
#include "../apps/python_interp.h"
#include "elf_loader.h"
#include "userspace.h"
#include "../userprogs/embedded_userprogs.h"
#include "../linux/linux_syscall.h"
#include "../proc/kernel_processes.h"
#include "../net/network.h"
#include "../net/tcpip.h"
#include "../net/sntp.h"   // kurono.ntp=0 (satoru)
#include "../net/dhcp.h"
#include "../net/tuntap.h"
#include "../net/ipv6.h"
#include "../net/netfilter.h"
#include "../net/unix_socket.h"
#include "../proc/cgroup.h"
#include "../drivers/tpm.h"
#include "../drivers/pulse_server.h"
#include "../drivers/audio_r3.h"      // kurono.r3audio=1 ring-3 audio daemon broker (satoru)
#include "../hal/cpufreq.h"
#include "../system/runtime_layout.h"
#include "../system/dbus_server.h"
#include "../ui/wayland_server.h"
#include "../linux/ld_kurono.h"
#include "../drivers/audio.h"
#include "../drivers/audio_server.h"
#include "../drivers/hda.h"
#include "../drivers/audio_mixer.h"
#include "../drivers/audio_dma.h"
#include "../drivers/e1000.h"
#include "../drivers/igc.h"
#include "../system/ktools.h"      // kdrv + ksys driver control and diagnostics (satoru)
#include "../system/agent_tools.h" // busybox applets + ksh-run on the linux PATH (satoru)
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
#include "../ui/boot_anim.h"
#include "../drivers/gpu_probe.h"
#include "../drivers/intel_display_probe.h"   // stage 0 read-only intel display probe (satoru)
#include "../drivers/intel_dp_aux.h"          // stage 1 dp aux dpcd/edid reads (satoru)
#include "../drivers/intel_modeset.h"         // stage 2 external dp on pipe b (satoru)
#include "../drivers/intel_accel.h"           // stage 3a hardware cursor, kurono.igpu.accel=1 (satoru)
#include "../drivers/intel_gt.h"              // gt bring-up and blitter, kurono.igpu.gt=1 (satoru)
#include "../ui/frame_pacer.h"   // panel rate + kurono.hz for the gui pacer (satoru)
#include "../ui/ui_diag.h"   // kurono.uidiag=1 pointer/drag counters (satoru)
#include "../../logo.h"
#include "../media/embedded_media.h"
#include "../virt/vm_host.h"   // background linux vm boot options (satoru)
#include "../virt/guest_mem.h"   // kurono.vm.splitmem (satoru)
// test rig globals, defined in hal/hal.cpp (satoru)
extern volatile uint32_t g_ffcrash_secs;
extern volatile uint64_t g_ffcrash_as, g_ffcrash_at;
extern volatile int g_ffreopen_left;
// kurono.hz=N, parsed with the early flags, applied once the pacer has the
// panel rate. 0 = not given. (satoru)
static uint32_t g_boot_hz = 0;
// kurono.panelhz=N: test-rig override of the detected panel/lit-output rate
// (qemu has no panel and always reports 60, so the pacer ladder could not be
// exercised there). 0 = not given. (satoru)
static uint32_t g_boot_panel_hz = 0;

// embedded inter ui faces (Makefile objcopy of assets/fonts). (satoru)
extern "C" {
    extern const uint8_t _binary_inter_regular_otf_start[]  __attribute__((weak));
    extern const uint8_t _binary_inter_regular_otf_end[]    __attribute__((weak));
    extern const uint8_t _binary_inter_semibold_otf_start[] __attribute__((weak));
    extern const uint8_t _binary_inter_semibold_otf_end[]   __attribute__((weak));
    extern const uint8_t _binary_dejavu_mono_ttf_start[]    __attribute__((weak));
    extern const uint8_t _binary_dejavu_mono_ttf_end[]      __attribute__((weak));
}

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

// value of a "prefix<digits>" boot token, or nullptr. (satoru)
static const char* boot_value(const char* cmdline, const char* prefix) {
    if (!cmdline || !prefix) return nullptr;
    size_t pl = 0;
    while (prefix[pl]) pl++;
    for (const char* p = cmdline; *p; p++) {
        if (p != cmdline && p[-1] != ' ') continue;
        bool hit = true;
        for (size_t i = 0; i < pl; i++)
            if (!p[i] || p[i] != prefix[i]) { hit = false; break; }
        if (hit) return p + pl;
    }
    return nullptr;
}

// tiny builders for the bench env/commands (no snprintf in this TU). (satoru)
static char* kld_append_str(char* d, const char* s) { while (*s) *d++ = *s++; return d; }
static char* kld_append_int(char* d, int v) {
    char t[12]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *d++ = t[--n];
    return d;
}

static bool boot_get_value(const char* cmdline, const char* key, char* out, int max_out) {
    if (!out || max_out < 2) return false;
    out[0] = 0;
    if (!cmdline || !key || !key[0]) return false;

    int key_len = stlen(key);
    for (const char* p = cmdline; *p; p++) {
        // grub re-quotes a spaced argument around the WHOLE token
        // ("key=multi word value"), so the char before the key may be an
        // opening quote rather than a space - accept it and remember it as
        // the value terminator so the spaces inside survive. (satoru)
        char lead_quote = 0;
        if (p != cmdline) {
            char prev = *(p - 1);
            if (prev == '"' || prev == '\'') lead_quote = prev;
            else if (!stspace(prev)) continue;
        }
        if (!ststarts_with(p, key) || p[key_len] != '=') continue;

        const char* value = p + key_len + 1;
        char quote = lead_quote;
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

// log the raw boot cmdline with the value of kurono.claude.env replaced by <redacted>: that
// value carries credentials and serial / KURONO.LOG (the disk log mirrors serial) must never
// see it. handles a value in quotes, and grub's quoting around the whole key=value token. (satoru)
static void log_cmdline_redacted(const char* cmd) {
    static const char key[] = "kurono.claude.env=";
    const int klen = (int)sizeof(key) - 1;
    for (const char* p = cmd; *p; ) {
        bool at_start = (p == cmd) || stspace(*(p - 1)) || *(p - 1) == '"' || *(p - 1) == '\'';
        if (at_start && ststarts_with(p, key)) {
            char lead = (p != cmd && (*(p - 1) == '"' || *(p - 1) == '\'')) ? *(p - 1) : 0;
            SerialLogger::Log("kurono.claude.env=<redacted>");
            p += klen;
            char quote = lead;
            if (*p == '"' || *p == '\'') { quote = *p; p++; }
            while (*p) {
                if (quote) { if (*p == quote) break; }
                else if (stspace(*p)) break;
                p++;
            }
            continue;
        }
        char c[2] = { *p, 0 };
        SerialLogger::Log(c);
        p++;
    }
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
        Dhcp::Tick();   // cli holds no network process; keep the lease alive (satoru)
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
// works before any subsystem init - writes directly to the framebuffer.
// this is essential for bare-metal debugging when there's no serial port.
static uint64_t _efb_addr = 0;
static uint32_t _efb_pitch = 0, _efb_w = 0, _efb_h = 0;
static uint8_t  _efb_bpp = 0;
static int _efb_cx = 4, _efb_cy = 36; // cursor position (below the color bar)
// glyph scale + opaque cell background. on a 2560-wide laptop panel the 8x8
// font is unreadable in a photo, and without a background it mixes with the
// boot manager's leftover text. (satoru)
static int _efb_scale = 1;

// tiny 8x8 font - covers printable ascii 0x20-0x7e only
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
    const int sc = _efb_scale;
    if (c == '\n') { _efb_cx = 4; _efb_cy += 10 * sc; return; }
    if (c < 0x20 || c > 0x7E) return;
    const uint8_t* glyph = _efb_font[c - 0x20];
    for (int row = 0; row < 10 * sc; row++) {
        int gr = row / sc;
        uint8_t bits = (gr < 8) ? glyph[gr] : 0;
        for (int col = 0; col < 8 * sc; col++) {
            bool on = (bits & (0x80 >> (col / sc))) != 0;
            early_fb_putpx(_efb_cx + col, _efb_cy + row, on ? fg : 0x000000);
        }
    }
    _efb_cx += 8 * sc;
    if ((uint32_t)_efb_cx >= _efb_w - 8 * sc) { _efb_cx = 4; _efb_cy += 10 * sc; }
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
    _efb_scale = (w >= 2400) ? 3 : (w >= 1600) ? 2 : 1;
}

// flush the early fb writes to the display - tries both wbinvd and sfence
static void early_fb_flush() {
    __asm__ __volatile__("wbinvd" ::: "memory");
}

//  multiboot2 → multiboot1 compatibility shim
//  converts mb2 tagged info into a static multiboot_info_t so all existing
//  kernel code works unchanged with either boot protocol.
static multiboot_info_t mb2_compat_info;
static char mb2_cmdline_buf[512];   // owned copy of the mb2 cmdline tag (satoru)
// buffer for converted mb1-format mmap entries (each 24 bytes, room for 170)
static uint8_t mb2_mmap_buf[4096];

// every gop handle the efi loader saw (mb2 tag 0x4B52). logged to KURONO.LOG and,
// for distinct extras, registered as additional display outputs. (satoru)
struct GopSeen { uint64_t base, size; uint32_t w, h, pitch; uint8_t fmt, flags, idx; };
static GopSeen g_gop_seen[6];
static int     g_gop_seen_n = 0;

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
                // copy it into kernel static memory like the mmap below. the mb1
                // cmdline field is 32-bit, and the efi loader (a 100 mb image) is
                // often loaded above 4 gb, so a raw pointer to its tag truncated
                // to garbage and every efi boot ran with an EMPTY cmdline. (satoru)
                const char* cs = (const char*)(tag + 8);
                uint32_t cn = 0;
                while (cn + 1 < sizeof(mb2_cmdline_buf) && cn + 8 < size && cs[cn]) {
                    mb2_cmdline_buf[cn] = cs[cn]; cn++;
                }
                mb2_cmdline_buf[cn] = 0;
                mb2_compat_info.flags |= (1u << 2);
                mb2_compat_info.cmdline = (uint32_t)KernelPhysOf(mb2_cmdline_buf);
                break;
            }
            case 12: // efi64 system table pointer: this is a uefi boot (satoru)
                BootAcpi::SetEfiBoot();
                if (size >= 16) BootAcpi::SetSystemTable(*(uint64_t*)(tag + 8));
                break;
            case 14:  // acpi 1.0 rsdp copy (satoru)
            case 15:  // acpi 2.0+ rsdp copy (satoru)
                BootAcpi::SetRsdp(tag + 8, size - 8);
                break;
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
                mb2_compat_info.mmap_addr = (uint32_t)KernelPhysOf(mb2_mmap_buf);
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
            case 0x4B52: { // per-gop-handle record from the efi loader (satoru)
                if (size >= 40 && g_gop_seen_n < 6) {
                    GopSeen& g = g_gop_seen[g_gop_seen_n++];
                    g.base  = *(uint64_t*)(tag + 8);
                    g.size  = *(uint64_t*)(tag + 16);
                    g.w     = *(uint32_t*)(tag + 24);
                    g.h     = *(uint32_t*)(tag + 28);
                    g.pitch = *(uint32_t*)(tag + 32);
                    g.fmt   = tag[36];
                    g.flags = tag[37];
                    g.idx   = tag[38];
                }
                break;
            }
            // module tags (type 3) have variable format - skip for now.
            // boot loader name (type 2), etc. - not needed.
        }

        // advance to next tag (8-byte aligned)
        uint32_t advance = (size + 7u) & ~7u;
        tag += advance;
    }
}

// kinit self-test runner kernel-process. spawned (when kurono.kinit.test is set)
// after the desktop is up; it runs the bounded scenarios off the gui loop, logs
// PASS/FAIL to serial, and optionally powers off for headless ci. (satoru)
static volatile bool g_kinit_test_poweroff = false;
[[noreturn]] static void kinit_selftest_entry() {
    // let the desktop + the kinit boot scenarios settle a moment first. (satoru)
    Scheduler::SleepMs(2500);
    KInit::RunSelfTests();
    if (g_kinit_test_poweroff) {
        SerialLogger::Log("[kinit-test] powering off (kurono.kinit.poweroff)\r\n");
        HAL::PowerOff();
    }
    for (;;) Scheduler::SleepMs(60000);
}

// ring-3 audio daemon supervisor (kurono.r3audio=1). spawns /usr/bin/audiod and
// restarts it if it ever exits - a crash in the daemon costs the pulse clients
// their connection, never the kernel. (satoru)
static bool g_r3audiotest = false;   // kurono.r3audiotest=1: audiod plays a tone through its own socket (pulsetest.c)
[[noreturn]] static void r3audio_runner_entry() {
    // launched from a sleeping kernel proc, never at boot: a ring-3 process started before the
    // gui has run starves the kernel procs that have not run yet (launching at 0.5 s and at the
    // first gui frame both stalled the whole boot, see phase3_report.md). while firefox boots
    // this proc wakes late (measured up to 12 s); audio is simply unavailable until it does.
    // async spawn (agent_sched_23): with an ap online the daemon runs on the aps via
    // Userspace::SpawnDetached - no RunProcessWithArgs frame for firefox to clobber,
    // nothing of it on the bsp - so it can start right away instead of after a sleep
    // this proc wakes from late. supervised by the broker heartbeat as before. (satoru)
    if (Userspace::CanSpawnDetached()) {
        for (int attempt = 0; attempt < 8; attempt++) {
            Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/audiod", "audiod");
            if (!p) { SerialLogger::Log("[r3audio] load FAILED\r\n"); break; }
            R3Audio::SetOwner(p->address_space);
            const char* av[] = { "audiod", g_r3audiotest ? "selftest" : nullptr, nullptr };
            const char* envp[] = { "PATH=/usr/bin", nullptr };
            DetachedHandle h;
            int sr = Userspace::SpawnDetached(p, av, envp, 0, 0, &h);
            SerialLogger::Log("[r3audio] spawned detached rc=");
            SerialLogger::LogDec(sr);
            SerialLogger::Log(" t=");
            SerialLogger::LogDec((int)Timer::GetRealMs64());
            SerialLogger::Log("\r\n");
            if (sr < 0) break;
            // the leader may end on its own thread while a worker serves: the daemon
            // counts as up while its heartbeat reaches the broker. (satoru)
            Scheduler::SleepMs(3500);
            while (Userspace::DetachedAlive(h) || R3Audio::DaemonAlive()) Scheduler::SleepMs(2000);
            SerialLogger::Log("[r3audio] daemon stopped answering, restarting\r\n");
            int rc = Userspace::ReapDetached(h);
            SerialLogger::Log("[r3audio] audiod exited rc=");
            SerialLogger::LogDec(rc);
            SerialLogger::Log("\r\n");
            Scheduler::SleepMs(1000);
        }
        for (;;) Scheduler::SleepMs(60000);
    }
    Scheduler::SleepMs(1500);
    for (int attempt = 0; attempt < 8; attempt++) {
        SerialLogger::Log("[r3audio] launching /usr/bin/audiod\r\n");
        Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/audiod", "audiod");
        if (!p) { SerialLogger::Log("[r3audio] load FAILED\r\n"); break; }
        R3Audio::SetOwner(p->address_space);
        const char* av[] = { "audiod", g_r3audiotest ? "selftest" : nullptr, nullptr };
        const char* envp[] = { "PATH=/usr/bin", nullptr };
        int rc = Userspace::RunProcessWithArgs(p, av, envp);
        SerialLogger::Log("[r3audio] audiod exited rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log("\r\n");
        // RunProcessWithArgs frames are per-cpu: another ring-3 launcher (firefox) on the same
        // cpu can longjmp back here while the daemon is still running. only restart a dead one.
        Scheduler::SleepMs(3500);
        if (R3Audio::DaemonAlive()) {
            SerialLogger::Log("[r3audio] daemon still alive, supervising\r\n");
            while (R3Audio::DaemonAlive()) Scheduler::SleepMs(2000);
            SerialLogger::Log("[r3audio] daemon stopped answering, restarting\r\n");
        }
        Scheduler::SleepMs(1000);
    }
    for (;;) Scheduler::SleepMs(60000);
}

// ring-3 wake-latency probe runner (kurono.irqlat=1). the KLD phase-0 gate:
// "an interrupt must be able to wake a userspace thread with bounded latency".
// RunProcessWithArgs blocks until the program exits, which is exactly why this
// lives in its own kernel-process instead of on the gui loop. let the desktop
// settle first so the numbers reflect a normally-loaded system, not boot.
// kurono.irqlat.busy=N runs the contention mode: N busy ring-3 threads while a
// thread blocks on a futex / pipe / timer (the KLD-free 2-cpu reproducer). (satoru)
static int g_irqlat_busy = -1;   // -1 = the old single-thread probe (satoru)
static int g_irqlat_secs = 8;
static int g_irqlat_mode = 0;
[[noreturn]] static void irqlat_runner_entry() {
    Scheduler::SleepMs(3000);
    Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/irqlat", "irqlat");
    if (!p) {
        SerialLogger::Log("[irqlat] load FAILED\r\n");
    } else if (g_irqlat_busy >= 0) {
        char b1[8], b2[8], b3[8];
        int v = g_irqlat_busy, k = 0;
        if (!v) b1[k++] = '0';
        while (v) { b1[k++] = (char)('0' + v % 10); v /= 10; }
        char a1[8]; int n1 = 0; while (k) a1[n1++] = b1[--k]; a1[n1] = 0;
        v = g_irqlat_secs; k = 0; if (!v) b2[k++] = '0';
        while (v) { b2[k++] = (char)('0' + v % 10); v /= 10; }
        char a2[8]; int n2 = 0; while (k) a2[n2++] = b2[--k]; a2[n2] = 0;
        v = g_irqlat_mode; k = 0; if (!v) b3[k++] = '0';
        while (v) { b3[k++] = (char)('0' + v % 10); v /= 10; }
        char a3[8]; int n3 = 0; while (k) a3[n3++] = b3[--k]; a3[n3] = 0;
        const char* av[] = { "irqlat", a1, a2, a3, nullptr };
        const char* envp[] = { "PATH=/usr/bin", "HOME=/home/user", nullptr };
        int rc = Userspace::RunProcessWithArgs(p, av, envp);
        SerialLogger::Log("[irqlat] contention runner finished rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log("\r\n");
    } else {
        const char* av[] = { "irqlat", nullptr };
        const char* envp[] = { "PATH=/usr/bin", "HOME=/home/user", nullptr };
        int rc = Userspace::RunProcessWithArgs(p, av, envp);
        SerialLogger::Log("[irqlat] runner finished rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log("\r\n");
    }
    for (;;) Scheduler::SleepMs(60000);
}

// cpu scaling probe runner (kurono.smpscale=N, N = max threads, 1 = the cpu
// count the program is told). runs /usr/bin/smpscale once, a few seconds after
// boot so the desktop has settled; it prints SMPSCALE lines to com1. (satoru)
static int g_smpscale_threads = 0;
[[noreturn]] static void straytest_entry() {   // kurono.straytest=1 (satoru)
    Scheduler::SleepMs(3000);
    HAL::StrayVectorSelfTest();
    for (;;) Scheduler::SleepMs(60000);
}

[[noreturn]] static void smpscale_runner_entry() {
    Scheduler::SleepMs(4000);
    Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/smpscale", "smpscale");
    if (!p) {
        SerialLogger::Log("[smpscale] load FAILED\r\n");
    } else {
        char nb[8]; int k = 0; int v = g_smpscale_threads;
        if (v > 1) { char t[8]; int m = 0; while (v && m < 7) { t[m++] = (char)('0' + v % 10); v /= 10; } while (m) nb[k++] = t[--m]; }
        nb[k] = 0;
        const char* av[] = { "smpscale", k ? nb : nullptr, nullptr };
        const char* envp[] = { "PATH=/usr/bin", "HOME=/home/user", nullptr };
        int rc = Userspace::RunProcessWithArgs(p, av, envp);
        SerialLogger::Log("[smpscale] runner finished rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log("\r\n");
    }
    for (;;) Scheduler::SleepMs(60000);
}

// posix_spawn of a static elf64 runner (kurono.spawntest=1). runs the ring-3
// repro once in its own kernel process and logs its exit code; the repro itself
// prints the [spawntest] case/DONE lines to stderr. (satoru)
[[noreturn]] static void spawntest_runner_entry() {
    Scheduler::SleepMs(3000);
    Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/spawntest", "spawntest");
    if (!p) {
        SerialLogger::Log("[spawntest] load FAILED\r\n");
    } else {
        const char* av[] = { "spawntest", nullptr };
        const char* envp[] = { "PATH=/system/bin", "HOME=/home/user", nullptr };
        int rc = Userspace::RunProcessWithArgs(p, av, envp);
        SerialLogger::Log("[spawntest] runner finished rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log("\r\n");
    }
    for (;;) Scheduler::SleepMs(60000);
}

// kurono.kld.yieldfair=1: pass the N1 yield-fairness reproducer argument to
// the apitest container. kurono.kld.stats=1: pass KLD_STATS=1 so the shim
// dumps its per-thread wait/yield table. (satoru)
static bool g_kldyieldfair;
static bool g_kldstats;

// KLD driver-container runner (kurono.kld=1). runs the unmodified upstream
// linux e1000e driver, linked against the lx_emul shim, as a ring-3 Kurono
// process. phase 1 expectation: module_init is REACHED, the driver's own printk
// banner appears, and it then aborts on the first unimplemented shim symbol
// (rc=42). that abort is the phase-1 exit criterion, not a failure. (satoru)
[[noreturn]] static void kld_runner_entry() {
    Scheduler::SleepMs(3500);
    SerialLogger::Log("[kld] launching /usr/bin/kld-e1000e (unmodified upstream driver)\r\n");
    KAsset::EnsureVfsFile("/usr/bin/kld-e1000e", "KLDE1K  ELF", _binary_kld_e1000e_elf_start, _binary_kld_e1000e_elf_end);   // (satoru)
    Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/kld-e1000e", "kld-e1000e");
    if (!p) {
        SerialLogger::Log("[kld] load FAILED\r\n");
    } else {
        const char* av[] = { "kld-e1000e", nullptr };
        const char* envp_stats[] = { "PATH=/usr/bin", "KLD_STATS=1", nullptr };
        const char* envp[] = { "PATH=/usr/bin", nullptr };
        const char** env = g_kldstats ? envp_stats : envp;
        // the address space value is the broker's grant key; capture it now,
        // the process object is gone once the run returns. (satoru)
        uint64_t kld_as = p->address_space;
        // named registration: 64 MiB for e1000e, 256 MiB for the iwlwifi
        // container M2 will add (same helper, different name). (satoru)
        KldBroker::SetDmaBudgetForName(kld_as, "/usr/bin/kld-e1000e", (int)p->pid);
        int rc = Userspace::RunProcessWithArgs(p, av, env);
        SerialLogger::Log("[kld] container exited rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log(" (42 = hit an unimplemented shim symbol)\r\n");
        // bus master off, msi off, frames back, grants dropped - a crashed
        // driver must not leave a device dma-ing into freed memory. (satoru)
        KldBroker::ReleaseAddressSpace(kld_as);
    }
    for (;;) Scheduler::SleepMs(60000);
}

// kurono.kld.stats=1: print the line-irq/ioapic counters once, after boot, so
// the default-boot proof can assert the I/O APIC was never touched. (satoru)
[[noreturn]] static void kld_stats_entry() {
    Scheduler::SleepMs(5000);
    SerialLogger::Log("[kld-ioapic] stats: accesses=");
    SerialLogger::LogDec((int)KldIoApic::Accesses());
    SerialLogger::Log(" reads=");
    SerialLogger::LogDec((int)KldIoApic::RegReads());
    SerialLogger::Log(" writes=");
    SerialLogger::LogDec((int)KldIoApic::RegWrites());
    SerialLogger::Log(" ioapics=");
    SerialLogger::LogDec((int)KldIoApic::IoApicCount());
    SerialLogger::Log(" (0 accesses = no container attached a line)\r\n");
    for (;;) Scheduler::SleepMs(60000);
}

// optional KURONO_KLD_FW=1 blob: unpack the ustar to /system/lib/firmware so
// request_firmware() finds it through the ordinary path. weak = default builds
// and command lines are unchanged. (satoru)
static void kld_firmware_init_once() {
    extern const uint8_t _binary_kld_firmware_tar_start[] __attribute__((weak));
    extern const uint8_t _binary_kld_firmware_tar_end[]   __attribute__((weak));
    static bool done;
    if (done)
        return;
    done = true;
    if (!_binary_kld_firmware_tar_start || !_binary_kld_firmware_tar_end ||
        +_binary_kld_firmware_tar_end <= +_binary_kld_firmware_tar_start)
        return;
    KVFS::Mkdirs("/system/lib/firmware");
    bool ok = PackageManager::ExtractTar((const char *)_binary_kld_firmware_tar_start,
                                         (int)(_binary_kld_firmware_tar_end -
                                               _binary_kld_firmware_tar_start),
                                         "/system/lib/firmware");
    SerialLogger::Log(ok ? "[kld] firmware unpacked to /system/lib/firmware\r\n"
                         : "[kld] firmware unpack FAILED\r\n");
}

static char g_kld_family_override[16];   // test-only family tag for the apitest embed slot (satoru)
static bool g_kld_i2chid_hostile;        // kurono.kld.i2chid.hostile=1: C5.8 fault test (satoru)
static int g_kld_loop;                   // kurono.kld.loop=N: run the apitest-slot / i2chid container N times in a row (satoru)

// one line per finished iteration of the loop: the frames in use after the
// release has settled, against the count before the first start. the point is
// a teardown that leaks nothing, so the delta must stay flat. (satoru)
static void kld_loop_note(int it, int n, uint64_t used0) {
    Scheduler::SleepMs(500);   // proven frees land after a tlb flush round (satoru)
    SerialLogger::Log("[kld-loop] ");
    SerialLogger::LogDec(it);
    SerialLogger::Log("/");
    SerialLogger::LogDec(n);
    SerialLogger::Log(" used frames now ");
    SerialLogger::LogDec((int)PMM::GetUsedFrames());
    SerialLogger::Log(" before the first start ");
    SerialLogger::LogDec((int)used0);
    SerialLogger::Log("\r\n");
}

// KLD shim self-test runner (kurono.kld=apitest): no device. the container
// runs [apitest] checks against the real shim primitives and prints one line
// per test plus the final N/N. (satoru)
[[noreturn]] static void kld_apitest_runner_entry() {
    Scheduler::SleepMs(3500);
    SerialLogger::Log("[apitest] launching /usr/bin/kld-apitest\r\n");
    if (!EmbeddedUserprogs::HasKldApitest()) {
        SerialLogger::Log("[apitest] container not embedded\r\n");
        for (;;) Scheduler::SleepMs(60000);
    }
    KVFS::Mkdirs("/usr/bin");
    KVFS::WriteFile("/usr/bin/kld-apitest", EmbeddedUserprogs::KldApitestData(),
                    EmbeddedUserprogs::KldApitestSize());
    KVFS::Chmod("/usr/bin/kld-apitest", 0755);
    int iters = g_kld_loop > 0 ? g_kld_loop : 1;
    uint64_t used0 = PMM::GetUsedFrames();
    for (int it = 0; it < iters; it++) {
        Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/kld-apitest", "kld-apitest");
        if (!p) {
            SerialLogger::Log("[apitest] load FAILED\r\n");
        } else {
            const char* av_yf[] = { "kld-apitest", "yieldfair", nullptr };
            const char* av[] = { "kld-apitest", nullptr };
            const char** argv = g_kldyieldfair ? av_yf : av;
            const char* envp_stats[] = { "PATH=/usr/bin", "KLD_STATS=1", nullptr };
            const char* envp[] = { "PATH=/usr/bin", nullptr };
            const char** env = g_kldstats ? envp_stats : envp;
            uint64_t as = p->address_space;
            if (g_kld_family_override[0]) {
                char path[32];
                int n = 0;
                const char* pre = "kld-";
                while (pre[n]) { path[n] = pre[n]; n++; }
                for (int i = 0; g_kld_family_override[i] && n < 30; i++)
                    path[n++] = g_kld_family_override[i];
                path[n] = 0;
                KldBroker::SetDmaBudgetForName(as, path, (int)p->pid);
            }
            int rc = Userspace::RunProcessWithArgs(p, argv, env);
            SerialLogger::Log("[apitest] container exited rc=");
            SerialLogger::LogDec(rc);
            SerialLogger::Log("\r\n");
            KldBroker::ReleaseAddressSpace(as);
        }
        if (g_kld_loop > 0) kld_loop_note(it + 1, iters, used0);
    }
    for (;;) Scheduler::SleepMs(60000);
}

// real touchpad family runner (kurono.kld=i2chid): ACPICA inside the container
// over the kernel's real tables, then the upstream lpss/designware/pinctrl/
// i2c-hid stack. it lives by itself here (no net bench parent) so the broker
// records its family from the runner path. (satoru)
[[noreturn]] static void kld_i2chid_runner_entry() {
    Scheduler::SleepMs(3500);
    SerialLogger::Log("[i2chid] launching /usr/bin/kld-i2chid\r\n");
    if (!EmbeddedUserprogs::HasKldI2chid()) {
        SerialLogger::Log("[i2chid] container not embedded\r\n");
        for (;;) Scheduler::SleepMs(60000);
    }
    KVFS::Mkdirs("/usr/bin");
    KVFS::WriteFile("/usr/bin/kld-i2chid", EmbeddedUserprogs::KldI2chidData(),
                    EmbeddedUserprogs::KldI2chidSize());
    KVFS::Chmod("/usr/bin/kld-i2chid", 0755);
    int iters = g_kld_loop > 0 ? g_kld_loop : 1;
    uint64_t used0 = PMM::GetUsedFrames();
    for (int it = 0; it < iters; it++) {
        Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/kld-i2chid", "kld-i2chid");
        if (!p) {
            SerialLogger::Log("[i2chid] load FAILED\r\n");
        } else {
            const char* av_hostile[] = { "kld-i2chid", "--hostile", nullptr };
            const char* av[] = { "kld-i2chid", nullptr };
            const char* envp[] = { "PATH=/usr/bin", nullptr };
            bool hostile = g_kld_i2chid_hostile;   // kurono.kld.i2chid.hostile=1 (satoru)
            uint64_t as = p->address_space;
            KldBroker::SetDmaBudgetForName(as, "/usr/bin/kld-i2chid", (int)p->pid);
            int rc = Userspace::RunProcessWithArgs(p, hostile ? av_hostile : av, envp);
            SerialLogger::Log("[i2chid] container exited rc=");
            SerialLogger::LogDec(rc);
            SerialLogger::Log("\r\n");
            KldBroker::ReleaseAddressSpace(as);
        }
        if (g_kld_loop > 0) kld_loop_note(it + 1, iters, used0);
    }
    for (;;) Scheduler::SleepMs(60000);
}

static bool g_kldwifi_scan;      // kurono.kldwifi=scan passes "scan" to the container (satoru)
static bool g_kldwifi_connect;   // kurono.kldwifi=connect reads fw_cfg and associates (satoru)
static bool g_kldbench_tls;       // kurono.kldbench.tls=1: bench the same HTTPS range over e1000e (satoru)
static int  g_kldbench_port = 8000;   // kurono.kldbench.port=N: kldbench/nettest host port (satoru)
static bool g_kldwifi_band24;     // kurono.kldwifi.band=2.4 diagnostic override (satoru)
static bool g_kldwifi_soak;       // kurono.kldwifi.soak=1 30-minute link soak (satoru)
static bool g_kldwifi_reconnect;  // kurono.kldwifi.reconnect=1 disconnect/reconnect (satoru)

// KLD iwlwifi container runner (kurono.kld=iwlwifi): the real BE200 through
// VFIO. the container runs unmodified iwlwifi/mac80211/cfg80211 and the runner
// only registers the 256 MiB DMA budget and tears the address space down. (satoru)
[[noreturn]] static void kld_iwlwifi_runner_entry() {
    Scheduler::SleepMs(3500);
    SerialLogger::Log("[kld-iwlwifi] launching /usr/bin/kld-iwlwifi\r\n");
    if (!EmbeddedUserprogs::HasKldIwlwifi()) {
        SerialLogger::Log("[kld-iwlwifi] container not embedded\r\n");
        for (;;) Scheduler::SleepMs(60000);
    }
    KVFS::Mkdirs("/usr/bin");
    KVFS::WriteFile("/usr/bin/kld-iwlwifi", EmbeddedUserprogs::KldIwlwifiData(),
                    EmbeddedUserprogs::KldIwlwifiSize());
    KVFS::Chmod("/usr/bin/kld-iwlwifi", 0755);
    Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/kld-iwlwifi", "kld-iwlwifi");
    if (!p) {
        SerialLogger::Log("[kld-iwlwifi] load FAILED\r\n");
    } else {
        if (g_kldwifi_connect)
            KldFwcfg::LoadWifiConf();
        const char* av_scan[] = { "kld-iwlwifi", "scan", nullptr };
        const char* av_connect[6];
        int ac = 0;
        av_connect[ac++] = "kld-iwlwifi";
        av_connect[ac++] = "connect";
        if (g_kldwifi_band24) av_connect[ac++] = "band24";
        if (g_kldwifi_soak) av_connect[ac++] = "soak";
        if (g_kldwifi_reconnect) av_connect[ac++] = "reconnect";
        av_connect[ac] = nullptr;
        const char* av_plain[] = { "kld-iwlwifi", nullptr };
        const char** av = g_kldwifi_connect ? av_connect :
                          (g_kldwifi_scan ? av_scan : av_plain);
        const char* envp[] = { "PATH=/usr/bin", nullptr };
        const char* envp_stats[] = { "PATH=/usr/bin", "KLD_STATS=1", nullptr };
        uint64_t as = p->address_space;
        KldBroker::SetDmaBudgetForName(as, "/usr/bin/kld-iwlwifi", (int)p->pid);
        int rc = Userspace::RunProcessWithArgs(p, av, g_kldstats ? envp_stats : envp);
        SerialLogger::Log("[kld-iwlwifi] container exited rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log("\r\n");
        if (g_kldwifi_connect)
            KldFwcfg::RemoveWifiConf();
        KldBroker::ReleaseAddressSpace(as);
    }
    for (;;) Scheduler::SleepMs(60000);
}

// KLD ACPI container runner (kurono.kld.acpi=1): unmodified ACPICA (linux
// 6.12.99 drivers/acpi/acpica) in a privileged ring-3 container, reading the
// real tables and hardware through the broker's acpi ops. prints its namespace
// walk and the evaluations below, then exits. (satoru)
[[noreturn]] static void kldacpi_runner_entry() {
    Scheduler::SleepMs(3000);
    if (!EmbeddedUserprogs::HasKldAcpi()) {
        SerialLogger::Log("[kld-acpi] not embedded\r\n");
        for (;;) Scheduler::SleepMs(60000);
    }
    KVFS::Mkdirs("/usr/bin");
    KVFS::WriteFile("/usr/bin/kld-acpi", EmbeddedUserprogs::KldAcpiData(), EmbeddedUserprogs::KldAcpiSize());
    Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/kld-acpi", "kld-acpi");
    if (!p) {
        SerialLogger::Log("[kld-acpi] load FAILED\r\n");
    } else {
        KldBroker::GrantAcpi(p->address_space);
        const char* av[] = { "kld-acpi", "--broker", "--shadowtest", "--devices",
                             "--eval", "\\_SB.PCI0._HID", "--crs", "\\_SB.PCI0",
                             "--eval", "\\_SB.PCI0._PRT", "--crs", "\\_SB.LNKA",
                             "--eval", "\\_S5", "--eval", "\\_SB.PCI0.ISA._ADR", nullptr };
        const char* envp[] = { "PATH=/usr/bin", nullptr };
        uint64_t as = p->address_space;

        int rc = Userspace::RunProcessWithArgs(p, av, envp);
        KldBroker::GrantAcpi(0);
        KldBroker::ReleaseAddressSpace(as);
        SerialLogger::Log("[kld-acpi] container exited rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log("\r\n");
    }
    for (;;) Scheduler::SleepMs(60000);
}

// KLD network a/b runner (kurono.kld.bench=1). runs the ring-3 bench; with
// kurono.kld=1 the bench spawns the driver container as its child, otherwise it
// measures the native driver. the bench must be ring 3: while a long-lived
// user process owns the bsp, cooperative kernel processes (nettest, the network
// loop) do not run. after the bench exits this runner gets the bsp back and
// uses the shell's kernel ping, which ring 3 cannot reach. (satoru)
[[noreturn]] static void kldbench_runner_entry() {
    Scheduler::SleepMs(3500);
    bool kld = KldNet::Wanted() && !g_kldwifi_connect;
    // linux processes see guest /usr/bin/X as kvfs /system/bin/X (ResolvePath), so the
    // bench's execve("/usr/bin/kld-e1000e") needs the container THERE. (satoru)
    if (kld && EmbeddedUserprogs::HasKldE1000e()) {
        KVFS::Mkdirs("/system/bin");
        KVFS::WriteFile("/system/bin/kld-e1000e", EmbeddedUserprogs::KldE1000eData(), EmbeddedUserprogs::KldE1000eSize());
        KVFS::Chmod("/system/bin/kld-e1000e", 0755);
    }
    SerialLogger::Log(kld ? "[kldbench] runner: KLD mode\r\n" : "[kldbench] runner: native mode\r\n");
    Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/kldnetbench", "kldnetbench");
    if (!p) {
        SerialLogger::Log("[kldbench] load FAILED\r\n");
    } else {
        const char* av_kld[] = { "kldnetbench", "--spawn", "/usr/bin/kld-e1000e", nullptr };
        const char* av_nat[] = { "kldnetbench", nullptr };
        const char* envp_nat[] = { "PATH=/usr/bin:/system/bin", "HOME=/root",
                                    "KLD_BENCH_LINK=eth0", nullptr };
        const char* envp_wifi[] = {
            "PATH=/usr/bin:/system/bin", "HOME=/root",
            "KLD_BENCH_TLS=1",
            "KLD_BENCH_HOST=kurono.satorut.com",
            "KLD_BENCH_PATH=/",
            "KLD_BENCH_BIG=/packages/firefox/firefox-140.11.0esr-full2.tar",
            "KLD_BENCH_LINK=wlan0",
            nullptr
        };
        const char* envp_wifi_eth[] = {
            "PATH=/usr/bin:/system/bin", "HOME=/root",
            "KLD_BENCH_TLS=1",
            "KLD_BENCH_HOST=kurono.satorut.com",
            "KLD_BENCH_PATH=/",
            "KLD_BENCH_BIG=/packages/firefox/firefox-140.11.0esr-full2.tar",
            "KLD_BENCH_LINK=eth0",
            nullptr
        };
        const char* envp_wifi_reconnect[] = {
            "PATH=/usr/bin:/system/bin", "HOME=/root",
            "KLD_BENCH_TLS=1",
            "KLD_BENCH_RECONNECT=1",
            "KLD_BENCH_HOST=kurono.satorut.com",
            "KLD_BENCH_PATH=/",
            "KLD_BENCH_LINK=wlan0",
            nullptr
        };
        const char* envp_stats[] = { "PATH=/usr/bin:/system/bin", "HOME=/root",
                                     "KLD_STATS=1", nullptr };
        const char* envp_wifi_soak[] = {
            "PATH=/usr/bin:/system/bin", "HOME=/root",
            "KLD_BENCH_TLS=1",
            "KLD_BENCH_SOAK=1",
            "KLD_BENCH_HOST=kurono.satorut.com",
            "KLD_BENCH_PATH=/",
            "KLD_BENCH_BIG=/packages/firefox/firefox-140.11.0esr-full2.tar",
            "KLD_BENCH_LINK=wlan0",
            nullptr
        };
        const char** envp = g_kldwifi_soak ? envp_wifi_soak :
                            (g_kldwifi_reconnect ? envp_wifi_reconnect :
                             (g_kldwifi_connect ? envp_wifi :
                              (g_kldbench_tls ? envp_wifi_eth : envp_nat)));
        // the host http port is a boot parameter (KLD_HTTP_PORT in the rigs). (satoru)
        static char bench_port_env[32];
        {
            char* q = kld_append_str(bench_port_env, "KLD_BENCH_PORT=");
            q = kld_append_int(q, g_kldbench_port);
            *q = 0;
        }
        const char* envp_final[20];
        int ne = 0;
        while (envp[ne] && ne < 18) { envp_final[ne] = envp[ne]; ne++; }
        envp_final[ne++] = bench_port_env;
        envp_final[ne] = nullptr;
        int rc = Userspace::RunProcessWithArgs(p, kld ? av_kld : av_nat, envp_final);
        SerialLogger::Log("[kldbench] bench exited rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log("\r\n");
    }
    static char buf[4096];
    for (int i = 0; i < 3; i++) {
        int n = KuronoShell::Execute("ping 10.0.2.2", buf, (int)sizeof(buf) - 1);
        if (n < 0) n = 0;
        buf[n] = 0;
        SerialLogger::Log("[kldbench] $ ping 10.0.2.2\r\n");
        SerialLogger::Log(buf);
        SerialLogger::Log("\r\n");
        Scheduler::SleepMs(500);
    }
    SerialLogger::Log("[kldbench] runner DONE\r\n");
    for (;;) Scheduler::SleepMs(60000);
}

// privilege-gate regression runner (kurono.kentest=1). creates a root-owned
// linux process record (no task, so signal 0 is enough) as the "foreign"
// kill target, then runs /usr/bin/kentest as uid 1000. (satoru)
[[noreturn]] static void kentest_runner_entry() {
    Scheduler::SleepMs(4000);
    int dummy = LinuxSyscall::CreateProcess("kendummy", 0, 0);
    LinuxProcess* dl = dummy >= 0 ? LinuxSyscall::GetProcess(dummy) : nullptr;
    char pidbuf[16]; pidbuf[0] = 0;
    if (dl) {
        int n = 0, v = (int)dl->pid;
        char t[16]; int k = 0;
        if (v == 0) t[k++] = '0';
        while (v > 0) { t[k++] = (char)('0' + v % 10); v /= 10; }
        while (k) pidbuf[n++] = t[--k];
        pidbuf[n] = 0;
    }
    SerialLogger::Log("[kentest] launching /usr/bin/kentest as uid 1000\r\n");
    Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/kentest", "kentest");
    if (!p) {
        SerialLogger::Log("[kentest] load FAILED\r\n");
    } else {
        const char* av[] = { "kentest", "foreign", pidbuf, nullptr };
        const char* envp[] = { "PATH=/usr/bin", "HOME=/home/user", nullptr };
        int rc = Userspace::RunProcessWithArgs(p, av, envp, 1000, 1000);
        SerialLogger::Log("[kentest] runner rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log("\r\n");
    }
    if (dummy >= 0) LinuxSyscall::DestroyProcess(dummy);
    SerialLogger::Log("[kentest] DONE\r\n");
    for (;;) Scheduler::SleepMs(60000);
}

// user-pointer validation regression runner (kurono.uatest=1). the audit's #1
// finding was that syscalls dereferenced user pointers unvalidated; this proves
// uaccess rejects a kernel address instead. logs [uatest] lines + OVERALL
// PASS/FAIL, then dumps the reject counters. (satoru)
[[noreturn]] static void uatest_runner_entry() {
    Scheduler::SleepMs(3000);
    SerialLogger::Log("[uatest] launching /usr/bin/uatest\r\n");
    Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/uatest", "uatest");
    if (!p) {
        SerialLogger::Log("[uatest] load FAILED\r\n");
    } else {
        const char* av[] = { "uatest", nullptr };
        const char* envp[] = { "PATH=/usr/bin", nullptr };
        int rc = Userspace::RunProcessWithArgs(p, av, envp);
        SerialLogger::Log("[uatest] runner rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log("\r\n");
        UAccess::LogStats();
    }
    for (;;) Scheduler::SleepMs(60000);
}

// parallel tree-walk runner (kurono.findtest=1). runs /usr/bin/findtest as a
// normal ring-3 user: several processes walk /system /usr /home with
// getdents64 + fstatat + readlinkat + openat while cache files are created and
// removed, the way claude's skill and Glob/Grep searches exercise the vfs. the
// runner prints FINDTEST_PASS/FAIL so a headless boot can classify it.
[[noreturn]] static void findtest_runner_entry() {
    Scheduler::SleepMs(3000);
    SerialLogger::Log("[findtest] launching /usr/bin/findtest\r\n");
    Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/findtest", "findtest");
    if (!p) {
        SerialLogger::Log("[findtest] load FAILED\r\n");
    } else {
        const char* av[] = { "findtest", "4", "3", "1", "1", "2000", "3", "3", nullptr };
        const char* envp[] = { "PATH=/usr/bin:/system/bin", "HOME=/home/user", "TMPDIR=/tmp", nullptr };
        LinuxSyscall::ClearConsoleOutput();
        int rc = Userspace::RunProcessWithArgs(p, av, envp, 1000, 1000);
        SerialLogger::Log("\r\n[findtest] runner rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log(rc == 0 ? " FINDTEST_PASS\r\n" : " FINDTEST_FAIL\r\n");
    }
    for (;;) Scheduler::SleepMs(60000);
}

// ring-3 memory fill + verify runner (kurono.memtest=1). the user-space proof
// for the pmm managing ram above the old 16 gb cap: /usr/bin/memtest maps and
// pattern-checks ~75% of free ram, and the pmm's own counter reports how many
// frames above 16 gb were handed out at the peak. (satoru)
extern "C" const uint8_t _binary_churn_elf_start[] __attribute__((weak));
extern "C" const uint8_t _binary_churn_elf_end[]   __attribute__((weak));
static char g_churn_rounds[24] = {0};   // kurono.churn.rounds=N (default 200, 0 = forever) (satoru)
static int  g_mmapchurn_delay_s = -1;   // kurono.mmapchurn=<s>: run churn pairs after s (satoru)
static int  g_kldperm_delay_s = -1;     // kurono.kldperm=<s>: run churn kldop after s (satoru)
// leak census workload runner (kurono.churn=1): /usr/bin/churn forks, execs,
// maps and spawns threads in a loop; a census report brackets it. (satoru)
[[noreturn]] static void churn_runner_entry() {
    Scheduler::SleepMs(3000);
    MemCensus::Report("churn-start");
    SerialLogger::Log("[churn] launching /usr/bin/churn\r\n");
    TaskMem::LogTop("churn-start");   // (satoru)
    Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/churn", "churn");
    if (!p) {
        SerialLogger::Log("[churn] load FAILED\r\n");
    } else {
        const char* av[] = { "churn", g_churn_rounds[0] ? g_churn_rounds : nullptr, nullptr };
        const char* envp[] = { "PATH=/usr/bin:/system/bin", nullptr };
        int rc = Userspace::RunProcessWithArgs(p, av, envp);
        SerialLogger::Log("[churn] runner rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log("\r\n");
        Scheduler::SleepMs(2000);
        MemCensus::Report("churn-end");
        TaskMem::LogTop("churn-end");   // kurono.taskmem=1: who still holds kernel heap (satoru)
    }
    for (;;) Scheduler::SleepMs(60000);
}

// kurono.mmapchurn=<s>: run the churn program's 10000 mmap/munmap pairs as an
// ordinary process after s seconds (D2.12: broker records must not outlive
// their address space and poison a later process's munmap). (satoru)
[[noreturn]] static void mmapchurn_runner_entry() {
    if (g_mmapchurn_delay_s > 0)
        Scheduler::SleepMs((uint32_t)g_mmapchurn_delay_s * 1000);
    SerialLogger::Log("[mmapchurn] launching /usr/bin/churn pairs\r\n");
    Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/churn", "mmapchurn");
    if (!p) {
        SerialLogger::Log("[mmapchurn] load FAILED\r\n");
    } else {
        const char* av[] = { "churn", "pairs", nullptr };
        const char* envp[] = { "PATH=/usr/bin:/system/bin", nullptr };
        int rc = Userspace::RunProcessWithArgs(p, av, envp);
        SerialLogger::Log("[mmapchurn] runner rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log("\r\n");
    }
    for (;;) Scheduler::SleepMs(60000);
}

// kurono.kldperm=<s>: after s seconds run one kld broker op as an ordinary
// process, 60 times, 1 s apart (each run is a new address space, so with
// kurono.directmap=1 and a display kill loop some land on a dead container's
// freed PML4 frame). every run must get EPERM; the broker logs the runs that
// hit a dead container's address space. (satoru)
[[noreturn]] static void kldperm_runner_entry() {
    if (g_kldperm_delay_s > 0)
        Scheduler::SleepMs((uint32_t)g_kldperm_delay_s * 1000);
    int ok = 0, bad = 0;
    for (int i = 0; i < 60; i++) {
        Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/churn", "kldperm");
        if (!p) {
            SerialLogger::Log("[kldperm] load FAILED\r\n");
            break;
        }
        SerialLogger::Log("[kldperm] run as=0x");
        SerialLogger::LogHex64(p->address_space);
        SerialLogger::Log("\r\n");
        const char* av[] = { "churn", "kldop", nullptr };
        const char* envp[] = { "PATH=/usr/bin:/system/bin", nullptr };
        int rc = Userspace::RunProcessWithArgs(p, av, envp);
        if (rc == 0) ok++; else bad++;
        Scheduler::SleepMs(1000);
    }
    SerialLogger::Log("[kldperm] done eperm=");
    SerialLogger::LogDec(ok);
    SerialLogger::Log(" other=");
    SerialLogger::LogDec(bad);
    SerialLogger::Log("\r\n");
    for (;;) Scheduler::SleepMs(60000);
}

// kurono.elfreloadtest=1: a ring-3 fault kill must free the killed static
// image's low-va frames, so the same binary can be loaded again immediately.
// the first process is dispatched on an AP (sched=2 containment) and gets an
// unmapped entry rip; the second load must succeed. in demand mode the frames
// are identity backed, so a leaked group owns 0x400000 and the reload fails
// with "[ELF] map_one_page failed". (satoru)
[[noreturn]] static void elfreload_runner_entry() {
    Scheduler::SleepMs(2500);
    const uint8_t* img = EmbeddedUserprogs::MuslHelloData();
    uint32_t sz = EmbeddedUserprogs::MuslHelloSize();
    int pass = 0, fail = 0;
    for (int round = 1; round <= 3; round++) {
        SerialLogger::Log("[elfreload] round ");
        SerialLogger::LogDec(round);
        SerialLogger::Log("\r\n");
        Process* p = ElfLoader::LoadELF64(img, sz, "elfreload");
        if (!p) { SerialLogger::Log("[elfreload] initial load FAILED\r\n"); fail++; break; }
        p->rip = 0xDEAD0000ULL;
        p->user_frame.rip = 0xDEAD0000ULL;
        const char* av[] = { "elfreload", nullptr };
        const char* ev[] = { "PATH=/system/bin", nullptr };
        DetachedHandle h;
        if (Userspace::SpawnDetached(p, av, ev, 0, 0, &h) != 0) {
            SerialLogger::Log("[elfreload] spawn FAILED\r\n");
            Scheduler::DestroyProcess(p);
            fail++;
            continue;
        }
        for (int i = 0; i < 200 && Userspace::DetachedAlive(h); i++) Scheduler::SleepMs(25);
        Userspace::ReapDetached(h);
        Scheduler::SleepMs(800);   // let the deferred group reap free the space
        Process* q = ElfLoader::LoadELF64(img, sz, "elfreload2");
        if (q) {
            SerialLogger::Log("[elfreload] round ");
            SerialLogger::LogDec(round);
            SerialLogger::Log(" reload OK\r\n");
            Scheduler::DestroyProcess(q);
            pass++;
        } else {
            SerialLogger::Log("[elfreload] round ");
            SerialLogger::LogDec(round);
            SerialLogger::Log(" reload FAILED (low image frames still owned)\r\n");
            fail++;
        }
    }
    SerialLogger::Log("[elfreload] RESULT pass=");
    SerialLogger::LogDec(pass);
    SerialLogger::Log(" fail=");
    SerialLogger::LogDec(fail);
    SerialLogger::Log("\r\n");
    for (;;) Scheduler::SleepMs(60000);
}

extern "C" const uint8_t _binary_memtest_elf_start[] __attribute__((weak));
extern "C" const uint8_t _binary_memtest_elf_end[]   __attribute__((weak));
static char g_memtest_pct[8] = {0};   // kurono.memtest.pct=N: share of free ram to fill (default 75) (satoru)
[[noreturn]] static void memtest_runner_entry() {
    Scheduler::SleepMs(3000);
    SerialLogger::Log("[memtest] launching /usr/bin/memtest\r\n");
    Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/memtest", "memtest");
    if (!p) {
        SerialLogger::Log("[memtest] load FAILED\r\n");
    } else {
        const char* av[] = { "memtest", g_memtest_pct[0] ? g_memtest_pct : nullptr, nullptr };
        const char* envp[] = { "PATH=/usr/bin", nullptr };
        uint64_t hi_before = PMM::HighFramesInUse();
        PMM::ResetHighPeak();
        int rc = Userspace::RunProcessWithArgs(p, av, envp);
        uint64_t hi_peak = PMM::HighFramesPeak();
        hi_peak = hi_peak > hi_before ? hi_peak - hi_before : 0;
        SerialLogger::Log("[memtest] runner rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log(" peak_frames_above_16g=");
        SerialLogger::LogDec((uint32_t)hi_peak);
        SerialLogger::Log(" (");
        SerialLogger::LogDec((uint32_t)(hi_peak >> 8));
        SerialLogger::Log(" MB above the 16 gb boot-time level) in_use_after=");
        SerialLogger::LogDec((uint32_t)PMM::HighFramesInUse());
        SerialLogger::Log(" phys_top_mb=");
        SerialLogger::LogDec((uint32_t)(PMM::GetPhysTop() >> 20));
        SerialLogger::Log("\r\n");
    }
    for (;;) Scheduler::SleepMs(60000);
}

// /dev/kshell gate runner (kurono.kshtest=1). the rescue agent's whole tool
// loop rides on this device, so it gets a headless regression of its own -
// including the kernel-side refusal of destructive commands. (satoru)
[[noreturn]] static void kshtest_runner_entry() {
    Scheduler::SleepMs(3000);
    SerialLogger::Log("[kshtest] launching /usr/bin/kshtest\r\n");
    Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/kshtest", "kshtest");
    if (!p) {
        SerialLogger::Log("[kshtest] load FAILED\r\n");
    } else {
        const char* av[] = { "kshtest", nullptr };
        const char* envp[] = { "PATH=/usr/bin", nullptr };
        int rc = Userspace::RunProcessWithArgs(p, av, envp);
        SerialLogger::Log("[kshtest] runner rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log("\r\n");
    }
    for (;;) Scheduler::SleepMs(60000);
}

// ai recovery client runner (kurono.dsai=1). the end-to-end gate for the
// rescue use case: dns -> tcp -> tls -> deepseek -> /dev/kshell tool loop,
// all from ring 3. waits longer than the other gates because the network
// stack and dhcp need to settle first. (satoru)
[[noreturn]] static void dsai_runner_entry() {
    Scheduler::SleepMs(12000);
    SerialLogger::Log("[dsai] launching /usr/bin/dsai\r\n");
    Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/dsai", "dsai");
    if (!p) {
        SerialLogger::Log("[dsai] load FAILED\r\n");
    } else {
        const char* av[] = { "dsai",
            "Identify the storage on this machine using your tools, then say "
            "in one sentence whether it looks healthy.", nullptr };
        const char* envp[] = { "PATH=/usr/bin", "HOME=/root",
                               "DSAI_MODEL=deepseek-v4-flash", nullptr };
        int rc = Userspace::RunProcessWithArgs(p, av, envp);
        SerialLogger::Log("[dsai] runner rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log("\r\n");
    }
    for (;;) Scheduler::SleepMs(60000);
}

// third-party linux binary gate (kurono.bbtest=1). busybox is PREBUILT and
// glibc-static - we did not compile it, did not link it against our musl, and
// did not adjust it in any way. that is the whole point: it exercises the kls
// syscall surface the way a real recovery tool would, instead of the way code
// written against kurono does. (satoru)
// headless recovery toolbox gate (kurono.rtest=1): drives the same shell
// commands a user would type and logs their output to com1. (satoru)
[[noreturn]] static void rtest_runner_entry() {
    Scheduler::SleepMs(8000);
    static const char* const CMDS[] = {
        "recovery", "ntfsfix /dev/sda1", "ntfsinfo -f -m /dev/sda1",
        "ntfsls -f /dev/sda1", "busybox ls -l /dev", "e2fsck -n /dev/sda2", "fsck.fat -n /dev/sda3",
        "testdisk /list", "testdisk /log /cmd /dev/sda analyze,list",
        "photorec /d /tmp/rec /cmd /dev/sda partition_none,fileopt,everything,enable,search",
        "busybox ls -lR /tmp", "busybox tail -n 25 testdisk.log", nullptr
    };
    static char buf[16384];
    for (int i = 0; CMDS[i]; i++) {
        SerialLogger::Log("[rtest] $ ");
        SerialLogger::Log(CMDS[i]);
        SerialLogger::Log("\r\n");
        int n = KuronoShell::Execute(CMDS[i], buf, (int)sizeof(buf) - 1);
        if (n < 0) n = 0;
        if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
        buf[n] = 0;
        SerialLogger::Log(buf);
        SerialLogger::Log("\r\n[rtest] ---\r\n");
    }
    SerialLogger::Log("[rtest] DONE\r\n");
    for (;;) Scheduler::SleepMs(60000);
}

// hd audio gate (kurono.audiotest=1): status, a 440 Hz tone, a driver restart,
// then an 880 Hz tone, so a captured wav proves both the stream and the restart.
// powers off when done so the vm exits by itself. (satoru)
// paced tone: AudioServer::PlayTone does not wait for room in the mixer stream
// ring, so a multi-second tone from a non-gui context would be truncated. this
// writes in chunks and sleeps while the ring is full. parabolic sine (satoru)
static void audiotest_tone(int hz, int ms) {
    AudioMixer::StreamID id = AudioServer::OpenStream("audiotest", AudioFormat::FMT_S16_LE, 48000, 1);
    if (id == AudioMixer::INVALID_STREAM) { SerialLogger::Log("[audiotest] open failed\r\n"); return; }
    AudioMixer::SetVolume(id, 100);
    SerialLogger::Log("[audiotest] stream open\r\n");
    static int16_t chunk[512];
    uint32_t total = (uint32_t)(48 * ms), done = 0, phase = 0;
    uint32_t inc = (uint32_t)(((uint64_t)hz << 16) / 48000);
    while (done < total) {
        uint32_t n = total - done;
        if (n > 512) n = 512;
        for (uint32_t i = 0; i < n; i++) {
            int32_t x = (int32_t)(phase & 0x7FFF);
            int32_t y = (x * (32768 - x)) >> 13;          // 0..32768
            if (phase & 0x8000) y = -y;
            chunk[i] = (int16_t)(y * 3 / 4);               // ~-2.5 dBfs peak
            phase = (phase + inc) & 0xFFFF;
        }
        uint32_t off = 0;
        long spins = 0;
        while (off < n && spins < 40000000L) {
            uint32_t w = AudioServer::WriteStream(id, chunk + off, n - off);
            off += w;
            // pump the mixer from here too: in this headless vm the audio process
            // is scheduled far too rarely to keep a real-time tone fed (satoru)
            AudioServer::LockedTick(); USB::PollHID();   // the input process is starved in this loop; drain usb events (satoru)
            if (w == 0) { __asm__ __volatile__("pause"); spins++; }   // no sleep: sleeps stretch to seconds here (satoru)
        }
        if (off < n) break;                               // gave up, ring never drained
        done += n;
        if ((done & 0x3FFF) < 512) { SerialLogger::Log("[audiotest] frames "); SerialLogger::LogDec((int)done); SerialLogger::Log("\r\n"); }
    }
    SerialLogger::Log("[audiotest] tone written\r\n");
    AudioServer::DrainStream(id);
    {
        // drain tail: keep pumping ~1.5 s (clock bounded and iteration capped) (satoru)
        uint32_t t0 = Timer::GetRealMs();
        for (long i = 0; i < 100000000L && (Timer::GetRealMs() - t0) < 1500u; i++) {
            AudioServer::LockedTick(); USB::PollHID();
            __asm__ __volatile__("pause");
        }
    }
    AudioServer::CloseStream(id);
}

[[noreturn]] static void audiotest_runner_entry() {
    static char buf[4096];
    Scheduler::SleepMs(6000);
    SerialLogger::Log("[audiotest] begin\r\n");
    KuronoShell::Execute("audio status", buf, (int)sizeof(buf) - 1);
    SerialLogger::Log(buf); SerialLogger::Log("\r\n");
    audiotest_tone(440, 3000);
    SerialLogger::Log("[audiotest] tone1 done lpib=");
    SerialLogger::LogDec((int)HDAudio::GetPosition());
    SerialLogger::Log("\r\n");
    Scheduler::SleepMs(1000);
    KuronoShell::Execute("audio restart", buf, (int)sizeof(buf) - 1);
    SerialLogger::Log(buf); SerialLogger::Log("\r\n");
    audiotest_tone(880, 2000);
    SerialLogger::Log("[audiotest] tone2 done\r\n");
    Scheduler::SleepMs(1000);
    SerialLogger::Log("[audiotest] end\r\n");
    HAL::PowerOff();
    while (true) Scheduler::SleepMs(1000);
}

namespace AudioServer { void PumpStats(uint32_t* calls, uint32_t* max_gap_ms); }

// streaming-client gate (kurono.audiotest=2): behaves like a pulse client. a 44.1 kHz stereo
// s16 tone (176400 bytes/s expected) is written at the pace the clock says playback consumes it,
// sleeping 10 ms between writes, WITHOUT pumping the mixer itself - the audio process has to
// keep it fed. once a second it logs bytes accepted, client backlog (frames the mixer refused),
// mixer underruns, pump count + longest pump gap and the client's own wake latency. at 5 s it
// runs `audio restart` mid-stream. (satoru)
[[noreturn]] static void audiotest2_entry() {
    static char buf[4096];
    static int16_t chunk[2 * 2048];
    Scheduler::SleepMs(6000);
    SerialLogger::Log("[audio2] begin\r\n");
    AudioMixer::StreamID id = AudioServer::OpenStream("audio2", AudioFormat::FMT_S16_LE, 44100, 2);
    if (id == AudioMixer::INVALID_STREAM) { SerialLogger::Log("[audio2] open failed\r\n"); HAL::PowerOff(); for (;;) Scheduler::SleepMs(1000); }
    AudioMixer::SetVolume(id, 100);
    uint32_t t0 = Timer::GetRealMs(), last_log = t0, written = 0, phase = 0, backlog_max = 0;
    uint32_t wake_max = 0, wake_sum = 0, wake_n = 0, sec = 0;
    bool restarted = false;
    const uint32_t inc = (uint32_t)(((uint64_t)440 << 16) / 44100);
    for (;;) {
        uint32_t tb = Timer::GetRealMs();
        Scheduler::SleepMs(10);
        uint32_t lat = Timer::GetRealMs() - tb;
        lat = lat > 10 ? lat - 10 : 0;
        if (lat > wake_max) wake_max = lat;
        wake_sum += lat; wake_n++;
        uint32_t now = Timer::GetRealMs();
        uint32_t el = now - t0;
        if (el >= 10000) break;
        // stay ~100 ms ahead of the playback clock, like a pulse client holding its tlength fill
        uint32_t due = (uint32_t)(((uint64_t)el * 44100) / 1000) + 4410;
        uint32_t guard = 0;
        while (written < due && guard++ < 8) {
            uint32_t n = due - written;
            if (n > 2048) n = 2048;
            for (uint32_t i = 0; i < n; i++) {
                int32_t x = (int32_t)(phase & 0x7FFF);
                int32_t y = (x * (32768 - x)) >> 13;
                if (phase & 0x8000) y = -y;
                chunk[2 * i] = chunk[2 * i + 1] = (int16_t)(y * 3 / 4);
                phase = (phase + inc) & 0xFFFF;
            }
            uint32_t w = AudioServer::WriteStream(id, chunk, n);
            // a partial accept: the unaccepted tail is dropped so the phase stays continuous
            // with what was taken (a real client would hold it and retry next REQUEST)
            written += w;
            if (w < n) { break; }
        }
        uint32_t backlog = due > written ? due - written : 0;
        if (backlog > backlog_max) backlog_max = backlog;
        if (!restarted && el >= 5000) {
            restarted = true;
            KuronoShell::Execute("audio restart", buf, (int)sizeof(buf) - 1);
            SerialLogger::Log("[audio2] audio restart issued mid-stream\r\n");
        }
        if (now - last_log >= 1000) {
            last_log = now; sec++;
            AudioMixer::StreamStats st = AudioMixer::GetStats(id);
            uint32_t calls = 0, gap = 0;
            AudioServer::PumpStats(&calls, &gap);
            SerialLogger::Log("[audio2] t="); SerialLogger::LogDec((int)el);
            SerialLogger::Log("ms accepted_bytes="); SerialLogger::LogDec((int)(written * 4));
            SerialLogger::Log(" backlog_frames="); SerialLogger::LogDec((int)backlog);
            SerialLogger::Log(" backlog_max="); SerialLogger::LogDec((int)backlog_max);
            SerialLogger::Log(" underruns="); SerialLogger::LogDec((int)st.underruns);
            SerialLogger::Log(" overflows="); SerialLogger::LogDec((int)st.overflows);
            SerialLogger::Log(" pumps/s="); SerialLogger::LogDec((int)calls);
            SerialLogger::Log(" pump_gap_max="); SerialLogger::LogDec((int)gap);
            SerialLogger::Log("ms wake_max="); SerialLogger::LogDec((int)wake_max);
            SerialLogger::Log("ms wake_mean="); SerialLogger::LogDec((int)(wake_n ? wake_sum / wake_n : 0));
            SerialLogger::Log("ms\r\n");
            wake_max = 0; wake_sum = 0; wake_n = 0;
        }
    }
    uint32_t el = Timer::GetRealMs() - t0;
    AudioMixer::StreamStats st = AudioMixer::GetStats(id);
    SerialLogger::Log("[audio2] RESULT elapsed_ms="); SerialLogger::LogDec((int)el);
    SerialLogger::Log(" accepted_bytes="); SerialLogger::LogDec((int)(written * 4));
    SerialLogger::Log(" bytes_per_s="); SerialLogger::LogDec((int)((uint64_t)written * 4 * 1000 / (el ? el : 1)));
    SerialLogger::Log(" expected=176400 underruns="); SerialLogger::LogDec((int)st.underruns);
    SerialLogger::Log(" consumed_frames="); SerialLogger::LogDec((int)st.frames_consumed);
    SerialLogger::Log(" backlog_max="); SerialLogger::LogDec((int)backlog_max);
    SerialLogger::Log("\r\n");
    AudioServer::DrainStream(id);
    Scheduler::SleepMs(2000);
    AudioServer::CloseStream(id);
    SerialLogger::Log("[audio2] end\r\n");
    HAL::PowerOff();
    for (;;) Scheduler::SleepMs(1000);
}

// usb audio soak gate (kurono.usbaudiotest=1): a few minutes of tone through
// the usb audio backend, with the recovery paths exercised mid-stream. each
// segment runs a command at a checkpoint: `audio restart` from the shell,
// `audio stop` from the shell, and the same two through /system/bin/kaudio
// (the ring-3 tool that writes /dev/kaudio). the 30 s [usbaudio] stats lines
// are the health record. powers off when done. (satoru)
static int run_kaudio_tool(const char* sub, char* out, int mx) {
    if (!Userspace::IsReady()) Userspace::Init();
    if (!KVFS::Exists("/system/bin/kaudio")) { out[0] = 0; return -2; }
    Process* proc = ElfLoader::LoadELF64FromVFS("/system/bin/kaudio", "kaudio");
    if (!proc) { out[0] = 0; return -3; }
    const char* av[] = { "kaudio", sub, nullptr };
    const char* envp[] = { "PATH=/system/bin:/usr/bin", "HOME=/root", "TMPDIR=/tmp", nullptr };
    LinuxSyscall::ClearConsoleOutput();
    int rc = Userspace::RunProcessWithArgs(proc, av, envp, 1000, 1000);
    int n = LinuxSyscall::ReadConsoleOutput(out, mx - 1);
    if (n < 0) n = 0;
    out[n] = 0;
    Scheduler::DestroyProcess(proc);
    return rc;
}

static bool usbaudiotest_tone(int hz, int ms, int action_ms, const char* action, bool via_tool) {
    AudioMixer::StreamID id = AudioServer::OpenStream("usbaudio-soak", AudioFormat::FMT_S16_LE, 48000, 2);
    if (id == AudioMixer::INVALID_STREAM) {
        SerialLogger::Log("[usbaudio-soak] open failed\r\n");
        return false;
    }
    AudioMixer::SetVolume(id, 100);
    static int16_t chunk[1024 * 2];
    uint32_t total = (uint32_t)(48 * ms), done = 0, phase = 0;
    uint32_t inc = (uint32_t)(((uint64_t)hz << 16) / 48000);
    uint32_t t0 = Timer::GetRealMs();
    bool acted = false, ended = false;
    while (done < total) {
        if (!acted && (int)(Timer::GetRealMs() - t0) >= action_ms) {
            acted = true;
            static char sbuf[4096];
            if (via_tool) {
                int rc = run_kaudio_tool(action, sbuf, (int)sizeof(sbuf));
                SerialLogger::Log("[usbaudio-soak] kaudio "); SerialLogger::Log(action);
                SerialLogger::Log(" rc="); SerialLogger::LogDec(rc);
                SerialLogger::Log(": "); SerialLogger::Log(sbuf);
                SerialLogger::Log("\r\n");
            } else {
                int n = KuronoShell::Execute(action, sbuf, (int)sizeof(sbuf) - 1);
                if (n > 0) { sbuf[n] = 0; SerialLogger::Log("[usbaudio-soak] $ "); SerialLogger::Log(action);
                             SerialLogger::Log("\r\n"); SerialLogger::Log(sbuf); SerialLogger::Log("\r\n"); }
            }
        }
        uint32_t n = total - done;
        if (n > 1024) n = 1024;
        for (uint32_t i = 0; i < n; i++) {
            int32_t x = (int32_t)(phase & 0x7FFF);
            int32_t y = (x * (32768 - x)) >> 13;
            if (phase & 0x8000) y = -y;
            chunk[2 * i] = chunk[2 * i + 1] = (int16_t)(y * 3 / 4);
            phase = (phase + inc) & 0xFFFF;
        }
        uint32_t off = 0;
        long spins = 0;
        while (off < n && spins < 400000L) {
            uint32_t w = AudioServer::WriteStream(id, chunk + off * 2, n - off);
            off += w;
            AudioServer::LockedTick(); USB::PollHID();
            if (w == 0) { __asm__ __volatile__("pause"); spins++; }
        }
        if (off < n) {
            // the stream was force-freed by `audio stop`, or the backend is gone. (satoru)
            ended = true;
            break;
        }
        done += n;
    }
    SerialLogger::Log("[usbaudio-soak] tone "); SerialLogger::LogDec(hz);
    SerialLogger::Log(" ms="); SerialLogger::LogDec(ms);
    SerialLogger::Log(" wrote="); SerialLogger::LogDec((int)done);
    SerialLogger::Log(ended ? " (ended by action)\r\n" : " (complete)\r\n");
    AudioServer::DrainStream(id);
    for (long i = 0; i < 20000; i++) { AudioServer::LockedTick(); USB::PollHID(); __asm__ __volatile__("pause"); }
    AudioServer::CloseStream(id);
    return true;
}

[[noreturn]] static void usbaudiotest_runner_entry() {
    static char buf[8192];
    Scheduler::SleepMs(8000);
    SerialLogger::Log("[usbaudio-soak] begin\r\n");
    int n = KuronoShell::Execute("audio status", buf, (int)sizeof(buf) - 1);
    if (n > 0) { buf[n] = 0; SerialLogger::Log(buf); SerialLogger::Log("\r\n"); }
    // 120 s with a shell restart at 30 s (the stream must keep going) (satoru)
    usbaudiotest_tone(440, 120000, 30000, "audio restart", false);
    // 60 s with a shell stop at 20 s (the stream must end) (satoru)
    usbaudiotest_tone(660, 60000, 20000, "audio stop", false);
    // the ring-3 tool: status, then a restart, then 90 s stopped mid-way by it (satoru)
    if (run_kaudio_tool("status", buf, (int)sizeof(buf)) >= 0) {
        SerialLogger::Log("[usbaudio-soak] kaudio status: "); SerialLogger::Log(buf); SerialLogger::Log("\r\n");
    } else {
        SerialLogger::Log("[usbaudio-soak] kaudio tool missing\r\n");
    }
    if (run_kaudio_tool("restart", buf, (int)sizeof(buf)) >= 0) {
        SerialLogger::Log("[usbaudio-soak] kaudio restart: "); SerialLogger::Log(buf); SerialLogger::Log("\r\n");
    }
    usbaudiotest_tone(880, 90000, 30000, "stop", true);
    run_kaudio_tool("status", buf, (int)sizeof(buf));
    SerialLogger::Log("[usbaudio-soak] final status: "); SerialLogger::Log(buf); SerialLogger::Log("\r\n");
    SerialLogger::Log("[usbaudio-soak] DONE\r\n");
    HAL::PowerOff();
    for (;;) Scheduler::SleepMs(1000);
}

// headless nic gate (kurono.nettest=1): drives real traffic through eth0 (ping,
// dns, an http fetch from the qemu host at 10.0.2.2:8000), restarts the nic
// driver with `nic restart`, then repeats the traffic to prove the data path
// survived. output goes to com1. (satoru)
[[noreturn]] static void nettest_runner_entry() {
    SerialLogger::Log("[r3audiotest] runner up\r\n");
    Scheduler::SleepMs(7000);
    static char curl_cmd[96];
    {
        char* q = kld_append_str(curl_cmd, "curl http://10.0.2.2:");
        q = kld_append_int(q, g_kldbench_port);
        q = kld_append_str(q, "/hello.txt");
        *q = 0;
    }
    const char* const CMDS[] = {
        "nic", "ping 10.0.2.2", "ping example.com", curl_cmd,
        "nic restart", "ping 10.0.2.2", curl_cmd, "nic", nullptr
    };
    static char buf[8192];
    for (int i = 0; CMDS[i]; i++) {
        SerialLogger::Log("[nettest] $ ");
        SerialLogger::Log(CMDS[i]);
        SerialLogger::Log("\r\n");
        int n = KuronoShell::Execute(CMDS[i], buf, (int)sizeof(buf) - 1);
        if (n < 0) n = 0;
        if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
        buf[n] = 0;
        SerialLogger::Log(buf);
        SerialLogger::Log("\r\n[nettest] ---\r\n");
        Scheduler::SleepMs(1500);
    }
    SerialLogger::Log("[nettest] DONE\r\n");
    for (;;) Scheduler::SleepMs(60000);
}

[[noreturn]] static void bbtest_runner_entry() {
    Scheduler::SleepMs(6000);
    // one argument per run. swap the blob at src/userprogs/busybox.elf to
    // test any prebuilt binary; these args suit busybox and ntfsprogs both.
    // (satoru)
    static const char* const TRIES[] = { "/dev/sda1", "-n /dev/sda1",
                                         "/dev/sda", nullptr };
    for (int i = 0; TRIES[i]; i++) {
        SerialLogger::Log("[bbtest] --- busybox ");
        SerialLogger::Log(TRIES[i][0] ? TRIES[i] : "(bare)");
        SerialLogger::Log(" ---\r\n");
        Process* p = ElfLoader::LoadELF64FromVFS("/usr/bin/busybox", "busybox");
        if (!p) { SerialLogger::Log("[bbtest] LOAD FAILED\r\n"); continue; }
        const char* av0[] = { "prog", nullptr };
        const char* av1[] = { "prog", TRIES[i], nullptr };
        const char** av = TRIES[i][0] ? av1 : av0;
        const char* envp[] = { "PATH=/usr/bin", "HOME=/root", nullptr };
        int rc = Userspace::RunProcessWithArgs(p, av, envp);
        SerialLogger::Log("[bbtest] rc=");
        SerialLogger::LogDec(rc);
        SerialLogger::Log("\r\n");
    }
    SerialLogger::Log("[bbtest] DONE\r\n");
    for (;;) Scheduler::SleepMs(60000);
}

// headless hypervisor gate (kurono.vmtest=1). there was no non-interactive way
// to exercise the vmm at all - the only trigger was the `alpine` shell command -
// so VMCS/EPT/device setup was untested in every automated boot. creates a
// minimal VM with defaults (no guest image needed: this exercises vmcs + ept +
// SetupDevices, which is where PCIPassthrough/IOMMU init lives) and reports the
// outcome. this is the go/no-go for the KLD passthrough fallback. (satoru)
[[noreturn]] static void vmtest_runner_entry() {
    Scheduler::SleepMs(3000);
    SerialLogger::Log("[vmtest] hypervisor available=");
    SerialLogger::Log(Hypervisor::IsAvailable() ? "yes" : "NO");
    SerialLogger::Log("\r\n");
    VMConfig cfg;
    cfg.SetDefaults();
    bool ok = Hypervisor::CreateVM(cfg);
    SerialLogger::Log(ok ? "[vmtest] CreateVM OK (vmcs+ept+devices up)\r\n"
                         : "[vmtest] CreateVM FAILED\r\n");
    if (ok) Hypervisor::DestroyVM();
    SerialLogger::Log("[vmtest] DONE\r\n");
    for (;;) Scheduler::SleepMs(60000);
}

// kdf crash-recovery self-test runner kernel-process (kurono.kdf.test). spawned
// after the desktop is up; deliberately faults a kdf driver + verifies the
// kernel survives + the driver restarts, logs PASS/FAIL, optionally powers off.
// (satoru)
[[noreturn]] static void kdf_selftest_entry() {
    // let the desktop + kinit settle so the crash monitor is ticking. (satoru)
    Scheduler::SleepMs(3000);
    KDF::RunCrashRecoveryTest();
    for (;;) Scheduler::SleepMs(60000);
}

bool g_envwatch = false;     // kurono.envwatch=1, shell.cpp (satoru)
bool g_pflooptest = false;   // kurono.pflooptest=1: [pfloop] self-test, linux_syscall.cpp (satoru)
extern "C" void kernel_main(uint64_t magic, uint64_t mb_addr) {
    SerialLogger::Init();
    SerialLogger::Log("Kurono OS Starting...\r\n");
    RuntimeLog::LogBoot("kernel_main entered");

    // if booted via mb2, convert the tagged info to mb1 format so all
    // existing kernel code works unchanged.
    if (magic == 0x36d76289) {
        SerialLogger::Log("Multiboot2 detected - converting to MB1 compat\r\n");
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
    // kurono.bootdebug=1: the early green bar + debug text below and the dark
    // blue test fill after Graphics::Init. default off, so the first thing on
    // screen after the firmware is the boot animation. (satoru)
    const bool boot_debug = magic == 0x2BADB002 && (mbi_early->flags & (1u << 2)) && mbi_early->cmdline != 0 &&
                            boot_has_token((const char*)(uintptr_t)mbi_early->cmdline, "kurono.bootdebug=1");
    if (!boot_debug) SerialLogger::Log("Early FB: debug screen off (kurono.bootdebug=1 shows it)\r\n");
    bool has_early_fb = false;
    if (boot_debug && magic == 0x2BADB002 && (mbi_early->flags & (1u << 12)) && mbi_early->framebuffer_addr != 0
        && mbi_early->framebuffer_width > 0 && mbi_early->framebuffer_height > 0
        && mbi_early->framebuffer_type != 2) {
        uint64_t fb = mbi_early->framebuffer_addr;
        uint32_t pitch = mbi_early->framebuffer_pitch;
        uint32_t fw = mbi_early->framebuffer_width;
        uint32_t fh = mbi_early->framebuffer_height;
        uint8_t  fbpp = mbi_early->framebuffer_bpp;

        // draw a bright green bar at top of screen (32px) - proves we're alive
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
    // kurono.mode=recovery (the EFI loader decides, see system/bootcfg.h):
    // the cli boot path, then Kurono Recovery instead of the cli prompt. (satoru)
    bool boot_recovery = false;
    int  boot_recovery_reason = 0;
    // kurono.safe=1 or recovery: no KLD linux drivers this boot. (satoru)
    bool boot_safe = false;
    static char boot_slot[32];
    boot_slot[0] = 0;
    bool boot_cli_poweroff = false;
    // autologin: skip the lockscreen/first-boot wizard and drop straight to
    // the desktop with a default user. used by the bootable iso "straight to
    // desktop" entry so a fresh vm boots to a usable desktop with no typing. (satoru)
    bool boot_autologin = false;
    bool boot_media_seed = false;   // kurono.media.seed=1: copy the on-demand demo clips into /home/user/Videos (satoru)
    // ffmpeg on-device smoke test, parsed early into a bool: the grub cmdline
    // string lives in low memory that the (now large) kernel image/heap can
    // clobber before the test runs deep in boot, so we must latch it now. (satoru)
    bool boot_ffmpeg_test = false;
    bool boot_dyntest_test = false;   // run the dynamic-pie ld-kurono smoke test (satoru)
    bool boot_logcheck = false;       // dump /kurono/var/log contents to verify logging (satoru)
    // headless two-boot kfs persistence test (kurono.kfstest): boot 1 writes user
    // data + times SaveTree then powers off; boot 2 verifies it restored from the
    // KFS volume + times LoadTree, logs PASS/FAIL, then powers off. (satoru)
    bool boot_kfstest = false;
    // headless storage benchmark (kurono.kfsbench): formats the nvme DATA disk to
    // a fresh KFS volume + measures seq/rand throughput, snapshot save/restore,
    // and per-op nvme command counts; logs everything to serial, then powers off.
    // destructive to the data disk, so gate it behind the explicit token. (satoru)
    bool boot_kfsbench = false;
    // ksa isolation self-test: spawn the isolated context, prove the main os has
    // no page-table mapping into it, exercise the read-only verdict channel, then
    // tear down. latched early like the other smoke-test flags. (satoru)
    bool boot_ksa_test = false;
    // interactive ksa prompt demo: once the desktop is up, draw the REAL
    // on-screen ksa modal and let keyboard/mouse (or synthetic input) drive the
    // verdict. for headless screendump + verdict-flow verification of the
    // prompt. gated by kurono.ksa.prompt[=cred|nocred]. (satoru)
    bool boot_ksa_prompt = false;
    bool boot_ksa_prompt_cred = true;   // default: collect a credential (satoru)
    // kcl language self-test: run the kcl interpreter test suite headless at
    // boot and log PASS/FAIL per test to serial. gated by kurono.kcltest. (satoru)
    bool boot_kcl_test = false;
    // 802.11i wpa2 crypto self-test: assert the wifi security core (pbkdf2/prf/
    // ccmp/...) against published vectors headless at boot. gated by
    // kurono.wifitest. proves the security core even with no radio. (satoru)
    bool boot_wifi_test = false;
    // platform self-test (kurono.platformtest=1): crypto vectors, password
    // hashing and the other platform modules' offline checks. (satoru)
    bool boot_platform_test = false;
    // kj language self-test: run the kj (kurono javascript) suite + kss-binding
    // checks headless at boot. gated by kurono.kjtest. (satoru)
    bool boot_kj_test = false;
    // kj-scripted animation demo: once the desktop is up, run the shipped
    // accent-animation .kj script so a real on-screen animation is driven through
    // the kj host bindings. gated by kurono.kjdemo. (satoru)
    bool boot_kj_demo = false;
    // kinit self-test gate (kurono.kinit.test): register the bounded kinit test
    // services (memory-limit hog, watchdog, socket-activated, parallel, cycle) and,
    // once the desktop is up, run the self-tests + log PASS/FAIL + measured numbers
    // to serial. kurono.kinit.poweroff=1 powers off after they finish (headless
    // ci). (satoru)
    bool boot_kinit_test = false;
    bool boot_kinit_poweroff = false;
    // kdf crash-recovery self-test gate (kurono.kdf.test) + poweroff-after. (satoru)
    bool boot_kdf_test = false;
    bool boot_kdf_poweroff = false;
    // linux syscall-abi self-test gate (kurono.klstest): drive a representative
    // syscall from each tier through LinuxSyscall::Dispatch and log PASS/FAIL per
    // check headless at boot. proves the tier 1-11 build-out is wired + sane
    // without a GUI run. kurono.klstest.poweroff=1 powers off after. (satoru)
    bool boot_kls_test = false;
    bool boot_kls_poweroff = false;
    // kmemx (memory compression engine) self-test gate (kurono.kmemxtest):
    // run the lz4 + pool + dedup self-tests headless at boot, log PASS/FAIL to
    // serial, optionally power off. proves the engine without the gui. (satoru)
    bool boot_kmemx_test = false;
    bool boot_kmemx_poweroff = false;
    // raw 1:1 mouse (no accel) - accessibility + deterministic synthetic input. (satoru)
    bool boot_mouse_raw = false;
    // setup mode: run the graphical installer / first-setup wizard instead of
    // dropping straight to the desktop. gated by the "Kurono Setup" grub entry
    // (kurono.setup=1). the main entry stays autologin -> desktop. (satoru)
    bool boot_setup = false;
    bool boot_kmemx_off = false;  // kurono.kmemx=0 a/b kill switch (task 20) (satoru)
    bool boot_kmx_idle_off = false; // kurono.kmxidle=0 disables the kinit-driven idle scan (satoru)
    bool boot_kmx_active_off = false; // kurono.kmxactive=0 restricts scanning to fully idle spaces (satoru)
    bool boot_kmx_all = false;        // kurono.kmxall=1 compresses non-zero pages too (satoru)
    bool boot_fftrace = false;    // kurono.fftrace=1 re-enables the ff debug probes (task 29) (satoru)
    bool boot_pincpu_off = false; // kurono.pincpu=0 disables the default pinning (task 21) (satoru)
    bool boot_madvlazy = false;   // kurono.madvlazy=1 lazy-madvise + thread spread a/b (task 23f) (satoru)
    bool boot_fastrun = true;     // short autorun grace is DEFAULT; kurono.fastrun=0 restores the old 4s/2s (satoru)
    int  boot_fpsbench = 0;       // kurono.fpsbench=1 uncapped / =2 capped-but-forced (satoru)
    uint32_t boot_pacetest = 0;   // kurono.pacetest=SECS synthetic slow frames, qemu rig (satoru)
    bool boot_vgsync = false;     // kurono.vgsync=1 restores the BLOCKING virtio present (a/b) (satoru)
    bool boot_irqlat = false;     // kurono.irqlat=1 runs the userspace wake-latency probe (satoru)
    bool boot_spawntest = false;  // kurono.spawntest=1 runs the posix_spawn static-elf64 repro (satoru)
    bool boot_vmtest = false;     // kurono.vmtest=1 headless hypervisor vmcs/ept gate (satoru)
    bool boot_kld = false;
    bool boot_kldapitest = false;   // kurono.kld=apitest shim self-test (satoru)
    bool boot_kldi2chid = false;    // kurono.kld=i2chid real touchpad family (satoru)
    bool boot_kldiwlwifi = false;   // kurono.kld=iwlwifi real BE200 (satoru)
    bool boot_kldbench = false;   // kurono.kld.bench=1 network a/b instrument (satoru)
    bool boot_kldacpi = false;    // kurono.kld.acpi=1 acpica container (satoru)        // kurono.kld=1 runs the KLD driver container (satoru)
    bool boot_kldauto = false;    // kurono.kld=auto match pci devices to driver id tables (satoru)
    bool boot_kldmsixcheck = false; // kurono.kld.msixcheck=1 kernel msi-x table page self check (satoru)
    bool boot_kldstats = false;   // kurono.kld.stats=1 print ioapic access counters once after boot (satoru)
    bool boot_uatest = false;     // kurono.uatest=1 user-pointer validation gate (satoru)
    bool boot_findtest = false;   // kurono.findtest=1 parallel find/Glob/Grep tree walk (satoru)
    bool boot_kentest = false;   // kurono.kentest=1 privilege-gate regression (satoru)
    bool boot_kshtest = false;    // kurono.kshtest=1 /dev/kshell rescue-channel gate (satoru)
    bool boot_memtest = false;    // kurono.memtest=1 ring-3 fill + verify of most of free ram (satoru)
    bool boot_churn = false;      // kurono.churn=1 fork/exec/mmap churn for the leak census (satoru)
    bool boot_mmapchurn = false;  // kurono.mmapchurn=<s>: 10000 mmap/munmap pairs after s (satoru)
    bool boot_kldperm = false;    // kurono.kldperm=<s>: one broker op from an ordinary process (satoru)
    bool boot_elfreload = false;  // kurono.elfreloadtest=1 fault-kill then reload a static image (satoru)
    uint32_t boot_census_s = 0;   // kurono.memcensus=1: report period (kurono.memcensus.s=N, default 30) (satoru)
    uint32_t boot_taskmem_log_s = 0;   // kurono.taskmem.log=N: [taskmem] report every N s (satoru)
    bool boot_dsai = false;       // kurono.dsai=1 ai recovery client end-to-end gate (satoru)
    bool boot_rtest = false;      // kurono.rtest=1 recovery toolbox gate (satoru)
    bool boot_claude = false;     // kurono.claude=1 claude code headless gate (satoru)
    bool boot_audiotest = false;  // kurono.audiotest=1 hd audio tone + restart gate (satoru)
    bool boot_uactest = false;    // kurono.uactest=1 usb audio descriptor parser self test (satoru)
    bool boot_usbtest = false;    // kurono.usbtest=1 usb hid report parser self test (satoru)
    bool boot_usbmsdtest = false; // kurono.usbmsdtest=1 usb mass storage end to end self test (satoru)
    bool boot_audiotest2 = false; // kurono.audiotest=2 streaming-client pump gate (satoru)
    bool boot_usbaudiotest = false; // kurono.usbaudiotest=1 usb audio soak + recovery gate (satoru)
    bool boot_nettest = false;    // kurono.nettest=1 nic traffic + `nic restart` gate (satoru)
    bool boot_bbtest = false;     // kurono.bbtest=1 third-party linux binary gate (satoru)
    int  boot_setup_screen = 0;   // optional start screen for the setup wizard (satoru)
    char boot_cli_run[160];
    boot_cli_run[0] = 0;
    static char boot_gui_run[256];
    boot_gui_run[0] = 0;
    const char* boot_cmdline = nullptr;
    // firmware IA32_TSC_ADJUST sanitize on the bsp, before anything keeps a tsc
    // base (CPUDetect calibrates and Timer::GetRealMs64 latches its base much
    // later). the laptop's firmware leaves a different value per cpu, which made
    // rdtsc disagree by 0.6 to 0.9 s between the bsp and the aps. on by default
    // (a no-op when the values are already 0, as in every vm);
    // kurono.tscadjust=0 keeps the firmware values, kurono.tscadjust.test=1
    // injects the laptop's value first (qemu). (satoru)
    {
        const char* tc = (mbi_early && (mbi_early->flags & (1u << 2)) && mbi_early->cmdline != 0)
                             ? (const char*)(uintptr_t)mbi_early->cmdline : nullptr;
        SMP::TscAdjustBoot(!boot_has_token(tc, "kurono.tscadjust=0"),
                           boot_has_token(tc, "kurono.tscadjust.test=1"));
    }
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
        if (boot_has_token(boot_cmdline, "kurono.mode=recovery")) {
            boot_recovery = true;
            boot_cli = true;
            boot_safe = true;
            for (int r = 1; r <= 3; r++) {
                char tok[32] = "kurono.recovery.reason=0";
                tok[23] = (char)('0' + r);
                if (boot_has_token(boot_cmdline, tok)) boot_recovery_reason = r;
            }
        }
        if (boot_has_token(boot_cmdline, "kurono.safe=1")) boot_safe = true;
        boot_get_value(boot_cmdline, "kurono.boot.slot", boot_slot, (int)sizeof(boot_slot));
        if (boot_has_token(boot_cmdline, "kurono.cli.poweroff=1")) {
            boot_cli_poweroff = true;
        }
        // autologin: bypass lockscreen + first-boot wizard, land on desktop. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.autologin=1") || boot_has_token(boot_cmdline, "kurono.autologin")) {
            boot_autologin = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.media.seed=1")) boot_media_seed = true;
        // full-session serial capture only on request (4 mb instead of 256 kb). (satoru)
        if (boot_has_token(boot_cmdline, "kurono.serlog=big")) SerialLogger::SetBigCapture(true);
        // kmemx a/b kill switch - parsed HERE with the other early flags: the
        // multiboot cmdline string can be reclaimed/clobbered by the allocator
        // inits, so a late boot_has_token on it silently fails (task 20). (satoru)
        if (boot_has_token(boot_cmdline, "kurono.kmemx=0")) {
            boot_kmemx_off = true;
        }
        // kurono.kmxidle=0: keep the engine but turn off kinit's background
        // scan pass (a/b for the idle-compression path). (satoru)
        if (boot_has_token(boot_cmdline, "kurono.kmxidle=0")) {
            boot_kmx_idle_off = true;
        }
        // default OFF now (kurono.kmxidle=1 turns it back on): with the idle
        // scan compressing claude's zero pages, Claude Code's threads found
        // small integers in pointer fields and segfaulted (QEMU, fixed vfork
        // kernel: 4 of 8 sessions; with kmxidle=0 or kmemx=0: 0 of 8), and the
        // laptop's fs.watch thread wrote to a not-present heap page with KMemX
        // holding 2110 pages. the mechanism inside KMemX is not found yet. (satoru)
        if (!boot_has_token(boot_cmdline, "kurono.kmxidle=1")) boot_kmx_idle_off = true;
        if (boot_has_token(boot_cmdline, "kurono.kmxactive=0")) {
            boot_kmx_active_off = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.kmxall=1") || boot_has_token(boot_cmdline, "kurono.kmxall")) {
            boot_kmx_all = true;
        }
        // startup latency: the autorun grace (was 4s here, 2s in the gui
        // process) is dead time on the boot-to-paint clock. measured a/b over
        // 6 boots each: launch 8.4s -> 6.9s, boot-to-paint 22.2s -> 20.6s,
        // paint rate unchanged at 3/6 and launch-to-paint unchanged, so the
        // short grace is the default. kurono.fastrun=0 restores the old
        // timings if a slow-desktop regression ever shows up. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.fastrun=0")) {
            boot_fastrun = false;
        }
        // kurono.donate=1 enables the starvation valve's slice donation
        // (Scheduler::DonateToKernelProcs). OFF by default: measured n=31 with
        // vs n=32 without, it is statistically identical on paint/stall/crash
        // while adding a whole user-task-through-the-kernel-proc-switcher path.
        // The stall fix is the trigger correction, not this. See the block
        // comment on g_donate_enabled in scheduler.cpp. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.donate=1")) {
            Scheduler::SetDonateEnabled(true);
        }
        // kurono.fpsbench=1: forced-render framerate benchmark (see
        // KernelProcesses::SetFpsBench). measures the pipeline, not the
        // damage gate. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.fpsbench=1")) {
            boot_fpsbench = 1;
        }
        if (boot_has_token(boot_cmdline, "kurono.fpsbench=2")) {
            boot_fpsbench = 2;
        }
        // =3: like =2 but every present transfers the whole screen, i.e. the
        // full-screen-content case the damage-aware path otherwise hides. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.fpsbench=3")) {
            boot_fpsbench = 3;
        }
        // async present is the DEFAULT (24% lower kernel scheduling latency,
        // see virtio_gpu.cpp). kurono.vgsync=1 restores the blocking one. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.vgsync=1")) {
            boot_vgsync = true;
        }
        // kurono.smpscale=N: run the cpu scaling probe at boot (N max threads, 1 = auto). (satoru)
        {
            char v[8] = {0};
            if (boot_get_value(boot_cmdline, "kurono.smpscale", v, (int)sizeof(v))) {
                int n = 0;
                for (int i = 0; v[i] >= '0' && v[i] <= '9'; i++) n = n * 10 + (v[i] - '0');
                g_smpscale_threads = n > 0 ? n : 1;
            }
        }
        // kurono.irqlat=1: run the ring-3 wake-latency probe at boot. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.irqlat=1")) {
            boot_irqlat = true;
        }
        // kurono.irqlat.busy=N [.secs=S] [.mode=M]: the contention reproducer. (satoru)
        {
            char v[8] = {0};
            if (boot_get_value(boot_cmdline, "kurono.irqlat.busy", v, (int)sizeof(v))) {
                int n = 0;
                for (int i = 0; v[i] >= '0' && v[i] <= '9'; i++) n = n * 10 + (v[i] - '0');
                g_irqlat_busy = n;
                boot_irqlat = true;
            }
            if (boot_get_value(boot_cmdline, "kurono.irqlat.secs", v, (int)sizeof(v))) {
                int n = 0;
                for (int i = 0; v[i] >= '0' && v[i] <= '9'; i++) n = n * 10 + (v[i] - '0');
                if (n > 0) g_irqlat_secs = n;
            }
            if (boot_get_value(boot_cmdline, "kurono.irqlat.mode", v, (int)sizeof(v))) {
                int n = 0;
                for (int i = 0; v[i] >= '0' && v[i] <= '9'; i++) n = n * 10 + (v[i] - '0');
                g_irqlat_mode = n;
            }
        }
        // kurono.wakelat=1: permanent wake-to-run accounting (stage f0b). (satoru)
        if (boot_has_token(boot_cmdline, "kurono.wakelat=1")) {
            Scheduler::SetWakeLat(true);
        }
        // kurono.vrblock=1: charge the real stint, including a block (default off). (satoru)
        if (boot_has_token(boot_cmdline, "kurono.vrblock=1")) {
            Scheduler::SetVrBlock(true);
        }
        // the non-connect parks are gated OFF by default in this job. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.lockpark=1")) { extern bool g_lock_park; g_lock_park = true; }
        if (boot_has_token(boot_cmdline, "kurono.ptypark=1"))  { extern bool g_pty_park;  g_pty_park  = true; }
        if (boot_has_token(boot_cmdline, "kurono.jobpark=1"))  { extern bool g_job_park;  g_job_park  = true; }
        if (boot_has_token(boot_cmdline, "kurono.sigsus=1"))   { extern bool g_sigsus_park; g_sigsus_park = true; }
        // kurono.claimage=1: the claim picker's ready_since override (default off). (satoru)
        if (boot_has_token(boot_cmdline, "kurono.claimage=1")) {
            Scheduler::SetClaimAge(true);
        }
        // kurono.spawntest=1: run the posix_spawn static-elf64 repro at boot. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.spawntest=1")) {
            boot_spawntest = true;
        }
        // kurono.vmtest=1: headless hypervisor gate. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.vmtest=1")) {
            boot_vmtest = true;
        }
        // kurono.kld=1: run the KLD linux-driver container. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.kld=apitest")) {
            boot_kldapitest = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.kld.yieldfair=1")) g_kldyieldfair = true;
        if (boot_has_token(boot_cmdline, "kurono.kld.stats=1")) g_kldstats = true;
        KldBroker::SetStats(g_kldstats);
        if (boot_has_token(boot_cmdline, "kurono.kld=i2chid")) {
            boot_kldi2chid = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.kld.i2chid.hostile=1"))
            g_kld_i2chid_hostile = true;
        boot_get_value(boot_cmdline, "kurono.kld.family",
                       g_kld_family_override, (int)sizeof(g_kld_family_override));
        {
            char lp[8] = { 0 };
            boot_get_value(boot_cmdline, "kurono.kld.loop", lp, (int)sizeof(lp));
            int n = 0;
            for (int i = 0; lp[i] >= '0' && lp[i] <= '9'; i++) n = n * 10 + (lp[i] - '0');
            g_kld_loop = n > 100 ? 100 : n;   // test only, off by default (satoru)
        }
        KldInput::SetVerbose(boot_has_token(boot_cmdline, "kurono.kld.inputlog=1"));
        if (boot_has_token(boot_cmdline, "kurono.kld=iwlwifi")) {
            boot_kldiwlwifi = true;
            WiFi::SetKldOwned(true);
            if (boot_has_token(boot_cmdline, "kurono.kldwifi=scan")) g_kldwifi_scan = true;
            if (boot_has_token(boot_cmdline, "kurono.kldbench.tls=1"))
                g_kldbench_tls = true;
            if (boot_has_token(boot_cmdline, "kurono.kldwifi.band=2.4"))
                g_kldwifi_band24 = true;
            if (boot_has_token(boot_cmdline, "kurono.kldwifi.soak=1"))
                g_kldwifi_soak = true;
            if (boot_has_token(boot_cmdline, "kurono.kldwifi.reconnect=1"))
                g_kldwifi_reconnect = true;
            if (boot_has_token(boot_cmdline, "kurono.kldwifi=connect")) {
                g_kldwifi_connect = true;
                KldNet::SetWanted(true);   // wlan0 via the same KLD bridge as eth0 (satoru)
            }
        }
        if (boot_has_token(boot_cmdline, "kurono.kld=1") ||
            boot_has_token(boot_cmdline, "kurono.kld=e1000e")) {
            boot_kld = true;
            if (boot_has_token(boot_cmdline, "kurono.kldbench.tls=1"))
                g_kldbench_tls = true;
            // eth0 is served by the container's linux driver unless
            // kurono.kld.net=0 keeps the native driver. (satoru)
            if (!boot_has_token(boot_cmdline, "kurono.kld.net=0")) KldNet::SetWanted(true);
            if (boot_has_token(boot_cmdline, "kurono.kld.irqwake=0")) KldBroker::SetIrqWake(false);
        }
        // kurono.r3audio=1: the pulse server + mixing run in the ring-3 audiod;
        // the in-kernel PulseServer stays off. default off. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.r3audio=1")) {
            R3Audio::SetEnabled(true);
        }
        if (boot_has_token(boot_cmdline, "kurono.r3audiotest=1")) g_r3audiotest = true;
        if (boot_has_token(boot_cmdline, "kurono.kld.bench=1")) boot_kldbench = true;   // (satoru)
        if (boot_has_token(boot_cmdline, "kurono.kldbench.tls=1")) g_kldbench_tls = true; // native TLS A0 (satoru)
        {
            const char* bp = boot_value(boot_cmdline, "kurono.kldbench.port=");
            if (bp && bp[0] >= '1' && bp[0] <= '9') {
                int v = 0;
                for (int i = 0; i < 5 && bp[i] >= '0' && bp[i] <= '9'; i++)
                    v = v * 10 + (bp[i] - '0');
                if (v > 0 && v < 65536) g_kldbench_port = v;
            }
        }
        if (boot_has_token(boot_cmdline, "kurono.kld.acpi=1")) boot_kldacpi = true;     // (satoru)
        // kurono.kld=auto: pick the container from the driver's own pci id table.
        // parsed here so the native nic drivers can stand down before
        // Network::Init runs. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.kld=auto") && !boot_safe) {   // safe start: no kld (satoru)
            boot_kldauto = true;
            KldAuto::BootInit(boot_cmdline);
        }
        // kurono.kld.display=pattern|mirror|extend|primary: the display bridge.
        // off by default; BootInit leaves MODE_OFF without the flag. (satoru)
        KldDisplay::BootInit(boot_cmdline);
        DriDev::BootInit(boot_cmdline);   // kurono.kld.dri=1 (satoru)
        // kurono.kld.msixcheck=1: kernel self check of the msi-x table rule.
        if (boot_has_token(boot_cmdline, "kurono.kld.msixcheck=1")) boot_kldmsixcheck = true;
        // vt-d flags apply to every kld boot, including the legacy bench path.
        KldAuto::ParseIommuFlags(boot_cmdline);
        // kurono.wl.childalpha=1: per-pixel alpha for wayland child subsurfaces (libdecor) (satoru)
        if (boot_has_token(boot_cmdline, "kurono.wl.childalpha=1")) WaylandServer::SetChildAlpha(true);
        if (boot_has_token(boot_cmdline, "kurono.wl.scdirect=1")) WaylandServer::SetScissorDirectBlit(true);   // (satoru)
        if (boot_has_token(boot_cmdline, "kurono.gfx.dirtymerge=0")) Graphics::SetDirtyMerge(false);   // old full-screen collapse (satoru)
        if (boot_has_token(boot_cmdline, "kurono.flip.emptyskip=0")) Graphics::SetFlipEmptySkip(false);   // undamaged frames flip again (satoru)
        if (boot_has_token(boot_cmdline, "kurono.gfx.selfcarry=0")) Graphics::SetScissorSelfCarry(false);   // (satoru)
        if (boot_has_token(boot_cmdline, "kurono.wl.deco=1")) WaylandServer::SetDecoration(true);
        if (boot_has_token(boot_cmdline, "kurono.wl.fullsize=1")) WaylandServer::SetFullsize(true);
        // wl shm pools are unpinned when their client goes, default on now: with it off
        // every app run left ~1.8 mb of memfd pinned for good (memcensus: 23 slots,
        // 41 mb after 24 app runs; 0 with it on). kurono.wl.poolfree=0 = old pinning. (satoru)
        { bool pf = !boot_has_token(boot_cmdline, "kurono.wl.poolfree=0");
          WaylandServer::SetPoolFree(pf); LinuxSyscall::SetPoolFree(pf); }
        // kurono.vmatrace=1: L1 page-table growth trace (map origin + census). (satoru)
        if (boot_has_token(boot_cmdline, "kurono.vmatrace=1")) {
            KernelVMM::SetVmTrace(true);
            LinuxSyscall::SetMapTrace(true);
        }
        // kurono.mmap.reuse=1: L2 freed-VA hole reuse for auto mmap. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.mmap.reuse=1")) LinuxSyscall::SetMmapReuse(true);
        // TEST-ONLY: cache the hole AND skip the munmap shootdown; T2 must fail.
        if (boot_has_token(boot_cmdline, "kurono.mmap.reuse.noflush=1")) {
            LinuxSyscall::SetMmapNoFlush(true);
            LinuxSyscall::SetMmapReuse(true);
        }
        // L3: one bump pointer per address space (default on); =0 reverts; ctidbelt=0 and audit=1 are measurement flags. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.mmap.groupbump=0")) LinuxSyscall::SetGroupBump(false);
        if (boot_has_token(boot_cmdline, "kurono.mmap.ctidbelt=0")) LinuxSyscall::SetCtidBelt(false);
        if (boot_has_token(boot_cmdline, "kurono.mmap.audit=1")) LinuxSyscall::SetMmapAudit(true);
        // TEST-ONLY: reuse without the clear_child_tid belt; T4 must fail. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.mmap.reuse.noctid=1")) {
            LinuxSyscall::SetMmapNoCtid(true);
            LinuxSyscall::SetMmapReuse(true);
        }
        if (boot_has_token(boot_cmdline, "kurono.kld.stats=1")) boot_kldstats = true;
        if (boot_has_token(boot_cmdline, "kurono.kld.nomsi=1")) KldBroker::SetNoMsi(true);
        KldIoApic::SetAudit(boot_has_token(boot_cmdline, "kurono.kld.ioapic.audit=1"));
        {
            char la[24] = { 0 };
            boot_get_value(boot_cmdline, "kurono.kld.line.allow", la, (int)sizeof(la));
            if (la[0]) KldBroker::SetLineAllow(la);
        }
        // kurono.kld.acpi.allow=...: the owner's writable acpi registers (the
        // kernel's allowlist starts empty). (satoru)
        {
            char acpi_allow[192];
            boot_get_value(boot_cmdline, "kurono.kld.acpi.allow",
                           acpi_allow, (int)sizeof(acpi_allow));
            if (acpi_allow[0]) KldBroker::SetAcpiAllow(acpi_allow);
        }
        // kurono.sctrace=1: log first use + first failure of every syscall (satoru)
        if (boot_has_token(boot_cmdline, "kurono.sctrace=1")) {
            extern bool g_sctrace;
            g_sctrace = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.tmkill=1")) { extern bool g_tmkill_test; g_tmkill_test = true; }   // task manager kill rehearsal (satoru)
        if (boot_has_token(boot_cmdline, "kurono.kbtest=1")) { extern bool g_kbtest; g_kbtest = true; }   // terminal [kbt] serial mirror for kbrun.sh (satoru)
        if (boot_has_token(boot_cmdline, "kurono.ntp=0")) Sntp::g_enabled = false;   // no network time sync (satoru)
        if (boot_has_token(boot_cmdline, "kurono.nocloexec=1")) { extern bool g_nocloexec; g_nocloexec = true; }   // escape hatch: exec keeps every fd (satoru)
        // kurono.connpark=0: the old ring-0 blocking-connect busy-wait (a/b arm). (satoru)
        if (boot_has_token(boot_cmdline, "kurono.connpark=0")) { extern bool g_conn_park; g_conn_park = false; }
        if (boot_has_token(boot_cmdline, "kurono.ffnolocal=1")) { extern bool g_ff_nolocal; g_ff_nolocal = true; }   // skip FIREFOX.TAR on the partition (satoru)
        {   // kurono.hz=N: fixed compositor refresh cap (60..360), overrides the
            // Settings > Display choice. software pacing only. (satoru)
            char hv[12];
            if (boot_get_value(boot_cmdline, "kurono.hz", hv, sizeof(hv))) {
                uint32_t n = 0;
                for (int k = 0; hv[k] >= '0' && hv[k] <= '9' && k < 6; k++) n = n * 10 + (uint32_t)(hv[k] - '0');
                g_boot_hz = n;
            }
        }
        {   // kurono.panelhz=N: qemu/test override of the lit output's rate, so
            // the pacer ceiling and ladder can be exercised without a real
            // panel. no hardware is touched. (satoru)
            char pv[12];
            if (boot_get_value(boot_cmdline, "kurono.panelhz", pv, sizeof(pv))) {
                uint32_t n = 0;
                for (int k = 0; pv[k] >= '0' && pv[k] <= '9' && k < 6; k++) n = n * 10 + (uint32_t)(pv[k] - '0');
                g_boot_panel_hz = n;
            }
            if (boot_get_value(boot_cmdline, "kurono.pacetest", pv, sizeof(pv))) {
                uint32_t n = 0;
                for (int k = 0; pv[k] >= '0' && pv[k] <= '9' && k < 5; k++) n = n * 10 + (uint32_t)(pv[k] - '0');
                boot_pacetest = n;
            }
        }
        {   // test rig: kurono.ffcrash=S and kurono.ffreopen=N (satoru)
            char tv[12]; int n;
            if (boot_get_value(boot_cmdline, "kurono.ffcrash", tv, sizeof(tv))) {
                n = 0; for (int k = 0; tv[k] >= '0' && tv[k] <= '9'; k++) n = n * 10 + (tv[k] - '0');
                g_ffcrash_secs = (uint32_t)n;
            }
            if (boot_get_value(boot_cmdline, "kurono.usnap", tv, sizeof(tv))) {   // all-thread snapshots from the disklog worker (satoru)
                extern volatile uint32_t g_usnap_secs;
                n = 0; for (int k = 0; tv[k] >= '0' && tv[k] <= '9'; k++) n = n * 10 + (tv[k] - '0');
                g_usnap_secs = (uint32_t)n;
            }
            if (boot_get_value(boot_cmdline, "kurono.ffsnap", tv, sizeof(tv))) {   // census snapshots (satoru)
                extern volatile uint32_t g_ffsnap_secs;
                n = 0; for (int k = 0; tv[k] >= '0' && tv[k] <= '9'; k++) n = n * 10 + (tv[k] - '0');
                g_ffsnap_secs = (uint32_t)n;
            }
            if (boot_get_value(boot_cmdline, "kurono.ffreopen", tv, sizeof(tv))) {
                n = 0; for (int k = 0; tv[k] >= '0' && tv[k] <= '9'; k++) n = n * 10 + (tv[k] - '0');
                g_ffreopen_left = n;
            }
        }
        {   // kurono.ffmirror=a.b.c.d: firefox download origin override for test mirrors (satoru)
            char fm[24];
            if (boot_get_value(boot_cmdline, "kurono.ffmirror", fm, sizeof(fm))) {
                uint32_t ip = 0, cur = 0; int parts = 0;
                for (int i = 0; ; i++) {
                    if (fm[i] >= '0' && fm[i] <= '9') cur = cur * 10 + (uint32_t)(fm[i] - '0');
                    else { ip = (ip << 8) | (cur & 255); parts++; cur = 0; if (!fm[i]) break; }
                }
                if (parts == 4) { extern uint32_t g_ff_origin_override; g_ff_origin_override = ip; }
            }
        }
        if (boot_has_token(boot_cmdline, "kurono.cc.noasfree=1")) { extern bool g_cc_noasfree; g_cc_noasfree = true; }
        if (boot_has_token(boot_cmdline, "kurono.cc.nokvfs=1")) { extern bool g_cc_nokvfs_destroy; g_cc_nokvfs_destroy = true; }
        if (boot_has_token(boot_cmdline, "kurono.cc.nopidfd=1")) { extern bool g_cc_nopidfd; g_cc_nopidfd = true; }
        if (boot_has_token(boot_cmdline, "kurono.sctrace.fail=1")) {   // failing-return trace (satoru)
            extern bool g_sctrace_fail; g_sctrace_fail = true;
            extern uint64_t g_sctrace_after_ms; extern int g_sctrace_minpid;
            char mv[16];
            if (boot_get_value(boot_cmdline, "kurono.sctrace.ms", mv, sizeof(mv))) { uint64_t v = 0; for (int i = 0; mv[i] >= '0' && mv[i] <= '9'; i++) v = v * 10 + (mv[i] - '0'); g_sctrace_after_ms = v; }
            if (boot_get_value(boot_cmdline, "kurono.sctrace.pid", mv, sizeof(mv))) { int v = 0; for (int i = 0; mv[i] >= '0' && mv[i] <= '9'; i++) v = v * 10 + (mv[i] - '0'); g_sctrace_minpid = v; }
        }
        if (boot_has_token(boot_cmdline, "kurono.sctrace=2")) {
            extern bool g_sctrace, g_sctrace_all;
            g_sctrace = true; g_sctrace_all = true;
            extern int g_sctrace_minpid;
            char mp[16];
            if (boot_get_value(boot_cmdline, "kurono.sctrace.pid", mp, sizeof(mp))) {
                int v = 0; for (int i = 0; mp[i] >= '0' && mp[i] <= '9'; i++) v = v * 10 + (mp[i] - '0');
                g_sctrace_minpid = v;
            }
            extern uint64_t g_sctrace_after_ms;
            if (boot_get_value(boot_cmdline, "kurono.sctrace.ms", mp, sizeof(mp))) {
                uint64_t v = 0; for (int i = 0; mp[i] >= '0' && mp[i] <= '9'; i++) v = v * 10 + (uint64_t)(mp[i] - '0');
                g_sctrace_after_ms = v;
            }
        }
        // kurono.uatest=1: user-pointer validation regression gate. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.uatest=1")) {
            boot_uatest = true;
        }
        // kurono.findtest=1: parallel getdents/stat/openat/readlink walk. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.findtest=1") || boot_has_token(boot_cmdline, "kurono.findtest")) {
            boot_findtest = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.kentest=1")) {
            boot_kentest = true;
        }
        // kurono.churn=1 / kurono.memcensus=1: leak census workload + reports. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.churn=1")) {
            boot_churn = true;
            boot_get_value(boot_cmdline, "kurono.churn.rounds", g_churn_rounds, (int)sizeof(g_churn_rounds));
        }
        {
            char mb[12] = {0};
            if (boot_get_value(boot_cmdline, "kurono.mmapchurn", mb, (int)sizeof(mb))) {
                int v = 0;
                for (int i = 0; mb[i] >= '0' && mb[i] <= '9'; i++) v = v * 10 + (mb[i] - '0');
                if (v >= 0) { g_mmapchurn_delay_s = v; boot_mmapchurn = true; }
            }
        }
        {
            char kb[12] = {0};
            if (boot_get_value(boot_cmdline, "kurono.kldperm", kb, (int)sizeof(kb))) {
                int v = 0;
                for (int i = 0; kb[i] >= '0' && kb[i] <= '9'; i++) v = v * 10 + (kb[i] - '0');
                if (v >= 0) { g_kldperm_delay_s = v; boot_kldperm = true; }
            }
        }
        if (boot_has_token(boot_cmdline, "kurono.elfreloadtest=1")) boot_elfreload = true;
        // kurono.r3apps=calculator (or 1 / all): kui apps run as ring-3 programs; (satoru)
        // kurono.kui.open=<name>: the test harness opens that kui app once the desktop is up (satoru)
        {
            char r3[96] = {0};
            if (boot_get_value(boot_cmdline, "kurono.r3apps", r3, (int)sizeof(r3))) KuiKernel::SetRing3Apps(r3);
            char ko[128] = {0};
            if (boot_get_value(boot_cmdline, "kurono.kui.open", ko, (int)sizeof(ko))) KuiKernel::SetTestOpen(ko);
        }
        // the installer app: kurono.installer=1 opens it once the desktop is up
        // (the "Kurono Setup" grub entry uses this), and kurono.installer.auto=1
        // drives its screens itself for a headless end-to-end test. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.installer=1")) KuiKernel::SetTestOpen("installer");
        if (boot_has_token(boot_cmdline, "kurono.installer.auto=1")) InstallerService::SetAuto(true);
        // kurono.taskmem=1: per-task kernel heap accounting, off by default (satoru)
        if (boot_has_token(boot_cmdline, "kurono.taskmem=1")) {
            TaskMem::Request();
            if (boot_has_token(boot_cmdline, "kurono.taskmem.bench=1")) TaskMem::RequestBench();
            char ts[8] = {0};
            if (boot_get_value(boot_cmdline, "kurono.taskmem.log", ts, (int)sizeof(ts))) {
                uint32_t v = 0; for (int i = 0; ts[i] >= '0' && ts[i] <= '9'; i++) v = v * 10 + (uint32_t)(ts[i] - '0');
                boot_taskmem_log_s = v;
            }
        }
        if (boot_has_token(boot_cmdline, "kurono.memcensus=1")) {
            char cs[8] = {0};
            boot_census_s = 30;
            if (boot_get_value(boot_cmdline, "kurono.memcensus.s", cs, (int)sizeof(cs))) {
                uint32_t v = 0; for (int i = 0; cs[i] >= '0' && cs[i] <= '9'; i++) v = v * 10 + (uint32_t)(cs[i] - '0');
                if (v >= 1) boot_census_s = v;
            }
            char lm[8] = {0};   // kurono.memcensus.lowmb=N: one extra report below N mb free (0 = off) (satoru)
            if (boot_get_value(boot_cmdline, "kurono.memcensus.lowmb", lm, (int)sizeof(lm))) {
                uint32_t v = 0; for (int i = 0; lm[i] >= '0' && lm[i] <= '9'; i++) v = v * 10 + (uint32_t)(lm[i] - '0');
                MemCensus::SetLowMemFloor(v);
            }
        }
        // kurono.memtest=1: ring-3 memory fill + verify gate. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.memtest=1")) {
            boot_memtest = true;
            boot_get_value(boot_cmdline, "kurono.memtest.pct", g_memtest_pct, (int)sizeof(g_memtest_pct));
        }
        // kurono.kshtest=1: /dev/kshell rescue-channel gate. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.kshtest=1")) {
            boot_kshtest = true;
        }
        // kurono.dsai=1: ai recovery client end-to-end gate. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.dsai=1")) {
            boot_dsai = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.rtest=1")) {
            boot_rtest = true;
        }
        // kurono.claude=1 [kurono.claude.args="--version"] [kurono.claude.env="A=b C=d"]:
        // run the real claude code binary from the KURONO partition headless. (satoru)
        {
            static char cl_args[512], cl_env[512];
            boot_get_value(boot_cmdline, "kurono.claude.args", cl_args, sizeof(cl_args));
            boot_get_value(boot_cmdline, "kurono.claude.env", cl_env, sizeof(cl_env));
            // the env also feeds the `claude` shell command when no variables are exported (satoru)
            if (cl_args[0] || cl_env[0]) ClaudeHost::SetBootArgs(cl_args, cl_env);
            if (boot_has_token(boot_cmdline, "kurono.claude=1")) boot_claude = true;
            if (boot_has_token(boot_cmdline, "kurono.window.test=1")) ClaudeHost::SetWindowTest(true);
        }
        if (boot_has_token(boot_cmdline, "kurono.audiotest=1")) {
            boot_audiotest = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.uactest=1")) {
            boot_uactest = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.usbtest=1")) {
            boot_usbtest = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.usbmsdtest=1")) {
            boot_usbmsdtest = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.audiotest=2")) {
            boot_audiotest2 = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.usbaudiotest=1")) {
            boot_usbaudiotest = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.nettest=1")) {
            boot_nettest = true;
        }
        // bare-metal disk safety. an efi boot is a real machine, and the persist
        // store formats the whole first nvme disk - on a laptop that is the
        // windows/linux ssd. so efi boots (or kurono.diskguard=1) make every disk
        // read-only except the gpt partition named KURONO. kurono.diskguard=0
        // turns it off. set here, before any storage driver comes up. (satoru)
        if ((boot_has_token(boot_cmdline, "efi") || boot_has_token(boot_cmdline, "kurono.diskguard=1")) &&
            !boot_has_token(boot_cmdline, "kurono.diskguard=0")) {
            NVMe::SetGuardPolicy(1);
            NVMe::SetWriteGuard(true);
            SerialLogger::Log("[diskguard] ON: disks are read-only outside the KURONO partition\r\n");
        }
        // nvme init also guards any real (non-qemu) controller on its own; this
        // is the only way to opt out of that. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.diskguard=0")) NVMe::SetGuardPolicy(0);
        // kurono.bbtest=1: prebuilt third-party linux binary gate. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.bbtest=1")) {
            boot_bbtest = true;
        }
        // task 21: hard cpu pinning is DEFAULT ON (proven wedge fix + 3.3x
        // paint speedup); kurono.pincpu=0 restores migration for debugging. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.pincpu=0")) {
            boot_pincpu_off = true;
        }
        // task 29: kurono.fftrace=1 re-enables the firefox debug probes. they
        // are OFF by default because the uart busy-waits ~23.6us per byte, so
        // the ~70KB they emit costs ~1.5s of every boot. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.wltrace=1")) { extern volatile bool g_wltrace; g_wltrace = true; }   // (satoru)
        // background linux vm: kurono.vmlog=1 mirrors the guest console into this
        // log, kurono.vmram=N is the default guest ram in mb (satoru)
        if (boot_has_token(boot_cmdline, "kurono.vmlog=1")) VMHost::SetLogMirror(true);
        if (boot_has_token(boot_cmdline, "kurono.vmdebug=1")) VMHost::SetDebug(true);
        if (boot_has_token(boot_cmdline, "kurono.vm.par=1")) VMHost::SetPar(true);   // parallel vcpus (satoru)
        {   // kurono.vm.smp=N: N vcpus for the alpine guest (satoru)
            char sv[8];
            if (boot_get_value(boot_cmdline, "kurono.vm.smp", sv, (int)sizeof(sv))) VMHost::SetSmp(sv[0] - '0');
        }
        {   // kurono.vmdisk=N: virtual guest disk size in gb, 0 = just the image (satoru)
            char dv[8];
            if (boot_get_value(boot_cmdline, "kurono.vmdisk", dv, (int)sizeof(dv))) {
                int gb = 0; for (int i = 0; dv[i] >= '0' && dv[i] <= '9'; i++) gb = gb * 10 + (dv[i] - '0');
                VMHost::SetDiskGb(gb);
            }
        }
        if (boot_has_token(boot_cmdline, "kurono.vm.fixed=1")) VMHost::SetFixedMode(true);   // old fixed mode, for a/b (satoru)
        {   // test: back lazy guest chunks with 4 kb pages (1 = all, 2 = odd chunks) (satoru)
            char sv[8];
            if (boot_get_value(boot_cmdline, "kurono.vm.splitmem", sv, (int)sizeof(sv))) GuestMemoryManager::SetForceSplit(sv[0] - '0');
        }
        if (boot_has_token(boot_cmdline, "kurono.vmboot=1")) VMHost::SetForceAutostart(true);   // test rig (satoru)
        {
            static char tc[900];
            if (boot_get_value(boot_cmdline, "kurono.vmtest", tc, (int)sizeof(tc))) VMHost::SetTestCmds(tc);   // test rig (satoru)
        }
        {
            char v[16];
            if (boot_get_value(boot_cmdline, "kurono.vmram", v, (int)sizeof(v))) {
                uint32_t mb = 0;
                for (int i = 0; v[i] >= '0' && v[i] <= '9'; i++) mb = mb * 10 + (uint32_t)(v[i] - '0');
                VMHost::SetDefaultRam(mb);
            }
        }
        if (boot_has_token(boot_cmdline, "kurono.fftrace=1")) {
            boot_fftrace = true;
        }
        // task 23f: lazy madvise + thread spread a/b. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.madvlazy=1")) {
            boot_madvlazy = true;
        }
        // latch the ffmpeg smoke-test flag early (see decl). (satoru)
        if (boot_has_token(boot_cmdline, "kurono.ffmpeg.test=1") || boot_has_token(boot_cmdline, "kurono.ffmpeg.test")) {
            boot_ffmpeg_test = true;
        }
        // latch the dynamic-pie smoke-test flag (kurono.dyntest). (satoru)
        if (boot_has_token(boot_cmdline, "kurono.dyntest=1") || boot_has_token(boot_cmdline, "kurono.dyntest")) {
            boot_dyntest_test = true;
        }
        // latch the logging self-check flag (kurono.logcheck). (satoru)
        if (boot_has_token(boot_cmdline, "kurono.logcheck=1") || boot_has_token(boot_cmdline, "kurono.logcheck")) {
            boot_logcheck = true;
        }
        // latch the headless two-boot kfs persistence test flag (see decl). (satoru)
        if (boot_has_token(boot_cmdline, "kurono.kfstest=1") || boot_has_token(boot_cmdline, "kurono.kfstest")) {
            boot_kfstest = true;
        }
        // latch the headless storage benchmark flag (see decl). (satoru)
        if (boot_has_token(boot_cmdline, "kurono.kfsbench=1") || boot_has_token(boot_cmdline, "kurono.kfsbench")) {
            boot_kfsbench = true;
        }
        // latch the kcl self-test flag (see decl). (satoru)
        if (boot_has_token(boot_cmdline, "kurono.kcltest=1") || boot_has_token(boot_cmdline, "kurono.kcltest")) {
            boot_kcl_test = true;
        }
        // latch the 802.11i wifi crypto self-test flag (see decl). (satoru)
        if (boot_has_token(boot_cmdline, "kurono.wifitest=1") || boot_has_token(boot_cmdline, "kurono.wifitest")) {
            boot_wifi_test = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.platformtest=1")) boot_platform_test = true;   // (satoru)
        if (boot_has_token(boot_cmdline, "kurono.panic.classic=1")) KernelPanic::SetClassicScreen(true);   // (satoru)
        // skip panic reset methods to watch the fallbacks (qemu emulates all). (satoru)
        HAL::SetPanicResetSkip((boot_has_token(boot_cmdline, "kurono.panic.noacpi=1") ? 1u : 0u) |
                               (boot_has_token(boot_cmdline, "kurono.panic.nocf9=1") ? 2u : 0u) |
                               (boot_has_token(boot_cmdline, "kurono.panic.nokbd=1") ? 4u : 0u));
        if (boot_has_token(boot_cmdline, "kurono.backlight=1")) Backlight::SetWritesAllowed(true);   // 4a (satoru)
        if (boot_has_token(boot_cmdline, "kurono.sleep=idle")) IdleSleep::SetEnabled(true);   // 4d (satoru)
        // latch the kj self-test flag. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.kjtest=1") || boot_has_token(boot_cmdline, "kurono.kjtest")) {
            boot_kj_test = true;
        }
        // latch the kj-scripted animation demo flag. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.kjdemo=1") || boot_has_token(boot_cmdline, "kurono.kjdemo")) {
            boot_kj_demo = true;
        }
        // latch the kinit self-test gate + optional poweroff-after. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.kinit.test=1") || boot_has_token(boot_cmdline, "kurono.kinit.test")) {
            boot_kinit_test = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.kinit.poweroff=1") || boot_has_token(boot_cmdline, "kurono.kinit.poweroff")) {
            boot_kinit_poweroff = true;
        }
        // latch the kdf crash-recovery self-test gate + optional poweroff-after. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.kdf.test=1") || boot_has_token(boot_cmdline, "kurono.kdf.test")) {
            boot_kdf_test = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.kdf.poweroff=1") || boot_has_token(boot_cmdline, "kurono.kdf.poweroff")) {
            boot_kdf_poweroff = true;
        }
        // latch the linux syscall-abi self-test gate + optional poweroff-after. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.klstest=1") || boot_has_token(boot_cmdline, "kurono.klstest")) {
            boot_kls_test = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.klstest.poweroff=1") || boot_has_token(boot_cmdline, "kurono.klstest.poweroff")) {
            boot_kls_poweroff = true;
        }
        // latch the kmemx (memory compression) self-test gate + optional poweroff. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.kmemxtest=1") || boot_has_token(boot_cmdline, "kurono.kmemxtest")) {
            boot_kmemx_test = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.kmemx.poweroff=1") || boot_has_token(boot_cmdline, "kurono.kmemx.poweroff")) {
            boot_kmemx_poweroff = true;
        }
        if (boot_has_token(boot_cmdline, "kurono.mouse.raw=1") || boot_has_token(boot_cmdline, "kurono.mouse.raw")) {
            boot_mouse_raw = true;
        }
        // setup/installer wizard gate (the "Kurono Setup" grub entry). (satoru)
        if (boot_has_token(boot_cmdline, "kurono.setup=1") || boot_has_token(boot_cmdline, "kurono.setup")) {
            boot_setup = true;
        }
        // optional: open the setup wizard on a specific screen index. used for
        // headless screendump verification of individual screens. (satoru)
        {
            char sv[8]; sv[0] = 0;
            if (boot_get_value(boot_cmdline, "kurono.setup.screen", sv, (int)sizeof(sv)) && sv[0]) {
                int v = 0; for (int i = 0; sv[i] >= '0' && sv[i] <= '9'; i++) v = v * 10 + (sv[i] - '0');
                boot_setup_screen = v;
                boot_setup = true;
            }
        }
        // ksa isolation self-test gate. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.ksa.test=1") || boot_has_token(boot_cmdline, "kurono.ksa.test")) {
            boot_ksa_test = true;
        }
        // interactive ksa prompt demo gate. bare token => credential variant;
        // kurono.ksa.prompt=nocred => approve/deny-only (no password field). (satoru)
        {
            char pv[16]; pv[0] = 0;
            if (boot_get_value(boot_cmdline, "kurono.ksa.prompt", pv, (int)sizeof(pv))) {
                boot_ksa_prompt = true;
                if (pv[0] == 'n' || pv[0] == '0') boot_ksa_prompt_cred = false;  // nocred / 0 (satoru)
                else boot_ksa_prompt_cred = true;
            } else if (boot_has_token(boot_cmdline, "kurono.ksa.prompt")) {
                boot_ksa_prompt = true;
                boot_ksa_prompt_cred = true;
            }
        }
        // admin gate: enforcement is ON by default. kurono.ken.enforce=0 is
        // the safety switch that restores the old unrestricted behaviour. (satoru)
        {
            char ev[16]; ev[0] = 0;
            if (boot_get_value(boot_cmdline, "kurono.ken.enforce", ev, (int)sizeof(ev))) {
                bool on = !(ev[0] == '0' && ev[1] == 0);
                AdminGate::SetEnforce(on);
                SerialLogger::Log(on ? "[admin] enforcement ON\r\n"
                                     : "[admin] enforcement OFF (kurono.ken.enforce=0)\r\n");
            }
        }
        // test/tuning: kurono.ksa.timeout=<seconds> shortens the modal timeout. (satoru)
        {
            char tv[16]; tv[0] = 0;
            if (boot_get_value(boot_cmdline, "kurono.ksa.timeout", tv, (int)sizeof(tv))) {
                uint32_t secs = 0;
                for (int i = 0; tv[i] >= '0' && tv[i] <= '9'; i++) secs = secs * 10 + (uint32_t)(tv[i] - '0');
                if (secs) KSA::SetPromptTimeoutMs(secs * 1000u);
            }
        }
        // smp 3d: run user processes on the application processors (opt-in). set the
        // gate BEFORE StartAPs so each ap dispatches from the moment it comes up;
        // without the token the APs park (hlt) exactly as before. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.apsched=1") || boot_has_token(boot_cmdline, "kurono.apsched")) {
            SMP::SetApUserSched(true);
        }
        // smp thread dispatch: aps also RESUME ready sibling threads of a
        // multi-threaded process (firefox's compositor/render/ipc threads run in
        // parallel with the chrome main on the bsp). same before-StartAPs
        // ordering as apsched. (satoru)
        // real hardware boots default this on: without it every app thread shares the bsp with the
        // desktop while the other cores sit parked. kurono.apthreads=0 turns it back off. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.apthreads=1") || boot_has_token(boot_cmdline, "kurono.apthreads") ||
            (boot_has_token(boot_cmdline, "efi") && !boot_has_token(boot_cmdline, "kurono.apthreads=0"))) {
            SMP::SetApThreadSched(true);
        }
        // firefox jit tier experiment gate: kurono.ffjit=1 (blinterp) or =2
        // (+baseline+native_regexp). default 0 = interpreter-only. (satoru)
        {
            char jv[8]; jv[0] = 0;
            if (boot_get_value(boot_cmdline, "kurono.ffjit", jv, (int)sizeof(jv)) && jv[0]) {
                int lvl = jv[0] - '0';
                if (lvl >= 0 && lvl <= 3) KuronoShell::SetFirefoxJit(lvl);
            }
        }
        boot_get_value(boot_cmdline, "kurono.cli.run", boot_cli_run, (int)sizeof(boot_cli_run));
        // optional GUI autorun: open Terminal and queue a command. Used purely
        // for headless QEMU debug runs where we cannot type interactively.
        boot_get_value(boot_cmdline, "kurono.gui.run", boot_gui_run, (int)sizeof(boot_gui_run));
        {   // power test hook: kurono.power.test=restart|shutdown|sleep|cc-restart|
            // cc-shutdown|button|autosave, with kurono.power.test.delay=MS. (satoru)
            char pt[48] = {0}, pd[16] = {0};
            if (boot_get_value(boot_cmdline, "kurono.power.test", pt, (int)sizeof(pt)) && pt[0]) {
                uint32_t delay = 15000;
                if (boot_get_value(boot_cmdline, "kurono.power.test.delay", pd, (int)sizeof(pd))) {
                    uint32_t v = 0;
                    for (int ci = 0; pd[ci] >= '0' && pd[ci] <= '9'; ci++) v = v * 10 + (uint32_t)(pd[ci] - '0');
                    if (v) delay = v;
                }
                PowerManager::SetTest(pt, delay);
            }
        }
        {   // test hook: extra terminal windows after the autorun one (satoru)
            char gt[8] = {0};
            boot_get_value(boot_cmdline, "kurono.guiterms", gt, (int)sizeof(gt));
            extern int g_gui_extra_terms;
            g_gui_extra_terms = (gt[0] >= '0' && gt[0] <= '9') ? gt[0] - '0' : 0;
        }
        {   // test hook: kurono.ffafter=N launches firefox N s after the autorun
            // command, so a qemu run can have claude (or anything) going first,
            // like the laptop session where firefox never started. (satoru)
            char fa[8] = {0};
            boot_get_value(boot_cmdline, "kurono.ffafter", fa, (int)sizeof(fa));
            extern uint32_t g_gui_ff_after_s;
            uint32_t v = 0;
            for (int i = 0; fa[i] >= '0' && fa[i] <= '9'; i++) v = v * 10 + (uint32_t)(fa[i] - '0');
            g_gui_ff_after_s = v;
        }
    }
    bool force_text_mode = boot_text_only || boot_console_realtime || boot_emergency || boot_cli;

    vga_puts("Multiboot OK\n");
    if (has_early_fb) { early_fb_puts("[OK] Multiboot\n", 0x00FF00); early_fb_flush(); }
    SerialLogger::Log("Multiboot OK\r\n");
    RuntimeLog::LogBoot("multiboot handshake complete");
    if (boot_cmdline) {
        SerialLogger::Log("Boot cmdline: ");
        log_cmdline_redacted(boot_cmdline);
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
    // SMAP (bit 21), UMIP (bit 11), OSXSAVE (bit 18) - gated by CPUID
    // capability bits to avoid #UD on older hardware.
    {
        // bound leaf 7 by the maximum basic leaf: an old part returns the
        // highest basic leaf's data for an out-of-range request, which could
        // report smep/umip bits this cpu does not have. (satoru)
        uint32_t a = 0, b = 0, c = 0, d = 0;
        uint32_t ebx7 = 0, ecx7 = 0;
        __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
        if (a >= 7)
            __asm__ __volatile__("cpuid" : "=a"(a), "=b"(ebx7), "=c"(ecx7), "=d"(d) : "a"(7), "c"(0));
        uint64_t cr4;
        __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
        uint64_t set = 0;
        if (ebx7 & (1u << 7)) set |= (1ULL << 20);  // SMEP
        // SMAP intentionally NOT enabled: the kernel currently performs
        // direct supervisor-mode access to user pointers in the syscall
        // dispatch path.  Turning SMAP on without STAC/CLAC wrappers
        // around every copy_to_user would #PF instantly.  Re-enable once
        // every user-pointer touch is funnelled through a proper helper.
        if (ecx7 & (1u << 2)) set |= (1ULL << 11);  // UMIP
        { extern bool g_ubtrace_on; g_ubtrace_on = boot_has_token(boot_cmdline, "kurono.ubtrace=1"); }   // [ubchk] (satoru)
        { extern bool g_vfork_copyfix; if (boot_has_token(boot_cmdline, "kurono.vfcopyfix=0")) g_vfork_copyfix = false; }   // (satoru)
        { g_envwatch = boot_has_token(boot_cmdline, "kurono.envwatch=1"); }   // (satoru)
        { g_pflooptest = boot_has_token(boot_cmdline, "kurono.pflooptest=1"); }   // [pfloop] self-test (satoru)
        // cr0.wp on the bsp now, and the policy every ap copies in ap_entry. (satoru)
        SMP::CrPolicyBsp(boot_has_token(boot_cmdline, "kurono.wp=1"), set);
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
    // the boot stub built its page tables inside the low boot segment, which a
    // fixed-address image may shadow later. deep-copy the live kernel address
    // space into pmm frames now (outside the still-reserved image window) and
    // switch cr3, so every table the vmm walks afterwards lives in ordinary ram.
    // this must happen before smp and before the window is released. (satoru)
    if (!boot_has_token(boot_cmdline, "kurono.root.keep=1")) KernelVMM::RelocateKernelRoot();
    MemCensus::Init();   // no-op unless kurono.memcensus=1 turned the pmm tags on (satoru)
    TaskMem::Init();     // no-op unless kurono.taskmem=1 (satoru)
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
    // [2c] kmemx (memory compression engine): reserve the compressed-page pool +
    // metadata table now, while contiguous memory is plentiful (it allocates the
    // table first, then a free-memory-bounded pool). this only RESERVES the
    // infrastructure; whether the engine is enabled + its worker process started
    // is decided later from config at the kernel.target (see kmemx.enabled). the
    // never-compress list for dma rings / framebuffer / ept is registered by the
    // owning subsystems as they come up. (satoru)
    // the pool itself is adaptive (grown under pressure by the worker), so Init
    // only takes the metadata table + scratch, and nothing at all when the
    // engine is switched off on the cmdline or the machine is below ~256mb. a
    // config-disabled engine gives the table back at ApplyConfig. (satoru)
    // kurono.kmemx=0: hard-disable the compression engine for the wedge a/b
    // (task 20: if the pre-KX2 glib wedge vanishes with the compressor off,
    // its free/refault path is the write-to-dead-frame source). flag parsed
    // early - the raw cmdline string is clobbered by the allocator inits. (satoru)
    if (boot_kmemx_off) {
        KMemX::ForceDisable();   // survives the later ApplyConfig (satoru)
        SerialLogger::Log("[2c] KMemX DISABLED by cmdline (kurono.kmemx=0), nothing reserved\r\n");
    } else {
        SerialLogger::Log("[2c] KMemX::Init\r\n");
        KMemX::Init(20);
    }
    if (boot_kmx_idle_off) {
        KMemX::SetIdleScan(false);
        SerialLogger::Log("[2c] KMemX idle scan OFF (kurono.kmxidle=0)\r\n");
    }
    if (boot_kmx_active_off) {
        KMemX::SetActiveScan(false);
        SerialLogger::Log("[2c] KMemX active-space scan OFF (kurono.kmxactive=0)\r\n");
    }
    if (boot_kmx_all) {
        KMemX::SetZeroOnly(false);
        SerialLogger::Log("[2c] KMemX non-zero page scan ON (kurono.kmxall=1)\r\n");
    }
    // task 29: the firefox debug probes are OFF by default (the uart busy-waits
    // ~23.6us/byte, so their ~70KB cost ~1.5s of every boot). (satoru)
    if (boot_has_token(boot_cmdline, "kurono.lktrace=1")) {
        extern bool g_lktrace;   // linux_syscall.cpp (satoru)
        g_lktrace = true;
    }
    if (boot_fftrace) {
        extern bool g_fftrace;   // linux_syscall.cpp (satoru)
        g_fftrace = true;
        SerialLogger::Log("[2c] firefox debug probes ON (kurono.fftrace=1)\r\n");
    }
    if (boot_pincpu_off) {
        Scheduler::SetPinCpu(false);
        SerialLogger::Log("[2c] cpu pinning DISABLED by cmdline (kurono.pincpu=0)\r\n");
    }
    if (boot_madvlazy) {
        extern bool g_madv_lazy;
        g_madv_lazy = true;
        SerialLogger::Log("[2c] madvlazy + thread-spread ENABLED (kurono.madvlazy=1)\r\n");
    }
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
    // kld: keep a copy of the firmware memory map so the acpi container can
    // never be handed usable ram. (satoru)
    KldBroker::SnapshotMemoryMap(mbi);

    // 1000 Hz PIT: a fine tick is needed so SleepMs-based process wakeups
    // (input/cursor at ~8ms) are timely -> smooth 60fps. The ~2000 timer
    // VM-exits/sec this costs is ~0.4% cpu (negligible; the big VM-exit
    // sources - input double-poll, idle non-HLT, audio polling - are fixed
    // separately). The ms clock is TSC-based regardless of tick rate. (satoru)
    Timer::Init(1000);
    TimeManager::SelectPIT(1000);
    TimeManager::Init();

    // Calibrate the TSC NOW (right after the PIT), not late in boot. Timer::
    // GetRealMs()/WaitMs() prefer the TSC, and the boot splash + other early
    // delays call WaitMs() long before this used to run. Without the TSC,
    // WaitMs falls back to the polled PIT clock, which LOSES timer periods
    // under hardware hypervisors (WHPX/VMware) where each port read VM-exits
    // and host scheduling delays the poll - so the "~3s" splash stretched to
    // 15s on WHPX and minutes on VMware (the black screen after the logo).
    // CPUDetect::Init calibrates the TSC via the precise PIT ch2 one-shot. (satoru)
    CPUDetect::Init();

    // allocate the userspace vdso time page now that the tsc clock is calibrated;
    // OnTimerTick refreshes it each ms and ld-kurono maps it read-only into every
    // process so clock_gettime/gettimeofday are served in userspace (the boot-
    // speed fix - see kvdso.h). (satoru)
    KernelVdso::Init();

    // smp phase 1: enable the local apic + enumerate the cpus (no APs started yet,
    // so this is harmless on the single-core path). (satoru)
    SerialLogger::Log("[SMP] Init...\r\n");
    {   // kurono.maxcpus=N: cap on online cpus (default 8 on efi boots) (satoru)
        char mcv[16];
        if (boot_get_value(boot_cmdline, "kurono.maxcpus", mcv, sizeof(mcv))) {
            uint32_t mv = 0;
            for (int ci = 0; mcv[ci] >= '0' && mcv[ci] <= '9'; ci++) mv = mv * 10 + (uint32_t)(mcv[ci] - '0');
            SMP::SetMaxCpus(mv);
        }
    }
    // kurono.sched=2: the new smp scheduling mode (see proc/scheduler.h and
    // scratchpad/agent_sched_design.md). must precede SMP::Init / StartAPs.
    // kurono.schedrel=0 keeps sched=2 but restores the early on_cpu release,
    // for a one-binary a/b of the stack hand-off. (satoru)
    if (boot_has_token(boot_cmdline, "kurono.schedswitch=0")) LinuxSyscall::SetSwitchStrict(false);
    if (boot_has_token(boot_cmdline, "kurono.schedswitch=1")) LinuxSyscall::SetSwitchStrict(true);   // the stranding fix in the old mode (satoru)
    if (boot_has_token(boot_cmdline, "kurono.ptytrace=1")) LinuxSyscall::SetPtyTrace(true);
    if (boot_has_token(boot_cmdline, "kurono.termsh=1")) TerminalApp::SetShellMode(true);   // a real shell on every terminal pty (satoru)
    if (boot_has_token(boot_cmdline, "kurono.pty=0")) LinuxSyscall::SetPtyLegacy(true);   // old single-session console, a/b (satoru)
    if (boot_has_token(boot_cmdline, "kurono.kls=global")) LinuxSyscall::SetKlsGlobal(true);   // keep the old single syscall lock, both sched modes (satoru)
    if (boot_has_token(boot_cmdline, "kurono.powertrace=1")) Scheduler::SetPickTrace(true);
    // sched=2 is the default since the smp job (agent_smp_results.md): every core
    // runs programs, the desktop and the services have cores of their own.
    // kurono.sched=1 (or =0) boots the old scheduler: kernel procs and the
    // programs' leaders on cpu 0, threads pinned, 8 cpus on efi. (satoru)
    bool sched_old = boot_has_token(boot_cmdline, "kurono.sched=1") ||
                     boot_has_token(boot_cmdline, "kurono.sched=0");
    if (sched_old) SerialLogger::Log("[sched] old scheduler (kurono.sched=1)\r\n");
    if (!sched_old) {
        Scheduler::SetSchedMode(2);
        // the aps run every user task in sched=2, so their dispatch loops must
        // claim (the old apthreads gate). (satoru)
        SMP::SetApThreadSched(true);
        if (boot_has_token(boot_cmdline, "kurono.schedrel=0")) Scheduler::SetDeferredRelease(false);
        if (boot_has_token(boot_cmdline, "kurono.schedlaunch=0")) Scheduler::SetDetachedLaunch(false);
        if (boot_has_token(boot_cmdline, "kurono.schedcpus=0")) Scheduler::SetReportAllCpus(false);
        if (boot_has_token(boot_cmdline, "kurono.klsprof=1")) LinuxSyscall::SetKlsProf(true);   // per-syscall kls profile (satoru)
        if (boot_has_token(boot_cmdline, "kurono.wrprefetch=0")) LinuxSyscall::SetWritePrefetch(false);   // write-path lazy materialize under kls (satoru)
        if (boot_has_token(boot_cmdline, "kurono.heapreserve=0")) KernelHeap::SetReserve(false);   // background heap growth (satoru)
        { extern bool g_heap_direct; if (boot_has_token(boot_cmdline, "kurono.heapdirect=0")) g_heap_direct = false; }   // big blocks stay in the regions (satoru)
        if (boot_has_token(boot_cmdline, "kurono.rdunlock=0")) LinuxSyscall::SetReadUnlock(false);   // lazy file reads with kls released (satoru)
        if (boot_has_token(boot_cmdline, "kurono.memboot=1")) MemBoot::SetBootReport(true);   // boot memory report on serial (satoru)
        if (boot_has_token(boot_cmdline, "kurono.mmapprefetch=0")) LinuxSyscall::SetMmapPrefetch(false);   // (satoru)
        if (boot_has_token(boot_cmdline, "kurono.kfsrun=0")) KFS::SetRunReads(false);   // (satoru)
        if (boot_has_token(boot_cmdline, "kurono.mmappfverify=1")) { KFS::SetRunVerify(true); LinuxSyscall::SetMmapPrefetchVerify(true); }   // test only (satoru)
        if (boot_has_token(boot_cmdline, "kurono.schedpoll=0")) LinuxSyscall::SetPollLongWait(false);
        if (boot_has_token(boot_cmdline, "kurono.schedtrace=1")) LinuxSyscall::SetSchedTrace(true);
        if (boot_has_token(boot_cmdline, "kurono.audiosplit=1")) KernelProcesses::SetAudioKlsPump(false);   // (satoru)
        if (boot_has_token(boot_cmdline, "kurono.schedterm=0")) KernelProcesses::SetTermWorkers(false);   // lost in the 6-8 merge (satoru)
        if (!boot_has_token(boot_cmdline, "kurono.kstackguard=0")) Scheduler::SetKstackGuard(true);
        SerialLogger::Log(boot_has_token(boot_cmdline, "kurono.schedrel=0")
            ? "[sched2] mode 2 on, deferred release OFF (a/b arm)\r\n"
            : "[sched2] mode 2 on: deferred on_cpu release, unpinned (kurono.sched=1 = old scheduler)\r\n");
    }
    if (boot_has_token(boot_cmdline, "kurono.termtest=1")) KernelProcesses::SetTermTest(true);   // test rig, lost in the 6-8 merge (satoru)
    {   // kurono.appexit=SECS: test rig, close firefox + census afterwards (satoru)
        char ae[8] = {0};
        if (boot_get_value(boot_cmdline, "kurono.appexit", ae, (int)sizeof(ae))) {
            int s = 0;
            for (int i = 0; ae[i] >= '0' && ae[i] <= '9'; i++) s = s * 10 + (ae[i] - '0');
            if (s > 0) KernelProcesses::SetAppExitTest((uint32_t)s);
        }
    }
    if (boot_has_token(boot_cmdline, "kurono.kstacktest=1")) KernelProcesses::SetKstackTest(true);   // test rig (satoru)
    if (boot_has_token(boot_cmdline, "kurono.kstackguard=1")) Scheduler::SetKstackGuard(true);   // also outside sched=2 (satoru)
    if (boot_has_token(boot_cmdline, "kurono.kplat=1")) Scheduler::SetKprocLatProbe(true);   // kernel-proc latency probe (satoru)
    if (boot_has_token(boot_cmdline, "kurono.madttest=1")) SMP::SelfTestMadt();
    if (boot_has_token(boot_cmdline, "kurono.x2apictest=1")) SMP::SetX2apicTest(1);
    if (boot_has_token(boot_cmdline, "kurono.x2apictest=2")) SMP::SetX2apicTest(2);
    // kurono.forcex2apic=1: qemu stand-in for the laptop's firmware-locked x2apic: the
    // bsp is forced into x2apic and treated as locked, the aps enable x2apic themselves. (satoru)
    if (boot_has_token(boot_cmdline, "kurono.forcex2apic=1")) SMP::SetX2apicTest(2);
    SMP::Init();
    SMP::EnableFastCpuIndex();   // sched=2 only: CpuIndex via tsc_aux (satoru)
    // smp phase 2: bring up the application processors (INIT-SIPI-SIPI). each ap
    // reaches ap_entry, marks itself online, then idles until the scheduler hands
    // it work (phase 3). a failed ap is non-fatal - the bsp carries on. (satoru)
    SMP::StartAPs();
    // bsp tick from its lapic timer once other cpus exist (see StartBspLapicTick).
    // kurono.bsptick=pit keeps the 8259 pit tick, =lapic forces it on one cpu. (satoru)
    SMP::StartBspLapicTick(boot_has_token(boot_cmdline, "kurono.bsptick=pit") ? -1 :
                           boot_has_token(boot_cmdline, "kurono.bsptick=lapic") ? 1 : 0);
    // kurono.svccpu (sched=2, default on): the desktop, the media/network services
    // and the vm host get aps of their own. decided here, before any kernel proc
    // is spawned and before the boot animation can claim an ap. kurono.svccpu=0
    // keeps every kernel proc on cpu 0; kurono.svc.ui / .io / .vm=N move one by
    // hand (0 = keep it on cpu 0). (satoru)
    {
        if (boot_has_token(boot_cmdline, "kurono.svccpu=0")) Scheduler::SetServiceCpus(false);
        auto svc_num = [&](const char* key) -> int {
            char v[8] = {0};
            if (!boot_get_value(boot_cmdline, key, v, (int)sizeof(v))) return -1;
            int n = 0, k = 0;
            for (; v[k] >= '0' && v[k] <= '9'; k++) n = n * 10 + (v[k] - '0');
            return k ? n : -1;
        };
        VMHost::AutoSmp((int)SMP::OnlineCount());
        { int nv = VMHost::ParCpus(); if (nv > 1) Scheduler::SetVcpuCpus(nv - 1); }   // kurono.vm.par (satoru)
        Scheduler::SetServiceCpuRequest(svc_num("kurono.svc.ui"), svc_num("kurono.svc.io"), svc_num("kurono.svc.vm"));
        Scheduler::PlanServiceCpus();
        int cl = svc_num("kurono.cpuload");
        if (cl >= 0) KernelProcesses::SetCpuLoadSecs((uint32_t)cl);
    }

    // note: interrupts stay disabled. the kernel is fully polling-based
    // (pit counter read, keyboard/mouse i/o ports). this avoids whpx
    // compatibility issues with hardware interrupt delivery.

    SerialLogger::Log("Initializing display...\r\n");
    vga_puts("Display init...\n");
    // early storage + boot log. bare metal has no serial port, so the log has
    // to reach disk BEFORE the steps that tend to hang (display, gpu probe).
    // kdf/irp/kexec only need pmm+vmm. nvme init guards a real controller on
    // its own, then disklog finds the KURONO partition and goes EAGER: every
    // line is written as it is printed until the 1 s worker takes over. (satoru)
    SerialLogger::Log("[KDF] Init...\r\n");
    KDF::Init();
    IRP::Init();
    KExec::Init();
    {
        bool nv = NVMe::Init();
        {   // kurono.logname=NAME: a test boot writes NAME.LOG instead of KURONO.LOG (satoru)
            char ln[16];
            if (boot_get_value(boot_cmdline, "kurono.logname", ln, sizeof(ln))) DiskLog::SetLogName(ln);
        }
        bool win = nv && NVMe::WriteGuardOn() && DiskLog::FindKuronoWindow();
        if (win && DiskLog::Init()) {
            DiskLog::SetEager(true);
            if (has_early_fb) { early_fb_puts("[OK] Boot log -> KURONO.LOG\n", 0x00FF00); early_fb_flush(); }
        } else if (nv && !NVMe::WriteGuardOn()) {
            // no write guard means no writable window was ever offered: this is
            // the normal qemu/bios boot, not a failure, so do not stop on the
            // photo screen (that hold used to burn minutes here because the
            // scheduler ms clock has not started ticking yet). (satoru)
            SerialLogger::Log("[disklog] disk write guard off: boot log stays serial-only\r\n");
        } else if (has_early_fb) {
            // say exactly which step failed, on screen, and HOLD it for 20 s:
            // there is no log to read and the lines otherwise scroll past
            // before anyone can photograph them. (satoru)
            early_fb_puts("[!!] NO BOOT LOG: ", 0xFF4444);
            early_fb_puts(nv ? DiskLog::Status() : "nvme init failed", 0xFF4444);
            early_fb_puts("\n", 0);
            if (nv) {
                char num[24];
                early_fb_puts("     nvme: ", 0xFFFFFF);
                early_fb_puts(NVMe::GetInfo().model, 0xFFFFFF);
                early_fb_puts("\n     lba size ", 0xFFFFFF);
                int ln = 0; uint64_t v = NVMe::GetLBASize();
                char t[24]; int tk = 0; if (!v) t[tk++] = '0';
                while (v) { t[tk++] = (char)('0' + v % 10); v /= 10; }
                while (tk) num[ln++] = t[--tk];
                num[ln] = 0;
                early_fb_puts(num, 0xFFFFFF);
                early_fb_puts("  capacity lbas ", 0xFFFFFF);
                early_fb_puthex32((uint32_t)(NVMe::GetCapacityLBA() >> 32), 0xFFFFFF);
                early_fb_puthex32((uint32_t)NVMe::GetCapacityLBA(), 0xFFFFFF);
                early_fb_puts("\n", 0);
            }
            // the last log lines (the failing driver's own messages), since
            // they never reach a disk. (satoru)
            early_fb_puts("     last log lines:\n", 0xFFFF00);
            {
                const char* cap = SerialLogger::CaptureData();
                uint32_t len = SerialLogger::CaptureLen();
                uint32_t pos = len; int lines = 0;
                while (pos > 0 && lines < 16) { pos--; if (cap[pos] == '\n') lines++; }
                if (pos < len && cap[pos] == '\n') pos++;
                char ln[104]; int n = 0;
                for (uint32_t i = pos; i < len; i++) {
                    char c = cap[i];
                    if (c == '\n') { ln[n] = 0; early_fb_puts("     ", 0); early_fb_puts(ln, 0xC0C0C0); early_fb_puts("\n", 0); n = 0; continue; }
                    if (c >= 0x20 && c < 0x7F && n < 100) ln[n++] = c;
                }
            }
            early_fb_puts("     (holding this screen for 20 seconds - take a photo)\n", 0xFFFF00);
            early_fb_flush();
            // the scheduler ms clock only advances once the timer irq runs, and
            // this point is before the scheduler starts with interrupts off, so
            // NowMs never moved and the old 4e9-iteration bound became the real
            // wait on some machines. use the tsc clock and keep a hard cap.
            // (satoru)
            uint64_t t0 = Timer::GetRealMs64();
            for (uint64_t spin = 0; spin < 1000000000ull; spin++) {
                if (Timer::GetRealMs64() - t0 >= 20000) break;
                __asm__ __volatile__("pause");
            }
        }
    }
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

    // stage 0 intel display probe: read-only dump of what the firmware lit,
    // as [igpu] lines in the serial log. one line and back when there is no
    // intel gpu. runs before any intel driver touches the device. (satoru)
    IntelDisplayProbe::Run(nullptr, 0);
    // stage 1: dpcd + edid over dp aux on ports the firmware already powers
    // and owns, as [igpu-aux] lines. default on, kurono.igpu.aux=0 skips it.
    // without a usable intel display engine it logs one line. (satoru)
    if (boot_has_token(boot_cmdline, "kurono.igpu.aux=0"))
        SerialLogger::Log("[igpu-aux] skipped (kurono.igpu.aux=0)\r\n");
    else {
        IntelDpAux::SetTcLegacyProbe(boot_has_token(boot_cmdline, "kurono.igpu.tclegacy=1"));   // fix 6 (satoru)
        // legacy type-c connect + modeset of its dp sink (default off) (satoru)
        IntelDpAux::SetLegacyTc(boot_has_token(boot_cmdline, "kurono.igpu.legacytc=1"));
        IntelDpAux::Run(nullptr, 0);
    }
    // stage 2: light the external dp sink stage 1 found on a type-c port,
    // on pipe b. runs only with such a sink; kurono.igpu.modeset=0 or a
    // KUROMODE.OFF file in the KURONO partition root skips it. without an
    // intel engine it logs one line. (satoru)
    IntelModeset::Boot(boot_has_token(boot_cmdline, "kurono.igpu.modeset=0"));

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

            // on an efi boot the gop address IS the surface the firmware is
            // scanning out, so it is authoritative. the probe's correction
            // assumes the intel igpu drives the panel; on an optimus laptop
            // with the panel on the nvidia gpu it redirected every draw into
            // the igpu aperture and the screen froze on the boot text. log
            // it, never apply it, on efi boots. (satoru)
            if (validated_fb != fb_phys && boot_has_token(boot_cmdline, "efi")) {
                SerialLogger::Log("Display: GpuProbe FB correction IGNORED on efi boot (gop is authoritative)\r\n");
                if (has_early_fb) {
                    early_fb_puts("[!!] FB correction IGNORED (efi gop): ", 0xFFFF00);
                    early_fb_puthex32((uint32_t)(validated_fb >> 32), 0xFFFF00);
                    early_fb_puthex32((uint32_t)(validated_fb & 0xFFFFFFFF), 0xFFFF00);
                    early_fb_puts("\n", 0);
                    early_fb_flush();
                }
                validated_fb = fb_phys;
            }
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
                early_fb_puts(" at ", 0x00FF00);
                early_fb_puthex32((uint32_t)(fb_phys >> 32), 0x00FF00);
                early_fb_puthex32((uint32_t)(fb_phys & 0xFFFFFFFF), 0x00FF00);
                early_fb_puts("\n", 0);
                early_fb_puts("[..] Graphics::Init (wc remap)\n", 0xFFFF00);
                early_fb_flush();
            }

            // init graphics first - this remaps fb pages to write-combining,
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
            if (boot_debug)   // kurono.bootdebug=1 only (satoru)
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

    // 3. bga (qemu/bochs only - not present on real hardware)
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
            vga_puts("=== Kurono OS - No Graphics Framebuffer ===", 0x0C);
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
            vga_puts("\n\nSystem halted - reboot and try a different GRUB entry.", 0x0C);
            // don't halt - let kernel continue so serial diag still works
        }
    }

    // stage 3a: hardware cursor on the intel display engine. default off; with
    // kurono.igpu.accel=1 it validates the scanout ptes and the cursor slot,
    // runs the self-test and leaves the cursor hidden until the pointer moves.
    // the call is silent and does nothing when the flag is absent. it runs
    // before the multi-monitor output setup because it only needs the boot
    // framebuffer and the stage 2 pipe b result. (satoru)
    if (has_display) {
        IntelAccel::SetWmCalc(boot_has_token(boot_cmdline, "kurono.igpu.accel.wmcalc=1"));
        IntelAccel::Boot(boot_has_token(boot_cmdline, "kurono.igpu.accel=1"),
                         !boot_has_token(boot_cmdline, "kurono.igpu.accel.noptefb=1"),
                         (uint64_t)(uintptr_t)Graphics::GetBuffer(),
                         Graphics::GetWidth(), Graphics::GetHeight(), Graphics::GetPitch());
    }

    // multi-monitor: log every gop handle the firmware exposed, then register the
    // primary plus each distinct extra framebuffer as display outputs and switch
    // the compositor to a virtual desktop when there are two or more. with one gop
    // (or kurono.multimon=0) none of this runs and the single-fb path is untouched.
    // must happen before the render mode / desktop layout below. (satoru)
    if (has_display) {
        for (int gi = 0; gi < g_gop_seen_n; gi++) {
            const GopSeen& g = g_gop_seen[gi];
            SerialLogger::Log("[gop] #"); SerialLogger::LogDec(g.idx);
            SerialLogger::Log(" base=");
            SerialLogger::LogHex((uint32_t)(g.base >> 32)); SerialLogger::LogHex((uint32_t)g.base);
            SerialLogger::Log(" "); SerialLogger::LogDec((int)g.w);
            SerialLogger::Log("x"); SerialLogger::LogDec((int)g.h);
            SerialLogger::Log(" pitch="); SerialLogger::LogDec((int)g.pitch);
            SerialLogger::Log(" fmt="); SerialLogger::LogDec(g.fmt);
            SerialLogger::Log(" devpath="); SerialLogger::LogDec(g.flags & 1);
            SerialLogger::Log((g.flags & 2) ? " role=PRIMARY" : (g.flags & 4) ? " role=EXTRA" : " role=ignored");
            SerialLogger::Log("\r\n");
        }
        SerialLogger::Log("[gop] handles seen="); SerialLogger::LogDec(g_gop_seen_n);
        SerialLogger::Log("\r\n");
        if (g_gop_seen_n >= 2 && (mbi->flags & (1u << 12)) &&
            !boot_has_token(boot_cmdline, "kurono.multimon=0")) {
            Outputs::Reset();
            Outputs::Add(mbi->framebuffer_addr, (int)mbi->framebuffer_width,
                         (int)mbi->framebuffer_height, mbi->framebuffer_pitch,
                         mbi->framebuffer_bpp, false, "Display 1");
            for (int gi = 0; gi < g_gop_seen_n; gi++) {
                const GopSeen& g = g_gop_seen[gi];
                if (!(g.flags & 4)) continue;
                static const char* nm[] = { "Display 2", "Display 3", "Display 4" };
                int ix = Outputs::Total();
                if (ix >= 4) break;
                Outputs::Add(g.base, (int)g.w, (int)g.h, g.pitch, 32, g.fmt == 0, nm[ix - 1]);
            }
            if (Outputs::Multi() && Graphics::EnableMultiOutput()) {
                SerialLogger::Log("[outputs] multi-monitor ACTIVE: ");
                SerialLogger::LogDec(Outputs::Connected());
                SerialLogger::Log(" outputs\r\n");
                for (int oi = 0; oi < Outputs::Total(); oi++) {
                    const Output* o = Outputs::Get(oi);
                    SerialLogger::Log("[outputs]  #"); SerialLogger::LogDec(oi);
                    SerialLogger::Log(" "); SerialLogger::Log(o->name);
                    SerialLogger::Log(" at ("); SerialLogger::LogDec(o->x);
                    SerialLogger::Log(","); SerialLogger::LogDec(o->y);
                    SerialLogger::Log(") "); SerialLogger::LogDec(o->w);
                    SerialLogger::Log("x"); SerialLogger::LogDec(o->h);
                    SerialLogger::Log("\r\n");
                }
            } else {
                Outputs::Reset();
                SerialLogger::Log("[outputs] single output (extras not usable)\r\n");
            }
        } else if (g_gop_seen_n >= 2) {
            SerialLogger::Log("[outputs] multi-monitor disabled by cmdline\r\n");
        } else {
            SerialLogger::Log("[outputs] single output\r\n");
        }
        // the external screen stage 2 lit on pipe b becomes a second output
        // (its framebuffer is ram kurono owns, 2 mb aligned for the wc
        // remap) unless the gop already gave us several. (satoru)
        uint64_t im_phys = 0; int im_w = 0, im_h = 0; uint32_t im_pitch = 0;
        if (!Outputs::Multi() && (mbi->flags & (1u << 12)) &&
            !boot_has_token(boot_cmdline, "kurono.multimon=0") &&
            IntelModeset::GetOutput(&im_phys, &im_w, &im_h, &im_pitch)) {
            Outputs::Reset();
            Outputs::Add(mbi->framebuffer_addr, (int)mbi->framebuffer_width,
                         (int)mbi->framebuffer_height, mbi->framebuffer_pitch,
                         mbi->framebuffer_bpp, false, "Display 1");
            Outputs::Add(im_phys, im_w, im_h, im_pitch, 32, false, "Display 2");
            if (Outputs::Multi() && Graphics::EnableMultiOutput()) {
                SerialLogger::Log("[outputs] multi-monitor ACTIVE with the intel pipe b screen: ");
                SerialLogger::LogDec(im_w); SerialLogger::Log("x"); SerialLogger::LogDec(im_h);
                SerialLogger::Log("\r\n");
                // a locked legacy type-c screen (kurono.igpu.legacytc=1) is the
                // one the owner reads: it takes the menu bar and dock, the
                // laptop panel becomes the second screen. queued like the
                // settings page's "make primary", applied by the compositor.
                // kurono.igpu.tcprimary=0 keeps the laptop panel primary. (satoru)
                if (IntelModeset::LegacyTcLit() && !boot_has_token(boot_cmdline, "kurono.igpu.tcprimary=0")) {
                    Graphics::RequestOutputOp(3, 1, 0);
                    SerialLogger::Log("[igpu-tc] external screen made the primary output (menu bar + dock)\r\n");
                }
            } else {
                Outputs::Reset();
                SerialLogger::Log("[outputs] intel pipe b screen lit but not usable as an output, test pattern stays\r\n");
            }
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

        // the panel's real rate for the frame pacer, read-only: the intel probe's
        // pipe timings first (the panel gop left lit), then a measured vga vsync,
        // else 60. no modeset or register write either way. kurono.hz=N then
        // caps it; auto with nothing better detected stays 60. (satoru)
        {
            uint32_t igpu_mhz = IntelDisplayProbe::RefreshMilliHz((uint32_t)Graphics::GetWidth(),
                                                                  (uint32_t)Graphics::GetHeight());
            if (g_boot_panel_hz) FramePacer::SetPanelMilliHz(g_boot_panel_hz * 1000u, "cmdline");
            else if (igpu_mhz >= 24000 && igpu_mhz <= 400000) FramePacer::SetPanelMilliHz(igpu_mhz, "igpu");
            else if (Graphics::RefreshWasMeasured()) FramePacer::SetPanelMilliHz(detected_hz * 1000u, "vga");
            else FramePacer::SetPanelMilliHz(60000u, "default");
            if (g_boot_hz) FramePacer::SetCmdlineHz(g_boot_hz);
        }
        Graphics::SetRenderMode(Graphics::DOUBLE_BUFFER);

        // tell the pacer which rate every lit output really scans out. the
        // primary inherits the detected panel rate, the intel pipe b output
        // its lit edid timing, anything else the measured vga rate. the pacer
        // never caps above the highest of them. (satoru)
        {
            uint64_t dp_phys = 0; int dp_w = 0, dp_h = 0; uint32_t dp_pitch = 0;
            bool have_dp = IntelModeset::GetOutput(&dp_phys, &dp_w, &dp_h, &dp_pitch);
            uint32_t panel_mhz = FramePacer::PanelHz() * 1000u;
            for (int i = 0; i < Outputs::Total(); i++) {
                const Output* o = Outputs::Get(i);
                if (!o) continue;
                uint32_t r = (i == 0) ? panel_mhz
                            : (have_dp && o->phys == dp_phys) ? IntelModeset::CurrentMilliHz()
                            : detected_hz * 1000u;
                Outputs::SetRefreshMilliHz(i, r);
            }
            FramePacer::OutputsChanged();
        }

        // wallpaper damage-only (kurono.wpdamage=1, default off). the desktop
        // repaints only the damaged rows of its cached wallpaper; a crc
        // self-check turns it back off if the scanned and compositor buffers
        // ever disagree. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.wpdamage=1")) {
            Graphics::SetWallpaperDamageOnly(true);
            SerialLogger::Log("[wpdamage] wallpaper damage-only enabled (kurono.wpdamage=1)\r\n");
        }
        // damage scissor rendering (kurono.gfx.scissor=1, default off): the
        // compositor repaints only damaged regions, every draw confined to
        // them. kurono.gfx.scissorcheck=N compares the scanned and the back
        // buffer on N presents (default 24) and turns it off on a mismatch.
        // frame stats and the bench (kurono.gfxstats=1, kurono.gfxbench=1)
        // only log. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.gfx.scissor=1")) {
            int chk = 24;
            char cv[8];
            if (boot_get_value(boot_cmdline, "kurono.gfx.scissorcheck", cv, sizeof(cv))) {
                chk = 0;
                for (int k = 0; cv[k] >= '0' && cv[k] <= '9'; k++) chk = chk * 10 + (cv[k] - '0');
            }
            Graphics::SetScissorEnabled(true);
            Graphics::SetScissorCheckFrames(chk);
            Graphics::SetScissorDebug(boot_has_token(boot_cmdline, "kurono.gfx.scissordebug=1"));
            DesktopEnvironment::SetScissorGuard(boot_has_token(boot_cmdline, "kurono.gfx.scissorguard=1"));   // (satoru)
            if (boot_get_value(boot_cmdline, "kurono.gfx.scissorverify", cv, sizeof(cv))) {
                int ve = 0;
                for (int k = 0; cv[k] >= '0' && cv[k] <= '9'; k++) ve = ve * 10 + (cv[k] - '0');
                Graphics::SetScissorVerifyEvery(ve);
            }
            SerialLogger::Log("[scissor] damage scissor rendering enabled (kurono.gfx.scissor=1), check frames ");
            SerialLogger::LogDec(chk);
            SerialLogger::Log("\r\n");
        }
        // clipped text skips its hidden glyphs and blends inline (identical
        // pixels): with the scissor, or alone with kurono.gfx.fasttext=1. (satoru)
        if (boot_has_token(boot_cmdline, "kurono.gfx.scissor=1") || boot_has_token(boot_cmdline, "kurono.gfx.fasttext=1")) {
            Graphics::RegisterFontTarget();
            SerialLogger::Log("[scissor] fast clipped text on\r\n");
        }
        {
            int soak = 0; char sv[12];
            if (boot_get_value(boot_cmdline, "kurono.gfx.oracle.soak", sv, sizeof(sv)))
                for (int k = 0; sv[k] >= '0' && sv[k] <= '9'; k++) soak = soak * 10 + (sv[k] - '0');
            GfxOracle::Configure(boot_has_token(boot_cmdline, "kurono.gfx.oracle=1"), boot_has_token(boot_cmdline, "kurono.gfx.oracle.shots=1"), soak);
        }   // (satoru)
        GfxBench::Configure(boot_has_token(boot_cmdline, "kurono.gfxstats=1"),
                            boot_has_token(boot_cmdline, "kurono.gfxbench=1") &&
                            !KldDisplay::DisplayBenchRequested());

        // verify display works - write directly to framebuffer
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

    // animated boot (ui/boot_anim): drawn by an ap while the bsp keeps going, so
    // it costs the boot nothing. kurono.bootanim=0 or kurono.splash=slow keep the
    // old logo + bar below, kurono.bootanim=fast plays a quicker tail. (satoru)
    bool boot_anim_on = false;
    if (has_display) {
        int bam = 1;
        if (boot_has_token(boot_cmdline, "kurono.bootanim=0") || boot_has_token(boot_cmdline, "kurono.splash=slow"))
            bam = 0;
        else if (boot_has_token(boot_cmdline, "kurono.bootanim=fast"))
            bam = 2;
        {   // test hooks: kurono.bootanim.hold=MS (slow boot), kurono.bootanim.early=MS (fast boot) (satoru)
            char tv[12];
            int th = 0, te = 0;
            if (boot_get_value(boot_cmdline, "kurono.bootanim.hold", tv, sizeof(tv)))
                for (int k = 0; tv[k] >= '0' && tv[k] <= '9'; k++) th = th * 10 + (tv[k] - '0');
            if (boot_get_value(boot_cmdline, "kurono.bootanim.early", tv, sizeof(tv)))
                for (int k = 0; tv[k] >= '0' && tv[k] <= '9'; k++) te = te * 10 + (tv[k] - '0');
            BootAnim::SetTest(th, te);
        }
        if (boot_has_token(boot_cmdline, "kurono.bootbar=live")) BootAnim::SetLiveBar(true);   // (satoru)
        boot_anim_on = BootAnim::Start(bam);
    }
    if (has_display && !boot_anim_on) {
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
        // kurono.splash=slow restores the old ~1.1 s animation; the default is 5x shorter (satoru)
        const bool splash_slow = boot_has_token(boot_cmdline, "kurono.splash=slow");
        for (int step = 1; step <= 24; step++) {
            int fill_w = (step * bar_w) / 24;
            for (int px = 0; px < fill_w; px++) {
                uint32_t c = 0xFFD8D8D8;
                for (int py = 0; py < bar_h; py++)
                    Graphics::DrawPixel(bar_x + px, bar_y + py, c);
            }
            Graphics::SwapBuffers();
            Timer::WaitMs(splash_slow ? 40u : 8u);
        }

        // brief pause then smooth clear
        Timer::WaitMs(splash_slow ? 80u : 16u);
        Graphics::Clear(0xFF000000);
        Graphics::SwapBuffers();
        Timer::WaitMs(splash_slow ? 60u : 12u);
        SerialLogger::Log("Boot splash complete\r\n");
    }

    System::Initialize();
    if (boot_has_token(boot_cmdline, "kurono.wptest=1")) KernelProcesses::SetWpTest(1);
    BootAnim::Progress(12);   // loading bar: core system up (satoru)

    // embedded inter ui faces; a "font" boot module, if present, replaces the
    // regular face below. (satoru)
    if (_binary_inter_regular_otf_start && _binary_inter_regular_otf_end)
        FontTTF::Init(_binary_inter_regular_otf_start,
                      (int)(_binary_inter_regular_otf_end - _binary_inter_regular_otf_start));
    if (_binary_inter_semibold_otf_start && _binary_inter_semibold_otf_end)
        FontTTF::InitBold(_binary_inter_semibold_otf_start,
                          (int)(_binary_inter_semibold_otf_end - _binary_inter_semibold_otf_start));
    if (_binary_dejavu_mono_ttf_start && _binary_dejavu_mono_ttf_end)
        FontTTF::InitMono(_binary_dejavu_mono_ttf_start,
                          (int)(_binary_dejavu_mono_ttf_end - _binary_dejavu_mono_ttf_start));

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

    // ── KDF: kernel driver framework (hybrid-kernel ring-0+ sandbox) ────
    // bring up the guard-page driver framework BEFORE any driver inits, since
    // Installer::MountDataDisk() below runs NVMe::Init() (the first kdf-migrated
    // driver) during the kvfs mount. kdf only needs vmm+pmm, both already up.
    // the kinit crash-restart notifier is wired later (after KInit::Init). (satoru)
    // KDF::Init() / IRP::Init() / KExec::Init() moved up to just before display
    // init, so nvme + the boot log come up before the riskiest early steps. (satoru)
    // the irp executive + the kexec facade are part of the hybrid-kernel layer;
    // bring them up alongside kdf (they need only pmm/vmm). nvme registers as an
    // irp block device after it inits (in MountDataDisk). (satoru)

    // ── block layer + sata ──────────────────────────────────────────────
    // bring the block registry up before any storage driver so drivers have
    // somewhere to register, then probe ahci. until this landed, every disk path
    // in the tree went through NVMe:: directly, so a machine with a sata ssd or a
    // spinning disk enumerated ZERO disks - the gpt/mbr parser in installer.cpp
    // was always fine, it just had nothing but nvme to look at. needs kdf (for
    // MapMMIO) and pmm, both up by here. (satoru)
    BlockDev::Init();
    Ahci::Init();
    // scan partition tables at boot rather than waiting for the installer gui to
    // open. a rescue environment needs to know what disks and partitions exist
    // the moment it comes up - that is the whole job - and Rescan() is cheap
    // (one 4-sector read per disk plus one per partition probe). (satoru)
    Installer::Rescan();

    SerialLogger::Log("[KVFS] Init...\r\n");
    KVFS::Init();
    // kvfs is up now: let kdf mirror its driver log to /kurono/var/log/drivers.log. (satoru)
    KDF::EnableFileLog();
    // bring up + mount a persistent ext4 data disk (if attached) BEFORE restoring,
    // so kvfs.img survives a reboot on a normal boot - not just after an install.
    // Installer::Init() runs much later, so do the mount here ourselves. (satoru)
    // detect the nvme data disk - MountDataDisk runs NVMe::Init for us; its ext4
    // probe just fails harmlessly on our raw persistence store. (satoru)
    Installer::MountDataDisk();
    BootAnim::Progress(30);   // loading bar: data disk mounted (satoru)
    // with the guard on, find the KURONO partition (the only writable range)
    // and start mirroring the boot log into its KURONO.LOG. (satoru)
    if (!DiskLog::Ready() && NVMe::WriteGuardOn() && DiskLog::FindKuronoWindow()) DiskLog::Init();
    {   // say where (or why not) the KFS store lives, before LoadTree (satoru)
        char pd[160];
        PersistStore::Describe(pd, sizeof(pd));
        SerialLogger::Log("[persist] "); SerialLogger::Log(pd); SerialLogger::Log("\r\n");
    }
    // restore persistent kvfs state from the on-disk KFS volume if one is present;
    // otherwise the default tree from init() stays. the /usr/bin binary re-seeding
    // below re-fills the large entries KFS deliberately doesn't carry. (satoru)
    if (PersistStore::Available()) {
        uint64_t t0 = Timer::GetRealMs64();
        bool ok = PersistStore::LoadTree();
        uint64_t t1 = Timer::GetRealMs64();
        if (ok) {
            SerialLogger::Log("[KVFS] restored persistent state from KFS in ");
            SerialLogger::LogDec((int)(t1 - t0)); SerialLogger::Log(" ms\r\n");
        } else {
            SerialLogger::Log("[KVFS] no KFS volume to restore (fresh disk)\r\n");
        }
        // a restore replays user-data subtrees (/home, /etc, /root) into the live
        // tree; re-assert the canonical /kurono dirs + compat symlinks afterward
        // so the overlay is intact regardless of what the on-disk image carried. (satoru)
        KVFS::InstallCanonicalLayout();
        // seed the persisted-firefox probe here on the single-threaded init
        // path (the volume is already mounted from LoadTree), so the desktop's
        // firefox-icon sync can read the cached answer without ever touching
        // KFS/nvme from the gui process. (satoru)
        if (ok && PersistStore::HasPersistedFirefox())
            SerialLogger::Log("[persist] firefox install present on data disk\r\n");
        // NOTE: deliberately NO KFS::Unmount() here. freeing ~36 mb of kernel
        // heap in the middle of the fragile SMP bringup window regressed boot
        // reliability hard (6/6 no-window vs the ~50% baseline). the metadata
        // is instead released by RestoreApps()/SaveTree(), which run well after
        // bringup once firefox is actually using the persist volume - the ram
        // is still reclaimed, just not during bringup. (satoru)
    }
    // decide the window policy once, with the desktop up and the disk readable.
    // a big machine with a CLAUDE image keeps the old reservation (claude must
    // always start there even after firefox grew the heap); otherwise the
    // window is released and its pages are claimed on demand. (satoru)
    // opencode is the same kind of fixed-address image and needs the same window (satoru)
    bool have_claude = ClaudeHost::Installed() || OpenCodeHost::Installed();
    if (!have_claude && !DiskLog::Ready()) {
        // a plain bios boot has no guard, so DiskLog was never opened. the
        // partition lookup itself is read only: use it, without Init (which
        // would blank KURONO.LOG), so an installed CLAUDE still selects the
        // reserved window on such a boot. (satoru)
        if (DiskLog::FindKuronoWindow()) have_claude = ClaudeHost::ProbeInstalled() || OpenCodeHost::ProbeInstalled();
    }
    PMM::FinalizeImageWindow(have_claude);

    // kurono.wctest=1: prove the live-table write-combining helper on addresses
    // a high GOP framebuffer would use. qemu's display devices hand out their
    // legacy aperture below 4 gb, so >4 gb and >2 tb (the old extra_pd range)
    // are forced here and only the page-table entry is inspected. (satoru)
    if (boot_has_token(boot_cmdline, "kurono.wctest=1")) {
        const uint64_t hi4g = 0x100000000ULL;          // ram above 4 gb
        const uint64_t hi3t = 0x30040000000ULL;        // beyond the 2 tb boot map
        uint64_t c1 = KernelVMM::SetIdentityWriteCombining(hi4g, 2ULL << 20);
        bool h1 = false;
        uint64_t e1 = KernelVMM::QueryRawEntry(hi4g, &h1);
        SerialLogger::Log("[wctest] 4gb chunks=");
        SerialLogger::LogDec((uint32_t)c1);
        SerialLogger::Log(" entry=0x");
        SerialLogger::LogHex((uint32_t)(e1 >> 32));
        SerialLogger::LogHex((uint32_t)e1);
        SerialLogger::Log(" ps=");
        SerialLogger::LogDec(h1 ? 1 : 0);
        SerialLogger::Log(" pwt=");
        SerialLogger::LogDec((int)((e1 >> 3) & 1));
        SerialLogger::Log(" pcd=");
        SerialLogger::LogDec((int)((e1 >> 4) & 1));
        SerialLogger::Log(" pat12=");
        SerialLogger::LogDec((int)((e1 >> 12) & 1));
        SerialLogger::Log("\r\n");
        uint64_t c2 = KernelVMM::SetIdentityWriteCombining(hi3t, 2ULL << 20);
        bool h2 = false;
        uint64_t e2 = KernelVMM::QueryRawEntry(hi3t, &h2);
        SerialLogger::Log("[wctest] 3tb chunks=");
        SerialLogger::LogDec((uint32_t)c2);
        SerialLogger::Log(" entry=0x");
        SerialLogger::LogHex((uint32_t)(e2 >> 32));
        SerialLogger::LogHex((uint32_t)e2);
        SerialLogger::Log(" ps=");
        SerialLogger::LogDec(h2 ? 1 : 0);
        SerialLogger::Log(" pwt=");
        SerialLogger::LogDec((int)((e2 >> 3) & 1));
        SerialLogger::Log(" pcd=");
        SerialLogger::LogDec((int)((e2 >> 4) & 1));
        SerialLogger::Log(" pat12=");
        SerialLogger::LogDec((int)((e2 >> 12) & 1));
        SerialLogger::Log("\r\n");
    }
    // headless two-boot kfs persistence test (gated by cmdline kurono.kfstest):
    // boot 1 - no marker present after restore - writes a small user-data tree
    // under /home/user/.kfstest, times PersistStore::SaveTree() (the KFS format +
    // mirror + sync), logs the byte count + ms, then powers off. boot 2 - the same
    // disk image - the restore above brings the tree back, so the marker IS present;
    // we verify each file's content + the deep path, log PASS/FAIL + the LoadTree
    // ms, then power off. this exercises the whole KFS persist/restore loop with no
    // interactive shell, and the timing proves the multi-page-dma speed. (satoru)
    // headless storage benchmark (gated by cmdline kurono.kfsbench): runs the KFS
    // / nvme bench suite against the data disk + powers off. destructive, so it
    // must come BEFORE any real persistence work and only on the explicit token. (satoru)
    if (boot_kfsbench) {
        KfsBench::Run();
        SerialLogger::Log("[KFSBENCH] powering off\r\n");
        HAL::PowerOff();
    }
    // headless kmemx self-test (gated by cmdline kurono.kmemxtest): run the
    // lz4 + pool + metadata self-tests inline here (pmm + heap are up; no gui
    // needed), log PASS/FAIL to serial, and power off if kurono.kmemx.poweroff is
    // set. running inline (like kfsbench) makes the test mode-independent - it
    // does not rely on the graphical scheduler path that hosts the kproc runner,
    // so a headless text-mode ci boot exercises it deterministically. (satoru)
    if (boot_kmemx_test) {
        SerialLogger::Log("[KMEMX-TEST] begin (inline headless)\r\n");
        KMemXTest::RunAll();
        if (boot_kmemx_poweroff) {
            SerialLogger::Log("[KMEMX-TEST] powering off (kurono.kmemx.poweroff)\r\n");
            HAL::PowerOff();
        }
    }
    if (boot_kfstest) {
        SerialLogger::Log("[KFSTEST] begin\r\n");
        const char* marker = "/home/user/.kfstest/marker";
        const char* big    = "/home/user/.kfstest/big.bin";
        const char* deep   = "/etc/kfstest/deep/nested/file.txt";
        const char* MVAL   = "kurono-kfs-twoboot-v1";
        const char* DVAL   = "deep-nested-payload";
        if (KVFS::Exists(marker)) {
            // boot 2: the restore should have rebuilt these from the KFS volume. (satoru)
            bool pass = true;
            char buf[64]; buf[0] = 0;
            KVFS::ReadString(marker, buf, (int)sizeof(buf));
            if (!streq(buf, MVAL)) { pass = false; SerialLogger::Log("[KFSTEST] marker content mismatch\r\n"); }
            char dbuf[64]; dbuf[0] = 0;
            if (!KVFS::Exists(deep)) { pass = false; SerialLogger::Log("[KFSTEST] deep path missing\r\n"); }
            else { KVFS::ReadString(deep, dbuf, (int)sizeof(dbuf)); if (!streq(dbuf, DVAL)) { pass = false; SerialLogger::Log("[KFSTEST] deep content mismatch\r\n"); } }
            // the big file is a 256 KB pattern (1 + (i&0xff)); verify a few bytes. (satoru)
            int bsz = KVFS::GetFileSize(big);
            if (bsz != 256 * 1024) { pass = false; SerialLogger::Log("[KFSTEST] big size wrong: "); SerialLogger::LogDec(bsz); SerialLogger::Log("\r\n"); }
            else {
                uint8_t* vbuf = (uint8_t*)PMM::AllocBytes(256 * 1024);
                int got = vbuf ? KVFS::ReadFile(big, vbuf, 256 * 1024) : -1;
                bool content_ok = vbuf && (got == 256 * 1024);
                if (content_ok) for (int i = 0; i < 256 * 1024; i++) if (vbuf[i] != (uint8_t)(1 + (i & 0xff))) { content_ok = false; SerialLogger::Log("[KFSTEST] big content mismatch at "); SerialLogger::LogDec(i); SerialLogger::Log("\r\n"); break; }
                if (!content_ok) pass = false;
                if (vbuf) PMM::FreeBytes(vbuf, 256 * 1024);
            }
            SerialLogger::Log(pass ? "[KFSTEST] PASS: user data restored from KFS volume across reboot\r\n"
                                   : "[KFSTEST] FAIL: restored data did not match\r\n");
        } else {
            // boot 1: write the user-data tree, then time the KFS save. (satoru)
            KVFS::Mkdirs("/home/user/.kfstest");
            KVFS::Mkdirs("/etc/kfstest/deep/nested");
            KVFS::WriteString(marker, MVAL);
            KVFS::WriteString(deep, DVAL);
            uint8_t* pat = (uint8_t*)PMM::AllocBytes(256 * 1024);
            if (pat) {
                for (int i = 0; i < 256 * 1024; i++) pat[i] = (uint8_t)(1 + (i & 0xff));
                KVFS::WriteFile(big, pat, 256 * 1024);
                PMM::FreeBytes(pat, 256 * 1024);
            }
            SerialLogger::Log("[KFSTEST] boot1 wrote marker + deep + 256KB big.bin\r\n");
            if (PersistStore::Available()) {
                // snapshot the nvme write counters around SaveTree so we can show the
                // multi-page-dma payoff: how many real commands moved the bytes vs how
                // many the old 4kb/command (single-prp1) path would have needed. (satoru)
                uint64_t wc0 = NVMe::GetWriteCount(); uint64_t wb0 = NVMe::GetBytesWritten();
                uint64_t s0 = Timer::GetRealMs64();
                bool ok = PersistStore::SaveTree();
                uint64_t s1 = Timer::GetRealMs64();
                uint64_t cmds = NVMe::GetWriteCount() - wc0;
                uint64_t bytes = NVMe::GetBytesWritten() - wb0;
                uint64_t old_cmds = (bytes + 4095) / 4096;   // one page per command on the old path (satoru)
                SerialLogger::Log(ok ? "[KFSTEST] boot1 SaveTree OK in " : "[KFSTEST] boot1 SaveTree FAILED in ");
                SerialLogger::LogDec((int)(s1 - s0)); SerialLogger::Log(" ms\r\n");
                SerialLogger::Log("[KFSTEST] boot1 nvme write: "); SerialLogger::LogDec((int)bytes);
                SerialLogger::Log(" bytes in "); SerialLogger::LogDec((int)cmds);
                SerialLogger::Log(" multi-page cmds (old 4kb/cmd path: ");
                SerialLogger::LogDec((int)old_cmds); SerialLogger::Log(" cmds)\r\n");
                // layer 6: a SECOND save with no changes must be near-instant (the
                // incremental fingerprint matches -> skip the full reformat+rewrite). (satoru)
                uint64_t iw0 = NVMe::GetWriteCount();
                uint64_t i0 = Timer::GetRealMs64();
                bool iok = PersistStore::SaveTree();
                uint64_t i1 = Timer::GetRealMs64();
                uint64_t icmds = NVMe::GetWriteCount() - iw0;
                SerialLogger::Log(iok ? "[KFSTEST] boot1 incremental re-save (no change) in " : "[KFSTEST] boot1 incremental re-save FAILED in ");
                SerialLogger::LogDec((int)(i1 - i0)); SerialLogger::Log(" ms, ");
                SerialLogger::LogDec((int)icmds); SerialLogger::Log(" nvme write cmds\r\n");
                SerialLogger::Log("[KFSTEST] boot1 done; reboot with the same disk to verify\r\n");
            } else {
                SerialLogger::Log("[KFSTEST] FAIL: no nvme data disk -> cannot persist\r\n");
            }
        }
        SerialLogger::Log("[KFSTEST] end; powering off\r\n");
        HAL::PowerOff();
    }
    // bring up the usb host controller + hid interrupt polling (no-op if no xhci). (satoru)
    UsbAudio::EarlyInit();   // usb audio hooks must exist before the first device enumerates (satoru)
    SerialLogger::Log("[USB] Init...\r\n");
    USB::Init();
    if (boot_usbtest) USB::SelfTest();
    if (boot_usbmsdtest) USB::MsdSelfTest();
    RuntimeLog::SetHoldGuardAcrossKvfs(boot_has_token(boot_cmdline, "kurono.rtlogleaf=0"));   // (satoru)
    RuntimeLog::InitFilesystem();
    RuntimeLog::LogBoot("kvfs online");
    RuntimeLog::LogSystem("kernel", "runtime filesystem layout created");

    // recover a persisted crash minidump now that kvfs is up, log it to the crash
    // log, and surface a toast if the previous boot ended in a panic. (satoru)
    KVFS::Mkdirs("/home/user");
    if (KernelPanic::ScanCrashDumpAtBoot()) {
        RuntimeLog::LogCrash("recovered minidump from previous boot panic",
                             "see " KP_LOG_CRASH_DIR);
        // R9: name the stop code; Recovery's "Fix last issue" has the rest. (satoru)
        static char rec_msg[96];
        {
            char code[16];
            KScreen::PanicStopCode(KernelPanic::LastRecoveredStopCode(), code);
            int p = 0;
            const char* a = "Stop code ";
            while (*a) rec_msg[p++] = *a++;
            for (int i = 0; code[i]; i++) rec_msg[p++] = code[i];
            a = ". Details: Settings > System > Recovery.";
            while (*a && p < 94) rec_msg[p++] = *a++;
            rec_msg[p] = 0;
        }
        NotificationManager::Post("Kurono restarted after a problem", rec_msg,
                                  NotificationManager::ICON_WARNING, 8000);
    }

    // uiconfig must be initialized before any ui subsystem reads colors/sizes.
    // it writes /etc/kurono/ui.conf with defaults on first boot.
    SerialLogger::Log("[UIConfig] Init...\r\n");
    UIConfig::Init();
    // the saved external panel timing lives in ui.conf, which is only readable
    // after KVFS + the data disk are up; relight pipe b now (still before the
    // desktop starts). IntelModeset::Boot lit the default 60 hz. (satoru)
    if (IntelModeset::ApplySavedRate()) FramePacer::OutputsChanged();
    // seed the kss theme tokens from ui.conf (defaults to the black/grey palette). (satoru)
    KSS::Init();
    // build the kss stylesheet layer (named rules + transitions + keyframes) from
    // the theme; this is what the kj interpreter binds to for scripted styling. (satoru)
    KSS::Sheet::Init();

    // apply persisted accessibility settings to the runtime layers
    Graphics::SetColorFilter(UIConfig::Int("a11y.color_filter", 0));
    Graphics::SetHighContrast(UIConfig::Int("a11y.high_contrast", 0) != 0);
    Keyboard::SetStickyKeys(UIConfig::Int("a11y.sticky_keys", 0) != 0);
    Keyboard::SetSlowKeys(UIConfig::Int("a11y.slow_keys", 0) ? 250 : 0);
    Keyboard::SetBounceKeys(UIConfig::Int("a11y.bounce_keys", 0) ? 200 : 0);
    Keyboard::SetScreenReader(UIConfig::Int("a11y.screen_reader", 0) != 0);

    SerialLogger::Log("[Shell] Init...\r\n");
    KuronoShell::Init();
    KuronoShell shell_instance;   // trivial object - all methods are static

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

    SerialLogger::Log("[KEN] Init...\r\n");
    KEN::Init();

    // ksa isolation self-test (kurono.ksa.test): prove the main os has no
    // page-table mapping into the isolated prompt region and that the verdict
    // only crosses via the read-only channel. logs KSA-SELFTEST lines to
    // serial. headless-verifiable; does not gate normal boot. (satoru)
    if (boot_ksa_test) {
        SerialLogger::Log("[KSA] running boot self-test...\r\n");
        bool ksa_ok = KSA::SelfTest();
        SerialLogger::Log(ksa_ok ? "[KSA] self-test PASS\r\n" : "[KSA] self-test FAIL\r\n");
        bool pol_ok = KEN::PolicySelfTest();
        SerialLogger::Log(pol_ok ? "[KSA] policy self-test PASS\r\n" : "[KSA] policy self-test FAIL\r\n");
    }

    SerialLogger::Log("[PackageManager] Init...\r\n");
    PackageManager::Init();
    PackageManager::RegisterCommands(&shell_instance);

    SerialLogger::Log("[GpuDriverInstaller] Init...\r\n");
    GpuDriverInstaller::Init();

    SerialLogger::Log("[Python] Init mini interpreter...\r\n");
    PythonInterp::Init();
    PythonInterp::RegisterShellCommands(&shell_instance);

    // kj (kurono javascript): register the `kj`/`node` shell commands. (satoru)
    SerialLogger::Log("[KJ] Init interpreter...\r\n");
    KJ::Init();
    KJ::RegisterShellCommands(&shell_instance);

    SerialLogger::Log("[Userspace] Init ring-3 runtime...\r\n");
    LinuxSyscall::Init();
    Userspace::Init();
    // udf: the ring-3 user driver framework. brought up after the syscall path so
    // SYS_UDF_CALL can route into the kernel UDFProxy. (satoru)
    UDF::Init();
    KldBroker::Init();
    // kurono.kld.asgonetest=1: dead-container address space refusal self-test. (satoru)
    if (boot_has_token(boot_cmdline, "kurono.kld.asgonetest=1")) KldBroker::AsGoneSelfTest();
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
    if (EmbeddedUserprogs::FfmpegLinked()) {   // on-demand copies register on first use (satoru)
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFileStatic("/usr/bin/ffmpeg",
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
    // L2 mmap hole reuse test: `mmapreuse [t1|t2|t3|all]` from the shell. (satoru)
    if (EmbeddedUserprogs::HasMmapReuseTest()) {
        KVFS::Mkdirs("/usr/bin");
        // registered silently: the dev test must not add a line to the default
        // boot log (the L2 gate compares it byte for byte with main). (satoru)
        KVFS::WriteFile("/usr/bin/mmap_reuse_test",
                        EmbeddedUserprogs::MmapReuseTestData(),
                        EmbeddedUserprogs::MmapReuseTestSize());
    }
    // register the signal-delivery self-test so it can prove real syscall-boundary
    // signal delivery (rt_sigaction/procmask/sigreturn + kill). runs from the shell
    // or an [aptest]-style launch; prints "SIGTEST PASS" to com1 on success. (satoru)
    if (EmbeddedUserprogs::HasSignalTest()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/signal_test",
                        EmbeddedUserprogs::SignalTestData(),
                        EmbeddedUserprogs::SignalTestSize());
        SerialLogger::Log("[Userspace] /usr/bin/signal_test registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::SignalTestSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    // interruptible-sleep self-test (nanosleep/usleep/clock_nanosleep timing,
    // SIGALRM EINTR + rem, ignored alarm, EINVAL, and 3 concurrent threads). (satoru)
    if (EmbeddedUserprogs::HasSleepTest()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/sleeptest",
                        EmbeddedUserprogs::SleepTestData(),
                        EmbeddedUserprogs::SleepTestSize());
        SerialLogger::Log("[Userspace] /usr/bin/sleeptest registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::SleepTestSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    // close-on-exec + unix socket table self-test, run by `fdtest`. (satoru)
    if (EmbeddedUserprogs::HasFdTest()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/fdtest", EmbeddedUserprogs::FdTestData(), EmbeddedUserprogs::FdTestSize());
        // a linux execve of /usr/bin/x resolves to the kvfs /system/bin/x. (satoru)
        KVFS::Mkdirs("/system/bin");
        KVFS::WriteFile("/system/bin/fdtest", EmbeddedUserprogs::FdTestData(), EmbeddedUserprogs::FdTestSize());
    }
    // register the KLD driver container. this is an UNMODIFIED upstream linux
    // e1000e driver linked against the lx_emul shim, running as an ordinary
    // Kurono userspace process. the kernel links none of it. (satoru)
    if (EmbeddedUserprogs::KldE1000eLinked()) {   // on-demand copies register on first use (satoru)
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/kld-e1000e",
                        EmbeddedUserprogs::KldE1000eData(),
                        EmbeddedUserprogs::KldE1000eSize());
        SerialLogger::Log("[Userspace] /usr/bin/kld-e1000e registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::KldE1000eSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    // ring-3 audio daemon binary (only run with kurono.r3audio=1). (satoru)
    if (EmbeddedUserprogs::HasAudiod()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/audiod",
                        EmbeddedUserprogs::AudiodData(),
                        EmbeddedUserprogs::AudiodSize());
        SerialLogger::Log("[Userspace] /usr/bin/audiod registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::AudiodSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    // audio control tool: kaudio restart|stop|status writes the command to
    // /dev/kaudio, the audio-named kshell channel. /system/bin is the PATH
    // entry a ring-3 shell and claude's bash already have. (satoru)
    if (EmbeddedUserprogs::HasKaudio()) {
        KVFS::Mkdirs("/system/bin");
        KVFS::WriteFile("/system/bin/kaudio",
                        EmbeddedUserprogs::KaudioData(),
                        EmbeddedUserprogs::KaudioSize());
        KVFS::Chmod("/system/bin/kaudio", 0755);
        SerialLogger::Log("[Userspace] /system/bin/kaudio registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::KaudioSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    // register the user-pointer validation regression test. kurono.uatest=1 runs
    // it headless; it passes kernel addresses into syscalls and PASSes only if
    // they are rejected with -EFAULT. (satoru)
    if (EmbeddedUserprogs::HasUaTest()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/uatest",
                        EmbeddedUserprogs::UaTestData(),
                        EmbeddedUserprogs::UaTestSize());
        SerialLogger::Log("[Userspace] /usr/bin/uatest registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::UaTestSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    if (EmbeddedUserprogs::HasKenTest()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/kentest",
                        EmbeddedUserprogs::KenTestData(),
                        EmbeddedUserprogs::KenTestSize());
        SerialLogger::Log("[Userspace] /usr/bin/kentest registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::KenTestSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    // register the parallel tree-walk regression (kurono.findtest=1) at both the
    // vfs and the linux-visible path, so the runner and a ring-3 shell can run it. (satoru)
    if (EmbeddedUserprogs::HasFindtest()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/findtest",
                        EmbeddedUserprogs::FindtestData(),
                        EmbeddedUserprogs::FindtestSize());
        KVFS::Mkdirs("/system/bin");
        KVFS::WriteFile("/system/bin/findtest",
                        EmbeddedUserprogs::FindtestData(),
                        EmbeddedUserprogs::FindtestSize());
        SerialLogger::Log("[Userspace] /usr/bin/findtest registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::FindtestSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    // register the census churn workload (kurono.churn=1). (satoru)
    if (_binary_churn_elf_start && _binary_churn_elf_end &&
        +_binary_churn_elf_end > +_binary_churn_elf_start) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/churn", _binary_churn_elf_start,
                        (uint32_t)(_binary_churn_elf_end - _binary_churn_elf_start));
        // the linux layer resolves /usr/bin/X to /system/bin/X, so the workload's
        // own execs look there. (satoru)
        KVFS::Mkdirs("/system/bin");
        KVFS::WriteFile("/system/bin/churn", _binary_churn_elf_start,
                        (uint32_t)(_binary_churn_elf_end - _binary_churn_elf_start));
        if (EmbeddedUserprogs::HasDyntest())
            KVFS::WriteFile("/system/bin/dyntest", EmbeddedUserprogs::DyntestData(),
                            EmbeddedUserprogs::DyntestSize());
    }
    // register the ring-3 memory fill test (kurono.memtest=1). (satoru)
    if (_binary_memtest_elf_start && _binary_memtest_elf_end &&
        +_binary_memtest_elf_end > +_binary_memtest_elf_start) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/memtest", _binary_memtest_elf_start,
                        (uint32_t)(_binary_memtest_elf_end - _binary_memtest_elf_start));
    }
    // register the /dev/kshell gate. kurono.kshtest=1 runs it headless; it is
    // the regression for the rescue agent's command channel, and specifically
    // for the kernel-side whitelist that must refuse destructive commands.
    // (satoru)
    if (EmbeddedUserprogs::HasKshTest()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/kshtest",
                        EmbeddedUserprogs::KshTestData(),
                        EmbeddedUserprogs::KshTestSize());
        SerialLogger::Log("[Userspace] /usr/bin/kshtest registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::KshTestSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    // register the ai recovery client + its optional baked api key.
    // kurono.dsai=1 runs it headless against a fixed prompt. the key is only
    // present when the image was built with DSAI_KEY_FILE pointing at a real
    // file; NEVER log its contents, only its length. (satoru)
    if (EmbeddedUserprogs::HasBusybox()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/busybox",
                        EmbeddedUserprogs::BusyboxData(),
                        EmbeddedUserprogs::BusyboxSize());
        SerialLogger::Log("[Userspace] /usr/bin/busybox registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::BusyboxSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    if (EmbeddedUserprogs::HasDsai()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/dsai",
                        EmbeddedUserprogs::DsaiData(),
                        EmbeddedUserprogs::DsaiSize());
        SerialLogger::Log("[Userspace] /usr/bin/dsai registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::DsaiSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    // kpkg's https downloader (kpkg install claude). (satoru)
    if (EmbeddedUserprogs::HasKfetch()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/kfetch",
                        EmbeddedUserprogs::KfetchData(),
                        EmbeddedUserprogs::KfetchSize());
        SerialLogger::Log("[Userspace] /usr/bin/kfetch registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::KfetchSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    if (EmbeddedUserprogs::HasDsaiKey()) {
        // guest /etc/X translates to kvfs /system/etc/X (see the path table
        // in linux_syscall.cpp), so writing plain /etc leaves it invisible
        // to ring 3 - which is exactly how the first boot failed. (satoru)
        KVFS::Mkdirs("/system/etc");
        KVFS::WriteFile("/system/etc/deepseek.key",
                        EmbeddedUserprogs::DsaiKeyData(),
                        EmbeddedUserprogs::DsaiKeySize());
        SerialLogger::Log("[Userspace] /etc/deepseek.key installed (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::DsaiKeySize());
        SerialLogger::Log(" bytes)\r\n");
    }
    // unpack the recovery toolbox (ntfsprogs, e2fsprogs, dosfstools, testdisk,
    // photorec, chntpw, busybox) into /system/bin, which is what a linux
    // program sees as /usr/bin and /bin. (satoru)
    if (EmbeddedUserprogs::HasRecoveryTools()) {
        KVFS::Mkdirs("/system/bin");
        bool ok = PackageManager::ExtractTar(
            (const char*)EmbeddedUserprogs::RecoveryToolsData(),
            (int)EmbeddedUserprogs::RecoveryToolsSize(), "/system/bin");
        KAsset::Release("RTOOLS  TAR");   // unpacked: drop the 10 mb tar (no-op if linked in) (satoru)
        SerialLogger::Log("[Userspace] recovery toolbox ");
        SerialLogger::Log(ok ? "unpacked (" : "UNPACK FAILED (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::RecoveryToolsSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    // the recovery toolbox is an on-demand asset now (RTOOLS.TAR on the KURONO
    // partition). without it /system/bin/busybox would be missing, and claude
    // code's bash shim plus every linux sh -c rely on it, so fall back to the
    // built-in busybox. (satoru)
    // item 11: src/userprogs/busybox.elf was a static ntfsfix build, so without
    // the toolbox asset every `sh -c` (and claude code's bash shim) ran ntfsfix,
    // which looked like an exec of a missing path running an unrelated binary.
    // only install the fallback if the blob really is busybox. (satoru)
    auto blob_is_busybox = [](const uint8_t* d, uint32_t n) -> bool {
        static const char tag[] = "BusyBox v";
        const uint32_t tl = sizeof(tag) - 1;
        for (uint32_t i = 0; d && i + tl <= n; i++) {
            uint32_t k = 0;
            while (k < tl && d[i + k] == (uint8_t)tag[k]) k++;
            if (k == tl) return true;
        }
        return false;
    };
    if (!KVFS::Exists("/system/bin/busybox") && EmbeddedUserprogs::HasBusybox() &&
        !blob_is_busybox(EmbeddedUserprogs::BusyboxData(), EmbeddedUserprogs::BusyboxSize())) {
        SerialLogger::Log("[Userspace] built-in busybox.elf is not busybox (no \"BusyBox v\" tag): not installed as /system/bin/busybox\r\n");
    } else if (!KVFS::Exists("/system/bin/busybox") && EmbeddedUserprogs::HasBusybox()) {
        KVFS::Mkdirs("/system/bin");
        KVFS::WriteFile("/system/bin/busybox", EmbeddedUserprogs::BusyboxData(), EmbeddedUserprogs::BusyboxSize());
        KVFS::Chmod("/system/bin/busybox", 0755);
        SerialLogger::Log("[Userspace] no recovery toolbox: built-in busybox at /system/bin/busybox\r\n");
    }
    // every busybox applet plus the kurono built-ins (ksys/kdrv/kinit/audio...)
    // become names in /system/bin, which linux sees as /bin and /usr/bin. (satoru)
    AgentTools::Install();
    ClaudeHost::CacheEnvFile();   // CLAUDE.ENV for claude started from any terminal (satoru)
    OpenCodeHost::CacheEnvFile();   // OPENCODE.ENV provider keys (satoru)
    { extern bool g_vfork_fix; if (boot_has_token(boot_cmdline, "kurono.vforkfix=0")) g_vfork_fix = false; }   // (satoru)
    { extern bool g_exit_wait; if (boot_has_token(boot_cmdline, "kurono.exitwait=0")) g_exit_wait = false; }   // (satoru)
    { extern bool g_futex_sig; if (boot_has_token(boot_cmdline, "kurono.futexsig=0")) g_futex_sig = false; }   // (satoru)
    { extern bool g_zcopy_einval; if (boot_has_token(boot_cmdline, "kurono.zcopy=old")) g_zcopy_einval = false; }   // (satoru)
    { extern bool g_symlink_old; if (boot_has_token(boot_cmdline, "kurono.symlinks=old")) g_symlink_old = true; }   // (satoru)
    { extern bool g_getdents_nodots; if (boot_has_token(boot_cmdline, "kurono.getdents=nodots")) g_getdents_nodots = true; }   // (satoru)
    { extern bool g_sigchld; if (boot_has_token(boot_cmdline, "kurono.sigchld=0")) g_sigchld = false; }   // (satoru)
    { extern bool g_sigsus_yield; if (boot_has_token(boot_cmdline, "kurono.sigsusyield=0")) g_sigsus_yield = false; }   // (satoru)
    { extern bool g_pipe_block; if (boot_has_token(boot_cmdline, "kurono.pipeblock=0")) g_pipe_block = false; }   // (satoru)
    { extern bool g_pgrp_zombies; if (boot_has_token(boot_cmdline, "kurono.pgrpzombie=0")) g_pgrp_zombies = false; }   // (satoru)
    { extern bool g_pid_mono; if (boot_has_token(boot_cmdline, "kurono.pidmono=0")) g_pid_mono = false; }   // (satoru)
    { extern bool g_exit_pending_sig; if (boot_has_token(boot_cmdline, "kurono.exitsig=0")) g_exit_pending_sig = false; }   // (satoru)
    {   // kurono.vm.pt=BB:DD.F[,...]: pci functions kept for vm kurono guests (satoru)
        char pt[64];
        if (boot_get_value(boot_cmdline, "kurono.vm.pt", pt, sizeof(pt))) PCIPassthrough::ParseReserveList(pt);
    }
    {   // kurono.dbgwatch=<hex va>[,<hex va>]: hardware write watchpoints (satoru)
        char dw[48];
        if (boot_get_value(boot_cmdline, "kurono.dbgwatch", dw, sizeof(dw))) {
            uint64_t v[2] = { 0, 0 }; int k = 0;
            for (int i = 0; dw[i] && k < 2; i++) {
                char c = dw[i];
                if (c == ',') { k++; continue; }
                if (c == 'x' || c == 'X') { v[k] = 0; continue; }
                int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                if (d >= 0) v[k] = v[k] * 16 + (uint64_t)d;
            }
            DbgWatch::Arm(v[0], v[1]);
        }
    }
    // (see irqlat_runner_entry above for the launcher.)
    // register the kld network a/b bench (kurono.kld.bench=1). (satoru)
    if (EmbeddedUserprogs::HasKldBench()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/kldnetbench", EmbeddedUserprogs::KldBenchData(), EmbeddedUserprogs::KldBenchSize());
    }
    // register the cpu scaling probe (kurono.smpscale). (satoru)
    if (EmbeddedUserprogs::HasSmpScale()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/smpscale", EmbeddedUserprogs::SmpScaleData(), EmbeddedUserprogs::SmpScaleSize());
    }
    // register the userspace wake-latency probe. kurono.irqlat runs it headless
    // and it prints [irqlat] lines to com1 - the KLD phase-0 instrument. (satoru)
    if (EmbeddedUserprogs::HasIrqLat()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/irqlat",
                        EmbeddedUserprogs::IrqLatData(),
                        EmbeddedUserprogs::IrqLatSize());
        SerialLogger::Log("[Userspace] /usr/bin/irqlat registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::IrqLatSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    // register the posix_spawn static-elf64 repro. kurono.spawntest=1 runs it
    // headless; it prints [spawntest] lines to com1. (satoru)
    if (EmbeddedUserprogs::HasSpawnTest()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/spawntest",
                        EmbeddedUserprogs::SpawnTestData(),
                        EmbeddedUserprogs::SpawnTestSize());
        SerialLogger::Log("[Userspace] /usr/bin/spawntest registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::SpawnTestSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    // register the dynamic musl pie + its libc.so. this is the first real
    // exercise of ld-kurono's dynamic-load path (load a pie at the 64tb aslr
    // base, recurse into DT_NEEDED, relocate, run). the .so lands in a default
    // ld-kurono search path so the pie's DT_NEEDED (libc.musl-x86_64.so.1)
    // resolves. `dyntest` runs it from the shell. (satoru)
    if (EmbeddedUserprogs::HasMuslLibc()) {
        KAsset::Deliver("MUSLLIBC SO ", _binary_musl_libc_so_start, _binary_musl_libc_so_end);
        KVFS::Mkdirs("/system/lib");
        KVFS::WriteFileStatic("/system/lib/libc.musl-x86_64.so.1",
                        EmbeddedUserprogs::MuslLibcData(),
                        EmbeddedUserprogs::MuslLibcSize());
        // also seed it at the PT_INTERP path so ld-kurono resolves the exe's
        // program interpreter (/lib/ld-musl-x86_64.so.1) verbatim - musl's libc
        // IS the dynamic linker. (satoru)
        KVFS::Mkdirs("/lib");
        KVFS::WriteFileStatic("/lib/ld-musl-x86_64.so.1",
                        EmbeddedUserprogs::MuslLibcData(),
                        EmbeddedUserprogs::MuslLibcSize());
        SerialLogger::Log("[Userspace] musl libc.so registered at /system/lib + /lib (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::MuslLibcSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    // the musl c++ runtime (alpine's libstdc++ + libgcc_s, the same files the
    // firefox package ships) for musl programs that link it dynamically, e.g.
    // opencode. root files LIBSTDC.SO and LIBGCCS.SO on the KURONO partition;
    // absent files are a no-op. (satoru)
    {
        bool a = KAsset::EnsureVfsFile("/system/lib/libstdc++.so.6", "LIBSTDC SO ", nullptr, nullptr);
        bool b = KAsset::EnsureVfsFile("/system/lib/libgcc_s.so.1", "LIBGCCS SO ", nullptr, nullptr);
        if (a || b) {
            SerialLogger::Log("[Userspace] musl c++ runtime: libstdc++=");
            SerialLogger::LogDec(a ? 1 : 0);
            SerialLogger::Log(" libgcc_s=");
            SerialLogger::LogDec(b ? 1 : 0);
            SerialLogger::Log("\r\n");
        }
    }
    if (EmbeddedUserprogs::HasLibfoo()) {
        // dyntest's extra dependency. seeded in a default musl search dir
        // (LD_LIBRARY_PATH=/system/lib, set in the env ExecPIE passes) so musl's
        // linker resolves + file-backed-mmaps it. (satoru)
        KVFS::Mkdirs("/system/lib");
        KVFS::WriteFile("/system/lib/libfoo.so",
                        EmbeddedUserprogs::LibfooData(),
                        EmbeddedUserprogs::LibfooSize());
        SerialLogger::Log("[Userspace] /system/lib/libfoo.so registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::LibfooSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    if (EmbeddedUserprogs::HasDyntest()) {
        KVFS::Mkdirs("/usr/bin");
        KVFS::WriteFile("/usr/bin/dyntest",
                        EmbeddedUserprogs::DyntestData(),
                        EmbeddedUserprogs::DyntestSize());
        SerialLogger::Log("[Userspace] /usr/bin/dyntest registered (");
        SerialLogger::LogDec((int)EmbeddedUserprogs::DyntestSize());
        SerialLogger::Log(" bytes)\r\n");
    }
    if (EmbeddedUserprogs::KpythonLinked()) {   // on-demand copies register on first use (satoru)
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
        // ship a kj-scripted animation example: a script that eases the desktop
        // accent via the kss host bindings, then toasts. runnable from the shell
        // (`kj /kurono/scripts/accent_anim.kj`) and by the kurono.kjdemo boot hook.
        // the "window" rule's accent feeds the live window borders/titlebar, so
        // running this animates the real on-screen accent. (satoru)
        KVFS::Mkdirs("/kurono/scripts");
        const char* accent_kj =
            "// accent_anim.kj - drive a real on-screen animation from kj. (satoru)\n"
            "// ease the live desktop accent (window borders + titlebar) to a warm\n"
            "// red over 600ms, then post a toast. re-run to ease it back.\n"
            "kss.transition('window', 'accent', 600, 'outcubic');\n"
            "kss.set('window', 'accent', 4292032130);   // 0xFFE74C42\n"
            "ui.notify('KJ animation', 'accent eased by a kj script');\n"
            "console.log('accent_anim: transition fired');\n";
        uint32_t akj_len = 0; while (accent_kj[akj_len]) ++akj_len;
        KVFS::WriteFile("/kurono/scripts/accent_anim.kj", (const uint8_t*)accent_kj, akj_len);
        SerialLogger::Log("[KJ] /kurono/scripts/accent_anim.kj shipped\r\n");
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
            int rc = Userspace::RunProcessWithArgs(proc, nullptr, nullptr, 1000, 1000);
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

            KAsset::EnsureVfsFile("/usr/bin/kpython", "KPYTHON ELF", _binary_kpython_elf_bin_start, _binary_kpython_elf_bin_end);   // (satoru)
            Process* proc = ElfLoader::LoadELF64FromVFS("/usr/bin/kpython", "kpython");
            if (!proc) { sappend("kpy: load failed\n"); return p; }
            int rc = Userspace::RunProcessWithArgs(proc, uargv, nullptr, 1000, 1000);
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
    // NOTE: boot-time smoke tests temporarily disabled - the post-exit
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
    // NOTE: temporarily disabled in boot path - kpython works on UEFI but
    // crashes here on BIOS Multiboot2 due to a memory-layout collision
    // (kpython uses an address that overlaps kernel .rodata under MB2).
    // Users can still launch kpy interactively from the shell.
    if (false && EmbeddedUserprogs::HasKpython()) {
        SerialLogger::Log("[Userspace] Smoke test: running kpython /usr/share/hello.py ...\r\n");
        KAsset::EnsureVfsFile("/usr/bin/kpython", "KPYTHON ELF", _binary_kpython_elf_bin_start, _binary_kpython_elf_bin_end);   // (satoru)
        Process* kp = ElfLoader::LoadELF64FromVFS("/usr/bin/kpython", "kpython");
        if (!kp) {
            SerialLogger::Log("[Userspace] SMOKE TEST: kpython load failed\r\n");
        } else {
            const char* kargv[] = { "kpython", "/usr/share/hello.py", nullptr };
            int rc = Userspace::RunProcessWithArgs(kp, kargv, nullptr, 1000, 1000);
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
    // diagnostics only - gated off so normal boot reaches the desktop. flip
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
        KAsset::EnsureVfsFile("/usr/bin/ffmpeg", "FFMPEG  ELF", _binary_ffmpeg_elf_start, _binary_ffmpeg_elf_end);   // (satoru)
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
        KAsset::EnsureVfsFile("/usr/bin/ffmpeg", "FFMPEG  ELF", _binary_ffmpeg_elf_start, _binary_ffmpeg_elf_end);   // (satoru)
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

    // dynamic-pie smoke test (gated by cmdline kurono.dyntest): the FIRST real
    // exercise of ld-kurono's dynamic-load path. loading /usr/bin/dyntest (a
    // musl pie, PT_INTERP present) routes through ld-kurono: pick a 64tb aslr
    // base, map the pie, recurse into DT_NEEDED (libc.musl-x86_64.so.1 from
    // /system/lib), apply relocations, set up tls + the sysv auxv stack, and
    // run. it prints DYNOK on success. DYNTEST_* markers let a headless qemu
    // boot verify it. proving this is the prerequisite for running firefox
    // (itself a musl pie + an 80-library closure). (satoru)
    if (boot_dyntest_test && EmbeddedUserprogs::HasDyntest()) {
        SerialLogger::Log("[dyntest] PMM free MB=");
        SerialLogger::LogDec((int)(PMM::GetFreeMemory() / (1024 * 1024)));
        SerialLogger::Log("\r\nDYNTEST_BEGIN\r\n");
        Process* dp = ElfLoader::LoadELF64FromVFS("/usr/bin/dyntest", "dyntest");
        if (!dp) {
            SerialLogger::Log("DYNTEST_END rc=load_failed\r\n");
        } else {
            LinuxSyscall::ClearConsoleOutput();
            const char* av[] = { "dyntest", nullptr };
            const char* ev[] = { nullptr };
            int rc = Userspace::RunProcessWithArgs(dp, av, ev);
            char buf[512]; int n;
            while ((n = LinuxSyscall::ReadConsoleOutput(buf, (int)sizeof(buf) - 1)) > 0) {
                buf[n] = 0; SerialLogger::Log(buf);
            }
            SerialLogger::Log("\r\nDYNTEST_END rc=");
            SerialLogger::LogDec(rc);
            SerialLogger::Log("\r\n");
        }
    }

    SerialLogger::Log("[Installer] Init...\r\n");
    Installer::Init();
    Installer::RegisterShellCommands(&shell_instance);
    InstallerService::RegisterShellCommands(&shell_instance);

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

    // ── kinit: service + init manager ──────────────────────────────────
    // register the service model + shell commands now (registration only; no
    // kernel-processes are spawned until KInit::Boot(), which runs after the
    // canonical processes are up so its supervisors/monitor join the scheduler).
    // (satoru)
    SerialLogger::Log("[kinit] Init...\r\n");
    // arm the bounded kinit self-test services before Init registers them. (satoru)
    if (boot_kinit_test) KInit::EnableTestServices(KInit::KTEST_ALL);
    KInit::Init();
    // wire the kdf -> kinit crash-restart bridge now that kinit's service model
    // exists: a kdf guard-page fault calls ReportCrash -> this notifier -> kinit
    // looks up the matching kdf-driver service and runs its restart/backoff
    // policy (which re-inits the driver via kdf). (satoru)
    KDF::SetCrashNotifier(KInit::NotifyDriverCrash);
    // adopt the kdf-migrated drivers as supervised kinit units so a guard-page
    // crash drives a restart. nvme was already inited during the data-disk mount,
    // so it is adopted as already-running (kinit won't re-init it; only a crash
    // restarts it). it sits at the kernel target (storage is foundational). (satoru)
    KInit::RegisterKdfDriver("nvme", &NVMe::KdfInit, KInit::KTGT_KERNEL,
                             /*critical=*/false, /*already_running=*/NVMe::IsDetected());
    // register the kdf crash-recovery test driver (kurono.kdf.test) as a kinit
    // unit BEFORE KInit::Boot so it is started at its target; the runner spawned
    // after Boot faults it and verifies recovery. (satoru)
    if (boot_kdf_test) KDF::RegisterCrashTestDriver();
    KDF::RegisterShellCommands(&shell_instance);   // `hwfw` status command (satoru)
    Igc::RegisterShellCommands();                  // `nic` status/restart (satoru)
    KTools::RegisterShellCommands(&shell_instance);  // `kdrv` + `ksys` (satoru)
    KInit::RegisterShellCommands(&shell_instance);
    KMemX::RegisterShellCommands(&shell_instance);  // `kmemx` engine control (satoru)
    KpkgDaemon::RegisterShellCommands(&shell_instance);
    KUpdate::RegisterShellCommands(&shell_instance);
    KSecurity::RegisterShellCommands(&shell_instance);

    // ── systemd compatibility shim ──────────────────────────────────────
    // create the /run/systemd presence tree (so linux apps' sd_booted() probes
    // pass) and register the systemctl/journalctl/loginctl shims, which translate
    // onto kinit. the org.freedesktop.systemd1 d-bus bridge lives in
    // dbus_server.cpp and is already wired. (satoru)
    SerialLogger::Log("[systemd-compat] Init...\r\n");
    SystemdCompat::InitRuntime();
    SystemdCompat::RegisterShellCommands(&shell_instance);

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
    if (EmbeddedMedia::VideosLinked() && EmbeddedMedia::HasDenjiMP4()) {
        KVFS::WriteFileStatic("/home/user/Documents/denji.mp4",
                        EmbeddedMedia::DenjiMP4Data(),
                        EmbeddedMedia::DenjiMP4Size());
        SerialLogger::Log("[KVFS] Embedded denji.mp4: ");
        SerialLogger::LogDec(EmbeddedMedia::DenjiMP4Size());
        SerialLogger::Log(" bytes\r\n");
    } else {
        KVFS::WriteString("/home/user/Documents/denji.mp4",
                          "[MP4 stub - asset not embedded]");
    }
    if (EmbeddedMedia::VideosLinked() && EmbeddedMedia::HasDenjiKVID()) {
        KVFS::WriteFileStatic("/home/user/Videos/denji.kvid",
                        EmbeddedMedia::DenjiKVIDData(),
                        EmbeddedMedia::DenjiKVIDSize());
        SerialLogger::Log("[KVFS] Embedded denji.kvid: ");
        SerialLogger::LogDec(EmbeddedMedia::DenjiKVIDSize());
        SerialLogger::Log(" bytes\r\n");
    }
    // test hook (kurono.media.seed=1): the ring-3 media player only sees /home, so copy the
    // on-demand demo clips from the KURONO partition into /home/user/Videos. (satoru)
    if (boot_media_seed && !EmbeddedMedia::VideosLinked()) {
        if (EmbeddedMedia::HasDenjiKVID())
            KVFS::WriteFileStatic("/home/user/Videos/denji.kvid", EmbeddedMedia::DenjiKVIDData(), EmbeddedMedia::DenjiKVIDSize());
        if (EmbeddedMedia::HasSsstikKVID())
            KVFS::WriteFileStatic("/home/user/Videos/ssstik.kvid", EmbeddedMedia::SsstikKVIDData(), EmbeddedMedia::SsstikKVIDSize());
        SerialLogger::Log("[KVFS] media seed: demo clips copied into /home/user/Videos\r\n");
    }
    // user-imported video: original mp4 + playable kvid (satoru)
    if (EmbeddedMedia::VideosLinked() && EmbeddedMedia::HasSsstikMP4()) {
        KVFS::WriteFileStatic("/home/user/Videos/ssstik.mp4",
                        EmbeddedMedia::SsstikMP4Data(),
                        EmbeddedMedia::SsstikMP4Size());
        SerialLogger::Log("[KVFS] Imported ssstik.mp4: ");
        SerialLogger::LogDec(EmbeddedMedia::SsstikMP4Size());
        SerialLogger::Log(" bytes\r\n");
    }
    if (EmbeddedMedia::VideosLinked() && EmbeddedMedia::HasSsstikKVID()) {
        KVFS::WriteFileStatic("/home/user/Videos/ssstik.kvid",
                        EmbeddedMedia::SsstikKVIDData(),
                        EmbeddedMedia::SsstikKVIDSize());
        SerialLogger::Log("[KVFS] Imported ssstik.kvid: ");
        SerialLogger::LogDec(EmbeddedMedia::SsstikKVIDSize());
        SerialLogger::Log(" bytes\r\n");
    }
    KVFS::WriteString("/home/user/Music/startup.wav", "[WAV PCM 22050Hz 16-bit stereo 0:05]");
    KVFS::WriteString("/home/user/Music/notification.wav", "[WAV PCM 22050Hz 16-bit mono 0:02]");
    // kcl sample scripts - kcl v2 syntax (set x = expr, for i in a..b). (satoru)
    KVFS::WriteString("/home/user/hello.kcl", "# kcl script\nprint(\"Hello from Kurono!\")\nset x = 42\nprint(\"x =\", x)\n");
    KVFS::WriteString("/home/user/math.kcl", "# math demo\nset a = 16\nprint(\"sqrt(16) =\", sqrt(a))\nprint(\"random =\", rand(100))\n");
    KVFS::WriteString("/home/user/loop.kcl", "# loop demo\nset sum = 0\nfor i in 1..10 do\n  set sum = sum + i\nend\nprint(\"Sum 1..10 =\", sum)\n");
    KVFS::WriteString("/home/user/fib.kcl", "# fibonacci\nfunc fib(n)\n  if n < 2 then\n    return n\n  end\n  return fib(n-1) + fib(n-2)\nend\nfor i in 0..10 do\n  print(fib(i))\nend\n");
    // Pre-populate more visible directories and files for the File Manager
    KVFS::Mkdirs("/home/user/Desktop");
    KVFS::Mkdirs("/home/user/Pictures");
    KVFS::Mkdirs("/home/user/Downloads");
    KVFS::Mkdirs("/home/user/Videos");
    KVFS::Mkdirs("/home/user/Projects");
    KVFS::Mkdirs("/home/user/Projects/kurono");
    KVFS::WriteString("/home/user/Desktop/README.txt", "Your Kurono desktop.\n\nApps can be launched from the Start Menu (bottom-left).\nRight-click the desktop for wallpaper & display settings.\n");
    KVFS::WriteString("/home/user/Pictures/screenshot.bmp", "[BMP 1024x768 24-bit placeholder]");
    KVFS::WriteString("/home/user/Projects/kurono/main.kcl", "# kurono project\nset project_name = \"MyApp\"\nprint(\"Building\", project_name, \"...\")\nprint(\"Done!\")\n");
    // the banner only for an interactive shell: claude code sources ~/.bashrc
    // for its tool shells, and the line ended up in every command's output. (satoru)
    KVFS::WriteString("/home/user/.bashrc", "alias ls='ls -l'\nalias ll='ls -la'\nalias cls='clear'\ncase $- in *i*) echo \"Welcome back to Kurono!\";; esac\n");
    KVFS::WriteString("/home/user/.profile", "export PATH=/usr/bin:/usr/local/bin:/home/user/bin\nexport HOME=/home/user\nexport USER=user\n");
    KVFS::WriteString("/home/user/notes.txt", "Kurono OS Notes\n==============\n\nThings to try:\n- Open Terminal and type 'help'\n- Run 'kurono log' to see system logs\n- Open Task Manager to see processes\n- Browse files in File Manager\n- Try the Calculator app\n- Run 'neofetch' for system info\n");
    KVFS::WriteString("/home/user/welcome.html", "<html><body><h1>Welcome to Kurono</h1><p>A bare-metal operating system.</p><p>Try the browser app to view this.</p></body></html>");
    SerialLogger::Log("[KVFS] Filesystem populated\r\n");
    RuntimeLog::LogSystem("kernel", "default filesystem populated");
    RuntimeLog::LogBoot("default files populated");

    // kcl language self-test (gated by cmdline kurono.kcltest): kcl + kvfs are
    // both ready here, so run the interpreter test suite and log PASS/FAIL per
    // test to serial for headless verification. (satoru)
    if (boot_kcl_test) {
        SerialLogger::Log("[KCL] running self-test suite\r\n");
        KCLTest::RunAll();
    }

    // 802.11i wpa2 crypto self-test (gated by kurono.wifitest): assert the wifi
    // security core against published test vectors. needs no radio/hardware - it
    // proves the pmk/ptk/mic/ccmp math the supplicant relies on is byte-correct.
    // (satoru)
    if (boot_wifi_test) {
        SerialLogger::Log("[80211] running wpa2 security self-test\r\n");
        Ieee80211Test::RunAll();
    }

    if (boot_platform_test) PlatformTest::Run();   // (satoru)

    // kj language self-test (gated by cmdline kurono.kjtest): kss sheet layer is
    // initialized above, so the kss host-binding checks have a live sheet. (satoru)
    if (boot_kj_test) {
        SerialLogger::Log("[KJ] running self-test suite\r\n");
        KJTest::RunAll();
    }

    // linux syscall-abi self-test (gated by kurono.klstest): exercise a
    // representative syscall from each tier through Dispatch and log PASS/FAIL.
    // LinuxSyscall::Init() ran earlier so the process table + fd machinery are
    // live. powers off afterwards if kurono.klstest.poweroff=1 (headless ci). (satoru)
    if (boot_kls_test) {
        SerialLogger::Log("[KLS] running syscall-abi self-test\r\n");
        int kls_fails = LinuxSyscall::SelfTest();
        SerialLogger::Log(kls_fails == 0 ? "[KLS] self-test: ALL PASS\r\n"
                                         : "[KLS] self-test: FAILURES\r\n");
        if (boot_kls_poweroff) {
            SerialLogger::Log("[KLS] poweroff requested\r\n");
            HAL::PowerOff();
        }
    }

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
    // logging self-check (gated by cmdline kurono.logcheck): read back the core
    // log files from /kurono/var/log and dump their sizes + a sample to serial so
    // a headless boot can confirm the logging subsystem actually captured boot /
    // system / network / security events to disk. (satoru)
    if (boot_logcheck) {
        SerialLogger::Log("LOGCHECK_BEGIN\r\n");
        static const char* logpaths[] = {
            "/kurono/var/log/boot.log",
            "/kurono/var/log/system.log",
            "/kurono/var/log/network.log",
            "/kurono/var/log/security.log",
            "/kurono/var/log/crash/crash.log",
            nullptr
        };
        static char lbuf[4096];
        for (int i = 0; logpaths[i]; i++) {
            int n = KVFS::ReadFile(logpaths[i], lbuf, (uint32_t)sizeof(lbuf) - 1);
            SerialLogger::Log("  ");
            SerialLogger::Log(logpaths[i]);
            SerialLogger::Log(" bytes=");
            SerialLogger::LogDec(n > 0 ? n : 0);
            SerialLogger::Log("\r\n");
            if (n > 0) {
                lbuf[n] = 0;
                int lines = 0, p = 0;
                while (lbuf[p] && lines < 3) {     // sample the first 3 lines (satoru)
                    int s = p;
                    while (lbuf[p] && lbuf[p] != '\n') p++;
                    char save = lbuf[p]; lbuf[p] = 0;
                    SerialLogger::Log("    | ");
                    SerialLogger::Log(lbuf + s);
                    SerialLogger::Log("\r\n");
                    lbuf[p] = save;
                    if (lbuf[p]) p++;
                    lines++;
                }
            }
        }
        SerialLogger::Log("LOGCHECK_END\r\n");
    }

    AudioDMA::Init();
    AudioServer::Init();
    if (boot_uactest) UsbAudio::SelfTest();

    // initialize audio driver (sb16) early so apps can use it immediately
    Audio::Init();

    // the desktop layout below may resize the back buffer (ui scale): keep the
    // boot animation's ap away from it until the display manager is up. (satoru)
    BootAnim::Pause();
    // the crash store (KBOOT.CFG / KDRIVERS.CFG / KCRASH.LOG with kurono.kcrash=1)
    // opens in the boot watch process, before it reads KBOOT.CFG (its writes may
    // sleep, which needs a kernel process). it used to open only from a kld
    // family runner (fix 4). (satoru)
    BootCfg::SetCrashStoreDisk(boot_has_token(boot_cmdline, "kurono.kcrash=1"));
    // 3a: confirm or roll back this boot. recovery boots are never counted
    // (the loader already reset the streak); a cli boot counts as started. (satoru)
    if (!boot_recovery) BootCfg::StartWatch(boot_slot);
    if (has_display && !boot_cli) {
        SerialLogger::Log("[Desktop] Init...\r\n");
        RuntimeLog::LogBoot("desktop initialization started");
        // settings > displays: the saved arrangement / primary / on-off, when the
        // same outputs are present. before the scale and the desktop layout. (satoru)
        DisplaySettings::RestoreAtBoot();
        // ui scale: persisted display.ui_scale (percent), overridable with
        // kurono.uiscale=NNN. applied before the desktop is laid out so it sees the
        // logical size. 100 (the default) touches nothing. (satoru)
        {
            int ui_pct = UIConfig::Int("display.ui_scale", 100);
            char sv[12];
            if (boot_get_value(boot_cmdline, "kurono.uiscale", sv, sizeof(sv))) {
                int v = 0;
                for (int k = 0; sv[k] >= '0' && sv[k] <= '9'; k++) v = v * 10 + (sv[k] - '0');
                if (v >= 100 && v <= 300) ui_pct = v;
            }
            if (ui_pct != 100) Graphics::SetUIScale(ui_pct);
        }
        DesktopEnvironment::Init(Graphics::GetWidth(), Graphics::GetHeight());

        // stage 2: zero-copy present (kurono.igpu.zerocopy=1). it must run after
        // the saved output arrangement and ui scale are applied, because any
        // relayout suspends zero-copy and the layout above is the final one.
        // silent and inert when the flag is absent. (satoru)
        UiDiag::Enable(boot_has_token(boot_cmdline, "kurono.uidiag=1"));
        IntelAccel::ZeroCopyBoot(boot_has_token(boot_cmdline, "kurono.igpu.zerocopy=1"));
        // stage 2b: double-buffered page flip (kurono.igpu.flip=1), the
        // replacement for direct zero-copy. kurono.igpu.fliptest=1 runs the
        // same flip bookkeeping in qemu against a stand-in scanout. silent
        // when neither flag is present. (satoru)
        // gt stages, each with its own flag, all default off: kurono.igpu.gt=1
        // brings the blitter engine up without guc and proves a no-op batch
        // ran; kurono.igpu.blt=1 adds the fill/copy tests with cpu readback.
        // kurono.igpu.gttest=1 runs both in qemu against the software model
        // of the engine. before FlipBoot, so the flip's ggtt windows are
        // picked with the gt's entries already owned. (satoru)
        {
            bool gt_sim = boot_has_token(boot_cmdline, "kurono.igpu.gttest=1");
            int st_n = 0;
            {
                char sv[12];
                if (boot_get_value(boot_cmdline, "kurono.igpu.selftest", sv, sizeof(sv))) {
                    for (int k = 0; sv[k] >= '0' && sv[k] <= '9'; k++) st_n = st_n * 10 + (sv[k] - '0');
                    if (st_n == 1) st_n = 10240;          // =1 means the default run (satoru)
                }
            }
            int st_ops = 0;
            {
                char ov[8];
                if (boot_get_value(boot_cmdline, "kurono.igpu.selftest.jobops", ov, sizeof(ov)))
                    for (int k = 0; ov[k] >= '0' && ov[k] <= '9'; k++) st_ops = st_ops * 10 + (ov[k] - '0');
            }
            int st_mode = 0, st_flush = 1;
            {
                char mv[8];
                if (boot_get_value(boot_cmdline, "kurono.igpu.selftest.mode", mv, sizeof(mv))) st_mode = mv[0] - '0';
                if (boot_get_value(boot_cmdline, "kurono.igpu.gt.flush", mv, sizeof(mv))) st_flush = mv[0] - '0';
            }
            IntelGt::SetSelftest(st_n, st_ops, st_mode, st_flush);
            bool gt_blt = boot_has_token(boot_cmdline, "kurono.igpu.blt=1") || st_n > 0;
            bool gt_on = boot_has_token(boot_cmdline, "kurono.igpu.gt=1") || gt_blt;
            IntelGt::Boot(gt_on, gt_blt, gt_sim);
        }
        // test rig: kurono.testdim=N sets every output's software dim to N%
        // right here, like a saved display.brightness restored by the control
        // center before the flip starts (the laptop's flip refusal). (satoru)
        {
            char dv[8];
            if (boot_get_value(boot_cmdline, "kurono.testdim", dv, sizeof(dv))) {
                int pct = 0;
                for (int k = 0; dv[k] >= '0' && dv[k] <= '9'; k++) pct = pct * 10 + (dv[k] - '0');
                for (int o = 0; o < OUTPUT_MAX; o++) Graphics::SetOutputBrightness(o, pct);
                SerialLogger::Log("[testdim] software dim of every output set to ");
                SerialLogger::LogDec(pct);
                SerialLogger::Log("%\r\n");
            }
        }
        IntelAccel::FlipBoot(boot_has_token(boot_cmdline, "kurono.igpu.flip=1"),
                             boot_has_token(boot_cmdline, "kurono.igpu.fliptest=1"));

        // qemu test hook: run the zero-copy present machinery without an intel
        // display. 1 = write-back ram scanout with a cpu damage copy; wc =
        // render into the wc framebuffer to reproduce the old slowdown. (satoru)
        {
            char zv[8];
            int zm = 0;
            if (boot_get_value(boot_cmdline, "kurono.igpu.zctest", zv, sizeof(zv))) {
                if (zv[0] == 'w' && zv[1] == 'c') zm = 2;
                else if (zv[0] == '1') zm = 1;
            }
            if (zm) {
                char why[64];
                if (Graphics::ZeroCopyTestStart(zm, why, sizeof(why))) {
                    SerialLogger::Log("[igpu-zc] test mode ");
                    SerialLogger::LogDec(zm);
                    SerialLogger::Log(" active\r\n");
                } else {
                    SerialLogger::Log("[igpu-zc] test mode refused: ");
                    SerialLogger::Log(why);
                    SerialLogger::Log("\r\n");
                }
            }
        }

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
        // settings that only the settings app wrote so far: solid wallpaper,
        // volume + mute, power mode. (satoru)
        SystemSettings::RestoreAtBoot();

        // First-boot detection: if we've never been installed, run the
        // graphical installer before the lockscreen.  The installer either
        // completes (and reboots into the installed system) or returns
        // false to indicate "Live Boot" - in which case we fall through
        // to the lockscreen and offer Install Kurono on the desktop later.
        // Installer is launched on demand from the desktop "Install Kurono"
        // shortcut, not auto-run on first boot - auto-launch left users
        // staring at a black installer screen with no working input.
        // Setup wizard: the "Kurono Setup" grub entry boots with kurono.setup=1
        // and lands here. run the graphical installer / first-setup flow. when
        // the user completes an install the success screen reboots; when they
        // choose "Live Boot" / finish setup, Run() returns false and we fall
        // through to the desktop just like a normal boot. (satoru)
        if (boot_setup) {
            RuntimeLog::LogBoot("setup wizard");
            SerialLogger::Log("[setup] kurono.setup=1 -> launching installer/first-setup wizard\r\n");
            InstallerGUI::Run(boot_setup_screen);
            SerialLogger::Log("[setup] wizard returned, continuing to desktop\r\n");
        }

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
            // the sign-in screen is up: the system started. a slow sign-in must
            // not count as a failed boot (or end a trial). (satoru)
            BootCfg::NoteDesktop();
            LockScreen::Show();
        }
        RuntimeLog::LogBoot("desktop ready");
        BootCfg::NoteDesktop();   // the boot watch confirms this boot 60 s later (satoru)
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
    SerialLogger::Log("[VMM] Deferred - virtualization initializes on demand only\r\n");
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
    Scheduler::CreateProcess("ken_engine",  nullptr, 0);
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
        SerialLogger::Log("[IOMMU] VT-d available - GPU passthrough possible\r\n");
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
            Backlight::Probe();   // read only (satoru)
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
        if (boot_vgsync) {
            VirtIOGPU::SetSyncPresent(true);
            SerialLogger::Log("[VirtIOGPU] blocking present forced (kurono.vgsync=1)\r\n");
        }
    }

    // initialize unified display manager
    // detects the active backend and enables runtime resolution switching.
    SerialLogger::Log("[DisplayMgr] Init...\r\n");
    if (DisplayManager::Init()) {
        {   // kurono.res=WxH: ask the display backend for a mode at boot. no-op where it can't. (satoru)
            char rv[16];
            if (boot_get_value(boot_cmdline, "kurono.res", rv, sizeof(rv))) {
                uint32_t rw = 0, rh = 0; int ri = 0;
                while (rv[ri] >= '0' && rv[ri] <= '9') rw = rw * 10 + (uint32_t)(rv[ri++] - '0');
                if (rv[ri] == 'x') { ri++; while (rv[ri] >= '0' && rv[ri] <= '9') rh = rh * 10 + (uint32_t)(rv[ri++] - '0'); }
                if (rw && rh) DisplayManager::SetResolution(rw, rh, 32);
            }
        }
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
    BootAnim::Resume();   // display config is final from here to the gui process (satoru)

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

    if (boot_recovery) {
        RuntimeLog::LogBoot("recovery");
        Recovery::Run(boot_recovery_reason);   // never returns (satoru)
    }
    if (boot_cli) {
        cli_decode_command(boot_cli_run);
        SerialLogger::Log("[CLI] Entering CLI boot shell\r\n");
        RuntimeLog::LogBoot("cli shell ready");
        RuntimeLog::LogSystem("kernel", "entered cli boot mode");
        // a cli boot that reached its prompt started fine. confirmed here and
        // now: kernel processes may not run before the cli loop does. (satoru)
        BootCfg::NoteDesktop();
        BootCfg::ConfirmDesktop(boot_slot);
        cli_run_shell(boot_cli_run[0] ? boot_cli_run : nullptr, boot_cli_poweroff);
    }

    // smp phase 3d (TEMP - gate behind a cmdline token before the final commit):
    // run this AFTER all the noisy init so an AP fault dump is clean. load
    // /usr/bin/hello, pin it to the APs (affinity excludes cpu 0), flip the
    // ap-dispatch gate; a spinning ap claims + launches it in ring-3 in parallel
    // with the bsp. watch the serial for "[SMP] cpuN running user proc". (satoru)
    // smp 3d demo (opt-in via kurono.apsched): queue a real user program for an
    // application processor to run in ring-3 in parallel with the bsp desktop. uses
    // mhello (static musl, SYSCALL-based, proven rc=0); the int-0x80 hello SIGSEGVs
    // on the bsp too (a pre-existing loader/int80 issue, unrelated to smp). (satoru)
    if (SMP::ApUserSched() && SMP::CpuCount() > 1 && EmbeddedUserprogs::HasPthreadTest()) {
        SerialLogger::Log("[aptest] queueing /usr/bin/pthread_test (multithreaded) for AP execution...\r\n");
        Process* ap_hp = ElfLoader::LoadELF64FromVFS("/usr/bin/pthread_test", "ap_pthtest");
        if (!ap_hp) {
            SerialLogger::Log("[aptest] load failed\r\n");
        } else {
            ap_hp->cpu_affinity = 0xFE;   // cpus 1..7 only, never the bsp (satoru)
            LinuxSyscall::ClearConsoleOutput();
            SerialLogger::Log("[aptest] queued; an AP should run it (preemptively) in ring-3\r\n");
        }
    }

    // kurono.irqlat=1: run the ring-3 wake-latency probe. spawned as its own
    // kernel-process so the (deliberately sleepy) probe never blocks the gui
    // loop, and so it is scheduled like any other task. it prints [irqlat]
    // lines to com1 and exits. (satoru)
    if (g_smpscale_threads > 0 && EmbeddedUserprogs::HasSmpScale()) {
        SerialLogger::Log("[smpscale] launching /usr/bin/smpscale\r\n");
        Scheduler::SpawnKernelProcess("smpscale-run", smpscale_runner_entry, PRIO_NORMAL, 64, 32 * 1024);
    }
    if (boot_irqlat && EmbeddedUserprogs::HasIrqLat()) {
        SerialLogger::Log("[irqlat] launching /usr/bin/irqlat (ring-3 wake latency)\r\n");
        Scheduler::SpawnKernelProcess("irqlat-run", irqlat_runner_entry,
                                      PRIO_NORMAL, 64, 32 * 1024);
    }
    // kurono.spawntest=1: run the posix_spawn static-elf64 repro once at boot. (satoru)
    if (boot_spawntest && EmbeddedUserprogs::HasSpawnTest()) {
        SerialLogger::Log("[spawntest] launching /usr/bin/spawntest (posix_spawn static elf)\r\n");
        Scheduler::SpawnKernelProcess("spawntest-run", spawntest_runner_entry,
                                      PRIO_NORMAL, 64, 32 * 1024);
    }

    if (boot_has_token(boot_cmdline, "kurono.straytest=1")) {   // stray-vector self test (satoru)
        Scheduler::SpawnKernelProcess("straytest", straytest_entry, PRIO_NORMAL, 64, 32 * 1024);
    }
    if (boot_vmtest) {
        Scheduler::SpawnKernelProcess("vmtest", vmtest_runner_entry,
                                      PRIO_LOW, 64, 32 * 1024);
    }
    if (R3Audio::Enabled() && EmbeddedUserprogs::HasAudiod()) {
        Scheduler::SpawnKernelProcess("r3audio", r3audio_runner_entry,
                                      PRIO_HIGH  , 64, 64 * 1024);
    }
    if (boot_uatest && EmbeddedUserprogs::HasUaTest()) {
        Scheduler::SpawnKernelProcess("uatest", uatest_runner_entry,
                                      PRIO_NORMAL, 64, 32 * 1024);
    }
    if (boot_findtest && EmbeddedUserprogs::HasFindtest()) {
        Scheduler::SpawnKernelProcess("findtest", findtest_runner_entry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    }
    if (boot_kentest && EmbeddedUserprogs::HasKenTest()) {
        Scheduler::SpawnKernelProcess("kentest", kentest_runner_entry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    }
    if (boot_census_s) {
        MemCensus::Report("boot");
        MemCensus::StartPeriodic(boot_census_s);
    }
    if (boot_taskmem_log_s) TaskMem::StartPeriodicLog(boot_taskmem_log_s);   // kurono.taskmem.log=N (satoru)
    if (boot_churn && _binary_churn_elf_start) {
        Scheduler::SpawnKernelProcess("churn", churn_runner_entry,
                                      PRIO_NORMAL, 64, 32 * 1024);
    }
    if (boot_mmapchurn && _binary_churn_elf_start) {
        Scheduler::SpawnKernelProcess("mmapchurn", mmapchurn_runner_entry,
                                      PRIO_NORMAL, 64, 32 * 1024);
    }
    if (boot_kldperm && _binary_churn_elf_start) {
        Scheduler::SpawnKernelProcess("kldperm", kldperm_runner_entry,
                                      PRIO_NORMAL, 64, 32 * 1024);
    }
    if (boot_elfreload && EmbeddedUserprogs::HasMuslHello()) {
        Scheduler::SpawnKernelProcess("elfreload", elfreload_runner_entry,
                                      PRIO_NORMAL, 64, 48 * 1024);
    }
    if (boot_memtest && _binary_memtest_elf_start) {
        Scheduler::SpawnKernelProcess("memtest", memtest_runner_entry,
                                      PRIO_NORMAL, 64, 32 * 1024);
    }
    if (boot_kshtest && EmbeddedUserprogs::HasKshTest()) {
        Scheduler::SpawnKernelProcess("kshtest", kshtest_runner_entry,
                                      PRIO_NORMAL, 64, 32 * 1024);
    }
    if (boot_dsai && EmbeddedUserprogs::HasDsai()) {
        Scheduler::SpawnKernelProcess("dsai", dsai_runner_entry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    }
    // one complete picture of the machine in the log: every pci function,
    // usb, display, disks, ps/2, network. read-only. (satoru)
    HwInfo::DumpAll();
    if (DiskLog::Ready()) {
        DiskLog::SetEager(false);   // the 1 s worker takes over from here (satoru)
        Scheduler::SpawnKernelProcess("disklog", DiskLog::WorkerEntry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    }
    PowerManager::Start();   // one worker for power actions + the autosave cadence (satoru)
    if (boot_claude) {
        Scheduler::SpawnKernelProcess("claude", ClaudeHost::BootRunnerEntry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    }
    if (boot_rtest) {
        Scheduler::SpawnKernelProcess("rtest", rtest_runner_entry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    }
    if (boot_audiotest) {
        Scheduler::SpawnKernelProcess("audiotest", audiotest_runner_entry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    }
    if (boot_audiotest2) {
        Scheduler::SpawnKernelProcess("audiotest2", audiotest2_entry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    }
    if (boot_usbaudiotest) {
        Scheduler::SpawnKernelProcess("usbaudiotest", usbaudiotest_runner_entry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    }
    if (boot_nettest) {
        Scheduler::SpawnKernelProcess("nettest", nettest_runner_entry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    }
    if (boot_bbtest && EmbeddedUserprogs::HasBusybox()) {
        Scheduler::SpawnKernelProcess("bbtest", bbtest_runner_entry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    }
    kld_firmware_init_once();
    if (boot_kldacpi) {
        Scheduler::SpawnKernelProcess("kld-acpi", kldacpi_runner_entry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    }
    if (boot_kldapitest) {
        Scheduler::SpawnKernelProcess("kld-apitest", kld_apitest_runner_entry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    }
    if (boot_kldi2chid) {
        Scheduler::SpawnKernelProcess("kld-i2chid", kld_i2chid_runner_entry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    }
    if (boot_kldiwlwifi) {
        Scheduler::SpawnKernelProcess("kld-iwlwifi", kld_iwlwifi_runner_entry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    }
    if (boot_kldbench && EmbeddedUserprogs::HasKldBench()) {
        // the bench spawns the container itself in kld mode. (satoru)
        Scheduler::SpawnKernelProcess("kld-bench", kldbench_runner_entry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    } else if (boot_kld && EmbeddedUserprogs::HasKldE1000e()) {
        Scheduler::SpawnKernelProcess("kld-run", kld_runner_entry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    }
    if (boot_kldauto) {
        Scheduler::SpawnKernelProcess("kld-auto", KldAuto::RunnerEntry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    }
    if (boot_kldstats) {
        Scheduler::SpawnKernelProcess("kld-stats", kld_stats_entry,
                                      PRIO_LOW, 64, 8 * 1024);
    }
    if (KldDisplay::Wanted() && KldDisplay::GetMode() == KldDisplay::MODE_PATTERN) {
        Scheduler::SpawnKernelProcess("kld-display-pattern", KldDisplay::PatternEntry,
                                      PRIO_NORMAL, 64, 64 * 1024);
    }
    if (boot_kldmsixcheck) {
        Scheduler::SpawnKernelProcess("kld-msixcheck", KldAuto::MsixCheckEntry,
                                      PRIO_NORMAL, 64, 16 * 1024);
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
    // test hook: start with the cursor hidden to exercise the show-on-input
    // path a usb-only laptop takes when the ps/2 port never answered. (satoru)
    if (boot_has_token(boot_cmdline, "kurono.mouse.hide=1")) Mouse::Hide();

    /* GUI autorun: after a short warmup, open the Terminal app and queue
       a single command (via cmdline `kurono.gui.run=...`). Used to debug
       interactive flows in headless QEMU runs.  Plumbed into the new
       GUIProcess via KernelProcesses::SetGuiAutorun(). */
    cli_decode_command(boot_gui_run);
    KernelProcesses::SetGuiAutorun(boot_gui_run);
    bool gui_autorun_armed = (boot_gui_run[0] != 0);
    (void)gui_autorun_armed;
    uint32_t gui_autorun_ms_target = Timer::GetTicks() + (boot_fastrun ? 250u : 4000u);
    (void)gui_autorun_ms_target;

    // arm the interactive ksa prompt demo so the gui process fires the real
    // on-screen modal once the desktop is up (kurono.ksa.prompt). (satoru)
    if (boot_ksa_prompt) {
        SerialLogger::Log("[KSA] interactive prompt demo armed (kurono.ksa.prompt)\r\n");
        KernelProcesses::ArmKsaPromptDemo(boot_ksa_prompt_cred);
    }

    // arm the kj-scripted animation demo so the gui process runs the shipped
    // accent-animation .kj once the desktop is up (kurono.kjdemo). (satoru)
    if (boot_kj_demo) {
        SerialLogger::Log("[KJ] scripted animation demo armed (kurono.kjdemo)\r\n");
        KernelProcesses::ArmKjDemo();
    }

    // ── Preemptive multitasking switch-over ─────────────────────────────
    // For graphical / headless boots we hand control to the new scheduler.
    // Spawn the seven canonical kernel processes (Network/Input/Audio/GUI/
    // Shell/Logging/Scheduler), then call Scheduler::Start() - this enables
    // IRQs and never returns.  The text-only and console-realtime fallbacks
    // below still use the polling loop so emergency boot keeps working
    // even if the preemptive path is later disabled at compile time.
    if (has_display) {
        // must precede SpawnAll: the gui process samples the grace at entry. (satoru)
        if (boot_fastrun) KernelProcesses::SetAutorunDelayMs(250u);
        if (boot_fpsbench) KernelProcesses::SetFpsBench(boot_fpsbench);
        if (boot_has_token(boot_cmdline, "kurono.pacesleep=1")) KernelProcesses::SetPaceSleep(true);   // 4e (satoru)
        if (boot_fpsbench == 3) Graphics::SetForceFullPresent(true);
        if (boot_pacetest) KernelProcesses::SetPaceTest(boot_pacetest);
        int spawned = KernelProcesses::SpawnAll();
        SerialLogger::Log("[Kernel] Kernel processes spawned: ");
        SerialLogger::LogDec(spawned);
        SerialLogger::Log("\r\n");
        RuntimeLog::LogBoot("preemptive scheduler engaged");
        // kmemx: apply the persisted config (kmemx.enabled/pool_pct/threshold)
        // and, if enabled, start the compression worker so it joins the scheduler
        // with the canonical processes. logs the boot banner. (satoru)
        KMemX::ApplyConfig();
        // bring up kinit AFTER the canonical kernel processes: it adopts the
        // already-running in-kernel services (klog/knet/kdbus/kwayland/kaudio)
        // as supervised units, sequences the boot targets (which start the
        // kpkg-daemon / kupdate / ksecurity workers via their start hooks), and
        // spawns its crash monitor - all joining the scheduler. (satoru)
        KernelHeap::LogMemReport("pre-kinit");
        KInit::Boot();
        // headless kinit self-tests: run off the gui loop so the bounded waits
        // never stall rendering. (satoru)
        if (boot_kinit_test) {
            g_kinit_test_poweroff = boot_kinit_poweroff;
            Scheduler::SpawnKernelProcess("kinit-selftest", kinit_selftest_entry, PRIO_LOW, 64, 16 * 1024);
        }
        // headless kdf crash-recovery self-test: spawn the runner off the gui loop.
        // the test driver was registered before KInit::Boot (so it is a known unit
        // started at the user target); the runner faults it + checks recovery. a
        // bigger kernel stack since RunGuarded + the fault unwind run on it. (satoru)
        if (boot_kdf_test) {
            KDF::SetCrashTestPoweroff(boot_kdf_poweroff);
            Scheduler::SpawnKernelProcess("kdf-selftest", kdf_selftest_entry, PRIO_LOW, 64, 32 * 1024);
        }
        // (the kmemx self-test runs inline earlier in kernel_main - see the
        //  boot_kmemx_test block near kfsbench - so it is mode-independent and
        //  needs no gui-path kproc here.) (satoru)
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
        Dhcp::Tick();   // no network process in this fallback loop (satoru)

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
            bool mouse_down = Mouse::IsLeftDown(); // true while button held - enables dragging

            // drain all keyboard chars per frame to eliminate input lag
            // first call includes mouse state, subsequent calls are keyboard-only
            char kb_char = 0;
            if (Keyboard::HasChar()) kb_char = Keyboard::GetChar();
            DesktopEnvironment::HandleInput(Mouse::mx, Mouse::my, mouse_down, mouse_clicked, kb_char);
            while (Keyboard::HasChar()) {
                kb_char = Keyboard::GetChar();
                // real held state, not false: a hardcoded false here spuriously
                // toggled the wl_pointer button edge mid-hold = double clicks. (satoru)
                DesktopEnvironment::HandleInput(Mouse::mx, Mouse::my, Mouse::IsLeftDown(), false, kb_char);
            }

            // forward scroll to the surface under the pointer (matches PumpUI). (satoru)
            if (scroll_delta != 0) {
                WaylandServer::ForwardPointerAxis(Mouse::mx, Mouse::my, 0, scroll_delta * 32);
                Window* fw = WindowManager::GetFocusedWindow();
                if (fw && fw->input && !WaylandServer::IsWaylandWindow(fw->id)) {
                    { TaskMem::WindowScope _tms(fw->id); fw->input(fw, 3, scroll_delta, 0); } // event 3 = scroll
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
                // the lock ui owned the scanned buffer: repaint everything. (satoru)
                Graphics::MarkDirty(0, 0, Graphics::GetWidth(), Graphics::GetHeight());
                Graphics::MarkUIDirty();
                continue;  // skip render this frame, restart loop fresh
            }

            // refresh task manager periodically - every 300 frames (~2s)
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
                        // sustained head-room - try original rate again
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
            // (satoru) the damage list already repaints the old and new window
            // footprints; the full-screen fill cost a complete frame copy per
            // drag and, with zero-copy, wrote the buffer the panel scans.

            // redraw every frame for full smoothness
            DesktopEnvironment::Render();

            // fps counter update - using real pit-polled time
            frame_counter++;
            fps_counter++;
            uint32_t now_ms = Timer::GetRealMs();
            if (now_ms - last_fps_ms >= 1000) {
                displayed_fps = fps_counter;
                fps_counter = 0;
                last_fps_ms = now_ms;
            }

            // fps pill, off unless ui.fps=on (f12 hud shows it too) (satoru)
            if (UIConfig::Bool("ui.fps", false)) {
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
