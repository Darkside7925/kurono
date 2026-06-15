#pragma once
#include "../kernel/types.h"

//  kurono os: systemd compatibility layer.
//
//  linux apps run through the in-kernel ld-kurono dynamic linker frequently
//  probe for systemd: they stat /run/systemd/system, shell out to systemctl /
//  journalctl / loginctl, or talk to org.freedesktop.systemd1 over the session
//  bus. kurono does not run systemd, it runs kinit (see kinit.h). this layer is
//  a thin shim that makes those probes succeed by translating them onto kinit:
//
//    1. /run/systemd/ runtime tree     so sd_booted() / stat probes pass.
//    2. systemctl shim                 start|stop|restart|status|enable|disable|
//                                      is-active|list-units mapped onto kinit.
//    3. .service unit parser           parses a standard systemd [Unit]/[Service]/
//                                      [Install] file, converts it to a kinit
//                                      .kservice, drops it in the services dir and
//                                      triggers kinit reload.
//    4. journalctl shim                journalctl -u <svc> [-n N] [-f] reads the
//                                      kinit audit log filtered by service.
//    5. loginctl stub                  returns one active session on seat0 for the
//                                      current user so login-session probes work.
//    6. org.freedesktop.systemd1       ListUnits/GetUnit/StartUnit/StopUnit/
//                                      RestartUnit + Unit ActiveState/SubState/
//                                      LoadState, backed by kinit state.
//
//  the shim never modifies kinit's core: it drives kinit only through kinit's
//  existing public control api (StartService/StopService/RestartService/Reload)
//  and reads kinit's state read-only (GetServices/FindService/StateName). (satoru)

namespace SystemdCompat {

// create /run/systemd/{,system,private,units,notify} (+ compat symlinks) so apps
// that probe for systemd presence do not fail. idempotent. wired into the linux
// runtime init. (satoru)
void InitRuntime();

// register the systemctl / journalctl / loginctl shell commands. additive: it
// registers them in ENV_KURONO so they take precedence over the legacy linux_init
// shims (which stay as ENV_AUTO fallbacks) without touching that file. (satoru)
void RegisterShellCommands(void* shell);

// shell command handlers (signature matches the shell's void* form). (satoru)
int CmdSystemctl(void* sh, int argc, const char** argv, char* out, int mx);
int CmdJournalctl(void* sh, int argc, const char** argv, char* out, int mx);
int CmdLoginctl(void* sh, int argc, const char** argv, char* out, int mx);

// parse a standard systemd .service file (text/len) and install it as a kinit
// .kservice under /kurono/system/services, then trigger kinit reload. unit_name
// is the unit's base name without the ".service" suffix (e.g. "firefox"); if a
// [Unit] has no usable name we fall back to unit_name. returns true on a
// successful install. (satoru)
bool InstallServiceUnit(const char* unit_name, const char* text, int len);

// the systemd1 / login1 d-bus bridge. dbus_server.cpp calls this for any method
// whose interface starts with org.freedesktop.systemd1 or .login1. it builds the
// reply body into out_body (cap bytes), sets *out_sig to the reply signature, and
// returns the body length, or -1 if the method is not handled here (the caller
// then falls back to its generic empty reply). msg/len/body_off describe the raw
// incoming d-bus message so we can pluck call arguments. (satoru)
int DBusDispatch(const char* iface, const char* member, const char* path,
                 const uint8_t* msg, int len, int body_off,
                 uint8_t* out_body, int cap, const char** out_sig);

}  // namespace SystemdCompat

// end (satoru)
