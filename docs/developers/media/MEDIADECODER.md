# Media Decoder

`src/media/mediadecoder.cpp` and `mediadecoder.h` implement image and media decoding for the Kurono UI.

## 1. What it decodes

The media decoder handles:

- **PNG** images - decoded for wallpapers, icons, and the media player image display
- **JPEG** images - decoded for photo viewing
- **BMP** images - simple uncompressed format support
- **Raw pixel buffers** - pass-through for pre-decoded data

The decoded result is a `MediaDecoder::Image` struct containing width, height, and a `uint32_t*` pixel buffer in `0xAARRGGBB` format.

## 2. Usage in the UI

Desktop wallpaper:
```cpp
MediaDecoder::Image img = MediaDecoder::DecodeFile(path);
Desktop::SetWallpaperImage(img);
```

The desktop caches the decoded pixel data and blits it as the background each frame.

The media player app also uses the decoder to show embedded album art and image files in its playlist.

## 3. Codec registry

`src/media/codec.cpp` maintains a registry of available codecs. The decoder queries the registry by file extension or magic bytes to select the right decoder. Adding support for a new format means registering a new codec entry.

## 4. MP3 decoding

`src/media/mp3_decoder.cpp` handles MP3 audio decoding (separate from image decoding). It reads MP3 frames, decodes them to PCM, and feeds the output to the audio driver. The implementation is a lightweight fixed-point decoder suited to embedded use.

## 5. Third-party components

`src/third_party` contains glue around embedded third-party libraries used by the decoder (likely `stb_image` or equivalent public domain decoders). These are headeronly or minimal builds.

## 6. Related files

- `src/ui/desktop.cpp` - `SetWallpaperImage()` consumer
- `src/apps/media_player.cpp` - audio and image display consumer
- `src/media/mp3_decoder.cpp` - audio decode companion
- `src/drivers/audio.cpp` - PCM output from MP3 decode
