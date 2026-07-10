//  kurono linux subsystem - user bridge
//  bidirectional user synchronization between supr and linux
#pragma once
#include "../kernel/types.h"

#define UB_MAX_MAPPED_USERS  32
#define UB_SYNC_INTERVAL     30000   // ms between auto-syncs

enum UBSyncDirection {
    UB_SUPR_TO_LINUX = 0,
    UB_LINUX_TO_SUPR = 1,
    UB_BIDIRECTIONAL = 2
};

enum UBConflictPolicy {
    UB_SUPR_WINS = 0,      // supr data takes precedence
    UB_LINUX_WINS = 1,     // linux /etc/passwd wins
    UB_NEWEST_WINS = 2,    // most recently modified wins
    UB_MANUAL = 3          // flag conflict for manual resolution
};

struct UBMappedUser {
    char     username[32];
    int      supr_uid;      // uid in supr
    int      linux_uid;     // uid in linux /etc/passwd
    int      supr_gid;
    int      linux_gid;
    char     home[64];      // shared home directory
    char     shell[32];     // preferred shell
    bool     synced;        // has been synced at least once
    uint32_t last_sync;     // timestamp of last sync
    bool     conflict;      // unresolved conflict
};

struct UBSyncResult {
    int users_added;
    int users_updated;
    int users_removed;
    int conflicts;
    bool success;
};

class UserBridge {
public:
    static void Init();
    static void SetDirection(UBSyncDirection dir);
    static void SetConflictPolicy(UBConflictPolicy pol);
    static UBSyncDirection GetDirection();
    static UBConflictPolicy GetConflictPolicy();

    // full sync
    static UBSyncResult Sync();
    static UBSyncResult SyncToLinux();
    static UBSyncResult SyncFromLinux();

    // single user operations
    static bool SyncUser(const char* username);
    static bool MapUser(const char* username, int linux_uid, int linux_gid);
    static bool UnmapUser(const char* username);

    // query
    static UBMappedUser* FindMapped(const char* username);
    static int GetLinuxUID(const char* username);
    static int GetSUPRUID(int linux_uid);
    static UBMappedUser* GetMappedUsers();
    static int GetMappedCount();

    // status
    static bool HasConflicts();
    static int  ResolveConflict(const char* username, UBSyncDirection winner);
    static void DumpStatus(char* out, int max_out);

    // auto-sync
    static void Tick(uint32_t now_ms);
    static void SetAutoSync(bool enabled);

private:
    static UBMappedUser    mapped[UB_MAX_MAPPED_USERS];
    static int             mapped_count;
    static UBSyncDirection direction;
    static UBConflictPolicy conflict_policy;
    static bool            auto_sync;
    static uint32_t        last_auto_sync;

    static int  ParsePasswdUID(const char* line);
    static void ParsePasswdLine(const char* line, char* user, int* uid, int* gid,
                                char* home, char* shell, int maxstr);
};
