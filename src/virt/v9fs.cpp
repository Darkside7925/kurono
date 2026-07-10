//  kurono os - virtual 9p filesystem implementation
//  maps 9p operations to kvfs (kurono virtual file system)
#include "v9fs.h"
#include "guest_mem.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../kernel/types.h"

// scratch buffer for file i/o (shared across operations, not reentrant)
static uint8_t v9fs_iobuf[V9FS_MAX_IOSIZE + 4096];

bool      V9FS::initialized = false;
V9FS_Fid  V9FS::fids[V9FS_MAX_FIDS];

static int kstrlen(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void kstrcpy(char* dst, const char* src, int max) {
    int i = 0;
    for (; i < max - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static bool kstreq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static void kpathcat(char* dst, int max, const char* parent, const char* child) {
    int plen = kstrlen(parent);
    int clen = kstrlen(child);
    int i = 0;
    for (; i < max - 1 && i < plen; i++) dst[i] = parent[i];
    // add separator if needed
    if (i > 0 && dst[i - 1] != '/' && i < max - 1) dst[i++] = '/';
    for (int j = 0; j < clen && i < max - 1; j++) dst[i++] = child[j];
    dst[i] = '\0';
}

//  init - reset all fids

void V9FS::Init() {
    for (int i = 0; i < V9FS_MAX_FIDS; i++) {
        fids[i].active = false;
        fids[i].is_open = false;
        fids[i].is_dir = false;
        fids[i].mode = 0;
        fids[i].offset = 0;
        fids[i].dir_offset = 0;
        fids[i].path[0] = '\0';
    }
    initialized = true;
    SerialLogger::Log("V9FS: Initialized (");
    SerialLogger::LogDec(V9FS_MAX_FIDS);
    SerialLogger::Log(" FID slots)\r\n");
}

//  fid management

int V9FS::AllocFid() {
    for (int i = 1; i < V9FS_MAX_FIDS; i++) { // skip 0 (reserved for root)
        if (!fids[i].active) return i;
    }
    return -1;
}

void V9FS::FreeFid(int fid) {
    if (fid >= 0 && fid < V9FS_MAX_FIDS) {
        fids[fid].active = false;
        fids[fid].is_open = false;
        fids[fid].offset = 0;
        fids[fid].dir_offset = 0;
        fids[fid].path[0] = '\0';
    }
}

bool V9FS::ValidFid(int fid) {
    return fid >= 0 && fid < V9FS_MAX_FIDS && fids[fid].active;
}

//  vmcall dispatch
//
//  register convention (intel standard: regs[0]=rax, [1]=rcx, ...):
//    regs[0] (eax) = 0x20 (hypercall number - already consumed by caller)
//    regs[1] (ecx) = v9fs_op sub-function
//    regs[2] (edx) = arg0 (typically fid)
//    regs[3] (ebx) = arg1
//    regs[6] (esi) = arg2
//    regs[7] (edi) = arg3 (often guest phys addr of buffer)
//
//  on return, regs[0] (eax) = v9fs_err result code.

void V9FS::HandleVMCall(uint64_t* regs) {
    if (!initialized) Init();

    uint32_t op = (uint32_t)regs[1]; // ecx = sub-function
    int result;

    switch (op) {
        case V9FS_VERSION:  result = DoVersion(regs);  break;
        case V9FS_ATTACH:   result = DoAttach(regs);   break;
        case V9FS_WALK:     result = DoWalk(regs);     break;
        case V9FS_OPEN:     result = DoOpen(regs);     break;
        case V9FS_READ:     result = DoRead(regs);     break;
        case V9FS_WRITE:    result = DoWrite(regs);    break;
        case V9FS_CLUNK:    result = DoClunk(regs);    break;
        case V9FS_STAT:     result = DoStat(regs);     break;
        case V9FS_READDIR:  result = DoReadDir(regs);  break;
        case V9FS_CREATE:   result = DoCreate(regs);   break;
        case V9FS_REMOVE:   result = DoRemove(regs);   break;
        default:
            result = V9FS_ENOSYS;
            break;
    }

    regs[0] = (uint64_t)result;
}

//  doversion - negotiate protocol (always succeeds)
//  returns: eax = 0, ecx = max i/o size

int V9FS::DoVersion(uint64_t* regs) {
    regs[1] = V9FS_MAX_IOSIZE; // ecx = max i/o size
    SerialLogger::Log("V9FS: Version negotiated, iosize=");
    SerialLogger::LogDec(V9FS_MAX_IOSIZE);
    SerialLogger::Log("\r\n");
    return V9FS_OK;
}

//  doattach - attach to root ("/"), returns fid 0
//  returns: eax = 0, ecx = root fid (0)

int V9FS::DoAttach(uint64_t* regs) {
    // set up fid 0 as the root directory
    fids[0].active = true;
    fids[0].is_open = false;
    fids[0].is_dir = true;
    fids[0].mode = V9FS_OREAD;
    fids[0].offset = 0;
    fids[0].dir_offset = 0;
    kstrcpy(fids[0].path, "/", V9FS_MAX_PATH);

    regs[1] = 0; // ecx = root fid
    SerialLogger::Log("V9FS: Attached to root '/'\r\n");
    return V9FS_OK;
}

//  dowalk - walk from src_fid along a path element, create new_fid
//  edx(regs[2]) = src_fid
//  ebx(regs[3]) = new_fid (caller chooses)
//  edi(regs[7]) = guest phys addr of null-terminated path element
//  returns: eax = 0 on success

int V9FS::DoWalk(uint64_t* regs) {
    int src_fid = (int)(regs[2] & 0xFFFF);
    int new_fid = (int)(regs[3] & 0xFFFF);

    if (!ValidFid(src_fid)) return V9FS_EBADF;
    if (new_fid < 0 || new_fid >= V9FS_MAX_FIDS) return V9FS_EINVAL;

    // read the path element from guest memory
    char element[128];
    element[0] = '\0';
    uint64_t guest_addr = regs[7];
    uint8_t* host_ptr = GuestMemoryManager::GuestPhysToHost(guest_addr);
    if (host_ptr) {
        // copy up to 127 chars
        for (int i = 0; i < 127; i++) {
            element[i] = (char)host_ptr[i];
            if (element[i] == '\0') break;
            if (i == 126) element[127] = '\0';
        }
    }

    // build new path
    char new_path[V9FS_MAX_PATH];
    if (kstreq(element, "..")) {
        // go up one level
        kstrcpy(new_path, fids[src_fid].path, V9FS_MAX_PATH);
        int len = kstrlen(new_path);
        if (len > 1) {
            // find last '/'
            for (int i = len - 1; i > 0; i--) {
                if (new_path[i] == '/') {
                    new_path[i] = '\0';
                    break;
                }
            }
        }
    } else if (element[0] == '\0') {
        // empty walk = clone fid
        kstrcpy(new_path, fids[src_fid].path, V9FS_MAX_PATH);
    } else {
        kpathcat(new_path, V9FS_MAX_PATH, fids[src_fid].path, element);
    }

    // verify the path exists in kvfs
    bool exists = KVFS::Exists(new_path);
    if (!exists) return V9FS_ENOENT;

    // set up the new fid
    fids[new_fid].active = true;
    fids[new_fid].is_open = false;
    fids[new_fid].is_dir = KVFS::IsDir(new_path);
    fids[new_fid].mode = 0;
    fids[new_fid].offset = 0;
    fids[new_fid].dir_offset = 0;
    kstrcpy(fids[new_fid].path, new_path, V9FS_MAX_PATH);

    return V9FS_OK;
}

//  doopen - open a fid for i/o
//  edx(regs[2]) = fid
//  ebx(regs[3]) = mode (v9fs_oread, etc.)
//  returns: eax = 0 on success, ecx = iounit (max transfer size)

int V9FS::DoOpen(uint64_t* regs) {
    int fid = (int)(regs[2] & 0xFFFF);
    uint32_t mode = (uint32_t)regs[3];

    if (!ValidFid(fid)) return V9FS_EBADF;

    // truncate if requested
    if ((mode & V9FS_OTRUNC) && !fids[fid].is_dir) {
        KVFS::WriteFile(fids[fid].path, nullptr, 0);
    }

    fids[fid].is_open = true;
    fids[fid].mode = mode & 0x03; // keep only read/write bits
    fids[fid].offset = 0;
    fids[fid].dir_offset = 0;

    regs[1] = V9FS_MAX_IOSIZE; // ecx = iounit
    return V9FS_OK;
}

//  doread - read data from an open file fid
//  edx(regs[2]) = fid
//  ebx(regs[3]) = offset
//  esi(regs[6]) = count (max bytes to read)
//  edi(regs[7]) = guest phys addr of destination buffer
//  returns: eax = 0, ecx = bytes actually read

int V9FS::DoRead(uint64_t* regs) {
    int fid = (int)(regs[2] & 0xFFFF);
    uint32_t offset = (uint32_t)regs[3];
    uint32_t count = (uint32_t)regs[6];
    uint64_t guest_buf = regs[7];

    if (!ValidFid(fid) || !fids[fid].is_open) return V9FS_EBADF;
    if (fids[fid].is_dir) return V9FS_EISDIR;

    // clamp count
    if (count > V9FS_MAX_IOSIZE) count = V9FS_MAX_IOSIZE;

    // read from kvfs into scratch buffer
    int file_size = KVFS::GetFileSize(fids[fid].path);
    if (file_size < 0 || offset >= (uint32_t)file_size) {
        regs[1] = 0; // ecx = 0 bytes read (eof)
        return V9FS_OK;
    }

    uint32_t avail = (uint32_t)file_size - offset;
    if (count > avail) count = avail;
    if (count > sizeof(v9fs_iobuf)) count = sizeof(v9fs_iobuf);

    int rd = KVFS::ReadFile(fids[fid].path, v9fs_iobuf, (uint32_t)file_size);
    if (rd < 0) return V9FS_EIO;

    // write to guest memory through the region-clipped helper. GuestPhysToHost
    // only translates the start address; copying `count` bytes past it would
    // overrun into adjacent host heap when guest_buf sits near a region
    // boundary (a guest vm-escape). WriteGuestPhys re-translates every chunk and
    // fails the moment any byte leaves a single backing region. (satoru)
    if (!GuestMemoryManager::WriteGuestPhys(guest_buf, v9fs_iobuf + offset, count))
        return V9FS_EIO;

    fids[fid].offset = offset + count;
    regs[1] = count; // ecx = bytes read
    return V9FS_OK;
}

//  dowrite - write data to an open file fid
//  edx(regs[2]) = fid
//  ebx(regs[3]) = offset
//  esi(regs[6]) = count
//  edi(regs[7]) = guest phys addr of source buffer
//  returns: eax = 0, ecx = bytes written

int V9FS::DoWrite(uint64_t* regs) {
    int fid = (int)(regs[2] & 0xFFFF);
    uint32_t offset = (uint32_t)regs[3];
    uint32_t count = (uint32_t)regs[6];
    uint64_t guest_buf = regs[7];

    if (!ValidFid(fid) || !fids[fid].is_open) return V9FS_EBADF;
    if (fids[fid].is_dir) return V9FS_EISDIR;
    if (fids[fid].mode == V9FS_OREAD) return V9FS_EINVAL;

    if (count > V9FS_MAX_IOSIZE) count = V9FS_MAX_IOSIZE;

    // read existing file content, extend if necessary
    int existing_size = KVFS::GetFileSize(fids[fid].path);
    if (existing_size < 0) existing_size = 0;

    uint32_t new_size = offset + count;
    if (new_size < (uint32_t)existing_size) new_size = (uint32_t)existing_size;

    // use scratch buffer for read-modify-write
    if (new_size > sizeof(v9fs_iobuf)) {
        regs[1] = 0;
        return V9FS_ENOMEM;
    }

    // copy existing content
    for (uint32_t i = 0; i < new_size; i++) v9fs_iobuf[i] = 0;
    if (existing_size > 0) {
        KVFS::ReadFile(fids[fid].path, v9fs_iobuf, (uint32_t)existing_size);
    }
    // overlay new data pulled from guest via the region-clipped helper.
    // GuestPhysToHost only translates the start address; reading `count` bytes
    // past it would overrun adjacent host heap when guest_buf sits near a region
    // boundary (a guest vm-escape). ReadGuestPhys re-translates every chunk and
    // fails the moment any byte leaves a single backing region. (satoru)
    if (!GuestMemoryManager::ReadGuestPhys(guest_buf, v9fs_iobuf + offset, count))
        return V9FS_EIO;

    if (KVFS::WriteFile(fids[fid].path, v9fs_iobuf, new_size) < 0) {
        return V9FS_EIO;
    }

    fids[fid].offset = offset + count;
    regs[1] = count; // ecx = bytes written
    return V9FS_OK;
}

//  doclunk - release a fid (close)
//  edx(regs[2]) = fid

int V9FS::DoClunk(uint64_t* regs) {
    int fid = (int)(regs[2] & 0xFFFF);
    if (!ValidFid(fid)) return V9FS_EBADF;
    FreeFid(fid);
    return V9FS_OK;
}

//  dostat - get file/directory attributes
//  edx(regs[2]) = fid
//  edi(regs[7]) = guest phys addr of v9fs_stat buffer (output)
//  returns: eax = 0 on success

int V9FS::DoStat(uint64_t* regs) {
    int fid = (int)(regs[2] & 0xFFFF);
    uint64_t guest_buf = regs[7];

    if (!ValidFid(fid)) return V9FS_EBADF;

    V9FS_Stat st;
    st.type = fids[fid].is_dir ? V9FS_TYPE_DIR : V9FS_TYPE_FILE;
    st.mode = fids[fid].is_dir ? 0755 : 0644;
    st.mtime = 0;

    if (fids[fid].is_dir) {
        st.size = 0;
    } else {
        int sz = KVFS::GetFileSize(fids[fid].path);
        st.size = (sz >= 0) ? (uint32_t)sz : 0;
    }

    // extract basename
    const char* p = fids[fid].path;
    const char* last_slash = p;
    for (const char* c = p; *c; c++) {
        if (*c == '/') last_slash = c + 1;
    }
    kstrcpy(st.name, last_slash, sizeof(st.name));

    // write the stat struct to guest memory via the region-clipped helper.
    // GuestPhysToHost only translates the start; writing sizeof(stat) bytes past
    // it would overrun adjacent host heap when guest_buf sits near a region
    // boundary (a guest vm-escape). WriteGuestPhys rejects cross-region. (satoru)
    if (!GuestMemoryManager::WriteGuestPhys(guest_buf, &st, sizeof(V9FS_Stat)))
        return V9FS_EIO;

    return V9FS_OK;
}

//  doreaddir - read directory entries
//  edx(regs[2]) = fid (must be a directory)
//  ebx(regs[3]) = start index (0-based entry number)
//  esi(regs[6]) = max entries to return
//  edi(regs[7]) = guest phys addr of v9fs_stat[] array
//  returns: eax = 0, ecx = number of entries returned

int V9FS::DoReadDir(uint64_t* regs) {
    int fid = (int)(regs[2] & 0xFFFF);
    uint32_t start = (uint32_t)regs[3];
    uint32_t max_entries = (uint32_t)regs[6];
    uint64_t guest_buf = regs[7];

    if (!ValidFid(fid)) return V9FS_EBADF;
    if (!fids[fid].is_dir) return V9FS_ENOTDIR;

    // clamp max_entries so we don't overflow the guest buffer
    if (max_entries > 16) max_entries = 16;

    // list directory contents via kvfs
    KVFSNode* entries[32];
    int total = KVFS::Listdir(fids[fid].path, entries, 32);
    if (total < 0) total = 0;

    uint32_t returned = 0;
    for (int i = (int)start; i < total && returned < max_entries; i++) {
        KVFSNode* node = entries[i];
        if (!node) continue;

        V9FS_Stat st;
        st.type = node->is_dir() ? V9FS_TYPE_DIR : V9FS_TYPE_FILE;
        st.mode = node->perms.mode;
        st.mtime = 0;
        st.size = node->is_dir() ? 0 : node->size;
        kstrcpy(st.name, node->name, sizeof(st.name));

        // write each entry through the region-clipped helper. the old code
        // translated only the base of the guest array then copied
        // returned*sizeof(stat) bytes sequentially, overrunning adjacent host
        // heap once the array crossed a region boundary (a guest vm-escape).
        // WriteGuestPhys re-translates per chunk; if an entry would leave a
        // single backing region it fails and we stop returning further. (satoru)
        uint64_t entry_addr = guest_buf + (uint64_t)returned * sizeof(V9FS_Stat);
        if (!GuestMemoryManager::WriteGuestPhys(entry_addr, &st, sizeof(V9FS_Stat))) {
            if (returned == 0) return V9FS_EIO;
            break;
        }
        returned++;
    }

    regs[1] = returned; // ecx = entries returned
    return V9FS_OK;
}

//  docreate - create a file or directory under a fid
//  edx(regs[2]) = parent fid (must be directory)
//  ebx(regs[3]) = new fid to assign to created entry
//  esi(regs[6]) = flags (bit 0 = directory, bit 1 = truncate)
//  edi(regs[7]) = guest phys addr of null-terminated name
//  returns: eax = 0

int V9FS::DoCreate(uint64_t* regs) {
    int parent_fid = (int)(regs[2] & 0xFFFF);
    int new_fid    = (int)(regs[3] & 0xFFFF);
    uint32_t flags = (uint32_t)regs[6];
    uint64_t guest_name = regs[7];

    if (!ValidFid(parent_fid) || !fids[parent_fid].is_dir) return V9FS_ENOTDIR;
    if (new_fid < 0 || new_fid >= V9FS_MAX_FIDS) return V9FS_EINVAL;

    // read name from guest memory
    char name[128];
    name[0] = '\0';
    uint8_t* host_ptr = GuestMemoryManager::GuestPhysToHost(guest_name);
    if (host_ptr) {
        for (int i = 0; i < 127; i++) {
            name[i] = (char)host_ptr[i];
            if (name[i] == '\0') break;
            if (i == 126) name[127] = '\0';
        }
    }
    if (name[0] == '\0') return V9FS_EINVAL;

    // build full path
    char path[V9FS_MAX_PATH];
    kpathcat(path, V9FS_MAX_PATH, fids[parent_fid].path, name);

    bool is_dir = (flags & 1);
    if (is_dir) {
        if (KVFS::Mkdir(path) < 0) return V9FS_EIO;
    } else {
        if (KVFS::CreateFile(path) < 0) return V9FS_EIO;
    }

    // assign new fid
    fids[new_fid].active = true;
    fids[new_fid].is_open = true;
    fids[new_fid].is_dir = is_dir;
    fids[new_fid].mode = V9FS_ORDWR;
    fids[new_fid].offset = 0;
    fids[new_fid].dir_offset = 0;
    kstrcpy(fids[new_fid].path, path, V9FS_MAX_PATH);

    return V9FS_OK;
}

//  doremove - delete a file or directory
//  edx(regs[2]) = fid
//  returns: eax = 0 (fid is implicitly clunked)

int V9FS::DoRemove(uint64_t* regs) {
    int fid = (int)(regs[2] & 0xFFFF);
    if (!ValidFid(fid)) return V9FS_EBADF;

    int rc;
    if (fids[fid].is_dir)
        rc = KVFS::Rmdir(fids[fid].path);
    else
        rc = KVFS::Unlink(fids[fid].path);
    FreeFid(fid);
    return (rc >= 0) ? V9FS_OK : V9FS_EIO;
}
