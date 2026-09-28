# SUPR

`src/security/supr.cpp` and `supr.h`. The privilege management layer.

SUPR handles privilege escalation requests. When a command or subsystem wants elevated access
(mounting a disk, changing system config, killing another user's process), it goes through here.

## Roles

Five levels (`SUPRLevel`):

- Guest: minimal access
- User: standard unprivileged operations
- Admin: elevated operations
- Root: everything permitted
- Sovereign: root-equivalent owner, the only role allowed to use `--sovereign-override` for the
  most dangerous policy changes (see `KSA.md`)

SUPR enforces behavioural limits even where hardware memory isolation isn't present.

## Escalation

`SUPR::Escalate()`, which both the GUI prompt and the `supr` reroute funnel into, calls the
single chokepoint `SUPR::RunEscalationGate()`. That applies the active auth policy: a password
factor, a KSA hypervisor-prompt factor, or both. Satisfy every required factor and SUPR grants
the elevated context.

`supr` is the sudo-style entry point. `supr <cmd> [args]` runs any registered command elevated,
not just SUPR's own subcommands (`cmd_supr` in `src/shell/shell.cpp`):

- it looks the target up in the shell registry, then wraps the call in `SUPR::SudoBegin()`,
  runs as root, and calls `SUPR::SudoEnd()`. `SudoBegin` collects the credential or approval
  through the interactive KSA modal per the active policy and temporarily raises the session's
  user to root. `SudoEnd` restores the pre-elevation user.
- `supr whoami` reports the elevated identity, `root`
- a Guest role is denied with "permission denied, your role may not escalate"
- shell builtins that mutate shell state (`cd`, `clear`, `history`, `linux`, `cmd`, `exit`) are
  refused with a "run it directly" message, since elevating them is meaningless
- a failed authentication aborts without running the command

## Auth policy and KSA

The policy (`SUPRAuthPolicy`) tracks two independent factors, `passwd_enabled` and
`kvault_enabled`, managed through the `supr policy` command. The KSA factor renders its prompt
inside a hypervisor-isolated context the main OS can't map into, and can't forge an answer into.
See `KSA.md` for the design, the loophole-prevention rules and the runtime self-test.

Every policy change, prompt, approval, denial, override and risk warning is audited to
`/kurono/var/log/security.log`, even when KSA is off.

## Where it's going

When full userspace process isolation lands with hardware ring separation, SUPR becomes the
bridge between user-mode syscall requests and kernel-mode handlers. On hosts that expose nested
VMX, the KSA factor upgrades from an EPT-isolated context to a real nested VM automatically.

## Files

- `src/security/ksa.{cpp,h}` the KSA auth factor
- `src/shell/shell.cpp` the `supr` command: `policy`, `passwd`, `selftest`, the `supr <cmd>`
  reroute, and `supr whoami`
- `src/security/supr.cpp` `SudoBegin` / `SudoEnd` elevate-and-restore helpers
- `src/system/user_mgmt.cpp` where user credentials are stored and checked
