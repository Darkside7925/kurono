#pragma once
//  kurono os  -  linux kernel emulation layer
//  a virtual linux kernel running inside kurono os, providing full linux
//  abi compatibility. this is not a real linux kernel  -  it translates
//  linux kernel interfaces to kurono's native subsystems while presenting
//  itself as a real linux 6.8 kernel to userspace programs.
//
//  design:
//  ┌─────────────────────────────────────────────────────────────────────┐
//  │  linux elf binaries (bash, ls, gcc, python, etc.)                  │
//  ├─────────────────────────────────────────────────────────────────────┤
//  │  linux kernel emulation layer  ←  you are here                     │
//  │   ┌──────────┬──────────┬──────────┬──────────┬──────────────────┐ │
//  │   │ signals  │ threads  │ tty/pty  │ /proc    │ /sys             │ │
//  │   │ layer    │ manager  │ driver   │ virtual  │ virtual          │ │
//  │   └──────────┴──────────┴──────────┴──────────┴──────────────────┘ │
//  ├─────────────────────────────────────────────────────────────────────┤
//  │  kurono native kernel (scheduler, kvfs, supr, hal, drivers)       │
//  └─────────────────────────────────────────────────────────────────────┘

#include "../kernel/types.h"

#define LINUX_KERNEL_MAJOR    6
#define LINUX_KERNEL_MINOR    8
#define LINUX_KERNEL_PATCH    0
#define LINUX_KERNEL_VERSION  "6.8.0-kurono"
#define LINUX_KERNEL_RELEASE  "6.8.0-1-kurono"
#define LINUX_KERNEL_SYSNAME  "Linux"
#define LINUX_KERNEL_MACHINE  "i686"

#define PROC_MAX_ENTRIES     64
#define PROC_BUF_SIZE        4096

enum ProcEntryType {
    PROC_FILE = 0,        // static or dynamic file
    PROC_DIR  = 1,        // directory
    PROC_LINK = 2,        // symbolic link
    PROC_PID  = 3         // per-process /proc/[pid] entry
};

struct ProcEntry {
    char            name[64];
    char            parent[64];     // parent path (e.g., "/proc")
    ProcEntryType   type;
    bool            active;

    // for dynamic content  -  called when read
    int (*generator)(char* buf, int max_len, void* ctx);
    void* ctx;

    // for static content
    char  static_content[512];
    int   static_len;
};

#define SYS_MAX_ENTRIES      32

struct SysEntry {
    char  path[128];            // full path under /sys
    char  value[256];           // current value
    bool  writable;
    bool  active;
};

#define LINUX_MAX_TTYS        8
#define LINUX_TTY_BUF_SIZE    1024

enum TTYType {
    TTY_CONSOLE = 0,       // /dev/tty0, /dev/console
    TTY_SERIAL  = 1,       // /dev/ttys0
    TTY_PTY     = 2,       // /dev/pts/n
    TTY_NULL    = 3        // /dev/null
};

struct LinuxTermios {
    uint32_t c_iflag;      // input  modes
    uint32_t c_oflag;      // output modes
    uint32_t c_cflag;      // control modes
    uint32_t c_lflag;      // local  modes
    uint8_t  c_cc[32];     // control characters
    uint32_t c_ispeed;     // input baud
    uint32_t c_ospeed;     // output baud
};

struct LinuxTTY {
    TTYType     type;
    char        name[16];      // e.g., "pts/0"
    bool        active;
    int         owner_pid;     // process owning this tty

    // ring buffers
    char        input_buf[LINUX_TTY_BUF_SIZE];
    int         input_head, input_tail;
    char        output_buf[LINUX_TTY_BUF_SIZE];
    int         output_head, output_tail;

    // terminal settings
    LinuxTermios termios;

    // window size (for tiocgwinsz)
    uint16_t    ws_row, ws_col;
    uint16_t    ws_xpixel, ws_ypixel;
};

#define LINUX_MAX_THREADS    32

// clone flags (subset)
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000
#define CLONE_PARENT_SETTID  0x00100000

struct LinuxThread {
    uint32_t tid;           // thread id
    uint32_t pid;           // parent process id (same for all threads in group)
    uint32_t tgid;          // thread group id
    bool     active;

    // thread-local storage
    uint32_t tls_base;      // set_thread_area / arch_prctl
    uint32_t stack_base;
    uint32_t stack_size;

    // saved registers (for context switch)
    uint32_t esp, ebp, eip;
    uint32_t eax, ebx, ecx, edx;
    uint32_t esi, edi;
    uint32_t eflags;

    // scheduling
    int      priority;
    uint32_t cpu_time;      // ticks used
    int      exit_code;
};

#define LINUX_MAX_FUTEXES    16
#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_PRIVATE_FLAG   128

struct LinuxFutex {
    uint32_t* addr;
    int       waiters[LINUX_MAX_THREADS];
    int       waiter_count;
    bool      active;
};

//  linuxkernel  -  the virtual kernel class

class LinuxKernel {
public:
    static void Init();
    static void Start();
    static void Stop();
    static void Tick(uint32_t now_ms);   // called each timer interrupt

    static void InitProcFS();
    static int  ReadProc(const char* path, char* buf, int max_len);
    static int  WriteProc(const char* path, const char* data, int len);
    static bool ProcExists(const char* path);
    static void RegisterProcEntry(const char* name, const char* parent,
                                   ProcEntryType type,
                                   int (*gen)(char*, int, void*), void* ctx);

    // standard /proc generators
    static int GenVersion(char* buf, int mx, void* ctx);
    static int GenUptime(char* buf, int mx, void* ctx);
    static int GenMeminfo(char* buf, int mx, void* ctx);
    static int GenCpuinfo(char* buf, int mx, void* ctx);
    static int GenStat(char* buf, int mx, void* ctx);
    static int GenLoadavg(char* buf, int mx, void* ctx);
    static int GenMounts(char* buf, int mx, void* ctx);
    static int GenFilesystems(char* buf, int mx, void* ctx);
    static int GenCmdline(char* buf, int mx, void* ctx);
    static int GenPidStatus(char* buf, int mx, void* ctx);
    static int GenPidStat(char* buf, int mx, void* ctx);
    static int GenPidMaps(char* buf, int mx, void* ctx);
    static int GenNetDev(char* buf, int mx, void* ctx);
    static int GenPartitions(char* buf, int mx, void* ctx);

    static void InitSysFS();
    static int  ReadSys(const char* path, char* buf, int max_len);
    static int  WriteSys(const char* path, const char* data, int len);
    static bool SysExists(const char* path);

    static void InitTTY();
    static int  AllocPTY();             // returns pty index
    static void FreePTY(int idx);
    static int  TTYRead(int idx, char* buf, int max_len);
    static int  TTYWrite(int idx, const char* buf, int len);
    static void TTYSetTermios(int idx, const LinuxTermios* t);
    static void TTYGetTermios(int idx, LinuxTermios* t);
    static void TTYSetWinSize(int idx, uint16_t rows, uint16_t cols);
    static LinuxTTY* GetTTY(int idx);

    static int  CreateThread(uint32_t pid, uint32_t flags,
                              uint32_t stack, uint32_t tls);
    static void DestroyThread(int tid);
    static LinuxThread* GetThread(int tid);
    static int  GetThreadCount();

    static int  FutexWait(uint32_t* addr, uint32_t expected, uint32_t timeout);
    static int  FutexWake(uint32_t* addr, int count);

    static uint32_t GetUptime();       // seconds since start
    static uint64_t GetUptimeMs();
    static void     GetLoadAvg(int* avg1, int* avg5, int* avg15);
    static uint32_t GetTotalMemory();
    static uint32_t GetFreeMemory();
    static uint32_t GetJiffies();
    static int      GetHZ();           // timer frequency

    static const char* GetVersion();
    static const char* GetRelease();
    static const char* GetSysname();
    static const char* GetMachine();
    static const char* GetDomainname();

    static int  GetRandom(void* buf, uint32_t len);
    static int  GetURandom(void* buf, uint32_t len);

private:
    static bool         running;
    static uint32_t     start_time;    // ms when start() was called
    static uint32_t     jiffies;       // tick counter

    // /proc
    static ProcEntry    proc_entries[PROC_MAX_ENTRIES];
    static int          proc_count;

    // /sys
    static SysEntry     sys_entries[SYS_MAX_ENTRIES];
    static int          sys_count;

    // tty
    static LinuxTTY     ttys[LINUX_MAX_TTYS];
    static int          tty_count;

    // threads
    static LinuxThread  threads[LINUX_MAX_THREADS];
    static int          next_tid;

    // futex
    static LinuxFutex   futexes[LINUX_MAX_FUTEXES];

    // load average tracking
    static int          load_1, load_5, load_15;

    // rng state
    static uint32_t     rng_state;

    // helpers
    static int  ProcIndex(const char* path);
    static int  SysIndex(const char* path);
    static int  PutStr(char* buf, int pos, int mx, const char* s);
    static int  PutDec(char* buf, int pos, int mx, int val);
    static int  PutHex(char* buf, int pos, int mx, uint32_t val);
    static int  PutLine(char* buf, int pos, int mx, const char* key, const char* val);
    static int  PutKB(char* buf, int pos, int mx, const char* key, uint32_t kb);
};
