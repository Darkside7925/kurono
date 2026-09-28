# Serial

`src/drivers/serial.cpp` and `serial.h`. COM1 output, the kernel debug log.

Only channel that works before the framebuffer is up. Comes up first in
`kurono_kernel.cpp` and it's the last thing still talking when everything else has
fallen over. Chasing a boot hang? This is where you look.

## How it works

Sets up the first COM UART at 0x3F8 (115200) and gives you three calls:

- `SerialLogger::Log(const char*)` null-terminated string
- `SerialLogger::LogDec(int)` decimal
- `SerialLogger::LogHex(uint32_t)` hex

All polled, no interrupts. On purpose: no dependency on the interrupt
infrastructure, so it's still safe to call from the panic path.

## Reading it

`-serial stdio`, or point a serial terminal at it:

```
qemu-system-x86_64 -cdrom kurono.iso -serial stdio
```

Boot phases, shell activity, crash info. All of it comes out here.

## What actually gets logged

- every major boot phase transition
- UIConfig load results
- KVFS file operations
- panic messages, duplicated to serial as well as the screen
- driver init results

## Files

- `src/kernel/kurono_kernel.cpp` serial is the very first init call
- `src/kernel/panic.cpp` writes panic info to serial
- `src/system/ui_config.cpp` writes config load results to serial
