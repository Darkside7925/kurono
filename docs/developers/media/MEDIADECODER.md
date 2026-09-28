# Media decoder

`src/media/mediadecoder.cpp` and `mediadecoder.h`. Image and media decoding for the UI.

Decodes PNG (wallpapers, icons, the media player's image view), JPEG and BMP, and passes raw
pixel buffers straight through. Output is a `MediaDecoder::Image` with width, height and a
`uint32_t*` pixel buffer in `0xAARRGGBB`.

## In the UI

Wallpaper:

```cpp
MediaDecoder::Image img = MediaDecoder::DecodeFile(path);
Desktop::SetWallpaperImage(img);
```

The desktop caches the decoded pixels and blits them as the background each frame. The media
player uses the same decoder for album art and images in its playlist.

## Codec registry

`src/media/codec.cpp` holds the registry. The decoder picks a codec by file extension or
magic bytes. Adding a format means adding a registry entry.

## MP3

`src/media/mp3_decoder.cpp` is separate from image decoding. It reads MP3 frames, decodes
them to PCM and hands the output to the audio driver. A lightweight fixed-point decoder,
aimed at embedded use.

## Third party

`src/third_party` holds glue around the embedded public-domain decoders (stb_image and
friends). Header-only or minimal builds.

## Files

- `src/ui/desktop.cpp` the `SetWallpaperImage()` consumer
- `src/apps/media_player.cpp` audio and image consumer
- `src/media/mp3_decoder.cpp` audio decode companion
- `src/drivers/audio.cpp` where the PCM ends up
