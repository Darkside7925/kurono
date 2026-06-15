#pragma once
#include "../kernel/types.h"

//  kurono os: kinit, the service + init manager (kurono's answer to systemd).
//
//  kinit is the supervisor for kurono's background services. it is NOT a
//  from-scratch unix init: kurono's core services (kdbus, kwayland, kaudio,
//  knet, klog) are in-kernel kernel-processes that already run, so kinit
//  SUPERVISES two kinds of unit honestly:
//
//    KUNIT_INKERNEL  = an existing in-kernel subsystem (dbus / wayland / audio /
//                      network / logging). kinit tracks its state and can
//                      "restart" it via a registered hook, but it is not a
//                      separate address space. these are marked so status / docs
//                      never pretend they are isolated processes. (satoru)
//
//    KUNIT_PROCESS   = a genuine isolated linux user process spawned via
//                      fork/exec (LinuxSyscall). the kpkg-daemon is the first
//                      real one: it downloads + installs in its own process so
//                      the gui never blocks. requires the dynamic-exec path
//                      (ld-kurono ExecPIE) to be runtime-working; until then the
//                      spawn is attempted and its failure is logged honestly.
//
//  features: boot targets (dependency-sequenced stages), a crash monitor with
//  exponential backoff (2s,4s,8s..max 60s; 5 crashes/60s -> failed + alert;
//  critical kdbus/kwayland failures raise a desktop notification), capability
//  gating at spawn (network/filesystem/gui via supr/ksa), and an audit log of
//  every start/stop/crash/restart to /kurono/var/log/services.log. (satoru)

namespace KInit {

constexpr int KINIT_MAX_SERVICES = 32;
constexpr int KINIT_NAME_LEN     = 32;
constexpr int KINIT_PATH_LEN     = 160;
constexpr int KINIT_DEP_LEN      = 96;   // "dbus network" style after= list (satoru)

// the supervised-unit kind (see file header). (satoru)
enum KUnitKind : uint8_t {
    KUNIT_INKERNEL = 0,   // an existing in-kernel kernel-process / server (satoru)
    KUNIT_PROCESS  = 1,   // a real isolated linux user process (fork/exec) (satoru)
    KUNIT_ONESHOT  = 2     // runs once at its target then completes (satoru)
};

// restart policy parsed from a .kservice [Service] Restart= key. (satoru)
enum KRestartPolicy : uint8_t {
    KRESTART_NO = 0,        // never restart (satoru)
    KRESTART_ON_FAILURE,    // restart only on crash / nonzero exit (satoru)
    KRESTART_ALWAYS         // always restart when it stops (satoru)
};

// observable lifecycle state. (satoru)
enum KServiceState : uint8_t {
    KSVC_INACTIVE = 0,   // registered, not started (satoru)
    KSVC_STARTING,       // start in progress (satoru)
    KSVC_RUNNING,        // up (satoru)
    KSVC_STOPPING,       // stop in progress (satoru)
    KSVC_STOPPED,        // cleanly stopped / oneshot completed (satoru)
    KSVC_FAILED,         // crashed too many times; gave up (satoru)
    KSVC_RESTARTING      // crashed, waiting out the backoff delay (satoru)
};

// boot stages. each target starts only after every service in the prior target
// has reached running (daemons) or stopped (oneshots). (satoru)
enum KTarget : uint8_t {
    KTGT_KERNEL = 0,   // logging + crash handler (satoru)
    KTGT_NETWORK,      // knet / dhcp / routing (satoru)
    KTGT_DBUS,         // kdbus (satoru)
    KTGT_DESKTOP,      // kwayland, kaudio (satoru)
    KTGT_USER,         // kpkg-daemon, kupdate, ksecurity (satoru)
    KTGT_COUNT
};

// capability grants parsed from the [Capabilities] section. enforced at spawn
// via supr/ksa; an in-kernel unit ignores them (it already runs in the kernel)
// but they are recorded + logged for honesty. (satoru)
struct KCapabilities {
    bool network;      // may use the network stack (satoru)
    bool filesystem;   // may touch the kvfs tree beyond its own dirs (satoru)
    bool gui;          // may open windows / talk to the compositor (satoru)
};

// the start/stop hooks for an in-kernel unit. for a KUNIT_PROCESS these are
// null and Exec is used instead. (satoru)
typedef void (*KInkernelHook)();

struct KService {
    char           name[KINIT_NAME_LEN];
    char           description[64];
    char           exec[KINIT_PATH_LEN];     // KUNIT_PROCESS: argv0 + args (satoru)
    char           after[KINIT_DEP_LEN];     // space-separated dep names (satoru)
    KUnitKind      kind;
    KTarget        target;                    // WantedBy= -> the boot stage (satoru)
    KRestartPolicy restart;
    uint32_t       restart_delay_ms;          // base backoff (default 2000) (satoru)
    KCapabilities  caps;
    bool           critical;                  // kdbus/kwayland -> notify on fail (satoru)
    bool           enabled;                   // disabled services are skipped (satoru)

    // in-kernel hooks (KUNIT_INKERNEL only). (satoru)
    KInkernelHook  start_fn;
    KInkernelHook  stop_fn;
    // probe whether an in-kernel unit is actually alive; null = assume the
    // start_fn succeeded. used by the crash monitor to detect a dead subsystem
    // without a real exit code. returns true if healthy. (satoru)
    bool         (*health_fn)();

    // ── live state ──────────────────────────────────────────────────────
    KServiceState  state;
    int            pid;                       // KUNIT_PROCESS linux pid, else 0 (satoru)
    uint32_t       start_time_ms;
    uint32_t       stop_time_ms;

    // crash bookkeeping for the backoff + 5-in-60s rule. (satoru)
    int            crash_count;               // total crashes since boot (satoru)
    int            crash_burst;               // crashes inside the rolling 60s window (satoru)
    uint32_t       burst_window_start_ms;     // start of the current 60s window (satoru)
    uint32_t       next_restart_ms;           // when KSVC_RESTARTING may relaunch (satoru)
    uint32_t       cur_backoff_ms;            // current backoff delay (doubles) (satoru)
};

// ── lifecycle ──────────────────────────────────────────────────────────────
void Init();                 // register built-in services + parse .kservice dir (satoru)
void Boot();                 // sequence all targets in order (satoru)
void Tick();                 // crash monitor: relaunch due restarts, probe health (satoru)
void Shutdown();             // stop all services in reverse target order (satoru)

// ── service control (used by the shell + boot) ──────────────────────────────
int  StartService(const char* name);
int  StopService(const char* name);
int  RestartService(const char* name);
int  EnableService(const char* name);
int  DisableService(const char* name);
void Reload();               // re-parse the .kservice directory (satoru)

KService* FindService(const char* name);
KService* GetServices();
int       GetServiceCount();
int       RunningCount();

// ── boot targets ────────────────────────────────────────────────────────────
void StartTarget(KTarget t);
void StopTarget(KTarget t);
const char* TargetName(KTarget t);
const char* StateName(KServiceState s);

// ── registration (programmatic; the parser + built-ins both use this) ────────
// returns the new service index, or -1 if full / duplicate name. (satoru)
int RegisterInkernel(const char* name, const char* desc, KTarget target,
                     KInkernelHook start, KInkernelHook stop, bool (*health)(),
                     const char* after, KRestartPolicy restart,
                     uint32_t restart_delay_ms, bool critical);
int RegisterProcess(const char* name, const char* desc, KTarget target,
                    const char* exec, const char* after, KRestartPolicy restart,
                    uint32_t restart_delay_ms, KCapabilities caps, bool critical);

// ── .kservice parsing ────────────────────────────────────────────────────────
// parse a single .kservice text blob into `out` (state fields zeroed). returns
// true if a [Service] Name= was found. the directory loader is LoadServiceDir.
// (satoru)
bool ParseKService(const char* text, int len, KService* out);
// scan /kurono/system/services/*.kservice and register each. returns count
// registered. (satoru)
int  LoadServiceDir();

// ── shell integration ────────────────────────────────────────────────────────
// kinit status|start|stop|restart|enable|disable|logs|reload (satoru)
int  CmdKinit(void* sh, int argc, const char** argv, char* out, int mx);
void RegisterShellCommands(void* shell);

// ── audit log ─────────────────────────────────────────────────────────────────
// append "<uptime_ms> <event> <service> <detail>" to services.log. (satoru)
void LogEvent(const char* event, const char* service, const char* detail);

}  // namespace KInit

// end (satoru)
