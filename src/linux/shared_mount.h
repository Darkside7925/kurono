//  kurono linux subsystem - shared mount layer
//  unified namespace: mounts kvfs paths into linux and ext4 paths into kurono
#pragma once
#include "../kernel/types.h"

#define SM_MAX_MOUNTS    32

enum SharedMountType {
    SM_BIND = 0,       // bind mount (same source, visible in both)
    SM_OVERLAY = 1,    // overlay - kvfs is lower, ext4 is upper
    SM_SYMLINK = 2,    // symbolic link redirect
    SM_PASSTHROUGH = 3 // direct passthrough (no translation)
};

enum SharedMountFS {
    SM_FS_KVFS = 0,
    SM_FS_EXT4 = 1,
    SM_FS_PROC = 2,
    SM_FS_DEV  = 3,
    SM_FS_TMP  = 4,
    SM_FS_SYS  = 5
};

struct SharedMount {
    char        linux_path[64];   // path as seen from linux (e.g., /home)
    char        kurono_path[64];  // path as seen from kurono (e.g., /home)
    SharedMountType type;
    SharedMountFS   fs;
    bool        active;
    bool        read_only;
    uint32_t    mount_time;
};

struct MountStats {
    int total_mounts;
    int active_mounts;
    int bind_mounts;
    int overlay_mounts;
};

class SharedMountMgr {
public:
    static void Init();

    // mount operations
    static int  Mount(const char* linux_path, const char* kurono_path,
                      SharedMountType type, SharedMountFS fs, bool read_only);
    static int  Unmount(const char* linux_path);
    static int  UnmountAll();
    static int  Remount(const char* linux_path, bool read_only);

    // setup defaults (called by kls::start)
    static void MountDefaults();

    // path translation
    static const char* LinuxToKurono(const char* linux_path);
    static const char* KuronoToLinux(const char* kurono_path);
    static bool IsSharedPath(const char* path);

    // query
    static SharedMount* FindMount(const char* linux_path);
    static SharedMount* GetMounts();
    static int  GetMountCount();
    static MountStats GetStats();

    // status
    static void DumpMounts(char* out, int max_out);

private:
    static SharedMount mounts[SM_MAX_MOUNTS];
    static int         mount_count;

    // path matching
    static bool PathStartsWith(const char* path, const char* prefix);
    static void TranslatePath(const char* src_path, const char* src_prefix,
                              const char* dst_prefix, char* out, int max_out);
};
