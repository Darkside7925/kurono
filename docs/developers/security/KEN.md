# KEN

`src/security/ken.cpp` and `ken.h`. The privilege management layer.

KEN handles privilege escalation requests. When a command or subsystem wants elevated access
(mounting a disk, changing system config, killing another user's process), it goes through here.

## Roles

Five levels (`KENLevel`):

- Guest: minimal access
- User: standard unprivileged operations
- Admin: elevated operations
- Root: everything permitted
- Sovereign: root-equivalent owner, the only role allowed to use `--sovereign-override` for the
  most dangerous policy changes (see `KSA.md`)

KEN enforces behavioural limits even where hardware memory isolation isn't present.

## Escalation

`KEN::Escalate()`, which both the GUI prompt and the `ken` reroute funnel into, calls the
single chokepoint `KEN::RunEscalationGate()`. That applies the active auth policy: a password
factor, a KSA hypervisor-prompt factor, or both. Satisfy every required factor and KEN grants
the elevated context.

`ken` is the sudo-style entry point. `ken <cmd> [args]` runs any registered command elevated,
not just KEN's own subcommands (`cmd_ken` in `src/shell/shell.cpp`). The `sudo` spelling and the
superseded `supr` spelling are kept as hidden aliases, callable but not listed by `help`.

- it looks the target up in the shell registry, then wraps the call in `KEN::SudoBegin()`,
  runs as root, and calls `KEN::SudoEnd()`. `SudoBegin` collects the credential or approval
  through the interactive KSA modal per the active policy and temporarily raises the session's
  user to root. `SudoEnd` restores the pre-elevation user.
- `ken whoami` reports the elevated identity, `root`
- a Guest role is denied with "permission denied, your role may not escalate"
- shell builtins that mutate shell state (`cd`, `clear`, `history`, `linux`, `cmd`, `exit`) are
  refused with a "run it directly" message, since elevating them is meaningless
- a failed authentication aborts without running the command

## Auth policy and KSA

The policy (`KENAuthPolicy`) tracks two independent factors, `passwd_enabled` and
`kvault_enabled`, managed through the `ken policy` command. The KSA factor renders its prompt
inside a hypervisor-isolated context the main OS can't map into, and can't forge an answer into.
See `KSA.md` for the design, the loophole-prevention rules and the runtime self-test.

Every policy change, prompt, approval, denial, override and risk warning is audited to
`/kurono/var/log/security.log`, even when KSA is off.

## Where it's going

When full userspace process isolation lands with hardware ring separation, KEN becomes the
bridge between user-mode syscall requests and kernel-mode handlers. On hosts that expose nested
VMX, the KSA factor upgrades from an EPT-isolated context to a real nested VM automatically.

## Files

- `src/security/ksa.{cpp,h}` the KSA auth factor
- `src/shell/shell.cpp` the `ken` command: `policy`, `passwd`, `selftest`, the `ken <cmd>`
  reroute, and `ken whoami`
- `src/security/ken.cpp` `SudoBegin` / `SudoEnd` elevate-and-restore helpers
- `src/system/user_mgmt.cpp` where user credentials are stored and checked
