# Settings

`src/apps/settings.cpp` and `settings.h`.

12 tabs (`SettingsTab` in `settings.h`, `STAB_COUNT = 12`), each with its own
`settings_mod_*.cpp` module:

1. Display: resolution, refresh, VSync, DPI
2. Sound: volume level, device selection
3. Network: connection status, IP config
4. Storage
5. Power
6. Personalize: wallpaper, theme colour presets
7. Security
8. Packages
9. Updates: real repo fetch and pending-update counts
10. System
11. About: version, build info, hardware summary
12. Accessibility

## Where settings land

Anything that maps to a `UIConfig` key writes to `/etc/kurono/ui.conf` through
`KVFS::WriteString`, then calls `DesktopEnvironment::ReloadFromConfig()` so the change
applies live, no reboot.

Hardware settings (display mode, audio volume) call the driver directly.

## Security: the KSA switch

Users & Security has an Approval group with one switch, KSA. KSA is the second approval step
behind `amdo` (see [../security/KEN.md](../security/KEN.md)). The switch is on by default.

- Turning KSA off asks for your password. The check runs in the service through the KEN policy
  call. Until the password is accepted nothing changes.
- Turning KSA back on needs no password.
- The setting persists across reboots. While it is off, `amdo` and approvals use your password
  only, and the row says so.

## Deferred changes

Display mode changes and friends go on a deferred work queue instead of being applied
where they're clicked. That keeps the settings render callback from firing in the
middle of a mode switch. The main loop drains the queue every iteration.

## About tab

Shows the 2026 Kurono OS copyright line and the version string from
`GetKernelVersion()`.

## Files

- `src/apps/settings.cpp` / `.h` the app and the `SettingsTab` enum
- `src/apps/settings_mod_*.cpp` the 11 per-tab modules
- `src/system/ui_config.cpp` reads and writes the config file
- `src/ui/desktop.cpp` `LaunchSettings()` and the `ReloadFromConfig()` target
- `src/drivers/display_mgr.cpp` display mode changes
- `src/drivers/audio.cpp` volume
