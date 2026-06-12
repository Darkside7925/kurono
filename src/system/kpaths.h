#pragma once

//  kurono canonical on-disk path layout  -  the single source of truth so the whole
//  tree agrees on where things live. before this, logs were triple-homed across
//  /system/logs, /kurono/logs and /var/log, and "system" files were split between
//  /system and /kurono. everything kurono-native now lives under /kurono; the
//  bare linux-compat dirs (/home /etc /usr /proc /dev /sys /tmp /boot) stay at the
//  root so the kurono linux subsystem (KLS) keeps working. change a root here and
//  the rest of the kernel follows. (satoru)

//  kurono-native namespace root (satoru)
#define KP_ROOT          "/kurono"

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
