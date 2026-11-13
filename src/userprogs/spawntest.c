// spawntest - posix_spawn() of a static x86_64 ELF from ring-3 (kurono.spawntest=1). (satoru)
//
// musl's posix_spawn() is clone(CLONE_VM|CLONE_VFORK|SIGCHLD) on a child stack,
// then execve() in the child; any exec error comes back through a CLOEXEC pipe.
// fork()+execve() of /system/bin/busybox works, so this isolates the vfork
// child's execve path. one [spawntest] line per case, then DONE pass/fail. (satoru)

#include <errno.h>
#include <spawn.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void logs(const char* s) { (void)!write(2, s, strlen(s)); }
static void logi(long v) {
    char b[24]; int n = 0; char t[24]; int k = 0;
    if (v < 0) { b[n++] = '-'; v = -v; }
    if (v == 0) t[k++] = '0';
    while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) b[n++] = t[--k];
    (void)!write(2, b, (size_t)n);
}

static int g_pass;
static int g_fail;

// one spawn case: rc is posix_spawn's return, status the raw waitpid status. (satoru)
static void report(const char* name, int rc, int wp, int status) {
    logs("[spawntest] case="); logs(name);
    logs(" rc="); logi(rc);
    logs(" status="); logi(status);
    logs("\r\n");
    int ok = rc == 0 && wp > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (ok) g_pass++; else g_fail++;
}

int main(void) {
    char* argv[] = { (char*)"busybox", (char*)"true", 0 };
    char* envp[] = { (char*)"PATH=/system/bin", 0 };

    // case=spawn: posix_spawn with the absolute path, no libc path search. (satoru)
    {
        pid_t pid = -1;
        int rc = posix_spawn(&pid, "/system/bin/busybox", 0, 0, argv, envp);
        int status = 0xffff;
        int wp = -1;
        if (rc == 0) wp = (int)waitpid(pid, &status, 0);
        report("spawn", rc, wp, status);
    }

    // case=spawnp: posix_spawnp does the PATH search inside the vfork child. (satoru)
    {
        pid_t pid = -1;
        int rc = posix_spawnp(&pid, "busybox", 0, 0, argv, envp);
        int status = 0xffff;
        int wp = -1;
        if (rc == 0) wp = (int)waitpid(pid, &status, 0);
        report("spawnp", rc, wp, status);
    }

    logs("[spawntest] DONE pass="); logi(g_pass);
    logs(" fail="); logi(g_fail);
    logs("\r\n");
    return g_fail ? 1 : 0;
}
