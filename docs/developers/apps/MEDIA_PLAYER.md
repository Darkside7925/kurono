# Media Player

`src/apps/media_player.cpp` and `media_player.h` implement the Kurono media player.

## 1. What it plays

The media player handles:

- **MP3** audio files - decoded by `src/media/mp3_decoder.cpp`
- **Image files** - displayed in the player window (PNG, JPEG, BMP)
- **Playlist support** - queue multiple files and navigate with next/previous

## 2. Playback pipeline

```
File (KVFS) → MediaDecoder → PCM samples → AudioDriver → Hardware
```

1. The player opens a file path from KVFS.
2. `MediaDecoder` identifies the format and selects the right codec.
3. The MP3 decoder produces PCM at the file's native sample rate.
4. The audio driver (`ac97` or `hda`) receives PCM and outputs to hardware.
5. The player UI shows the track name, elapsed time, and a seek bar.

## 3. Controls

- Play / Pause
- Stop
- Next / Previous track
- Volume (0 - 100, synced with the Taskbar volume indicator)
- Seek by clicking the progress bar

## 4. Visualizer

A simple waveform or spectrum visualizer renders in the player window using the current audio buffer. It is updated each frame.

## 5. Related files

- `src/media/mp3_decoder.cpp` - audio decode
- `src/media/mediadecoder.cpp` - image decode for album art
- `src/drivers/audio.cpp` - audio driver
- `src/ui/desktop.cpp` - `LaunchMediaPlayer()` entry point
