# Privilege System (SUPR)

`src/security/supr.cpp` and `supr.h` implement the Kurono privilege management layer.

## 1. What it does

SUPR (Super Privilege) manages privilege escalation requests. When a command or subsystem needs elevated access  -  for example, mounting a disk, modifying system config, or killing another user's process  -  it calls through the SUPR layer.

## 2. Privilege model

SUPR has five roles (`SUPRLevel`):

- **Guest**  -  minimal access
- **User**  -  standard unprivileged operations
- **Admin**  -  elevated operations
- **Root**  -  all operations permitted
- **Sovereign**  -  root-equivalent owner; the only role allowed to use
  `--sovereign-override` for the most dangerous policy changes (see KSA).

SUPR enforces behavioral limits even when hardware memory isolation is not
present.

## 3. sudo / escalation integration

`SUPR::Escalate()` (the GUI prompt and the `supr` reroute funnel here) calls the
single escalation chokepoint, `SUPR::RunEscalationGate()`, which applies the
active **auth policy**: a password factor, a KSA hypervisor-prompt factor, or
both. If the user satisfies every *required* factor, SUPR grants the elevated
context.

**`supr` is the sudo-style entry point.** `supr <cmd> [args]` runs *any*
registered command elevated, not just SUPR's own subcommands (`cmd_supr` in
`src/shell/shell.cpp`):

- it looks the target command up in the shell registry, then wraps the call in
  `SUPR::SudoBegin()` → run-as-root → `SUPR::SudoEnd()`. `SudoBegin` collects the
  credential/approval through the interactive KSA modal per the active policy and
  temporarily raises the session's user to root; `SudoEnd` restores the
  pre-elevation user (`src/security/supr.cpp`).
- `supr whoami` reports the elevated identity (`root`).
- a **Guest** role is denied (`permission denied  -  your role may not escalate`).
- shell builtins that mutate shell state (`cd`, `clear`, `history`, `linux`,
  `cmd`, `exit`) are refused with a "run it directly" message, since elevating
  them is meaningless.
- a failed authentication aborts without running the command.

## 4. Auth policy + KSA

The auth policy (`SUPRAuthPolicy`) tracks two independent factors  - 
`passwd_enabled` and `kvault_enabled`  -  managed via the `supr policy` command.
The KSA (Kurono Secure Authorization) factor renders its prompt inside a
hypervisor-isolated context the main OS cannot map or forge an answer into. See
**`KSA.md`** for the full design, loophole-prevention rules, and the runtime
self-test. All policy changes, prompts, approvals, denials, overrides, and risk
warnings are audited to `/kurono/var/log/security.log` even when KSA is off.

## 5. Future direction

When full user-space process isolation is implemented with hardware ring
separation, SUPR will be the bridge between user-mode syscall requests and
kernel-mode handlers. On hosts that expose nested VMX, the KSA factor upgrades
from an EPT-isolated context to a true nested VM automatically.

## 6. Related files

- `src/security/ksa.{cpp,h}`  -  the KSA hypervisor-backed auth factor (see `KSA.md`)
- `src/shell/shell.cpp`  -  the `supr` command (`policy` / `passwd` / `selftest`,
  the `supr <cmd>` sudo-style reroute, and `supr whoami`)
- `src/security/supr.cpp`  -  `SudoBegin` / `SudoEnd` elevate-and-restore helpers
- `src/system/user_mgmt.cpp`  -  user credentials stored and checked here
