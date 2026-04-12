# Real Time Clock

`src/drivers/rtc.cpp` and `rtc.h` read the hardware RTC to get the current date and time.

## 1. What it does

The RTC is a battery-backed clock in the CMOS chip. It holds the current date and time even when the machine is powered off. The driver reads the RTC registers at boot to seed the kernel's wall clock.

## 2. Reading the RTC

RTC registers are accessed via the CMOS index/data port pair (0x70/0x71). The driver reads:

- Seconds, minutes, hours
- Day, month, year

The values may be in BCD or binary format depending on the RTC's status register B. The driver checks the format flag and converts BCD to binary if needed.

## 3. Limitations

The driver reads the RTC once at boot. After that, the kernel's timer-based counter tracks elapsed time. The RTC is not polled continuously because RTC register reads are slow (microseconds each) and the PIT provides better time resolution.

## 4. 24-hour vs 12-hour

The RTC's status register determines whether hours are in 24-hour or 12-hour format. The driver handles both cases to work correctly on all machines.

## 5. Related files

- `src/kernel/time.cpp`  -  consumer of the boot-time RTC reading
- `src/drivers/timer.cpp`  -  provides ongoing elapsed time after boot
