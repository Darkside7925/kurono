# Timer (PIT)

`src/drivers/timer.cpp` and `timer.h`. The 8253/8254 PIT.

Channel 0, fixed rate (100 to 1000 Hz). Every interrupt bumps the kernel tick counter
and `time.cpp` turns ticks into milliseconds.

## Init

After the HAL has the IDT up. Three steps:

1. work out the reload value from the frequency and the PIT's 1.193182 MHz input
2. write it to channel 0
3. register the IRQ 0 handler with the HAL

Then IRQ 0 fires at that rate and the handler advances the tick counter.

## Who uses it

- `src/kernel/time.cpp` for elapsed time
- `src/proc/scheduler.cpp` for preemption on tick boundaries
- the main loop, for frame pacing

## When it breaks

| Symptom | Cause |
| --- | --- |
| time runs fast or slow | reload value miscalculated for the frequency |
| no ticks at all | IRQ 0 not registered, or the PIT ports aren't reachable |
| scheduler never preempts | the timer IRQ isn't reaching the scheduler callback |

## Files

- `src/hal/hal.cpp` IRQ 0 registration
- `src/kernel/time.cpp` tick consumer
- `src/proc/scheduler.cpp` preemption consumer
