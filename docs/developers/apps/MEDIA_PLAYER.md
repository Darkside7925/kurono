# Media player

`src/apps/media_player.cpp` and `media_player.h`.

Plays MP3 audio (decoded by `src/media/mp3_decoder.cpp`), shows images in the window
(PNG, JPEG, BMP), and takes a playlist you can step through with next/previous.

## Playback path

```
File (KVFS) -> MediaDecoder -> PCM samples -> AudioDriver -> hardware
```

1. player opens a path from KVFS
2. `MediaDecoder` works out the format and picks a codec
3. the MP3 decoder gives PCM at the file's native rate
4. `ac97` or `hda` takes the PCM and plays it
5. the UI shows track name, elapsed time and a seek bar

## Controls

Play/pause, stop, next/previous, volume (0 to 100, synced with the taskbar indicator),
and seek by clicking the progress bar.

## Visualizer

A simple waveform or spectrum thing drawn from the current audio buffer, updated every
frame.

## Playback stats

Two serial log lines, every 10 seconds, show what the video and audio paths are doing:

- `[kuiloop]` from the desktop's frame loop (`kui/ring3/kui_wayland.cpp`), while a window paces
  itself with `RequestFrameAt`, as a playing video does. It logs the passes, frames, the longest
  poll lateness and the callback drops.
- `[audiopump]` from the kernel audio pump (`proc/kernel_processes.cpp`), with the number of pumps.


## Files

- `src/media/mp3_decoder.cpp` audio decode
- `src/media/mediadecoder.cpp` image decode, album art
- `src/drivers/audio.cpp` audio driver
- `src/ui/desktop.cpp` `LaunchMediaPlayer()` entry point
