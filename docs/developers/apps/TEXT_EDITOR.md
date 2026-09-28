# Text editor

`src/apps/text_editor.cpp` and `text_editor.h`. Small modal editor for plain text.

Opens files from KVFS and edits line by line. Default handler for `.txt`, `.conf` and
friends when opened from the file manager or the desktop.

## Editing

Content is an array of lines, the cursor is a (line, column) pair. Supports character
insert at the cursor, backspace including line merge, enter to split a line, arrow
navigation, Ctrl+S to save back to KVFS, Ctrl+Q to quit with an unsaved-changes prompt.

## Rendering

Draws the visible lines from a scroll offset, highlights the current line with a faint
background, blinks a cursor cell. Line numbers in a left gutter. The status bar shows
file name, line/column and the modified flag.

## KVFS

Reads with `KVFS::ReadString`, writes with `KVFS::WriteString`. Files made with "New
File" on the desktop open straight away.

## Files

- `src/fs/kvfs.cpp` read and write
- `src/ui/font.cpp` text rendering
- `src/apps/file_manager.cpp` launches it on double click
