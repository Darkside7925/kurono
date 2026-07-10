#include "firefox_launcher.h"
#include "../fs/kvfs.h"
#include "../linux/linux_syscall.h"
#include "../drivers/serial.h"

namespace {

bool path_exists(const char* p) {
    return KVFS::Resolve(p) != nullptr;
}

}  // namespace

namespace FirefoxLauncher {

bool IsInstalled() {
    return path_exists("/apps/firefox/firefox") &&
           path_exists("/system/lib/firefox-deps.manifest");
}

int LoadEnvironment(char** out_envp, char* out_buf, int buf_size) {
    if (!out_envp || !out_buf) return 0;
    KVFSNode* n = KVFS::Resolve("/home/user/.config/kurono/firefox.env");
    if (!n || n->type != KVFS_FILE || n->size == 0) return 0;
    int copy = (int)n->size;
    if (copy > buf_size - 1) copy = buf_size - 1;
    for (int i = 0; i < copy; i++) out_buf[i] = (char)n->content[i];
    out_buf[copy] = 0;

    int count = 0;
    int p = 0;
    while (p < copy && count < 32) {
        // Skip blank lines / comments.
        while (p < copy && (out_buf[p] == '\n' || out_buf[p] == '\r' ||
                            out_buf[p] == ' '  || out_buf[p] == '\t'))
            p++;
        if (p >= copy) break;
        if (out_buf[p] == '#') {
            while (p < copy && out_buf[p] != '\n') p++;
            continue;
        }
        out_envp[count++] = &out_buf[p];
        while (p < copy && out_buf[p] != '\n' && out_buf[p] != '\r') p++;
        if (p < copy) out_buf[p++] = 0;
    }
    out_envp[count] = nullptr;
    return count;
}

int Launch(uint32_t uid, uint32_t gid, const char* url) {
    if (!IsInstalled()) {
        SerialLogger::Log("[FirefoxLauncher] /apps/firefox/firefox not installed; "
                          "use kpkg install firefox-esr first.\r\n");
        return -2;
    }

    int pid = LinuxSyscall::CreateProcess("firefox", uid, gid);
    if (pid < 0) {
        SerialLogger::Log("[FirefoxLauncher] CreateProcess failed\r\n");
        return pid;
    }

    char envbuf[4096];
    char* envp[33];
    int env_count = LoadEnvironment(envp, envbuf, sizeof(envbuf));
    SerialLogger::Log("[FirefoxLauncher] loaded ");
    SerialLogger::LogDec(env_count);
    SerialLogger::Log(" env vars\r\n");

    // The actual exec - pass argv = ["firefox", url?, NULL].
    const char* argv0 = "/apps/firefox/firefox";
    const char* argv[3] = { argv0, url, nullptr };
    if (!url) { argv[1] = nullptr; }

    // We invoke execve through the public Dispatch entrypoint.
    int32_t r = LinuxSyscall::Dispatch(
        LSYS_EXECVE,
        (uint32_t)(uintptr_t)argv0,
        (uint32_t)(uintptr_t)argv,
        (uint32_t)(uintptr_t)envp,
        0, 0);
    if (r < 0) {
        SerialLogger::Log("[FirefoxLauncher] execve failed: ");
        SerialLogger::LogDec(r);
        SerialLogger::Log("\r\n");
        return r;
    }
    SerialLogger::Log("[FirefoxLauncher] firefox launched pid=");
    SerialLogger::LogDec(pid);
    SerialLogger::Log("\r\n");
    return pid;
}

}  // namespace FirefoxLauncher
