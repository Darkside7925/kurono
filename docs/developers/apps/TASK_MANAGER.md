# Task manager

`src/apps/task_manager.cpp` and `task_manager.h`. Five tabs (`TMTab`):
`TM_PROCESSES`, `TM_PERFORMANCE`, `TM_DETAILS`, `TM_SERVICES`, `TM_WINDOWS`.

## Processes

Sortable list. Each row has PID, name, CPU%, memory, thread count, state, user and
priority. Rows are labelled kernel vs user. A user (Linux) process's memory is
RSS-style: its mapped regions plus its `brk` heap, not a made-up per-process number.

## Performance

Live CPU and memory graphs with history, plus uptime, core count and network I/O.

Memory here is real. `PMM::GetTotalMemory()` and `GetFreeMemory()` give total, used
and free, with a separate kernel-heap sub-total from
`KernelHeap::GetTotal/Used/Free`. Not a sum of per-process `mem_kb`.

Uptime is monotonic off `Scheduler::NowMs()` (boot-relative). The old wall-clock
version produced a nonsense "899 hour" uptime and is gone.

## Details

Everything on the selected process, including I/O counters and full state.

## Services

Kurono services with name, PID, type (kernel/system/user) and running status.

## Windows

Open WM windows get their own tab now, they are no longer listed as processes. A
window is a WM record plus a framebuffer, not a scheduler task. Rows here can be closed
or restarted from the action menu, which lands on `WindowManager::CloseWindow`.

## The process list

`RefreshProcesses()` fills it from the kernel scheduler/process table, plus the Linux
process records for better command names and `brk` size. Windows are excluded on
purpose, they live in the Windows tab. Sort by Name, PID, CPU or Memory, click the
header to flip direction. Column geometry got reworked to fix the old column
collisions, and the window is taller now (560x500) so the Performance panes and process
rows fit without clipping.

## Kill and restart

Click a row to select, click the same row again for the action menu:

- Kill: drops it from the list immediately. On real hardware it sends SIGKILL.
- Restart: resets CPU usage and state back to running.

Turn the whole thing off with `taskmgr.allow_kill = 0` in `/etc/kurono/ui.conf`.

## Graphs

`Tick()` runs every frame from the main loop. It advances a 60-sample history ring and
updates the live CPU and memory counters. `RenderPerformance()` draws from the history.

## Opening it

Start menu, `DesktopEnvironment::LaunchTaskManager()`, and Ctrl+Shift+Esc when that
gets wired up. One instance at a time.

## UIConfig keys

| Key | Default | Meaning |
| --- | --- | --- |
| `taskmgr.row_h` | 20 | row height in the process list |
| `taskmgr.allow_kill` | 1 | enables the Kill/Restart action menu |

## Files

- `src/apps/task_manager.cpp` / `.h` the app
- `src/proc/scheduler.cpp` process table plus `Scheduler::NowMs()`
- `src/kernel/pmm.cpp` `PMM::GetTotalMemory/GetFreeMemory`, real RAM numbers
- `src/kernel/heap.cpp` `KernelHeap::GetTotal/Used/Free`, kernel heap sub-total
- `src/ui/window_manager.cpp` Windows-tab rows and `CloseWindow`
- `src/system/ui_config.cpp` the `taskmgr.*` keys
- `src/ui/desktop.cpp` `LaunchTaskManager()` entry point
