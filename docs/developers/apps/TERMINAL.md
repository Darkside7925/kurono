# Terminal

`src/apps/terminal.cpp` and `terminal.h`. The windowed VT100-ish terminal that hosts
the Kurono shell.

It spawns a shell session on the `KuronoShell` backend and renders output into a
scrollable text buffer.

## Input

Every keystroke goes to `KuronoShell::Input()`. The shell does the line editing and
history recall, writes output into a character queue, and the terminal drains that
queue into the display buffer.

## Display buffer

A grid of cells. Each cell has a character, a foreground colour and a background
colour. The renderer walks the grid and draws every cell through the font engine. When
the bottom row fills, the grid shifts up. A scrollback buffer keeps recent history for
the scroll bar.

## ANSI

Basic SGR for colours, bold and reset. Enough for the shell's prompt formatting and for
commands that print colour.

## When it breaks

| Symptom | Cause |
| --- | --- |
| text lands in the wrong column | tab stop maths is off |
| wrong colours | SGR parameter parsing is incomplete |
| input never reaches the shell | the key event isn't forwarded to the terminal window |
| scroll is broken | grid shift logic, check the bottom-row boundary |

## Files

- `src/shell/shell.cpp` the shell backend
- `src/ui/font.cpp` character rendering
- `src/ui/gui.cpp` window background and scroll bar
