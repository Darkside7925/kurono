// media_audio.h: the audio output the media player needs, as a thin adapter over kui::Audio (satoru)
// the player hands over whole decoded buffers; the adapter converts to s16, opens one kui::Audio stream and
// feeds it from Pump() (called every loop pass from the window's OnTick), so output keeps flowing while the
// window is hidden. only the backend name in the status line is kernel specific. (satoru)
#pragma once
#include <stdint.h>

namespace MpAudio {

// queue interleaved pcm for output. bits 8 (unsigned), 16 or 32 (signed little endian). false if
// there is no output device. replaces anything that was playing. (satoru)
bool PlayPCM(const uint8_t* data, int len, int rate, int bits, int channels);

void Pump();                    // keep the stream fed; cheap, safe every loop pass (satoru)
void Stop();                    // end the stream and free its buffer (satoru)
void Pause(bool on);            // pause or resume without dropping the queue (satoru)
void SetMasterVolume(int pct);  // 0..100 (satoru)

const char* BackendName();      // "HDA", "AC97", ... or "No Audio" (satoru)
const char* StatusText();       // "Stopped", "Paused", "Playing (HDA)" for the status line (satoru)
int  SampleRateHz();            // hz shown next to the codec badge, 0 = none (satoru)
bool Drained();                 // everything given to the device has been played (satoru)

}  // namespace MpAudio

// end (satoru)
