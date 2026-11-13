# OpenCode

OpenCode runs on Kurono as an unmodified program. It is the `linux-x64-musl` build of
sst/opencode, a large bun binary. The full-screen interface works, `opencode run` works,
and the tools work. The code is in `src/system/opencode_host.cpp` and `opencode_host.h`,
and the shell command is `cmd_opencode` in `src/shell/shell.cpp`.

The binary is not in the tree. You put it on the KURONO partition yourself.

## Root files

Put these in the root of the KURONO partition. The names are 8.3 FAT names.

| file | what |
| --- | --- |
| `OPENCODE` | the opencode binary, about 210 MB |
| `LIBSTDC.SO` | musl's `libstdc++.so.6` |
| `LIBGCCS.SO` | musl's `libgcc_s.so.1` |
| `OPENCODE.ENV` | provider keys, one `NAME=value` per line, e.g. `ANTHROPIC_API_KEY=...` |

The Firefox package's `firefox/lib` directory has both libraries, and they can be copied
from there. They are loaded into `/system/lib` at boot.

## Running it

Run `opencode` on the shell and give it the arguments you want, e.g. `opencode run "..."`.
The binary is read from the partition the first time it is needed, and the image is
given back after the run. Programs that exec `opencode` from bash or sh get the same
environment as the shell command.

## Keys

`OPENCODE.ENV` is plain text. Anything that can read the KURONO partition can read it.
The keys are never written to the log. `/etc/opencode.env` is read too, if it exists.

## Memory

OpenCode is linked at a fixed address, like Claude Code. A machine with `OPENCODE` or
`CLAUDE` on the KURONO partition keeps the fixed user image window reserved at boot, so
the binary can always load at the address it expects.

## Debugging

`kurono.lktrace=1` logs advisory locks and `-shm` maps on the opencode and sqlite paths.
See `boot-flags.md`.
