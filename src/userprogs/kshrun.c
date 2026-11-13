// ksh-run - run a kurono shell builtin from a linux shell. (satoru)
//
// claude code's bash tool (and any developer) can reach the machine's own
// commands (ksys, kdrv, kinit, audio, gpu...) through /dev/kshell, which
// captures the command output. this client opens one session, writes the
// command with the status-request prefix, prints what came back and exits
// with the command's status.
//
//   ksh-run <command> [args...]      ksh-run -c "command line"
//   kcmd <command> [args...]         kcmd -c "command line"
//   ksys health                      (symlink to ksh-run: argv[0] is the command)
//
// exit: the builtin's status; 126 refused/denied, 127 no such command,
//       125 could not reach /dev/kshell.

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static const char *base(const char *p)
{
    const char *b = p;
    for (; p && *p; p++) if (*p == '/') b = p + 1;
    return b;
}

static int known_command(const char *n)
{
    static const char *names[] = {
        "ksys", "kdrv", "kinit", "kctl", "audio", "recovery", "persisttest",
        "gpu", "gpuprobe", "gpuaux", "gpumode", "gpuaccel", "display",
        "sysinfo", "hwinfo", "drivers", "lsblk", "lspci", "lsusb", "lscpu",
        "dmidecode", "hwfw", "iotop", "ss", "nic", "wifi", "nmcli", "journal",
        "ver", "version", "tree", "fls", "fcat", "codecs", "vgpu", "screenshot",
        "help", "usermode", "ken", "amdo", 0
    };
    for (int i = 0; names[i]; i++) if (!strcmp(names[i], n)) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    char line[2048];
    int n = 0;
    const char *me = argc > 0 ? base(argv[0]) : "ksh-run";

    if (!strcmp(me, "kctl")) {
        // the small service-control spelling: kctl list|restart|stop|start|logs
        // maps onto the kinit command. (satoru)
        const char *sub = argc > 1 ? argv[1] : "status";
        if (!strcmp(sub, "list")) sub = "status";
        if (strcmp(sub, "status") && strcmp(sub, "restart") && strcmp(sub, "stop") &&
            strcmp(sub, "start") && strcmp(sub, "logs") && strcmp(sub, "enable") &&
            strcmp(sub, "disable") && strcmp(sub, "reload") && strcmp(sub, "analyze")) {
            fprintf(stderr, "usage: kctl list | restart <svc> | stop <svc> | start <svc> | logs [svc]\n");
            return 2;
        }
        n = snprintf(line, sizeof line, "kinit %s", sub);
        for (int i = 2; i < argc && n > 0 && n < (int)sizeof line; i++) {
            int l = (int)strlen(argv[i]);
            if (n + l + 2 >= (int)sizeof line) break;
            line[n++] = ' ';
            memcpy(line + n, argv[i], (size_t)l);
            n += l;
            line[n] = 0;
        }
    } else if (argc >= 3 && !strcmp(argv[1], "-c")) {
        snprintf(line, sizeof line, "%s", argv[2]);
    } else if (!strcmp(me, "kcmd") || !strcmp(me, "ksh-run") || !strcmp(me, "ksh")) {
        if (argc < 2) {
            fprintf(stderr, "usage: %s <command> [args...]\n", me);
            return 2;
        }
        for (int i = 1; i < argc; i++) {
            int l = (int)strlen(argv[i]);
            if (n + l + 2 >= (int)sizeof line) break;
            if (n) line[n++] = ' ';
            memcpy(line + n, argv[i], (size_t)l);
            n += l;
        }
        line[n] = 0;
    } else if (known_command(me)) {
        int l = (int)strlen(me);
        if (l + 2 < (int)sizeof line) { memcpy(line, me, (size_t)l); n = l; line[n] = 0; }
        for (int i = 1; i < argc; i++) {
            l = (int)strlen(argv[i]);
            if (n + l + 2 >= (int)sizeof line) break;
            line[n++] = ' ';
            memcpy(line + n, argv[i], (size_t)l);
            n += l;
            line[n] = 0;
        }
    } else {
        if (argc < 2) {
            fprintf(stderr, "usage: %s <command> [args...]\n", me);
            return 2;
        }
        for (int i = 1; i < argc; i++) {
            int l = (int)strlen(argv[i]);
            if (n + l + 2 >= (int)sizeof line) break;
            if (n) line[n++] = ' ';
            memcpy(line + n, argv[i], (size_t)l);
            n += l;
        }
        line[n] = 0;
    }

    int fd = open("/dev/kshell", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "%s: cannot open /dev/kshell: %s\n", me, strerror(errno));
        return 125;
    }
    char wbuf[2050];
    wbuf[0] = 0x01;   // ask for the \x02<status>\x02 trailer
    memcpy(wbuf + 1, line, (size_t)n);
    wbuf[n + 1] = '\n';
    if (write(fd, wbuf, (size_t)n + 2) < 0) {
        fprintf(stderr, "%s: command refused: %s\n", me, strerror(errno));
        close(fd);
        return 126;
    }

    static char out[16384];
    int total = 0;
    for (;;) {
        int r = (int)read(fd, out + total, sizeof(out) - 1 - (size_t)total);
        if (r <= 0) break;
        total += r;
        if (total >= (int)sizeof(out) - 1) break;
    }
    out[total] = 0;
    close(fd);

    int status = 0;
    int end = total;
    if (end >= 4 && out[end - 1] == '\n') {
        int q = end - 2;
        if (q >= 1 && out[q] == 0x02) {
            int a = q;
            while (a > 0 && out[a - 1] >= '0' && out[a - 1] <= '9') a--;
            if (a < q) {
                int v = 0;
                for (int i = a; i < q; i++) v = v * 10 + (out[i] - '0');
                status = v;
                end = a - 1;   // drop the opening 0x02 too
                if (end < 0) end = 0;
            }
        }
    }
    if (end > 0) {
        ssize_t wr = write(1, out, (size_t)end);
        (void)wr;
    }
    return status;
}

// end (satoru)
