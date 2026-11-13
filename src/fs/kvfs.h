#pragma once
#include "../kernel/types.h"
#include "../kernel/heap.h"

//  kurono virtual file system - full hierarchical fs with posix semantics

#define KVFS_MAX_NAME     64
#define KVFS_MAX_PATH     256
#define KVFS_MAX_CHILDREN 512   // Listdir buffer size callers use; a directory itself is not limited by it (satoru)
#define KVFS_DIR_LIMIT    65536 // entries per directory (children grow on demand) (satoru)
#define KVFS_MAX_CONTENT  (64 * 1024)  // 64kb - shell cat/grep display chunk (stack-safe)
#define KVFS_MAX_FILE_SIZE (4 * 1024 * 1024)  // 4mb - real per-file storage ceiling (satoru)
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
    // children and their name hash grow on demand: every node used to carry a
    // fixed 512-slot array plus a 1024-slot hash (~6 KB, files too), and a
    // directory could not hold a 513th entry (satoru)
    KVFSNode** children;
    int child_count;
    int child_cap;

    // file content (inline for small files)
    uint8_t* content;
    uint32_t content_capacity;

    // symlink target
    char link_target[KVFS_MAX_PATH];

    // device callbacks
    int (*dev_read)(KVFSNode* node, uint32_t offset, uint32_t len, uint8_t* buf);
    int (*dev_write)(KVFSNode* node, uint32_t offset, uint32_t len, const uint8_t* buf);

    // true: size is valid but the bytes still live in the KFS volume, at
    // link_target (used as the backing path for files, not symlinks). reading
    // materializes or streams them; the node is dropped without any copy at
    // boot, which is the whole point of the lazy /home and /apps restore. (satoru)
    bool lazy_kfs;

    // open-addressing hash over children[] (index, -1 = empty); hash_size is a
    // power of two >= 2 * child_cap, 0 while the node has no children (satoru)
    int32_t* hash_table;
    int hash_size;

    bool is_dir() const { return type == KVFS_DIR || type == KVFS_MOUNTPOINT; }
    bool is_file() const { return type == KVFS_FILE; }

    static uint32_t hash_name(const char* s) {
        uint32_t h = 2166136261u;
        while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
        return h;
    }

    KVFSNode* find_child(const char* n) const {
        if (child_count == 0 || hash_size == 0) return nullptr;
        uint32_t h = hash_name(n) & (uint32_t)(hash_size - 1);
        for (int probe = 0; probe < hash_size; probe++) {
            int slot = (int)((h + (uint32_t)probe) & (uint32_t)(hash_size - 1));
            int32_t idx = hash_table[slot];
            if (idx < 0) return nullptr;
            if (idx < child_count && children[idx] && str_eq(children[idx]->name, n))
                return children[idx];
        }
        return nullptr;
    }

    void rebuild_hash();                 // kvfs.cpp (satoru)
    bool add_child(KVFSNode* c);         // kvfs.cpp: grows children/hash (satoru)
    void free_child_arrays();            // kvfs.cpp (satoru)

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

#define KVFS_MAX_FDS 1024   // global open-file table (was 64: tool-running sessions leaked it dry) (satoru)

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

//  kvfs - main filesystem class

class KVFS {
public:
    static void Init();

    // path operations
    static KVFSNode* Resolve(const char* path);
    static KVFSNode* ResolveNoFollow(const char* path);   // lstat/readlink (satoru)
    static KVFSNode* ResolvePath(const char* path, KVFSNode* cwd = nullptr);

    // directory operations
    static int Mkdir(const char* path, uint16_t mode = 0755);
    static int Mkdirs(const char* path, uint16_t mode = 0755);
    static int Rmdir(const char* path);
    static int Listdir(const char* path, KVFSNode** out, int max_count);

    // symlink - create (or replace a stale entry of) a KVFS_SYMLINK node at
    // `path` pointing at `target`. used to lay the compat-symlink overlay that
    // maps the old top-level names (/system /home /etc /bin /lib /tmp /proc /dev
    // /var /apps /usr/bin /usr/lib) into the canonical /kurono tree, so the ~800
    // hardcoded path references keep resolving. parent dirs are created as
    // needed. (satoru)
    static int Symlink(const char* path, const char* target);

    // install the canonical /kurono/* tree + the top-level compat symlinks.
    // MUST run at the earliest fs init (called at the tail of Init() and again
    // after a persistent-tree restore) - before any other code touches a path,
    // so a later Mkdirs("/system/...") resolves THROUGH the symlink instead of
    // materialising a real /system dir. (satoru)
    static void InstallCanonicalLayout();

    // file operations
    static int CreateFile(const char* path, uint16_t mode = 0644);
    static int WriteFile(const char* path, const void* data, uint32_t len);
    // point a file at read-only data that outlives it (an embedded kernel blob)
    // instead of copying it onto the heap. content_capacity 0 marks it borrowed:
    // every write path already reallocates when capacity is short, so the first
    // write copies it out, and Free() ignores the non-heap pointer. (satoru)
    static int WriteFileStatic(const char* path, const void* data, uint32_t len);
    static int AppendFile(const char* path, const void* data, uint32_t len);
    // ranged i/o on a whole-file node (the vm share streams files 4 KB at a
    // time): ReadFileAt returns the bytes copied (0 at/after the end);
    // WriteFileAt overwrites inside the current size only. (satoru)
    static int ReadFileAt(const char* path, uint32_t offset, void* buf, uint32_t len);
    static int WriteFileAt(const char* path, uint32_t offset, const void* data, uint32_t len);
    static int ReadFile(const char* path, void* buf, uint32_t max_len);
    // a file whose bytes stay on the KFS volume until read. returns KVFS_OK,
    // ERR_NO_MEM, or ERR_INVALID (path too long for the node's backing slot).
    static int WriteLazyFile(const char* path, uint32_t size);
    // lazy-file backing, installed by PersistStore once the KFS volume exists. (satoru)
    typedef bool    (*LazyEnsureFn)();
    typedef int64_t (*LazyReadFn)(const char* path, uint64_t off, void* buf, uint64_t len);
    static void SetLazyBackend(LazyEnsureFn ensure, LazyReadFn read);
    // bytes [off, off+len) of a node (lazy or resident); <0 on error. (satoru)
    static int ReadAt(KVFSNode* node, uint32_t off, uint32_t len, void* buf);
    // bytes [off, off+len) straight from the lazy backend by its KFS path, no
    // node pointer: for a reader that dropped the syscall lock and so cannot
    // trust a node it resolved earlier (file-backed mmap prefetch). (satoru)
    static int ReadLazyPath(const char* kfs_path, uint32_t off, uint32_t len, void* buf);
    // write-path materialize with the syscall lock dropped: allocate a content
    // buffer for `size` bytes (cap_out = its capacity), fill it with ReadLazyPath,
    // then adopt it. adopt installs it only if the node is still lazy with no
    // content, the same size and the same kfs path; otherwise it frees the buffer
    // and returns false (the write then materializes the old way). (satoru)
    // the kvfs fd reads a lazy (kfs-backed, not resident) file: its reads go to disk. (satoru)
    static bool FdIsLazy(int fd);
    static void* AllocLazyContent(uint32_t size, uint32_t* cap_out);
    static bool AdoptLazyContent(KVFSNode* node, const char* kfs_path, uint32_t size, void* buf, uint32_t cap);
    // bring a lazy node fully into ram (used by mutation/copy paths). (satoru)
    static uint8_t* Materialize(KVFSNode* node);
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

    // persistence - flatten the whole tree to / rebuild it from a binary blob.
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
    static int Symlink_nolock(const char* path, const char* target);
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
    // image stays small - the big re-seeded /usr binaries + sample media are
    // restored as empty entries and re-filled by the boot seeding. (satoru)
    static bool SerializeNode(const KVFSNode* node, uint8_t* buffer,
                              size_t maxSize, size_t* pos, uint32_t* count,
                              bool persist_content);
    static void NormalizePath(const char* input, char* output, int max_len);
    static bool PatternMatch(const char* pattern, const char* str);
    static void FindRecursive(KVFSNode* node, const char* pattern,
                              KVFSNode** results, int max_results, int& count);
};
