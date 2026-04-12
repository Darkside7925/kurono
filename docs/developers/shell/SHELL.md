# Shell

`src/shell/shell.cpp` and `shell.h` implement the Kurono shell core: command parsing, history, tab completion, and the command registry.

## 1. Architecture

The shell is not a separate process  -  it is a library that the terminal app and the emergency boot shell both call into. The shell receives characters through `KuronoShell::Input(char)`, assembles lines, and dispatches to registered commands.

## 2. Command registry

Commands are registered with `RegisterCommand(name, description, env_flags, category, handler_fn)`. The `env_flags` field controls which runtime environments (desktop, emergency, KCL, etc.) each command is visible in.

All built-in commands are registered in `RegisterBuiltins()` which is called from `KuronoShell::Init()`. Subsystems like the package manager, Linux integration, and system control register their own commands there as well.

The `kurono` command group provides:

- `kurono reload`  -  reload `/etc/kurono/ui.conf` and apply live
- `kurono info`  -  show OS version, CPU, and memory summary
- `kurono config`  -  print the current config file contents

## 3. Line editing

The shell supports:

- Backspace and delete
- Left/right arrow for cursor movement within the line
- Up/down arrow for command history recall
- Tab completion against the command registry

## 4. Output system

Commands write output via `KuronoShell::Print(str)` and `KuronoShell::Println(str)`. The terminal app reads this output queue and renders it. The same output queue is used in emergency mode with a VGA text renderer.

## 5. Command categories

The `category` field groups commands for the `help` command output. Current categories: `navigation`, `files`, `system`, `network`, `packages`, `linux`, `virt`, `kcl`.

## 6. Related files

- `src/apps/terminal.cpp`  -  terminal that calls `KuronoShell::Input()`
- `src/shell/linux_cmds.cpp`  -  Linux-style commands (ls, cat, pwd, etc.)
- `src/shell/windows_cmds.cpp`  -  Windows-style commands (dir, type, etc.)
- `src/system/ui_config.cpp`  -  used by `kurono config` and `kurono reload`
- `src/ui/desktop.cpp`  -  `ReloadFromConfig()` called from `kurono reload`
