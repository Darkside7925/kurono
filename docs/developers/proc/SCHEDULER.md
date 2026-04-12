# Scheduler

`src/proc/scheduler.cpp` and `scheduler.h` implement the Kurono process scheduler.

## 1. What it does

The scheduler maintains the list of runnable tasks and decides which one runs each time it is ticked. It is called from the main loop and from the timer interrupt callback.

## 2. Current model

The current scheduler is a cooperative/time-slice hybrid. Processes yield explicitly or are preempted after a timer quantum. Priority values (stored in the `TMProcess` struct) influence the time quantum allocation  -  higher priority processes run more often.

Because Kurono does not use hardware context switching (no separate user-space rings with full register save/restore), the "processes" are actually OS-managed tasks that share the kernel stack. Full preemptive user-space processes would require TSS setup and syscall infrastructure.

## 3. Process table

The scheduler owns the canonical process table. The Task Manager app reads from this table for its display. Each entry includes:

- PID (unique integer)
- Name (32-char string)
- CPU usage percentage
- Memory usage (KB)
- Thread count
- State (RUNNING, SLEEPING, STOPPED, ZOMBIE)
- Priority
- I/O counters

## 4. Tick

`Scheduler::Tick()` is called from the main loop each iteration. It advances time-slice counters and runs context switches.

## 5. Related files

- `src/apps/task_manager.cpp`  -  reads process table for display
- `src/drivers/timer.cpp`  -  timer interrupt triggers tick
- `src/kernel/kurono_kernel.cpp`  -  calls `Scheduler::Tick()` in main loop
