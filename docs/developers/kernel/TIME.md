# Timekeeping

`src/kernel/time.cpp` and `time.h`. Two things live here: an elapsed millisecond
counter (timeouts, animations, scheduler ticks) and a wall clock estimate (shell,
taskbar).

## Where the numbers come from

The PIT drives the elapsed counter. The HAL programs it, each tick advances it, and
this module converts ticks to milliseconds.

The RTC seeds the wall clock once at boot. After that the PIT carries wall time,
because poking the RTC every tick is slow.

## Init

In `kurono_kernel.cpp`, once the HAL and the PIT are ready. First wall clock comes
straight from the RTC.

## Who uses it

- the main loop advances tick state
- the taskbar clock reads hour and minute
- task manager uptime uses elapsed seconds
- the audio subsystem schedules tones off elapsed time
- desktop animations and timeouts use the `GetTicks()` calls

## Files

- `src/drivers/timer.cpp` PIT driver feeding ticks in
- `src/drivers/rtc.cpp` wall clock seed
- `src/kernel/kurono_kernel.cpp` init order
