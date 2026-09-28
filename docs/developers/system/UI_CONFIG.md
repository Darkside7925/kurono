# UI config

`src/system/ui_config.cpp` and `ui_config.h`. The runtime config engine behind all the visual
customisation.

A static class backed by a plain key=value file at `/etc/kurono/ui.conf` in KVFS. Reads it at
boot, caches the entries, exposes typed accessors. Any subsystem that wants a user-configurable
colour, size or toggle reads from here instead of hardcoding the value.

## File format

Line oriented:

```
key = value    # optional comment
```

- keys and values split on `=`
- `#` starts a comment, everything after it is ignored
- whitespace around `=` and at line ends is trimmed
- blank lines are fine
- colours are `0xAARRGGBB` hex, no quotes
- integers are plain decimal
- booleans are `0` or `1`, and also `true`/`false`, `yes`/`no`, `on`/`off`

## Keys

### Taskbar
| Key | Default | Type |
| --- | --- | --- |
| `taskbar.height` | 44 | int |
| `taskbar.position` | bottom | string |
| `taskbar.bg` | 0xFF0C0C14 | color |
| `taskbar.top_edge` | 0xFF2A2A40 | color |
| `taskbar.text` | 0xFFBBBBCC | color |
| `taskbar.start_btn_bg` | 0xFF5C8AFF | color |
| `taskbar.start_btn_hover` | 0xFF4470E0 | color |
| `taskbar.show_clock` | 1 | bool |
| `taskbar.show_battery` | 1 | bool |
| `taskbar.show_wifi` | 1 | bool |
| `taskbar.show_volume` | 1 | bool |
| `taskbar.show_search` | 1 | bool |

### Desktop
| Key | Default | Type |
| --- | --- | --- |
| `desktop.bg` | 0xFF0C0818 | color |
| `desktop.icon_text` | 0xFFE8E8F0 | color |
| `desktop.icon_selected` | 0xFF2A3860 | color |
| `desktop.icon_size` | 56 | int |
| `desktop.icon_spacing_x` | 96 | int |
| `desktop.icon_spacing_y` | 100 | int |
| `desktop.icon_margin_x` | 24 | int |
| `desktop.icon_margin_y` | 20 | int |
| `desktop.allow_edit` | 1 | bool |

### Context menu
| Key | Default | Type |
| --- | --- | --- |
| `ctxmenu.bg` | 0xFF121228 | color |
| `ctxmenu.border` | 0xFF5C8AFF | color |
| `ctxmenu.text` | 0xFFE8E8F0 | color |
| `ctxmenu.item_h` | 30 | int |
| `ctxmenu.width` | 180 | int |

### Window manager
| Key | Default | Type |
| --- | --- | --- |
| `window.titlebar_height` | 36 | int |
| `window.corner_radius` | 10 | int |
| `window.shadow_size` | 6 | int |
| `window.title_bg` | 0xFF1C1C2E | color |
| `window.title_focused` | 0xFF22223A | color |
| `window.title_text` | 0xFFF0F0F5 | color |
| `window.border_focus` | 0xFF6C8CFF | color |
| `window.close_btn` | 0xFFFF5F57 | color |
| `window.min_btn` | 0xFFFFBD2E | color |
| `window.max_btn` | 0xFF28C840 | color |

### Task manager
| Key | Default | Type |
| --- | --- | --- |
| `taskmgr.row_h` | 20 | int |
| `taskmgr.allow_kill` | 1 | bool |

### Display frame pacing
| Key | Default | Type |
| --- | --- | --- |
| `display.refresh_hz` | 60 | int |
| `display.vsync` | 1 | bool, paces frames to `refresh_hz` |
| `display.adaptive_sync` | 1 | bool, drops to half rate on budget overrun |

### Compositor shadows and animation
| Key | Default | Type |
| --- | --- | --- |
| `compositor.shadow_enabled` | 1 | bool |
| `compositor.shadow_radius` | 8 | int, 0 to 16 layers |
| `compositor.shadow_opacity` | 60 | int, 0 to 100 percent |
| `compositor.shadow_during_drag` | 0 | bool |
| `compositor.window_animations` | 1 | bool |
| `compositor.animation_speed_ms` | 90 | int, 0 for instant |
| `compositor.window_alpha` | 255 | int, 0 to 255 |
| `compositor.frosted_titlebar` | 1 | bool |
| `compositor.reduced_motion` | 0 | bool, 1 disables all animations |

### KSS theme tokens

The KSS styling layer (`src/ui/kss.cpp`) reads its theme tokens from the same `ui.conf` through
`UIConfig::Color(...)`. These keys are not written into the generated default file, since KSS has
built-in defaults, but adding them to `ui.conf` overrides the theme. All are `0xAARRGGBB` except
the metric tokens, which are plain ints.

| Key | Default | Notes |
| --- | --- | --- |
| `theme.bg` | 0xFF1B1B1D | base background |
| `theme.surface` | 0xFF2A2A2D | panel surface |
| `theme.surface_hi` | 0xFF35353A | raised surface |
| `theme.sel` | 0xFF3A3A40 | selection |
| `theme.header` | 0xFF222225 | header bar |
| `theme.text` | 0xFFF0F0F2 | body text |
| `theme.text_dim` | 0xFF9A9AA2 | dim text |
| `theme.heading` | 0xFFF0F0F2 | heading text |
| `theme.border` | 0xFF3A3A40 | borders |
| `theme.accent`, `theme.on`, `theme.off`, `theme.track`, `theme.white`, `theme.shadow` | built-in | accent, toggle on/off, slider track, white, shadow |
| `theme.radius`, `theme.pad` | built-in ints | corner radius, padding metrics |

## Applying changes

Edit `/etc/kurono/ui.conf`, then:

```
kurono reload
```

That calls `UIConfig::Reload()`, bumps the version counter, and calls
`DesktopEnvironment::ReloadFromConfig()`, which pushes the new values at the taskbar, desktop and
window manager straight away.

## The default file

If the config file doesn't exist at boot, UIConfig writes the complete default file with every
key documented. Modifying the generated file is the recommended starting point.

## Files

- `src/ui/desktop.cpp` `ReloadFromConfig()` for taskbar and desktop
- `src/shell/shell.cpp` `kurono reload` and `kurono config`
- `src/fs/kvfs.cpp` the file read/write backing
