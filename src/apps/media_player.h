#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Media Player Application
//  Supports PCM audio playback and basic media file UI
// ═══════════════════════════════════════════════════════════════════════════
#include "../ui/window_manager.h"
#include "../kernel/types.h"

#define MP_MAX_PLAYLIST 32
#define MP_MAX_NAME     64

enum MediaType {
    MEDIA_UNKNOWN = 0,
    MEDIA_AUDIO,
    MEDIA_VIDEO
};

struct PlaylistEntry {
    char name[MP_MAX_NAME];
    char path[128];
    MediaType type;
    int  duration_sec;    // Duration in seconds (estimated)
    int  file_size;
    bool is_playing;
};

class MediaPlayerApp {
public:
    static void Open();
    static void Open(const char* file_path);
    static void Close();
    static bool IsOpen();

    // Playback
    static void Play();
    static void Pause();
    static void Stop();
    static void Next();
    static void Previous();
    static void Seek(int position_pct);  // 0-100

    // Playlist
    static void AddToPlaylist(const char* path);
    static void ClearPlaylist();

    // Window callbacks
    static void OnRender(Window* w);
    static void OnInput(Window* w, int event, int a, int b);

private:
    static int  win_id;
    static PlaylistEntry playlist[MP_MAX_PLAYLIST];
    static int  playlist_count;
    static int  current_track;
    static int  playback_progress;     // 0-100
    static bool playing;
    static bool paused;
    static int  seek_position;         // Click tracking

    // UI
    static int  hover_button;          // 0=prev, 1=play/pause, 2=stop, 3=next, 4=seek
    static int  scroll_offset;
    static bool show_playlist;

    static void RenderPlayerUI(Window* w);
    static void RenderPlaylist(Window* w);
    static void RenderControls(Window* w);
    static void RenderProgressBar(Window* w);
    static void RenderVideoPreview(Window* w);
    static void LoadFile(const char* path);
    static MediaType DetectMediaType(const char* path);
};
