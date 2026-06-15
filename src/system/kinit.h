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

constexpr int KINIT_MAX_SERVICES = 48;   // raised for templates + user-session units (satoru)
constexpr int KINIT_NAME_LEN     = 32;
constexpr int KINIT_PATH_LEN     = 160;
constexpr int KINIT_DEP_LEN      = 96;   // "dbus network" style after= list (satoru)
constexpr int KINIT_USER_LEN     = 32;   // owning user for a user-session unit (satoru)
constexpr int KINIT_SOCK_LEN     = 108;  // AF_UNIX path for socket activation (satoru)

// the supervised-unit kind (see file header). (satoru)
enum KUnitKind : uint8_t {
    KUNIT_INKERNEL = 0,   // an existing in-kernel kernel-process / server (satoru)
    KUNIT_PROCESS  = 1,   // a real isolated linux user process (fork/exec) (satoru)
    KUNIT_ONESHOT  = 2     // runs once at its target then completes (satoru)
};

// systemd-style service Type=. controls when a service counts as "started":
//  SIMPLE  -> immediately on spawn (the historical default).
//  NOTIFY  -> only after it sends sd_notify READY=1 (see §sd_notify). (satoru)
enum KServiceType : uint8_t {
    KTYPE_SIMPLE = 0,
    KTYPE_NOTIFY = 1
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

// optional per-service current-memory probe (kib). for a process unit kinit
// reads the scheduler snapshot by pid; an in-kernel unit may register this to
// report its own tracked allocation so MemoryMax can be enforced honestly. a
// null probe means "no MemoryMax enforcement for this unit". (satoru)
typedef uint32_t (*KMemProbe)();

// resource limits parsed from [Service]/[Limits]. zero = unlimited. (satoru)
struct KLimits {
    uint32_t memory_max_kb;    // MemoryMax= (accepts K/M/G suffix); 0 = unlimited (satoru)
    uint32_t cpu_quota_pct;    // CPUQuota= as a percent (e.g. 50 for "50%"); 0 = unlimited (satoru)
    uint32_t limit_nofile;     // LimitNOFILE= max open fds; 0 = default (satoru)
};

struct KService {
    char           name[KINIT_NAME_LEN];
    char           description[64];
    char           exec[KINIT_PATH_LEN];     // KUNIT_PROCESS: argv0 + args (satoru)
    char           after[KINIT_DEP_LEN];     // space-separated dep names (satoru)
    KUnitKind      kind;
    KServiceType   type;                      // simple | notify (satoru)
    KTarget        target;                    // WantedBy= -> the boot stage (satoru)
    KRestartPolicy restart;
    uint32_t       restart_delay_ms;          // base backoff (default 2000) (satoru)
    KCapabilities  caps;
    bool           critical;                  // kdbus/kwayland -> notify on fail (satoru)
    bool           enabled;                   // disabled services are skipped (satoru)

    // ── resource limits (satoru) ──────────────────────────────────────────
    KLimits        limits;
    KMemProbe      mem_probe;                 // in-kernel current-mem reporter (satoru)

    // ── socket activation (satoru) ──────────────────────────────────────────
    // a unit with listen_path set is NOT started at its target; kinit binds the
    // AF_UNIX socket itself and only spawns the service on the first connect. (satoru)
    char           listen_path[KINIT_SOCK_LEN];
    int            listen_sd;                 // kinit-owned listening sd, -1 if none (satoru)
    bool           socket_activated;          // true once a connect triggered the spawn (satoru)

    // ── sd_notify + watchdog (satoru) ─────────────────────────────────────
    uint32_t       watchdog_sec;              // WatchdogSec=; 0 = no watchdog (satoru)
    uint32_t       last_watchdog_ms;          // last WATCHDOG=1 seen (satoru)
    bool           notify_ready;              // READY=1 received (notify type) (satoru)
    char           notify_status[64];         // last STATUS= text (satoru)

    // ── templates + user sessions (satoru) ─────────────────────────────────
    bool           is_template;               // "foo@.kservice" definition, never run directly (satoru)
    bool           is_instance;               // "foo@bar" instantiated from a template (satoru)
    char           instance[KINIT_NAME_LEN];  // the %i value for an instance (satoru)
    char           owner_user[KINIT_USER_LEN];// non-empty -> user-session unit (satoru)

    // ── isolation (satoru) ─────────────────────────────────────────────────
    bool           isolate;                   // run in its own address space if safe (satoru)
    bool           isolated_active;           // true once isolation framework is live (satoru)

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
    uint32_t       ready_time_ms;             // when it reached running (for analyze) (satoru)
    uint32_t       boot_ms;                   // measured start->running time (analyze) (satoru)

    // crash bookkeeping for the backoff + 5-in-60s rule. (satoru)
    int            crash_count;               // total crashes since boot (satoru)
    int            crash_burst;               // crashes inside the rolling 60s window (satoru)
    uint32_t       burst_window_start_ms;     // start of the current 60s window (satoru)
    uint32_t       next_restart_ms;           // when KSVC_RESTARTING may relaunch (satoru)
    uint32_t       cur_backoff_ms;            // current backoff delay (doubles) (satoru)
    int            oom_kills;                  // times MemoryMax killed it (satoru)
    int            watchdog_kills;             // times the watchdog killed it (satoru)
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
// StartTarget brings up a stage's services respecting direct After= deps.
// parallel=true starts every dep-ready unit in this target concurrently (the
// supervisors/start-hooks run on their own kernel-processes), then waits for the
// stage to settle; parallel=false is the historical one-at-a-time fixpoint. the
// default StartTarget(t) is parallel. (satoru)
void StartTarget(KTarget t);
void StartTarget(KTarget t, bool parallel);
void StopTarget(KTarget t);
const char* TargetName(KTarget t);
const char* StateName(KServiceState s);

// ── dependency cycle detection (satoru) ──────────────────────────────────────
// scan the After= graph for a circular dependency. logs a clear error for every
// cycle found and BREAKS it (drops the back-edge) so boot never deadlocks.
// returns the number of cycles broken. called once at the end of Init(). (satoru)
int DetectAndBreakCycles();

// ── registration (programmatic; the parser + built-ins both use this) ────────
// returns the new service index, or -1 if full / duplicate name. (satoru)
int RegisterInkernel(const char* name, const char* desc, KTarget target,
                     KInkernelHook start, KInkernelHook stop, bool (*health)(),
                     const char* after, KRestartPolicy restart,
                     uint32_t restart_delay_ms, bool critical);
int RegisterProcess(const char* name, const char* desc, KTarget target,
                    const char* exec, const char* after, KRestartPolicy restart,
                    uint32_t restart_delay_ms, KCapabilities caps, bool critical);

// register a fully-populated KService (state fields are reset). used by the
// .kservice loader so every parsed key (limits, sockets, notify, template,
// owner, isolate) lands without widening the positional helpers above. returns
// the new index or -1 (full / duplicate name). (satoru)
int RegisterService(const KService* tmpl);

// ── sd_notify (satoru) ────────────────────────────────────────────────────────
// the in-kernel entry to the sd_notify state machine. a process unit reaches
// this by sending a datagram to its NOTIFY_SOCKET (kinit's listener parses it
// and calls through here); an in-kernel daemon calls it directly with its own
// name. msg is the newline-separated sd_notify payload, e.g. "READY=1",
// "WATCHDOG=1", "STATUS=working". returns true if the named service was found.
// (satoru)
bool SdNotify(const char* service, const char* msg);
// the canonical NOTIFY_SOCKET path kinit listens on. (satoru)
const char* NotifySocketPath();

// ── socket activation (satoru) ────────────────────────────────────────────────
// poll all socket-activated units; spawn any whose listening socket got a
// connection. called from Tick(). (satoru)
void PollSocketActivation();

// ── templates + user sessions (satoru) ───────────────────────────────────────
// instantiate "tmpl@inst" from a registered template unit "tmpl@", substituting
// %i=inst in Exec/Description. returns the new instance index or -1. (satoru)
int InstantiateTemplate(const char* tmpl_at_inst);
// start/stop a user's per-user units from /kurono/user/home/<user>/.config/kinit.
// called on login/logout. returns count started/stopped. (satoru)
int StartUserSession(const char* user);
int StopUserSession(const char* user);

// ── analyze (satoru) ──────────────────────────────────────────────────────────
// render the boot-time analysis (per-service start->running ms, slowest units,
// a text dependency graph) into out. (satoru)
int Analyze(char* out, int mx);
// total boot critical-path time in ms (last service ready - boot start). (satoru)
uint32_t BootElapsedMs();

// ── runtime capability sandboxing (satoru) ───────────────────────────────────
// enforce a service's [Capabilities] at RUNTIME (not only at spawn) via SUPR:
// returns true if the calling service is permitted the requested capability now.
// an in-kernel service calls this before a privileged operation; a denied call is
// audited. CAP_* select the capability. (satoru)
enum KCapBit : uint32_t { KCAP_NETWORK = 1u<<0, KCAP_FILESYSTEM = 1u<<1, KCAP_GUI = 1u<<2 };
bool CheckCapability(const char* service, uint32_t cap);

// ── headless test harness (satoru) ───────────────────────────────────────────
// enable the bounded self-test services (gated by the kurono.kinit.test boot
// token). called from kernel_main BEFORE Init() so register_test_services adds
// them. bits select which scenarios to register. (satoru)
enum KTestBits : uint32_t {
    KTEST_NONE      = 0,
    KTEST_MEMLIMIT  = 1u << 0,   // a MemoryMax=64M hog that gets oom-killed (satoru)
    KTEST_WATCHDOG  = 1u << 1,   // a watchdog unit that stops pinging -> restart (satoru)
    KTEST_SOCKET    = 1u << 2,   // a socket-activated unit (satoru)
    KTEST_PARALLEL  = 1u << 3,   // slow units used to measure parallel vs sequential (satoru)
    KTEST_CYCLE     = 1u << 4,   // a circular After= pair (satoru)
    KTEST_SANDBOX   = 1u << 6,   // a runtime-capability sandbox test service (satoru)
    KTEST_ALL       = 0xFFFFFFFFu
};
void EnableTestServices(uint32_t bits);
// run the bounded self-test scenarios once the desktop is up and log PASS/FAIL +
// measured numbers to serial, then (optionally) power off. driven by the boot
// token handler. (satoru)
void RunSelfTests();

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

// ── kdf (kernel driver framework) integration (satoru) ────────────────────────
// register a kdf-sandboxed kernel driver as a supervised in-kernel kinit unit.
// kinit owns the (re-)init policy: it brings the driver up via KDF::Start at the
// given boot target, and a kdf guard-page crash (reported through
// NotifyDriverCrash) is run through the same backoff + 5-in-60s machinery as any
// other in-kernel unit, re-running KDF::Start to re-init the driver. `init` is
// the driver's kdf init entry (the same one passed to KDF::RegisterDriver, which
// this calls for you). returns the kinit service index, or -1. (satoru)
typedef bool (*KdfDriverInit)();
int RegisterKdfDriver(const char* name, KdfDriverInit init, KTarget target,
                      bool critical);

// the bridge KDF calls (via KDF::SetCrashNotifier) when a driver's guard page
// faults: looks up the matching kdf-driver unit and schedules a backoff restart
// (honouring the burst limit). safe to call from the fault path. (satoru)
void NotifyDriverCrash(const char* driver, const char* reason);

}  // namespace KInit

// end (satoru)
