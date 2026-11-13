# OpenCode

`src/system/opencode_host.cpp` and `opencode_host.h`. OpenCode is the terminal coding tool from
sst/opencode. Kurono runs the unmodified `linux-x64-musl` build natively, both the TUI and
`opencode run`. Its tools work.

```
opencode           start the TUI
opencode run ...   run it to completion from the shell
```

The shell command is registered in `src/shell/shell.cpp` under the `apps` category.

## Files on the KURONO partition

All three go in the partition root. The names are 8.3 names.

| File | What |
| --- | --- |
| `OPENCODE` | the linux-x64-musl opencode binary, about 210 MB |
| `LIBSTDC.SO` | musl's `libstdc++.so.6` |
| `LIBGCCS.SO` | musl's `libgcc_s.so.1` |

The two libraries can come from Alpine, or from the `firefox/lib` directory of the Firefox
package. The loader puts them in `/system/lib` at boot.

`OPENCODE` is read once, the first time it is used, into a physical memory block. It is exposed
as `/system/bin/opencode` without a heap copy. The image is dropped again once no opencode process
is left.

## Environment and keys

Provider keys go in `OPENCODE.ENV`, one `NAME=value` per line, for example
`ANTHROPIC_API_KEY=...`. `/etc/opencode.env` is read too. Lines with spaces, comments and lines
without `=` are skipped. The values are not written to the log. The file is plain text on the
KURONO partition, so anything that can read that partition can read the keys.

These defaults are set for every launch:

- `OPENCODE_DISABLE_AUTOUPDATE=1`, since the binary is a read-only root file
- `OPENCODE_DISABLE_LSP_DOWNLOAD=true`
- `NODE_EXTRA_CA_CERTS` and `SSL_CERT_FILE` pointing at `/etc/ssl/certs/ca-certificates.crt`
- `PATH=/system/bin:/usr/bin:/bin`, `HOME=/home/user`, `TERM=xterm-256color`, `LANG=C.UTF-8`

## Errors you may see

- "no OPENCODE file on the KURONO partition": copy the binary there as `OPENCODE`.
- "the musl c++ runtime is missing": copy the two libraries as `LIBSTDC.SO` and `LIBGCCS.SO`,
  then reboot.
- "the fixed-image memory window is in use": reboot, then run it again.
- "OPENCODE is not an ELF file": the wrong file was copied.

## Files

- `src/system/opencode_host.cpp` loading, the launch environment and `Run()`
- `src/system/opencode_host.h` the public calls
- `src/shell/shell.cpp` the `opencode` command
- `src/linux/ld_kurono.cpp` the loader that maps the binary
