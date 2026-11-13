#pragma once
//  agent tools: the linux-visible tool layer for the machine's own agent. (satoru)
//
//  claude code's bash tool runs as an unprivileged linux process. it expects a
//  normal unix box: every busybox applet on PATH, a real bash, and a way to run
//  the kurono shell's own commands (ksys, kdrv, kinit, audio, gpu...). this
//  seeds those names into /system/bin, which the linux path translation serves
//  as /bin and /usr/bin. called once at boot after the recovery toolbox unpack.

namespace AgentTools {

// lay the applet symlinks, the ksh-run bridge and the real bash (when the
// blobs are present in this build). idempotent; never overwrites a real file.
void Install();

}  // namespace AgentTools

// end (satoru)
