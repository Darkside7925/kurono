#pragma once
#include "../kernel/types.h"
#include "../kernel/heap.h"

//  kurono virtual file system  -  full hierarchical fs with posix semantics

#define KVFS_MAX_NAME     64
#define KVFS_MAX_PATH     256
#define KVFS_MAX_CHILDREN 512   // entries per directory (TODO: make children dynamic for unbounded dirs) (satoru)
#define KVFS_MAX_CONTENT  (64 * 1024)  // 64kb  -  shell cat/grep display chunk (stack-safe)
#define KVFS_MAX_FILE_SIZE (4 * 1024 * 1024)  // 4mb  -  real per-file storage ceiling (satoru)
#define KVFS_BLOCK_SIZE   4096

enum KVFSNodeType : uint8_t {
    KVFS_FILE      = 0,
    KVFS_DIR       = 1,
    KVFS_SYMLINK   = 2,
    KVFS_DEVICE    = 3,
    KVFS_PIPE      = 4,
    KVFS_MOUNTPOINT= 5,
};

struct KVFSPermissions {
    uint16_t mode;    // unix-style: rwxrwxrwx (9 bits)
    uint16_t uid;
    uint16_t gid;

    bool can_read(uint16_t u, uint16_t g) const {
        if (u == 0) return true;  // root
        if (u == uid) return (mode & 0400) != 0;
        if (g == gid) return (mode & 040) != 0;
        return (mode & 04) != 0;
    }
    bool can_write(uint16_t u, uint16_t g) const {
        if (u == 0) return true;
        if (u == uid) return (mode & 0200) != 0;
        if (g == gid) return (mode & 020) != 0;
        return (mode & 02) != 0;
    }
    bool can_exec(uint16_t u, uint16_t g) const {
        if (u == 0) return true;
        if (u == uid) return (mode & 0100) != 0;
        if (g == gid) return (mode & 010) != 0;
        return (mode & 01) != 0;
    }
};

struct KVFSNode {
    char name[KVFS_MAX_NAME];
    KVFSNodeType type;
    KVFSPermissions perms;
    uint32_t size;
    uint32_t created;   // seconds since boot
    uint32_t modified;
    uint32_t accessed;

    KVFSNode* parent;
    KVFSNode* children[KVFS_MAX_CHILDREN];
    int child_count;

    // file content (inline for small files)
    uint8_t* content;
    uint32_t content_capacity;

    // symlink target
    char link_target[KVFS_MAX_PATH];

    // device callbacks
    int (*dev_read)(KVFSNode* node, uint32_t offset, uint32_t len, uint8_t* buf);
    int (*dev_write)(KVFSNode* node, uint32_t offset, uint32_t len, const uint8_t* buf);

    // small open-addressing hash; size = next power of two >= KVFS_MAX_CHILDREN*2.
    // 1024 buckets, each storing the index into children[] (-1 = empty).
    static const int HASH_SIZE = 1024;
    int16_t hash_table[HASH_SIZE];

    bool is_dir() const { return type == KVFS_DIR || type == KVFS_MOUNTPOINT; }
    bool is_file() const { return type == KVFS_FILE; }

    static uint32_t hash_name(const char* s) {
        uint32_t h = 2166136261u;
        while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
        return h;
    }

    KVFSNode* find_child(const char* n) const {
        if (child_count == 0) return nullptr;
        uint32_t h = hash_name(n) & (HASH_SIZE - 1);
        for (int probe = 0; probe < HASH_SIZE; probe++) {
            int slot = (h + probe) & (HASH_SIZE - 1);
            int16_t idx = hash_table[slot];
            if (idx < 0) return nullptr;
            if (idx < child_count && children[idx] && str_eq(children[idx]->name, n))
                return children[idx];
        }
        return nullptr;
    }

    void rebuild_hash() {
        for (int i = 0; i < HASH_SIZE; i++) hash_table[i] = -1;
        for (int i = 0; i < child_count; i++) {
            if (!children[i]) continue;
            uint32_t h = hash_name(children[i]->name) & (HASH_SIZE - 1);
            for (int probe = 0; probe < HASH_SIZE; probe++) {
                int slot = (h + probe) & (HASH_SIZE - 1);
                if (hash_table[slot] < 0) { hash_table[slot] = (int16_t)i; break; }
            }
        }
    }

    bool add_child(KVFSNode* c) {
        if (child_count >= KVFS_MAX_CHILDREN) return false;
        c->parent = this;
        children[child_count] = c;
        uint32_t h = hash_name(c->name) & (HASH_SIZE - 1);
        for (int probe = 0; probe < HASH_SIZE; probe++) {
            int slot = (h + probe) & (HASH_SIZE - 1);
            if (hash_table[slot] < 0) { hash_table[slot] = (int16_t)child_count; break; }
        }
        child_count++;
        return true;
    }

    bool remove_child(const char* n) {
        for (int i = 0; i < child_count; i++) {
            if (children[i] && str_eq(children[i]->name, n)) {
                for (int j = i; j < child_count - 1; j++)
                    children[j] = children[j + 1];
                child_count--;
                rebuild_hash();
                return true;
            }
        }
        return false;
    }

private:
    static bool str_eq(const char* a, const char* b) {
        while (*a && *b) { if (*a != *b) return false; a++; b++; }
        return *a == 0 && *b == 0;
    }
};

struct KVFSFileDesc {
    KVFSNode* node;
    uint32_t offset;
    uint8_t flags;  // 1=read, 2=write, 4=append
    bool open;
};

#define KVFS_MAX_FDS 64

enum KVFSError {
    KVFS_OK = 0,
    KVFS_ERR_NOT_FOUND = -1,
    KVFS_ERR_EXISTS = -2,
    KVFS_ERR_NOT_DIR = -3,
    KVFS_ERR_IS_DIR = -4,
    KVFS_ERR_FULL = -5,
    KVFS_ERR_PERM = -6,
    KVFS_ERR_NO_MEM = -7,
    KVFS_ERR_NOT_EMPTY = -8,
    KVFS_ERR_INVALID = -9,
    KVFS_ERR_NO_FD = -10,
};

//  kvfs  -  main filesystem class

class KVFS {
public:
    static void Init();

    // path operations
    static KVFSNode* Resolve(const char* path);
    static KVFSNode* ResolvePath(const char* path, KVFSNode* cwd = nullptr);

    // directory operations
    static int Mkdir(const char* path, uint16_t mode = 0755);
    static int Mkdirs(const char* path, uint16_t mode = 0755);
    static int Rmdir(const char* path);
    static int Listdir(const char* path, KVFSNode** out, int max_count);

    // file operations
    static int CreateFile(const char* path, uint16_t mode = 0644);
    static int WriteFile(const char* path, const void* data, uint32_t len);
    static int AppendFile(const char* path, const void* data, uint32_t len);
    static int ReadFile(const char* path, void* buf, uint32_t max_len);
    static int Unlink(const char* path);

    // file descriptors
    static int Open(const char* path, uint8_t flags);
    static int Read(int fd, void* buf, uint32_t len);
    static int Write(int fd, const void* buf, uint32_t len);
    static int Seek(int fd, int32_t offset, int whence);  // 0=set, 1=cur, 2=end
    static int Close(int fd);

    // copy / move
    static int Copy(const char* src, const char* dst);
    static int Move(const char* src, const char* dst);

    // metadata
    static int Chmod(const char* path, uint16_t mode);
    static int Chown(const char* path, uint16_t uid, uint16_t gid);
    static int Stat(const char* path, KVFSNode** out);

    // working directory
    static void SetCwd(const char* path);
    static const char* GetCwd();
    static KVFSNode* GetCwdNode();

    // search
    static int Find(const char* path, const char* pattern, KVFSNode** results, int max_results);
    static int Grep(const char* path, const char* pattern, char* output, int max_output);

    // tree builder
    static int RmTree(const char* path);
    static uint32_t DiskUsage(const char* path);

    // helpers
    static KVFSNode* GetRoot();
    static int GetFileSize(const char* path);
    static bool Exists(const char* path);
    static bool IsDir(const char* path);
    static bool IsFile(const char* path);

    // string file helpers
    static int WriteString(const char* path, const char* str);
    static int ReadString(const char* path, char* buf, int max_len);

    // persistence  -  flatten the whole tree to / rebuild it from a binary blob.
    // Serialize returns bytes written (0 on failure/overflow); Deserialize
    // returns true on success and leaves the live tree untouched on any
    // malformation. (satoru)
    static size_t Serialize(uint8_t* buffer, size_t maxSize);
    static bool   Deserialize(const uint8_t* buffer, size_t size);

    // panic-path best-effort write. The kernel panic handler runs from an
    // exception/IRQ context that may have interrupted a process holding
    // g_vfs_lock; since the lock is a non-recursive SpinLockCpuGuard, an
    // unconditional WriteFile() there would self-deadlock the dying CPU and
    // silently kill the crash dump. This TRIES the lock (no spin) and writes
    // via the _nolock cores only if it's free; if the lock is held it skips
    // persistence (the physical-RAM minidump already covers recovery) and
    // returns false rather than hanging. ONLY for the panic path. (satoru)
    static bool TryWriteCrashDump(const char* path, const void* data, uint32_t len);

private:
    static KVFSNode* root;
    static KVFSNode* cwd;
    static char cwd_path[KVFS_MAX_PATH];
    static KVFSFileDesc fds[KVFS_MAX_FDS];

    // ── unlocked cores ──────────────────────────────────────────────────
    // These contain the real logic and assume g_vfs_lock is ALREADY held.
    // The public methods above are thin wrappers that take g_vfs_lock once
    // and delegate here, so a public op that needs another op calls the
    // _nolock core instead of re-entering the (non-recursive) lock. (satoru)
    static KVFSNode* Resolve_nolock(const char* path);
    static KVFSNode* ResolvePath_nolock(const char* path, KVFSNode* from = nullptr);
    static int Mkdirs_nolock(const char* path, uint16_t mode = 0755);
    static int CreateFile_nolock(const char* path, uint16_t mode = 0644);
    static int WriteFile_nolock(const char* path, const void* data, uint32_t len);
    static int ReadFile_nolock(const char* path, void* buf, uint32_t max_len);
    static int Unlink_nolock(const char* path);
    static int Copy_nolock(const char* src, const char* dst);

    static KVFSNode* AllocNode(const char* name, KVFSNodeType type, uint16_t mode = 0755);
    static void FreeNode(KVFSNode* node);
    static void FreeTree(KVFSNode* node);
    static void BuildDefaultTree();
    // depth-first serialize of one node + its subtree into buffer at *pos,
    // bounds-checked against maxSize; returns false on overflow. persist_content
    // gates whether file BYTES are written: the tree structure is always saved,
    // but content is only kept for user-data subtrees (/home, /etc, /root) so the
    // image stays small  -  the big re-seeded /usr binaries + sample media are
    // restored as empty entries and re-filled by the boot seeding. (satoru)
    static bool SerializeNode(const KVFSNode* node, uint8_t* buffer,
                              size_t maxSize, size_t* pos, uint32_t* count,
                              bool persist_content);
    static void NormalizePath(const char* input, char* output, int max_len);
    static bool PatternMatch(const char* pattern, const char* str);
    static void FindRecursive(KVFSNode* node, const char* pattern,
                              KVFSNode** results, int max_results, int& count);
};
