//  kurono os  -  gpu driver installer (path a/b finishing piece)
//
//  detects the host gpu, asks (via ui) whether the user wants to install
//  vendor drivers into a guest distro, and runs the actual install
//  inside the running alpine or debian guest via Hypervisor::AlpineExec
//  / DebianExec.
//
//  exposed surfaces:
//    - kpkg setup <alpine|debian>-<nvidia|amd|auto>
//    - installer gui screen scr_drivers (after main install, before success)
//    - settings -> updates panel "Install GPU Drivers" button
#pragma once
#include "../kernel/types.h"

enum DetectedGPU {
    DGPU_NONE = 0,
    DGPU_NVIDIA,
    DGPU_AMD,
    DGPU_INTEL,
};

enum DriverDistro {
    DRV_DISTRO_ALPINE = 0,
    DRV_DISTRO_DEBIAN = 1,
};

enum DriverInstallStatus {
    DRV_IDLE = 0,
    DRV_RUNNING,
    DRV_DONE,
    DRV_ERROR_NO_DISTRO,
    DRV_ERROR_NO_GPU,
    DRV_ERROR_NO_NETWORK,
    DRV_ERROR_EXEC,
};

class GpuDriverInstaller {
public:
    static void Init();

    // hardware probe (non-destructive, safe from any context)
    static DetectedGPU DetectVendor();
    static const char* VendorName(DetectedGPU v);

    // distro presence  -  alpine is always available (initramfs is baked in),
    // debian needs the rootfs to be extracted first
    static bool IsAlpineAvailable();
    static bool IsDebianAvailable();

    // perform a full driver install into the named distro for the
    // specified vendor.  blocks until done; the guest vm is booted on
    // demand.  log/err are filled with human-readable progress.
    //
    // returns DRV_DONE on success, an error code otherwise.
    static DriverInstallStatus Setup(DriverDistro distro, DetectedGPU vendor,
                                       char* log, int log_max);

    // last status (for ui polling between draw frames)
    static DriverInstallStatus GetStatus();
    static int  GetProgress();        // 0-100
    static const char* GetStatusText();

    // shell entry  -  wired from PackageManager::cmd_install when argv[1] == "setup"
    //   kpkg setup alpine-nvidia | alpine-amd | alpine-auto
    //   kpkg setup debian-nvidia | debian-amd | debian-auto
    static int CmdSetup(int argc, const char** argv, char* out, int out_max);

private:
    static DriverInstallStatus s_status;
    static int                 s_progress;
    static char                s_status_text[128];

    static void SetStatus(DriverInstallStatus st, int pct, const char* msg);
    static bool RunAlpineNvidia(char* log, int max);
    static bool RunAlpineAmd   (char* log, int max);
    static bool RunDebianNvidia(char* log, int max);
    static bool RunDebianAmd   (char* log, int max);
};
