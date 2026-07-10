#pragma once
//  kurono os - linux syscall abi translation layer
//  translates linux syscalls into kurono kernel operations, enabling
//  unmodified linux elf binaries to run inside kurono os.

#include "../kernel/types.h"
#include "../proc/smp.h"        // SMP_MAX_CPUS - current_proc is per-cpu (smp 3d) (satoru)

struct InterruptFrame;
struct Process;

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
#define LSYS_GETPGRP        65
#define LSYS_SETPGID        57
#define LSYS_GETPGID       132
#define LSYS_GETSID        147
#define LSYS_PRCTL         172
#define LSYS_GETTID        224
#define LSYS_TGKILL        234
#define LSYS_SET_TID_ADDRESS 258
#define LSYS_SYSINFO       116
#define LSYS_GETRLIMIT      76
#define LSYS_SETRLIMIT      75
#define LSYS_PRLIMIT64     340
#define LSYS_PERSONALITY   136
#define LSYS_CAPGET        184
#define LSYS_CAPSET        185
#define LSYS_DUP3          330
#define LSYS_PIPE2         331
#define LSYS_PREAD64       180
#define LSYS_PWRITE64      181
#define LSYS_FTRUNCATE      93
#define LSYS_FSYNC         118
#define LSYS_FDATASYNC     148
#define LSYS_MADVISE       219
#define LSYS_MSYNC         144
#define LSYS_REBOOT         88
#define LSYS_SYSLOG        103
#define LSYS_CLOCK_GETRES  266
#define LSYS_FUTEX         240
#define LSYS_CLONE         120
#define LSYS_RT_SIGACTION    513   // i386=174, x64=13: dispatcher uses internal ID
#define LSYS_RT_SIGPROCMASK  514   // i386=175, x64=14
#define LSYS_RT_SIGRETURN    515   // i386=173, x64=15
#define LSYS_RT_SIGSUSPEND  130
#define LSYS_RT_SIGPENDING  127
#define LSYS_POLL           168
#define LSYS_PPOLL          271
#define LSYS_SELECT         142
#define LSYS_PSELECT6       270
#define LSYS_EPOLL_CREATE1  329
#define LSYS_EPOLL_CTL      255
#define LSYS_EPOLL_WAIT     256
#define LSYS_EVENTFD2       323
#define LSYS_SIGNALFD4      327
#define LSYS_TIMERFD_CREATE 322
#define LSYS_TIMERFD_SETTIME 325
#define LSYS_TIMERFD_GETTIME 326
#define LSYS_INOTIFY_INIT1  332
#define LSYS_INOTIFY_ADD_WATCH 254
#define LSYS_INOTIFY_RM_WATCH  253
#define LSYS_SENDFILE       187
#define LSYS_SPLICE         313
#define LSYS_TEE            315
#define LSYS_MEMFD_CREATE   356
#define LSYS_PIDFD_OPEN     434
#define LSYS_PIDFD_SEND_SIGNAL 424
#define LSYS_EXECVEAT       358
#define LSYS_FALLOCATE      285
#define LSYS_RENAMEAT2      316
#define LSYS_GETRANDOM      355
#define LSYS_BPF            321
#define LSYS_PERF_EVENT_OPEN 298
#define LSYS_UNSHARE        272
#define LSYS_SETNS          308
#define LSYS_KEYCTL         250
#define LSYS_KCMP           312
#define LSYS_USERFAULTFD    516
#define LSYS_PIVOT_ROOT     519
#define LSYS_CHROOT          61
#define LSYS_SETHOSTNAME     74
#define LSYS_SETDOMAINNAME  121
#define LSYS_INIT_MODULE    517
#define LSYS_DELETE_MODULE  518
#define LSYS_KEXEC_LOAD     246
#define LSYS_FANOTIFY_INIT  367
#define LSYS_FANOTIFY_MARK  368
#define LSYS_FINIT_MODULE   373
#define LSYS_NAME_TO_HANDLE_AT 303
#define LSYS_OPEN_BY_HANDLE_AT 304

// Firefox / modern userspace extras (Kurono-internal IDs to avoid
// collisions with the i386-style numbers used elsewhere in this header).
#define LSYS_STATX             542
#define LSYS_COPY_FILE_RANGE   543
#define LSYS_CLOSE_RANGE       544
#define LSYS_CLONE3            545
#define LSYS_PIDFD_GETFD       546
#define LSYS_LANDLOCK_CREATE   547
#define LSYS_LANDLOCK_ADD      548
#define LSYS_LANDLOCK_RESTRICT 549
#define LSYS_IO_URING_SETUP    550
#define LSYS_IO_URING_ENTER    551
#define LSYS_IO_URING_REGISTER 552
#define LSYS_SECCOMP           553
#define LSYS_PROCESS_VM_READV  554
#define LSYS_PROCESS_VM_WRITEV 555
#define LSYS_MEMBARRIER        556
#define LSYS_RSEQ              557

// ── x86_64 ABI completeness build-out - internal IDs (satoru) ──────────────
// fresh 600+ block so these never collide with the i386-style numbers used as
// dispatch keys above.  each is routed from the real x86_64 number in
// linux_syscall_x64.cpp (kNrMap) and handled in Dispatch. (satoru)
#define LSYS_VMSPLICE          600
#define LSYS_OPENAT2           601
#define LSYS_SYNC_FILE_RANGE   602
#define LSYS_POSIX_FADVISE     603
#define LSYS_READAHEAD         604
#define LSYS_EPOLL_PWAIT2      605
#define LSYS_PREADV            606
#define LSYS_PWRITEV           607
#define LSYS_PREADV2           608
#define LSYS_PWRITEV2          609
#define LSYS_SCHED_SETAFFINITY 610
#define LSYS_SCHED_GETAFFINITY 611
#define LSYS_SCHED_SETATTR     612
#define LSYS_SCHED_GETATTR     613
#define LSYS_SCHED_SETPARAM    614
#define LSYS_SCHED_GETPARAM    615
#define LSYS_SCHED_SETSCHEDULER 616
#define LSYS_SCHED_GETSCHEDULER 617
#define LSYS_SCHED_GET_PRIORITY_MAX 618
#define LSYS_SCHED_GET_PRIORITY_MIN 619
#define LSYS_SCHED_RR_GET_INTERVAL 620
#define LSYS_SETPRIORITY       621
#define LSYS_GETPRIORITY       622
#define LSYS_IOPRIO_SET        623
#define LSYS_IOPRIO_GET        624
#define LSYS_SCHED_YIELD       625
#define LSYS_LINKAT            626
#define LSYS_SYMLINKAT         627
#define LSYS_FCHMODAT          628
#define LSYS_FACCESSAT         629
#define LSYS_FACCESSAT2        630
#define LSYS_UTIMENSAT         631
#define LSYS_FUTIMESAT         632
#define LSYS_UMOUNT2           633
#define LSYS_SWAPON            634
#define LSYS_SWAPOFF           635
#define LSYS_QUOTACTL          636
#define LSYS_SETXATTR          637
#define LSYS_LSETXATTR         638
#define LSYS_FSETXATTR         639
#define LSYS_GETXATTR          640
#define LSYS_LGETXATTR         641
#define LSYS_FGETXATTR         642
#define LSYS_LISTXATTR         643
#define LSYS_LLISTXATTR        644
#define LSYS_FLISTXATTR        645
#define LSYS_REMOVEXATTR       646
#define LSYS_LREMOVEXATTR      647
#define LSYS_FREMOVEXATTR      648
#define LSYS_SENDMMSG          649
#define LSYS_RECVMMSG          650
#define LSYS_MREMAP            651
#define LSYS_MLOCK             652
#define LSYS_MUNLOCK           653
#define LSYS_MLOCKALL          654
#define LSYS_MUNLOCKALL        655
#define LSYS_MINCORE           656
#define LSYS_MEMFD_SECRET      657
#define LSYS_MBIND             658
#define LSYS_SET_MEMPOLICY     659
#define LSYS_GET_MEMPOLICY     660
#define LSYS_MIGRATE_PAGES     661
#define LSYS_MOVE_PAGES        662
#define LSYS_RT_SIGTIMEDWAIT   663
#define LSYS_RT_SIGQUEUEINFO   664
#define LSYS_RT_TGSIGQUEUEINFO 665
#define LSYS_KILL_             666
#define LSYS_TKILL             667
#define LSYS_PAUSE             668
#define LSYS_CLOCK_SETTIME     669
#define LSYS_GETITIMER         670
#define LSYS_SETITIMER         671
#define LSYS_ALARM             672
#define LSYS_TIMES             673
#define LSYS_ADJTIMEX          674
#define LSYS_CLOCK_ADJTIME     675
#define LSYS_SETUID_           676
#define LSYS_SETGID_           677
#define LSYS_SETREUID_         678
#define LSYS_SETREGID_         679
#define LSYS_SETRESUID         680
#define LSYS_SETRESGID         681
#define LSYS_GETRESUID         682
#define LSYS_GETRESGID         683
#define LSYS_SETFSUID          684
#define LSYS_SETFSGID          685
#define LSYS_GETGROUPS         686
#define LSYS_SETGROUPS         687
#define LSYS_SYSFS             688
#define LSYS_ACCT              689
#define LSYS_USTAT             690
#define LSYS_STATFS_           691
#define LSYS_FSTATFS_          692
#define LSYS_PTRACE            693
#define LSYS_KEXEC_FILE_LOAD   694
#define LSYS_LOOKUP_DCOOKIE    695
#define LSYS_ADD_KEY           696
#define LSYS_REQUEST_KEY       697
#define LSYS_FANOTIFY_INIT_    698
#define LSYS_FANOTIFY_MARK_    699
#define LSYS_PIDFD_OPEN_       700
#define LSYS_PIDFD_SEND_SIG    701
#define LSYS_GETPRIORITY_DONE  702

// AF_UNIX socket family - internal IDs.  Dispatch also accepts the
// Linux x86_64 numbers (41..55) where they don't collide.
#define LSYS_SOCKET            560
#define LSYS_BIND              561
#define LSYS_LISTEN            562
#define LSYS_ACCEPT            563
#define LSYS_CONNECT           564
#define LSYS_SENDTO            565
#define LSYS_RECVFROM          566
#define LSYS_SENDMSG           567
#define LSYS_RECVMSG           568
#define LSYS_SHUTDOWN          569
#define LSYS_SETSOCKOPT        570
#define LSYS_GETSOCKOPT        571
#define LSYS_GETSOCKNAME       572
#define LSYS_GETPEERNAME       573
#define LSYS_SOCKETPAIR        574
#define LSYS_ACCEPT4           575
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

// x86_64 `struct stat` - the layout an unmodified amd64 musl/glibc binary
// expects from stat/fstat/lstat/newfstatat.  This is NOT the i386 layout above:
// fields are 64-bit, st_mode/uid/gid are 32-bit, and there is a __pad0 word
// before st_rdev.  Verified field-for-field against
// sources/linux-ref/arch/x86/include/uapi/asm/stat.h (the !__i386__ branch).
// Total size is exactly 144 bytes. (satoru)
struct LinuxStat64 {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime;
    uint64_t st_atime_nsec;
    uint64_t st_mtime;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime;
    uint64_t st_ctime_nsec;
    int64_t  __unused[3];
} __attribute__((packed));

static_assert(sizeof(LinuxStat64) == 144, "x86_64 struct stat must be 144 bytes");

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
    uint64_t iov_base;   // 64-bit user pointer (x86_64 iovec layout) (satoru)
    uint64_t iov_len;
} __attribute__((packed));

// statx() result block - Linux 5.x layout.
struct LinuxStatxTimestamp {
    int64_t  tv_sec;
    uint32_t tv_nsec;
    uint32_t __reserved;
} __attribute__((packed));

struct LinuxStatx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare0[1];
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    LinuxStatxTimestamp stx_atime;
    LinuxStatxTimestamp stx_btime;
    LinuxStatxTimestamp stx_ctime;
    LinuxStatxTimestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint64_t __spare2;
    uint64_t __spare3[12];
} __attribute__((packed));

// memfd_create flags + F_ADD_SEALS / F_GET_SEALS for fcntl.
#define LMFD_CLOEXEC      0x0001
#define LMFD_ALLOW_SEALING 0x0002
#define LMFD_HUGETLB      0x0004
#define LF_ADD_SEALS      1033
#define LF_GET_SEALS      1034
#define LF_SEAL_SEAL      0x0001
#define LF_SEAL_SHRINK    0x0002
#define LF_SEAL_GROW      0x0004
#define LF_SEAL_WRITE     0x0008
#define LF_SEAL_FUTURE_WRITE 0x0010

#define LINUX_MAX_FDS      256
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
    LFD_SOCKET,      // AF_UNIX socket - backend_fd = UnixSocket sd
    LFD_MEMFD,       // memfd_create - backend_fd = kvfs anon file fd
    LFD_URING,       // io_uring instance - backend_fd = ring id (stub)
    LFD_LANDLOCK,    // landlock ruleset (stub)
    LFD_FANOTIFY,    // fanotify group (stub)
    LFD_EPOLL,       // epoll instance - backend_fd = epoll table slot (satoru)
    LFD_EVENTFD,     // eventfd - backend_fd = eventfd table slot (satoru)
    LFD_TIMERFD,     // timerfd - backend_fd = timerfd table slot (satoru)
    LFD_SIGNALFD,    // signalfd - harmless stub, never fires (satoru)
    LFD_INET,        // AF_INET tcp/udp - backend_fd = LinuxNetBridge slot (satoru)
};

// memfd seal bits live alongside the LinuxFd so fcntl can interrogate.
struct LinuxMemfdSeal { uint32_t seals; };

struct LinuxFd {
    LinuxFdType type;
    int backend_fd;     // underlying kvfs or ext4 fd
    char path[256];     // for stat purposes
    uint32_t flags;
    uint64_t offset;
    bool     open;
    uint32_t seals;     // memfd seal bits
};

struct LinuxProcess {
    uint32_t pid;
    uint32_t ppid;
    uint32_t uid, gid;
    uint32_t euid, egid;
    char     cwd[256];
    char     name[64];
    // heap-allocated fd table. CLONE_FILES threads share ONE table (the leader's)
    // so a multithreaded app's threads see each other's fds -- required by e.g.
    // firefox's WaylandProxy + GTK threads. allocated in CreateProcess. (satoru)
    LinuxFd* fds;
    uint64_t brk_base;     // heap addresses are 64-bit so a high pie heap fits (satoru)
    uint64_t brk_current;
    uint64_t brk_max;
    bool     active;
    bool     exited;
    int      exit_code;
    Process* task;

    // poll/ppoll cooperative-block state: a user thread that blocks in poll
    // deschedules to a sibling and re-runs the syscall on wake; poll_blocking
    // marks that an in-progress block owns poll_deadline_ms (absolute ms; ~UINT64_MAX
    // for an infinite -1 timeout) so the deadline survives the re-runs. (satoru)
    bool     poll_blocking;
    uint64_t poll_deadline_ms;

    // signal state (simplified)
    uint32_t signal_mask;
    uint32_t pending_signals;

    // session / process group / controlling terminal (POSIX session model)
    uint32_t sid;          // session id (0 = none)
    uint32_t pgid;         // process group id
    int      ctty_pgrp;    // foreground process group on the controlling tty
    bool     is_session_leader;
};

//  linuxsyscall - the syscall handler

#define LINUX_MAX_PROCS  256   // 64->256: firefox (multi-thread parent + fork
                               // server + 8 content procs w/ threads) exceeds 64
                               // live tasks; clone then fails and a critical
                               // launch thread never spawns -> deadlock. (satoru)

class LinuxSyscall {
public:
    static void Init();

    // create a new linux process context
    static int  CreateProcess(const char* name, uint32_t uid, uint32_t gid);
    static void DestroyProcess(int pid_idx);
    static LinuxProcess* GetProcess(int pid_idx);
    static LinuxProcess* Current();
    static int  GetCurrentIndex();
    static void SetCurrent(int pid_idx);
    // point this cpu's linux "current process" at the LinuxProcess owning `task`.
    // an application processor that RESUMES a scheduler thread (smp thread
    // dispatch) must sync this before the thread runs, or LinuxSyscall::Current()
    // is null on that cpu and syscalls (mremap stack-probe, etc.) misbehave in a
    // loop. no-op if the task has no LinuxProcess. (satoru)
    static void SyncCurrentToTask(Process* task);
    static bool HandlePageFault(InterruptFrame* frame);
    // register the irq0 timer-preemption handler (round-robins user threads). (satoru)
    static void EnableTimerPreemption();

    // the main syscall dispatcher
    // called when int 0x80 fires from a linux elf binary, or from the
    // x86_64 SYSCALL fast path. the syscall number stays small but the
    // arg registers carry full 64-bit user pointers/addresses (a pie
    // binary loaded above 4gb must round-trip its pointers intact), and
    // the return value is widened so a >4gb mmap result is not truncated
    // or sign-mangled on the way back to userspace (satoru)
    static int64_t Dispatch(uint64_t eax, uint64_t ebx, uint64_t ecx,
                            uint64_t edx, uint64_t esi, uint64_t edi);

    // public so the x86_64 SYSCALL path can route mmap straight here with the
    // full 64-bit file offset (a5/r9) - the i386 dispatch drops it, which breaks
    // file-backed mmap of .so segments at non-zero offsets. (satoru)
    static int64_t sys_mmap(uintptr_t addr, uint64_t length, uint32_t prot,
                            uint32_t flags, int fd, uint64_t offset);

    // x86_64-ABI stat family: same kvfs/ext4 resolution as the i386
    // sys_stat/sys_fstat handlers, but they fill the 64-bit
    // `struct LinuxStat64` layout and validate the user statbuf pointer.
    // Called directly from the SYSCALL fast path (linux_syscall_x64.cpp) so
    // an amd64 binary gets the struct it expects; the int 0x80 path keeps the
    // 32-bit layout via sys_stat/sys_fstat. (satoru)
    static int32_t sys_stat64(uintptr_t pathname, uintptr_t statbuf);
    static int32_t sys_fstat64(int fd, uintptr_t statbuf);
    static int32_t sys_fstatat64(int dirfd, uintptr_t pathname,
                                 uintptr_t statbuf, int flags);

    // readlink resolution shared by the x86_64 readlink/readlinkat handlers.
    // handles /proc/self/exe, /proc/self/fd/N, real kvfs symlinks, and - crucially
    // - returns -EINVAL (not -ENOENT) for a path that EXISTS but is not a symlink,
    // because musl's realpath() walks each path component with readlink and treats
    // ENOENT as "path missing" (which aborts gecko's XRE_GetFileFromPath -> the
    // -profile path). writes up to bufsiz bytes (no nul); returns count or -errno.
    // (satoru)
    static int ReadlinkResolve(const char* path, char* buf, int bufsiz);

    // readv (x86_64 nr 19) - the read counterpart of writev. there is no i386
    // dispatch entry for it, so the SYSCALL fast path (linux_syscall_x64.cpp)
    // calls this directly; public for the same reason sys_mmap is. (satoru)
    static int32_t sys_readv(int fd, uintptr_t iov, uint64_t iovcnt);

    // permanent, rate-limited "[kls] ENOSYS nr=<n> <name>" trace. an unknown or
    // unimplemented x86_64 number routes here from BOTH the i386 dispatch default
    // and the x64 translation miss, so a strace-equivalent audit of any real
    // binary shows exactly which numbers it still needs. rate-limited so a busy
    // poll loop on a missing nr cannot flood COM1. (satoru)
    static void LogEnosys(uint64_t nr, const char* name);

    // stats
    static int  ActiveProcessCount();

    static bool HasConsoleOutput();
    static int  ReadConsoleOutput(char* buf, int max_len);
    static void ClearConsoleOutput();

    // true when stdin has buffered, unread bytes - used by the poll/epoll
    // readiness logic to report EPOLLIN on fd 0 (satoru)
    static bool StdinReadable();

    static void InjectStdin(const char* data, int len);

    // creates a process, runs the named builtin, and returns output
    static int  RunProgram(const char* name, int argc, const char** argv,
                           char* output, int max_output);

    // headless syscall-ABI self-test (gated by kurono.klstest): exercises a
    // representative syscall from each tier through Dispatch and logs PASS/FAIL
    // per check to serial, in the kurono.kfstest/kjtest style. proves the new
    // tier 1-11 handlers are wired and return sane values without a GUI run.
    // returns the number of FAILED checks (0 == all pass). (satoru)
    static int  SelfTest();

private:
    static LinuxProcess procs[LINUX_MAX_PROCS];
    // the "current linux process" is per-cpu so each core resolves its own caller
    // when running user threads in parallel; -1 = none. (smp phase 3d) (satoru)
    static int current_proc_cpu[SMP_MAX_CPUS];

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

    // individual syscall handlers.
    // user pointers/addresses widened to uintptr_t and the paired
    // sizes/lengths/offsets to uint64_t so >4gb user memory round-trips;
    // genuine 32-bit values (fd, mode, flags, prot, whence, exit codes,
    // pid/status/options) stay 32-bit. mmap/brk/getcwd return an address,
    // so their return type is widened to int64_t to avoid truncation (satoru)
    static int32_t sys_exit(uint32_t code);
    static int32_t sys_fork();
    static int32_t sys_read(int fd, uintptr_t buf, uint64_t count);
    static int32_t sys_write(int fd, uintptr_t buf, uint64_t count);
    static int32_t sys_open(uintptr_t pathname, uint32_t flags, uint32_t mode);
    static int32_t sys_close(int fd);
    static int32_t sys_waitpid(uint32_t pid, uintptr_t status, uint32_t options);
    static int32_t sys_execve(uintptr_t filename, uintptr_t argv, uintptr_t envp);
    static int32_t sys_lseek(int fd, int32_t offset, uint32_t whence);
    static int64_t sys_brk(uintptr_t addr);
    static int32_t sys_getpid();
    static int32_t sys_getuid();
    static int32_t sys_getgid();
    static int32_t sys_geteuid();
    static int32_t sys_getegid();
    static int32_t sys_getppid();
    static int32_t sys_stat(uintptr_t pathname, uintptr_t statbuf);
    static int32_t sys_fstat(int fd, uintptr_t statbuf);
    static int32_t sys_uname(uintptr_t buf);
    static int64_t sys_getcwd(uintptr_t buf, uint64_t size);
    static int32_t sys_chdir(uintptr_t pathname);
    static int32_t sys_mkdir(uintptr_t pathname, uint32_t mode);
    static int32_t sys_rmdir(uintptr_t pathname);
    static int32_t sys_unlink(uintptr_t pathname);
    static int32_t sys_access(uintptr_t pathname, uint32_t mode);
    static int32_t sys_dup(int oldfd);
    static int32_t sys_dup2(int oldfd, int newfd);
    static int32_t sys_ioctl(int fd, uint32_t cmd, uint64_t arg);
    static int32_t sys_writev(int fd, uintptr_t iov, uint64_t iovcnt);
    static int32_t sys_munmap(uintptr_t addr, uint64_t length);
    static int32_t sys_mprotect(uintptr_t addr, uint64_t length, uint32_t prot);
    static int32_t sys_madvise(uintptr_t addr, uint64_t length, uint32_t advice);
    static int32_t sys_nanosleep(uintptr_t req, uintptr_t rem);
    static int32_t sys_getdents64(int fd, uintptr_t dirp, uint64_t count);
    static int32_t sys_clock_gettime(uint32_t clk_id, uintptr_t tp);
    static int32_t sys_set_thread_area(uintptr_t u_info);
    static int32_t sys_exit_group(uint32_t code);

    // fd helpers
    static int  AllocFd(LinuxProcess* p);
    static void InitStdFds(LinuxProcess* p);

    // path resolution: translates linux paths to kurono paths
    static void ResolvePath(const char* linux_path, char* kurono_path,
                            int max_len, LinuxProcess* p);
};
