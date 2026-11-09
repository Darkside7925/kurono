// kurono os - media / video player app (window around VideoPlayer)
// =====================================================================
// one window that is either the video library (the dock's Media icon) or a
// player with an auto-hiding control bar.  opening another video reuses the
// window.  the window can be closed at any moment: the app notices on its own
// background tick, stops the audio and frees every buffer. (satoru)
#pragma once
#include "../kernel/types.h"

namespace DenjiApp {

// open the player on the Denji demo clip (loaded on demand from the
// KURONO partition); focuses the window if it is already open. (satoru)
void Open();

// demo clips: 0 = Denji, 1 = ssstik. (satoru)
void OpenSample(int which);

// open the window showing the list of playable videos. (satoru)
void OpenLibrary();

void Close();
bool IsOpen();

// one-shot opener for an arbitrary buffer.  data must stay valid until the
// video is closed or replaced. (satoru)
void OpenBuffer(const uint8_t* data, uint32_t size, const char* title);

// open a video file from the vfs: reads it into a buffer owned by the app
// (freed when the video is closed or replaced) and plays it.  returns false
// if the file can't be read or isn't a kvid / mp4. (satoru)
bool OpenFile(const char* path);

// true while the window needs frames composited back to back (loading, a
// control fade, a seek drag).  playback itself asks for frames at the exact
// moment the next video frame is due, so a playing video does not pin the
// desktop at full refresh rate. (satoru)
bool NeedsFrames();

// called once per gui loop iteration, independent of painting: notices a
// closed window, feeds the audio mixer and schedules the next frame. (satoru)
void BackgroundTick();

} // namespace DenjiApp

// end (satoru)
