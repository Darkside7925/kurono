#pragma once
#include "../kernel/types.h"

// claude code host support (satoru)
//
// the real, unmodified claude code cli is a ~300 mb linux-x64-musl binary. it is
// NOT embedded in the kernel image (grub load time scales with image size).
// instead it lives as a root file named CLAUDE on the fat32 partition labelled
// KURONO. EnsureLoaded() streams it read-only into physical memory at first use
// and exposes it in the vfs as /system/bin/claude (guest /usr/bin/claude), borrowed (no heap copy).
// nothing here ever writes to the disk. (satoru)
namespace ClaudeHost {
    // any launcher (the ring-3 terminal's sh, bash, an agent) can exec
    // /system/bin/claude, but only the kernel `claude` command used to add the
    // token from CLAUDE.ENV, so a claude started from another terminal said
    // "not logged in". CacheEnvFile (boot, disk ready) reads /etc/claude.env and
    // the CLAUDE.ENV root file ONCE into kernel memory; AugmentExecEnv (execve
    // of the claude binary) appends those NAME=value lines plus the same
    // defaults Run() sets, for names the caller's env does not already have.
    // values are never logged. no disk i/o in execve. (satoru)
    void CacheEnvFile();
    bool IsClaudeBinary(const char* vfs_path);
    int  AugmentExecEnv(const char** ev, int count, int max);
    // load CLAUDE from the KURONO partition if /system/bin/claude is not present
    // yet. returns true when /system/bin/claude is ready. msg gets a one line
    // reason on failure (never a crash on a missing or damaged file). (satoru)
    bool EnsureLoaded(char* msg, int msgmax);

    // true if a claude image exists this boot (already loaded) or is installed
    // on disk (the KURONO root file or the persistent /opt copy). the boot path
    // uses this to decide whether the fixed-image ram window below the kernel
    // must stay reserved. does not read the image. (satoru)
    bool Installed();
    // read-only disk probe for the boot image-window policy: looks for the
    // persistent copy or the CLAUDE root file without opening the disk log. (satoru)
    bool ProbeInstalled();

    // `kpkg install claude` hands over the verified image it just registered at
    // /system/bin/claude so EnsureLoaded treats it as loaded. buf is a pmm
    // block that stays allocated for the boot; nullptr forgets it. (satoru)
    void AdoptImage(uint8_t* buf, uint64_t len);

    // drop the loaded image and give its ~300 mb pmm block back. the vfs nodes
    // that borrow it are unlinked first, so the next launch reloads from the
    // volume (or the KURONO root file). call only after no claude process is
    // left (ReleaseImage kills any lingering members of the last group). (satoru)
    void ReleaseImage();

    // run claude to completion with the given argument line (argv[0] is added).
    // env_extra is an optional space separated list of NAME=value pairs that
    // is appended to the default environment (used by tests and the shell).
    // console output is echoed to the serial log and also captured; returns
    // the process exit code, or a negative value if it could not start. (satoru)
    int Run(const char* args_line, const char* env_extra, char* msg, int msgmax);

    // remember the kurono.claude.args / kurono.claude.env boot values for the
    // headless gate. (satoru)
    void SetBootArgs(const char* args, const char* env);
    // kurono.window.test=1: exercise the demand-backed low window after the
    // desktop/firefox is up (owned-frame refusal, repeated claude/static runs, a
    // heap checksum). test only. (satoru)
    void SetWindowTest(bool on);

    // lay the mozilla root store at /system/etc/ssl/certs/ca-certificates.crt
    // (guest /etc/ssl/certs/...) if it is not there yet; shared with opencode. (satoru)
    void EnsureCaBundle();

    // kurono.claude=1 headless gate entry (spawned as a kernel process). (satoru)
    [[noreturn]] void BootRunnerEntry();
}

// end (satoru)
