# Shell

`src/shell/shell.cpp` and `shell.h`. The shell core: parsing, history, tab completion and
the command registry.

It is not a process. It's a library the terminal app and the emergency boot shell both call
into. Characters arrive through `KuronoShell::Input(char)`, lines get assembled, commands
get dispatched.

## Command registry

`RegisterCommand(name, description, env_flags, category, handler_fn)`. `env_flags` decides
which runtime environments (desktop, emergency, KCL, and so on) can see the command.

The Kurono-native built-ins register in `RegisterBuiltins()`, called from
`KuronoShell::Init()`. Plenty of commands register themselves from their own subsystem
instead: `src/shell/linux_cmds.cpp` and `windows_cmds.cpp` (the Linux and Windows
surfaces), `src/apps/kj.cpp` (`kj`, `node`), `src/kcl/kcl.cpp` (`kcl`),
`src/apps/python_interp.cpp` (`python`, `py`), `src/packages/pkgmgr.cpp` (`kpkg` and
friends), `src/system/installer.cpp` (`installer`), `src/net/network.cpp`,
`src/shell/shell.cpp` (the `firefox` command, which auto-installs the package then launches
it through `LdKurono::ExecPIE`), and `src/linux/*`. About 154 distinct commands in total,
counting `RegisterCommand` call sites across the tree.

The `kurono` group:

- `kurono reload` reload `/etc/kurono/ui.conf` and apply it live
- `kurono info` OS version, CPU and memory summary
- `kurono config` print the config file

## Line editing

Backspace and delete, left/right arrows inside the line, up/down for history, and tab
completion against the registry.

## Output

Commands write with `KuronoShell::Print(str)` and `KuronoShell::Println(str)`. The terminal
drains that queue and renders it. Emergency mode uses the same queue with a VGA text
renderer.

## Categories

`category` groups commands in `help`. The strings actually passed to `RegisterCommand` across
the tree: `builtin`, `filesystem`, `text`, `system`, `network`, `package`, `linux`, `virt`,
`security`, `media`, `lang`, `scripting`.

## Files

- `src/apps/terminal.cpp` calls `KuronoShell::Input()`
- `src/shell/linux_cmds.cpp` Linux-style commands
- `src/shell/windows_cmds.cpp` Windows-style commands
- `src/system/ui_config.cpp` behind `kurono config` and `kurono reload`
- `src/ui/desktop.cpp` `ReloadFromConfig()`, called from `kurono reload`
