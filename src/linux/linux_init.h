#pragma once
//  kurono os  -  linux init system
//  bootstraps the linux subsystem like a real init/systemd process.
//  this is pid 1 of the linux world inside kurono.
//
//  boot sequence:
//  1. mount virtual filesystems (/proc, /sys, /dev, /tmp)
//  2. configure hostname, networking
//  3. start essential services
//  4. sync users from supr
//  5. start default shell or login
//
//  services are managed similar to systemd targets:
//    basic.target → networking.target → multi-user.target

#include "../kernel/types.h"

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
    LSVC_TYPE_ONESHOT = 0,    // runs once, then marked completed
    LSVC_TYPE_DAEMON,         // long-running background process
    LSVC_TYPE_VIRTUAL         // virtual  -  just marks a target as reached
};

enum LinuxTarget {
    LTGT_SYSINIT = 0,         // early boot
    LTGT_BASIC,               // basic services
    LTGT_NETWORK,             // networking
    LTGT_MULTIUSER,           // full multi-user mode
    LTGT_GRAPHICAL            // with desktop integration
};

struct LinuxService {
    char              name[32];
    char              description[64];
    LinuxServiceState state;
    LinuxServiceType  type;
    LinuxTarget       target;
    int               pid;        // pid if daemon

    // dependency  -  name of service that must run first
    char              after[32];

    // function to call to start/stop
    void (*start_fn)();
    void (*stop_fn)();

    uint32_t          start_time;
    uint32_t          stop_time;
    int               restart_count;
    bool              enabled;
    bool              essential;   // system won't reach target without it
};

enum LinuxInitState {
    LINIT_OFF = 0,
    LINIT_BOOTING,
    LINIT_RUNNING,
    LINIT_SHUTTING_DOWN,
    LINIT_HALTED
};

#define LINIT_LOG_MAX  64

struct LinitLogEntry {
    char     message[128];
    uint32_t timestamp;
    bool     ok;           // green check or red x
};

//  linuxinit  -  pid 1 for the linux subsystem

class LinuxInit {
public:
    static void Init();
    static void Boot();         // full boot sequence
    static void Shutdown();     // graceful shutdown
    static void Reboot();       // stop + start
    static void Halt();         // emergency stop

    static LinuxInitState GetState();
    static LinuxTarget    GetCurrentTarget();
    static bool           IsRunning();

    static void RegisterService(const char* name, const char* desc,
                                 LinuxServiceType type, LinuxTarget target,
                                 void (*start)(), void (*stop)(),
                                 const char* after, bool essential);
    static int  StartService(const char* name);
    static int  StopService(const char* name);
    static int  RestartService(const char* name);
    static LinuxService* FindService(const char* name);
    static LinuxServiceState ServiceStatus(const char* name);

    static LinuxService* GetServices();
    static int  GetServiceCount();
    static int  RunningServiceCount();

    static void LogBoot(const char* msg, bool ok);
    static LinitLogEntry* GetBootLog();
    static int  GetBootLogCount();

    static void RegisterShellCommands(void* shell);

    static void DumpStatus(char* out, int max_out);

private:
    static LinuxInitState state;
    static LinuxTarget    current_target;
    static LinuxService   services[LINIT_MAX_SERVICES];
    static int            service_count;
    static LinitLogEntry  boot_log[LINIT_LOG_MAX];
    static int            log_count;
    static uint32_t       boot_start_time;

    // boot phases
    static void BootSysinit();
    static void BootBasic();
    static void BootNetwork();
    static void BootMultiUser();
    static void BootGraphical();

    // start all services for a target
    static void StartTarget(LinuxTarget target);
    static void StopTarget(LinuxTarget target);

    // default service functions
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

    // shell command handlers
    static int cmd_systemctl(void* sh, int argc, const char** argv,
                              char* out, int mx);
    static int cmd_service(void* sh, int argc, const char** argv,
                            char* out, int mx);
    static int cmd_journalctl(void* sh, int argc, const char** argv,
                               char* out, int mx);
};
