//  kurono os - boot-time system update screen.  see header for design.
#include "system_update.h"
#include "gpu_driver_installer.h"
#include "../fs/kvfs.h"
#include "../drivers/graphics.h"
#include "../drivers/mouse.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../drivers/serial.h"
#include "../ui/font.h"
#include "../virt/hypervisor.h"
#include "../virt/debian_data.h"
#include "../kernel/time.h"
#include "input_manager.h"
#include "../kernel/types.h"

static const char* SU_MARKER_PATH = "/var/lib/kurono/pending-update";

// ---------------------------------------------------------------- helpers
static int su_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; ++a; ++b; }
    return *a == 0 && *b == 0;
}
static int su_starts(const char* s, const char* p) {
    while (*p) { if (*s++ != *p++) return 0; }
    return 1;
}
static int su_cat(char* d, int p, int max, const char* s) {
    if (!s) return p;
    while (*s && p < max - 1) d[p++] = *s++;
    if (p < max) d[p] = 0;
    return p;
}
static void su_extract_value(const char* line, const char* key, char* out, int max) {
    out[0] = 0;
    if (!su_starts(line, key)) return;
    line += 0;
    int kl = 0; while (key[kl]) kl++;
    line += kl;
    if (*line != '=') return;
    line++;
    int p = 0;
    while (*line && *line != '\n' && *line != '\r' && p < max - 1) out[p++] = *line++;
    out[p] = 0;
}

// ---------------------------------------------------------------- marker io
bool SystemUpdate::HasPendingUpdate() {
    return KVFS::Exists(SU_MARKER_PATH);
}

bool SystemUpdate::QueueUpdate(const char* action, const char* gpu_hint) {
    KVFS::Mkdirs("/var/lib/kurono");
    char buf[256];
    int p = 0;
    p = su_cat(buf, p, sizeof(buf), "action=");
    p = su_cat(buf, p, sizeof(buf), action ? action : "");
    p = su_cat(buf, p, sizeof(buf), "\ngpu=");
    p = su_cat(buf, p, sizeof(buf), gpu_hint ? gpu_hint : "none");
    p = su_cat(buf, p, sizeof(buf), "\nrestart_to=desktop\n");
    return KVFS::WriteString(SU_MARKER_PATH, buf) >= 0;
}

// ---------------------------------------------------------------- ui
namespace {
int g_w = 0, g_h = 0;

void su_bg() {
    for (int y = 0; y < g_h; y += 4) {
        uint8_t shade = (uint8_t)(18 + (y * 24) / (g_h + 1));
        uint32_t c = 0xFF000000 | ((uint32_t)shade << 16) | ((uint32_t)(shade + 6) << 8) | (uint32_t)(shade + 16);
        Graphics::FillRect(0, y, g_w, 4, c);
    }
    Graphics::FillRect(0, 0, g_w, 60, 0xFF12121C);
    FontTTF::DrawString(24, 40, 22.0f, "Kurono OS - System Update", 0xFFFFFFFF);
    Graphics::FillRect(0, 60, g_w, 1, 0xFF5C8AFF);
}

void su_progress_bar(int pct, const char* status, const char* sub) {
    int bw = 600, bh = 24;
    int bx = g_w / 2 - bw / 2;
    int by = g_h / 2;
    Graphics::FillRoundedRect(bx, by, bw, bh, 12, 0x401F2030);
    int filled = (bw * pct) / 100;
    Graphics::FillRoundedRect(bx, by, filled, bh, 12, 0xFF5C8AFF);

    char pctbuf[8]; int pp = 0;
    int v = pct;
    if (v >= 100) { pctbuf[pp++] = '1'; pctbuf[pp++] = '0'; pctbuf[pp++] = '0'; }
    else if (v >= 10) { pctbuf[pp++] = (char)('0' + v / 10); pctbuf[pp++] = (char)('0' + v % 10); }
    else { pctbuf[pp++] = (char)('0' + v); }
    pctbuf[pp++] = '%'; pctbuf[pp] = 0;
    int pw = FontTTF::Measure(15.0f, pctbuf);
    FontTTF::DrawString(g_w / 2 - pw / 2, by - 14, 15.0f, pctbuf, 0xFFFFFFFF);

    if (status) {
        int sw = FontTTF::Measure(16.0f, status);
        FontTTF::DrawString(g_w / 2 - sw / 2, by + bh + 30, 16.0f, status, 0xFFFFFFFF);
    }
    if (sub) {
        int sw = FontTTF::Measure(13.0f, sub);
        FontTTF::DrawString(g_w / 2 - sw / 2, by + bh + 56, 13.0f, sub, 0xC0FFFFFF);
    }
}

void su_render(int pct, const char* status, const char* sub) {
    su_bg();
    su_progress_bar(pct, status, sub);
    Graphics::SwapBuffers();
}
}

// ---------------------------------------------------------------- run
bool SystemUpdate::RunPendingUpdate() {
    g_w = Graphics::GetWidth();
    g_h = Graphics::GetHeight();

    char marker[512];
    int n = KVFS::ReadFile(SU_MARKER_PATH, marker, (uint32_t)sizeof(marker) - 1);
    if (n <= 0) return false;
    marker[n] = 0;

    char action[64] = {0};
    char gpu[16]    = {0};
    int line_start = 0;
    for (int i = 0; i <= n; i++) {
        if (marker[i] == 0 || marker[i] == '\n') {
            char tmp = marker[i]; marker[i] = 0;
            const char* line = marker + line_start;
            if (action[0] == 0) su_extract_value(line, "action", action, sizeof(action));
            if (gpu[0] == 0)    su_extract_value(line, "gpu",    gpu,    sizeof(gpu));
            marker[i] = tmp;
            line_start = i + 1;
            if (tmp == 0) break;
        }
    }

    SerialLogger::Log("[SystemUpdate] running pending action: ");
    SerialLogger::Log(action);
    SerialLogger::Log("\r\n");

    su_render(5, "Preparing update...", "Do not power off your computer");
    Timer::WaitMs(400);

    // ---- debian install branch ----
    if (su_eq(action, "debian-install")) {
        su_render(15, "Verifying downloaded Debian rootfs", "/var/lib/kurono/debian-rootfs.ext4");
        if (!DebianRootfs::Available()) {
            su_render(0, "Debian rootfs missing!", "Re-run: kpkg install debian");
            Timer::WaitMs(3000);
            KVFS::Unlink(SU_MARKER_PATH);
            return false;
        }

        su_render(35, "Loading Debian rootfs into memory", "First boot stages may take a moment");
        // touch the data path to force the disk-cache fill before we boot
        const uint8_t* d = DebianRootfs::Data();
        uint32_t sz = DebianRootfs::Size();
        (void)d; (void)sz;

        su_render(60, "Booting Debian guest for post-install setup", "apt update + first run scripts");
        Hypervisor::BootDebianWithExtraction(75000);

        if (Hypervisor::IsDebianBooted()) {
            char res[2048];
            su_render(75, "Configuring apt sources inside Debian", "");
            Hypervisor::DebianExec(
                "sed -i 's/main$/main contrib non-free non-free-firmware/' /etc/apt/sources.list 2>&1",
                res, (int)sizeof(res) - 1);
            su_render(82, "Running apt-get update inside Debian", "");
            Hypervisor::DebianExec("apt-get update 2>&1 | tail -3", res, (int)sizeof(res) - 1);
        }

        // optional GPU driver step
        if (gpu[0] && !su_eq(gpu, "none") && !su_eq(gpu, "")) {
            su_render(90, "Installing GPU drivers in Debian", gpu);
            DetectedGPU vendor = DGPU_NONE;
            if      (su_eq(gpu, "nvidia")) vendor = DGPU_NVIDIA;
            else if (su_eq(gpu, "amd"))    vendor = DGPU_AMD;
            else if (su_eq(gpu, "auto"))   vendor = GpuDriverInstaller::DetectVendor();

            if (vendor == DGPU_NVIDIA || vendor == DGPU_AMD) {
                char log[2048];
                GpuDriverInstaller::Setup(DRV_DISTRO_DEBIAN, vendor, log, (int)sizeof(log));
            }
        }

        su_render(98, "Finalizing Debian setup", "Writing /etc/debian-installed");
        KVFS::Mkdirs("/etc");
        KVFS::WriteString("/etc/debian-installed", "true\n");
    }

    // ---- (other actions can be added here) ----

    su_render(100, "System update complete", "Continuing to desktop...");
    Timer::WaitMs(900);

    KVFS::Unlink(SU_MARKER_PATH);
    return true;
}

// ---------------------------------------------------------------- reboot
void SystemUpdate::Reboot() {
    // 8042 keyboard controller hard-reset line
    for (int i = 0; i < 100; i++) {
        uint8_t st;
        __asm__ __volatile__("inb %1, %0" : "=a"(st) : "Nd"((uint16_t)0x64));
        if (!(st & 0x02)) break;
    }
    __asm__ __volatile__("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    while (1) __asm__ __volatile__("hlt");
}
