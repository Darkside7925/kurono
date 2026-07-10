# CPU Detection

`src/drivers/cpu_detect.cpp` and `cpu_detect.h` run CPUID to discover processor features.

## 1. What it does

The CPU detect module executes the `CPUID` instruction to query the processor for:

- Vendor string (GenuineIntel, AuthenticAMD, etc.)
- Brand string (human-readable model name)
- Feature flags in ECX/EDX for basic CPUID and extended leaves
- SSE, SSE2, AVX, AVX2, AES-NI availability
- Hypervisor presence bit
- Physical and logical core counts

## 2. When it runs

CPU detection runs during early boot in `kurono_kernel.cpp`. The results are cached in the module and available to any code that calls `CPUDetect::Get()`.

## 3. Uses in the codebase

- The hypervisor checks for VMX/SVM support before attempting to bring up virtualization hardware.
- The media decoder can use SIMD paths if SSE2 is present.
- The Task Manager's CPU info tab displays the vendor string and feature set.
- The `kurono info` shell command reports CPU features.

## 4. Hypervisor detection

The hypervisor presence bit (CPUID ECX bit 31 on leaf 1) indicates that the processor is running inside a virtual machine. Kurono uses this to adjust behavior - for example, preferring the BGA display path when a hypervisor is detected.

## 5. Related files

- `src/virt/hypervisor.cpp` - checks for VMX support before init
- `src/apps/task_manager.cpp` - displays CPU info
- `src/shell/shell.cpp` - `kurono info` command reports CPU features
