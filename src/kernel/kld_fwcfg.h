// QEMU fw_cfg delivery for the Wi-Fi connect configuration.
//
// The passphrase is never stored in the kernel or a disk image: the rig uses
// -fw_cfg file=...; this reader copies the blob into a RAM-only KVFS file for
// the container and logs only the byte count. (satoru)
#ifndef KURONO_KLD_FWCFG_H
#define KURONO_KLD_FWCFG_H

namespace KldFwcfg {
// Read opt/kurono/wifi.conf from QEMU fw_cfg and write /run/kld/wifi.conf.
// Returns true when a blob was found and written. Never logs file contents.
bool LoadWifiConf();
// Remove the RAM-only file (idempotent).
void RemoveWifiConf();
}

#endif
