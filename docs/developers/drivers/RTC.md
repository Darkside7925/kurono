# RTC

`src/drivers/rtc.cpp` and `rtc.h`. Reads the battery-backed CMOS clock.

Keeps date and time across power-off. The driver reads it once at boot to seed the
kernel wall clock, then gets out of the way.

## Reading it

CMOS index/data ports, 0x70/0x71. Reads seconds, minutes, hours, day, month, year.
Values can be BCD or binary depending on status register B, so the driver checks the
format flag and converts when it has to. 12-hour vs 24-hour is handled too.

## Why only once

RTC reads are slow (microseconds each) and the PIT gives better resolution, so after
boot the PIT carries wall time forward. Nothing polls the RTC.

## Files

- `src/kernel/time.cpp` consumes the boot-time reading
- `src/drivers/timer.cpp` ongoing elapsed time after boot
