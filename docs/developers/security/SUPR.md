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

`SUPR::Escalate()` (su/sudo and GUI prompts funnel here) calls the single
escalation chokepoint, `SUPR::RunEscalationGate()`, which applies the active
**auth policy**: a password factor, a KSA hypervisor-prompt factor, or both.
If the user satisfies every *required* factor, SUPR grants the elevated context.

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
- `src/shell/shell.cpp`  -  the `supr` command (policy / passwd / selftest)
- `src/shell/linux_cmds.cpp`  -  `sudo` command calls SUPR
- `src/system/user_mgmt.cpp`  -  user credentials stored and checked here
