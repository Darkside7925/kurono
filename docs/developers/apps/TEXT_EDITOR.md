# Text Editor App

`src/apps/text_editor.cpp` and `text_editor.h` implement a basic modal text editor.

## 1. What it does

The text editor opens files from KVFS and provides a simple line-based editing interface. It is the default handler for `.txt`, `.conf`, and other plain text files when opened from the file manager or the desktop.

## 2. Editing model

The text content is stored as an array of lines. The cursor tracks a (line, column) position. Supported operations:

- Character insertion at cursor
- Backspace (character and line merge)
- Enter (line split)
- Arrow key navigation
- Ctrl+S to save (writes back to KVFS)
- Ctrl+Q to quit (prompts if unsaved changes)

## 3. Rendering

The editor renders the visible lines from a scroll offset. The current line is highlighted with a faint background. The cursor is drawn as a blinking character-cell highlight.

Line numbers are shown in a left gutter. The status bar at the bottom shows the file name, current line/column, and modified flag.

## 4. Integration with KVFS

On open, the editor reads the file with `KVFS::ReadString`. On save, it writes with `KVFS::WriteString`. Files created with "New File" on the desktop can be opened immediately.

## 5. Related files

- `src/fs/kvfs.cpp` - file read/write
- `src/ui/font.cpp` - text rendering
- `src/apps/file_manager.cpp` - launches editor on file double-click
