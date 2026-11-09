// kfetch - tiny https/http downloader for kurono's package manager (satoru)
//
// kpkg's in-kernel http client speaks plain http only. anthropic's release
// storage (downloads.claude.ai) is https-only, so kpkg runs this ring 3
// helper: musl-static + bearssl, the same stack the dsai recovery client
// already proved works on kurono. it fetches ONE url, follows redirects,
// streams the body into a file and prints a sha256 of what it wrote so the
// caller can verify before anything is installed. no credentials of any kind
// are read, sent or logged; the request carries only a host and a path.
//
//   kfetch [-q] [-r FIRST-LAST] URL OUT
//
//   -r asks for a byte range (inclusive) and insists on a 206 answer, so a
//   huge file can be pulled in slices and each slice retried on its own.
//
//   exit 0   body complete (content-length honoured), status 200
//   exit 2   usage / bad url        exit 3  dns or connect failed
//   exit 4   tls failed             exit 5  http status not 200
//   exit 6   body short or io error
//
// stdout, last line on success:  kfetch: ok bytes=N sha256=<64 hex>
// env: KFETCH_HOST_IP=<ipv4>  skip dns (testing, rescue nets), tls still
//      validates the real hostname.
//
//   musl-gcc -static -no-pie -O2 -o kfetch kfetch.c -idirafter third_party/bearssl/inc
//     -idirafter /usr/include third_party/bearssl/libbearssl.a

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <ctype.h>
#include <poll.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "bearssl.h"
#include "kf_tas.h"

#define WAIT_MS   30000
#define MAX_HOP   4

// ---------------------------------------------------------------- tcp

static int connect_finish(int fd)
{
    struct pollfd pfd;
    pfd.fd = fd; pfd.events = POLLOUT; pfd.revents = 0;
    if (poll(&pfd, 1, WAIT_MS) <= 0) return -1;
    int err = 0;
    socklen_t elen = sizeof err;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0) return -1;
    if (err) { errno = err; return -1; }
    return 0;
}

static int try_connect(int fd, const struct sockaddr *sa, socklen_t slen)
{
    if (connect(fd, sa, slen) == 0) return 0;
    if (errno == EINPROGRESS || errno == EALREADY || errno == EAGAIN)
        return connect_finish(fd);
    return -1;
}

static int host_connect(const char *host, const char *port)
{
    const char *ip = getenv("KFETCH_HOST_IP");
    if (ip && *ip) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((unsigned short)atoi(port));
        if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) return -1;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        if (try_connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return -1; }
        return fd;
    }
    struct addrinfo hints, *ai = NULL, *p;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, port, &hints, &ai);
    if (rc) {
        fprintf(stderr, "kfetch: dns lookup of %s failed: %s\n", host, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (p = ai; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (try_connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0) {
        fprintf(stderr, "kfetch: could not connect to %s:%s\n", host, port);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

// kurono's sockets are non-blocking: connect gives EINPROGRESS, read and
// write give EAGAIN until ready. wait for readiness, never treat as fatal. (satoru)
static int sock_wait(int fd, short ev)
{
    struct pollfd pfd;
    pfd.fd = fd; pfd.events = ev; pfd.revents = 0;
    return poll(&pfd, 1, WAIT_MS) > 0 ? 0 : -1;
}

static int sock_read(void *ctx, unsigned char *b, size_t len)
{
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t n = read(fd, b, len);
        if (n > 0) return (int)n;
        if (n == 0) return -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (sock_wait(fd, POLLIN) < 0) { fprintf(stderr, "kfetch: timed out waiting for data\n"); return -1; }
            continue;
        }
        return -1;
    }
}

static int sock_write(void *ctx, const unsigned char *b, size_t len)
{
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t n = write(fd, b, len);
        if (n > 0) return (int)n;
        if (n == 0) return -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOTCONN) {
            if (sock_wait(fd, POLLOUT) < 0) return -1;
            continue;
        }
        return -1;
    }
}

static void seed_entropy(br_ssl_client_context *sc)
{
    unsigned char seed[64];
    size_t have = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, seed, sizeof seed);
        if (n > 0) have = (size_t)n;
        close(fd);
    }
    if (have < sizeof seed) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        unsigned long long tsc = 0;
#if defined(__x86_64__)
        unsigned int lo, hi;
        __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
        tsc = ((unsigned long long)hi << 32) | lo;
#endif
        unsigned long long mix[4];
        mix[0] = (unsigned long long)ts.tv_sec;
        mix[1] = (unsigned long long)ts.tv_nsec;
        mix[2] = tsc;
        mix[3] = (unsigned long long)getpid();
        size_t room = sizeof seed - have;
        size_t take = room < sizeof mix ? room : sizeof mix;
        memcpy(seed + have, mix, take);
        have += take;
    }
    br_ssl_engine_inject_entropy(&sc->eng, seed, have);
}

// ---------------------------------------------------------------- connection

typedef struct {
    int tls;
    int fd;
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    br_sslio_context ioc;
} conn_t;

static conn_t g_conn;     // big (iobuf is ~33 kb), keep it off the stack

static int conn_open(conn_t *c, int tls, const char *host, const char *port)
{
    c->tls = tls;
    c->fd = host_connect(host, port);
    if (c->fd < 0) return 3;
    if (!tls) return 0;

    br_ssl_client_init_full(&c->sc, &c->xc, TAs, TAs_NUM);

    // pin the validation clock. a dead or unset rtc otherwise shows up as
    // BR_ERR_X509_EXPIRED / NOT_YET_VALID, which reads like a broken server
    // certificate and sends you looking in the wrong place. say so plainly,
    // and fall back to the build date when the clock is clearly bogus. (satoru)
    {
        time_t now = time(NULL);
        if (now < 1767225600L) {          // 2026-01-01, older than this build
            fprintf(stderr, "kfetch: system clock reads %ld (implausible), using a fallback date for certificate checks\n", (long)now);
            now = 1793000000L;             // late 2026
        }
        uint32_t days = (uint32_t)(now / 86400) + 719528u;
        uint32_t secs = (uint32_t)(now % 86400);
        br_x509_minimal_set_time(&c->xc, days, secs);
    }

    seed_entropy(&c->sc);
    br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof c->iobuf, 1);
    if (!br_ssl_client_reset(&c->sc, host, 0)) { close(c->fd); return 4; }
    br_sslio_init(&c->ioc, &c->sc.eng, sock_read, &c->fd, sock_write, &c->fd);
    return 0;
}

static int conn_write_all(conn_t *c, const char *s, size_t n)
{
    if (c->tls) {
        if (br_sslio_write_all(&c->ioc, s, n) < 0) return -1;
        return br_sslio_flush(&c->ioc) < 0 ? -1 : 0;
    }
    while (n) {
        int w = sock_write(&c->fd, (const unsigned char *)s, n);
        if (w < 0) return -1;
        s += w; n -= (size_t)w;
    }
    return 0;
}

static int conn_read(conn_t *c, void *b, size_t n)
{
    if (c->tls) return br_sslio_read(&c->ioc, b, n);
    return sock_read(&c->fd, (unsigned char *)b, n);
}

static void conn_close(conn_t *c)
{
    if (c->tls) {
        int err = br_ssl_engine_last_error(&c->sc.eng);
        if (err != 0 && err != BR_ERR_IO)
            fprintf(stderr, "kfetch: tls error %d\n", err);
        br_sslio_close(&c->ioc);
    }
    close(c->fd);
}

// ---------------------------------------------------------------- url

typedef struct { int tls; char host[256]; char port[8]; char path[1024]; } url_t;

static int url_parse(const char *u, url_t *o)
{
    if (!strncmp(u, "https://", 8)) { o->tls = 1; u += 8; strcpy(o->port, "443"); }
    else if (!strncmp(u, "http://", 7)) { o->tls = 0; u += 7; strcpy(o->port, "80"); }
    else return -1;
    size_t i = 0;
    while (*u && *u != '/' && *u != ':' && i < sizeof o->host - 1) o->host[i++] = *u++;
    o->host[i] = 0;
    if (!i) return -1;
    if (*u == ':') {
        u++;
        size_t k = 0;
        while (*u && *u != '/' && k < sizeof o->port - 1) o->port[k++] = *u++;
        o->port[k] = 0;
    }
    if (*u == '/') {
        if (strlen(u) >= sizeof o->path) return -1;
        strcpy(o->path, u);
    } else strcpy(o->path, "/");
    return 0;
}

// ---------------------------------------------------------------- http

// buffered reader over the connection so headers and chunk framing can be read
// a byte at a time without a syscall each. (satoru)
static unsigned char g_rb[16384];
static size_t g_rb_pos, g_rb_len;

static int rb_fill(conn_t *c)
{
    int n = conn_read(c, g_rb, sizeof g_rb);
    if (n <= 0) return -1;
    g_rb_pos = 0; g_rb_len = (size_t)n;
    return 0;
}

static int rb_getc(conn_t *c)
{
    if (g_rb_pos >= g_rb_len && rb_fill(c) < 0) return -1;
    return g_rb[g_rb_pos++];
}

static int rb_line(conn_t *c, char *line, size_t max)
{
    size_t n = 0;
    for (;;) {
        int ch = rb_getc(c);
        if (ch < 0) return -1;
        if (ch == '\n') break;
        if (ch != '\r' && n < max - 1) line[n++] = (char)ch;
    }
    line[n] = 0;
    return (int)n;
}

static unsigned long long g_total;      // bytes written to the output
static unsigned long long g_expect;     // content-length, 0 unknown
static unsigned long long g_next_prog;
static int g_out = -1;
static int g_quiet;
static const char *g_range;     // "first-last" or NULL
static br_sha256_context g_sha;

static int out_write(const unsigned char *p, size_t n)
{
    br_sha256_update(&g_sha, p, n);
    while (n) {
        size_t chunk = n > 65536 ? 65536 : n;
        ssize_t w = write(g_out, p, chunk);
        if (w <= 0) {
            if (w < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            fprintf(stderr, "kfetch: write to output failed: %s\n", strerror(errno));
            return -1;
        }
        p += w; n -= (size_t)w; g_total += (unsigned long long)w;
    }
    if (!g_quiet && g_total >= g_next_prog) {
        printf("kfetch: %llu / %llu MB\n", g_total >> 20, g_expect >> 20);
        fflush(stdout);
        g_next_prog = g_total + (16ull << 20);
    }
    return 0;
}

static int read_body_len(conn_t *c, unsigned long long len)
{
    while (len) {
        if (g_rb_pos >= g_rb_len && rb_fill(c) < 0) return -1;
        size_t avail = g_rb_len - g_rb_pos;
        size_t take = len < avail ? (size_t)len : avail;
        if (out_write(g_rb + g_rb_pos, take) < 0) return -1;
        g_rb_pos += take; len -= take;
    }
    return 0;
}

static int read_body_chunked(conn_t *c)
{
    char line[128];
    for (;;) {
        if (rb_line(c, line, sizeof line) < 0) return -1;
        unsigned long long sz = strtoull(line, NULL, 16);
        if (sz == 0) return 0;
        if (read_body_len(c, sz) < 0) return -1;
        if (rb_line(c, line, sizeof line) < 0) return -1;   // crlf after the chunk
    }
}

static int read_body_close(conn_t *c)
{
    for (;;) {
        if (g_rb_pos >= g_rb_len && rb_fill(c) < 0) return 0;   // eof is the end
        if (out_write(g_rb + g_rb_pos, g_rb_len - g_rb_pos) < 0) return -1;
        g_rb_pos = g_rb_len;
    }
}

// one request. returns 0 done, 1 redirect (loc filled), else an exit code. (satoru)
static int fetch_once(const url_t *u, const char *outpath, char *loc, size_t locmax)
{
    int rc = conn_open(&g_conn, u->tls, u->host, u->port);
    if (rc) return rc;
    char req[1400];
    char rng[64] = "";
    if (g_range) snprintf(rng, sizeof rng, "Range: bytes=%s\r\n", g_range);
    int rl = snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: kurono-kpkg\r\nAccept: */*\r\nAccept-Encoding: identity\r\n%sConnection: close\r\n\r\n",
        u->path, u->host, rng);
    if (conn_write_all(&g_conn, req, (size_t)rl) < 0) { conn_close(&g_conn); return u->tls ? 4 : 3; }

    char line[2048];
    g_rb_pos = g_rb_len = 0;
    if (rb_line(&g_conn, line, sizeof line) < 0) { conn_close(&g_conn); return u->tls ? 4 : 6; }
    int status = 0;
    if (!strncmp(line, "HTTP/", 5)) {
        char *sp = strchr(line, ' ');
        if (sp) status = atoi(sp + 1);
    }
    unsigned long long clen = 0;
    int have_clen = 0, chunked = 0;
    loc[0] = 0;
    for (;;) {
        int n = rb_line(&g_conn, line, sizeof line);
        if (n < 0) { conn_close(&g_conn); return 6; }
        if (n == 0) break;
        if (!strncasecmp(line, "content-length:", 15)) { clen = strtoull(line + 15, NULL, 10); have_clen = 1; }
        else if (!strncasecmp(line, "transfer-encoding:", 18) && strcasestr(line + 18, "chunked")) chunked = 1;
        else if (!strncasecmp(line, "location:", 9)) {
            const char *v = line + 9;
            while (*v == ' ') v++;
            { size_t vl = strlen(v); if (vl >= locmax) vl = locmax - 1; memcpy(loc, v, vl); loc[vl] = 0; }
        }
    }
    if (status >= 300 && status < 400 && loc[0]) { conn_close(&g_conn); return 1; }
    if (status != (g_range ? 206 : 200)) {
        fprintf(stderr, "kfetch: http status %d for %s%s\n", status, u->host, u->path);
        conn_close(&g_conn);
        return 5;
    }

    g_out = open(outpath, O_WRONLY | O_CREAT, 0644);
    if (g_out < 0) {
        fprintf(stderr, "kfetch: cannot open %s: %s\n", outpath, strerror(errno));
        conn_close(&g_conn);
        return 6;
    }
    g_expect = have_clen ? clen : 0;
    g_total = 0; g_next_prog = 0;
    br_sha256_init(&g_sha);
    int br = chunked ? read_body_chunked(&g_conn) : (have_clen ? read_body_len(&g_conn, clen) : read_body_close(&g_conn));
    conn_close(&g_conn);
    close(g_out);
    if (br < 0 || (have_clen && !chunked && g_total != clen)) {
        fprintf(stderr, "kfetch: body incomplete (%llu of %llu bytes)\n", g_total, clen);
        return 6;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int ai = 1;
    for (;;) {
        if (ai < argc && !strcmp(argv[ai], "-q")) { g_quiet = 1; ai++; continue; }
        if (ai + 1 < argc && !strcmp(argv[ai], "-r")) {
            g_range = argv[ai + 1];
            for (const char *q = g_range; *q; q++)
                if (!isdigit((unsigned char)*q) && *q != '-') { fprintf(stderr, "kfetch: bad range\n"); return 2; }
            ai += 2; continue;
        }
        break;
    }
    if (argc - ai != 2) {
        fprintf(stderr, "usage: kfetch [-q] [-r FIRST-LAST] URL OUT\n");
        return 2;
    }
    char cur[1100];
    snprintf(cur, sizeof cur, "%s", argv[ai]);
    const char *outpath = argv[ai + 1];
    char loc[1100];
    for (int hop = 0; hop <= MAX_HOP; hop++) {
        url_t u;
        if (url_parse(cur, &u) < 0) { fprintf(stderr, "kfetch: unsupported url\n"); return 2; }
        int rc = fetch_once(&u, outpath, loc, sizeof loc);
        if (rc == 0) {
            unsigned char dg[32];
            br_sha256_out(&g_sha, dg);
            printf("kfetch: ok bytes=%llu sha256=", g_total);
            for (int i = 0; i < 32; i++) printf("%02x", dg[i]);
            printf("\n");
            fflush(stdout);
            return 0;
        }
        if (rc != 1) return rc;
        // redirects: absolute urls only, and https may not drop to http. (satoru)
        if (strncmp(loc, "http://", 7) && strncmp(loc, "https://", 8)) { fprintf(stderr, "kfetch: relative redirect refused\n"); return 5; }
        if (u.tls && !strncmp(loc, "http://", 7)) { fprintf(stderr, "kfetch: https to http redirect refused\n"); return 5; }
        snprintf(cur, sizeof cur, "%s", loc);
    }
    fprintf(stderr, "kfetch: too many redirects\n");
    return 5;
}
// end (satoru)
