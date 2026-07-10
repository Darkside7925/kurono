# KSA - Kurono Secure Authorization (Hypervisor-Backed Privilege Prompts)

`src/security/ksa.cpp` / `ksa.h` implement KSA, Kurono's equivalent of Windows
UAC - but backed by the hypervisor so that malware with ring-0 access in the
main OS still cannot touch the prompt, read its memory, or auto-approve it.

KSA is an *auth factor* that plugs into the existing SUPR privilege engine (see
`SUPR.md`). When a privilege escalation is requested, SUPR runs an **escalation
gate** that may require a password, a KSA prompt, or both, according to the
active auth policy.

## 1. Threat model

The assumption is the strongest realistic one for a desktop OS: the main OS
kernel itself is compromised (ring-0 malware). KSA's job is to keep the
authorization decision *outside* that compromised domain:

- the prompt is rendered + arbitrated by hypervisor-side code operating on
  memory the main OS has **no page-table mapping into**;
- the only thing that crosses back is a verdict (approve/deny + a salted
  credential hash), through a **single, read-only VMCALL channel**;
- there is no inverse channel - nothing lets the main OS *write* an approval
  into KSA memory, so a forged "yes" cannot be injected.

## 2. Isolation model (how the guarantee is realized)

1. **Spawn.** `KSA::SpawnContext()` carves an exactly-2 MB, 2 MB-aligned
   physical region from the PMM (`AllocContiguous`, head/tail slack returned).
   The region holds the prompt framebuffer, the in-context verdict struct, and
   a credential scratch area.
2. **Dedicated EPT.** A separate EPT root is built (`EPTManager::CreateEPT`) and
   the region is mapped into KSA guest-physical space **only**. The main-OS
   identity map never references it.
3. **Unmap from the main OS - the key step.**
   `KernelVMM::IsolateFrames(phys, 512)` removes the region from the main-OS
   page tables. Because the low memory is identity-mapped with 2 MB huge pages,
   this first *demotes* the covering huge page into 4 KB leaves, then zeroes the
   target PTEs. After this, `KernelVMM::QueryMapping()` returns 0 for every
   frame - ring-0 code in the main OS that dereferences the region faults.
4. **Render + input (the interactive prompt).** KSA (the arbiter) is the only
   code that briefly re-establishes an ephemeral mapping to draw the prompt and
   read the verdict (`ksa_open_window` / `ksa_close_window`), re-isolating
   between frames. The main-OS compositor never receives a pointer into the
   region; only the composited frame is blitted to the screen. The prompt is a
   **real on-screen modal** (see §8): it dims the desktop, shows the action +
   requesting identity, a credential field when the policy needs one, and
   clickable **Approve / Deny** buttons, and it blocks the escalation on live
   keyboard + mouse input.
5. **Result channel.** The verdict crosses back via **VMCALL `0x4B`** (`'K'`),
   sub-function `KSA_SUB_GET_VERDICT`. The handler in `src/virt/vmexit.cpp`
   returns a *copy* of the latched verdict (completed / approved / has-hash) - 
   never a pointer, and the credential hash itself is consumed only in-kernel by
   SUPR, never exposed over the channel.
6. **Teardown.** The region is wiped (credential residue destroyed),
   re-revealed, and freed.

## 3. Nested-virtualization note (honest fallback)

KSA wants the prompt to run inside a *true nested VM* (its own `VMLAUNCH`).
That requires the host to expose nested VMX to Kurono. In the common dev
environment - **Kurono itself running as a guest under KVM/QEMU** - nested VMX
for an *inner* VM is not available to Kurono, so KSA runs the prompt as an
**EPT-isolated guest context** instead of a separately launched VM.

`KSA::IsRealNestedVM()` reports which path is active, and the boot log prints
`prompt path=ept-isolated-context` or `prompt path=nested-vm`. This is reported,
not hidden.

**What still holds in the fallback:** the memory-isolation invariant (no main-OS
page-table mapping into the region) and the read-only result channel - both are
exercised and proven by the self-test below. **What real hardware adds:** a true
inner `VMLAUNCH` would also put a CPU privilege boundary between the main OS and
the prompt's *execution*, not just its memory. On bare metal with nested VMX (or
on the host hypervisor), KSA takes the `nested-vm` path automatically.

## 4. Auth policy (`supr policy`)

The policy tracks two independent factors (`SUPRAuthPolicy`): `passwd_enabled`
and `kvault_enabled`. The effective mode is derived from them.

| Command | Effect |
| --- | --- |
| `supr policy` | show current policy + KSA status |
| `supr policy --auth=passwd` | password prompt only (**default**) |
| `supr policy --auth=kvault` | KSA prompt only |
| `supr policy --auth=both` | require both (max security) |
| `supr policy kvault enable` / `... passwd enable` | turn a factor on |
| `supr passwd` | change password - **no KSA involved**, normal flow |
| `supr selftest` | run the KSA isolation self-test (logs to serial) |

Disabling KSA:
```
supr policy kvault disable --force --acknowledge-risk
```
- requires `--force --acknowledge-risk`;
- refuses if password auth is also off, unless `--sovereign-override`;
- if it lands in "both off", a risk warning is logged.

Disabling password:
```
supr policy --auth=kvault --disable-passwd --acknowledge-risk
```
- requires `--acknowledge-risk`;
- requires KSA to be active first (refuses if KSA is off, unless
  `--sovereign-override`).

## 5. Loophole prevention

- **Neither factor can be silently fully disabled.** Landing in "both off"
  always requires `--sovereign-override`.
- **`--sovereign-override` is gated to the Sovereign role** (`SUPR_SOVEREIGN`,
  the root-equivalent owner added for this) and is itself audited.
- If both factors are somehow off, **every** escalation calls
  `RunEscalationGate` which logs a risk warning (`ACT_RISK_WARNING` +
  `RISK: ...` to the security log) before proceeding.
- **Unavailable factor is never silently satisfied.** If policy requires
  `kvault` but the hypervisor isn't present, the gate downgrades to the
  password factor and audits it; if password is also off, it refuses.
- **EPT isolation** - the main OS has no mapping to KSA VM memory (proven at
  runtime, below).
- **Read-only channel** - no VMCALL sub-function writes an approval into KSA
  memory, so a forged approval cannot be injected from the main OS.
- **Tamper-evident audit** - all policy changes, prompts, approvals, denials,
  overrides and risk warnings are written to `/kurono/var/log/security.log`
  (`RuntimeLog::LogSecurity`) **even when KSA is disabled**.
- The KSA region has **no network, no disk, no shared memory** with the main OS
  (it is a standalone, unmapped, separately-EPT'd region).

## 6. Escalation gate

`SUPR::Escalate()` (su/sudo and the GUI funnel here) calls
`SUPR::RunEscalationGate(session, password, reason)`, the single chokepoint:

1. if both factors off → log risk warning, proceed;
2. else require the password factor and/or the KSA factor per policy;
3. the KSA factor calls `KSA::Prompt()` and checks `verdict.approved` (and, when
   KSA is the sole credential collector, verifies its hash against root);
4. returns true only if every *required* factor passed.

## 7. Verification (runtime self-test)

Boot with `kurono.ksa.test=1` (or run `supr selftest`). The self-test spawns the
isolated context and asserts, logging each result to serial:

```
KSA: init - hypervisor available, prompt path=ept-isolated-context
KSA-SELFTEST: main-os reach base=no mid=no verdict=no
KSA-SELFTEST: PASS isolation (region unmapped from main OS)
KSA-SELFTEST: while arbiter window open, main-os reach=YES (arbiter only)
KSA-SELFTEST: PASS channel (verdict crossed via copy)
KSA-SELFTEST: channel is read-only (no host->ksa approval write path)
KSA-SELFTEST: after re-isolate, main-os reach=no (PASS)
KSA-SELFTEST: OVERALL PASS
```

The policy/loophole state machine is also exercised
(`SUPR::PolicySelfTest()`), covering: non-sovereign override refused;
disable-passwd refused while KSA off; disable-kvault refused without
force/ack; disable-kvault refused while passwd off; sovereign override; both-off
risk warning; `auth=both` gated on KSA availability; password verification.

## 8. The interactive on-screen prompt

When `RunEscalationGate` requires the KSA factor (`policy auth=kvault` or
`both`), `KSA::Prompt()` renders a **real modal confirmation panel on the
actual framebuffer** and blocks the escalation until the user answers. It is
not the old auto-answered self-test - it takes live keyboard and mouse input.

**What it shows.** A dimmed-desktop backdrop with a centred panel: the
"KSA - Kurono Secure Authorization" header, the action being authorized
(`req.title` / `req.detail`), the requesting identity (`Account: <user>`), a
masked credential field when the policy needs KSA to collect the credential
(`req.want_cred`), and clickable **Deny** (left) / **Approve** (right) buttons.

**Input.**
- keyboard: printable keys type into the credential field (echoed masked, the
  real characters never leave the isolated region); **Enter = approve**,
  **Esc = deny**; backspace edits.
- mouse: the loop polls the pointer itself and hit-tests a left click against
  the Approve / Deny button rects (with hover highlight). 
- on a ~60 s timeout with no answer the prompt **fails closed** (deny).

**Display ownership / secure desktop.** The guarantee is enforced by the
cooperative scheduler. `KSA::Prompt()` runs on the stack of whichever process
triggered the escalation (e.g. the GUI process) and its render/input loop
**never yields** (`Scheduler::Yield`/`SleepMs`). While it runs, the GUI
compositor process and the input process are both starved, so:

- the main OS cannot draw over the prompt (it never gets a frame);
- the main OS cannot read the framebuffer KSA is presenting to or the
  credential (it isn't running);
- this loop is the **only** code polling the 8042, so the main OS cannot forge
  a keystroke or click into the verdict.

On entry KSA snapshots the back buffer (`PMM::AllocBytes` + `memcpy`) and
**flushes any pre-queued keyboard/mouse input** so a keystroke the main OS left
in the ring can't be consumed as a credential char or counted as Enter/Esc.
Each frame is composed into the isolated framebuffer, blitted into the back
buffer, overlaid with text + buttons, then presented in one `SwapBuffers()` - 
with a full-screen `MarkDirty` so the dim + panel (drawn via
`FillRectAlpha`/`DrawPixel`, which don't self-mark dirty) actually reach the
front buffer and the virtio GPU. On exit KSA restores the saved desktop pixels,
wipes the local credential buffer, and calls `MarkUIDirty()` so the resumed
compositor repaints. The verdict still crosses back **only** via the read-only
VMCALL channel (§2.5) - the credential is hashed inside the isolated region and
only the salted hash + approve/deny flag leave it.

**Verification (render path + verdict flow).** Boot with
`kurono.ksa.prompt` (credential variant) or `kurono.ksa.prompt=nocred`
(approve/deny only). Once the desktop is up the GUI process fires
`KSA::PromptDemo()` once, so the modal is drawn on the real screen (capture it
headless with `qmp_shot.py`) and synthetic input drives the verdict:

```
[gui] firing interactive ksa prompt demo
KSA-PROMPT-DEMO: begin (want_cred=yes)
KSA: prompt up - secure desktop owns the screen + input
KSA-PROMPT-DEMO: prompt ran, verdict=APPROVE (cred-hash present)
```

This was verified headless under QEMU/KVM: the panel renders (screendump shows
header, title, detail, account, masked credential field, Deny/Approve buttons),
a QMP `send-key ret` Enter flows through as `verdict=APPROVE`, a correctly typed
credential yields `cred-hash present`, and the desktop is restored cleanly after
the prompt closes.

## 9. Related files

- `src/security/ksa.{cpp,h}` - KSA module (spawn / isolate / prompt / channel / self-test / `PromptDemo`)
- `src/security/supr.{cpp,h}` - auth policy, escalation gate, Sovereign role, audit actions
- `src/kernel/vmm.{cpp,h}` - `IsolateFrames` / `RevealFrames` isolation primitives
- `src/virt/vmexit.cpp` - VMCALL `0x4B` read-only result channel
- `src/virt/ept.cpp`, `hypervisor.cpp` - EPT root + hardware-virt detection
- `src/shell/shell.cpp` - the `supr` command
- `src/kernel/kurono_kernel.cpp` - `kurono.ksa.test` self-test + `kurono.ksa.prompt` interactive demo gates
- `src/proc/kernel_processes.cpp` - GUI process fires the armed `KSA::PromptDemo()` once the desktop is up
