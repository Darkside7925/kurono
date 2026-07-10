# Timer Driver

`src/drivers/timer.cpp` and `timer.h` control the x86 PIT (Programmable Interval Timer).

## 1. What it does

The timer driver programs Channel 0 of the 8253/8254 PIT to fire at a fixed rate (typically 100 - 1000 Hz). Each interrupt advances the kernel tick counter. The time module converts those ticks to milliseconds.

## 2. Initialization

Timer initialization happens after HAL sets up the IDT. The driver:

1. Calculates the reload value from the desired frequency and the PIT's 1.193182 MHz input clock.
2. Writes the reload value to the PIT's channel 0 registers.
3. Registers the IRQ 0 handler with the HAL.

After init, IRQ 0 fires at the programmed rate and the handler increments the tick counter.

## 3. Used by

- `src/kernel/time.cpp` - consumes the tick counter for elapsed time
- `src/proc/scheduler.cpp` - preemption based on tick boundary
- Main loop - advances frame timing

## 4. Common problems

| Problem | Likely cause |
| --- | --- |
| Time runs too fast or slow | Reload value calculated wrong for chosen frequency |
| No timer ticks | IRQ 0 not registered or PIT ports not accessible |
| Scheduler not preempting | Timer IRQ not reaching the scheduler callback |

## 5. Related files

- `src/hal/hal.cpp` - IRQ 0 registration
- `src/kernel/time.cpp` - client of the tick counter
- `src/proc/scheduler.cpp` - preemption client
