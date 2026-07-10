// Firefox launcher - sets up the env Firefox expects, then exec's the
// installed binary at /apps/firefox/firefox.
#ifndef KURONO_FIREFOX_LAUNCHER_H
#define KURONO_FIREFOX_LAUNCHER_H

#include <stdint.h>

namespace FirefoxLauncher {

// Launch Firefox under the given uid/gid (typically 1000/1000).
// Returns the linux pid on success, negative errno on failure.
int Launch(uint32_t uid, uint32_t gid, const char* url /* may be null */);

// Returns true if /apps/firefox/firefox is installed and the
// firefox-deps manifest at /system/lib/firefox-deps.manifest is readable.
bool IsInstalled();

// Read /home/user/.config/kurono/firefox.env into the process env table.
// out_envp must have room for at least 32 entries; values are pointers
// into out_buf which must be at least 4 KiB.  Returns the count.
int LoadEnvironment(char** out_envp, char* out_buf, int buf_size);

}  // namespace FirefoxLauncher

#endif
