#pragma once
#include "../kernel/types.h"

//  /dev/dri for Linux programs on Kurono: the Linux ABI half of the KLD drm
//  userspace interface. Everything is behind the boot flag kurono.kld.dri=1.
//  The broker half is src/kernel/kld_dri.cpp. (satoru)
//
//  end (satoru)

struct LinuxProcess;
struct LinuxFd;
struct Process;

namespace DriDev {

extern bool g_enabled;
static inline bool Enabled() { return g_enabled; }

void     BootInit(const char* cmdline);
uint32_t TestId();
uint32_t TestCg();   // rig: id of the test cgroup, 0 when none (satoru)

// policy hook: may this process open this node? the first userspace opener of
// the primary node becomes drm master and can take the screen, so a permission
// model plugs in here. today: everything is allowed when the flag is on. (satoru)
bool MayOpen(uint32_t pid, uint32_t node);

bool PathIsDri(const char* resolved);

// open returns OPEN_READY with *file_out set, or the syscall return value (a
// negative errno, or the value of a restartable park). (satoru)
constexpr int64_t OPEN_READY = 0x7ffe000000000001LL;
int64_t Open(LinuxProcess* p, const char* resolved, uint32_t flags, int* file_out);
void    OpenAbort(int file);

int32_t Ioctl(LinuxProcess* p, LinuxFd* lfd, int fd, uint32_t cmd, uint64_t arg);
int64_t Mmap(LinuxProcess* p, LinuxFd* lfd, uint64_t addr, uint64_t len, uint32_t prot,
             uint32_t flags, uint64_t offset);
int64_t Read(LinuxProcess* p, LinuxFd* lfd, uint64_t buf, uint64_t count);

void     FdClose(LinuxProcess* p, LinuxFd* lfd);
void     Retain(const LinuxFd* lfd);
uint32_t Readiness(const LinuxFd* lfd);
void     TaskGone(Process* task);
void     StaleCheck(LinuxProcess* p, int fd);

// rig only: spawn/kill/inspect unprivileged test processes for the container's test program (satoru)
int64_t ChildOp(uint32_t op, uint64_t a0, uint64_t a1);
void RequestKill(uint32_t pid);   // rig: a runner kernel process kills this address space group (satoru)

// broker callbacks: nodes appear when the container attaches. (satoru)
void OnAttach(uint32_t minors);
void OnDetach();

}  // namespace DriDev

// end (satoru)
