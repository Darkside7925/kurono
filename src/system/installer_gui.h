#pragma once
#include "../kernel/types.h"

// Graphical Kurono OS Installer.
//   InstallerGUI::Run() blocks until the user either:
//     - completes the installation -> writes /etc/kurono-installed marker
//     - chooses "Live Boot"        -> exits, leaves marker absent
class InstallerGUI {
public:
    enum Screen {
        SCR_WELCOME = 0,
        SCR_LANGUAGE,
        SCR_KEYBOARD,
        SCR_NETWORK,        // network setup: wired status + offer wifi config (satoru)
        SCR_WIFI,           // wifi ssid/password config screen (satoru)
        SCR_DISK,
        SCR_PARTITION_MODE,
        SCR_FILESYSTEM,
        SCR_USER,
        SCR_HOSTNAME,       // hostname + basic prefs (satoru)
        SCR_GUESTS,         // optional linux guests / packages (debian, alpine, python) (satoru)
        SCR_SUMMARY,
        SCR_CONFIRM,
        SCR_PROGRESS,
        SCR_DRIVERS,
        SCR_SUCCESS,
        SCR_LIVE_EXIT,
    };

    // Returns true if installation was completed (user should reboot),
    // false if the user chose Live Boot (continue to desktop).
    // start_screen lets the "Kurono Setup" boot path (and headless tests) open
    // the wizard on a specific screen; defaults to the welcome screen. (satoru)
    static bool Run(int start_screen = 0 /* SCR_WELCOME */);

    // Detect whether the OS has been installed previously.  Looks for the
    // marker file /etc/kurono-installed in KVFS (mirrored from disk).
    static bool IsInstalled();

    // Returns true if we are running from an ISO/live medium and have
    // never been installed; in that case kurono_kernel.cpp should call
    // InstallerGUI::Run() before LockScreen::Show().
    static bool ShouldAutoLaunch();

    // Launches the installer from the desktop's "Install Kurono" icon.
    static void LaunchFromDesktop();

    static Screen current_screen;
    static int    progress_pct;
    static char   progress_status[128];
};
