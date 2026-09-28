# Audio

`src/drivers/audio.cpp` and `audio.h`. The audio layer the media player and system sounds
go through.

It sits above two hardware backends, AC97 (`ac97.cpp`) and Intel HD Audio (`hda.cpp`),
and gives you:

- `PlayTone(freq, duration_ms)` beeper-style tone
- `PlayPCM(buf, len, sample_rate)` raw PCM through the backend
- `SetVolume(0-100)` master volume

## Backends

AC97 (`src/drivers/ac97.cpp`) is the older Intel controller. Found on older hardware and
in QEMU's default sound model. Simple DMA buffer model.

HDA (`src/drivers/hda.cpp`) is Intel High Definition Audio, the modern standard. More
work: command rings and codec probing.

Which one gets used depends on what the PCI scan finds.

## Who calls it

`src/apps/media_player.cpp` for playback, fed by `src/media/mp3_decoder.cpp`.

## When it breaks

| Symptom | Cause |
| --- | --- |
| no sound | backend not detected, QEMU needs `-soundhw ac97` or `-soundhw hda` |
| distorted playback | sample rate mismatch between decoder and driver |
| volume does nothing | wrong mixer register for the codec that got detected |

## Files

- `src/drivers/ac97.cpp` AC97 backend
- `src/drivers/hda.cpp` HDA backend
- `src/media/mp3_decoder.cpp` where the audio comes from
- `src/apps/media_player.cpp` the app that calls this
