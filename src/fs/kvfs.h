#pragma once
#include "../kernel/types.h"
#include "../kernel/heap.h"

//  kurono virtual file system  -  full hierarchical fs with posix semantics

#define KVFS_MAX_NAME     64
#define KVFS_MAX_PATH     256
#define KVFS_MAX_CHILDREN 128
#define KVFS_MAX_CONTENT  (64 * 1024)  // 64kb per file max
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

    bool is_dir() const { return type == KVFS_DIR || type == KVFS_MOUNTPOINT; }
    bool is_file() const { return type == KVFS_FILE; }

    KVFSNode* find_child(const char* n) const {
        for (int i = 0; i < child_count; i++) {
            if (children[i] && str_eq(children[i]->name, n))
                return children[i];
        }
        return nullptr;
    }

    bool add_child(KVFSNode* c) {
        if (child_count >= KVFS_MAX_CHILDREN) return false;
        c->parent = this;
        children[child_count++] = c;
        return true;
    }

    bool remove_child(const char* n) {
        for (int i = 0; i < child_count; i++) {
            if (children[i] && str_eq(children[i]->name, n)) {
                for (int j = i; j < child_count - 1; j++)
                    children[j] = children[j + 1];
                child_count--;
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

private:
    static KVFSNode* root;
    static KVFSNode* cwd;
    static char cwd_path[KVFS_MAX_PATH];
    static KVFSFileDesc fds[KVFS_MAX_FDS];

    static KVFSNode* AllocNode(const char* name, KVFSNodeType type, uint16_t mode = 0755);
    static void FreeNode(KVFSNode* node);
    static void FreeTree(KVFSNode* node);
    static void BuildDefaultTree();
    static void NormalizePath(const char* input, char* output, int max_len);
    static bool PatternMatch(const char* pattern, const char* str);
    static void FindRecursive(KVFSNode* node, const char* pattern,
                              KVFSNode** results, int max_results, int& count);
};
