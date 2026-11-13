// lx_emul: static-call trampolines, defined OUTSIDE kernel context.
//
// The kernel declares __SCT__might_resched and friends with a static-call type
// that conflicts with a plain void(void) definition, so they cannot live in a
// kimpl/ file. Defining them here gives real no-ops instead of aborting stubs,
// which matters: the driver's cond_resched() calls through them, and aborting
// there would be a false "unimplemented" on a code path that legitimately has
// nothing to do in a non-preemptible container.
//
// If preemption is ever introduced in the container, these become real yield
// points and belong back in kernel context with the correct type.
//
// end (satoru)

void __SCT__might_resched(void)            { }
void __SCT__preempt_schedule(void)         { }
void __SCT__cond_resched(void)             { }
void __SCT__preempt_schedule_notrace(void) { }

// end (satoru)
