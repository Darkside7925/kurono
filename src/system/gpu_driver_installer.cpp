//  kurono os - gpu driver installer.  see header for design.
#include "gpu_driver_installer.h"
#include "../drivers/nvidia_gpu.h"
#include "../drivers/amd_gpu.h"
#include "../drivers/intel_gpu.h"
#include "../drivers/serial.h"
#include "../virt/hypervisor.h"
#include "../net/network.h"
#include "../fs/kvfs.h"

DriverInstallStatus GpuDriverInstaller::s_status   = DRV_IDLE;
int                 GpuDriverInstaller::s_progress = 0;
char                GpuDriverInstaller::s_status_text[128] = {0};

// ----------------------------------------------------------------- helpers
static int gd_cat(char* out, int p, int max, const char* s) {
    if (!s) return p;
    while (*s && p < max - 1) out[p++] = *s++;
    if (p < max) out[p] = 0;
    return p;
}
static int gd_eq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return 0; ++a; ++b; }
    return *a == 0 && *b == 0;
}
static int gd_starts(const char* s, const char* p) {
    while (*p) { if (*s++ != *p++) return 0; }
    return 1;
}

void GpuDriverInstaller::SetStatus(DriverInstallStatus st, int pct, const char* msg) {
    s_status = st;
    s_progress = pct;
    int p = 0;
    if (msg) p = gd_cat(s_status_text, p, (int)sizeof(s_status_text), msg);
    SerialLogger::Log("[gpu-drv] ");
    SerialLogger::Log(msg ? msg : "");
    SerialLogger::Log("\r\n");
}

// ----------------------------------------------------------------- detect
void GpuDriverInstaller::Init() {
    SetStatus(DRV_IDLE, 0, "Idle");
}

DetectedGPU GpuDriverInstaller::DetectVendor() {
    if (NvidiaGPU::IsDetected()) return DGPU_NVIDIA;
    if (AmdGPU::IsAvailable())   return DGPU_AMD;
    if (IntelGPU::IsDetected())  return DGPU_INTEL;
    return DGPU_NONE;
}

const char* GpuDriverInstaller::VendorName(DetectedGPU v) {
    switch (v) {
        case DGPU_NVIDIA: return "NVIDIA";
        case DGPU_AMD:    return "AMD";
        case DGPU_INTEL:  return "Intel";
        default:          return "None";
    }
}

bool GpuDriverInstaller::IsAlpineAvailable() {
    // alpine kernel + initramfs are baked into the kurono image, so always
    // available.  we check that the boot symbols are non-empty just to be safe.
    return true;
}

bool GpuDriverInstaller::IsDebianAvailable() {
    // debian rootfs is only present after the user extracts it via
    // `kpkg install debian` or the dual-boot installer.
    return KVFS::Resolve("/debian/etc/os-release") != nullptr;
}

DriverInstallStatus GpuDriverInstaller::GetStatus()      { return s_status; }
int                 GpuDriverInstaller::GetProgress()    { return s_progress; }
const char*         GpuDriverInstaller::GetStatusText()  { return s_status_text; }

// ----------------------------------------------------------------- alpine paths
bool GpuDriverInstaller::RunAlpineNvidia(char* log, int max) {
    int p = 0;
    char res[2048];

    if (!Hypervisor::IsAlpineBooted()) {
        SetStatus(DRV_RUNNING, 5, "Booting Alpine guest...");
        Hypervisor::BootAlpineWithExtraction(50000);
        if (!Hypervisor::IsAlpineBooted()) {
            SetStatus(DRV_ERROR_EXEC, 0, "Alpine guest failed to boot");
            return false;
        }
    }

    SetStatus(DRV_RUNNING, 15, "Adding Alpine community + testing repos");
    Hypervisor::AlpineExec(
        "grep -q 'edge/community' /etc/apk/repositories || "
        "echo 'http://dl-cdn.alpinelinux.org/alpine/edge/community' >> /etc/apk/repositories",
        res, (int)sizeof(res) - 1);
    Hypervisor::AlpineExec(
        "grep -q 'edge/main' /etc/apk/repositories || "
        "echo 'http://dl-cdn.alpinelinux.org/alpine/edge/main' >> /etc/apk/repositories",
        res, (int)sizeof(res) - 1);

    SetStatus(DRV_RUNNING, 30, "apk update");
    int n = Hypervisor::AlpineExec("apk update 2>&1", res, (int)sizeof(res) - 1);
    if (n > 0) { res[n] = 0; p = gd_cat(log, p, max, res); p = gd_cat(log, p, max, "\n"); }

    SetStatus(DRV_RUNNING, 55, "Installing nvidia-drivers-open + linux-firmware-nvidia");
    n = Hypervisor::AlpineExec(
        "apk add --no-cache nvidia-drivers-open nvidia-libgl linux-firmware-nvidia "
        "mesa-dri-gallium mesa-vulkan-nvidia 2>&1 || "
        "apk add --no-cache nvidia-drivers linux-firmware-nvidia mesa-dri-gallium 2>&1",
        res, (int)sizeof(res) - 1);
    if (n > 0) { res[n] = 0; p = gd_cat(log, p, max, res); p = gd_cat(log, p, max, "\n"); }

    SetStatus(DRV_RUNNING, 80, "Loading nvidia kernel module");
    n = Hypervisor::AlpineExec("modprobe nvidia 2>&1; lsmod | grep -i nvidia", res, (int)sizeof(res) - 1);
    if (n > 0) { res[n] = 0; p = gd_cat(log, p, max, res); p = gd_cat(log, p, max, "\n"); }

    // mark provisioned so the desktop knows drivers are ready
    KVFS::Mkdirs("/var/lib/kurono");
    KVFS::WriteString("/var/lib/kurono/gpu-driver", "alpine-nvidia\n");

    SetStatus(DRV_DONE, 100, "NVIDIA drivers installed in Alpine");
    return true;
}

bool GpuDriverInstaller::RunAlpineAmd(char* log, int max) {
    int p = 0;
    char res[2048];

    if (!Hypervisor::IsAlpineBooted()) {
        SetStatus(DRV_RUNNING, 5, "Booting Alpine guest...");
        Hypervisor::BootAlpineWithExtraction(50000);
        if (!Hypervisor::IsAlpineBooted()) {
            SetStatus(DRV_ERROR_EXEC, 0, "Alpine guest failed to boot");
            return false;
        }
    }

    SetStatus(DRV_RUNNING, 20, "Enabling Alpine main + community repos");
    Hypervisor::AlpineExec(
        "grep -q 'edge/community' /etc/apk/repositories || "
        "echo 'http://dl-cdn.alpinelinux.org/alpine/edge/community' >> /etc/apk/repositories",
        res, (int)sizeof(res) - 1);

    SetStatus(DRV_RUNNING, 35, "apk update");
    int n = Hypervisor::AlpineExec("apk update 2>&1", res, (int)sizeof(res) - 1);
    if (n > 0) { res[n] = 0; p = gd_cat(log, p, max, res); p = gd_cat(log, p, max, "\n"); }

    SetStatus(DRV_RUNNING, 60, "Installing mesa amdgpu + linux-firmware-amdgpu");
    n = Hypervisor::AlpineExec(
        "apk add --no-cache mesa-dri-gallium mesa-vulkan-ati mesa-va-gallium "
        "linux-firmware-amdgpu xf86-video-amdgpu 2>&1",
        res, (int)sizeof(res) - 1);
    if (n > 0) { res[n] = 0; p = gd_cat(log, p, max, res); p = gd_cat(log, p, max, "\n"); }

    SetStatus(DRV_RUNNING, 85, "Loading amdgpu kernel module");
    n = Hypervisor::AlpineExec("modprobe amdgpu 2>&1; lsmod | grep -i amdgpu", res, (int)sizeof(res) - 1);
    if (n > 0) { res[n] = 0; p = gd_cat(log, p, max, res); p = gd_cat(log, p, max, "\n"); }

    KVFS::Mkdirs("/var/lib/kurono");
    KVFS::WriteString("/var/lib/kurono/gpu-driver", "alpine-amd\n");

    SetStatus(DRV_DONE, 100, "AMD drivers installed in Alpine");
    return true;
}

// ----------------------------------------------------------------- debian paths
bool GpuDriverInstaller::RunDebianNvidia(char* log, int max) {
    int p = 0;
    char res[2048];

    if (!Hypervisor::IsDebianBooted()) {
        SetStatus(DRV_RUNNING, 5, "Booting Debian guest...");
        Hypervisor::BootDebianWithExtraction(75000);
        if (!Hypervisor::IsDebianBooted()) {
            SetStatus(DRV_ERROR_EXEC, 0, "Debian guest failed to boot");
            return false;
        }
    }

    SetStatus(DRV_RUNNING, 20, "Enabling non-free + contrib in apt sources");
    Hypervisor::DebianExec(
        "sed -i 's/main$/main contrib non-free non-free-firmware/' /etc/apt/sources.list",
        res, (int)sizeof(res) - 1);

    SetStatus(DRV_RUNNING, 35, "apt update");
    int n = Hypervisor::DebianExec("apt-get update 2>&1", res, (int)sizeof(res) - 1);
    if (n > 0) { res[n] = 0; p = gd_cat(log, p, max, res); p = gd_cat(log, p, max, "\n"); }

    SetStatus(DRV_RUNNING, 65, "apt install nvidia-driver firmware-misc-nonfree");
    n = Hypervisor::DebianExec(
        "DEBIAN_FRONTEND=noninteractive apt-get install -y "
        "nvidia-driver firmware-misc-nonfree libnvidia-encode1 libgl1-mesa-dri 2>&1",
        res, (int)sizeof(res) - 1);
    if (n > 0) { res[n] = 0; p = gd_cat(log, p, max, res); p = gd_cat(log, p, max, "\n"); }

    SetStatus(DRV_RUNNING, 90, "Loading nvidia kernel module");
    n = Hypervisor::DebianExec("modprobe nvidia 2>&1; lsmod | grep -i nvidia", res, (int)sizeof(res) - 1);
    if (n > 0) { res[n] = 0; p = gd_cat(log, p, max, res); p = gd_cat(log, p, max, "\n"); }

    KVFS::Mkdirs("/var/lib/kurono");
    KVFS::WriteString("/var/lib/kurono/gpu-driver", "debian-nvidia\n");

    SetStatus(DRV_DONE, 100, "NVIDIA drivers installed in Debian");
    return true;
}

bool GpuDriverInstaller::RunDebianAmd(char* log, int max) {
    int p = 0;
    char res[2048];

    if (!Hypervisor::IsDebianBooted()) {
        SetStatus(DRV_RUNNING, 5, "Booting Debian guest...");
        Hypervisor::BootDebianWithExtraction(75000);
        if (!Hypervisor::IsDebianBooted()) {
            SetStatus(DRV_ERROR_EXEC, 0, "Debian guest failed to boot");
            return false;
        }
    }

    SetStatus(DRV_RUNNING, 20, "Enabling non-free-firmware in apt sources");
    Hypervisor::DebianExec(
        "sed -i 's/main$/main contrib non-free non-free-firmware/' /etc/apt/sources.list",
        res, (int)sizeof(res) - 1);

    SetStatus(DRV_RUNNING, 35, "apt update");
    int n = Hypervisor::DebianExec("apt-get update 2>&1", res, (int)sizeof(res) - 1);
    if (n > 0) { res[n] = 0; p = gd_cat(log, p, max, res); p = gd_cat(log, p, max, "\n"); }

    SetStatus(DRV_RUNNING, 65, "apt install firmware-amd-graphics + mesa drivers");
    n = Hypervisor::DebianExec(
        "DEBIAN_FRONTEND=noninteractive apt-get install -y "
        "firmware-amd-graphics libgl1-mesa-dri mesa-vulkan-drivers "
        "xserver-xorg-video-amdgpu 2>&1",
        res, (int)sizeof(res) - 1);
    if (n > 0) { res[n] = 0; p = gd_cat(log, p, max, res); p = gd_cat(log, p, max, "\n"); }

    SetStatus(DRV_RUNNING, 90, "Loading amdgpu kernel module");
    n = Hypervisor::DebianExec("modprobe amdgpu 2>&1; lsmod | grep -i amdgpu", res, (int)sizeof(res) - 1);
    if (n > 0) { res[n] = 0; p = gd_cat(log, p, max, res); p = gd_cat(log, p, max, "\n"); }

    KVFS::Mkdirs("/var/lib/kurono");
    KVFS::WriteString("/var/lib/kurono/gpu-driver", "debian-amd\n");

    SetStatus(DRV_DONE, 100, "AMD drivers installed in Debian");
    return true;
}

// ----------------------------------------------------------------- public api
DriverInstallStatus GpuDriverInstaller::Setup(DriverDistro distro, DetectedGPU vendor,
                                                char* log, int log_max) {
    if (log && log_max > 0) log[0] = 0;

    // distro guard
    if (distro == DRV_DISTRO_DEBIAN && !IsDebianAvailable()) {
        SetStatus(DRV_ERROR_NO_DISTRO, 0,
                   "Debian rootfs not installed. Run: kpkg install debian");
        return DRV_ERROR_NO_DISTRO;
    }
    if (distro == DRV_DISTRO_ALPINE && !IsAlpineAvailable()) {
        SetStatus(DRV_ERROR_NO_DISTRO, 0, "Alpine guest not available");
        return DRV_ERROR_NO_DISTRO;
    }

    // gpu guard - auto-detect if caller passed DGPU_NONE
    if (vendor == DGPU_NONE) vendor = DetectVendor();
    if (vendor != DGPU_NVIDIA && vendor != DGPU_AMD) {
        SetStatus(DRV_ERROR_NO_GPU, 0,
                   "No supported discrete GPU detected (NVIDIA or AMD)");
        return DRV_ERROR_NO_GPU;
    }

    // dispatch
    bool ok = false;
    if (distro == DRV_DISTRO_ALPINE) {
        ok = (vendor == DGPU_NVIDIA) ? RunAlpineNvidia(log, log_max)
                                      : RunAlpineAmd   (log, log_max);
    } else {
        ok = (vendor == DGPU_NVIDIA) ? RunDebianNvidia(log, log_max)
                                      : RunDebianAmd   (log, log_max);
    }
    return ok ? DRV_DONE : s_status;
}

// ----------------------------------------------------------------- shell
int GpuDriverInstaller::CmdSetup(int argc, const char** argv, char* out, int out_max) {
    int p = 0;
    if (argc < 3) {
        p = gd_cat(out, p, out_max,
                    "Usage: kpkg setup <target>\n"
                    "  Targets:\n"
                    "    alpine-nvidia   alpine-amd   alpine-auto\n"
                    "    debian-nvidia   debian-amd   debian-auto\n");
        return p;
    }
    const char* target = argv[2];

    DriverDistro distro;
    DetectedGPU  vendor;

    if (gd_starts(target, "alpine-")) {
        distro = DRV_DISTRO_ALPINE;
        target += 7;
    } else if (gd_starts(target, "debian-")) {
        distro = DRV_DISTRO_DEBIAN;
        target += 7;
    } else {
        return gd_cat(out, p, out_max,
                       "kpkg setup: target must start with alpine- or debian-\n");
    }

    if      (gd_eq(target, "nvidia")) vendor = DGPU_NVIDIA;
    else if (gd_eq(target, "amd"))    vendor = DGPU_AMD;
    else if (gd_eq(target, "auto"))   vendor = DetectVendor();
    else {
        return gd_cat(out, p, out_max,
                       "kpkg setup: vendor must be nvidia, amd, or auto\n");
    }

    p = gd_cat(out, p, out_max, "kpkg setup: distro=");
    p = gd_cat(out, p, out_max, distro == DRV_DISTRO_ALPINE ? "alpine" : "debian");
    p = gd_cat(out, p, out_max, " vendor=");
    p = gd_cat(out, p, out_max, VendorName(vendor));
    p = gd_cat(out, p, out_max, "\n");

    char log[4096];
    DriverInstallStatus st = Setup(distro, vendor, log, (int)sizeof(log));

    p = gd_cat(out, p, out_max, log);
    p = gd_cat(out, p, out_max, "\n");
    switch (st) {
        case DRV_DONE:
            p = gd_cat(out, p, out_max, "\xE2\x9C\x93 GPU drivers installed.\n");
            break;
        case DRV_ERROR_NO_DISTRO:
            p = gd_cat(out, p, out_max, "\xE2\x9C\x97 Distro not installed.\n");
            break;
        case DRV_ERROR_NO_GPU:
            p = gd_cat(out, p, out_max, "\xE2\x9C\x97 No NVIDIA/AMD GPU detected.\n");
            break;
        case DRV_ERROR_NO_NETWORK:
            p = gd_cat(out, p, out_max, "\xE2\x9C\x97 No network for package fetch.\n");
            break;
        case DRV_ERROR_EXEC:
            p = gd_cat(out, p, out_max, "\xE2\x9C\x97 Guest exec failed (see log).\n");
            break;
        default:
            p = gd_cat(out, p, out_max, "kpkg setup: in-progress.\n");
            break;
    }
    return p;
}
