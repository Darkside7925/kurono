# Customising the UI

All the visual properties (colours, sizes, toggles) live in a plain text file at
`/etc/kurono/ui.conf` in KVFS. Read at boot, reloadable any time.

```
kurono reload
```

`UIConfig::Reload()` re-reads the file and pushes the new values at the taskbar, desktop,
window manager and everything else themed.

## Editing it

```
edit /etc/kurono/ui.conf
```

Or Settings, Appearance section. The file documents itself: every key is listed with its
default and a comment.

## Colour format

`0xAARRGGBB`. AA is alpha (FF is opaque), then red, green, blue. Example `0xFF5C8AFF` is a
fully opaque blue-purple.

## Useful keys

Taskbar colour:
```
taskbar.bg = 0xFF1A1A2E
taskbar.top_edge = 0xFF3A3A50
```

Move the taskbar to the top:
```
taskbar.position = top
```

Hide bits you don't use:
```
taskbar.show_search = 0
taskbar.show_battery = 0
```

Desktop background:
```
desktop.bg = 0xFF0A1020
```

Bigger icons:
```
desktop.icon_size = 72
desktop.icon_spacing_x = 120
desktop.icon_spacing_y = 130
```

Context menu:
```
ctxmenu.bg = 0xFF0F0F20
ctxmenu.border = 0xFF8844FF
ctxmenu.text = 0xFFFFFFFF
```

Window chrome:
```
window.title_bg = 0xFF101020
window.close_btn = 0xFFCC3333
window.min_btn = 0xFFCCAA00
window.max_btn = 0xFF00AA44
```

Turn off desktop file editing:
```
desktop.allow_edit = 0
```

Turn off task manager kill/restart:
```
taskmgr.allow_kill = 0
```

Retheme through KSS tokens. These aren't in the generated default, add them yourself to
override the built-in theme:
```
theme.bg      = 0xFF101014
theme.surface = 0xFF1E1E24
theme.accent  = 0xFF5C8AFF
theme.text    = 0xFFF0F0F2
```

`system/UI_CONFIG.md` has the full `theme.*`, `compositor.*` and `display.*` list.

## Applying it

Save, then `kurono reload`. No reboot. Colours, sizes and toggles all apply.

## Back to defaults

```
rm /etc/kurono/ui.conf
kurono reload
```

It writes a fresh default file on the next reload.

## More

- `system/UI_CONFIG.md` full config reference
- `shell/SHELL.md` the `kurono` command
