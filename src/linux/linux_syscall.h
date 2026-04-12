#pragma once
//  kurono os  -  linux syscall abi translation layer
//  translates linux syscalls into kurono kernel operations, enabling
//  unmodified linux elf binaries to run inside kurono os.

#include "../kernel/types.h"

#define LSYS_EXIT            1
#define LSYS_FORK            2
#define LSYS_READ            3
#define LSYS_WRITE           4
#define LSYS_OPEN            5
#define LSYS_CLOSE           6
#define LSYS_WAITPID         7
#define LSYS_CREAT           8
#define LSYS_LINK            9
#define LSYS_UNLINK         10
#define LSYS_EXECVE         11
#define LSYS_CHDIR          12
#define LSYS_TIME           13
#define LSYS_MKNOD          14
#define LSYS_CHMOD          15
#define LSYS_LSEEK          19
#define LSYS_GETPID         20
#define LSYS_MOUNT          21
#define LSYS_UMOUNT         22
#define LSYS_SETUID         23
#define LSYS_GETUID         24
#define LSYS_ACCESS         33
#define LSYS_SYNC           36
#define LSYS_KILL           37
#define LSYS_RENAME         38
#define LSYS_MKDIR          39
#define LSYS_RMDIR          40
#define LSYS_DUP            41
#define LSYS_PIPE           42
#define LSYS_BRK            45
#define LSYS_SETGID         46
#define LSYS_GETGID         47
#define LSYS_SIGNAL         48
#define LSYS_GETEUID        49
#define LSYS_GETEGID        50
#define LSYS_IOCTL          54
#define LSYS_FCNTL          55
#define LSYS_DUP2           63
#define LSYS_GETPPID        64
#define LSYS_SETSID         66
#define LSYS_SIGACTION       67
#define LSYS_SETREUID       70
#define LSYS_SETREGID       71
#define LSYS_MMAP           90
#define LSYS_MUNMAP         91
#define LSYS_FCHMOD         94
#define LSYS_FCHOWN         95
#define LSYS_STAT           106
#define LSYS_FSTAT          108
#define LSYS_UNAME          122
#define LSYS_MPROTECT       125
#define LSYS_WRITEV         146
#define LSYS_NANOSLEEP      162
#define LSYS_GETCWD         183
#define LSYS_GETDENTS       141
#define LSYS_GETDENTS64     220
#define LSYS_READLINK       85
#define LSYS_STATFS         99
#define LSYS_FSTATFS       100
#define LSYS_CLOCK_GETTIME 265
#define LSYS_OPENAT        295
#define LSYS_MKDIRAT       296
#define LSYS_FSTATAT       300
#define LSYS_UNLINKAT      301
#define LSYS_RENAMEAT      302
#define LSYS_SET_THREAD_AREA 243
#define LSYS_EXIT_GROUP    252

#define L_O_RDONLY  0x0000
#define L_O_WRONLY  0x0001
#define L_O_RDWR    0x0002
#define L_O_CREAT   0x0040
#define L_O_EXCL    0x0080
#define L_O_TRUNC   0x0200
#define L_O_APPEND  0x0400
#define L_O_NONBLOCK 0x0800
#define L_O_DIRECTORY 0x10000

struct LinuxStat {
    uint32_t st_dev;
    uint32_t __pad1;
    uint32_t st_ino;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint16_t st_uid;
    uint16_t st_gid;
    uint32_t st_rdev;
    uint32_t __pad2;
    uint32_t st_size;
    uint32_t st_blksize;
    uint32_t st_blocks;
    uint32_t st_atime;
    uint32_t st_atime_nsec;
    uint32_t st_mtime;
    uint32_t st_mtime_nsec;
    uint32_t st_ctime;
    uint32_t st_ctime_nsec;
    uint32_t __unused4;
    uint32_t __unused5;
} __attribute__((packed));

struct LinuxDirent64 {
    uint64_t d_ino;
    uint64_t d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
} __attribute__((packed));

struct LinuxUtsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} __attribute__((packed));

struct LinuxIovec {
    uint32_t iov_base;
    uint32_t iov_len;
} __attribute__((packed));

#define LINUX_MAX_FDS      64
#define LINUX_FD_STDIN      0
#define LINUX_FD_STDOUT     1
#define LINUX_FD_STDERR     2
#define LINUX_BRK_INITIAL  0x08100000
#define LINUX_BRK_MAX      0x0C000000

// file descriptor types
enum LinuxFdType {
    LFD_NONE = 0,
    LFD_KVFS,        // backed by kurono kvfs
    LFD_EXT4,        // backed by ext4 driver
    LFD_CONSOLE,     // stdin/stdout/stderr → terminal
    LFD_PIPE,
    LFD_DEVNULL,
    LFD_PROC,        // /proc virtual files
};

struct LinuxFd {
    LinuxFdType type;
    int backend_fd;     // underlying kvfs or ext4 fd
    char path[256];     // for stat purposes
    uint32_t flags;
    uint64_t offset;
    bool     open;
};

struct LinuxProcess {
    uint32_t pid;
    uint32_t ppid;
    uint32_t uid, gid;
    uint32_t euid, egid;
    char     cwd[256];
    char     name[64];
    LinuxFd  fds[LINUX_MAX_FDS];
    uint32_t brk_current;
    uint32_t brk_max;
    bool     active;
    int      exit_code;

    // signal state (simplified)
    uint32_t signal_mask;
    uint32_t pending_signals;
};

//  linuxsyscall  -  the syscall handler

#define LINUX_MAX_PROCS  16

class LinuxSyscall {
public:
    static void Init();

    // create a new linux process context
    static int  CreateProcess(const char* name, uint32_t uid, uint32_t gid);
    static void DestroyProcess(int pid_idx);
    static LinuxProcess* GetProcess(int pid_idx);
    static LinuxProcess* Current();
    static void SetCurrent(int pid_idx);

    // the main syscall dispatcher
    // called when int 0x80 fires from a linux elf binary
    // eax=syscall#, ebx/ecx/edx/esi/edi = args
    static int32_t Dispatch(uint32_t eax, uint32_t ebx, uint32_t ecx,
                            uint32_t edx, uint32_t esi, uint32_t edi);

    // stats
    static int  ActiveProcessCount();

    static bool HasConsoleOutput();
    static int  ReadConsoleOutput(char* buf, int max_len);
    static void ClearConsoleOutput();

    static void InjectStdin(const char* data, int len);

    // creates a process, runs the named builtin, and returns output
    static int  RunProgram(const char* name, int argc, const char** argv,
                           char* output, int max_output);

private:
    static LinuxProcess procs[LINUX_MAX_PROCS];
    static int current_proc;

    // console output ring buffer (syscall writes land here)
    static constexpr int CONSOLE_BUF_SIZE = 8192;
    static char console_buf[CONSOLE_BUF_SIZE];
    static int  console_head;
    static int  console_tail;

    // stdin injection buffer
    static constexpr int STDIN_BUF_SIZE = 1024;
    static char stdin_buf[STDIN_BUF_SIZE];
    static int  stdin_head;
    static int  stdin_tail;

    // individual syscall handlers
    static int32_t sys_exit(uint32_t code);
    static int32_t sys_read(int fd, uint32_t buf, uint32_t count);
    static int32_t sys_write(int fd, uint32_t buf, uint32_t count);
    static int32_t sys_open(uint32_t pathname, uint32_t flags, uint32_t mode);
    static int32_t sys_close(int fd);
    static int32_t sys_lseek(int fd, int32_t offset, uint32_t whence);
    static int32_t sys_brk(uint32_t addr);
    static int32_t sys_getpid();
    static int32_t sys_getuid();
    static int32_t sys_getgid();
    static int32_t sys_geteuid();
    static int32_t sys_getegid();
    static int32_t sys_getppid();
    static int32_t sys_stat(uint32_t pathname, uint32_t statbuf);
    static int32_t sys_fstat(int fd, uint32_t statbuf);
    static int32_t sys_uname(uint32_t buf);
    static int32_t sys_getcwd(uint32_t buf, uint32_t size);
    static int32_t sys_chdir(uint32_t pathname);
    static int32_t sys_mkdir(uint32_t pathname, uint32_t mode);
    static int32_t sys_rmdir(uint32_t pathname);
    static int32_t sys_unlink(uint32_t pathname);
    static int32_t sys_access(uint32_t pathname, uint32_t mode);
    static int32_t sys_dup(int oldfd);
    static int32_t sys_dup2(int oldfd, int newfd);
    static int32_t sys_ioctl(int fd, uint32_t cmd, uint32_t arg);
    static int32_t sys_writev(int fd, uint32_t iov, uint32_t iovcnt);
    static int32_t sys_mmap(uint32_t addr, uint32_t length, uint32_t prot,
                            uint32_t flags, int fd, uint32_t offset);
    static int32_t sys_munmap(uint32_t addr, uint32_t length);
    static int32_t sys_nanosleep(uint32_t req, uint32_t rem);
    static int32_t sys_getdents64(int fd, uint32_t dirp, uint32_t count);
    static int32_t sys_clock_gettime(uint32_t clk_id, uint32_t tp);
    static int32_t sys_set_thread_area(uint32_t u_info);
    static int32_t sys_exit_group(uint32_t code);

    // fd helpers
    static int  AllocFd(LinuxProcess* p);
    static void InitStdFds(LinuxProcess* p);

    // path resolution: translates linux paths to kurono paths
    static void ResolvePath(const char* linux_path, char* kurono_path,
                            int max_len, LinuxProcess* p);
};
