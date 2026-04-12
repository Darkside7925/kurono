# Task Manager

`src/apps/task_manager.cpp` and `task_manager.h` implement the Task Manager application.

## 1. What it shows

The Task Manager has four tabs.

**Processes**  -  a sortable list of running processes. Each row shows PID, name, CPU%, memory (KB), thread count, state, user, and priority.

**Performance**  -  real-time graphs for CPU and memory usage with history plots. Also shows uptime, core count, and network I/O statistics.

**Details**  -  detailed view of the selected process including I/O counters and full state.

**Services**  -  list of Kurono system services with name, PID, type (Kernel/System/User), and running status.

## 2. Process list

The process list is populated by `RefreshProcesses()`. On real hardware this reads the kernel's process table. The list supports sorting by any column (Name, PID, CPU, Memory) in ascending or descending order. Click the column header to toggle.

## 3. Kill and Restart

Clicking a process row selects it. Clicking the same row again opens the action popup menu:

- **Kill**  -  removes the process from the list immediately. On real hardware this sends SIGKILL.
- **Restart**  -  resets the process's CPU usage and state back to a running condition.

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

- `src/proc/scheduler.cpp`  -  process table source
- `src/system/ui_config.cpp`  -  `taskmgr.*` configuration
- `src/ui/desktop.cpp`  -  `LaunchTaskManager()` entry point
