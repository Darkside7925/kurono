# UI Customization Guide

This document explains how to customize the Kurono OS visual appearance at runtime using the configuration system.

## 1. How customization works

All visual properties  -  colors, sizes, toggles  -  are stored in a plain text file at `/etc/kurono/ui.conf` inside the KVFS filesystem. The file is read at boot and can be reloaded at any time without restarting.

After making changes to the config file, apply them with:

```
kurono reload
```

This command calls `UIConfig::Reload()` which re-reads the file and propagates new values to the taskbar, desktop, window manager, and all other themed components immediately.

## 2. Editing the config file

Open the text editor from the terminal:

```
edit /etc/kurono/ui.conf
```

Or from the Settings app → Appearance section.

The config file is self-documenting. Every key is listed with its default value and a comment explaining what it controls.

## 3. Color format

All color values use `0xAARRGGBB` hexadecimal:

- `AA`  -  alpha (FF = fully opaque)
- `RR`  -  red component
- `GG`  -  green component
- `BB`  -  blue component

Example: `0xFF5C8AFF` is fully opaque blue-purple.

## 4. Quick reference: most useful keys

### Change the taskbar color
```
taskbar.bg = 0xFF1A1A2E
taskbar.top_edge = 0xFF3A3A50
```

### Move the taskbar to the top
```
taskbar.position = top
```

### Hide taskbar elements you don't use
```
taskbar.show_search = 0
taskbar.show_battery = 0
```

### Change the desktop background color
```
desktop.bg = 0xFF0A1020
```

### Make icons larger
```
desktop.icon_size = 72
desktop.icon_spacing_x = 120
desktop.icon_spacing_y = 130
```

### Change the context menu style
```
ctxmenu.bg = 0xFF0F0F20
ctxmenu.border = 0xFF8844FF
ctxmenu.text = 0xFFFFFFFF
```

### Change window chrome colors
```
window.title_bg = 0xFF101020
window.close_btn = 0xFFCC3333
window.min_btn = 0xFFCCAA00
window.max_btn = 0xFF00AA44
```

### Disable desktop file editing
```
desktop.allow_edit = 0
```

### Disable Task Manager kill/restart
```
taskmgr.allow_kill = 0
```

### Retheme via KSS theme tokens
The KSS styling layer reads a `theme.*` token set from the same file (these keys
aren't in the generated default  -  add them to override the built-in theme):
```
theme.bg      = 0xFF101014
theme.surface = 0xFF1E1E24
theme.accent  = 0xFF5C8AFF
theme.text    = 0xFFF0F0F2
```
See `system/UI_CONFIG.md` for the full `theme.*` / `compositor.*` / `display.*`
key list.

## 5. Applying changes on the fly

After editing and saving the config file:

```
kurono reload
```

No reboot needed. Changes to colors, sizes, and toggles all apply the moment reload completes.

## 6. Resetting to defaults

Delete the config file and reload:

```
rm /etc/kurono/ui.conf
kurono reload
```

The system will write a fresh default config file on the next reload.

## 7. Related documentation

- `system/UI_CONFIG.md`  -  full technical reference for the UIConfig module
- `shell/SHELL.md`  -  `kurono` command documentation
