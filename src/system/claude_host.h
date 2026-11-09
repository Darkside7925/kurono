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
    // load CLAUDE from the KURONO partition if /system/bin/claude is not present
    // yet. returns true when /system/bin/claude is ready. msg gets a one line
    // reason on failure (never a crash on a missing or damaged file). (satoru)
    bool EnsureLoaded(char* msg, int msgmax);

    // `kpkg install claude` hands over the verified image it just registered at
    // /system/bin/claude so EnsureLoaded treats it as loaded. buf is a pmm
    // block that stays allocated for the boot; nullptr forgets it. (satoru)
    void AdoptImage(uint8_t* buf, uint64_t len);

    // run claude to completion with the given argument line (argv[0] is added).
    // env_extra is an optional space separated list of NAME=value pairs that
    // is appended to the default environment (used by tests and the shell).
    // console output is echoed to the serial log and also captured; returns
    // the process exit code, or a negative value if it could not start. (satoru)
    int Run(const char* args_line, const char* env_extra, char* msg, int msgmax);

    // remember the kurono.claude.args / kurono.claude.env boot values for the
    // headless gate. (satoru)
    void SetBootArgs(const char* args, const char* env);

    // kurono.claude=1 headless gate entry (spawned as a kernel process). (satoru)
    [[noreturn]] void BootRunnerEntry();
}

// end (satoru)
