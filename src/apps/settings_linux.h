//  kurono os - settings > linux: the bits the desktop needs (satoru)
//  the linux page (settings_mod_linux.cpp) owns the "linux.*" ui.conf keys.
//  the dock asks here whether the Alpine tile is shown and which guest apps
//  were pinned to it. (satoru)
#pragma once
#include "../kernel/types.h"

namespace LinuxSettings {
    // false while linux is turned off: the dock hides the Alpine tile (satoru)
    bool ShowLinuxTile();
    // guest apps pinned with "show in dock" (persisted, so they show before
    // the vm runs). name is the .desktop Name. (satoru)
    int  DockAppCount();
    const char* DockAppName(int i);
    // start the vm if needed, then the app (asynchronous) (satoru)
    void LaunchDockApp(int i);
    // the Alpine tile / terminal while linux is off: open the linux page (satoru)
    void OpenPage();
}

// end (satoru)
