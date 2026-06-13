#pragma once

//  kurono canonical on-disk path layout  -  the single source of truth so the whole
//  tree agrees on where things live. before this, logs were triple-homed across
//  /system/logs, /kurono/logs and /var/log, and "system" files were split between
//  /system and /kurono. everything kurono-native now lives under /kurono; the
//  bare linux-compat names (/home /etc /usr /proc /dev /sys /tmp /boot /bin /lib
//  /var /apps /system) stay at the root as SYMLINKS into the /kurono tree so the
//  ~800 hardcoded references (and the kurono linux subsystem, KLS) keep working.
//  change a root here and the rest of the kernel follows. (satoru)
//
//  the canonical tree (real dirs live under /kurono; the rest are compat symlinks):
//
//    /kurono/system/   core os (bin, lib, drivers, config, boot, security)
//    /kurono/linux/    linux compat (compat libs, linker, bridge, drivers)
//    /kurono/windows/  windows compat environment
//    /kurono/apps/     installed native kurono apps
//    /kurono/user/     all user data (home, shared)
//    /kurono/packages/ kpkg state
//    /kurono/runtime/  live state (proc, dev, tmp, sockets), cleared on boot
//    /kurono/var/{log,updates,state,lib}
//
//    /system -> /kurono/system          /home   -> /kurono/user/home
//    /etc    -> /kurono/system/config   /bin    -> /kurono/system/bin
//    /lib    -> /kurono/linux/compat    /usr/bin-> /kurono/linux/compat/bin
//    /usr/lib-> /kurono/linux/compat/lib   /tmp -> /kurono/runtime/tmp
//    /proc   -> /kurono/runtime/proc    /dev    -> /kurono/runtime/dev
//    /var    -> /kurono/var
//  (satoru)

//  kurono-native namespace root (satoru)
#define KP_ROOT          "/kurono"

//  ── canonical top-level subsystem roots (real directories under /kurono) ────
#define KP_SYSTEM_ROOT   "/kurono/system"   // core os (satoru)
#define KP_LINUX_ROOT    "/kurono/linux"    // linux compat (satoru)
#define KP_WINDOWS_ROOT  "/kurono/windows"  // windows compat (satoru)
#define KP_APPS          "/kurono/apps"     // native kurono apps (satoru)
#define KP_USER          "/kurono/user"     // all user data (satoru)
#define KP_USER_HOME     "/kurono/user/home"
#define KP_PACKAGES      "/kurono/packages" // kpkg state (satoru)
#define KP_RUNTIME       "/kurono/runtime"  // live state, cleared on boot (satoru)
#define KP_VAR           "/kurono/var"

//  ── /kurono/system subtree (the old /system tree) ───────────────────────────
#define KP_SYS_BIN       "/kurono/system/bin"
#define KP_SYS_LIB       "/kurono/system/lib"
#define KP_SYS_DRIVERS   "/kurono/system/drivers"
#define KP_SYS_CONFIG    "/kurono/system/config"   // the old "etc" under system (satoru)
#define KP_SYS_BOOT      "/kurono/system/boot"
#define KP_SYS_SECURITY  "/kurono/system/security"

//  ── /kurono/linux compat libs (where the dynamic-linker libs live) ──────────
#define KP_LINUX_COMPAT      "/kurono/linux/compat"
#define KP_LINUX_COMPAT_BIN  "/kurono/linux/compat/bin"
#define KP_LINUX_COMPAT_LIB  "/kurono/linux/compat/lib"
#define KP_LINUX_LINKER      "/kurono/linux/linker"
#define KP_LINUX_BRIDGE      "/kurono/linux/bridge"
#define KP_LINUX_DRIVERS     "/kurono/linux/drivers"

//  ── /kurono/runtime live state ──────────────────────────────────────────────
#define KP_RUN_PROC      "/kurono/runtime/proc"
#define KP_RUN_DEV       "/kurono/runtime/dev"
#define KP_RUN_TMP       "/kurono/runtime/tmp"
#define KP_RUN_SOCKETS   "/kurono/runtime/sockets"

//  ── logs ────────────────────────────────────────────────────────────────────
//  one canonical home; retires the old /system/logs, /kurono/logs and /var/log.
//  keep it minimal + structured: one file per category, a dir for crash dumps,
//  and per-app / per-process subdirs. (satoru)
#define KP_LOG_DIR       "/kurono/var/log"
#define KP_LOG_BOOT      "/kurono/var/log/boot.log"        // boot milestones (satoru)
#define KP_LOG_SYSTEM    "/kurono/var/log/system.log"      // general system events (satoru)
#define KP_LOG_SERIAL    "/kurono/var/log/serial.log"      // mirror of the serial console (satoru)
#define KP_LOG_NETWORK   "/kurono/var/log/network.log"     // connect/disconnect/errors (satoru)
#define KP_LOG_SECURITY  "/kurono/var/log/security.log"    // supr escalations, ksa prompts (satoru)
#define KP_LOG_CRASH_DIR "/kurono/var/log/crash"           // kernel panics + minidumps (satoru)
#define KP_LOG_APPS_DIR  "/kurono/var/log/apps"            // per-app logs (satoru)
#define KP_LOG_PROC_DIR  "/kurono/var/log/processes"       // per-kernel-process logs (satoru)

//  ── kurono persistent state ────────────────────────────────────────────────
//  the on-disk persistence volume (KFS / the raw store image), kpkg db, etc. (satoru)
#define KP_VAR_LIB       "/kurono/var/lib"
#define KP_VAR_LIB_KPKG  "/kurono/var/lib/kpkg"

//  ── kurono config + system files ───────────────────────────────────────────
#define KP_ETC           "/kurono/etc"
#define KP_SYSTEM        "/kurono/system"

// end (satoru)
