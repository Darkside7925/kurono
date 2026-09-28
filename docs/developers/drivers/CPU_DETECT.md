# CPU detection

`src/drivers/cpu_detect.cpp` and `cpu_detect.h`. Runs CPUID and caches the answers.

It pulls out:

- vendor string (GenuineIntel, AuthenticAMD and friends)
- brand string
- feature flags from ECX/EDX on the basic and extended leaves
- SSE, SSE2, AVX, AVX2, AES-NI presence
- the hypervisor presence bit
- physical and logical core counts

## When it runs

Early boot, in `kurono_kernel.cpp`. Results get cached, so anything can call
`CPUDetect::Get()` later.

## Who reads it

- the hypervisor, which checks VMX/SVM before trying to bring up virtualization
- the media decoder, which takes SIMD paths when SSE2 is there
- the task manager's CPU info
- `kurono info` in the shell

## The hypervisor bit

CPUID leaf 1, ECX bit 31, says the CPU is inside a VM. Kurono uses it to change
behaviour, for example preferring the BGA display path.

## Files

- `src/virt/hypervisor.cpp` checks VMX before init
- `src/apps/task_manager.cpp` shows CPU info
- `src/shell/shell.cpp` `kurono info` reports the feature set
