// kxfer - copy files between a Linux guest and the Kurono host it runs on (satoru)
//
//   kxfer put <guest file> <name>   guest -> host /home/user/vmshare/<name>
//   kxfer get <name> <guest file>   host /home/user/vmshare/<name> -> guest
//   kxfer ls                        list the share
//
// uses Kurono's v9fs hypercall (vmcall 0x20): 4 KB per call through one
// locked page whose guest-physical address comes from /proc/self/pagemap, so
// it needs root in the guest and nothing else (no network, no guest driver).
// build in the guest: gcc -O2 -static -o kxfer kxfer.c (satoru)
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum { V_VERSION = 0, V_ATTACH, V_WALK, V_OPEN, V_READ, V_WRITE, V_CLUNK, V_STAT, V_READDIR, V_CREATE, V_REMOVE };
static uint8_t* g_page;
static uint64_t g_phys;

static uint32_t vcall(uint32_t op, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t* ecx_out) {
    uint64_t rax = 0x20, rcx = op, rdx = a0, rbx = a1, rsi = a2, rdi = a3;
    __asm__ volatile("vmcall" : "+a"(rax), "+c"(rcx), "+d"(rdx), "+b"(rbx), "+S"(rsi), "+D"(rdi) :: "memory");
    if (ecx_out) *ecx_out = rcx;
    return (uint32_t)rax;
}

static int setup(void) {
    if (posix_memalign((void**)&g_page, 4096, 4096)) return -1;
    memset(g_page, 0, 4096);
    if (mlock(g_page, 4096)) { perror("mlock"); return -1; }
    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) { perror("pagemap"); return -1; }
    uint64_t e = 0;
    if (pread(fd, &e, 8, ((uint64_t)(uintptr_t)g_page / 4096) * 8) != 8) { perror("pread"); return -1; }
    close(fd);
    if (!(e >> 63) || !(e & ((1ull << 55) - 1))) { fprintf(stderr, "kxfer: no pfn (run as root)\n"); return -1; }
    g_phys = (e & ((1ull << 55) - 1)) * 4096;
    uint64_t io = 0;
    if (vcall(V_VERSION, 0, 0, 0, 0, &io) != 0) { fprintf(stderr, "kxfer: no v9fs hypercall (not a Kurono guest?)\n"); return -1; }
    if (vcall(V_ATTACH, 0, 0, 0, 0, 0) != 0) { fprintf(stderr, "kxfer: attach failed\n"); return -1; }
    return 0;
}

static uint32_t name_call(uint32_t op, uint64_t a0, uint64_t a1, uint64_t a2, const char* name) {
    size_t n = strlen(name);
    if (n > 120) return 22;
    memcpy(g_page, name, n + 1);
    return vcall(op, a0, a1, a2, g_phys, 0);
}

static int put(const char* local, const char* name) {
    FILE* f = fopen(local, "rb");
    if (!f) { perror(local); return 1; }
    if (name_call(V_WALK, 0, 2, 0, name) == 0) vcall(V_REMOVE, 2, 0, 0, 0, 0);   // replace (satoru)
    if (name_call(V_CREATE, 0, 1, 0, name) != 0) { fprintf(stderr, "kxfer: create %s failed\n", name); return 1; }
    uint64_t off = 0, got;
    size_t n;
    while ((n = fread(g_page, 1, 4096, f)) > 0) {
        uint32_t r = vcall(V_WRITE, 1, off, n, g_phys, &got);
        if (r != 0 || got != n) { fprintf(stderr, "kxfer: write failed at %llu (err %u)\n", (unsigned long long)off, r); return 1; }
        off += n;
    }
    fclose(f);
    vcall(V_CLUNK, 1, 0, 0, 0, 0);
    printf("kxfer: put %s -> vmshare/%s (%llu bytes)\n", local, name, (unsigned long long)off);
    return 0;
}

static int get(const char* name, const char* local) {
    if (name_call(V_WALK, 0, 1, 0, name) != 0) { fprintf(stderr, "kxfer: %s not in the share\n", name); return 1; }
    if (vcall(V_OPEN, 1, 0, 0, 0, 0) != 0) { fprintf(stderr, "kxfer: open failed\n"); return 1; }
    FILE* f = fopen(local, "wb");
    if (!f) { perror(local); return 1; }
    uint64_t off = 0, got;
    for (;;) {
        uint32_t r = vcall(V_READ, 1, off, 4096, g_phys, &got);
        if (r != 0) { fprintf(stderr, "kxfer: read failed at %llu (err %u)\n", (unsigned long long)off, r); return 1; }
        if (!got) break;
        if (fwrite(g_page, 1, got, f) != got) { perror("write"); return 1; }
        off += got;
    }
    fclose(f);
    vcall(V_CLUNK, 1, 0, 0, 0, 0);
    printf("kxfer: get vmshare/%s -> %s (%llu bytes)\n", name, local, (unsigned long long)off);
    return 0;
}

static int ls(void) {
    if (vcall(V_OPEN, 0, 0, 0, 0, 0) != 0) return 1;
    uint64_t cnt = 0;
    for (uint32_t i = 0;; i++) {
        if (vcall(V_READDIR, 0, i, 1, g_phys, &cnt) != 0 || !cnt) break;
        uint32_t* st = (uint32_t*)g_page;   // type, size, mode, mtime, name[64] (satoru)
        printf("%10u %s%s\n", st[1], (const char*)(st + 4), st[0] == 1 ? "/" : "");
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: kxfer put <file> <name> | get <name> <file> | ls\n"); return 2; }
    if (setup()) return 1;
    if (!strcmp(argv[1], "put") && argc == 4) return put(argv[2], argv[3]);
    if (!strcmp(argv[1], "get") && argc == 4) return get(argv[2], argv[3]);
    if (!strcmp(argv[1], "ls")) return ls();
    fprintf(stderr, "usage: kxfer put <file> <name> | get <name> <file> | ls\n");
    return 2;
}
// end (satoru)
