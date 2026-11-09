// findtest - a parallel tree walk over the kurono vfs, shaped like the file
// searches claude code runs for its skill/Glob/Grep lookups: many worker
// processes call getdents64, fstatat/newfstatat (both dirfd-relative and by
// absolute path), readlinkat and openat/read while a mutator creates and
// removes cache files in the home tree. prints one progress line every 4096
// entries per worker and a final "FINDTEST PASS" (or FAIL/TIMEOUT) marker to
// stderr. built musl-static for the ring-3 linux abi.
//
// usage: findtest [workers] [rounds] [read] [mutate] [ndirs] [nfiles] [nthreads]
// ndirs > 0 first builds a synthetic skill-tree under /tmp/bigtree (ndirs
// directories in small parents, nfiles files in each), so the walk covers
// a tree the size of a real project/home directory rather than the seed fs.
// (satoru)
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pthread.h>

#ifndef SYS_getdents64
#define SYS_getdents64 217
#endif
#ifndef SYS_newfstatat
#define SYS_newfstatat 262
#endif
#ifndef SYS_readlinkat
#define SYS_readlinkat 267
#endif

#define FT_AT_FDCWD (-100)
#define FT_AT_SYMLINK_NOFOLLOW 0x100
#define FT_MAX_DEPTH 40
#define FT_PATH_MAX 512
#define FT_GBUF 2048
#define FT_READ_CAP (256u * 1024u)

struct ft_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

struct ft_stats {
    unsigned long entries;
    unsigned long dirs;
    unsigned long files;
    unsigned long links;
    unsigned long errs;
    unsigned long reads;
    unsigned long muts;
    int           wid;
    int           round;
};

static int g_read = 1;
static int g_mutate = 1;
static __thread char g_tmpdir[128];

static void ft_logs(const char* s) { (void)!write(2, s, strlen(s)); }

static char* ft_u2s(char* p, unsigned long v) {
    char t[24];
    int n = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) *p++ = t[--n];
    return p;
}

static void ft_progress(const struct ft_stats* st) {
    char b[192];
    char* p = b;
    memcpy(p, "FT w=", 5); p += 5;
    p = ft_u2s(p, (unsigned long)st->wid);
    memcpy(p, " r=", 3); p += 3;
    p = ft_u2s(p, (unsigned long)st->round);
    memcpy(p, " n=", 3); p += 3;
    p = ft_u2s(p, st->entries);
    memcpy(p, " d=", 3); p += 3;
    p = ft_u2s(p, st->dirs);
    memcpy(p, " f=", 3); p += 3;
    p = ft_u2s(p, st->files);
    memcpy(p, " l=", 3); p += 3;
    p = ft_u2s(p, st->links);
    memcpy(p, " e=", 3); p += 3;
    p = ft_u2s(p, st->errs);
    *p++ = '\n';
    (void)!write(2, b, (size_t)(p - b));
}

static void ft_mutate(struct ft_stats* st) {
    if (!g_mutate || !g_tmpdir[0]) return;
    char path[192];
    char path2[192];
    int n = (int)strlen(g_tmpdir);
    memcpy(path, g_tmpdir, (size_t)n);
    char* p = path + n;
    *p++ = '/'; *p++ = 'm';
    p = ft_u2s(p, st->muts);
    memcpy(p, ".tmp", 5);
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd >= 0) {
        char data[64];
        memset(data, 'x', sizeof(data));
        (void)!write(fd, data, sizeof(data));
        close(fd);
        memcpy(path2, path, strlen(path) + 1);
        char* q = path2 + strlen(path2) - 4;
        memcpy(q, ".ren", 5);
        (void)!rename(path, path2);
        (void)!unlink(path2);
        st->muts++;
    }
}

static void ft_read_file(int dfd, const char* name, struct ft_stats* st) {
    int fd = openat(dfd, name, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) { st->errs++; return; }
    char b[4096];
    unsigned long total = 0;
    while (total < FT_READ_CAP) {
        long r = read(fd, b, sizeof(b));
        if (r <= 0) break;
        total += (unsigned long)r;
    }
    close(fd);
    st->reads++;
}

static void ft_walk(int dfd, const char* path, int depth, struct ft_stats* st) {
    if (depth > FT_MAX_DEPTH) return;
    int dirfd = openat(dfd, path, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) { st->errs++; return; }
    st->dirs++;
    char buf[FT_GBUF];
    for (;;) {
        long n = syscall(SYS_getdents64, dirfd, buf, sizeof(buf));
        if (n < 0) { st->errs++; break; }
        if (n == 0) break;
        long off = 0;
        while (off < n) {
            struct ft_dirent64* de = (struct ft_dirent64*)(buf + off);
            if (de->d_reclen < 19 || off + de->d_reclen > n) break;
            off += de->d_reclen;
            const char* name = de->d_name;
            if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
            st->entries++;
            struct stat sb;
            long r = syscall(SYS_newfstatat, dirfd, name, &sb, FT_AT_SYMLINK_NOFOLLOW);
            if (r < 0) { st->errs++; }
            if (r == 0 && S_ISLNK(sb.st_mode)) {
                char lb[256];
                (void)syscall(SYS_readlinkat, dirfd, name, lb, sizeof(lb) - 1);
                st->links++;
            } else if (r == 0 && S_ISDIR(sb.st_mode)) {
                ft_walk(dirfd, name, depth + 1, st);
            } else if (r == 0 && S_ISREG(sb.st_mode)) {
                st->files++;
                if (depth == 0) {
                    struct stat f2;
                    char full[FT_PATH_MAX];
                    int pl = (int)strlen(path);
                    int nl = (int)strlen(name);
                    if (pl + 1 + nl < (int)sizeof(full)) {
                        memcpy(full, path, (size_t)pl);
                        full[pl] = '/';
                        memcpy(full + pl + 1, name, (size_t)nl + 1);
                        (void)syscall(SYS_newfstatat, FT_AT_FDCWD, full, &f2, 0);
                    }
                }
                if (g_read) ft_read_file(dirfd, name, st);
            }
            if ((st->entries & 4095) == 0) { ft_progress(st); ft_mutate(st); }
        }
    }
    close(dirfd);
}

static const char* const g_roots[] = {
    "/system", "/usr", "/home", "/proc", "/dev", "/etc", "/var", "/lib", "/kurono", "/tmp",
};
#define FT_NROOTS ((int)(sizeof(g_roots) / sizeof(g_roots[0])))

static int g_ndirs = 0;
static int g_nthreads = 1;
static int g_nfiles = 0;
static unsigned long g_made;

// build a wide skill tree the search tools then have to walk. every directory
// gets nfiles files plus a nested level, so dirfd-relative getdents/fstatat
// exercise both flat and nested lookups. (satoru)
static void ft_build_tree(void) {
    if (g_ndirs <= 0) return;
    mkdir("/tmp/bigtree", 0755);
    char dir[128];
    char file[160];
    for (int i = 0; i < g_ndirs; i++) {
        // group the leaves below a small number of parents: kvfs caps one
        // directory at 512 children, so a flat 1200-entry list would silently
        // stop at the cap. (satoru)
        char grp[128];
        char* g = grp;
        memcpy(g, "/tmp/bigtree/g", 14); g += 14;
        g = ft_u2s(g, (unsigned long)(i / 100));
        *g = 0;
        if (mkdir(grp, 0755) != 0) { /* already there */ }
        char* p = dir;
        memcpy(p, grp, strlen(grp)); p += strlen(grp);
        *p++ = '/'; *p++ = 'd';
        p = ft_u2s(p, (unsigned long)i);
        *p = 0;
        if (mkdir(dir, 0755) == 0) g_made++;
        char sub[160];
        memcpy(sub, dir, strlen(dir) + 1);
        strcat(sub, "/sub");
        if (mkdir(sub, 0755) == 0) g_made++;
        int nf = g_nfiles > 0 ? g_nfiles : 1;
        for (int j = 0; j < nf; j++) {
            char* q = file;
            memcpy(q, dir, strlen(dir)); q += strlen(dir);
            *q++ = '/'; *q++ = 'f';
            q = ft_u2s(q, (unsigned long)j);
            memcpy(q, ".md", 4);
            int fd = open(file, O_CREAT | O_TRUNC | O_WRONLY, 0644);
            if (fd >= 0) {
                char data[96];
                memset(data, 's', sizeof(data));
                (void)!write(fd, data, sizeof(data));
                close(fd);
                g_made++;
            }
        }
    }
}

static int ft_worker(int wid, int rounds) {
    struct ft_stats st;
    memset(&st, 0, sizeof(st));
    st.wid = wid;
    // a private cache dir per worker, the way claude's session writes skill and
    // project caches while its search tools run. (satoru)
    memset(g_tmpdir, 0, sizeof(g_tmpdir));
    if (g_mutate) {
        strcpy(g_tmpdir, "/home/user/.findtest");
        mkdir(g_tmpdir, 0755);
        char sub[160];
        strcpy(sub, g_tmpdir);
        char* p = sub + strlen(sub);
        *p++ = '/'; *p++ = 'w';
        p = ft_u2s(p, (unsigned long)wid);
        *p = 0;
        strcpy(g_tmpdir, sub);
        mkdir(g_tmpdir, 0755);
    }
    for (int r = 0; r < rounds; r++) {
        st.round = r;
        for (int k = 0; k < FT_NROOTS; k++) {
            int idx = (k + wid) % FT_NROOTS;
            ft_walk(FT_AT_FDCWD, g_roots[idx], 0, &st);
        }
        ft_progress(&st);
    }
    ft_progress(&st);
    char b[128];
    char* p = b;
    memcpy(p, "FT DONE w=", 10); p += 10;
    p = ft_u2s(p, (unsigned long)wid);
    memcpy(p, " n=", 3); p += 3;
    p = ft_u2s(p, st.entries);
    memcpy(p, " d=", 3); p += 3;
    p = ft_u2s(p, st.dirs);
    memcpy(p, " f=", 3); p += 3;
    p = ft_u2s(p, st.files);
    memcpy(p, " l=", 3); p += 3;
    p = ft_u2s(p, st.links);
    memcpy(p, " e=", 3); p += 3;
    p = ft_u2s(p, st.errs);
    *p++ = '\n';
    (void)!write(2, b, (size_t)(p - b));
    return 0;
}

struct ft_tharg { int wid; int rounds; };

static void* ft_thread_entry(void* arg) {
    struct ft_tharg* a = (struct ft_tharg*)arg;
    (void)ft_worker(a->wid, a->rounds);
    return NULL;
}

// one worker process: nthreads threads walk the same roots in parallel, the
// way ripgrep's walker threads and bun's worker pool run file syscalls inside
// one address space and one fd table. (satoru)
static int ft_parallel_worker(int wid, int rounds) {
    int nt = g_nthreads - 1;
    if (nt > 6) nt = 6;
    pthread_t th[6];
    struct ft_tharg ta[6];
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 512 * 1024);   // deep tree recursion (satoru)
    int made = 0;
    for (int t = 0; t < nt; t++) {
        ta[t].wid = wid * 8 + t + 1;
        ta[t].rounds = rounds;
        if (pthread_create(&th[t], &at, ft_thread_entry, &ta[t]) == 0) made++;
    }
    pthread_attr_destroy(&at);
    int rc = ft_worker(wid * 8, rounds);
    for (int t = 0; t < made; t++) pthread_join(th[t], NULL);
    return rc;
}

static void ft_alarm(int sig) {
    (void)sig;
    (void)!write(2, "FINDTEST TIMEOUT\n", 17);
    _exit(9);
}

int main(int argc, char** argv) {
    int workers = argc > 1 ? atoi(argv[1]) : 4;
    int rounds  = argc > 2 ? atoi(argv[2]) : 2;
    g_read      = argc > 3 ? atoi(argv[3]) : 1;
    g_mutate    = argc > 4 ? atoi(argv[4]) : 1;
    g_ndirs     = argc > 5 ? atoi(argv[5]) : 0;
    g_nfiles    = argc > 6 ? atoi(argv[6]) : 0;
    g_nthreads  = argc > 7 ? atoi(argv[7]) : 1;
    if (g_nthreads < 1) g_nthreads = 1;
    if (g_nthreads > 7) g_nthreads = 7;
    if (g_ndirs < 0) g_ndirs = 0;
    if (g_ndirs > 20000) g_ndirs = 20000;
    if (g_nfiles < 0) g_nfiles = 0;
    if (g_nfiles > 64) g_nfiles = 64;
    if (workers < 1) workers = 1;
    if (workers > 16) workers = 16;
    if (rounds < 1) rounds = 1;
    if (rounds > 50) rounds = 50;

    signal(SIGALRM, ft_alarm);
    alarm(180);

    ft_logs("FINDTEST: start\n");
    ft_build_tree();
    if (g_made) {
        ft_logs("FINDTEST: tree built nodes=");
        char b[64]; char* p = b;
        p = ft_u2s(p, g_made); *p++ = '\n';
        (void)!write(2, b, (size_t)(p - b));
    }

    int pids[16];
    int npid = 0;
    pid_t pid = fork();
    if (pid == 0) {
        int rc = ft_parallel_worker(0, rounds);
        _exit(rc);
    }
    if (pid > 0) pids[npid++] = pid;
    for (int i = 1; i < workers; i++) {
        pid = fork();
        if (pid == 0) {
            int rc = ft_parallel_worker(i, rounds);
            _exit(rc);
        }
        if (pid > 0) pids[npid++] = pid;
    }

    // the parent joins the walk too, so the tree is read from one more process
    // while the children run. (satoru)
    (void)ft_parallel_worker(workers < 16 ? workers : 15, rounds);

    int bad = 0;
    for (int i = 0; i < npid; i++) {
        int st = 0;
        if (waitpid(pids[i], &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) bad++;
    }
    if (bad) {
        char b[64];
        char* p = b;
        memcpy(p, "FINDTEST FAIL bad=", 18); p += 18;
        p = ft_u2s(p, (unsigned long)bad);
        *p++ = '\n';
        (void)!write(2, b, (size_t)(p - b));
        return 1;
    }
    ft_logs("FINDTEST PASS\n");
    return 0;
}

// end (satoru)
