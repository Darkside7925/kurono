// kurono os  -  denji video viewer (window wrapper around VideoPlayer)
// =====================================================================
// thin app that opens a window and binds VideoPlayer::Tick/Render
// to it.  loads the embedded denji.kvid by default.
#pragma once
#include "../kernel/types.h"

namespace DenjiApp {

// open the viewer window; if already open just brings it forward.
// uses the embedded denji.kvid asset if available.
void Open();
void Close();
bool IsOpen();

// one-shot opener for an arbitrary buffer (e.g. user files later).
// data must remain valid for the lifetime of the window.
void OpenBuffer(const uint8_t* data, uint32_t size, const char* title);

} // namespace DenjiApp
