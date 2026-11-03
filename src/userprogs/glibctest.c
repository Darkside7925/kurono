// glibctest - isolate the glibc-static exit crash under kurono (satoru)
//
// busybox (prebuilt, glibc-static) runs correctly under kurono and then dies in
// __run_exit_handlers with a wild `call *%rax`. the disassembly showed glibc's
// PTR_DEMANGLE sequence:
//
//     mov 0x18(%rax),%rcx      ; mangled handler
//     ror $0x11,%rax           ; rotate right 17
//     xor %fs:0x30,%rax        ; xor with the tcb pointer guard
//     call *%rax
//
// so either the pointer guard at %fs:0x30 differs between mangle and demangle
// time, or the atexit entry itself is corrupt. busybox is stripped, which makes
// that hard to tell apart - this repro is tiny and UNSTRIPPED so the fault lands
// on a named symbol.
//
// build: gcc -static -O0 -g -o glibctest.elf glibctest.c   (glibc, NOT musl)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

static uint64_t rd_fs30(void)
{
    uint64_t v;
    __asm__ __volatile__("mov %%fs:0x30, %0" : "=r"(v));
    return v;
}

static uint64_t rd_fs28(void)
{
    // fs:0x28 is the stack-protector canary - a second tcb field to compare
    // against, so we can tell "the whole tcb moved" from "one field is off"
    uint64_t v;
    __asm__ __volatile__("mov %%fs:0x28, %0" : "=r"(v));
    return v;
}

static uint64_t g_guard_at_register;
static uint64_t g_guard_in_ctor;

// runs before main but AFTER glibc has set the pointer guard and registered its
// own call_fini entry. if the guard differs here from main, the guard is moving
// under us and every pointer glibc mangled early demangles to garbage. (satoru)
__attribute__((constructor))
static void ctor_probe(void)
{
    __asm__ __volatile__("mov %%fs:0x30, %0" : "=r"(g_guard_in_ctor));
}

static void handler_a(void)
{
    printf("[glibctest] handler_a RAN\n");
}

static void handler_b(void)
{
    printf("[glibctest] handler_b RAN\n");
}

int main(int argc, char **argv)
{
    (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("[glibctest] start argc=%d\n", argc);

    g_guard_at_register = rd_fs30();
    printf("[glibctest] pointer_guard in ctor    = 0x%016llx\n",
           (unsigned long long)g_guard_in_ctor);
    printf("[glibctest] ctor vs main: %s\n",
           g_guard_in_ctor == g_guard_at_register ? "SAME"
                                                  : "*** DIFFERENT ***");

    // round-trip the mangle glibc uses, to prove the arithmetic itself works
    // here: PTR_MANGLE = xor guard then rol 17; PTR_DEMANGLE = ror 17 then xor
    {
        uint64_t h = (uint64_t)(uintptr_t)handler_a, m, d;
        uint64_t g = g_guard_at_register;
        m = h ^ g;
        m = (m << 17) | (m >> 47);
        d = (m >> 17) | (m << 47);
        d ^= g;
        printf("[glibctest] mangle round-trip %s (h=%p d=0x%llx)\n",
               d == h ? "OK" : "*** BROKEN ***", (void *)handler_a,
               (unsigned long long)d);
    }
    printf("[glibctest] pointer_guard at startup = 0x%016llx\n",
           (unsigned long long)g_guard_at_register);
    printf("[glibctest] stack_canary  at startup = 0x%016llx\n",
           (unsigned long long)rd_fs28());

    // a guard of exactly zero would make mangling a no-op and could not produce
    // a wild jump on its own, so record it explicitly (satoru)
    if (g_guard_at_register == 0)
        printf("[glibctest] NOTE pointer_guard is ZERO\n");

    // argc varies across the runner's four invocations, so one binary covers
    // both cases: WITH our handlers and with NONE. if it still dies with none
    // registered, the bad entry is glibc's own call_fini and not ours. (satoru)
    if (argc == 1) {
        if (atexit(handler_a) != 0) printf("[glibctest] atexit(a) FAILED\n");
        if (atexit(handler_b) != 0) printf("[glibctest] atexit(b) FAILED\n");
        printf("[glibctest] registered 2 atexit handlers\n");
        printf("[glibctest] handler_a=%p handler_b=%p\n",
               (void *)handler_a, (void *)handler_b);
    } else {
        printf("[glibctest] registered ZERO atexit handlers this run\n");
    }

    // force some scheduling before exit: if the guard is being clobbered by a
    // context switch (fs_base save/restore), this is where it would happen
    for (int i = 0; i < 200000; i++) __asm__ __volatile__("" ::: "memory");
    fflush(stdout);

    uint64_t after = rd_fs30();
    printf("[glibctest] pointer_guard before exit = 0x%016llx %s\n",
           (unsigned long long)after,
           after == g_guard_at_register ? "STABLE" : "*** CHANGED ***");

    printf("[glibctest] calling exit(0) - handlers must run now\n");
    fflush(stdout);
    return 0;
}

// end (satoru)
