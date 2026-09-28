# Desktop and taskbar

`src/ui/desktop.cpp` and `desktop.h`. The desktop environment: taskbar, start menu, desktop
icons, right-click context menu, and the outermost event routing.

## Three classes

**`Taskbar`** draws the bar (bottom or top) with the start button, running window buttons, the
system tray (clock, battery, Wi-Fi, volume) and the search bar.

**`Desktop`** draws the background, wallpaper or gradient, places icons, and handles
double-click to launch and right-click for menus.

**`DesktopEnvironment`** owns both. All input goes through
`DesktopEnvironment::HandleInput()`, and the main loop calls
`DesktopEnvironment::Update()` and `Render()`.

## Icons

`DesktopIcon` structs in a fixed array, `DESKTOP_MAX_ICONS = 32`. Each one has a name, path,
position, type flag and selection state.

`Desktop::ArrangeIcons()` lays them out in a grid from the top-left margin using the spacing
values from `UIConfig`. Call it again after adding or removing an icon and the grid resizes.

## Context menu

Right-click the desktop. Two modes:

- empty space: New Folder, New File, Refresh, Settings
- on an icon: Open, Delete, Refresh, Properties

`context_menu_target` records which icon was right-clicked, -1 for empty space.
`RenderContextMenu()` draws the menu from config colours and sizes, and `HandleClick()` checks
whether a menu is open before processing any other click.

## Creating and deleting

`Desktop::CreateFolderInteractive()` makes "New Folder N" in KVFS under `/home/user/Desktop/`,
bumping `new_folder_counter` each time, and the icon shows up immediately.
`Desktop::CreateFileInteractive()` does the same for "New File N.txt".

`Desktop::RemoveIcon(int)` drops the icon, deletes the KVFS entry, and re-arranges.

## Taskbar config

Everything visual is read from `UIConfig` at init and again after `kurono reload`:

| Key | Default | Meaning |
| --- | --- | --- |
| `taskbar.height` | 44 | bar height in pixels |
| `taskbar.position` | bottom | `bottom` or `top` |
| `taskbar.bg` | 0xFF0C0C14 | background colour |
| `taskbar.text` | 0xFFBBBBCC | text colour |
| `taskbar.show_clock` | 1 | show the clock |
| `taskbar.show_search` | 1 | show the search bar |

`Taskbar::ReloadFromConfig()` applies it live, no reboot.

## Wallpaper

`Desktop::SetWallpaper(color)` for a solid or gradient colour,
`Desktop::SetWallpaperImage(img)` for a scaled image. The rendered result is cached in
`gradient_cache` and blitted whole-frame, because redrawing it every frame is too slow.

## Files

- `src/system/ui_config.cpp` the config source for all of it
- `src/ui/window_manager.cpp` the window layer above the desktop
- `src/ui/wayland_server.cpp` the in-kernel compositor that can bridge to the WM
- `src/fs/kvfs.cpp` desktop file create and delete
- `src/apps/settings.cpp` launched from the context menu's Settings item
