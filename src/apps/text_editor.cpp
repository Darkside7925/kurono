//  kurono os - text editor application implementation
#include "text_editor.h"
// the editor is a kui app: one source serves the in-kernel window manager and the ring 3
// wayland build, with tabs, a file sidebar, syntax highlighting, regex find and replace,
// multi level undo, word wrap, bracket matching and modal save prompts. (satoru)
#ifndef KUI_BACKEND_WAYLAND
#include "../system/logging.h"
#include "../kernel/taskmem.h"   // kurono.taskmem=1 app scopes (satoru)
#define ED_APPLOG(ev, detail) RuntimeLog::LogAppEvent("editor", (ev), (detail))
#else
#define ED_APPLOG(ev, detail) kui::Log("editor", (ev))
#endif

#include "editor/editor_common.inc"
#include "editor/editor_docs.inc"
#include "editor/editor_menu.inc"
#include "editor/editor_find.inc"
#include "editor/editor_dialog.inc"
#include "editor/editor_edit.inc"
#include "editor/editor_draw.inc"
#include "editor/editor_event.inc"

// end (satoru)
