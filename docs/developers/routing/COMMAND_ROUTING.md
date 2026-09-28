# Command routing

How a line typed into Kurono reaches the code that runs it.

## The central file

The routing brain is `src/shell/shell.cpp`, with the declarations and state model in
`src/shell/shell.h`. If command behaviour looks strange, start there.

## Registration

A static command registry. Each record stores name, description, environment, category and
handler function.

Subsystems register themselves during boot, so command availability depends on the boot path
actually calling the subsystem registration code. That matters when a command goes missing.

## Environments

Four values: Kurono, Linux, Windows, Auto. The active environment affects lookup priority, prompt
style and some routing behaviour.

## The execution pipeline

When a line reaches `KuronoShell::Execute()`, roughly:

1. empty input is rejected, and the raw line goes into history
2. if a conflict picker is already open, the shell checks whether you typed a numeric choice
3. variable expansion runs over the line
4. a pipe sends it into `ExecutePiped()`
5. an explicit prefix like `linux:` or `windows:` temporarily overrides the environment for that
   one execution
6. argument parsing, with quoted text preserved as single arguments
7. conflict probing across backends
8. exactly one backend makes sense, so it executes directly
9. several match, so the conflict state is recorded and a numbered selector prints
10. nothing special won, so it falls back to direct lookup and handler invocation

Step 7 is the distinctive one. The shell doesn't only look at statically registered commands, it
also probes dynamic backends like the Alpine guest path and the PowerShell bridge. That's why a
name can trigger a selection menu even when the local registry alone wouldn't be ambiguous.

## Conflicts are state, not a menu

`ResolveConflict()` takes your later numeric answer and replays the stored command line against the
chosen backend. So a later line like `1` is not a command while a conflict is pending, it's a route
choice.

## Pipes

`ExecutePiped()` splits the line and runs the segments in order through alternating buffers.
Closer to staged output forwarding than a Unix stream model. The UX looks familiar, the
implementation is lighter than a real process and fd pipeline, and that difference shows up when
you read the results.

## Cross-environment calls

`linux:ls /home` and `windows:findstr user` work because `Execute()` can swap the active
environment before lookup. It's a routing convenience layer, not a separate parser.

## Backends

Three practical families in the current design:

1. registered native handlers: normal command handlers compiled into the kernel. Built-in shell
   commands, Linux-style commands, Windows-style commands, installer commands, package manager
   commands, virtualization commands.
2. the Alpine guest bridge: commands forwarded into an Alpine guest through
   hypervisor-assisted serial execution helpers, which lets the shell act as a frontend to guest
   tools.
3. the PowerShell bridge: PowerShell commands route through the Alpine guest once PowerShell has
   been installed there.

## Where registration happens

Split between the shell internals and the kernel boot code.

Inside the shell, `RegisterBuiltins()` installs the base commands: help, version, switch, reboot,
shutdown, history, and the bridge commands.

During kernel boot, the kernel registers external command families through subsystem hooks: Linux
commands, Windows commands, installer commands, package manager commands, Linux integration
commands, hypervisor commands.

The result is a shell that acts as one shared execution surface for a lot of otherwise separate
parts of the OS.

## Prompts

`GetPrompt()` also participates in environment routing. Windows mode renders its own form. Linux
and Kurono modes share a PS1-style expansion path using user, host and working directory data.

## Why command bugs happen

1. it was never registered during boot
2. it exists, but in a different environment than you expect
3. a conflict is pending and your next line is being read as a selector
4. a dynamic backend probe succeeded or failed unexpectedly
5. the target subsystem is alive but returned an error

## Fast debug checklist

1. `src/shell/shell.cpp`
2. `src/shell/shell.h`
3. `src/shell/linux_cmds.cpp` or `src/shell/windows_cmds.cpp`
4. the subsystem registration call in `src/kernel/kurono_kernel.cpp`
5. the subsystem implementation itself

## Mental model

Think of the shell as a router with a terminal attached, not a command parser. That one idea
explains most of its design.
