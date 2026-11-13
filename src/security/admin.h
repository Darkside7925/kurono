#pragma once
//  kurono os - admin gate
//  the enforcement layer between an unprivileged ring-3 process and a
//  privileged operation. it decides whether the calling linux process may
//  perform an action and, if not, shows the ksa admin prompt (the
//  hypervisor-isolated one when available, otherwise the secure desktop
//  fallback in ksa.cpp). one approval grants the calling process a short
//  window so a multi-step operation does not prompt for every write.
//
//  kernel processes and uid 0 processes are always allowed; the gate is a
//  no-op when kurono.ken.enforce=0. (satoru)
#include "../kernel/types.h"

namespace AdminGate {

// boot flag: kurono.ken.enforce=0 disables enforcement entirely. default on. (satoru)
void SetEnforce(bool on);
bool EnforceEnabled();

// true if the current task is privileged (kernel process, uid 0, active
// elevation grant, or a ken/sudo-elevated session). (satoru)
bool IsPrivileged();

// prompt for and (on approval) grant the current process a short window.
// returns true when the operation may proceed. (satoru)
bool Authorize(const char* action, const char* detail);

// the same gate for a ring-3 client of a kernel service. pid and uid are the
// peer credentials the unix socket stamped at connect time; the grant follows
// that client's linux process slot, not the kernel process dispatching it.
// (satoru)
bool AuthorizeForPid(uint32_t pid, uint32_t uid, const char* action, const char* detail);

// path policy: writes to system paths and raw devices need elevation. (satoru)
bool PathNeedsElevation(const char* resolved_path);
bool AuthorizePathWrite(const char* resolved_path, const char* how);

// kernel-shell command classification: a ring-3 shell can drive kernel
// builtins through /dev/kexec. true means the command needs approval first. (satoru)
bool ShellCommandNeedsElevation(const char* cmdline);

// explicit grants (sudo-like). the shell's ken/sudo path uses these. (satoru)
void GrantCurrent(uint32_t ms);
void RevokeCurrent();

// clear a slot's grant when its linux process record is reused. (satoru)
void ForgetProcess(int pid_index);

}  // namespace AdminGate (satoru)

// end (satoru)
