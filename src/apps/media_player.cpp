// ═══════════════════════════════════════════════════════════════════════════
//  Kurono OS — Media Player Application Implementation
// ═══════════════════════════════════════════════════════════════════════════
#include "media_player.h"
#include "../drivers/graphics.h"
#include "../drivers/audio.h"
#include "../ui/window_manager.h"
#include "../fs/vfs.h"
#include "../fs/kvfs.h"
#include <string.h>

// ── Static member initialization ──
int  MediaPlayerApp::win_id          = -1;
PlaylistEntry MediaPlayerApp::playlist[MP_MAX_PLAYLIST];
int  MediaPlayerApp::playlist_count  = 0;
int  MediaPlayerApp::current_track   = -1;
int  MediaPlayerApp::playback_progress = 0;
bool MediaPlayerApp::playing         = false;
bool MediaPlayerApp::paused          = false;
int  MediaPlayerApp::seek_position   = -1;
int  MediaPlayerApp::hover_button    = -1;
int  MediaPlayerApp::scroll_offset   = 0;
bool MediaPlayerApp::show_playlist   = true;

// ── Helpers ──
static int mp_slen(const char* s) { int n=0; if(s) while(s[n]) n++; return n; }
static void mp_scpy(char* d, const char* s, int mx) {
    int i=0; if(s) while(s[i] && i<mx-1) { d[i]=s[i]; i++; } d[i]=0;
}
static bool mp_ends_with(const char* s, const char* suffix) {
    int sl = mp_slen(s), el = mp_slen(suffix);
    if (el > sl) return false;
    for (int i = 0; i < el; i++) {
        char a = s[sl - el + i]; char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}
static const char* mp_basename(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last = p + 1;
    }
    return last;
}
static void mp_int_str(int v, char* b, int mx) {
    if(mx<2){b[0]=0;return;}
    char t[16]; int n=0;
    if(v<0){b[0]='-';mp_int_str(-v,b+1,mx-1);return;}
    do{t[n++]='0'+(v%10);v/=10;}while(v&&n<15);
    int i=0;while(n>0&&i<mx-1)b[i++]=t[--n];b[i]=0;
}

// ── UI Colors ──
static const uint32_t MP_BG         = 0xFF121220;
static const uint32_t MP_BG_LIGHT   = 0xFF1A1A30;
static const uint32_t MP_ACCENT     = 0xFF5C8AFF;
static const uint32_t MP_ACCENT_DIM = 0xFF3A5AB0;
static const uint32_t MP_TEXT       = 0xFFE0E0F0;
static const uint32_t MP_TEXT_DIM   = 0xFF888899;
static const uint32_t MP_PROGRESS_BG = 0xFF333355;
static const uint32_t MP_RED        = 0xFFE74C3C;
static const uint32_t MP_GREEN      = 0xFF2ECC71;

// ═══════════════════════════════════════════════════════════════════════════
//  Window Management
// ═══════════════════════════════════════════════════════════════════════════

void MediaPlayerApp::Open() {
    if (win_id >= 0) return;

    win_id = WindowManager::CreateWindow("Media Player", 200, 120, 500, 380,
        (WindowRenderFunc)[](Window* w, int cx, int cy, int cw, int ch) {
            (void)cx; (void)cy; (void)cw; (void)ch;
            MediaPlayerApp::OnRender(w);
        },
        (WindowInputFunc)MediaPlayerApp::OnInput
    );
    if (win_id < 0) return;

    // Add some demo entries if playlist is empty
    if (playlist_count == 0) {
        AddToPlaylist("/home/user/Documents/denji.mp4");
        AddToPlaylist("/home/user/Music/startup.wav");
        AddToPlaylist("/home/user/Music/notification.wav");
    }
}

void MediaPlayerApp::Open(const char* file_path) {
    Open();
    if (file_path) {
        AddToPlaylist(file_path);
        current_track = playlist_count - 1;
    }
}

void MediaPlayerApp::Close() {
    Stop();
    if (win_id >= 0) {
        WindowManager::CloseWindow(win_id);
        win_id = -1;
    }
}

bool MediaPlayerApp::IsOpen() {
    return win_id >= 0;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Media Type Detection
// ═══════════════════════════════════════════════════════════════════════════

MediaType MediaPlayerApp::DetectMediaType(const char* path) {
    if (mp_ends_with(path, ".mp4") || mp_ends_with(path, ".avi") ||
        mp_ends_with(path, ".mkv") || mp_ends_with(path, ".webm") ||
        mp_ends_with(path, ".mov")) {
        return MEDIA_VIDEO;
    }
    if (mp_ends_with(path, ".wav") || mp_ends_with(path, ".pcm") ||
        mp_ends_with(path, ".mp3") || mp_ends_with(path, ".ogg") ||
        mp_ends_with(path, ".flac")) {
        return MEDIA_AUDIO;
    }
    return MEDIA_UNKNOWN;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Playlist Management
// ═══════════════════════════════════════════════════════════════════════════

void MediaPlayerApp::AddToPlaylist(const char* path) {
    if (playlist_count >= MP_MAX_PLAYLIST) return;
    if (!path) return;

    PlaylistEntry* e = &playlist[playlist_count++];
    mp_scpy(e->path, path, 128);
    mp_scpy(e->name, mp_basename(path), MP_MAX_NAME);
    e->type = DetectMediaType(path);
    e->is_playing = false;

    // Try to read real file metadata from KVFS
    e->file_size = 0;
    e->duration_sec = 0;
    char kvfs_buf[256];
    int rd = KVFS::ReadFile(path, kvfs_buf, sizeof(kvfs_buf) - 1);
    if (rd > 0) {
        kvfs_buf[rd] = 0;
        e->file_size = rd;
        // Parse duration from metadata string like "... 3:42]"
        // Scan backwards for M:SS or MM:SS pattern
        for (int i = rd - 1; i >= 2; i--) {
            if (kvfs_buf[i] == ']' || kvfs_buf[i] == ' ') continue;
            // Find colon
            if (kvfs_buf[i] >= '0' && kvfs_buf[i] <= '9') {
                // Look for M:SS pattern
                int j = i;
                while (j >= 0 && kvfs_buf[j] >= '0' && kvfs_buf[j] <= '9') j--;
                if (j >= 0 && kvfs_buf[j] == ':') {
                    // Parse seconds (digits after colon)
                    int sec = 0, mul = 1;
                    for (int k = i; k > j; k--) {
                        sec += (kvfs_buf[k] - '0') * mul;
                        mul *= 10;
                    }
                    // Parse minutes (digits before colon)
                    int min_end = j - 1;
                    int minutes = 0; mul = 1;
                    for (int k = min_end; k >= 0 && kvfs_buf[k] >= '0' && kvfs_buf[k] <= '9'; k--) {
                        minutes += (kvfs_buf[k] - '0') * mul;
                        mul *= 10;
                    }
                    e->duration_sec = minutes * 60 + sec;
                    break;
                }
            }
        }
        if (e->duration_sec == 0) {
            e->duration_sec = (e->type == MEDIA_VIDEO) ? 180 : 60;
        }
    } else {
        e->duration_sec = (e->type == MEDIA_VIDEO) ? 180 : 60;
    }
}

void MediaPlayerApp::ClearPlaylist() {
    playlist_count = 0;
    current_track = -1;
    Stop();
}

// ═══════════════════════════════════════════════════════════════════════════
//  Playback Control
// ═══════════════════════════════════════════════════════════════════════════

void MediaPlayerApp::Play() {
    if (playlist_count == 0) return;
    if (current_track < 0) current_track = 0;

    if (paused) {
        Audio::Resume();
        paused = false;
        playing = true;
        return;
    }

    // Generate a continuous tone that loops — different per track
    if (Audio::IsAvailable()) {
        // Different frequencies for different tracks to prove audio works
        static const int track_freqs[] = {440, 523, 587, 659, 784, 880, 988, 1047};
        int freq = track_freqs[current_track % 8];

        // For video files, use a chord-like pattern (lower frequency)
        if (current_track >= 0 && current_track < playlist_count &&
            playlist[current_track].type == MEDIA_VIDEO) {
            freq = 330; // E4 — cinematic feel
        }

        Audio::PlayLoopTone(freq, 60);
    }

    playing = true;
    paused = false;
    playback_progress = 0;

    if (current_track >= 0 && current_track < playlist_count) {
        for (int i = 0; i < playlist_count; i++) playlist[i].is_playing = false;
        playlist[current_track].is_playing = true;
    }
}

void MediaPlayerApp::Pause() {
    if (!playing) return;
    Audio::Pause();
    playing = false;
    paused = true;
}

void MediaPlayerApp::Stop() {
    Audio::Stop();
    playing = false;
    paused = false;
    playback_progress = 0;
    for (int i = 0; i < playlist_count; i++) playlist[i].is_playing = false;
}

void MediaPlayerApp::Next() {
    if (playlist_count == 0) return;
    bool was_playing = playing;
    Stop();
    current_track = (current_track + 1) % playlist_count;
    if (was_playing) Play();
}

void MediaPlayerApp::Previous() {
    if (playlist_count == 0) return;
    bool was_playing = playing;
    Stop();
    current_track--;
    if (current_track < 0) current_track = playlist_count - 1;
    if (was_playing) Play();
}

void MediaPlayerApp::Seek(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    playback_progress = pct;
}

// ═══════════════════════════════════════════════════════════════════════════
//  Rendering
// ═══════════════════════════════════════════════════════════════════════════

void MediaPlayerApp::RenderVideoPreview(Window* w) {
    int cx = w->content_x;
    int cy = w->content_y;
    int cw = w->content_w;

    // Video preview area (top portion)
    int preview_h = 180;
    Graphics::FillRect(cx, cy, cw, preview_h, 0xFF0A0A15);

    // If video file selected, show cinematic preview
    if (current_track >= 0 && current_track < playlist_count &&
        playlist[current_track].type == MEDIA_VIDEO) {

        // Read metadata from KVFS to show info
        char meta[256] = {0};
        int mrd = KVFS::ReadFile(playlist[current_track].path, meta, 255);
        if (mrd > 0) meta[mrd] = 0;

        // Film frame border (cinematic letterbox look)
        int border = 16;
        Graphics::FillRect(cx + border, cy + border, cw - border*2, preview_h - border*2, 0xFF181828);
        // Widescreen bars (letterbox)
        Graphics::FillRect(cx + border, cy + border, cw - border*2, 14, 0xFF000000);
        Graphics::FillRect(cx + border, cy + preview_h - border - 14, cw - border*2, 14, 0xFF000000);

        // Large play triangle in center
        int tri_cx = cx + cw/2;
        int tri_cy = cy + preview_h/2;
        // Circle background
        Graphics::FillCircle(tri_cx, tri_cy, 22, 0x88000000);
        Graphics::DrawCircle(tri_cx, tri_cy, 22, MP_ACCENT);
        // Play triangle
        for (int r = 0; r < 20; r++) {
            int w = (r < 10) ? (r * 2) : ((20 - r) * 2);
            if (w > 0) Graphics::FillRect(tri_cx - 6, tri_cy - 10 + r, w, 1, 0xFFFFFFFF);
        }

        // Show metadata info below preview
        const char* name = playlist[current_track].name;
        int nw = mp_slen(name) * 8;
        Graphics::DrawString(cx + cw/2 - nw/2, cy + preview_h - 28, name, MP_TEXT, 0xFF000000);

        // Show resolution/codec if metadata exists
        if (mrd > 0) {
            // Extract useful info (e.g. "1920x1080 H.264")
            Graphics::DrawString(cx + border + 4, cy + border + 2, meta + 1, 0xFF888899, 0xFF000000);
        }
    } else if (current_track >= 0 && current_track < playlist_count &&
               playlist[current_track].type == MEDIA_AUDIO) {
        // Audio file — show waveform visualization
        Graphics::FillRect(cx, cy, cw, preview_h, 0xFF0A0A18);

        // Show track name
        const char* name = playlist[current_track].name;
        int nw = mp_slen(name) * 8;
        Graphics::DrawString(cx + cw/2 - nw/2, cy + 20, name, MP_ACCENT, 0xFF000000);

        // Music note icon
        int note_cx = cx + cw/2;
        int note_cy = cy + preview_h/2 - 5;
        Graphics::FillCircle(note_cx - 8, note_cy + 12, 6, MP_ACCENT);
        Graphics::FillCircle(note_cx + 12, note_cy + 8, 6, MP_ACCENT);
        Graphics::FillRect(note_cx - 2, note_cy - 20, 3, 32, MP_ACCENT);
        Graphics::FillRect(note_cx + 18, note_cy - 24, 3, 32, MP_ACCENT);
        Graphics::FillRect(note_cx - 2, note_cy - 20, 23, 3, MP_ACCENT);

        // Animated equalizer bars when playing
        if (playing) {
            static int eq_frame = 0;
            eq_frame++;
            for (int i = 0; i < 11; i++) {
                int bar_h = 8 + ((eq_frame * 3 + i * 13) % 40);
                int bx = cx + cw/2 - 50 + i * 10;
                int by = cy + preview_h - 10;
                uint32_t col = (i % 3 == 0) ? MP_ACCENT : MP_ACCENT_DIM;
                Graphics::FillRect(bx, by - bar_h, 7, bar_h, col);
            }
        }
    } else {
        // No media selected
        Graphics::DrawString(cx + cw/2 - 48, cy + preview_h/2 - 8,
                             "No Media", MP_TEXT_DIM, 0xFF000000);
        Graphics::DrawString(cx + cw/2 - 80, cy + preview_h/2 + 10,
                             "Open from playlist", MP_TEXT_DIM, 0xFF000000);
    }
}

void MediaPlayerApp::RenderProgressBar(Window* w) {
    int cx = w->content_x;
    int cy = w->content_y;
    int cw = w->content_w;
    int bar_y = cy + 186;

    // Progress bar track
    Graphics::FillRect(cx + 10, bar_y, cw - 20, 4, MP_PROGRESS_BG);

    // Filled portion
    int filled = ((cw - 20) * playback_progress) / 100;
    if (filled > 0) {
        Graphics::FillRect(cx + 10, bar_y, filled, 4, MP_ACCENT);
    }

    // Knob
    int knob_x = cx + 10 + filled;
    Graphics::FillCircle(knob_x, bar_y + 2, 5, 0xFFFFFFFF);
    Graphics::DrawCircle(knob_x, bar_y + 2, 5, MP_ACCENT);

    // Time labels
    if (current_track >= 0 && current_track < playlist_count) {
        int dur = playlist[current_track].duration_sec;
        int cur = (dur * playback_progress) / 100;
        char cur_str[16], dur_str[16];
        // Format MM:SS
        char m[4], s[4];
        mp_int_str(cur / 60, m, 4);
        mp_int_str(cur % 60, s, 4);
        mp_scpy(cur_str, m, 16);
        int cl = mp_slen(cur_str);
        cur_str[cl] = ':'; cur_str[cl+1] = 0;
        if (cur % 60 < 10) { int l2=mp_slen(cur_str); cur_str[l2]='0'; cur_str[l2+1]=0; }
        char s2[4]; mp_int_str(cur%60, s2, 4);
        int l3 = mp_slen(cur_str);
        for(int i=0; s2[i] && l3<15; i++) cur_str[l3++] = s2[i];
        cur_str[l3] = 0;

        mp_int_str(dur / 60, m, 4);
        mp_scpy(dur_str, m, 16);
        cl = mp_slen(dur_str);
        dur_str[cl] = ':'; dur_str[cl+1] = 0;
        if (dur % 60 < 10) { int l2=mp_slen(dur_str); dur_str[l2]='0'; dur_str[l2+1]=0; }
        mp_int_str(dur%60, s2, 4);
        l3 = mp_slen(dur_str);
        for(int i=0; s2[i] && l3<15; i++) dur_str[l3++] = s2[i];
        dur_str[l3] = 0;

        Graphics::DrawString(cx + 10, bar_y + 8, cur_str, MP_TEXT_DIM, 0xFF000000);
        int dw = mp_slen(dur_str) * 8;
        Graphics::DrawString(cx + cw - 10 - dw, bar_y + 8, dur_str, MP_TEXT_DIM, 0xFF000000);
    }
}

void MediaPlayerApp::RenderControls(Window* w) {
    int cx = w->content_x;
    int cy = w->content_y;
    int cw = w->content_w;
    int ctrl_y = cy + 208;

    // Control buttons centered
    int btn_w = 36, btn_h = 36;
    int total_w = btn_w * 4 + 12 * 3;
    int bx = cx + cw/2 - total_w/2;

    // Previous
    Graphics::FillRoundedRect(bx, ctrl_y, btn_w, btn_h, 6, MP_BG_LIGHT);
    // << arrows
    Graphics::DrawString(bx + 8, ctrl_y + 10, "<<", MP_TEXT, 0xFF000000);
    bx += btn_w + 12;

    // Play/Pause
    uint32_t play_col = playing ? MP_GREEN : MP_ACCENT;
    Graphics::FillRoundedRect(bx, ctrl_y, btn_w, btn_h, 6, play_col);
    if (playing) {
        // Pause bars
        Graphics::FillRect(bx + 12, ctrl_y + 10, 4, 16, 0xFFFFFFFF);
        Graphics::FillRect(bx + 20, ctrl_y + 10, 4, 16, 0xFFFFFFFF);
    } else {
        // Play triangle
        for (int r = 0; r < 16; r++) {
            int ww = (r < 8) ? r : (16 - r);
            Graphics::FillRect(bx + 12, ctrl_y + 10 + r, ww, 1, 0xFFFFFFFF);
        }
    }
    bx += btn_w + 12;

    // Stop
    Graphics::FillRoundedRect(bx, ctrl_y, btn_w, btn_h, 6, MP_BG_LIGHT);
    Graphics::FillRect(bx + 11, ctrl_y + 11, 14, 14, MP_RED);
    bx += btn_w + 12;

    // Next
    Graphics::FillRoundedRect(bx, ctrl_y, btn_w, btn_h, 6, MP_BG_LIGHT);
    Graphics::DrawString(bx + 8, ctrl_y + 10, ">>", MP_TEXT, 0xFF000000);
}

void MediaPlayerApp::RenderPlaylist(Window* w) {
    int cx = w->content_x;
    int cy = w->content_y;
    int cw = w->content_w;
    int ch = w->content_h;
    int list_y = cy + 260;
    int list_h = ch - 260;

    if (list_h <= 0) return;

    // Playlist header
    Graphics::FillRect(cx, list_y, cw, 20, MP_BG_LIGHT);
    Graphics::DrawString(cx + 8, list_y + 4, "Playlist", MP_TEXT, 0xFF000000);

    char count_str[16];
    mp_int_str(playlist_count, count_str, 16);
    int csw = mp_slen(count_str) * 8;
    Graphics::DrawString(cx + cw - csw - 8, list_y + 4, count_str, MP_TEXT_DIM, 0xFF000000);

    // Playlist items
    int iy = list_y + 22;
    for (int i = scroll_offset; i < playlist_count && iy < cy + ch - 4; i++) {
        PlaylistEntry* e = &playlist[i];
        bool selected = (i == current_track);

        // Background
        uint32_t bg = selected ? 0xFF252540 : ((i % 2) ? MP_BG : 0xFF151525);
        Graphics::FillRect(cx + 2, iy, cw - 4, 24, bg);

        // Playing indicator
        if (e->is_playing) {
            Graphics::FillCircle(cx + 12, iy + 12, 4, MP_GREEN);
        }

        // Media type icon
        const char* icon = (e->type == MEDIA_VIDEO) ? "[V]" : "[A]";
        uint32_t icon_col = (e->type == MEDIA_VIDEO) ? MP_ACCENT : MP_GREEN;
        Graphics::DrawString(cx + 22, iy + 5, icon, icon_col, 0xFF000000);

        // File name (truncated)
        char display_name[40];
        mp_scpy(display_name, e->name, 36);
        Graphics::DrawString(cx + 50, iy + 5, display_name, selected ? MP_TEXT : MP_TEXT_DIM, 0xFF000000);

        // Duration
        char dur[8];
        mp_int_str(e->duration_sec / 60, dur, 8);
        int dl = mp_slen(dur);
        dur[dl] = ':'; dur[dl+1] = 0;
        if (e->duration_sec % 60 < 10) { dur[mp_slen(dur)] = '0'; dur[mp_slen(dur)+1] = 0; }
        char sec[4]; mp_int_str(e->duration_sec % 60, sec, 4);
        dl = mp_slen(dur);
        for(int j=0; sec[j] && dl<7; j++) dur[dl++] = sec[j];
        dur[dl] = 0;
        int dw = mp_slen(dur) * 8;
        Graphics::DrawString(cx + cw - dw - 8, iy + 5, dur, MP_TEXT_DIM, 0xFF000000);

        iy += 26;
    }
}

void MediaPlayerApp::RenderPlayerUI(Window* w) {
    int cx = w->content_x;
    int cy = w->content_y;
    int cw = w->content_w;
    int ch = w->content_h;

    // Background
    Graphics::FillRect(cx, cy, cw, ch, MP_BG);

    RenderVideoPreview(w);
    RenderProgressBar(w);
    RenderControls(w);
    RenderPlaylist(w);
}

void MediaPlayerApp::OnRender(Window* w) {
    if (!w) return;
    RenderPlayerUI(w);

    // Advance progress if playing
    if (playing) {
        // Simulated progress: advance slowly each frame
        static int frame_acc = 0;
        frame_acc++;
        if (frame_acc >= 30) {  // ~every 0.2 sec
            frame_acc = 0;
            playback_progress++;
            if (playback_progress > 100) {
                playback_progress = 0;
                Next();  // Auto-advance to next track
            }
        }
    }
}

void MediaPlayerApp::OnInput(Window* w, int event, int a, int b) {
    if (!w) return;

    if (event == 1) {
        // Mouse click: a=local_x, b=local_y (relative to content area)
        int cw = w->content_w;

        // Progress bar click (local y ~= 186, height ~= 10px)
        if (b >= 180 && b <= 200) {
            int rel = a - 10;
            int bar_w = cw - 20;
            if (rel >= 0 && rel <= bar_w) {
                Seek((rel * 100) / bar_w);
            }
            return;
        }

        // Control buttons (local y ~= 208, height 36)
        if (b >= 208 && b <= 244) {
            int btn_w = 36;
            int total_w = btn_w * 4 + 12 * 3;
            int bx = cw/2 - total_w/2;

            if (a >= bx && a < bx + btn_w) { Previous(); return; }
            bx += btn_w + 12;
            if (a >= bx && a < bx + btn_w) {
                if (playing) Pause(); else Play();
                return;
            }
            bx += btn_w + 12;
            if (a >= bx && a < bx + btn_w) { Stop(); return; }
            bx += btn_w + 12;
            if (a >= bx && a < bx + btn_w) { Next(); return; }
        }

        // Playlist click (local y >= 282)
        if (b >= 282) {
            int idx = scroll_offset + (b - 282) / 26;
            if (idx >= 0 && idx < playlist_count) {
                bool was_playing = playing;
                Stop();
                current_track = idx;
                if (was_playing) Play();
            }
            return;
        }
    }

    if (event == 2) {
        // Keyboard: a=key
        if (a == ' ') { if (playing) Pause(); else Play(); }
        if (a == 's' || a == 'S') Stop();
        if (a == 'n' || a == 'N') Next();
        if (a == 'p' || a == 'P') Previous();
    }

    if (event == 3) {
        // Scroll
        scroll_offset -= a;
        if (scroll_offset < 0) scroll_offset = 0;
        if (scroll_offset >= playlist_count) scroll_offset = playlist_count - 1;
        if (scroll_offset < 0) scroll_offset = 0;
    }
}

void MediaPlayerApp::LoadFile(const char* path) {
    if (!path) return;
    AddToPlaylist(path);
    current_track = playlist_count - 1;
}
