#include "linux_sync.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int run_cmd(const char* cmd) {
    int rc = system(cmd);
    return rc;
}

bool linux_sync_create_user(const char* username, bool is_admin) {
    if (!username || strlen(username) == 0) return false;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "wsl -d KuronoLinux sh -lc 'id -u %s >/dev/null 2>&1 || (adduser -D %s %s)'",
             username, username, is_admin ? "; addgroup %s wheel" : "");
    return run_cmd(cmd) == 0;
}

bool linux_sync_delete_user(const char* username) {
    if (!username || strlen(username) == 0) return false;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "wsl -d KuronoLinux sh -lc 'id -u %s >/dev/null 2>&1 && deluser %s || true'",
             username, username);
    return run_cmd(cmd) == 0;
}

bool linux_sync_sync_from_linux(void) {
    char cmd[512];
    const char* base = getenv("KURONO_BASE");
    if (!base || strlen(base) == 0) base = "D:\\OS\\Kurono OS";
    snprintf(cmd, sizeof(cmd), "wsl -d KuronoLinux sh -lc 'cat /etc/passwd' > \"%s\\Users\\linux_passwd.txt\"", base);
    return run_cmd(cmd) == 0;
}