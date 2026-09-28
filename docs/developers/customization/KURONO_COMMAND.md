# The `kurono` command

The main system control surface in the shell. Runtime management without a reboot.

## `kurono reload`

Re-reads `/etc/kurono/ui.conf` out of KVFS and pushes the values at the live desktop:

- taskbar colours, height, position and visibility toggles
- desktop background colour, icon size and spacing
- context menu colours and dimensions
- window manager chrome colours and sizes
- task manager row height and feature flags

The screen refreshes immediately after.

```
$ kurono reload
[UIConfig] loaded 42 entries
[Desktop] config reloaded
[Taskbar] config reloaded
```

## `kurono info`

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

## `kurono config`

Dumps the current `/etc/kurono/ui.conf`.

## `kurono vm start` and `kurono vm stop`

Starts or stops the Debian guest through the hypervisor. Wants Intel VMX.

## Where it lives

Registered in `KuronoShell::RegisterBuiltins()` in `src/shell/shell.cpp` and handled by
`ShellBuiltins::cmd_kurono()`, which parses the first argument as a subcommand.

`reload` calls `DesktopEnvironment::ReloadFromConfig()`, which runs `UIConfig::Reload()`
then `Taskbar::ReloadFromConfig()` and `Desktop::ReloadFromConfig()`.

## More

- `customization/UI_CUSTOMIZATION.md` how to actually use reload
- `system/UI_CONFIG.md` the config reference
- `shell/SHELL.md` shell architecture
