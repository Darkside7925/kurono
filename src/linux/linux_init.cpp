//  kurono os  -  linux init system  -  implementation

#include "linux_init.h"
#include "linux_kernel.h"
#include "linux_devices.h"
#include "linux_signals.h"
#include "shared_mount.h"
#include "user_bridge.h"
#include "kls.h"
#include "../kernel/time.h"
#include "../kernel/heap.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../shell/shell.h"
#include "../net/network.h"

LinuxInitState  LinuxInit::state = LINIT_OFF;
LinuxTarget     LinuxInit::current_target = LTGT_SYSINIT;
LinuxService    LinuxInit::services[LINIT_MAX_SERVICES];
int             LinuxInit::service_count = 0;
LinitLogEntry   LinuxInit::boot_log[LINIT_LOG_MAX];
int             LinuxInit::log_count = 0;
uint32_t        LinuxInit::boot_start_time = 0;

static void li_scpy(char* d, const char* s, int mx) {
    int i = 0;
    while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static bool li_seq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

static int li_slen(const char* s) {
    int n = 0; while (s && s[n]) n++; return n;
}

static int li_pa(char* out, int pos, int mx, const char* s) {
    while (*s && pos < mx - 1) out[pos++] = *s++;
    out[pos] = 0;
    return pos;
}

//  init

void LinuxInit::Init() {
    memset(services, 0, sizeof(services));
    memset(boot_log, 0, sizeof(boot_log));
    service_count = 0;
    log_count = 0;
    state = LINIT_OFF;
    current_target = LTGT_SYSINIT;

    // register built-in services
    RegisterService("mount-proc",    "Mount /proc filesystem",
        LSVC_TYPE_ONESHOT, LTGT_SYSINIT, svc_mount_proc, nullptr, "", true);
    RegisterService("mount-sys",     "Mount /sys filesystem",
        LSVC_TYPE_ONESHOT, LTGT_SYSINIT, svc_mount_sys, nullptr, "mount-proc", true);
    RegisterService("mount-dev",     "Mount /dev filesystem",
        LSVC_TYPE_ONESHOT, LTGT_SYSINIT, svc_mount_dev, nullptr, "mount-proc", true);
    RegisterService("mount-tmp",     "Mount /tmp filesystem",
        LSVC_TYPE_ONESHOT, LTGT_SYSINIT, svc_mount_tmp, nullptr, "mount-dev", true);

    RegisterService("hostname",      "Set hostname",
        LSVC_TYPE_ONESHOT, LTGT_BASIC, svc_hostname, nullptr, "mount-proc", false);
    RegisterService("shared-mounts", "Mount shared Kurono directories",
        LSVC_TYPE_ONESHOT, LTGT_BASIC, svc_shared_mounts, nullptr, "mount-dev", true);
    RegisterService("user-sync",     "Synchronize SUPR users to /etc/passwd",
        LSVC_TYPE_ONESHOT, LTGT_BASIC, svc_user_sync, nullptr, "shared-mounts", true);

    RegisterService("networking",    "Network configuration",
        LSVC_TYPE_ONESHOT, LTGT_NETWORK, svc_networking, nullptr, "shared-mounts", false);

    RegisterService("rsyslog",       "System logging daemon",
        LSVC_TYPE_DAEMON, LTGT_MULTIUSER, svc_syslog, nullptr, "networking", false);
    RegisterService("cron",          "Task scheduler",
        LSVC_TYPE_DAEMON, LTGT_MULTIUSER, svc_cron, nullptr, "rsyslog", false);
    RegisterService("sshd",          "OpenSSH server",
        LSVC_TYPE_DAEMON, LTGT_MULTIUSER, svc_sshd, nullptr, "networking", false);
    RegisterService("login",         "Login service",
        LSVC_TYPE_DAEMON, LTGT_MULTIUSER, svc_login, nullptr, "user-sync", false);

    RegisterService("desktop-bridge","Kurono desktop integration",
        LSVC_TYPE_DAEMON, LTGT_GRAPHICAL, svc_desktop_bridge, nullptr, "login", false);

    SerialLogger::Log("[LinuxInit] Init system ready (");
    SerialLogger::LogDec(service_count);
    SerialLogger::Log(" services registered)\r\n");
}

//  boot

void LinuxInit::Boot() {
    if (state == LINIT_RUNNING) return;
    state = LINIT_BOOTING;
    boot_start_time = Time::GetTicks();

    SerialLogger::Log("\r\n");
    SerialLogger::Log("═══════════════════════════════════════════════════════════\r\n");
    SerialLogger::Log("  Kurono Linux Subsystem  -  Booting...\r\n");
    SerialLogger::Log("═══════════════════════════════════════════════════════════\r\n");

    LogBoot("Starting Kurono Linux Subsystem...", true);

    BootSysinit();
    BootBasic();
    BootNetwork();
    BootMultiUser();
    BootGraphical();

    state = LINIT_RUNNING;

    uint32_t elapsed = Time::GetTicks() - boot_start_time;
    char msg[128] = "Boot complete in ";
    int p = li_slen(msg);
    char tmp[12]; int i = 0;
    int ms = (int)elapsed;
    if (ms == 0) { tmp[0] = '0'; i = 1; }
    else { while (ms > 0) { tmp[i++] = '0' + (ms % 10); ms /= 10; } }
    for (int j = i - 1; j >= 0; j--) msg[p++] = tmp[j];
    msg[p++] = 'm'; msg[p++] = 's'; msg[p] = 0;
    LogBoot(msg, true);

    SerialLogger::Log("[LinuxInit] ");
    SerialLogger::Log(msg);
    SerialLogger::Log("\r\n");
    SerialLogger::Log("═══════════════════════════════════════════════════════════\r\n");
}

void LinuxInit::BootSysinit() {
    LogBoot("Reached sysinit.target", true);
    StartTarget(LTGT_SYSINIT);
    current_target = LTGT_SYSINIT;
}

void LinuxInit::BootBasic() {
    LogBoot("Reached basic.target", true);
    StartTarget(LTGT_BASIC);
    current_target = LTGT_BASIC;
}

void LinuxInit::BootNetwork() {
    LogBoot("Reached network.target", true);
    StartTarget(LTGT_NETWORK);
    current_target = LTGT_NETWORK;
}

void LinuxInit::BootMultiUser() {
    LogBoot("Reached multi-user.target", true);
    StartTarget(LTGT_MULTIUSER);
    current_target = LTGT_MULTIUSER;
}

void LinuxInit::BootGraphical() {
    LogBoot("Reached graphical.target  -  Kurono desktop integration active", true);
    StartTarget(LTGT_GRAPHICAL);
    current_target = LTGT_GRAPHICAL;
}

//  shutdown / reboot

void LinuxInit::Shutdown() {
    if (state != LINIT_RUNNING && state != LINIT_BOOTING) return;
    state = LINIT_SHUTTING_DOWN;

    SerialLogger::Log("[LinuxInit] Shutting down...\r\n");
    LogBoot("System going down...", true);

    // stop targets in reverse order
    StopTarget(LTGT_GRAPHICAL);
    StopTarget(LTGT_MULTIUSER);
    StopTarget(LTGT_NETWORK);
    StopTarget(LTGT_BASIC);
    StopTarget(LTGT_SYSINIT);

    state = LINIT_HALTED;
    LogBoot("System halted", true);
    SerialLogger::Log("[LinuxInit] System halted\r\n");
}

void LinuxInit::Reboot() {
    Shutdown();
    Boot();
}

void LinuxInit::Halt() {
    // emergency: stop everything immediately
    for (int i = 0; i < service_count; i++) {
        if (services[i].state == LSVC_RUNNING) {
            services[i].state = LSVC_STOPPED;
        }
    }
    state = LINIT_HALTED;
    SerialLogger::Log("[LinuxInit] EMERGENCY HALT\r\n");
}

//  service management

void LinuxInit::RegisterService(const char* name, const char* desc,
                                 LinuxServiceType type, LinuxTarget target,
                                 void (*start)(), void (*stop)(),
                                 const char* after, bool essential) {
    if (service_count >= LINIT_MAX_SERVICES) return;
    LinuxService* svc = &services[service_count++];
    li_scpy(svc->name, name, sizeof(svc->name));
    li_scpy(svc->description, desc, sizeof(svc->description));
    svc->state = LSVC_INACTIVE;
    svc->type = type;
    svc->target = target;
    svc->start_fn = start;
    svc->stop_fn = stop;
    li_scpy(svc->after, after ? after : "", sizeof(svc->after));
    svc->essential = essential;
    svc->enabled = true;
    svc->pid = 0;
    svc->restart_count = 0;
}

void LinuxInit::StartTarget(LinuxTarget target) {
    // start all services in this target, respecting ordering
    bool changed = true;
    int iterations = 0;

    while (changed && iterations < 50) {
        changed = false;
        iterations++;

        for (int i = 0; i < service_count; i++) {
            LinuxService* svc = &services[i];
            if (svc->target != target) continue;
            if (!svc->enabled) continue;
            if (svc->state != LSVC_INACTIVE) continue;

            // check dependency
            if (svc->after[0] != 0) {
                LinuxService* dep = FindService(svc->after);
                if (dep && dep->state != LSVC_RUNNING && dep->state != LSVC_STOPPED) {
                    continue;  // dependency not met yet
                }
            }

            // start service
            StartService(svc->name);
            changed = true;
        }
    }
}

void LinuxInit::StopTarget(LinuxTarget target) {
    // stop services in reverse order
    for (int i = service_count - 1; i >= 0; i--) {
        if (services[i].target == target && services[i].state == LSVC_RUNNING) {
            StopService(services[i].name);
        }
    }
}

int LinuxInit::StartService(const char* name) {
    LinuxService* svc = FindService(name);
    if (!svc) return -1;
    if (svc->state == LSVC_RUNNING) return 0;

    svc->state = LSVC_STARTING;
    svc->start_time = Time::GetTicks();

    char msg[128];
    int p = 0;
    p = li_pa(msg, p, sizeof(msg), "Starting ");
    p = li_pa(msg, p, sizeof(msg), svc->description);
    p = li_pa(msg, p, sizeof(msg), "...");
    LogBoot(msg, true);

    if (svc->start_fn) {
        svc->start_fn();
    }

    if (svc->type == LSVC_TYPE_ONESHOT) {
        svc->state = LSVC_STOPPED;  // oneshot runs and completes
    } else {
        svc->state = LSVC_RUNNING;
    }

    p = 0;
    p = li_pa(msg, p, sizeof(msg), "  [OK] ");
    p = li_pa(msg, p, sizeof(msg), svc->name);
    SerialLogger::Log(msg);
    SerialLogger::Log("\r\n");

    return 0;
}

int LinuxInit::StopService(const char* name) {
    LinuxService* svc = FindService(name);
    if (!svc) return -1;
    if (svc->state != LSVC_RUNNING) return 0;

    svc->state = LSVC_STOPPING;
    svc->stop_time = Time::GetTicks();

    if (svc->stop_fn) {
        svc->stop_fn();
    }

    svc->state = LSVC_STOPPED;
    return 0;
}

int LinuxInit::RestartService(const char* name) {
    StopService(name);
    LinuxService* svc = FindService(name);
    if (svc) {
        svc->state = LSVC_INACTIVE;  // reset for re-start
        svc->restart_count++;
    }
    return StartService(name);
}

LinuxService* LinuxInit::FindService(const char* name) {
    for (int i = 0; i < service_count; i++) {
        if (li_seq(services[i].name, name))
            return &services[i];
    }
    return nullptr;
}

LinuxServiceState LinuxInit::ServiceStatus(const char* name) {
    LinuxService* svc = FindService(name);
    return svc ? svc->state : LSVC_INACTIVE;
}

LinuxService* LinuxInit::GetServices() { return services; }
int LinuxInit::GetServiceCount() { return service_count; }
LinuxInitState LinuxInit::GetState() { return state; }
LinuxTarget LinuxInit::GetCurrentTarget() { return current_target; }
bool LinuxInit::IsRunning() { return state == LINIT_RUNNING; }

int LinuxInit::RunningServiceCount() {
    int c = 0;
    for (int i = 0; i < service_count; i++) {
        if (services[i].state == LSVC_RUNNING) c++;
    }
    return c;
}

//  boot log

void LinuxInit::LogBoot(const char* msg, bool ok) {
    if (log_count >= LINIT_LOG_MAX) return;
    LinitLogEntry* e = &boot_log[log_count++];
    li_scpy(e->message, msg, sizeof(e->message));
    e->timestamp = Time::GetTicks();
    e->ok = ok;
}

LinitLogEntry* LinuxInit::GetBootLog() { return boot_log; }
int LinuxInit::GetBootLogCount() { return log_count; }

//  service implementations

void LinuxInit::svc_mount_proc() {
    LinuxKernel::InitProcFS();
    KVFS::Mkdirs("/proc");
}

void LinuxInit::svc_mount_sys() {
    LinuxKernel::InitSysFS();
    KVFS::Mkdirs("/sys");
}

void LinuxInit::svc_mount_dev() {
    LinuxDeviceBridge::Init();
}

void LinuxInit::svc_mount_tmp() {
    KVFS::Mkdirs("/tmp");
    KVFS::Mkdirs("/run");
    KVFS::Mkdirs("/run/lock");
}

void LinuxInit::svc_hostname() {
    KLSConfig* cfg = KLS::GetConfig();
    KVFS::WriteString("/linux/etc/hostname", cfg->hostname);
    KVFS::WriteString("/proc/sys/kernel/hostname", cfg->hostname);

    // also write to /sys
    LinuxKernel::WriteSys("/sys/kernel/hostname", cfg->hostname, li_slen(cfg->hostname));
}

void LinuxInit::svc_shared_mounts() {
    SharedMountMgr::Init();
    SharedMountMgr::MountDefaults();
}

void LinuxInit::svc_user_sync() {
    UserBridge::Init();
    UserBridge::SetDirection(UB_BIDIRECTIONAL);
    UserBridge::SetConflictPolicy(UB_SUPR_WINS);
    UserBridge::Sync();
}

void LinuxInit::svc_networking() {
    // write /etc/resolv.conf
    KVFS::WriteString("/linux/etc/resolv.conf",
        "# Generated by Kurono Linux Init\n"
        "nameserver 8.8.8.8\n"
        "nameserver 8.8.4.4\n");

    // write /etc/network/interfaces
    KVFS::WriteString("/linux/etc/network/interfaces",
        "# Managed by Kurono\n"
        "auto lo\n"
        "iface lo inet loopback\n\n"
        "auto eth0\n"
        "iface eth0 inet dhcp\n");
}

void LinuxInit::svc_syslog() {
    // virtual syslog  -  writes to serial
    KVFS::Mkdirs("/var/log");
    KVFS::WriteString("/var/log/syslog",
        "Kurono Linux  -  syslog started\n");
}

void LinuxInit::svc_cron() {
    // virtual cron daemon
    KVFS::Mkdirs("/var/spool/cron");
}

void LinuxInit::svc_sshd() {
    // virtual sshd  -  registers but doesn't truly listen
    KVFS::Mkdirs("/etc/ssh");
    KVFS::WriteString("/etc/ssh/sshd_config",
        "# Kurono SSH\n"
        "Port 22\n"
        "PermitRootLogin no\n"
        "PasswordAuthentication yes\n");
}

void LinuxInit::svc_login() {
    // login service  -  creates /etc/login.defs
    KVFS::WriteString("/linux/etc/login.defs",
        "MAIL_DIR /var/mail\n"
        "LOG_OK_LOGINS no\n"
        "UMASK 022\n"
        "PASS_MAX_DAYS 99999\n"
        "PASS_MIN_DAYS 0\n"
        "PASS_WARN_AGE 7\n"
        "UID_MIN 1000\n"
        "UID_MAX 60000\n"
        "GID_MIN 1000\n"
        "GID_MAX 60000\n"
        "HOME_MODE 0755\n");
}

void LinuxInit::svc_desktop_bridge() {
    // desktop bridge  -  linux apps can open windows in kurono wm
    SerialLogger::Log("[LinuxInit] Desktop bridge active  -  "
                      "Linux GUI apps integrated with Kurono WM\r\n");
}

//  shell integration

void LinuxInit::RegisterShellCommands(void* shell) {
    Shell* sh = (Shell*)shell;
    if (!sh) return;
    sh->RegisterCommand("systemctl", cmd_systemctl, "Linux service management");
    sh->RegisterCommand("service",   cmd_service,   "Control Linux services");
    sh->RegisterCommand("journalctl",cmd_journalctl,"View boot logs");
}

int LinuxInit::cmd_systemctl(void* sh, int argc, const char** argv,
                              char* out, int mx) {
    (void)sh;
    int p = 0;

    if (argc < 2) {
        // list all services
        for (int i = 0; i < service_count; i++) {
            LinuxService* svc = &services[i];
            const char* st;
            switch (svc->state) {
                case LSVC_INACTIVE: st = "inactive"; break;
                case LSVC_STARTING: st = "starting"; break;
                case LSVC_RUNNING:  st = "running "; break;
                case LSVC_STOPPING: st = "stopping"; break;
                case LSVC_STOPPED:  st = "stopped "; break;
                case LSVC_FAILED:   st = "failed  "; break;
                default: st = "unknown "; break;
            }
            p = li_pa(out, p, mx, "  ");
            p = li_pa(out, p, mx, st);
            p = li_pa(out, p, mx, "  ");
            p = li_pa(out, p, mx, svc->name);
            p = li_pa(out, p, mx, "\t");
            p = li_pa(out, p, mx, svc->description);
            p = li_pa(out, p, mx, "\n");
        }
        return p;
    }

    const char* action = argv[1];
    if (argc >= 3) {
        const char* sname = argv[2];
        if (li_seq(action, "start"))   { StartService(sname); }
        else if (li_seq(action, "stop"))    { StopService(sname); }
        else if (li_seq(action, "restart")) { RestartService(sname); }
        else if (li_seq(action, "status"))  {
            LinuxService* svc = FindService(sname);
            if (svc) {
                p = li_pa(out, p, mx, svc->name);
                p = li_pa(out, p, mx, "  -  ");
                p = li_pa(out, p, mx, svc->description);
                p = li_pa(out, p, mx, "\n  State: ");
                const char* st;
                switch (svc->state) {
                    case LSVC_RUNNING: st = "active (running)"; break;
                    case LSVC_STOPPED: st = "inactive (dead)"; break;
                    default: st = "unknown"; break;
                }
                p = li_pa(out, p, mx, st);
                p = li_pa(out, p, mx, "\n");
            } else {
                p = li_pa(out, p, mx, "Service not found: ");
                p = li_pa(out, p, mx, sname);
                p = li_pa(out, p, mx, "\n");
            }
        }
    } else if (li_seq(action, "list-units")) {
        return cmd_systemctl(sh, 1, argv, out, mx);
    }

    return p;
}

int LinuxInit::cmd_service(void* sh, int argc, const char** argv,
                            char* out, int mx) {
    // wrapper: service <name> start/stop/status
    if (argc < 3) {
        return li_pa(out, 0, mx, "Usage: service <name> start|stop|restart|status\n");
    }
    const char* new_argv[4] = { "systemctl", argv[2], argv[1], nullptr };
    return cmd_systemctl(sh, 3, new_argv, out, mx);
}

int LinuxInit::cmd_journalctl(void*, int, const char**, char* out, int mx) {
    int p = 0;
    p = li_pa(out, p, mx, "-- Kurono Linux Boot Log --\n");
    for (int i = 0; i < log_count; i++) {
        p = li_pa(out, p, mx, boot_log[i].ok ? "[  OK  ] " : "[FAILED] ");
        p = li_pa(out, p, mx, boot_log[i].message);
        p = li_pa(out, p, mx, "\n");
    }
    return p;
}

//  status dump

void LinuxInit::DumpStatus(char* out, int max_out) {
    int p = 0;
    p = li_pa(out, p, max_out, "Linux Init System  -  ");
    switch (state) {
        case LINIT_OFF: p = li_pa(out, p, max_out, "OFF"); break;
        case LINIT_BOOTING: p = li_pa(out, p, max_out, "BOOTING"); break;
        case LINIT_RUNNING: p = li_pa(out, p, max_out, "RUNNING"); break;
        case LINIT_SHUTTING_DOWN: p = li_pa(out, p, max_out, "SHUTTING DOWN"); break;
        case LINIT_HALTED: p = li_pa(out, p, max_out, "HALTED"); break;
    }
    p = li_pa(out, p, max_out, "\n\nServices:\n");

    for (int i = 0; i < service_count; i++) {
        LinuxService* svc = &services[i];
        p = li_pa(out, p, max_out, "  ");
        p = li_pa(out, p, max_out, svc->state == LSVC_RUNNING ? "[*]" :
                                   svc->state == LSVC_STOPPED ? "[ ]" : "[?]");
        p = li_pa(out, p, max_out, " ");
        p = li_pa(out, p, max_out, svc->name);
        p = li_pa(out, p, max_out, "\n");
    }
    out[p] = 0;
}
