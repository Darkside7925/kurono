// lx_emul: the x86 return thunk.
//
// The pinned config has CONFIG_RETHUNK, so upstream asm's RET macro jumps here
// instead of using a bare ret. The container has no spectre mitigation; the
// thunk is the identity. (satoru)
//
// end (satoru)

__attribute__((naked)) void __x86_return_thunk(void)
{
    __asm__ __volatile__("ret");
}

// end (satoru)
