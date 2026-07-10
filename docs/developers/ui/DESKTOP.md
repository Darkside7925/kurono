# Desktop and Taskbar

`src/ui/desktop.cpp` and `desktop.h` implement the desktop environment: the taskbar, start menu, desktop icons, right-click context menu, and the outermost event routing layer.

## 1. Architecture overview

Three classes cooperate to produce the desktop.

**`Taskbar`** - renders the bottom (or top) bar with the start button, running window buttons, system tray (clock, battery, Wi-Fi, volume), and the search bar.

**`Desktop`** - renders the desktop background (wallpaper or gradient), manages icon placement, handles double-click to launch and right-click for context menus.

**`DesktopEnvironment`** - the coordinator that owns both Taskbar and Desktop. All input routes through `DesktopEnvironment::HandleInput()`. The main loop calls `DesktopEnvironment::Update()` and `DesktopEnvironment::Render()`.

## 2. Icons

Icons are `DesktopIcon` structs stored in a fixed array (`DESKTOP_MAX_ICONS = 32`). Each icon has a name, path, position, type flag, and selection state.

`Desktop::ArrangeIcons()` places icons in a grid starting from the top-left margin, using the spacing values from `UIConfig`. Calling `ArrangeIcons()` again after adding or removing icons resizes the grid.

## 3. Context menu

Right-clicking the desktop opens a context menu. The menu has two modes:

- **Empty space**: New Folder, New File, Refresh, Settings
- **Icon targeted**: Open, Delete, Refresh, Properties

The `context_menu_target` field records which icon was right-clicked (-1 for empty space). `RenderContextMenu()` draws the menu using config-driven colors and sizes. `HandleClick()` checks whether a context menu is open before processing any other click.

## 4. Creating and deleting files and folders

`Desktop::CreateFolderInteractive()` creates "New Folder N" in KVFS at `/home/user/Desktop/`. Each call increments `new_folder_counter`. The new icon appears immediately.

`Desktop::CreateFileInteractive()` does the same for "New File N.txt".

`Desktop::RemoveIcon(int)` removes an icon from the array, deletes the backing KVFS entry, and calls `ArrangeIcons()`.

## 5. Taskbar configuration

All taskbar visual properties and toggles are read from `UIConfig` at init time and after `kurono reload`. The relevant keys are:

| Key | Default | Meaning |
| --- | --- | --- |
| `taskbar.height` | 44 | Bar height in pixels |
| `taskbar.position` | bottom | `bottom` or `top` |
| `taskbar.bg` | 0xFF0C0C14 | Background color |
| `taskbar.text` | 0xFFBBBBCC | Text color |
| `taskbar.show_clock` | 1 | Show the clock |
| `taskbar.show_search` | 1 | Show the search bar |

Call `Taskbar::ReloadFromConfig()` to apply changes at runtime without rebooting.

## 6. Wallpaper

`Desktop::SetWallpaper(color)` sets a solid or gradient wallpaper color. `Desktop::SetWallpaperImage(img)` sets a pixel-art scale image. The rendered wallpaper is cached in `gradient_cache` and blitted whole-frame for performance.

## 7. Related files

- `src/system/ui_config.cpp` - config source for all visual settings
- `src/ui/window_manager.cpp` - window layer rendered above the desktop
- `src/ui/wayland_server.cpp` - in-kernel Wayland compositor that can bridge to the window manager
- `src/fs/kvfs.cpp` - KVFS used for desktop file creation and deletion
- `src/apps/settings.cpp` - launched from the context menu's Settings item
