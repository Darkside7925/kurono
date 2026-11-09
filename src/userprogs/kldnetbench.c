// kldnetbench - the KLD phase-C network A/B instrument (kurono.kld.bench=1).
//
// Runs the same measurements against whichever driver backs eth0, from ring 3,
// through Kurono's ordinary linux socket layer:
//   --spawn PATH   first start the KLD driver container as a child (KLD mode);
//                  without it the native driver is measured.
//   readiness      time until the first HTTP GET to the host succeeds
//   latency        N sequential HTTP/1.0 GETs of a small file, one TCP
//                  connection each (connect + request + full response)
//   throughput     GET of a large file, bytes / wall time
//   https          busybox wget of an https url (TLS through the same nic)
//
// Kurono sockets are NON-BLOCKING (connect -> EINPROGRESS, recv -> EAGAIN), so
// every call here goes through poll(). Output is [kldbench] lines on stderr
// (com1). (satoru)
//
// end (satoru)

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>

#include <bearssl.h>
#include "ds_tas.h"   // the 5 trimmed ca roots dsai uses (isrg x1 among them) (satoru)

extern char **environ;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    int n = snprintf(b, sizeof(b), "[kldbench] ");
    va_start(ap, fmt);
    n += vsnprintf(b + n, sizeof(b) - (size_t)n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(b) - 2) n = (int)sizeof(b) - 2;
    b[n++] = '\n';
    (void)!write(2, b, (size_t)n);
}

static int wait_fd(int fd, short ev, int ms)
{
    struct pollfd p = { .fd = fd, .events = ev, .revents = 0 };
    return poll(&p, 1, ms);
}

// GET http://ip:port/path. returns body bytes (>= 0) or -1. (satoru)
static long http_get(const char *ip, int port, const char *path, int timeout_ms)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    double t_end = now_ms() + timeout_ms;
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0 && errno != EINPROGRESS && errno != EAGAIN) {
        close(s);
        return -1;
    }
    char req[256];
    int rl = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, ip);
    int sent = 0;
    while (sent < rl) {
        if (now_ms() > t_end) { close(s); return -1; }
        ssize_t w = send(s, req + sent, (size_t)(rl - sent), 0);
        if (w > 0) { sent += (int)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EINPROGRESS || errno == ENOTCONN || errno == EWOULDBLOCK)) {
            wait_fd(s, POLLOUT, 50);
            continue;
        }
        close(s);
        return -1;
    }
    static char buf[65536];
    long total = 0, body = -1;
    int hdr_state = 0;   // matches "\r\n\r\n" across reads (satoru)
    for (;;) {
        if (now_ms() > t_end) { close(s); return -1; }
        ssize_t r = recv(s, buf, sizeof(buf), 0);
        if (r > 0) {
            total += r;
            if (body < 0) {
                for (ssize_t i = 0; i < r; i++) {
                    char c = buf[i];
                    if ((hdr_state == 0 || hdr_state == 2) && c == '\r') hdr_state++;
                    else if ((hdr_state == 1 || hdr_state == 3) && c == '\n') hdr_state++;
                    else hdr_state = (c == '\r') ? 1 : 0;
                    if (hdr_state == 4) { body = (long)(r - i - 1); break; }
                }
            } else {
                body += r;
            }
            continue;
        }
        if (r == 0)
            break;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOTCONN) {
            wait_fd(s, POLLIN, 200);
            continue;
        }
        break;
    }
    close(s);
    return body;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

// fork + execve: the path Kurono's Linux layer exercises most (firefox's
// content processes). musl's posix_spawn clone(CLONE_VFORK) path returned
// ECHILD here. (satoru)
static pid_t spawn(char *const argv[])
{
    pid_t pid = fork();
    if (pid == 0) {
        execve(argv[0], argv, environ);
        _exit(127);
    }
    return pid;
}

// ── https: bearssl with full chain validation (the dsai client's setup) ─────
static int bs_fd;
static int bs_read(void *ctx, unsigned char *b, size_t len)
{
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t n = read(fd, b, len);
        if (n > 0) return (int)n;
        if (n == 0) return -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) { if (wait_fd(fd, POLLIN, 20000) <= 0) return -1; continue; }
        return -1;
    }
}
static int bs_write(void *ctx, const unsigned char *b, size_t len)
{
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t n = write(fd, b, len);
        if (n > 0) return (int)n;
        if (n == 0) return -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOTCONN) { if (wait_fd(fd, POLLOUT, 20000) <= 0) return -1; continue; }
        return -1;
    }
}

// returns body+header bytes read, sets *status. -1 on failure, -2 tls error. (satoru)
static long https_get(const char *host, const char *path, int *status, int *tls_err, double *t_dns)
{
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    double t0 = now_ms();
    if (getaddrinfo(host, "443", &hints, &ai) != 0 || !ai) return -1;
    *t_dns = now_ms() - t0;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(ai); return -1; }
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) != 0 && errno != EINPROGRESS && errno != EAGAIN) {
        freeaddrinfo(ai); close(fd); return -1;
    }
    freeaddrinfo(ai);
    static br_ssl_client_context sc;
    static br_x509_minimal_context xc;
    static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    br_sslio_context ioc;
    br_ssl_client_init_full(&sc, &xc, TAs, TAs_NUM);
    time_t now = time(NULL);
    br_x509_minimal_set_time(&xc, (uint32_t)(now / 86400) + 719528u, (uint32_t)(now % 86400));
    unsigned char seed[32];
    int rfd = open("/dev/urandom", O_RDONLY);
    ssize_t got = rfd >= 0 ? read(rfd, seed, sizeof(seed)) : 0;
    if (rfd >= 0) close(rfd);
    if (got < (ssize_t)sizeof(seed)) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); memcpy(seed, &ts, sizeof(ts)); }
    br_ssl_engine_inject_entropy(&sc.eng, seed, sizeof(seed));
    br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof(iobuf), 1);
    br_ssl_client_reset(&sc, host, 0);
    bs_fd = fd;
    br_sslio_init(&ioc, &sc.eng, bs_read, &bs_fd, bs_write, &bs_fd);
    char req[256];
    int rl = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: kurono-kldnetbench\r\nConnection: close\r\n\r\n", path, host);
    long total = 0;
    if (br_sslio_write_all(&ioc, req, (size_t)rl) == 0 && br_sslio_flush(&ioc) == 0) {
        static unsigned char buf[16384];
        int n;
        while ((n = br_sslio_read(&ioc, buf, sizeof(buf))) > 0) {
            if (total == 0 && n > 12) *status = atoi((const char *)buf + 9);
            total += n;
        }
    }
    *tls_err = br_ssl_engine_last_error(&sc.eng);
    close(fd);
    return total > 0 ? total : -2;
}

int main(int argc, char **argv)
{
    const char *host = "10.0.2.2";
    int port = 8000;
    const char *mode = "native";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--spawn") && i + 1 < argc) {
            char *cav[] = { argv[i + 1], NULL };
            mode = "kld";
            pid_t pid = spawn(cav);
            if (pid < 0) {
                say("FAIL spawn %s errno=%d", argv[i + 1], errno);
                return 2;
            }
            say("spawned driver container %s pid %d", argv[i + 1], (int)pid);
            i++;
        }
    }
    say("mode=%s target=%s:%d", mode, host, port);

    // readiness: dhcp + link + first successful http round trip. (satoru)
    double t0 = now_ms();
    long b = -1;
    while (now_ms() - t0 < 120000.0) {
        b = http_get(host, port, "/hello.txt", 3000);
        if (b > 0)
            break;
        usleep(500000);
    }
    if (b <= 0) {
        say("FAIL no http after %.0f ms", now_ms() - t0);
        return 3;
    }
    say("ready: first http GET ok after %.0f ms (%ld body bytes)", now_ms() - t0, b);

    // latency. (satoru)
    enum { N = 30 };
    double lat[N];
    int ok = 0;
    for (int i = 0; i < N; i++) {
        double s = now_ms();
        long r = http_get(host, port, "/hello.txt", 5000);
        double e = now_ms() - s;
        if (r > 0)
            lat[ok++] = e;
    }
    if (ok) {
        double sum = 0;
        for (int i = 0; i < ok; i++) sum += lat[i];
        qsort(lat, (size_t)ok, sizeof(double), cmp_d);
        say("latency http GET x%d (ok %d): min %.2f p50 %.2f p95 %.2f max %.2f mean %.2f ms",
            N, ok, lat[0], lat[ok / 2], lat[(ok * 95) / 100 >= ok ? ok - 1 : (ok * 95) / 100], lat[ok - 1], sum / ok);
    }

    // throughput. (satoru)
    for (int i = 0; i < 3; i++) {
        double s = now_ms();
        long r = http_get(host, port, "/big.bin", 120000);
        double e = now_ms() - s;
        if (r > 0)
            say("throughput GET /big.bin run %d: %ld bytes in %.0f ms = %.2f MB/s", i + 1, r, e, (double)r / 1048576.0 / (e / 1000.0));
        else
            say("throughput run %d FAILED after %.0f ms", i + 1, e);
    }

    // https through the same nic: dns + tcp + tls 1.2 with the certificate
    // chain validated against ISRG Root X1 (bearssl, trust anchors as dsai). (satoru)
    {
        int status = 0, terr = 0;
        double tdns = 0, t1 = now_ms();
        long r = https_get("letsencrypt.org", "/", &status, &terr, &tdns);
        double e = now_ms() - t1;
        say("https GET https://letsencrypt.org/: %ld bytes, HTTP %d, bearssl err %d (0 = chain validated, clean close), dns %.0f ms, total %.0f ms",
            r, status, terr, tdns, e);
    }

    say("DONE mode=%s", mode);
    return 0;
}

// end (satoru)
