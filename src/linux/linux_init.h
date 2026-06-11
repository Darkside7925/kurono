#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Linux Init System
//  Bootstraps the Linux subsystem like a real init/systemd process.
//  This is PID 1 of the Linux world inside Kurono.
//
//  Boot sequence:
//  1. Mount virtual filesystems (/proc, /sys, /dev, /tmp)
//  2. Configure hostname, networking
//  3. Start essential services
//  4. Sync users from SUPR
//  5. Start default shell or login
//
//  Services are managed similar to systemd targets:
//    basic.target → networking.target → multi-user.target
// ═══════════════════════════════════════════════════════════════════════════

#include "../kernel/types.h"

// ─── Service management ─────────────────────────────────────────────────

#define LINIT_MAX_SERVICES  24

enum LinuxServiceState {
    LSVC_INACTIVE = 0,
    LSVC_STARTING,
    LSVC_RUNNING,
    LSVC_STOPPING,
    LSVC_STOPPED,
    LSVC_FAILED
};

enum LinuxServiceType {
    LSVC_TYPE_ONESHOT = 0,    // Runs once, then marked completed
    LSVC_TYPE_DAEMON,         // Long-running background process
    LSVC_TYPE_VIRTUAL         // Virtual — just marks a target as reached
};

enum LinuxTarget {
    LTGT_SYSINIT = 0,         // Early boot
    LTGT_BASIC,               // Basic services
    LTGT_NETWORK,             // Networking
    LTGT_MULTIUSER,           // Full multi-user mode
    LTGT_GRAPHICAL            // With desktop integration
};

struct LinuxService {
    char              name[32];
    char              description[64];
    LinuxServiceState state;
    LinuxServiceType  type;
    LinuxTarget       target;
    int               pid;        // PID if daemon

    // Dependency — name of service that must run first
    char              after[32];

    // Function to call to start/stop
    void (*start_fn)();
    void (*stop_fn)();

    uint32_t          start_time;
    uint32_t          stop_time;
    int               restart_count;
    bool              enabled;
    bool              essential;   // System won't reach target without it
};

// ─── Init state ─────────────────────────────────────────────────────────

enum LinuxInitState {
    LINIT_OFF = 0,
    LINIT_BOOTING,
    LINIT_RUNNING,
    LINIT_SHUTTING_DOWN,
    LINIT_HALTED
};

// ─── Boot log ───────────────────────────────────────────────────────────
#define LINIT_LOG_MAX  64

struct LinitLogEntry {
    char     message[128];
    uint32_t timestamp;
    bool     ok;           // Green check or red X
};

// ═══════════════════════════════════════════════════════════════════════════
//  LinuxInit — PID 1 for the Linux subsystem
// ═══════════════════════════════════════════════════════════════════════════

class LinuxInit {
public:
    // ── Lifecycle ─────────────────────────────────────────────────────
    static void Init();
    static void Boot();         // Full boot sequence
    static void Shutdown();     // Graceful shutdown
    static void Reboot();       // Stop + start
    static void Halt();         // Emergency stop

    // ── Current state ─────────────────────────────────────────────────
    static LinuxInitState GetState();
    static LinuxTarget    GetCurrentTarget();
    static bool           IsRunning();

    // ── Service management ────────────────────────────────────────────
    static void RegisterService(const char* name, const char* desc,
                                 LinuxServiceType type, LinuxTarget target,
                                 void (*start)(), void (*stop)(),
                                 const char* after, bool essential);
    static int  StartService(const char* name);
    static int  StopService(const char* name);
    static int  RestartService(const char* name);
    static LinuxService* FindService(const char* name);
    static LinuxServiceState ServiceStatus(const char* name);

    // ── Query ─────────────────────────────────────────────────────────
    static LinuxService* GetServices();
    static int  GetServiceCount();
    static int  RunningServiceCount();

    // ── Boot log ──────────────────────────────────────────────────────
    static void LogBoot(const char* msg, bool ok);
    static LinitLogEntry* GetBootLog();
    static int  GetBootLogCount();

    // ── Shell integration ─────────────────────────────────────────────
    static void RegisterShellCommands(void* shell);

    // ── Status dump ───────────────────────────────────────────────────
    static void DumpStatus(char* out, int max_out);

private:
    static LinuxInitState state;
    static LinuxTarget    current_target;
    static LinuxService   services[LINIT_MAX_SERVICES];
    static int            service_count;
    static LinitLogEntry  boot_log[LINIT_LOG_MAX];
    static int            log_count;
    static uint32_t       boot_start_time;

    // Boot phases
    static void BootSysinit();
    static void BootBasic();
    static void BootNetwork();
    static void BootMultiUser();
    static void BootGraphical();

    // Start all services for a target
    static void StartTarget(LinuxTarget target);
    static void StopTarget(LinuxTarget target);

    // Default service functions
    static void svc_mount_proc();
    static void svc_mount_sys();
    static void svc_mount_dev();
    static void svc_mount_tmp();
    static void svc_hostname();
    static void svc_shared_mounts();
    static void svc_user_sync();
    static void svc_networking();
    static void svc_syslog();
    static void svc_cron();
    static void svc_sshd();
    static void svc_login();
    static void svc_desktop_bridge();

    // Shell command handlers
    static int cmd_systemctl(void* sh, int argc, const char** argv,
                              char* out, int mx);
    static int cmd_service(void* sh, int argc, const char** argv,
                            char* out, int mx);
    static int cmd_journalctl(void* sh, int argc, const char** argv,
                               char* out, int mx);
};
