// kld supervisor: restarts a kld container that crashed or hung (kurono.kld.sup=1,
// off by default). state lives in its own tables keyed by address space, never in
// struct Process. (satoru)
#pragma once
#include <stdint.h>

namespace KldSupervisor {

// parse kurono.kld.sup=1 and kurono.faultinject=kld:<family>:<segv|hang>. (satoru)
void BootInit(const char* boot_cmdline);
bool Enabled();

// `kld reload <family>`: stop the running instance and load the image at the
// family's path again (a new binary dropped there first), with no reboot and
// no crash accounting. -1 = supervisor off, -2 = family not running. (satoru)
int RequestReload(const char* family);
uint32_t ReloadCount(const char* family);
bool Running(const char* family);

// KLD_OP_HEARTBEAT from a container: remember when it was last alive. (satoru)
void Heartbeat(uint64_t address_space);

// load and run the family's container under supervision. never returns. (satoru)
[[noreturn]] void Run(const char* family, const char* path);

}  // namespace KldSupervisor

// end (satoru)
