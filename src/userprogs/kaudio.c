// kaudio - ring-3 control tool for the kurono audio server.
//
// writes one command to /dev/kaudio, the audio-named alias of the /dev/kshell
// rescue channel, and prints the kernel's reply. this is how a linux program
// (or the claude agent's bash) restarts or stops the audio services without a
// reboot: the audio server lives in the kernel, so kill/signal cannot reach it.
//
// usage: kaudio [status|restart|stop|use usb|use hda]
// end (satoru)
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

static const char* kPaths[] = {
    "/dev/kaudio",
    "/system/dev/kaudio",
    NULL
};

int main(int argc, char** argv) {
    const char* sub = argc > 1 ? argv[1] : "status";
    char line[160];

    if (!strcmp(sub, "restart") || !strcmp(sub, "stop") || !strcmp(sub, "status") ||
        !strcmp(sub, "tone")) {
        snprintf(line, sizeof(line), "audio %s", sub);
    } else if (!strcmp(sub, "use")) {
        if (argc < 3) {
            fprintf(stderr, "usage: kaudio use usb|hda\n");
            return 2;
        }
        snprintf(line, sizeof(line), "audio use %s", argv[2]);
    } else if (!strncmp(sub, "audio ", 6)) {
        snprintf(line, sizeof(line), "%s", sub);
    } else {
        fprintf(stderr, "usage: kaudio [status|restart|stop|use usb|use hda]\n");
        return 2;
    }

    int fd = -1;
    for (int i = 0; kPaths[i]; i++) {
        fd = open(kPaths[i], O_RDWR);
        if (fd >= 0) break;
    }
    if (fd < 0) {
        fprintf(stderr, "kaudio: cannot open /dev/kaudio (%s)\n", strerror(errno));
        return 1;
    }

    size_t len = strlen(line);
    if (write(fd, line, len) != (ssize_t)len) {
        fprintf(stderr, "kaudio: write failed (%s)\n", strerror(errno));
        close(fd);
        return 1;
    }

    char buf[512];
    ssize_t n;
    int any = 0;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        fwrite(buf, 1, (size_t)n, stdout);
        any = 1;
    }
    if (!any) fprintf(stderr, "kaudio: no reply\n");
    close(fd);
    return 0;
}

// end (satoru)
