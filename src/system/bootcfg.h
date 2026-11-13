// bootcfg: the boot decisions stored in KBOOT.CFG on the KURONO partition
// (recovery_PLAN.md section 3; created by R2's kcrash, kurono.kcrash=1, or by
// the installer). the EFI loader reads and updates the same 512 bytes before
// it picks what to boot. all writes overwrite the existing file in place
// (no allocation, no fat change). when the file does not exist every call
// here is a no-op that returns false. (satoru)
//
// layout (little endian; R2 defined bytes 0..47 and 508..511):
//   0  u32 magic 'KBOT'   4 u16 version   6 u16 pad
//   8  u32 boot_count (bumped by kcrash at boot)
//   12 u32 flags: bit0 recovery once, bit1 previous kernel once,
//                 bit2 start with drivers disabled once (safe start),
//                 bit3 the last trial kernel failed (set by the loader)
//   16 char last_good[32]   the kernel slot that last reached the desktop
//   48 u32 trial_tries      boots left for the trial slot (loader decrements)
//   52 char trial[32]       8.3 root name of the kernel being tried, "" none
//   84 char current[32]     8.3 root name of the default kernel, "" = the
//                           kernel embedded in the loader itself
//   116 u32 confirmed_boot  boot_count of the last boot that reached the desktop
//   120 u32 fail_streak     boots in a row that never reached the desktop
//                           (loader increments, the desktop clears)
//   124 u32 recovery_reason 0 none, 1 asked, 2 two failed boots, 3 key held
//   128 char previous[32]   the slot `current` replaced, for "previous version"
//   508 u32 fnv-1a over bytes 0..507 (satoru)
#pragma once
#include "../kernel/types.h"

namespace BootCfg {

enum Flags : uint32_t {
    F_RECOVERY_ONCE = 1u << 0,
    F_PREVIOUS_ONCE = 1u << 1,
    F_SAFE_ONCE     = 1u << 2,
    F_TRIAL_FAILED  = 1u << 3,
};

struct State {
    uint32_t boot_count, flags, trial_tries, confirmed_boot, fail_streak, recovery_reason;
    char last_good[32], trial[32], current[32], previous[32];
};

bool Read(State* out);
// read, let `edit` change the state, reseal and write back. (satoru)
bool Update(void (*edit)(State& s, void* ctx), void* ctx);

bool RequestRecoveryOnce();
bool RequestSafeStartOnce();
bool RequestPreviousOnce();
// the desktop has run long enough: clear fail_streak, remember the slot this
// boot came from as last_good and, for a trial boot, promote it to current. (satoru)
void ConfirmDesktop(const char* slot);
// stage a kernel slot for one trial boot. (satoru)
bool SetTrial(const char* name83, uint32_t tries);

// the boot watch (3a): started once at boot with kurono.boot.slot (null when
// the loader did not say). 60 s after the desktop came up it confirms the
// boot; a trial slot that has no desktop after kTrialDeadlineS restarts the
// machine so the loader falls back to the previous kernel. (satoru)
static const uint32_t kConfirmAfterS = 60;
static const uint32_t kTrialDeadlineS = 180;
void StartWatch(const char* slot);
void NoteDesktop();
// kurono.kcrash=1: the watch process opens the crash store on disk (KBOOT.CFG,
// KDRIVERS.CFG, KCRASH.LOG, created when missing) before it reads KBOOT.CFG. (satoru)
void SetCrashStoreDisk(bool on);

}  // namespace BootCfg

// end (satoru)
