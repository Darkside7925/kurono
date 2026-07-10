# Task Manager

`src/apps/task_manager.cpp` and `task_manager.h` implement the Task Manager application.

## 1. What it shows

The Task Manager has **five tabs** (`TMTab`: `TM_PROCESSES`, `TM_PERFORMANCE`,
`TM_DETAILS`, `TM_SERVICES`, `TM_WINDOWS`).

**Processes** - a sortable list of running processes. Each row shows PID, name,
CPU%, memory, thread count, state, user, and priority. Rows are **labelled kernel
vs user**; a user (Linux) process's memory is computed RSS-style (its mapped
regions plus its `brk` heap), not a fabricated per-process figure.

**Performance** - real-time graphs for CPU and memory usage with history plots,
plus uptime, core count, and network I/O. **Memory is real physical RAM
accounting from the PMM** (`PMM::GetTotalMemory()` / `GetFreeMemory()` → total /
used / free) with a separate **kernel-heap** sub-total
(`KernelHeap::GetTotal/Used/Free`) - *not* a sum of per-process `mem_kb`. The
**uptime is monotonic** via `Scheduler::NowMs()` (boot-relative); the old
wall-clock-derived path produced a nonsensical "899-hour" uptime and is gone.

**Details** - detailed view of the selected process including I/O counters and full state.

**Services** - list of Kurono system services with name, PID, type (Kernel/System/User), and running status.

**Windows** - open window-manager windows get their **own tab** now; they are
**no longer listed as processes** (a window is its WM record + framebuffer, not a
scheduler task). Rows here can be closed/restarted via the action menu, mapping
onto `WindowManager::CloseWindow`.

## 2. Process list

The process list is populated by `RefreshProcesses()`, which reads the kernel's
scheduler/process table (and the Linux-process records for richer command names +
`brk` heap size). Windows are intentionally excluded - they live in the Windows
tab. The list supports sorting by column (Name, PID, CPU, Memory) ascending or
descending; click the column header to toggle. Column geometry was reworked to
fix earlier column-collision layout bugs, and the window is taller (560×500) so
the Performance panes and process rows fit without clipping.

## 3. Kill and Restart

Clicking a process row selects it. Clicking the same row again opens the action popup menu:

- **Kill** - removes the process from the list immediately. On real hardware this sends SIGKILL.
- **Restart** - resets the process's CPU usage and state back to a running condition.

The kill/restart feature can be disabled by setting `taskmgr.allow_kill = 0` in `/etc/kurono/ui.conf`.

## 4. Performance graphs

`Tick()` is called each frame from the main loop. It advances the 60-sample history ring buffer and updates the live CPU and memory counters. `RenderPerformance()` draws the graph area using the history values.

## 5. Opening the Task Manager

The start menu, the keyboard shortcut Ctrl+Shift+Esc (when implemented), and `DesktopEnvironment::LaunchTaskManager()` all open the same window. Only one instance is open at a time.

## 6. UIConfig keys

| Key | Default | Meaning |
| --- | --- | --- |
| `taskmgr.row_h` | 20 | Row height in process list |
| `taskmgr.allow_kill` | 1 | Enables Kill/Restart action menu |

## 7. Related files

- `src/apps/task_manager.cpp` / `.h` - the app
- `src/proc/scheduler.cpp` - process table source + `Scheduler::NowMs()` (uptime/CPU clock)
- `src/kernel/pmm.cpp` - `PMM::GetTotalMemory/GetFreeMemory` (real RAM accounting)
- `src/kernel/heap.cpp` - `KernelHeap::GetTotal/Used/Free` (kernel-heap sub-total)
- `src/ui/window_manager.cpp` - Windows-tab rows + `CloseWindow`
- `src/system/ui_config.cpp` - `taskmgr.*` configuration
- `src/ui/desktop.cpp` - `LaunchTaskManager()` entry point
