# Serial Logger

`src/drivers/serial.cpp` and `serial.h` implement COM1 serial output used for kernel debug logging.

## 1. Purpose

The serial logger is the only output channel available before the framebuffer is initialized. It is the first thing brought up in `kurono_kernel.cpp` and the last thing still working when everything else has failed.

If you need to debug a boot hang, the serial port is where to look.

## 2. How it works

The driver initializes the first COM UART (0x3F8) at boot with a standard baud rate (typically 115200). It provides simple log functions:

- `SerialLogger::Log(const char*)` - write a null-terminated string
- `SerialLogger::LogDec(int)` - write a decimal integer
- `SerialLogger::LogHex(uint32_t)` - write a hex value

All log calls are synchronous (polling, no interrupts). This avoids any dependency on the interrupt infrastructure and makes the logger safe to call from the panic path.

## 3. Receiving output

Connect a serial terminal or use QEMU's `-serial stdio` option to see serial output.

In QEMU:
```
qemu-system-x86_64 -cdrom kurono.iso -serial stdio
```

This streams all serial output to the QEMU window. Boot logs, shell activity, and crash info all appear here.

## 4. What gets logged

- Every major boot phase transition.
- UIConfig load results.
- KVFS file operations.
- Panic messages (duplicated to serial in addition to the screen).
- Driver initialization results.

## 5. Related files

- `src/kernel/kurono_kernel.cpp` - serial is the very first init call
- `src/kernel/panic.cpp` - writes panic info to serial
- `src/system/ui_config.cpp` - writes config load results to serial
