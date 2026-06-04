#include "kvfs.h"
#include "../drivers/serial.h"
#include "../kernel/time.h"

KVFSNode* KVFS::root = nullptr;
KVFSNode* KVFS::cwd = nullptr;
char KVFS::cwd_path[KVFS_MAX_PATH] = "/home/user";
KVFSFileDesc KVFS::fds[KVFS_MAX_FDS];

static int kstrlen(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}

static void kstrcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void kstrcat(char* dst, const char* src, int max) {
    int d = kstrlen(dst);
    int i = 0;
    while (src[i] && d + i < max - 1) { dst[d + i] = src[i]; i++; }
    dst[d + i] = 0;
}

static bool kstreq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

static void kmemcpy(void* dst, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static void kmemset(void* dst, uint8_t v, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = v;
}

// small recently-resolved-path cache so repeated Open of the same path
// doesn't re-walk the tree
struct PathCacheEntry { char path[KVFS_MAX_PATH]; KVFSNode* node; uint32_t lru; bool valid; };
static const int PATH_CACHE_N = 16;
static PathCacheEntry path_cache[PATH_CACHE_N];
static uint32_t path_cache_clock = 0;

static KVFSNode* PathCacheGet(const char* p) {
    for (int i = 0; i < PATH_CACHE_N; i++) {
        if (path_cache[i].valid && kstreq(path_cache[i].path, p)) {
            path_cache[i].lru = ++path_cache_clock;
            return path_cache[i].node;
        }
    }
    return nullptr;
}

static void PathCachePut(const char* p, KVFSNode* n) {
    if (!n) return;
    int victim = 0;
    uint32_t worst = 0xFFFFFFFFu;
    for (int i = 0; i < PATH_CACHE_N; i++) {
        if (!path_cache[i].valid) { victim = i; break; }
        if (path_cache[i].lru < worst) { victim = i; worst = path_cache[i].lru; }
    }
    kstrcpy(path_cache[victim].path, p, KVFS_MAX_PATH);
    path_cache[victim].node = n;
    path_cache[victim].valid = true;
    path_cache[victim].lru = ++path_cache_clock;
}

static void PathCacheInvalidate() {
    for (int i = 0; i < PATH_CACHE_N; i++) path_cache[i].valid = false;
    path_cache_clock = 0;
}

KVFSNode* KVFS::AllocNode(const char* name, KVFSNodeType type, uint16_t mode) {
    KVFSNode* n = (KVFSNode*)KernelHeap::Alloc(sizeof(KVFSNode));
    if (!n) return nullptr;
    kmemset(n, 0, sizeof(KVFSNode));
    kstrcpy(n->name, name, KVFS_MAX_NAME);
    n->type = type;
    n->perms.mode = mode;
    n->perms.uid = 0;
    n->perms.gid = 0;
    n->size = 0;
    n->parent = nullptr;
    n->child_count = 0;
    n->content = nullptr;
    n->content_capacity = 0;
    n->dev_read = nullptr;
    n->dev_write = nullptr;
    n->link_target[0] = 0;
    for (int i = 0; i < KVFSNode::HASH_SIZE; i++) n->hash_table[i] = -1;
    return n;
}

void KVFS::FreeNode(KVFSNode* node) {
    if (!node) return;
    if (node->content) KernelHeap::Free(node->content);
    KernelHeap::Free(node);
}

void KVFS::FreeTree(KVFSNode* node) {
    if (!node) return;
    // iterative post-order to avoid kernel stack blowouts on deep trees
    KVFSNode* stack[256];
    int sp = 0;
    stack[sp++] = node;
    KVFSNode* order[1024];
    int op = 0;
    while (sp > 0 && op < 1024) {
        KVFSNode* n = stack[--sp];
        order[op++] = n;
        for (int i = 0; i < n->child_count && sp < 256; i++) {
            if (n->children[i]) stack[sp++] = n->children[i];
        }
    }
    for (int i = op - 1; i >= 0; i--) FreeNode(order[i]);
}

void KVFS::NormalizePath(const char* input, char* output, int max_len) {
    char temp[KVFS_MAX_PATH];
    temp[0] = 0;

    if (input[0] == '/') {
        kstrcpy(temp, input, KVFS_MAX_PATH);
    } else if (input[0] == '~') {
        kstrcpy(temp, "/home/user", KVFS_MAX_PATH);
        if (input[1]) kstrcat(temp, input + 1, KVFS_MAX_PATH);
    } else {
        kstrcpy(temp, cwd_path, KVFS_MAX_PATH);
        int tlen = kstrlen(temp);
        if (tlen == 0 || temp[tlen - 1] != '/') kstrcat(temp, "/", KVFS_MAX_PATH);
        kstrcat(temp, input, KVFS_MAX_PATH);
    }

    // split by /, resolve . and ..
    char parts[32][KVFS_MAX_NAME];
    int part_count = 0;

    int i = 0, j = 0;
    while (true) {
        char c = temp[i];
        if (c == '/' || c == 0) {
            if (j > 0) {
                if (part_count < 32) {
                    parts[part_count][j] = 0;
                    if (kstreq(parts[part_count], ".")) {
                        // skip
                    } else if (kstreq(parts[part_count], "..")) {
                        if (part_count > 0) part_count--;
                    } else {
                        part_count++;
                    }
                }
                j = 0;
            }
            if (c == 0) break;
        } else {
            if (j < KVFS_MAX_NAME - 1 && part_count < 32) parts[part_count][j++] = c;
        }
        i++;
    }

    // reconstruct
    int op = 0;
    if (part_count == 0) {
        if (max_len > 1) { output[0] = '/'; output[1] = 0; }
        else if (max_len > 0) output[0] = 0;
        return;
    }
    for (int k = 0; k < part_count; k++) {
        if (op < max_len - 1) output[op++] = '/';
        for (int x = 0; parts[k][x] && op < max_len - 1; x++) output[op++] = parts[k][x];
    }
    output[op] = 0;
}

KVFSNode* KVFS::Resolve(const char* path) {
    char norm[KVFS_MAX_PATH];
    NormalizePath(path, norm, KVFS_MAX_PATH);
    KVFSNode* cached = PathCacheGet(norm);
    if (cached) return cached;
    KVFSNode* n = ResolvePath(norm, nullptr);
    if (n) PathCachePut(norm, n);
    return n;
}

KVFSNode* KVFS::ResolvePath(const char* path, KVFSNode* from) {
    if (!root) return nullptr;
    if (!path || path[0] == 0) return cwd ? cwd : root;

    KVFSNode* cur;
    int i = 0;

    if (path[0] == '/') {
        cur = root;
        i = 1;
    } else {
        cur = from ? from : (cwd ? cwd : root);
    }

    if (path[0] == '/' && path[1] == 0) return root;

    char component[KVFS_MAX_NAME];
    int ci = 0;

    while (true) {
        if (path[i] == '/' || path[i] == 0) {
            if (ci > 0) {
                component[ci] = 0;
                if (kstreq(component, ".")) {
                    // stay
                } else if (kstreq(component, "..")) {
                    if (cur->parent) cur = cur->parent;
                } else {
                    KVFSNode* child = cur->find_child(component);
                    if (!child) return nullptr;
                    // iterative symlink follow with hop budget
                    int hops = 0;
                    while (child && child->type == KVFS_SYMLINK && hops < 16) {
                        char ntmp[KVFS_MAX_PATH];
                        NormalizePath(child->link_target, ntmp, KVFS_MAX_PATH);
                        // direct walk (don't re-enter Resolve which would re-cache wrong)
                        KVFSNode* tgt = root;
                        int ii = 1;
                        char comp2[KVFS_MAX_NAME];
                        int cci = 0;
                        bool fail = false;
                        if (ntmp[0] != '/' || ntmp[1] == 0) { tgt = ntmp[0] == '/' ? root : nullptr; }
                        while (tgt && ntmp[ii]) {
                            if (ntmp[ii] == '/' || ntmp[ii + 1] == 0) {
                                if (ntmp[ii] != '/') { if (cci < KVFS_MAX_NAME - 1) comp2[cci++] = ntmp[ii]; ii++; }
                                if (cci > 0) {
                                    comp2[cci] = 0;
                                    tgt = tgt->find_child(comp2);
                                    cci = 0;
                                    if (!tgt) { fail = true; break; }
                                }
                                if (ntmp[ii] == '/') ii++;
                            } else {
                                if (cci < KVFS_MAX_NAME - 1) comp2[cci++] = ntmp[ii];
                                ii++;
                            }
                        }
                        if (fail) return nullptr;
                        child = tgt;
                        hops++;
                    }
                    if (!child) return nullptr;
                    cur = child;
                }
                ci = 0;
            }
            if (path[i] == 0) break;
            i++;
        } else {
            if (ci < KVFS_MAX_NAME - 1) component[ci++] = path[i];
            i++;
        }
    }

    return cur;
}

int KVFS::Mkdir(const char* path, uint16_t mode) {
    char norm[KVFS_MAX_PATH];
    NormalizePath(path, norm, KVFS_MAX_PATH);

    char parent_path[KVFS_MAX_PATH];
    char name[KVFS_MAX_NAME];
    int len = kstrlen(norm);

    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (norm[i] == '/') { last_slash = i; break; }
    }
    if (last_slash < 0) return KVFS_ERR_INVALID;

    if (last_slash == 0) {
        parent_path[0] = '/'; parent_path[1] = 0;
    } else {
        for (int i = 0; i < last_slash && i < KVFS_MAX_PATH - 1; i++) parent_path[i] = norm[i];
        parent_path[last_slash] = 0;
    }
    kstrcpy(name, norm + last_slash + 1, KVFS_MAX_NAME);
    if (name[0] == 0) return KVFS_ERR_INVALID;

    KVFSNode* parent = ResolvePath(parent_path);
    if (!parent) return KVFS_ERR_NOT_FOUND;
    if (!parent->is_dir()) return KVFS_ERR_NOT_DIR;
    if (parent->find_child(name)) return KVFS_ERR_EXISTS;

    KVFSNode* dir = AllocNode(name, KVFS_DIR, mode);
    if (!dir) return KVFS_ERR_NO_MEM;
    if (!parent->add_child(dir)) { FreeNode(dir); return KVFS_ERR_FULL; }
    PathCacheInvalidate();
    return KVFS_OK;
}

int KVFS::Mkdirs(const char* path, uint16_t mode) {
    char norm[KVFS_MAX_PATH];
    NormalizePath(path, norm, KVFS_MAX_PATH);

    KVFSNode* cur = root;
    int i = 1;
    char component[KVFS_MAX_NAME];
    int ci = 0;

    while (true) {
        if (norm[i] == '/' || norm[i] == 0) {
            if (ci > 0) {
                component[ci] = 0;
                KVFSNode* child = cur->find_child(component);
                if (!child) {
                    child = AllocNode(component, KVFS_DIR, mode);
                    if (!child) return KVFS_ERR_NO_MEM;
                    if (!cur->add_child(child)) { FreeNode(child); return KVFS_ERR_FULL; }
                }
                if (!child->is_dir()) return KVFS_ERR_NOT_DIR;
                cur = child;
                ci = 0;
            }
            if (norm[i] == 0) break;
            i++;
        } else {
            if (ci < KVFS_MAX_NAME - 1) component[ci++] = norm[i];
            i++;
        }
    }
    PathCacheInvalidate();
    return KVFS_OK;
}

int KVFS::Rmdir(const char* path) {
    KVFSNode* node = Resolve(path);
    if (!node) return KVFS_ERR_NOT_FOUND;
    if (!node->is_dir()) return KVFS_ERR_NOT_DIR;
    if (node->child_count > 0) return KVFS_ERR_NOT_EMPTY;
    if (!node->parent) return KVFS_ERR_PERM;
    KVFSNode* p = node->parent;
    p->remove_child(node->name);
    FreeNode(node);
    PathCacheInvalidate();
    return KVFS_OK;
}

int KVFS::Listdir(const char* path, KVFSNode** out, int max_count) {
    KVFSNode* node = Resolve(path);
    if (!node) return KVFS_ERR_NOT_FOUND;
    if (!node->is_dir()) return KVFS_ERR_NOT_DIR;
    int count = 0;
    for (int i = 0; i < node->child_count && count < max_count; i++) {
        out[count++] = node->children[i];
    }
    return count;
}

int KVFS::CreateFile(const char* path, uint16_t mode) {
    char norm[KVFS_MAX_PATH];
    NormalizePath(path, norm, KVFS_MAX_PATH);

    char parent_path[KVFS_MAX_PATH];
    char name[KVFS_MAX_NAME];
    int len = kstrlen(norm);
    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (norm[i] == '/') { last_slash = i; break; }
    }
    if (last_slash < 0) return KVFS_ERR_INVALID;
    if (last_slash == 0) {
        parent_path[0] = '/'; parent_path[1] = 0;
    } else {
        for (int i = 0; i < last_slash && i < KVFS_MAX_PATH - 1; i++) parent_path[i] = norm[i];
        parent_path[last_slash] = 0;
    }
    kstrcpy(name, norm + last_slash + 1, KVFS_MAX_NAME);
    if (name[0] == 0) return KVFS_ERR_INVALID;

    KVFSNode* parent = ResolvePath(parent_path);
    if (!parent) {
        Mkdirs(parent_path);
        parent = ResolvePath(parent_path);
        if (!parent) return KVFS_ERR_NOT_FOUND;
    }
    if (!parent->is_dir()) return KVFS_ERR_NOT_DIR;

    KVFSNode* existing = parent->find_child(name);
    if (existing) {
        if (existing->is_dir()) return KVFS_ERR_IS_DIR;
        return KVFS_OK;
    }

    KVFSNode* f = AllocNode(name, KVFS_FILE, mode);
    if (!f) return KVFS_ERR_NO_MEM;
    if (!parent->add_child(f)) { FreeNode(f); return KVFS_ERR_FULL; }
    PathCacheInvalidate();
    return KVFS_OK;
}

int KVFS::WriteFile(const char* path, const void* data, uint32_t len) {
    KVFSNode* node = Resolve(path);
    if (!node) {
        int r = CreateFile(path);
        if (r != KVFS_OK) return r;
        node = Resolve(path);
        if (!node) return KVFS_ERR_NOT_FOUND;
    }
    if (node->is_dir()) return KVFS_ERR_IS_DIR;

    if (!node->content || node->content_capacity < len) {
        uint32_t new_cap = (len + KVFS_BLOCK_SIZE - 1) & ~(KVFS_BLOCK_SIZE - 1);
        if (new_cap < KVFS_BLOCK_SIZE) new_cap = KVFS_BLOCK_SIZE;
        uint8_t* new_buf = (uint8_t*)KernelHeap::Alloc(new_cap);
        if (!new_buf) return KVFS_ERR_NO_MEM;
        if (node->content) KernelHeap::Free(node->content);
        node->content = new_buf;
        node->content_capacity = new_cap;
    }

    if (len > 0 && data) kmemcpy(node->content, data, len);
    node->size = len;
    return KVFS_OK;
}

int KVFS::AppendFile(const char* path, const void* data, uint32_t len) {
    KVFSNode* node = Resolve(path);
    if (!node) return WriteFile(path, data, len);
    if (node->is_dir()) return KVFS_ERR_IS_DIR;

    uint32_t new_size = node->size + len;
    if (!node->content || node->content_capacity < new_size) {
        uint32_t new_cap = (new_size + KVFS_BLOCK_SIZE - 1) & ~(KVFS_BLOCK_SIZE - 1);
        if (new_cap < KVFS_BLOCK_SIZE) new_cap = KVFS_BLOCK_SIZE;
        uint8_t* new_buf = (uint8_t*)KernelHeap::Alloc(new_cap);
        if (!new_buf) return KVFS_ERR_NO_MEM;
        if (node->content) {
            kmemcpy(new_buf, node->content, node->size);
            KernelHeap::Free(node->content);
        }
        node->content = new_buf;
        node->content_capacity = new_cap;
    }

    if (len > 0 && data) kmemcpy(node->content + node->size, data, len);
    node->size = new_size;
    return KVFS_OK;
}

int KVFS::ReadFile(const char* path, void* buf, uint32_t max_len) {
    KVFSNode* node = Resolve(path);
    if (!node) return KVFS_ERR_NOT_FOUND;
    if (node->is_dir()) return KVFS_ERR_IS_DIR;
    uint32_t to_read = node->size < max_len ? node->size : max_len;
    if (node->content && to_read > 0 && buf)
        kmemcpy(buf, node->content, to_read);
    return (int)to_read;
}

int KVFS::Unlink(const char* path) {
    KVFSNode* node = Resolve(path);
    if (!node) return KVFS_ERR_NOT_FOUND;
    if (node->is_dir()) return KVFS_ERR_IS_DIR;
    if (!node->parent) return KVFS_ERR_PERM;
    // close any fds that reference this node to prevent UAF
    for (int i = 0; i < KVFS_MAX_FDS; i++) {
        if (fds[i].open && fds[i].node == node) {
            fds[i].open = false;
            fds[i].node = nullptr;
        }
    }
    KVFSNode* p = node->parent;
    p->remove_child(node->name);
    FreeNode(node);
    PathCacheInvalidate();
    return KVFS_OK;
}

int KVFS::Open(const char* path, uint8_t flags) {
    KVFSNode* node = Resolve(path);
    if (!node) {
        if (flags & 2) {
            int r = CreateFile(path);
            if (r != KVFS_OK) return r;
            node = Resolve(path);
            if (!node) return KVFS_ERR_NOT_FOUND;
        } else {
            return KVFS_ERR_NOT_FOUND;
        }
    }
    if (node->is_dir()) return KVFS_ERR_IS_DIR;
    for (int i = 3; i < KVFS_MAX_FDS; i++) {
        if (!fds[i].open) {
            fds[i].node = node;
            fds[i].offset = (flags & 4) ? node->size : 0;
            fds[i].flags = flags;
            fds[i].open = true;
            return i;
        }
    }
    return KVFS_ERR_NO_FD;
}

int KVFS::Read(int fd, void* buf, uint32_t len) {
    if (fd < 0 || fd >= KVFS_MAX_FDS || !fds[fd].open) return KVFS_ERR_INVALID;
    KVFSNode* node = fds[fd].node;
    if (!node) return KVFS_ERR_INVALID;

    if (node->dev_read) {
        int r = node->dev_read(node, fds[fd].offset, len, (uint8_t*)buf);
        if (r > 0) fds[fd].offset += (uint32_t)r;
        return r;
    }
    if (!node->content) return 0;

    uint32_t avail = node->size > fds[fd].offset ? node->size - fds[fd].offset : 0;
    uint32_t to_read = len < avail ? len : avail;
    if (to_read > 0 && buf) kmemcpy(buf, node->content + fds[fd].offset, to_read);
    fds[fd].offset += to_read;
    return (int)to_read;
}

int KVFS::Write(int fd, const void* buf, uint32_t len) {
    if (fd < 0 || fd >= KVFS_MAX_FDS || !fds[fd].open) return KVFS_ERR_INVALID;
    if (!(fds[fd].flags & 2)) return KVFS_ERR_PERM;
    KVFSNode* node = fds[fd].node;
    if (!node) return KVFS_ERR_INVALID;

    if (node->dev_write) {
        int r = node->dev_write(node, fds[fd].offset, len, (const uint8_t*)buf);
        if (r > 0) fds[fd].offset += (uint32_t)r;
        return r;
    }

    uint32_t write_end = fds[fd].offset + len;
    if (!node->content || node->content_capacity < write_end) {
        uint32_t new_cap = (write_end + KVFS_BLOCK_SIZE - 1) & ~(KVFS_BLOCK_SIZE - 1);
        if (new_cap < KVFS_BLOCK_SIZE) new_cap = KVFS_BLOCK_SIZE;
        uint8_t* new_buf = (uint8_t*)KernelHeap::Alloc(new_cap);
        if (!new_buf) return KVFS_ERR_NO_MEM;
        if (node->content) {
            kmemcpy(new_buf, node->content, node->size);
            KernelHeap::Free(node->content);
        } else {
            kmemset(new_buf, 0, new_cap);
        }
        node->content = new_buf;
        node->content_capacity = new_cap;
    }
    if (len > 0 && buf) kmemcpy(node->content + fds[fd].offset, buf, len);
    fds[fd].offset += len;
    if (fds[fd].offset > node->size) node->size = fds[fd].offset;
    return (int)len;
}

int KVFS::Seek(int fd, int32_t offset, int whence) {
    if (fd < 0 || fd >= KVFS_MAX_FDS || !fds[fd].open) return KVFS_ERR_INVALID;
    KVFSNode* node = fds[fd].node;
    if (!node) return KVFS_ERR_INVALID;
    int32_t new_off;
    switch (whence) {
        case 0: new_off = offset; break;
        case 1: new_off = (int32_t)fds[fd].offset + offset; break;
        case 2: new_off = (int32_t)node->size + offset; break;
        default: return KVFS_ERR_INVALID;
    }
    if (new_off < 0) new_off = 0;
    fds[fd].offset = (uint32_t)new_off;
    return (int)fds[fd].offset;
}

int KVFS::Close(int fd) {
    if (fd < 0 || fd >= KVFS_MAX_FDS || !fds[fd].open) return KVFS_ERR_INVALID;
    fds[fd].open = false;
    fds[fd].node = nullptr;
    return KVFS_OK;
}

int KVFS::Copy(const char* src, const char* dst) {
    KVFSNode* s = Resolve(src);
    if (!s) return KVFS_ERR_NOT_FOUND;
    if (s->is_dir()) return KVFS_ERR_IS_DIR;
    return WriteFile(dst, s->content, s->size);
}

int KVFS::Move(const char* src, const char* dst) {
    int r = Copy(src, dst);
    if (r < 0) return r;
    return Unlink(src);
}

int KVFS::Chmod(const char* path, uint16_t mode) {
    KVFSNode* n = Resolve(path);
    if (!n) return KVFS_ERR_NOT_FOUND;
    n->perms.mode = mode;
    return KVFS_OK;
}

int KVFS::Chown(const char* path, uint16_t uid, uint16_t gid) {
    KVFSNode* n = Resolve(path);
    if (!n) return KVFS_ERR_NOT_FOUND;
    n->perms.uid = uid;
    n->perms.gid = gid;
    return KVFS_OK;
}

int KVFS::Stat(const char* path, KVFSNode** out) {
    KVFSNode* n = Resolve(path);
    if (!n) return KVFS_ERR_NOT_FOUND;
    if (out) *out = n;
    return KVFS_OK;
}

void KVFS::SetCwd(const char* path) {
    char norm[KVFS_MAX_PATH];
    NormalizePath(path, norm, KVFS_MAX_PATH);
    KVFSNode* node = ResolvePath(norm);
    if (node && node->is_dir()) {
        cwd = node;
        kstrcpy(cwd_path, norm, KVFS_MAX_PATH);
    }
}

const char* KVFS::GetCwd() { return cwd_path; }
KVFSNode* KVFS::GetCwdNode() { return cwd; }

bool KVFS::PatternMatch(const char* pattern, const char* str) {
    // iterative wildcard match (no recursion): backtracks on '*' star
    const char* p = pattern;
    const char* s = str;
    const char* star_p = nullptr;
    const char* star_s = nullptr;
    while (*s) {
        if (*p == '?') { p++; s++; }
        else if (*p == '*') { star_p = ++p; star_s = s; }
        else if (*p == *s) { p++; s++; }
        else if (star_p) { p = star_p; s = ++star_s; }
        else return false;
    }
    while (*p == '*') p++;
    return *p == 0;
}

void KVFS::FindRecursive(KVFSNode* node, const char* pattern,
                          KVFSNode** results, int max_results, int& count) {
    if (!node || count >= max_results) return;
    // iterative BFS to avoid blowing the kernel stack on deep trees
    KVFSNode* stack[256];
    int sp = 0;
    stack[sp++] = node;
    while (sp > 0 && count < max_results) {
        KVFSNode* n = stack[--sp];
        for (int i = 0; i < n->child_count; i++) {
            KVFSNode* c = n->children[i];
            if (!c) continue;
            if (PatternMatch(pattern, c->name)) {
                if (count < max_results) results[count++] = c;
            }
            if (c->is_dir() && sp < 256) stack[sp++] = c;
        }
    }
}

int KVFS::Find(const char* path, const char* pattern, KVFSNode** results, int max_results) {
    KVFSNode* node = Resolve(path);
    if (!node || !node->is_dir()) return 0;
    int count = 0;
    FindRecursive(node, pattern, results, max_results, count);
    return count;
}

int KVFS::Grep(const char* path, const char* pattern, char* output, int max_output) {
    KVFSNode* node = Resolve(path);
    if (!node || !node->content) { if (output && max_output > 0) output[0] = 0; return 0; }

    int plen = kstrlen(pattern);
    int out_pos = 0;
    int line_num = 1;

    const char* content = (const char*)node->content;
    int clen = (int)node->size;
    int line_start = 0;

    for (int i = 0; i <= clen; i++) {
        if (i == clen || content[i] == '\n') {
            bool found = false;
            for (int j = line_start; j <= i - plen; j++) {
                bool match = true;
                for (int k = 0; k < plen; k++) {
                    if (content[j + k] != pattern[k]) { match = false; break; }
                }
                if (match) { found = true; break; }
            }
            if (found && out_pos < max_output - 64) {
                char num[12];
                int nd = 0;
                int ln = line_num;
                if (ln == 0) { num[nd++] = '0'; }
                else {
                    char tmp[12]; int ti = 0;
                    while (ln > 0) { tmp[ti++] = '0' + (ln % 10); ln /= 10; }
                    while (ti > 0) num[nd++] = tmp[--ti];
                }
                num[nd] = 0;
                for (int k = 0; num[k] && out_pos < max_output - 2; k++)
                    output[out_pos++] = num[k];
                output[out_pos++] = ':';
                output[out_pos++] = ' ';
                for (int k = line_start; k < i && out_pos < max_output - 2; k++)
                    output[out_pos++] = content[k];
                output[out_pos++] = '\n';
            }
            line_num++;
            line_start = i + 1;
        }
    }
    if (out_pos < max_output) output[out_pos] = 0;
    else if (max_output > 0) output[max_output - 1] = 0;
    return out_pos;
}

int KVFS::RmTree(const char* path) {
    KVFSNode* node = Resolve(path);
    if (!node) return KVFS_ERR_NOT_FOUND;
    if (!node->parent) return KVFS_ERR_PERM;
    // close any fds that reference nodes in this subtree
    for (int i = 0; i < KVFS_MAX_FDS; i++) {
        if (fds[i].open) {
            KVFSNode* n = fds[i].node;
            while (n) {
                if (n == node) { fds[i].open = false; fds[i].node = nullptr; break; }
                n = n->parent;
            }
        }
    }
    KVFSNode* p = node->parent;
    p->remove_child(node->name);
    FreeTree(node);
    PathCacheInvalidate();
    return KVFS_OK;
}

uint32_t KVFS::DiskUsage(const char* path) {
    KVFSNode* node = Resolve(path);
    if (!node) return 0;
    if (node->is_file()) return node->size;
    uint32_t total = 0;
    KVFSNode* stack[256];
    int sp = 0;
    stack[sp++] = node;
    while (sp > 0) {
        KVFSNode* n = stack[--sp];
        if (n->is_file()) total += n->size;
        for (int i = 0; i < n->child_count && sp < 256; i++) {
            if (n->children[i]) stack[sp++] = n->children[i];
        }
    }
    return total;
}

KVFSNode* KVFS::GetRoot() { return root; }

int KVFS::GetFileSize(const char* path) {
    KVFSNode* n = Resolve(path);
    if (!n) return KVFS_ERR_NOT_FOUND;
    return (int)n->size;
}

bool KVFS::Exists(const char* path) { return Resolve(path) != nullptr; }
bool KVFS::IsDir(const char* path) { KVFSNode* n = Resolve(path); return n && n->is_dir(); }
bool KVFS::IsFile(const char* path) { KVFSNode* n = Resolve(path); return n && n->is_file(); }

int KVFS::WriteString(const char* path, const char* str) {
    return WriteFile(path, str, (uint32_t)kstrlen(str));
}

int KVFS::ReadString(const char* path, char* buf, int max_len) {
    if (max_len <= 0 || !buf) return KVFS_ERR_INVALID;
    int r = ReadFile(path, buf, (uint32_t)(max_len - 1));
    if (r < 0) { buf[0] = 0; return r; }
    buf[r] = 0;
    return r;
}

void KVFS::BuildDefaultTree() {
    const char* dirs[] = {
        "/bin", "/sbin", "/usr", "/usr/bin", "/usr/lib", "/usr/share",
        "/etc", "/etc/kurono", "/etc/network", "/etc/wifi",
        "/home", "/home/user", "/home/user/Documents", "/home/user/Downloads",
        "/home/user/Desktop", "/home/user/Pictures", "/home/user/Music",
        "/home/user/Videos",
        "/var", "/var/log", "/var/lib", "/var/cache",
        "/tmp", "/proc", "/sys", "/dev",
        "/lib", "/lib64", "/root",
        "/opt", "/opt/kurono",
        "/mnt", "/mnt/usb",
        "/kurono", "/kurono/apps", "/kurono/packages", "/kurono/scripts",
        "/kurono/themes", "/kurono/drivers", "/kurono/drivers/wifi",
        "/kurono/drivers/audio", "/kurono/drivers/video",
        "/kurono/logs", "/kurono/config",
        "/windows", "/windows/System32", "/windows/Users",
        "/windows/Program Files",
        nullptr,
    };

    for (int i = 0; dirs[i]; i++) Mkdirs(dirs[i]);

    WriteString("/etc/kurono/os-release",
        "NAME=\"Kurono OS\"\nVERSION=\"1.0.0\"\nID=kurono\nPRETTY_NAME=\"Kurono OS 1.0.0\"\n");
    WriteString("/etc/kurono/hostname", "kurono-machine\n");
    WriteString("/etc/kurono/version", "1.0.0");
    WriteString("/etc/kurono/passwd",
        "root:x:0:0:root:/root:/bin/ksh\nuser:x:1000:1000:User:/home/user:/bin/ksh\n");
    WriteString("/etc/network/interfaces",
        "auto lo\niface lo inet loopback\nauto eth0\niface eth0 inet dhcp\n");
    WriteString("/etc/wifi/wpa_supplicant.conf",
        "ctrl_interface=/var/run/wpa_supplicant\nnetwork={\n  ssid=\"KuronoNet\"\n  psk=\"kurono\"\n}\n");
    WriteString("/home/user/Documents/welcome.txt",
        "Welcome to Kurono OS!\n\nType 'help' in the terminal to get started.\n"
        "Use Ctrl+T to open a terminal.\nUse the taskbar to launch apps.\n");
    WriteString("/home/user/Desktop/hello.kcl",
        "set name = \"User\"\nprint \"Welcome to Kurono OS, $name!\"\nexec ls /home/user\n");
    WriteString("/kurono/config/theme.json",
        "{\"name\":\"Kurono Dark\",\"accent\":\"#7C6FFF\",\"bg\":\"#0D0D2B\",\"glass\":true}");
    WriteString("/kurono/drivers/wifi/driver.ini",
        "[wifi]\ndriver=kurono_wifi_ng\nversion=1.0\nchips=Intel,Broadcom,Realtek,Atheros\n");
    WriteString("/proc/version", "Kurono 1.0.0 (kurono-kernel 1.0) #1 SMP x86_64");
    WriteString("/proc/cpuinfo",
        "processor\t: 0\nvendor_id\t: KuronoChip\nmodel name\t: Kurono CPU @ 3.60GHz\ncpu cores\t: 4\n");
    WriteString("/proc/meminfo",
        "MemTotal:     8388608 kB\nMemFree:      4194304 kB\nMemAvailable: 5242880 kB\n");
    WriteString("/var/log/kurono.log", "[boot] Kurono OS started successfully.\n");
}

void KVFS::Init() {
    SerialLogger::Log("KVFS: Initializing...\r\n");

    for (int i = 0; i < KVFS_MAX_FDS; i++) {
        fds[i].open = false;
        fds[i].node = nullptr;
    }
    for (int i = 0; i < PATH_CACHE_N; i++) path_cache[i].valid = false;

    root = AllocNode("/", KVFS_DIR, 0755);
    cwd = root;
    kstrcpy(cwd_path, "/", KVFS_MAX_PATH);

    BuildDefaultTree();
    SetCwd("/home/user");

    SerialLogger::Log("KVFS: Ready.\r\n");
}
