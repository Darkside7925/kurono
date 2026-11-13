#pragma once
#include "../kernel/types.h"

// opencode host support (satoru)
//
// opencode (sst/opencode, the linux-x64-musl build) is a ~210 mb bun binary
// laid out like claude code: an ET_EXEC at 0x200000 that needs musl's
// libstdc++.so.6 + libgcc_s.so.1 (the LIBSTDC.SO / LIBGCCS.SO root files,
// loaded into /system/lib at boot). it lives as the root file OPENCODE on the
// KURONO fat32 partition, is read once into a pmm block at first use and
// exposed as /system/bin/opencode (borrowed, no heap copy). provider keys come
// from /etc/opencode.env and the OPENCODE.ENV root file (NAME=value lines,
// e.g. ANTHROPIC_API_KEY=...), never logged. nothing here writes to the disk
// except the persistent store save after a run. (satoru)
namespace OpenCodeHost {
    // OPENCODE root file present (cheap stat; never reads the payload). (satoru)
    bool Installed();
    // read-only probe for the boot image-window policy (no disk log init). (satoru)
    bool ProbeInstalled();
    // load /system/bin/opencode if needed. msg gets a one line reason on failure. (satoru)
    bool EnsureLoaded(char* msg, int msgmax);
    // drop the loaded image once no opencode process is left. (satoru)
    void ReleaseImage();
    // run opencode to completion. args_line: space separated, 'single quotes'
    // group words. env_extra: optional NAME=value pairs. returns the exit code,
    // or a negative value if it could not start. (satoru)
    int Run(const char* args_line, const char* env_extra, char* msg, int msgmax);

    // the same launch environment for an opencode exec'd by any launcher
    // (bash, sh): defaults + the cached env file lines, for names the caller's
    // env does not already have. CacheEnvFile runs once at boot. (satoru)
    void CacheEnvFile();
    bool IsOpenCodeBinary(const char* vfs_path);
    int  AugmentExecEnv(const char** ev, int count, int max);
}

// end (satoru)
