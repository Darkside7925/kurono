#pragma once
//  ktools: the `kdrv` and `ksys` shell commands. driver control and system
//  diagnostics for the machine's own operator, human or the claude agent that
//  runs inside kurono. both commands are reachable through /dev/kshell. (satoru)

namespace KTools {

// register `kdrv` and `ksys`. `shell` is the KuronoShell instance (opaque here). (satoru)
void RegisterShellCommands(void* shell);

// /dev/kshell sets this around Execute() so kdrv can refuse the few actions
// that are fine for an operator at the console but would cut the agent's own
// lifeline (unloading the nic, switching off usb input). (satoru)
void SetAgentCaller(bool on);

}  // namespace KTools

// end (satoru)
