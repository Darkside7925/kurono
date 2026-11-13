//  kurono os - the kernel dev loop, one command (see kloop.h) (satoru)
#include "kloop.h"
#include "vm_host.h"
#include "../fs/kvfs.h"
#include "../drivers/serial.h"
#include "../drivers/timer.h"
#include "../proc/scheduler.h"
#include "../drivers/disklog.h"
#include "../kernel/heap.h"

// tools/selfhost/kxfer.c, pasted into the alpine guest and compiled there (satoru)
asm(".section .rodata\n"
    ".global g_kxfer_src\n"
    "g_kxfer_src:\n"
    ".incbin \"../tools/selfhost/kxfer.c\"\n"
    ".byte 0\n"
    ".previous\n");
extern "C" const char g_kxfer_src[];
// tools/selfhost/kloop_build.sh, the incremental guest build (satoru)
asm(".section .rodata\n"
    ".global g_kbuild_sh\n"
    "g_kbuild_sh:\n"
    ".incbin \"../tools/selfhost/kloop_build.sh\"\n"
    ".byte 0\n"
    ".previous\n");
extern "C" const char g_kbuild_sh[];

namespace {
    const char* const SHARE = "/home/user/vmshare";
    const char* const LOG   = "/home/user/vmshare/kloop.log";
    char* g_out; int g_p; int g_mx;

    int slen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
    void say(const char* s) {
        SerialLogger::Log("[kloop] "); SerialLogger::Log(s); SerialLogger::Log("\r\n");
        KVFS::AppendFile(LOG, s, (uint32_t)slen(s));
        KVFS::AppendFile(LOG, "\n", 1);
        for (int i = 0; s[i] && g_p < g_mx - 2; i++) g_out[g_p++] = s[i];
        if (g_p < g_mx - 1) g_out[g_p++] = '\n';
        g_out[g_p] = 0;
    }
    void sayn(const char* a, uint64_t v, const char* b = "") {
        char t[160]; int k = 0;
        for (int i = 0; a[i] && k < 120; i++) t[k++] = a[i];
        char d[24]; int n = 0;
        if (!v) d[n++] = '0';
        while (v && n < 22) { d[n++] = (char)('0' + v % 10); v /= 10; }
        while (n) t[k++] = d[--n];
        for (int i = 0; b[i] && k < 158; i++) t[k++] = b[i];
        t[k] = 0;
        say(t);
    }
    uint64_t now_s() { return Timer::GetRealMs64() / 1000; }

    // run a guest command; the output tail goes to the log (satoru)
    char g_gout[4096];   // the last guest command's output (satoru)
    int  g_gout_n;
    int gexec(const char* cmd, uint32_t timeout_s, bool show = true) {
        char* out = g_gout;
        int rc = -1;
        int n = VMHost::Exec(cmd, out, sizeof(g_gout) - 1, timeout_s * 1000u, &rc);
        g_gout_n = n > 0 ? n : 0;
        g_gout[g_gout_n] = 0;
        if (n < 0) { say("  guest command timed out or the agent is gone"); return -1000; }
        if ((show || rc != 0) && n > 0) {
            int from = n > 600 ? n - 600 : 0;
            KVFS::AppendFile(LOG, out + from, (uint32_t)(n - from));
            KVFS::AppendFile(LOG, "\n", 1);
        }
        if (rc != 0 && n > 0) {
            // a failing step shows its output tail on the serial log too (satoru)
            int from = n > 400 ? n - 400 : 0;
            SerialLogger::Log("[kloop] guest output: "); SerialLogger::Log(out + from); SerialLogger::Log("\r\n");
        }
        return rc;
    }

    const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    // copy a kernel string into a guest file through the agent, base64 in
    // 360-char pieces (an agent line is 512 bytes) (satoru)
    bool push_text(const char* text, const char* guest_path) {
        char cmd[512];
        int p = 0;
        const char* pre = "rm -f /root/.kl.b64";
        while (*pre) cmd[p++] = *pre++;
        cmd[p] = 0;
        if (gexec(cmd, 30, false) != 0) return false;
        int n = slen(text);
        int i = 0;
        while (i < n) {
            p = 0;
            const char* a = "printf %s '";
            while (*a) cmd[p++] = *a++;
            int chunk = 0;
            while (i < n && chunk < 360) {
                uint32_t v = (uint8_t)text[i] << 16;
                int have = 1;
                if (i + 1 < n) { v |= (uint8_t)text[i + 1] << 8; have++; }
                if (i + 2 < n) { v |= (uint8_t)text[i + 2]; have++; }
                cmd[p++] = B64[(v >> 18) & 63];
                cmd[p++] = B64[(v >> 12) & 63];
                cmd[p++] = have > 1 ? B64[(v >> 6) & 63] : '=';
                cmd[p++] = have > 2 ? B64[v & 63] : '=';
                i += 3; chunk += 4;
            }
            const char* z = "' >> /root/.kl.b64";
            while (*z) cmd[p++] = *z++;
            cmd[p] = 0;
            if (gexec(cmd, 30, false) != 0) return false;
        }
        p = 0;
        const char* d = "base64 -d /root/.kl.b64 > ";
        while (*d) cmd[p++] = *d++;
        for (int k = 0; guest_path[k]; k++) cmd[p++] = guest_path[k];
        cmd[p] = 0;
        return gexec(cmd, 30, false) == 0;
    }

    // the guest's own clock at its last log line: every kurono log line
    // starts "[<ms since boot>]". "still running" alone hid a guest that had
    // been asleep since second 10 (its timer never ticked again) (satoru)
    uint64_t guest_uptime_ms(const char* path) {
        static char buf[8192];
        int sz = KVFS::GetFileSize(path);
        if (sz <= 0) return 0;
        uint32_t off = sz > 8192 ? (uint32_t)sz - 8192 : 0;
        int got = KVFS::ReadFileAt(path, off, buf, 8192);
        uint64_t best = 0;
        for (int i = 0; i < got; i++) {
            if (buf[i] != '[' || (i && buf[i - 1] != '\n')) continue;
            uint64_t v = 0; int k = i + 1, nd = 0;
            while (k < got && buf[k] >= '0' && buf[k] <= '9' && nd < 12) { v = v * 10 + (uint64_t)(buf[k] - '0'); k++; nd++; }
            if (nd && k < got && buf[k] == ']' && v > best) best = v;
        }
        return best;
    }

    bool wait_state(VMHost::State want, uint32_t secs) {
        uint64_t t0 = now_s();
        while (VMHost::GetState() != want) {
            if (now_s() - t0 > secs) return false;
            Scheduler::SleepMs(200);
        }
        return true;
    }
}

int KLoop::Run(uint32_t boot_secs, uint32_t ram_mb, bool clean, const char* guest_cmdline, char* out, int mx) {
    g_out = out; g_p = 0; g_mx = mx; out[0] = 0;
    KVFS::Mkdirs(SHARE);
    KVFS::WriteFile(LOG, "", 0);
    uint64_t t_all = now_s();
    if (KVFS::GetFileSize("/home/user/vmshare/src.tar") <= 0) {
        // or a KSRC.TAR in the KURONO partition root (dropped there from the
        // other os, or by a test rig) (satoru)
        static const char k83[11] = { 'K','S','R','C',' ',' ',' ',' ','T','A','R' };
        uint64_t sz = 0;
        if (DiskLog::StatRootFile(k83, &sz) == 0 && sz > 0 && sz < (1ull << 30)) {
            uint8_t* b = (uint8_t*)KernelHeap::Alloc(sz);
            if (b && DiskLog::ReadRootFile(k83, b, sz, nullptr) == 0) {
                KVFS::WriteFile("/home/user/vmshare/src.tar", b, (uint32_t)sz);
                sayn("0/5 source taken from KSRC.TAR on the KURONO partition, bytes: ", sz);
            }
            if (b) KernelHeap::Free(b);
        }
    }
    if (KVFS::GetFileSize("/home/user/vmshare/src.tar") <= 0) {
        say("kloop: put the source first: tar of src/ Alpine/ kld/ tools/ logo.h pciids.h as /home/user/vmshare/src.tar");
        return g_p;
    }
    // 1. the build vm (satoru)
    if (VMHost::GetState() == VMHost::VMH_RUNNING && !VMHost::AgentReady()) { VMHost::Kill(); wait_state(VMHost::VMH_OFF, 30); }
    if (!VMHost::AgentReady()) {
        say("1/5 starting the Alpine build vm");
        if (!VMHost::Start(VMHost::DISTRO_ALPINE, 3008)) { say(VMHost::LastError()); return g_p; }
        // Start only queues the request: wait for the host thread to boot it (satoru)
        uint64_t tw = now_s();
        while (VMHost::GetState() != VMHost::VMH_RUNNING && now_s() - tw < 90) {
            if (VMHost::GetState() == VMHost::VMH_FAILED) break;
            Scheduler::SleepMs(200);
        }
        if (VMHost::GetState() != VMHost::VMH_RUNNING) { say("the Alpine vm did not start:"); say(VMHost::LastError()); return g_p; }
        if (!VMHost::WaitAgent(180000)) { say("the Alpine guest agent did not come up"); return g_p; }
        sayn("   agent up after ", now_s() - tw, " s");
    }
    uint64_t t0 = now_s();
    say("2/5 toolchain + kxfer in the guest");
    if (!push_text(g_kxfer_src, "/root/kxfer.c") || !push_text(g_kbuild_sh, "/root/kloop_build.sh")) {
        say("could not copy kxfer.c / kloop_build.sh into the guest"); return g_p;
    }
    // the guest network can still be coming up when the agent answers: retry
    // apk (an install that failed left gcc missing, the rc=127 of run kl3) (satoru)
    int rc = -1;
    for (int tries = 0; tries < 4 && rc != 0; tries++) {
        if (tries) { sayn("   apk add failed, retry ", (uint64_t)tries); Scheduler::SleepMs(10000); }
        rc = gexec("apk add -q build-base nasm rsync 2>&1 | tail -3; command -v gcc && command -v rsync", 900);
    }
    if (rc != 0) { say("could not install build-base in the guest (network?)"); return g_p; }
    sayn("   packages ready in ", now_s() - t0, " s");
    rc = gexec("gcc -O2 -static -o /usr/local/bin/kxfer /root/kxfer.c && "
                   "printf '#!/bin/sh\\nexec gcc \"$@\"\\n' > /usr/local/bin/musl-gcc && chmod +x /usr/local/bin/musl-gcc && "
                   "mkdir -p /usr/lib/x86_64-linux-musl && touch /usr/lib/x86_64-linux-musl/musl-gcc.specs && echo ready", 900);
    if (rc != 0) { sayn("toolchain step failed, rc=", (uint64_t)(rc < 0 ? -rc : rc)); return g_p; }
    sayn("   toolchain ready in ", now_s() - t0, " s");
    // 2. build (satoru)
    t0 = now_s();
    say("3/5 building kurono_base.elf in the guest (log: vmshare/kmake.log)");
    rc = gexec(clean ? "sh /root/kloop_build.sh clean" : "sh /root/kloop_build.sh", 3600);
    for (int k = 0; k + 12 < g_gout_n; k++) {
        // the script's one-line verdict: clean or incremental, files changed (satoru)
        if (k && g_gout[k - 1] != '\n') continue;
        const char* m = "kloop-build:";
        int q = 0;
        while (m[q] && g_gout[k + q] == m[q]) q++;
        if (m[q]) continue;
        char line[120]; int L = 0;
        line[L++] = ' '; line[L++] = ' '; line[L++] = ' ';
        for (int z = k + 13; z < g_gout_n && g_gout[z] != '\n' && L < 118; z++) line[L++] = g_gout[z];
        line[L] = 0;
        say(line);
        break;
    }
    sayn("   make rc=", (uint64_t)(rc < 0 ? 999 : rc));
    sayn("   build took ", now_s() - t0, " s");
    if (rc != 0) { say("build failed: see vmshare/kmake.log"); return g_p; }
    // 3. swap vms (satoru)
    say("4/5 stopping the build vm, booting the new kernel as a guest");
    VMHost::Stop();
    if (!wait_state(VMHost::VMH_OFF, 60)) { VMHost::Kill(); wait_state(VMHost::VMH_OFF, 30); }
    if (!VMHost::StartKurono("/home/user/vmshare/kguest.elf", ram_mb ? ram_mb : 2048,
                             guest_cmdline && *guest_cmdline ? guest_cmdline : "kurono.autologin=1")) {
        say(VMHost::LastError()); return g_p;
    }
    if (!wait_state(VMHost::VMH_RUNNING, 60)) { say("the guest kernel did not start (see vmshare/kguest.log)"); return g_p; }
    t0 = now_s();
    while (now_s() - t0 < boot_secs && VMHost::GetState() == VMHost::VMH_RUNNING) Scheduler::SleepMs(500);
    bool alive = VMHost::GetState() == VMHost::VMH_RUNNING;
    char m[96];
    if (alive) VMHost::SnapKurono("/home/user/vmshare/kguest.png", m, sizeof(m));
    // 4. verdict from the guest's own log (satoru)
    int lsz = KVFS::GetFileSize("/home/user/vmshare/kguest.log");
    sayn("5/5 guest ran ", now_s() - t0, alive ? " s, still alive" : " s, it STOPPED (crash or triple fault)");
    sayn("   guest log bytes: ", (uint64_t)(lsz > 0 ? lsz : 0));
    uint64_t up = guest_uptime_ms("/home/user/vmshare/kguest.log") / 1000;
    sayn("   guest clock at its last log line: ", up,
         up + 30 >= boot_secs ? " s (alive the whole run)" : " s (the guest STALLED or went quiet early)");
    VMHost::Kill();
    wait_state(VMHost::VMH_OFF, 30);
    sayn("kloop done in ", now_s() - t_all, " s. files: vmshare/kguest.log kguest.png kmake.log kguest.elf kloop.log");
    return g_p;
}

// end (satoru)
