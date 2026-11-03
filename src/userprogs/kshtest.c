// kshtest - headless gate for /dev/kshell, the rescue-agent command channel.
// run with kurono.kshtest=1; prints [kshtest] lines to com1. (satoru)
//
// this exists because the ai recovery client (dsai.c) depends on /dev/kshell
// for its entire tool loop - musl's popen() execve's /bin/sh and kurono has
// none - and that dependency was previously untestable without dragging the
// whole tls + network stack into the boot. this isolates the device.
//
// it checks BOTH halves that matter:
//   1. an allowed diagnostic actually runs and its output comes back
//   2. a destructive command is REFUSED by the kernel-side whitelist
// (2) is the one that must never regress. the ring-3 gate in dsai protects
// nothing on its own - any process can open this device - so the kernel is
// where the policy has to hold. (satoru)

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static int run(const char *cmd, char *out, int mx)
{
    int fd = open("/dev/kshell", O_RDWR);
    if (fd < 0) {
        printf("[kshtest] open /dev/kshell FAILED\n");
        return -1;
    }
    if (write(fd, cmd, strlen(cmd)) < 0) {
        printf("[kshtest] write FAILED\n");
        close(fd);
        return -1;
    }
    int total = 0;
    for (;;) {
        int n = (int)read(fd, out + total, (size_t)(mx - 1 - total));
        if (n <= 0) break;
        total += n;
        if (total >= mx - 1) break;
    }
    out[total] = 0;
    close(fd);
    return total;
}

int main(void)
{
    static char out[16384];
    int pass = 0, fail = 0;

    printf("[kshtest] start\n");

    // 1. an allowed diagnostic must run and produce output
    int n = run("lsblk", out, sizeof out);
    if (n > 0 && !strstr(out, "refused")) {
        printf("[kshtest] PASS allowed-cmd lsblk (%d bytes)\n", n);
        printf("[kshtest] ---- lsblk output ----\n%s\n", out);
        pass++;
    } else {
        printf("[kshtest] FAIL allowed-cmd lsblk n=%d\n", n);
        fail++;
    }

    // 2. a destructive command must be refused by the kernel, not run
    n = run("mkfs.ext4 /dev/sda1", out, sizeof out);
    if (n > 0 && strstr(out, "refused")) {
        printf("[kshtest] PASS refused destructive cmd\n");
        pass++;
    } else {
        printf("[kshtest] FAIL destructive cmd was NOT refused n=%d out=%s\n", n, out);
        fail++;
    }

    // 3. a metacharacter smuggle must be refused even though it starts with an
    //    allowed token - this is the "lsblk; mkfs" bypass (satoru)
    n = run("lsblk; mkfs.ext4 /dev/sda1", out, sizeof out);
    if (n > 0 && strstr(out, "refused")) {
        printf("[kshtest] PASS refused metachar smuggle\n");
        pass++;
    } else {
        printf("[kshtest] FAIL metachar smuggle was NOT refused n=%d\n", n);
        fail++;
    }

    // 4. a path-qualified binary must be refused (satoru)
    n = run("/sbin/mkfs.ext4 /dev/sda1", out, sizeof out);
    if (n > 0 && strstr(out, "refused")) {
        printf("[kshtest] PASS refused path-qualified binary\n");
        pass++;
    } else {
        printf("[kshtest] FAIL path-qualified binary was NOT refused n=%d\n", n);
        fail++;
    }

    // 5. sessions must be reclaimed on close, or the 2-slot table runs dry.
    //    the loop is deliberately longer than the table. (satoru)
    int leaked = 0;
    for (int i = 0; i < 8; i++) {
        int fd = open("/dev/kshell", O_RDWR);
        if (fd < 0) { leaked = 1; break; }
        close(fd);
    }
    if (!leaked) {
        printf("[kshtest] PASS session slots reclaimed on close\n");
        pass++;
    } else {
        printf("[kshtest] FAIL session slot leak\n");
        fail++;
    }

    printf("[kshtest] OVERALL %s (%d pass, %d fail)\n",
           fail ? "FAIL" : "PASS", pass, fail);
    printf("[kshtest] DONE\n");
    return fail ? 1 : 0;
}

// end (satoru)
