#ifndef KURONO_SYSTEM_RUNTIME_LAYOUT_H
#define KURONO_SYSTEM_RUNTIME_LAYOUT_H

#include "../kernel/types.h"

// Seeds the Kurono filesystem layout that user-space binaries (Firefox,
// GTK apps, glibc-linked programs in general) expect at runtime.
//
// Creates the canonical kurono directory tree:
//   /system/{bin,lib,libexec,etc,share,fonts,ssl/certs,run,log,tmp,
//            var/{cache,lib,spool},dev,proc,sys}
//   /system/run/user/1000/{pulse,wayland,bus.dir}
//   /apps/{lib,bin}
//   /home/user/{.config,.local/share,.cache,Desktop,Documents,Downloads}
//
// Then populates the small files Firefox & friends consult at startup:
//   /system/etc/{hostname,resolv.conf,nsswitch.conf,hosts,localtime,
//                machine-id,os-release,passwd,group,shadow}
//   /system/fonts/fonts.conf
//   /system/ssl/certs/ca-certificates.crt   (placeholder bundle)
//   /system/proc/sys/kernel/{overcommit_memory, ostype, osrelease, hostname}
//   /system/proc/sys/vm/{max_map_count, overcommit_memory, swappiness}
//   /system/proc/{cpuinfo, meminfo, version, uptime, loadavg, mounts,
//                 stat, partitions, filesystems, modules, devices,
//                 net/dev, net/tcp, net/udp}
//   /system/proc/self/{exe, cwd, status, cmdline, comm, maps, stat}
//   /system/lib/firefox-deps.manifest       (machine-readable list of
//                                            the .so files Firefox needs)
//
// Also installs the dynamic-loader search path into KVFS so anything
// reading /system/etc/ld.so.conf.d/kurono.conf gets the right answer.

namespace RuntimeLayout {

    void Init();

    // Ensure the per-user runtime directory exists with the right perms.
    // Called from the lockscreen on successful login.
    void EnsureUserRuntime(const char* user, uint32_t uid);

    // Re-seed dynamic /proc files at the given moment.  Called by the
    // scheduler tick at low frequency so /proc/uptime stays current.
    void RefreshProc();

    // List of canonical search paths for ld.so / dlopen().
    // NULL-terminated.
    const char* const* LibrarySearchPath();
}

#endif
