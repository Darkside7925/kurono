#pragma once
//  kurono os - media player application
//  supports wav/mp3/aac/flac/mp4 via codecregistry + sb16/ac97/hda output
#include "../kui/kui.h"
#include "../kernel/types.h"
#include "../media/codec.h"

#define MP_MAX_PLAYLIST  32
#define MP_MAX_NAME      64

enum MediaType {
    MEDIA_UNKNOWN = 0,
    MEDIA_AUDIO,
    MEDIA_VIDEO
};

struct PlaylistEntry {
    char       name[MP_MAX_NAME];
    char       path[128];
    MediaType  type;
    CodecType  codec;           // detected codec type
    int        duration_sec;    // duration in seconds (0 = unknown)
    int        file_size;       // actual file size in bytes
    bool       is_playing;
    // video metadata (cached from codec detection)
    int        video_width;
    int        video_height;
    int        video_codec;     // 0=unknown, 1=h.264, 2=h.265
    bool       has_video;
    bool       has_audio;
};

class MediaPlayerApp {
public:
    static void Open();
    static void Open(const char* file_path);
    static void Close();
    static bool IsOpen();

    // playback
    static void Play();
    static void Pause();
    static void Stop();
    static void Next();
    static void Previous();
    static void Seek(int position_pct);      // 0-100
    static void SetVolume(int vol_pct);      // 0-100
    static int  GetVolume();

    // now-playing snapshot for shell surfaces (control center). title is the
    // current playlist file name, empty when nothing is loaded. (satoru)
    static bool NowPlayingActive();
    static const char* NowPlayingTitle();
    static int  NowPlayingProgress();        // 0-100
    static bool IsPlayingNow();
    static bool IsPausedNow();
    static void TogglePlayback();

    // playlist
    static void AddToPlaylist(const char* path);
    static void ClearPlaylist();

    // window callbacks
    static void OnFrame(kui::Window& w, kui::Canvas& c, uint32_t now_ms);
    static bool OnEvent(kui::Window& w, const kui::Event& e);
    static void WindowGone(kui::Window* who);    // the kui window was destroyed (satoru)

    // exposed state for render
    static int  win_id;

private:
    static PlaylistEntry playlist[MP_MAX_PLAYLIST];
    static int  playlist_count;
    static int  current_track;
    static int  playback_progress;      // 0-100
    static bool playing;
    static bool paused;
    static int  seek_position;
    static int  volume_pct;             // 0-100

    // ui
    static int  hover_button;
    static int  scroll_offset;
    static bool show_playlist;
    static bool dragging_seek;
    static bool dragging_vol;

    // cached video preview data (avoids re-reading every frame)
    static int   cached_track;        // which track the cache is for
    static int   cached_vwidth;
    static int   cached_vheight;
    static int   cached_vcodec;
    static bool  cached_has_video;
    static bool  cached_has_audio;
    static bool  cached_valid;
    static uint8_t  video_frame_buf[4096]; // raw frame data for visualization
    static int   video_frame_len;
    static int   video_anim_frame;    // animation counter for video viewport

    // decoded-frame-rate counter for the video viewport overlay (satoru)
    static int      fps_frame_accum;   // frames decoded in the current 1s window
    static int      fps_display;       // frames-per-second from the last window
    static uint32_t fps_window_start_ms;

    static void RenderPlayerUI(kui::Canvas& c);
    static void RenderPlaylist(kui::Canvas& c);
    static void RenderControls(kui::Canvas& c);
    static void RenderProgressBar(kui::Canvas& c);
    static void RenderVideoPreview(kui::Canvas& c);
    static void RenderVolumeBar(kui::Canvas& c);
    static void RenderCodecInfo(kui::Canvas& c);
    static void CacheVideoInfo(int track_idx);
    static void LoadFile(const char* path);
    static MediaType DetectMediaType(const char* path);
};

