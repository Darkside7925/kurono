# `kurono` Shell Command

The `kurono` command is the primary system control interface in the Kurono shell. It provides runtime management operations that affect the running OS without requiring a reboot.

## 1. Subcommands

### `kurono reload`

Re-reads `/etc/kurono/ui.conf` from KVFS and applies all current values to the live desktop environment. This includes:

- Taskbar colors, height, position, and visibility toggles
- Desktop background color, icon size and spacing
- Context menu colors and dimensions
- Window manager chrome colors and sizes
- Task Manager row height and feature flags

After reload completes, the screen refreshes with the new settings immediately.

```
$ kurono reload
[UIConfig] loaded 42 entries
[Desktop] config reloaded
[Taskbar] config reloaded
```

### `kurono info`

Prints a summary of the running system:

```
$ kurono info
Kurono OS  (c) 2026
Kernel:    6.12.0-kurono
Debian:    13.4 (trixie)
CPU:       Intel Core i7-1165G7 (4 cores / 8 threads)
           VMX: yes  SSE2: yes  AES-NI: yes
Memory:    3.8 GB total  /  1.2 GB used
Uptime:    0d 00:03:14
UIConfig:  version 3  /  42 entries loaded
```

### `kurono config`

Prints the full current contents of `/etc/kurono/ui.conf`:

```
$ kurono config
# ═════════════════════════════════════════
#  Kurono OS - UI configuration
...
```

### `kurono vm start` / `kurono vm stop`

Starts or stops the Debian Linux guest via the hypervisor. Requires Intel VMX support.

## 2. How it is implemented

The `kurono` command is registered in `KuronoShell::RegisterBuiltins()` in `src/shell/shell.cpp`. The handler is `ShellBuiltins::cmd_kurono()`. It parses the first argument as a subcommand and dispatches accordingly.

`reload` calls `DesktopEnvironment::ReloadFromConfig()` which calls `UIConfig::Reload()` then `Taskbar::ReloadFromConfig()` and `Desktop::ReloadFromConfig()`.

## 3. Related documentation

- `customization/UI_CUSTOMIZATION.md` - how to use reload in practice
- `system/UI_CONFIG.md` - UIConfig technical reference
- `shell/SHELL.md` - shell architecture
