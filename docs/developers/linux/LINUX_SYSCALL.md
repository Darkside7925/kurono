# Linux Syscall Layer

`src/linux/linux_syscall.cpp` and `linux_syscall.h` implement the Linux system call dispatch table.

## 1. What it does

The Linux syscall layer intercepts `syscall` instructions from guest code that expects a Linux kernel. It maps Linux syscall numbers to Kurono implementations or personality stubs.

This is the core of the Linux binary compatibility feature: native Kurono code can run simple Linux ELF binaries by routing their syscalls through this table.

## 2. Dispatch table

Syscalls are dispatched by number. The table covers the most commonly used x86-64 Linux syscalls:

| Number | Name | Implementation |
| --- | --- | --- |
| 0 | `read` | KVFS read path |
| 1 | `write` | Output buffer |
| 2 | `open` | KVFS open |
| 3 | `close` | File descriptor close |
| 12 | `brk` | Heap extension stub |
| 60 | `exit` | Process termination |
| 231 | `exit_group` | Group exit |

The list extends to cover enough surface area for basic command-line programs.

## 3. Unsupported syscalls

Unsupported syscall numbers are logged to serial and return `-ENOSYS`. Programs that hit unsupported syscalls frequently may need the full hypervisor path (a real Linux guest) rather than the native syscall stub layer.

## 4. Related files

- `src/linux/linux_kernel.cpp`  -  personality coordinator that activates the syscall layer
- `src/virt/hypervisor.cpp`  -  full hardware virtualization path for heavier Linux workloads
