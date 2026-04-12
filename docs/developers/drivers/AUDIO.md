# Audio Driver

`src/drivers/audio.cpp` and `audio.h` provide the audio services layer used by the media player and system sounds.

## 1. What it does

The audio driver is a unified interface that sits above two hardware backends: AC97 (`ac97.cpp`) and Intel HD Audio (`hda.cpp`). It provides:

- `PlayTone(freq, duration_ms)`  -  beeper-style tone generation
- `PlayPCM(buf, len, sample_rate)`  -  raw PCM playback through the audio backend
- `SetVolume(0-100)`  -  master volume control

## 2. Hardware backends

**AC97 (`src/drivers/ac97.cpp`)**  -  The older Intel AC97 audio controller, present in older hardware and in QEMU's default sound model. AC97 uses a simple DMA-based buffer model.

**HDA (`src/drivers/hda.cpp`)**  -  Intel High Definition Audio, the standard on modern hardware. HDA uses a more complex stream model with command rings and codec probing.

The audio driver selects a backend based on which hardware is detected on the PCI bus.

## 3. Integration with media player

The media player (`src/apps/media_player.cpp`) calls the audio driver to output decoded PCM. The MP3 decoder (`src/media/mp3_decoder.cpp`) feeds decoded samples to the audio driver.

## 4. Common problems

| Problem | Likely cause |
| --- | --- |
| No sound | Backend not detected; QEMU needs `-soundhw ac97` or `-soundhw hda` |
| Distorted playback | Sample rate mismatch between decoder and driver |
| Volume control has no effect | Wrong mixer register for detected codec |

## 5. Related files

- `src/drivers/ac97.cpp`  -  AC97 backend
- `src/drivers/hda.cpp`  -  HDA backend
- `src/media/mp3_decoder.cpp`  -  audio data source
- `src/apps/media_player.cpp`  -  application that calls the audio driver
