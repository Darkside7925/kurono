# Settings App

`src/apps/settings.cpp` and `settings.h` implement the system settings application.

## 1. What it configures

The Settings app provides a graphical interface for changing system preferences.
It has **12 tabs** (`SettingsTab` enum in `settings.h`, `STAB_COUNT = 12`), each
backed by its own `settings_mod_*.cpp` module:

1. **Display**  -  resolution, refresh, VSync, DPI
2. **Sound**  -  volume level, device selection
3. **Network**  -  connection status, IP configuration
4. **Storage**
5. **Power**
6. **Personalize** (Personal)  -  wallpaper, theme color presets
7. **Security**
8. **Packages**
9. **Updates**  -  real repository fetch + pending-update counts
10. **System**
11. **About**  -  OS version, build info, hardware summary
12. **Accessibility** (a11y)

## 2. How settings are saved

Settings that map to `UIConfig` keys write to `/etc/kurono/ui.conf` via `KVFS::WriteString`. After writing, the app calls `DesktopEnvironment::ReloadFromConfig()` to apply changes live without reboot.

Settings that affect hardware (display mode, audio volume) call the relevant driver directly.

## 3. Deferred work

Some settings changes  -  particularly display mode changes  -  are placed on a deferred work queue rather than applied immediately. This prevents the settings window render callback from being called in the middle of a mode switch. The main loop polls and drains the deferred queue each iteration.

## 4. Version display

The About tab shows `© 2026 Kurono OS` and the current kernel version string from `GetKernelVersion()`.

## 5. Related files

- `src/apps/settings.cpp` / `.h`  -  the app + `SettingsTab` enum
- `src/apps/settings_mod_*.cpp`  -  the 11 per-tab module implementations
- `src/system/ui_config.cpp`  -  reads and writes the config file
- `src/ui/desktop.cpp`  -  `LaunchSettings()` entry point and `ReloadFromConfig()` target
- `src/drivers/display_mgr.cpp`  -  display mode changing
- `src/drivers/audio.cpp`  -  volume control
