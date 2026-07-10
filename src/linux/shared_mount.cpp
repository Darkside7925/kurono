//  kurono linux subsystem - shared mount layer implementation
//  unified namespace between kvfs and linux ext4

#include "shared_mount.h"
#include "ext4.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../kernel/time.h"

SharedMount SharedMountMgr::mounts[SM_MAX_MOUNTS];
int         SharedMountMgr::mount_count = 0;

static int sm_slen(const char* s) { int n=0; while(s&&s[n])n++; return n; }
static void sm_scpy(char* d, const char* s, int mx) {
    int i=0; while(s&&s[i]&&i<mx-1){d[i]=s[i];i++;} d[i]=0;
}
static bool sm_seq(const char* a, const char* b) {
    while(*a&&*b){if(*a!=*b)return false;a++;b++;} return *a==0&&*b==0;
}
static int sm_pa(char* o, int p, int mx, const char* s) {
    while(*s&&p<mx-1) o[p++]=*s++; o[p]=0; return p;
}
static int sm_pad(char* o, int p, int mx, int v) {
    char t[12]; int i=0;
    if(v==0){t[i++]='0';}
    else{bool n=v<0;if(n)v=-v;while(v>0){t[i++]='0'+(v%10);v/=10;}if(n)t[i++]='-';}
    for(int j=i-1;j>=0&&p<mx-1;j--)o[p++]=t[j]; o[p]=0; return p;
}

//  lifecycle

void SharedMountMgr::Init() {
    mount_count = 0;
    memset(mounts, 0, sizeof(mounts));
    SerialLogger::Log("[SharedMount] Initialized\r\n");
}

//  mount operations

int SharedMountMgr::Mount(const char* linux_path, const char* kurono_path,
                           SharedMountType type, SharedMountFS fs, bool read_only) {
    // check if already mounted
    SharedMount* existing = FindMount(linux_path);
    if (existing && existing->active) return -1; // already mounted

    // find free slot or reuse inactive
    SharedMount* slot = nullptr;
    if (existing && !existing->active) {
        slot = existing;
    } else {
        if (mount_count >= SM_MAX_MOUNTS) return -2;
        slot = &mounts[mount_count++];
    }

    sm_scpy(slot->linux_path,  linux_path,  sizeof(slot->linux_path));
    sm_scpy(slot->kurono_path, kurono_path, sizeof(slot->kurono_path));
    slot->type = type;
    slot->fs = fs;
    slot->active = true;
    slot->read_only = read_only;
    slot->mount_time = Time::GetTicks();

    // ensure target directories exist in kvfs
    KVFS::Mkdirs(kurono_path);

    // if this is a linux-side path under /linux, create that too
    char linux_full[128];
    sm_scpy(linux_full, "/linux", sizeof(linux_full));
    int lfl = sm_slen(linux_full);
    sm_scpy(linux_full + lfl, linux_path, (int)(sizeof(linux_full) - lfl));
    KVFS::Mkdirs(linux_full);

    SerialLogger::Log("[SharedMount] Mounted ");
    SerialLogger::Log(linux_path);
    SerialLogger::Log(" <-> ");
    SerialLogger::Log(kurono_path);
    SerialLogger::Log("\r\n");

    return 0;
}

int SharedMountMgr::Unmount(const char* linux_path) {
    SharedMount* m = FindMount(linux_path);
    if (!m || !m->active) return -1;
    m->active = false;

    SerialLogger::Log("[SharedMount] Unmounted ");
    SerialLogger::Log(linux_path);
    SerialLogger::Log("\r\n");
    return 0;
}

int SharedMountMgr::UnmountAll() {
    int count = 0;
    for (int i = 0; i < mount_count; i++) {
        if (mounts[i].active) {
            mounts[i].active = false;
            count++;
        }
    }
    SerialLogger::Log("[SharedMount] Unmounted all\r\n");
    return count;
}

int SharedMountMgr::Remount(const char* linux_path, bool read_only) {
    SharedMount* m = FindMount(linux_path);
    if (!m || !m->active) return -1;
    m->read_only = read_only;
    return 0;
}

//  default mounts (called by kls during startup)

void SharedMountMgr::MountDefaults() {
    SerialLogger::Log("[SharedMount] Setting up default mounts...\r\n");

    // shared /home - same users, same files
    Mount("/home",     "/home",     SM_BIND, SM_FS_KVFS, false);

    // shared /tmp
    Mount("/tmp",      "/tmp",      SM_BIND, SM_FS_TMP,  false);

    // /var/log shared for unified logging
    Mount("/var/log",  "/var/log",  SM_BIND, SM_FS_KVFS, false);

    // /proc virtual filesystem (read-only)
    Mount("/proc",     "/proc",     SM_PASSTHROUGH, SM_FS_PROC, true);

    // /dev devices
    Mount("/dev",      "/dev",      SM_PASSTHROUGH, SM_FS_DEV,  false);

    // /sys sysfs (read-only)
    Mount("/sys",      "/sys",      SM_PASSTHROUGH, SM_FS_SYS,  true);

    // /etc shared (overlay - kvfs base, ext4 overlay)
    Mount("/etc",      "/etc",      SM_OVERLAY, SM_FS_KVFS, false);

    // /mnt/kurono - linux can access all of kvfs here
    Mount("/mnt/kurono", "/",       SM_BIND, SM_FS_KVFS, false);

    // /boot shared
    Mount("/boot",     "/boot",     SM_BIND, SM_FS_KVFS, true);

    SerialLogger::Log("[SharedMount] Default mounts ready\r\n");
}

//  path translation

bool SharedMountMgr::PathStartsWith(const char* path, const char* prefix) {
    while (*prefix) {
        if (*path != *prefix) return false;
        path++; prefix++;
    }
    // must match at boundary: end of path or /
    return *path == 0 || *path == '/';
}

void SharedMountMgr::TranslatePath(const char* src_path, const char* src_prefix,
                                    const char* dst_prefix, char* out, int max_out) {
    int dp = 0;
    // copy dst prefix
    const char* d = dst_prefix;
    while (*d && dp < max_out - 1) out[dp++] = *d++;

    // skip src prefix in source path
    const char* s = src_path;
    int sp_len = sm_slen(src_prefix);
    s += sp_len;

    // copy remainder
    while (*s && dp < max_out - 1) out[dp++] = *s++;
    out[dp] = 0;

    // handle root mount case - if result is empty, make it "/"
    if (dp == 0) { out[0] = '/'; out[1] = 0; }
}

const char* SharedMountMgr::LinuxToKurono(const char* linux_path) {
    static char translated[128];

    // find the longest matching mount
    SharedMount* best = nullptr;
    int best_len = 0;

    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].active) continue;
        if (PathStartsWith(linux_path, mounts[i].linux_path)) {
            int len = sm_slen(mounts[i].linux_path);
            if (len > best_len) {
                best_len = len;
                best = &mounts[i];
            }
        }
    }

    if (best) {
        TranslatePath(linux_path, best->linux_path,
                      best->kurono_path, translated, sizeof(translated));
        return translated;
    }

    // no mount found - prefix with /linux
    sm_scpy(translated, "/linux", sizeof(translated));
    int tl = sm_slen(translated);
    sm_scpy(translated + tl, linux_path, (int)(sizeof(translated) - tl));
    return translated;
}

const char* SharedMountMgr::KuronoToLinux(const char* kurono_path) {
    static char translated[128];

    SharedMount* best = nullptr;
    int best_len = 0;

    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].active) continue;
        if (PathStartsWith(kurono_path, mounts[i].kurono_path)) {
            int len = sm_slen(mounts[i].kurono_path);
            if (len > best_len) {
                best_len = len;
                best = &mounts[i];
            }
        }
    }

    if (best) {
        TranslatePath(kurono_path, best->kurono_path,
                      best->linux_path, translated, sizeof(translated));
        return translated;
    }

    // no translation - return as-is
    sm_scpy(translated, kurono_path, sizeof(translated));
    return translated;
}

bool SharedMountMgr::IsSharedPath(const char* path) {
    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].active) continue;
        if (mounts[i].type == SM_BIND || mounts[i].type == SM_OVERLAY) {
            if (PathStartsWith(path, mounts[i].linux_path) ||
                PathStartsWith(path, mounts[i].kurono_path)) {
                return true;
            }
        }
    }
    return false;
}

//  query

SharedMount* SharedMountMgr::FindMount(const char* linux_path) {
    for (int i = 0; i < mount_count; i++) {
        if (sm_seq(mounts[i].linux_path, linux_path)) return &mounts[i];
    }
    return nullptr;
}

SharedMount* SharedMountMgr::GetMounts() { return mounts; }
int SharedMountMgr::GetMountCount() { return mount_count; }

MountStats SharedMountMgr::GetStats() {
    MountStats s = {0, 0, 0, 0};
    s.total_mounts = mount_count;
    for (int i = 0; i < mount_count; i++) {
        if (mounts[i].active) s.active_mounts++;
        if (mounts[i].type == SM_BIND) s.bind_mounts++;
        if (mounts[i].type == SM_OVERLAY) s.overlay_mounts++;
    }
    return s;
}

//  status dump (for shell "mount" command)

void SharedMountMgr::DumpMounts(char* out, int max_out) {
    int p = 0;
    p = sm_pa(out, p, max_out, "Kurono Shared Mount Table\n");
    p = sm_pa(out, p, max_out, "═════════════════════════════════════════════════════\n");
    p = sm_pa(out, p, max_out, "Linux Path          Kurono Path       Type      FS\n");
    p = sm_pa(out, p, max_out, "─────────────────────────────────────────────────────\n");

    for (int i = 0; i < mount_count; i++) {
        SharedMount* m = &mounts[i];
        if (!m->active) continue;

        p = sm_pa(out, p, max_out, m->linux_path);
        int ll = sm_slen(m->linux_path);
        for (int j = ll; j < 20; j++) p = sm_pa(out, p, max_out, " ");

        p = sm_pa(out, p, max_out, m->kurono_path);
        int kl = sm_slen(m->kurono_path);
        for (int j = kl; j < 18; j++) p = sm_pa(out, p, max_out, " ");

        switch (m->type) {
            case SM_BIND:        p = sm_pa(out, p, max_out, "bind      "); break;
            case SM_OVERLAY:     p = sm_pa(out, p, max_out, "overlay   "); break;
            case SM_SYMLINK:     p = sm_pa(out, p, max_out, "symlink   "); break;
            case SM_PASSTHROUGH: p = sm_pa(out, p, max_out, "passthru  "); break;
        }

        switch (m->fs) {
            case SM_FS_KVFS: p = sm_pa(out, p, max_out, "kvfs"); break;
            case SM_FS_EXT4: p = sm_pa(out, p, max_out, "ext4"); break;
            case SM_FS_PROC: p = sm_pa(out, p, max_out, "proc"); break;
            case SM_FS_DEV:  p = sm_pa(out, p, max_out, "dev");  break;
            case SM_FS_TMP:  p = sm_pa(out, p, max_out, "tmp");  break;
            case SM_FS_SYS:  p = sm_pa(out, p, max_out, "sys");  break;
        }

        if (m->read_only) p = sm_pa(out, p, max_out, " (ro)");
        p = sm_pa(out, p, max_out, "\n");
    }

    p = sm_pa(out, p, max_out, "─────────────────────────────────────────────────────\n");
    MountStats st = GetStats();
    p = sm_pad(out, p, max_out, st.active_mounts);
    p = sm_pa(out, p, max_out, " active mounts (");
    p = sm_pad(out, p, max_out, st.bind_mounts);
    p = sm_pa(out, p, max_out, " bind, ");
    p = sm_pad(out, p, max_out, st.overlay_mounts);
    p = sm_pa(out, p, max_out, " overlay)\n");
}
