// installer_service.h: the privileged installer service a ring-3 installer app
// talks to. it owns the disk scan, the admin gate and the job dispatch. the
// in-kernel fallback of the same app calls these functions directly. (satoru)
#pragma once
#include "../kernel/types.h"
#include "installer_proto.h"

namespace InstallerService {

// lazy init: create the unix socket. safe to call often. (satoru)
void Init();

// once per gui loop pass. cheap when no install is running. (satoru)
void Pump();

// kurono.installer.auto=1: the app drives itself through the screens so a
// headless qemu run can test the whole path. test only. (satoru)
void SetAuto(bool on);

// false when this kernel image has no embedded EFI loader (the normal desktop
// boot uses the base kernel; the setup entry boots the full one). (satoru)
bool PayloadAvailable();

int List(KinList* out);
int Plan(int disk, uint32_t kfs_mb, KinPlan* out);
int Start(const KinReq& req, uint32_t peer_pid, uint32_t peer_uid, KinReply* rep);
int Status(KinStatus* out);

// the shell command that runs a queued install job on a launcher kernel proc.
// called by the kernel shell, not by the app. (satoru)
void RegisterShellCommands(void* shell);
int  RunJob(char* out, int max_out);

// end (satoru)
}  // namespace InstallerService

// end (satoru)
