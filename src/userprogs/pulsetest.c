// pulsetest - minimal PulseAudio-native client used to gate the ring-3 audiod
// (kurono.r3audiotest=1). It speaks the same v12 subset libpulse does: AUTH,
// SET_CLIENT_NAME, CREATE_PLAYBACK_STREAM, then writes a sine tone in response
// to the server's REQUESTs, and reports how many bytes were requested/written.
// PASS means the whole path worked: unix socket -> audiod -> kernel mixer.

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>

static void be32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t rbe32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static void logf2(const char* fmt, long a, long b) {
    char m[160]; int n = snprintf(m, sizeof m, fmt, a, b); (void)!write(2, m, (size_t)n);
}

static uint8_t cmd[1024]; static int cl;
static void u32(uint32_t v) { cmd[cl++] = 'L'; be32(cmd + cl, v); cl += 4; }

static void bol(int v) { cmd[cl++] = v ? '1' : '0'; }
static void spec(uint8_t f, uint8_t c, uint32_t r) { cmd[cl++] = 'a'; cmd[cl++] = f; cmd[cl++] = c; be32(cmd + cl, r); cl += 4; }
static int fd;
static void send_cmd(uint32_t chan) {
    uint8_t h[20]; memset(h, 0, 20); be32(h, (uint32_t)cl); be32(h + 4, chan);
    (void)!write(fd, h, 20); (void)!write(fd, cmd, (size_t)cl);
}
// read one packet; returns payload length or -1
static uint8_t rx[16384]; static int rxl;
static int next_packet(uint8_t** payload, uint32_t* chan, int wait_ms) {
    for (;;) {
        if (rxl >= 20) {
            uint32_t plen = rbe32(rx);
            if (rxl >= (int)(20 + plen)) {
                *payload = rx + 20; *chan = rbe32(rx + 4);
                return (int)plen;
            }
        }
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, wait_ms) <= 0) return -1;
        ssize_t n = read(fd, rx + rxl, sizeof rx - (size_t)rxl);
        if (n <= 0) return -1;
        rxl += (int)n;
    }
}
static void consume(int plen) { memmove(rx, rx + 20 + plen, (size_t)(rxl - 20 - plen)); rxl -= 20 + plen; }

static int expect_reply(uint32_t tag) {
    for (int i = 0; i < 50; i++) {
        uint8_t* p; uint32_t ch;
        int n = next_packet(&p, &ch, 2000);
        if (n < 0) return -1;
        uint32_t c = rbe32(p + 1), t = rbe32(p + 6);
        consume(n);
        if (c == 2 && t == tag) return 0;
        if (c == 0 && t == tag) return -2;
    }
    return -1;
}

int main(int argc, char** argv) {
    int rate = argc > 1 ? atoi(argv[1]) : 44100;
    int secs = argc > 2 ? atoi(argv[2]) : 3;
    double hz = argc > 3 ? atof(argv[3]) : 440.0;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa; memset(&sa, 0, sizeof sa); sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, "/system/run/user/1000/pulse/native");
    int tries = 0;
    while (connect(fd, (struct sockaddr*)&sa, sizeof sa) < 0) {
        if (++tries > 40) { logf2("[pulsetest] connect failed errno=%ld%ld\r\n", errno, 0); return 1; }
        usleep(250000);
        close(fd); fd = socket(AF_UNIX, SOCK_STREAM, 0);
    }
    cl = 0; u32(8); u32(1); u32(12); cmd[cl++] = 'x'; be32(cmd + cl, 4); cl += 4; memset(cmd + cl, 0, 4); cl += 4;
    send_cmd(0xFFFFFFFFu);
    if (expect_reply(1)) { logf2("[pulsetest] auth failed\r\n%ld%ld", 0, 0); return 2; }
    // minimal proplist: 'P' + terminating NULL string tag
    cl = 0; u32(9); u32(2); cmd[cl++] = 'P'; cmd[cl++] = 'N';
    send_cmd(0xFFFFFFFFu);
    int rc = expect_reply(2);
    if (rc) { logf2("[pulsetest] set_client_name rc=%ld\r\n%ld", rc, 0); }
    // create playback stream (mono s16, `rate`)
    cl = 0; u32(3); u32(3);
    spec(3, 1, (uint32_t)rate);
    cmd[cl++] = 'm'; cmd[cl++] = 1; cmd[cl++] = 0;          // channel map: mono
    u32(0xFFFFFFFFu);                                       // sink index
    cmd[cl++] = 'N';                                        // sink name (null)
    u32(0xFFFFFFFFu);                                       // maxlength
    bol(0);                                                 // corked
    u32((uint32_t)(rate * 2 / 8));                          // tlength = 125ms
    u32(0xFFFFFFFFu); u32((uint32_t)(rate * 2 / 50)); u32(0);   // prebuf, minreq, syncid
    cmd[cl++] = 'v'; cmd[cl++] = 1; be32(cmd + cl, 0x10000); cl += 4;   // cvolume
    send_cmd(0xFFFFFFFFu);
    uint8_t* p; uint32_t ch;
    int n = next_packet(&p, &ch, 3000);
    if (n < 0 || rbe32(p + 1) != 2) { logf2("[pulsetest] create stream failed n=%ld\r\n%ld", n, 0); return 3; }
    uint32_t chan = rbe32(p + 11), missing = rbe32(p + 21);
    consume(n);
    logf2("[pulsetest] stream channel=%ld missing=%ld\r\n", chan, missing);

    uint64_t want = missing, sent = 0, total = (uint64_t)rate * 2 * (uint64_t)secs;
    uint64_t reqs = 0; int idle = 0;
    double ph = 0; double inc = 2.0 * M_PI * hz / rate;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    while (sent < total) {
        while (want > 0 && sent < total) {
            uint32_t chunk = want > 4096 ? 4096 : (uint32_t)want;
            if (chunk > total - sent) chunk = (uint32_t)(total - sent);
            chunk &= ~1u; if (!chunk) break;
            static uint8_t pkt[20 + 4096]; int16_t* s = (int16_t*)(pkt + 20);
            for (uint32_t i = 0; i < chunk / 2; i++) { s[i] = (int16_t)(sin(ph) * 20000.0); ph += inc; }
            memset(pkt, 0, 20); be32(pkt, chunk); be32(pkt + 4, chan);
            (void)!write(fd, pkt, 20 + chunk);
            sent += chunk; want -= chunk;
        }
        int w = next_packet(&p, &ch, 1000);
        if (w < 0) {
            if (++idle >= 5) { logf2("[pulsetest] stalled sent=%ld want=%ld\r\n", (long)sent, (long)want); break; }
            continue;
        }
        idle = 0;
        if (ch == 0xFFFFFFFFu && rbe32(p + 1) == 61) { want += rbe32(p + 16); reqs++; }
        consume(w);
    }
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    logf2("[pulsetest] sent=%ld bytes in %ld ms\r\n", (long)sent, ms);
    logf2("[pulsetest] requests=%ld expect_ms=%ld\r\n", (long)reqs, (long)secs * 1000);
    sleep(1);
    close(fd);
    // pacing proof: the server must have throttled us to ~realtime (hardware rate)
    int pass = sent >= total && ms >= (long)secs * 1000 - 600 && reqs > 3;
    logf2(pass ? "[pulsetest] PASS%ld%ld\r\n" : "[pulsetest] FAIL%ld%ld\r\n", 0, 0);
    return pass ? 0 : 4;
}
