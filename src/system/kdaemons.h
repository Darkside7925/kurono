#pragma once
#include "../kernel/types.h"

//  kurono os: kupdate + ksecurity, two small in-kernel service daemons managed
//  by kinit.
//
//  like kpkg-daemon's in-kernel worker, these run as dedicated kernel-processes
//  (so they never block the gui) and wrap functionality that already exists in
//  the kernel rather than pretending to be separate linux binaries:
//
//    kupdate   = periodically asks the package manager how many updates are
//                pending (a real PackageManager::GetPendingUpdateCount query)
//                and publishes the count; raises a toast when updates appear.
//
//    ksecurity = periodically re-runs the supr + ksa policy self-tests
//                (SUPR::PolicySelfTest / KSA::SelfTest) so a corrupted security
//                policy is caught early, and exposes the last result as the
//                unit's health. (satoru)

namespace KUpdate {
void Init();                 // spawn the update-checker kernel-process (satoru)
bool IsHealthy();            // for kinit's health probe (satoru)
int  PendingCount();         // last observed pending-update count (satoru)
int  Cmd(void* sh, int argc, const char** argv, char* out, int mx);
void RegisterShellCommands(void* shell);
}

namespace KSecurity {
void Init();                 // spawn the policy-watch kernel-process (satoru)
bool IsHealthy();            // false if the last policy self-test failed (satoru)
int  Cmd(void* sh, int argc, const char** argv, char* out, int mx);
void RegisterShellCommands(void* shell);
}

// end (satoru)
