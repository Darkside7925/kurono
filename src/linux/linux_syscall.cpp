//  kurono os  -  linux syscall abi translation layer  -  implementation
//  intercepts int 0x80 and translates linux syscalls to kurono operations

#include "linux_syscall.h"
#include "ext4.h"
#include "../fs/kvfs.h"
#include "../kernel/heap.h"
#include "../kernel/time.h"
#include "../drivers/serial.h"
#include "../security/supr.h"

LinuxProcess LinuxSyscall::procs[LINUX_MAX_PROCS];
int LinuxSyscall::current_proc = -1;

// console output capture ring buffer
char LinuxSyscall::console_buf[CONSOLE_BUF_SIZE];
int  LinuxSyscall::console_head = 0;
int  LinuxSyscall::console_tail = 0;

// stdin injection ring buffer
char LinuxSyscall::stdin_buf[STDIN_BUF_SIZE];
int  LinuxSyscall::stdin_head = 0;
int  LinuxSyscall::stdin_tail = 0;

static int ls_slen(const char* s) {
    int n = 0; while (s && s[n]) n++; return n;
}

static void ls_scpy(char* d, const char* s, int mx) {
    int i = 0;
    while (s && s[i] && i < mx - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static bool ls_seq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == 0 && *b == 0;
}

static bool ls_starts(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s != *prefix) return false;
        s++; prefix++;
    }
    return true;
}

static void ls_cat(char* d, const char* s, int mx) {
    int dl = ls_slen(d);
    int i = 0;
    while (s[i] && dl + i < mx - 1) { d[dl + i] = s[i]; i++; }
    d[dl + i] = 0;
}

//  init

void LinuxSyscall::Init() {
    for (int i = 0; i < LINUX_MAX_PROCS; i++) {
        procs[i].active = false;
        procs[i].pid = 0;
    }
    current_proc = -1;
    console_head = 0;
    console_tail = 0;
    stdin_head = 0;
    stdin_tail = 0;
    SerialLogger::Log("[LinuxSyscall] Initialized\r\n");
}

//  process management

int LinuxSyscall::CreateProcess(const char* name, uint32_t uid, uint32_t gid) {
    for (int i = 0; i < LINUX_MAX_PROCS; i++) {
        if (!procs[i].active) {
            LinuxProcess* p = &procs[i];
            memset(p, 0, sizeof(LinuxProcess));
            p->pid = (uint32_t)(i + 100);  // linux pids start at 100
            p->ppid = 1;
            p->uid = uid;
            p->gid = gid;
            p->euid = uid;
            p->egid = gid;
            ls_scpy(p->cwd, "/", sizeof(p->cwd));
            ls_scpy(p->name, name, sizeof(p->name));
            p->brk_current = LINUX_BRK_INITIAL;
            p->brk_max = LINUX_BRK_MAX;
            p->active = true;
            p->exit_code = -1;
            p->signal_mask = 0;
            p->pending_signals = 0;

            // initialize file descriptors
            for (int j = 0; j < LINUX_MAX_FDS; j++)
                p->fds[j].open = false;
            InitStdFds(p);

            SerialLogger::Log("[LinuxSyscall] Created process: ");
            SerialLogger::Log(name);
            SerialLogger::Log(" pid=");
            SerialLogger::LogDec((int)p->pid);
            SerialLogger::Log("\r\n");

            return i;
        }
    }
    return -1;
}

void LinuxSyscall::DestroyProcess(int pid_idx) {
    if (pid_idx < 0 || pid_idx >= LINUX_MAX_PROCS) return;
    LinuxProcess* p = &procs[pid_idx];
    // close all fds
    for (int i = 0; i < LINUX_MAX_FDS; i++) {
        if (p->fds[i].open) {
            if (p->fds[i].type == LFD_EXT4)
                Ext4::Close(p->fds[i].backend_fd);
            // kvfs fds closed similarly
            p->fds[i].open = false;
        }
    }
    p->active = false;
}

LinuxProcess* LinuxSyscall::GetProcess(int pid_idx) {
    if (pid_idx < 0 || pid_idx >= LINUX_MAX_PROCS) return nullptr;
    return procs[pid_idx].active ? &procs[pid_idx] : nullptr;
}

LinuxProcess* LinuxSyscall::Current() {
    if (current_proc < 0 || current_proc >= LINUX_MAX_PROCS) return nullptr;
    return procs[current_proc].active ? &procs[current_proc] : nullptr;
}

void LinuxSyscall::SetCurrent(int pid_idx) {
    current_proc = pid_idx;
}

int LinuxSyscall::ActiveProcessCount() {
    int n = 0;
    for (int i = 0; i < LINUX_MAX_PROCS; i++)
        if (procs[i].active) n++;
    return n;
}

int LinuxSyscall::AllocFd(LinuxProcess* p) {
    for (int i = 0; i < LINUX_MAX_FDS; i++) {
        if (!p->fds[i].open) return i;
    }
    return -1;
}

void LinuxSyscall::InitStdFds(LinuxProcess* p) {
    // stdin
    p->fds[0].type = LFD_CONSOLE;
    p->fds[0].backend_fd = 0;
    ls_scpy(p->fds[0].path, "/dev/stdin", sizeof(p->fds[0].path));
    p->fds[0].flags = L_O_RDONLY;
    p->fds[0].offset = 0;
    p->fds[0].open = true;

    // stdout
    p->fds[1].type = LFD_CONSOLE;
    p->fds[1].backend_fd = 1;
    ls_scpy(p->fds[1].path, "/dev/stdout", sizeof(p->fds[1].path));
    p->fds[1].flags = L_O_WRONLY;
    p->fds[1].offset = 0;
    p->fds[1].open = true;

    // stderr
    p->fds[2].type = LFD_CONSOLE;
    p->fds[2].backend_fd = 2;
    ls_scpy(p->fds[2].path, "/dev/stderr", sizeof(p->fds[2].path));
    p->fds[2].flags = L_O_WRONLY;
    p->fds[2].offset = 0;
    p->fds[2].open = true;
}

// linux paths get translated:
//   /home/user → kvfs /home/user  (shared)
//   /tmp       → kvfs /tmp        (shared)
//   /etc       → try ext4 first, fall back to kvfs
//   /proc      → virtual proc filesystem
//   /linux/*   → ext4: /*         (linux root)
//   everything else → try kvfs, then ext4

void LinuxSyscall::ResolvePath(const char* linux_path, char* kurono_path,
                                int max_len, LinuxProcess* p) {
    char abs[256];

    if (linux_path[0] != '/') {
        // relative path  -  prepend cwd
        ls_scpy(abs, p->cwd, sizeof(abs));
        if (abs[ls_slen(abs) - 1] != '/') ls_cat(abs, "/", sizeof(abs));
        ls_cat(abs, linux_path, sizeof(abs));
    } else {
        ls_scpy(abs, linux_path, sizeof(abs));
    }

    // shared paths that map directly to kvfs
    if (ls_starts(abs, "/home/") || ls_starts(abs, "/tmp/") ||
        ls_starts(abs, "/tmp") || ls_starts(abs, "/home")) {
        ls_scpy(kurono_path, abs, max_len);
        return;
    }

    // /proc  -  virtual filesystem
    if (ls_starts(abs, "/proc")) {
        ls_scpy(kurono_path, abs, max_len);
        return;
    }

    // /dev  -  device files
    if (ls_starts(abs, "/dev")) {
        ls_scpy(kurono_path, abs, max_len);
        return;
    }

    // default: pass through (unified namespace)
    ls_scpy(kurono_path, abs, max_len);
}

//  syscall dispatcher

int32_t LinuxSyscall::Dispatch(uint32_t eax, uint32_t ebx, uint32_t ecx,
                                uint32_t edx, uint32_t esi, uint32_t edi) {
    (void)esi; (void)edi;

    switch (eax) {
        case LSYS_EXIT:        return sys_exit(ebx);
        case LSYS_READ:        return sys_read((int)ebx, ecx, edx);
        case LSYS_WRITE:       return sys_write((int)ebx, ecx, edx);
        case LSYS_OPEN:        return sys_open(ebx, ecx, edx);
        case LSYS_CLOSE:       return sys_close((int)ebx);
        case LSYS_LSEEK:       return sys_lseek((int)ebx, (int32_t)ecx, edx);
        case LSYS_BRK:         return sys_brk(ebx);
        case LSYS_GETPID:      return sys_getpid();
        case LSYS_GETUID:      return sys_getuid();
        case LSYS_GETGID:      return sys_getgid();
        case LSYS_GETEUID:     return sys_geteuid();
        case LSYS_GETEGID:     return sys_getegid();
        case LSYS_GETPPID:     return sys_getppid();
        case LSYS_STAT:        return sys_stat(ebx, ecx);
        case LSYS_FSTAT:       return sys_fstat((int)ebx, ecx);
        case LSYS_UNAME:       return sys_uname(ebx);
        case LSYS_GETCWD:      return sys_getcwd(ebx, ecx);
        case LSYS_CHDIR:       return sys_chdir(ebx);
        case LSYS_MKDIR:       return sys_mkdir(ebx, ecx);
        case LSYS_RMDIR:       return sys_rmdir(ebx);
        case LSYS_UNLINK:      return sys_unlink(ebx);
        case LSYS_ACCESS:      return sys_access(ebx, ecx);
        case LSYS_DUP:         return sys_dup((int)ebx);
        case LSYS_DUP2:        return sys_dup2((int)ebx, (int)ecx);
        case LSYS_IOCTL:       return sys_ioctl((int)ebx, ecx, edx);
        case LSYS_WRITEV:      return sys_writev((int)ebx, ecx, edx);
        case LSYS_MMAP:        return sys_mmap(ebx, ecx, edx, esi, (int)edi, 0);
        case LSYS_MUNMAP:      return sys_munmap(ebx, ecx);
        case LSYS_NANOSLEEP:   return sys_nanosleep(ebx, ecx);
        case LSYS_GETDENTS64:  return sys_getdents64((int)ebx, ecx, edx);
        case LSYS_CLOCK_GETTIME: return sys_clock_gettime(ebx, ecx);
        case LSYS_SET_THREAD_AREA: return sys_set_thread_area(ebx);
        case LSYS_EXIT_GROUP:  return sys_exit_group(ebx);

        // stubs that return success
        case LSYS_SIGNAL:
        case LSYS_SIGACTION:
        case LSYS_MPROTECT:
        case LSYS_SYNC:
        case LSYS_SETSID:
            return 0;

        default:
            SerialLogger::Log("[LinuxSyscall] Unhandled syscall: ");
            SerialLogger::LogDec((int)eax);
            SerialLogger::Log("\r\n");
            return -38;  // enosys
    }
}

//  individual syscall implementations

int32_t LinuxSyscall::sys_exit(uint32_t code) {
    LinuxProcess* p = Current();
    if (p) {
        p->exit_code = (int)code;
        p->active = false;
        SerialLogger::Log("[LinuxSyscall] Process exited: ");
        SerialLogger::LogDec((int)code);
        SerialLogger::Log("\r\n");
    }
    return 0;
}

int32_t LinuxSyscall::sys_exit_group(uint32_t code) {
    return sys_exit(code);
}

int32_t LinuxSyscall::sys_read(int fd, uint32_t buf, uint32_t count) {
    LinuxProcess* p = Current();
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9; // ebadf

    LinuxFd* lfd = &p->fds[fd];
    uint8_t* dst = (uint8_t*)(uintptr_t)buf;

    switch (lfd->type) {
        case LFD_CONSOLE: {
            // stdin  -  read from injection buffer (non-blocking)
            uint32_t read = 0;
            while (read < count && stdin_head != stdin_tail) {
                dst[read++] = (uint8_t)stdin_buf[stdin_tail];
                stdin_tail = (stdin_tail + 1) % STDIN_BUF_SIZE;
            }
            return (int32_t)read;
        }

        case LFD_KVFS: {
            int r = KVFS::Read(lfd->backend_fd, dst, count);
            if (r > 0) lfd->offset += r;
            return r;
        }

        case LFD_EXT4: {
            int r = Ext4::Read(lfd->backend_fd, dst, count);
            if (r > 0) lfd->offset += r;
            return r;
        }

        case LFD_DEVNULL:
            return 0;

        case LFD_PROC: {
            // /proc virtual files
            char procdata[256];
            int plen = 0;
            if (ls_starts(lfd->path, "/proc/self/status")) {
                const char* s = "Name:\tkurono\nState:\tR (running)\nPid:\t";
                ls_scpy(procdata, s, sizeof(procdata));
                plen = ls_slen(procdata);
            }
            if ((uint32_t)plen > count) plen = (int)count;
            if (plen > 0) memcpy(dst, procdata, plen);
            return plen;
        }

        default:
            return -9;
    }
}

int32_t LinuxSyscall::sys_write(int fd, uint32_t buf, uint32_t count) {
    LinuxProcess* p = Current();
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;

    LinuxFd* lfd = &p->fds[fd];
    const uint8_t* src = (const uint8_t*)(uintptr_t)buf;

    switch (lfd->type) {
        case LFD_CONSOLE: {
            // stdout/stderr → capture to console ring buffer + serial
            for (uint32_t i = 0; i < count; i++) {
                char c = (char)src[i];
                // write to capture buffer
                int next = (console_head + 1) % CONSOLE_BUF_SIZE;
                if (next != console_tail) {
                    console_buf[console_head] = c;
                    console_head = next;
                }
                // also echo to serial for debugging
                char s[2] = { c, 0 };
                SerialLogger::Log(s);
            }
            return (int32_t)count;
        }

        case LFD_KVFS: {
            int r = KVFS::Write(lfd->backend_fd, src, count);
            if (r > 0) lfd->offset += r;
            return r;
        }

        case LFD_EXT4: {
            int r = Ext4::Write(lfd->backend_fd, src, count);
            if (r > 0) lfd->offset += r;
            return r;
        }

        case LFD_DEVNULL:
            return (int32_t)count;

        default:
            return -9;
    }
}

int32_t LinuxSyscall::sys_writev(int fd, uint32_t iov, uint32_t iovcnt) {
    LinuxIovec* vecs = (LinuxIovec*)(uintptr_t)iov;
    int32_t total = 0;
    for (uint32_t i = 0; i < iovcnt; i++) {
        int32_t r = sys_write(fd, vecs[i].iov_base, vecs[i].iov_len);
        if (r < 0) return r;
        total += r;
    }
    return total;
}

int32_t LinuxSyscall::sys_open(uint32_t pathname, uint32_t flags, uint32_t mode) {
    LinuxProcess* p = Current();
    if (!p) return -9;

    const char* path = (const char*)(uintptr_t)pathname;
    char resolved[256];
    ResolvePath(path, resolved, sizeof(resolved), p);

    int lfd_idx = AllocFd(p);
    if (lfd_idx < 0) return -24;  // emfile

    LinuxFd* lfd = &p->fds[lfd_idx];
    memset(lfd, 0, sizeof(LinuxFd));
    ls_scpy(lfd->path, resolved, sizeof(lfd->path));
    lfd->flags = flags;

    // /dev/null
    if (ls_seq(resolved, "/dev/null")) {
        lfd->type = LFD_DEVNULL;
        lfd->open = true;
        return lfd_idx;
    }

    // /proc files
    if (ls_starts(resolved, "/proc")) {
        lfd->type = LFD_PROC;
        lfd->open = true;
        return lfd_idx;
    }

    // try kvfs first
    uint8_t kvfs_flags = 0;
    if ((flags & 3) == L_O_RDONLY) kvfs_flags = 1;
    else if ((flags & 3) == L_O_WRONLY) kvfs_flags = 2;
    else if ((flags & 3) == L_O_RDWR) kvfs_flags = 3;
    if (flags & L_O_APPEND) kvfs_flags |= 4;

    if (flags & L_O_CREAT) {
        if (!KVFS::Exists(resolved)) {
            KVFS::CreateFile(resolved, (uint16_t)(mode & 0777));
        }
    }

    if (KVFS::Exists(resolved)) {
        int kfd = KVFS::Open(resolved, kvfs_flags);
        if (kfd >= 0) {
            lfd->type = LFD_KVFS;
            lfd->backend_fd = kfd;
            lfd->open = true;
            return lfd_idx;
        }
    }

    // try ext4
    if (Ext4::IsMounted()) {
        uint8_t e4flags = 0;
        if ((flags & 3) == L_O_RDONLY) e4flags = 1;
        else if ((flags & 3) == L_O_WRONLY) e4flags = 2;
        else e4flags = 3;

        if (flags & L_O_CREAT) {
            if (!Ext4::Exists(resolved)) {
                Ext4::CreateFile(resolved, (uint16_t)(mode & 0777));
            }
        }

        int e4fd = Ext4::Open(resolved, e4flags);
        if (e4fd >= 0) {
            lfd->type = LFD_EXT4;
            lfd->backend_fd = e4fd;
            lfd->open = true;
            return lfd_idx;
        }
    }

    return -2;  // enoent
}

int32_t LinuxSyscall::sys_close(int fd) {
    LinuxProcess* p = Current();
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;

    LinuxFd* lfd = &p->fds[fd];
    if (lfd->type == LFD_KVFS) KVFS::Close(lfd->backend_fd);
    else if (lfd->type == LFD_EXT4) Ext4::Close(lfd->backend_fd);
    lfd->open = false;
    return 0;
}

int32_t LinuxSyscall::sys_lseek(int fd, int32_t offset, uint32_t whence) {
    LinuxProcess* p = Current();
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;

    LinuxFd* lfd = &p->fds[fd];
    if (lfd->type == LFD_KVFS) {
        return KVFS::Seek(lfd->backend_fd, offset, (int)whence);
    }
    if (lfd->type == LFD_EXT4) {
        return Ext4::Seek(lfd->backend_fd, offset, (int)whence);
    }
    return -29;  // espipe
}

int32_t LinuxSyscall::sys_brk(uint32_t addr) {
    LinuxProcess* p = Current();
    if (!p) return -1;

    if (addr == 0) return (int32_t)p->brk_current;
    if (addr > p->brk_max) return (int32_t)p->brk_current;
    if (addr < LINUX_BRK_INITIAL) return (int32_t)p->brk_current;

    p->brk_current = addr;
    return (int32_t)addr;
}

int32_t LinuxSyscall::sys_getpid() {
    LinuxProcess* p = Current();
    return p ? (int32_t)p->pid : -1;
}

int32_t LinuxSyscall::sys_getppid() {
    LinuxProcess* p = Current();
    return p ? (int32_t)p->ppid : -1;
}

int32_t LinuxSyscall::sys_getuid() {
    LinuxProcess* p = Current();
    return p ? (int32_t)p->uid : 0;
}

int32_t LinuxSyscall::sys_getgid() {
    LinuxProcess* p = Current();
    return p ? (int32_t)p->gid : 0;
}

int32_t LinuxSyscall::sys_geteuid() {
    LinuxProcess* p = Current();
    return p ? (int32_t)p->euid : 0;
}

int32_t LinuxSyscall::sys_getegid() {
    LinuxProcess* p = Current();
    return p ? (int32_t)p->egid : 0;
}

int32_t LinuxSyscall::sys_stat(uint32_t pathname, uint32_t statbuf) {
    LinuxProcess* p = Current();
    if (!p) return -1;

    const char* path = (const char*)(uintptr_t)pathname;
    char resolved[256];
    ResolvePath(path, resolved, sizeof(resolved), p);

    LinuxStat* st = (LinuxStat*)(uintptr_t)statbuf;
    memset(st, 0, sizeof(LinuxStat));

    // try kvfs
    KVFSNode* node = KVFS::Resolve(resolved);
    if (node) {
        st->st_ino = (uint32_t)(uintptr_t)node;
        st->st_mode = node->perms.mode;
        if (node->is_dir()) st->st_mode |= EXT4_S_IFDIR;
        else st->st_mode |= EXT4_S_IFREG;
        st->st_nlink = 1;
        st->st_uid = node->perms.uid;
        st->st_gid = node->perms.gid;
        st->st_size = node->size;
        st->st_blksize = 4096;
        st->st_blocks = (node->size + 511) / 512;
        return 0;
    }

    // try ext4
    if (Ext4::IsMounted()) {
        Ext4Inode in;
        if (Ext4::Stat(resolved, &in) == 0) {
            st->st_mode = in.i_mode;
            st->st_nlink = in.i_links_count;
            st->st_uid = in.i_uid;
            st->st_gid = in.i_gid;
            st->st_size = in.i_size_lo;
            st->st_blksize = Ext4::BlockSize();
            st->st_blocks = in.i_blocks_lo;
            st->st_atime = in.i_atime;
            st->st_mtime = in.i_mtime;
            st->st_ctime = in.i_ctime;
            return 0;
        }
    }

    return -2;  // enoent
}

int32_t LinuxSyscall::sys_fstat(int fd, uint32_t statbuf) {
    LinuxProcess* p = Current();
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;

    // use the path stored in the fd
    LinuxStat* st = (LinuxStat*)(uintptr_t)statbuf;
    memset(st, 0, sizeof(LinuxStat));

    LinuxFd* lfd = &p->fds[fd];

    if (lfd->type == LFD_CONSOLE) {
        st->st_mode = 0020666;  // character device
        st->st_blksize = 1024;
        return 0;
    }

    if (lfd->type == LFD_DEVNULL) {
        st->st_mode = 0020666;
        st->st_blksize = 4096;
        return 0;
    }

    // delegate to stat with the stored path
    return sys_stat((uint32_t)(uintptr_t)lfd->path, statbuf);
}

int32_t LinuxSyscall::sys_uname(uint32_t buf) {
    LinuxUtsname* u = (LinuxUtsname*)(uintptr_t)buf;
    memset(u, 0, sizeof(LinuxUtsname));
    ls_scpy(u->sysname, "Linux", sizeof(u->sysname));
    ls_scpy(u->nodename, "kurono", sizeof(u->nodename));
    ls_scpy(u->release, "6.1.0-kurono", sizeof(u->release));
    ls_scpy(u->version, "#1 SMP Kurono OS Linux Subsystem", sizeof(u->version));
    ls_scpy(u->machine, "i686", sizeof(u->machine));
    ls_scpy(u->domainname, "(none)", sizeof(u->domainname));
    return 0;
}

int32_t LinuxSyscall::sys_getcwd(uint32_t buf, uint32_t size) {
    LinuxProcess* p = Current();
    if (!p) return -1;
    char* dst = (char*)(uintptr_t)buf;
    ls_scpy(dst, p->cwd, (int)size);
    return (int32_t)(uintptr_t)dst;
}

int32_t LinuxSyscall::sys_chdir(uint32_t pathname) {
    LinuxProcess* p = Current();
    if (!p) return -1;
    const char* path = (const char*)(uintptr_t)pathname;
    char resolved[256];
    ResolvePath(path, resolved, sizeof(resolved), p);

    // verify it's a directory
    if (KVFS::IsDir(resolved) || (Ext4::IsMounted() && Ext4::IsDir(resolved))) {
        ls_scpy(p->cwd, resolved, sizeof(p->cwd));
        return 0;
    }
    return -20;  // enotdir
}

int32_t LinuxSyscall::sys_mkdir(uint32_t pathname, uint32_t mode) {
    LinuxProcess* p = Current();
    if (!p) return -1;
    const char* path = (const char*)(uintptr_t)pathname;
    char resolved[256];
    ResolvePath(path, resolved, sizeof(resolved), p);

    int r = KVFS::Mkdir(resolved, (uint16_t)(mode & 0777));
    if (r == 0) return 0;

    if (Ext4::IsMounted()) {
        r = Ext4::Mkdir(resolved, (uint16_t)(mode & 0777));
        if (r == 0) return 0;
    }
    return -1;
}

int32_t LinuxSyscall::sys_rmdir(uint32_t pathname) {
    LinuxProcess* p = Current();
    if (!p) return -1;
    const char* path = (const char*)(uintptr_t)pathname;
    char resolved[256];
    ResolvePath(path, resolved, sizeof(resolved), p);

    if (KVFS::Rmdir(resolved) == 0) return 0;
    if (Ext4::IsMounted() && Ext4::Rmdir(resolved) == 0) return 0;
    return -2;
}

int32_t LinuxSyscall::sys_unlink(uint32_t pathname) {
    LinuxProcess* p = Current();
    if (!p) return -1;
    const char* path = (const char*)(uintptr_t)pathname;
    char resolved[256];
    ResolvePath(path, resolved, sizeof(resolved), p);

    if (KVFS::Unlink(resolved) == 0) return 0;
    if (Ext4::IsMounted() && Ext4::Unlink(resolved) == 0) return 0;
    return -2;
}

int32_t LinuxSyscall::sys_access(uint32_t pathname, uint32_t mode) {
    (void)mode;
    LinuxProcess* p = Current();
    if (!p) return -1;
    const char* path = (const char*)(uintptr_t)pathname;
    char resolved[256];
    ResolvePath(path, resolved, sizeof(resolved), p);

    if (KVFS::Exists(resolved)) return 0;
    if (Ext4::IsMounted() && Ext4::Exists(resolved)) return 0;
    return -2;
}

int32_t LinuxSyscall::sys_dup(int oldfd) {
    LinuxProcess* p = Current();
    if (!p || oldfd < 0 || oldfd >= LINUX_MAX_FDS || !p->fds[oldfd].open)
        return -9;

    int newfd = AllocFd(p);
    if (newfd < 0) return -24;

    memcpy(&p->fds[newfd], &p->fds[oldfd], sizeof(LinuxFd));
    return newfd;
}

int32_t LinuxSyscall::sys_dup2(int oldfd, int newfd) {
    LinuxProcess* p = Current();
    if (!p || oldfd < 0 || oldfd >= LINUX_MAX_FDS || !p->fds[oldfd].open)
        return -9;
    if (newfd < 0 || newfd >= LINUX_MAX_FDS) return -9;

    if (p->fds[newfd].open) {
        sys_close(newfd);
    }
    memcpy(&p->fds[newfd], &p->fds[oldfd], sizeof(LinuxFd));
    return newfd;
}

int32_t LinuxSyscall::sys_ioctl(int fd, uint32_t cmd, uint32_t arg) {
    (void)fd; (void)cmd; (void)arg;
    // terminal ioctls  -  return enotty for non-ttys
    LinuxProcess* p = Current();
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;
    if (p->fds[fd].type == LFD_CONSOLE) return 0;  // pretend success
    return -25;    // enotty
}

int32_t LinuxSyscall::sys_mmap(uint32_t addr, uint32_t length, uint32_t prot,
                                uint32_t flags, int fd, uint32_t offset) {
    (void)addr; (void)prot; (void)flags; (void)fd; (void)offset;
    // simplified: allocate from heap
    void* mem = KernelHeap::Alloc(length);
    if (!mem) return -12;  // enomem
    memset(mem, 0, length);
    return (int32_t)(uintptr_t)mem;
}

int32_t LinuxSyscall::sys_munmap(uint32_t addr, uint32_t length) {
    (void)addr; (void)length;
    // can't truly unmap from flat memory  -  no-op
    return 0;
}

int32_t LinuxSyscall::sys_nanosleep(uint32_t req, uint32_t rem) {
    (void)rem;
    struct { uint32_t tv_sec; uint32_t tv_nsec; }* ts =
        (decltype(ts))(uintptr_t)req;
    if (ts) {
        uint32_t ms = ts->tv_sec * 1000 + ts->tv_nsec / 1000000;
        if (ms > 5000) ms = 5000;  // cap at 5 seconds
        if (ms > 0) {
            uint32_t start = Time::GetTicks();
            while ((Time::GetTicks() - start) < ms) {
                asm volatile("hlt");
            }
        }
    }
    return 0;
}

int32_t LinuxSyscall::sys_getdents64(int fd, uint32_t dirp, uint32_t count) {
    LinuxProcess* p = Current();
    if (!p || fd < 0 || fd >= LINUX_MAX_FDS || !p->fds[fd].open) return -9;

    LinuxFd* lfd = &p->fds[fd];
    uint8_t* buf = (uint8_t*)(uintptr_t)dirp;
    uint32_t pos = 0;

    if (lfd->type == LFD_KVFS) {
        KVFSNode* node = KVFS::Resolve(lfd->path);
        if (!node || !node->is_dir()) return -20;

        int start = (int)lfd->offset;
        for (int i = start; i < node->child_count && pos + 280 < count; i++) {
            KVFSNode* child = node->children[i];
            if (!child) continue;

            LinuxDirent64* de = (LinuxDirent64*)(buf + pos);
            de->d_ino = (uint64_t)(uintptr_t)child;
            de->d_type = child->is_dir() ? 4 : 8;
            int nl = ls_slen(child->name);
            memcpy(de->d_name, child->name, nl);
            de->d_name[nl] = 0;
            uint16_t reclen = (uint16_t)(19 + nl + 1 + 7) & ~7;  // align 8
            de->d_reclen = reclen;
            de->d_off = (uint64_t)(i + 1);
            pos += reclen;
            lfd->offset = (uint64_t)(i + 1);
        }
    } else if (lfd->type == LFD_EXT4 && Ext4::IsMounted()) {
        Ext4DirInfo entries[32];
        int n = Ext4::ListDir(lfd->path, entries, 32);

        int start = (int)lfd->offset;
        for (int i = start; i < n && pos + 280 < count; i++) {
            LinuxDirent64* de = (LinuxDirent64*)(buf + pos);
            de->d_ino = entries[i].inode;
            de->d_type = entries[i].file_type;
            int nl = ls_slen(entries[i].name);
            memcpy(de->d_name, entries[i].name, nl);
            de->d_name[nl] = 0;
            uint16_t reclen = (uint16_t)(19 + nl + 1 + 7) & ~7;
            de->d_reclen = reclen;
            de->d_off = (uint64_t)(i + 1);
            pos += reclen;
            lfd->offset = (uint64_t)(i + 1);
        }
    }

    return (int32_t)pos;
}

int32_t LinuxSyscall::sys_clock_gettime(uint32_t clk_id, uint32_t tp) {
    (void)clk_id;
    struct { uint32_t tv_sec; uint32_t tv_nsec; }* ts =
        (decltype(ts))(uintptr_t)tp;
    uint32_t ticks = Time::GetTicks();
    ts->tv_sec = ticks / 1000;
    ts->tv_nsec = (ticks % 1000) * 1000000;
    return 0;
}

int32_t LinuxSyscall::sys_set_thread_area(uint32_t u_info) {
    (void)u_info;
    // tls setup  -  simplified: pretend success
    return 0;
}

//  console output capture  -  read syscall output back into the shell

bool LinuxSyscall::HasConsoleOutput() {
    return console_head != console_tail;
}

int LinuxSyscall::ReadConsoleOutput(char* buf, int max_len) {
    int read = 0;
    while (read < max_len && console_head != console_tail) {
        buf[read++] = console_buf[console_tail];
        console_tail = (console_tail + 1) % CONSOLE_BUF_SIZE;
    }
    return read;
}

void LinuxSyscall::ClearConsoleOutput() {
    console_head = 0;
    console_tail = 0;
}

//  stdin injection  -  push data from shell into linux process stdin

void LinuxSyscall::InjectStdin(const char* data, int len) {
    for (int i = 0; i < len; i++) {
        int next = (stdin_head + 1) % STDIN_BUF_SIZE;
        if (next == stdin_tail) break; // buffer full
        stdin_buf[stdin_head] = data[i];
        stdin_head = next;
    }
}

//  runprogram  -  execute a simulated linux program via syscalls
//  this creates a process context, runs the named builtin, captures
//  all console output, and returns it to the caller.

// built-in linux programs that exercise real syscalls
static void builtin_hello(LinuxSyscall* /*sys*/) {
    const char msg[] = "Hello from Linux syscall layer!\n";
    LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)msg,
                           sizeof(msg) - 1, 0, 0);
}

static void builtin_uname(LinuxSyscall* /*sys*/) {
    LinuxUtsname u;
    LinuxSyscall::Dispatch(LSYS_UNAME, (uint32_t)(uintptr_t)&u, 0, 0, 0, 0);
    char line[256];
    int p = 0;
    auto sa = [&](const char* s) { while (*s && p < 250) line[p++] = *s++; };
    sa(u.sysname); line[p++] = ' ';
    sa(u.nodename); line[p++] = ' ';
    sa(u.release); line[p++] = ' ';
    sa(u.version); line[p++] = ' ';
    sa(u.machine); line[p++] = '\n';
    line[p] = 0;
    LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)line, p, 0, 0);
}

static void builtin_getpid(LinuxSyscall* /*sys*/) {
    int32_t pid = LinuxSyscall::Dispatch(LSYS_GETPID, 0, 0, 0, 0, 0);
    char line[64];
    int p = 0;
    auto sa = [&](const char* s) { while (*s && p < 60) line[p++] = *s++; };
    sa("PID: ");
    // int to string
    if (pid <= 0) { line[p++] = '0'; } else {
        char tmp[12]; int ti = 0;
        int v = pid;
        while (v > 0) { tmp[ti++] = '0' + (v % 10); v /= 10; }
        while (ti > 0) line[p++] = tmp[--ti];
    }
    line[p++] = '\n'; line[p] = 0;
    LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)line, p, 0, 0);
}

static void builtin_pwd(LinuxSyscall* /*sys*/) {
    char cwd[256];
    LinuxSyscall::Dispatch(LSYS_GETCWD, (uint32_t)(uintptr_t)cwd, 256, 0, 0, 0);
    int len = 0; while (cwd[len]) len++;
    cwd[len++] = '\n'; cwd[len] = 0;
    LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)cwd, len, 0, 0);
}

static void builtin_ls(LinuxSyscall* /*sys*/) {
    // open current directory via syscall, read entries via getdents64
    const char* path = ".";
    int fd = LinuxSyscall::Dispatch(LSYS_OPEN, (uint32_t)(uintptr_t)path,
                                     L_O_RDONLY | L_O_DIRECTORY, 0, 0, 0);
    if (fd < 0) {
        const char* msg = "ls: cannot open directory\n";
        LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)msg, 26, 0, 0);
        return;
    }
    uint8_t buf[1024];
    int32_t n = LinuxSyscall::Dispatch(LSYS_GETDENTS64, (uint32_t)fd,
                                        (uint32_t)(uintptr_t)buf, 1024, 0, 0);
    if (n > 0) {
        uint32_t off = 0;
        while (off < (uint32_t)n) {
            LinuxDirent64* de = (LinuxDirent64*)(buf + off);
            int nl = 0; while (de->d_name[nl]) nl++;
            LinuxSyscall::Dispatch(LSYS_WRITE, 1,
                (uint32_t)(uintptr_t)de->d_name, nl, 0, 0);
            const char* nl_s = "  ";
            LinuxSyscall::Dispatch(LSYS_WRITE, 1,
                (uint32_t)(uintptr_t)nl_s, 2, 0, 0);
            off += de->d_reclen;
        }
        const char* nl_c = "\n";
        LinuxSyscall::Dispatch(LSYS_WRITE, 1,
            (uint32_t)(uintptr_t)nl_c, 1, 0, 0);
    }
    LinuxSyscall::Dispatch(LSYS_CLOSE, (uint32_t)fd, 0, 0, 0, 0);
}

static void builtin_cat(const char* path) {
    int fd = LinuxSyscall::Dispatch(LSYS_OPEN, (uint32_t)(uintptr_t)path,
                                     L_O_RDONLY, 0, 0, 0);
    if (fd < 0) {
        const char* msg = "cat: No such file\n";
        LinuxSyscall::Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)msg, 18, 0, 0);
        return;
    }
    uint8_t buf[1024];
    int32_t n;
    while ((n = LinuxSyscall::Dispatch(LSYS_READ, (uint32_t)fd,
                (uint32_t)(uintptr_t)buf, 1024, 0, 0)) > 0) {
        LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)buf, n, 0, 0);
    }
    LinuxSyscall::Dispatch(LSYS_CLOSE, (uint32_t)fd, 0, 0, 0, 0);
}

static void builtin_mkdir(const char* path) {
    int32_t r = LinuxSyscall::Dispatch(LSYS_MKDIR, (uint32_t)(uintptr_t)path,
                                        0755, 0, 0, 0);
    if (r < 0) {
        const char* msg = "mkdir: failed\n";
        LinuxSyscall::Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)msg, 14, 0, 0);
    }
}

static void builtin_write_file(const char* path, const char* content) {
    int fd = LinuxSyscall::Dispatch(LSYS_OPEN, (uint32_t)(uintptr_t)path,
                                     L_O_WRONLY | L_O_CREAT | L_O_TRUNC, 0644, 0, 0);
    if (fd < 0) {
        const char* msg = "write: cannot create file\n";
        LinuxSyscall::Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)msg, 26, 0, 0);
        return;
    }
    int len = 0; while (content[len]) len++;
    LinuxSyscall::Dispatch(LSYS_WRITE, (uint32_t)fd,
                           (uint32_t)(uintptr_t)content, len, 0, 0);
    LinuxSyscall::Dispatch(LSYS_CLOSE, (uint32_t)fd, 0, 0, 0, 0);
    const char* ok = "Written OK\n";
    LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)ok, 11, 0, 0);
}

static void builtin_sleep(int seconds) {
    struct { uint32_t tv_sec; uint32_t tv_nsec; } ts;
    ts.tv_sec = (uint32_t)seconds;
    ts.tv_nsec = 0;
    const char* msg = "Sleeping...\n";
    LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)msg, 12, 0, 0);
    LinuxSyscall::Dispatch(LSYS_NANOSLEEP, (uint32_t)(uintptr_t)&ts, 0, 0, 0, 0);
    const char* done = "Done.\n";
    LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)done, 6, 0, 0);
}

static void builtin_stat_file(const char* path) {
    LinuxStat st;
    int32_t r = LinuxSyscall::Dispatch(LSYS_STAT, (uint32_t)(uintptr_t)path,
                                        (uint32_t)(uintptr_t)&st, 0, 0, 0);
    if (r < 0) {
        const char* msg = "stat: not found\n";
        LinuxSyscall::Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)msg, 16, 0, 0);
        return;
    }
    char line[128];
    int p = 0;
    auto sa = [&](const char* s) { while (*s && p < 120) line[p++] = *s++; };
    auto si = [&](uint32_t v) {
        if (v == 0) { line[p++] = '0'; return; }
        char t[12]; int ti = 0;
        while (v > 0) { t[ti++] = '0' + (v % 10); v /= 10; }
        while (ti > 0) line[p++] = t[--ti];
    };
    sa("  Size: "); si(st.st_size);
    sa("  Mode: 0"); si((st.st_mode >> 6) & 7); si((st.st_mode >> 3) & 7); si(st.st_mode & 7);
    sa("  Blocks: "); si(st.st_blocks);
    line[p++] = '\n'; line[p] = 0;
    LinuxSyscall::Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)line, p, 0, 0);
}

int LinuxSyscall::RunProgram(const char* name, int argc, const char** argv,
                              char* output, int max_output) {
    // clear console buffer
    ClearConsoleOutput();

    // create a process
    int idx = CreateProcess(name, 0, 0);
    if (idx < 0) {
        const char* err = "linux-exec: cannot create process\n";
        int len = 0; while (err[len]) len++;
        for (int i = 0; i < len && i < max_output - 1; i++) output[i] = err[i];
        output[len < max_output ? len : max_output - 1] = 0;
        return len;
    }

    int saved = current_proc;
    SetCurrent(idx);

    // dispatch to built-in programs
    if (ls_seq(name, "hello") || ls_seq(name, "/bin/hello")) {
        builtin_hello(nullptr);
    } else if (ls_seq(name, "uname") || ls_seq(name, "/bin/uname")) {
        builtin_uname(nullptr);
    } else if (ls_seq(name, "getpid") || ls_seq(name, "/bin/getpid")) {
        builtin_getpid(nullptr);
    } else if (ls_seq(name, "pwd") || ls_seq(name, "/bin/pwd")) {
        builtin_pwd(nullptr);
    } else if (ls_seq(name, "ls") || ls_seq(name, "/bin/ls")) {
        builtin_ls(nullptr);
    } else if (ls_seq(name, "cat") || ls_seq(name, "/bin/cat")) {
        if (argc > 1) builtin_cat(argv[1]);
        else {
            const char* msg = "cat: missing operand\n";
            Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)msg, 21, 0, 0);
        }
    } else if (ls_seq(name, "mkdir") || ls_seq(name, "/bin/mkdir")) {
        if (argc > 1) builtin_mkdir(argv[1]);
        else {
            const char* msg = "mkdir: missing operand\n";
            Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)msg, 23, 0, 0);
        }
    } else if (ls_seq(name, "stat") || ls_seq(name, "/bin/stat")) {
        if (argc > 1) builtin_stat_file(argv[1]);
        else {
            const char* msg = "stat: missing operand\n";
            Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)msg, 22, 0, 0);
        }
    } else if (ls_seq(name, "sleep") || ls_seq(name, "/bin/sleep")) {
        int secs = 1;
        if (argc > 1) {
            secs = 0;
            const char* s = argv[1];
            while (*s >= '0' && *s <= '9') { secs = secs * 10 + (*s - '0'); s++; }
        }
        builtin_sleep(secs);
    } else if (ls_seq(name, "echo") || ls_seq(name, "/bin/echo")) {
        for (int i = 1; i < argc; i++) {
            int len = 0; while (argv[i][len]) len++;
            Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)argv[i], len, 0, 0);
            if (i < argc - 1) {
                const char* sp = " ";
                Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)sp, 1, 0, 0);
            }
        }
        const char* nl = "\n";
        Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)nl, 1, 0, 0);
    } else if (ls_seq(name, "write") || ls_seq(name, "/bin/write")) {
        if (argc > 2) builtin_write_file(argv[1], argv[2]);
        else {
            const char* msg = "write: usage: write <file> <content>\n";
            Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)msg, 37, 0, 0);
        }
    } else if (ls_seq(name, "id") || ls_seq(name, "/bin/id")) {
        char line[128];
        int p = 0;
        auto sa = [&](const char* s) { while (*s && p < 120) line[p++] = *s++; };
        auto si = [&](int32_t v) {
            if (v == 0) { line[p++] = '0'; return; }
            char t[12]; int ti = 0;
            while (v > 0) { t[ti++] = '0' + (v % 10); v /= 10; }
            while (ti > 0) line[p++] = t[--ti];
        };
        sa("uid="); si(Dispatch(LSYS_GETUID, 0, 0, 0, 0, 0));
        sa("(root) gid="); si(Dispatch(LSYS_GETGID, 0, 0, 0, 0, 0));
        sa("(root)\n");
        line[p] = 0;
        Dispatch(LSYS_WRITE, 1, (uint32_t)(uintptr_t)line, p, 0, 0);
    } else {
        // unknown program
        const char* pre = "linux-exec: unknown program: ";
        Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)pre, 29, 0, 0);
        int nl = 0; while (name[nl]) nl++;
        Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)name, nl, 0, 0);
        const char* suf = "\nAvailable: hello uname getpid pwd ls cat mkdir stat sleep echo write id\n";
        int sl = 0; while (suf[sl]) sl++;
        Dispatch(LSYS_WRITE, 2, (uint32_t)(uintptr_t)suf, sl, 0, 0);
    }

    // process exits
    Dispatch(LSYS_EXIT, 0, 0, 0, 0, 0);

    // read captured output
    int out_len = ReadConsoleOutput(output, max_output - 1);
    output[out_len] = 0;

    // cleanup
    DestroyProcess(idx);
    SetCurrent(saved);

    return out_len;
}