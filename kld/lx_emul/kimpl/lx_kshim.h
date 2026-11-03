// kernel-context view of the shim's own helpers.
//
// lx_emul.h includes <stdint.h>/<stddef.h>, which collides with the kernel
// headers (conflicting int64_t/uint64_t). Kernel-context sources must therefore
// NOT include lx_emul.h - they include this instead, which declares the same
// handful of helpers with no libc headers at all.
//
// end (satoru)

#ifndef KLD_LX_KSHIM_H
#define KLD_LX_KSHIM_H

void lx_log(const char *s);
void lx_log_u(unsigned long v);
void lx_log_line(const char *s);
__attribute__((noreturn)) void lx_unimpl(const char *sym);

// monotonic ns since container start (lx_time.c). (satoru)
unsigned long long lx_uptime_ns(void);

// single-threaded-assumption tracking (lx_sync.c). (satoru)
void lx_sync_note_thread_created(void);
int  lx_sync_is_single_threaded(void);

#endif
// end (satoru)
