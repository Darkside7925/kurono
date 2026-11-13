#pragma once
#include "../kernel/types.h"

// the kernel dev loop in one command: `vm kloop [clean] [boot secs] [guest MB] [guest cmdline]`
// (satoru)
//   0. the agent drops a source tarball at /home/user/vmshare/src.tar
//   1. the Alpine vm starts (if it is not up)
//   2. kxfer (tools/selfhost/kxfer.c, embedded) is pasted in, toolchain installed
//   3. the guest pulls src.tar over the v9fs share, builds kurono_base.elf
//      in its persistent tree (/root/kb, incremental unless a header changed
//      or `clean`; tools/selfhost/kloop_build.sh), pushes back kguest.elf
//      (debug stripped) and kmake.log
//   4. the build vm stops, the new kernel boots as a Kurono guest for N s,
//      its com1 streams into vmshare/kguest.log, the screen into kguest.png
//   5. the guest is killed; a summary returns (also in vmshare/kloop.log)
// runs in the caller's (shell) context and blocks it for the whole loop. (satoru)
namespace KLoop {
    int Run(uint32_t boot_secs, uint32_t ram_mb, bool clean, const char* guest_cmdline, char* out, int mx);
}

// end (satoru)
