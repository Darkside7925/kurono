// kui_r3_launch.cpp: ring-3 side of kui::Launch, a validated one-shot request to the desktop (satoru)
// over the /system/run/user/1000/kui-launch unix socket. the desktop re-checks every field and (satoru)
// never runs a shell command from here, only registered apps, file opens and .kcl scripts. (satoru)
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "../kui_internal.h"
#include "../kui_launch_proto.h"

namespace {

bool request(uint32_t kind, const char* name, const char* arg) {
    KuiLaunchMsg m;
    m.magic = KUI_LAUNCH_MAGIC;
    m.kind = kind;
    m.name_len = 0;
    m.arg_len = 0;
    for (int i = 0; i < KUI_LAUNCH_NAME_MAX; i++) m.name[i] = 0;
    for (int i = 0; i < KUI_LAUNCH_ARG_MAX; i++) m.arg[i] = 0;
    if (name) {
        int i = 0;
        while (name[i] && i < KUI_LAUNCH_NAME_MAX - 1) { m.name[i] = name[i]; i++; }
        m.name[i] = 0;
        m.name_len = (uint32_t)i;
    }
    if (arg) {
        int i = 0;
        while (arg[i] && i < KUI_LAUNCH_ARG_MAX - 1) { m.arg[i] = arg[i]; i++; }
        m.arg[i] = 0;
        m.arg_len = (uint32_t)i;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    const char* path = KUI_LAUNCH_PATH;
    int i = 0;
    while (path[i] && i < (int)sizeof(sa.sun_path) - 1) { sa.sun_path[i] = path[i]; i++; }
    sa.sun_path[i] = 0;
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(fd);
        return false;
    }
    const uint8_t* p = (const uint8_t*)&m;
    int left = (int)sizeof(m);
    while (left > 0) {
        ssize_t n = send(fd, p, (size_t)left, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        p += n;
        left -= (int)n;
    }
    struct pollfd pf = { fd, POLLIN, 0 };
    if (poll(&pf, 1, 500) <= 0) { close(fd); return false; }
    KuiLaunchReply rp;
    uint8_t* q = (uint8_t*)&rp;
    int need = (int)sizeof(rp);
    int got = 0;
    while (got < need) {
        ssize_t n = recv(fd, q + got, (size_t)(need - got), 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        got += (int)n;
    }
    close(fd);
    return got == need && rp.magic == KUI_LAUNCH_REPLY && rp.ok != 0;
}

}  // namespace

namespace kui {

namespace Launch {

bool App(const char* name, const char* arg) {
    if (!kui_launch_app_ok(name)) return false;
    if (arg && arg[0] && !kui_launch_path_ok(arg)) return false;
    return request(KUI_LAUNCH_APP, name, (arg && arg[0]) ? arg : nullptr);
}

bool Open(const char* path) {
    if (!kui_launch_path_ok(path)) return false;
    return request(KUI_LAUNCH_OPEN, nullptr, path);
}

bool RunScript(const char* path) {
    if (!kui_launch_script_ok(path)) return false;
    return request(KUI_LAUNCH_KCL, nullptr, path);
}

bool RefreshDesktop() { return request(KUI_LAUNCH_REFRESH, nullptr, nullptr); }

}  // namespace Launch

}  // namespace kui

// end (satoru)
