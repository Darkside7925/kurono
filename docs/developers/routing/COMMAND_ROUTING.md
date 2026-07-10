# Command Routing

This document explains how a line typed into Kurono reaches the code that actually runs.

## 1. The central file

The command routing brain is `src/shell/shell.cpp`.

The declarations and state model are in `src/shell/shell.h`.

If command behavior looks strange, these are the first files to inspect.

## 2. Registration model

The shell uses a static command registry.

Each command record stores five main pieces of information.

1. Name
2. Description
3. Environment
4. Category
5. Handler function

Subsystems register themselves during boot. This means command availability depends on the boot path actually calling the subsystem registration code.

That detail matters when debugging missing commands.

## 3. Environments

Kurono understands four environment values.

1. Kurono
2. Linux
3. Windows
4. Auto

The current environment affects command lookup priority, prompt style, and some routing behavior.

## 4. The execution pipeline

When a line reaches `KuronoShell::Execute()`, the flow is broadly this.

### Stage 1

The shell rejects empty input and appends the raw line to history.

### Stage 2

If a conflict picker is already open, the shell checks whether the user typed a numeric resolution choice.

### Stage 3

Variable expansion runs across the input line.

### Stage 4

If the line contains a pipe, the shell routes into `ExecutePiped()`.

### Stage 5

If the line begins with an explicit environment prefix such as `linux:` or `windows:`, the shell temporarily overrides the active environment for that single execution.

### Stage 6

Argument parsing runs. Quoted text is preserved as single arguments.

### Stage 7

The shell probes for conflicts across multiple backends.

This part is one of the most distinctive features in the codebase.

The shell does not only look at statically registered commands. It can also probe dynamic backends such as the Alpine guest path and the PowerShell bridge. That is why a command name can trigger a selection menu even when the local registry alone would not have been ambiguous.

### Stage 8

If exactly one backend makes sense, the shell executes it directly.

### Stage 9

If multiple backends match, the shell records the conflict state and prints a numbered selector.

### Stage 10

If no special routing won, the shell falls back to direct command lookup and handler invocation.

## 5. Conflict resolution

Conflict resolution is not a toy menu. It is a persistent routing state.

`ResolveConflict()` consumes the user's later numeric answer and replays the stored command line against the selected backend.

That means a later line like `1` is not interpreted as a normal command when a conflict is pending. It is treated as a route choice.

## 6. Pipe routing

The pipe system is intentionally simple.

`ExecutePiped()` splits the line into segments and executes them in order using alternating buffers. It is closer to staged output forwarding than a full Unix stream model.

That distinction is important when reading results. The pipe UX looks familiar, but the internal implementation is lighter than a real process and file descriptor pipeline.

## 7. Cross environment routing

Cross environment calls such as `linux:ls /home` and `windows:findstr user` work because `Execute()` can temporarily swap the active environment before command lookup.

This is a routing convenience layer, not a separate parser.

## 8. Backend types

There are three practical backend families in the current shell design.

### 8.1 Registered native handlers

These are normal command handlers compiled into the kernel.

Examples include built in shell commands, Linux style commands, Windows style commands, installer commands, package manager commands, and virtualization commands.

### 8.2 Alpine guest bridge

Commands can be forwarded into an Alpine guest through hypervisor assisted serial execution helpers. This lets the shell act as a frontend to guest tools.

### 8.3 PowerShell bridge

PowerShell commands can route through the Alpine guest when PowerShell has been installed there.

## 9. Boot time registration map

The high level registration route is split between shell internals and kernel boot code.

### Inside the shell

`RegisterBuiltins()` installs the base shell commands such as help, version, switch, reboot, shutdown, history, and bridge commands.

### During kernel boot

The kernel registers external command families through subsystem hooks.

Examples include these.

1. Linux commands
2. Windows commands
3. Installer commands
4. Package manager commands
5. Linux integration commands
6. Hypervisor related commands

The result is that the shell is a shared execution surface for many otherwise separate parts of the operating system.

## 10. Prompt routing

`GetPrompt()` also participates in environment routing.

Windows mode renders its own prompt form.

Linux and Kurono modes share a PS1 style expansion path using user, host, and working directory data.

## 11. Why command bugs happen

A command can fail for five different reasons.

1. It was never registered during boot.
2. It exists, but in a different environment than expected.
3. A conflict is pending and the next line is being interpreted as a selector.
4. A dynamic backend probe succeeded or failed unexpectedly.
5. The target subsystem is alive, but the subsystem implementation returned an error.

## 12. Fast debug checklist

When a command misroutes, inspect the code in this order.

1. `src/shell/shell.cpp`
2. `src/shell/shell.h`
3. `src/shell/linux_cmds.cpp` or `src/shell/windows_cmds.cpp`
4. The subsystem registration call in `src/kernel/kurono_kernel.cpp`
5. The subsystem implementation itself

## 13. Mental model

The shell is best thought of as a router with a terminal attached, not just as a command parser.

That single idea explains most of its design.
