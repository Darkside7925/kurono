// kentest - privilege-gate regression (kurono.kentest=1).
//
// runs as a normal user (uid 1000) when launched by the boot runner and tries
// each operation the admin gate is supposed to stop. without an approval every
// one must fail; with an approval (QMP Enter on the modal) the first one passes
// and the short grant lets the rest through. logs one RESULT line per action.
//
// if argv[1] is "dummy" the binary just records its pid and sleeps so the real
// test has a foreign-uid (root) process it does not own to signal.
//
// end (satoru)

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/syscall.h>

static void logs(const char* s) { (void)!write(2, s, strlen(s)); }
static void logd(long v) {
    char b[24]; int n = 0; char t[24]; int k = 0;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (u == 0) t[k++] = '0';
    while (u) { t[k++] = (char)('0' + u % 10); u /= 10; }
    if (neg) b[n++] = '-';
    while (k) b[n++] = t[--k];
    (void)!write(2, b, (size_t)n);
}
static void result(const char* what, long rc) {
    logs("[kentest] RESULT ");
    logs(what);
    logs(" rc="); logd(rc);
    logs(rc >= 0 ? " ALLOWED\n" : " DENIED\n");
}

static long write_file(const char* path, const char* data, int flags) {
    int fd = open(path, flags, 0644);
    if (fd < 0) return -errno;
    long r = (long)write(fd, data, strlen(data));
    close(fd);
    return r < 0 ? -errno : 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "dummy") == 0) {
        char b[24]; int n = 0; int v = (int)getpid();
        char t[16]; int k = 0;
        if (v == 0) t[k++] = '0';
        while (v > 0) { t[k++] = (char)('0' + v % 10); v /= 10; }
        while (k) b[n++] = t[--k];
        b[n++] = '\n'; b[n] = 0;
        (void)write_file("/tmp/kendummy.pid", b, O_WRONLY | O_CREAT | O_TRUNC);
        logs("[kentest] dummy up uid="); logd((long)getuid());
        logs(" pid="); logd((long)getpid()); logs("\n");
        for (int i = 0; i < 120; i++) sleep(1);
        return 0;
    }

    logs("[kentest] begin uid="); logd((long)getuid());
    logs(" euid="); logd((long)geteuid()); logs("\n");

    // 1. a system-path write (satoru)
    result("write /system", write_file("/system/kentest_probe", "probe\n",
                                        O_WRONLY | O_CREAT | O_TRUNC));

    // 2. raw block device (satoru)
    {
        static const char* devs[] = { "/dev/sda", "/dev/sda1", "/dev/nvme0n1",
                                      "/dev/nvme0n1p1", NULL };
        long rc = -2; const char* used = "none";
        for (int i = 0; devs[i]; i++) {
            int fd = open(devs[i], O_RDONLY);
            if (fd >= 0) { close(fd); rc = 0; used = devs[i]; break; }
            rc = -errno;
            if (errno != ENOENT) { used = devs[i]; break; }
        }
        logs("[kentest] rawdev "); logs(used); logs("\n");
        result("open raw block", rc);
    }

    // 3. signal a process owned by another uid. the runner passes its pid
    // (argv[1]="foreign", argv[2]=pid). signal 0 only probes existence, but
    // the gate runs first. (satoru)
    {
        long rc = -2;
        if (argc > 2 && strcmp(argv[1], "foreign") == 0) {
            int pid = 0;
            for (int i = 0; argv[2][i] >= '0' && argv[2][i] <= '9'; i++)
                pid = pid * 10 + (argv[2][i] - '0');
            if (pid > 0) rc = (kill(pid, 0) == 0) ? 0 : -errno;
        }
        result("kill foreign process", rc);
    }

    // 4. the kld driver broker (KLD_OP_STATUS) (satoru)
    result("kld broker", (long)syscall(0x4B4C4442ull, 0x10ull, 0, 0, 0));

    // 5. changing system settings (the ui config file) (satoru)
    result("write /etc settings", write_file("/etc/kurono/ui.conf", "\n# kentest\n",
                                              O_WRONLY | O_CREAT | O_APPEND));

    // 5b. a dot-dot component must not turn a writable /home prefix into a
    // system write. the policy is path based, so it has to see the normalized
    // path, not the spelling. (satoru)
    result("dotdot escape", write_file("/home/../system/kentest_dotdot", "probe\n",
                                        O_WRONLY | O_CREAT | O_TRUNC));

    // 5c. a symlink created under a writable root must not be a write-through
    // to a system path: creating the link is allowed, following it is not.
    // the target already exists, and the probe only opens it for write (no
    // write/truncate), so the old gate is proven by the open alone. (satoru)
    {
        long rc = -2;
        if (symlink("/kurono/linux/compat/bin/kentest", "/tmp/kentest_link") == 0) {
            int fd = open("/tmp/kentest_link", O_WRONLY);
            rc = (fd >= 0) ? 0 : -errno;
            if (fd >= 0) close(fd);
        } else {
            rc = -errno;
        }
        result("symlink escape", rc);
    }

    // 5d. the kernel exec bridge is a privileged shell. kcl can write any path
    // and exec any command as the kernel, so it must be gated like any other
    // privileged builtin. without the gate the write only fails with EBUSY
    // because this test has no tty, so EPERM is the proof it was refused
    // before dispatch. (satoru)
    {
        long rc = -2;
        int fd = open("/dev/kexec", O_WRONLY);
        if (fd < 0) {
            rc = -errno;
        } else {
            static const char cmd[] = "kcl -c \"write('/system/kentest_kcl','pwn')\"\n";
            ssize_t w = write(fd, cmd, sizeof(cmd) - 1);
            if (w == (ssize_t)(sizeof(cmd) - 1)) rc = 0;
            else rc = (errno == EPERM) ? -1 : 0;
            close(fd);
        }
        result("kexec kcl", rc);
    }

    // 5d2. the ken policy subcommands mutate the auth policy without a
    // password (the sudo-style form is the one that does authenticate), so a
    // ring-3 shell must not run them through the privileged exec bridge
    // without approval. EPERM before dispatch is the pass condition. (satoru)
    {
        long rc = -2;
        int fd = open("/dev/kexec", O_WRONLY);
        if (fd < 0) {
            rc = -errno;
        } else {
            static const char cmd[] = "ken policy kvault disable --force --acknowledge-risk\n";
            ssize_t w = write(fd, cmd, sizeof(cmd) - 1);
            if (w == (ssize_t)(sizeof(cmd) - 1)) rc = 0;
            else rc = (errno == EPERM) ? -1 : 0;
            close(fd);
        }
        result("kexec ken policy", rc);
    }

    // 5e. getdents64 writes a dirent stream into the caller's buffer. a kernel
    // address must be refused with EFAULT, not written through. (satoru)
    {
        long rc = -2;
        int fd = open("/proc", O_RDONLY | O_DIRECTORY);
        if (fd < 0) {
            rc = -errno;
        } else {
            long r = syscall(SYS_getdents64, fd, (void*)0xFFFF800000000000ULL, 4096);
            rc = (r == -1 && errno == EFAULT) ? -1 : 0;
            close(fd);
        }
        result("getdents kernel ptr", rc);
    }

    // 6. explicit sudo-style identity change: must be the last check (satoru)
    {
        long rc = (setuid(0) == 0) ? 0 : -errno;
        result("setuid(0)", rc);
        if (rc == 0) {
            logs("[kentest] now euid="); logd((long)geteuid()); logs("\n");
            result("write /system as root",
                   write_file("/system/kentest_root", "root\n",
                              O_WRONLY | O_CREAT | O_TRUNC));
        }
    }

    logs("[kentest] done\n");
    return 0;
}

// end (satoru)
