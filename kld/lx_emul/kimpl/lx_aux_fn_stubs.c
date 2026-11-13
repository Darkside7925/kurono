// function providers split from lx_aux_providers.c: LX_STUB defines a
// zero-argument function, which conflicts with the real prototypes once the
// typed headers are visible. this file deliberately includes only lx_emul.h.
// (satoru)

#include "lx_emul.h"

// the drm families link the upstream seq_file.c, which owns these symbols; a
// weak stub here yields to it. every other family has no other definition.
// (satoru)
#define LX_STUB_WEAK(sym) __attribute__((weak)) void sym(void) { lx_stub_note(#sym); lx_unimpl(#sym); }

// ----- functions whose address the driver tables store ----------------------
LX_STUB(compat_ptr_ioctl)
LX_STUB(handle_bad_irq)
LX_STUB(handle_edge_irq)
LX_STUB(handle_level_irq)
LX_STUB(irq_domain_free_irqs_common)
LX_STUB(irq_domain_xlate_twocell)
LX_STUB_WEAK(noop_llseek)
LX_STUB_WEAK(seq_lseek)
LX_STUB_WEAK(seq_read)
LX_STUB_WEAK(seq_release)
LX_STUB_WEAK(seq_release_private)
LX_STUB_WEAK(single_release)

// mt76 debugfs.c stores these three in a struct file_operations
// (DEFINE_DEBUGFS_ATTRIBUTE: .read, .write, .release). the container has no
// debugfs (debugfs_create_* return NULL), so the file is never created and the
// callbacks are never invoked; if one ever is, the stub aborts loudly. they
// are function addresses, not data. (satoru)
LX_STUB_WEAK(debugfs_attr_read)
LX_STUB_WEAK(debugfs_attr_write)
LX_STUB_WEAK(simple_attr_release)

// end (satoru)
