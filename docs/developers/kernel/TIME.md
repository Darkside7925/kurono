# Timekeeping

`src/kernel/time.cpp` and `time.h` expose the kernel's wall clock and elapsed time accounting.

## 1. What it does

The time module maintains two things: an elapsed millisecond counter used by timeouts, animations, and scheduler tick tracking, and a wall-clock estimate used by the shell and UI clock display.

## 2. Sources

The time module reads from two hardware sources.

The **PIT (Programmable Interval Timer)** drives the primary elapsed tick counter. The kernel programs the PIT in the HAL bring-up and each tick advances the counter. The time module consumes that counter and converts it to milliseconds.

The **RTC (Real Time Clock)** is read at boot to seed the wall clock. After that the PIT counter is used to advance wall time because polling the RTC on every tick is slow.

## 3. Initialization

Time is initialized in `kurono_kernel.cpp` after HAL and the PIT are ready. The initial wall clock is set from the current RTC values.

## 4. Uses in the codebase

- The main loop calls the time module to advance tick state.
- The taskbar clock reads hour and minute from this module.
- Task manager uptime counter uses elapsed seconds.
- The audio subsystem uses elapsed time for tone scheduling.
- Animations and timeouts in the desktop environment use `GetTicks()` style calls.

## 5. Related files

- `src/drivers/timer.cpp` - PIT driver that feeds ticks into the time module
- `src/drivers/rtc.cpp` - RTC driver used for wall clock seeding
- `src/kernel/kurono_kernel.cpp` - initialization order
