//  kurono os - media player application implementation
//  supports wav/mp3/aac/flac/mp4 via codecregistry + sb16/ac97/hda output
#include "media_player.h"
#include "media_audio.h"
#include "../media/codec.h"
// no kernel headers: windows, drawing, input, files and time come from kui and audio from MpAudio, so
// this file builds for the in-kernel backend and, later, for ring 3. the denji video player, the runtime
// log and the taskmem scopes are kernel-only and sit behind KUI_BACKEND_WAYLAND. (satoru)
#ifndef KUI_BACKEND_WAYLAND
#include "../system/logging.h"
#include "denji_app.h"
#include "../kernel/taskmem.h"   // kurono.taskmem=1 app scopes (satoru)
#include "../drivers/mouse.h"     // drag safety net below (satoru)
#define MP_APPLOG(ev, detail) RuntimeLog::LogAppEvent("media", (ev), (detail))
#else
#define MP_APPLOG(ev, detail) kui::Log("media", (ev))
#endif

// forward declaration
static bool PlayDecodedAudio(const uint8_t* data, int len, int rate, int bits, int channels);

static kui::Window* g_win = nullptr;       // the open window, null when closed (satoru)
static uint32_t     g_prog_t0 = 0;         // when the progress bar last advanced (satoru)

int  MediaPlayerApp::win_id          = -1;
PlaylistEntry MediaPlayerApp::playlist[MP_MAX_PLAYLIST];
int  MediaPlayerApp::playlist_count  = 0;
int  MediaPlayerApp::current_track   = -1;
int  MediaPlayerApp::playback_progress = 0;
bool MediaPlayerApp::playing         = false;
bool MediaPlayerApp::paused          = false;
int  MediaPlayerApp::seek_position   = -1;
int  MediaPlayerApp::volume_pct      = 80;
int  MediaPlayerApp::hover_button    = -1;
int  MediaPlayerApp::scroll_offset   = 0;
bool MediaPlayerApp::show_playlist   = true;
bool MediaPlayerApp::dragging_seek   = false;
bool MediaPlayerApp::dragging_vol    = false;

int      MediaPlayerApp::cached_track      = -1;
int      MediaPlayerApp::cached_vwidth     = 0;
int      MediaPlayerApp::cached_vheight    = 0;
int      MediaPlayerApp::cached_vcodec     = 0;
bool     MediaPlayerApp::cached_has_video  = false;
bool     MediaPlayerApp::cached_has_audio  = false;
bool     MediaPlayerApp::cached_valid      = false;
uint8_t  MediaPlayerApp::video_frame_buf[4096] = {0};
int      MediaPlayerApp::video_frame_len   = 0;
int      MediaPlayerApp::video_anim_frame  = 0;
int      MediaPlayerApp::fps_frame_accum   = 0;
int      MediaPlayerApp::fps_display       = 0;
uint32_t MediaPlayerApp::fps_window_start_ms = 0;

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

// the smooth face at the size Graphics::DrawString used (16 px scaled by the ui scale); classic
// style falls back to the 8x16 bitmap inside kui, so the player looks as before. (satoru)
static int mp_text(kui::Canvas& c, int x, int y, const char* s, uint32_t color) {
    return c.Text(x, y, kui::Face::Sans, 16.0f * (float)kui::CurrentTheme().ui_scale / 100.0f, s, color);
}

static const uint32_t MP_BG         = 0xFF121220;
static const uint32_t MP_BG_LIGHT   = 0xFF1A1A30;
static const uint32_t MP_ACCENT     = 0xFF5C8AFF;
static const uint32_t MP_ACCENT_DIM = 0xFF3A5AB0;
static const uint32_t MP_TEXT       = 0xFFE0E0F0;
static const uint32_t MP_TEXT_DIM   = 0xFF888899;
static const uint32_t MP_PROGRESS_BG = 0xFF333355;
static const uint32_t MP_RED        = 0xFFE74C3C;
static const uint32_t MP_GREEN      = 0xFF2ECC71;

//  window management

// the old audio window, now only used for audio files. the bare launch (dock,
// start menu, desktop icon) opens the video library instead. (satoru)
// the window is a kui handler: it only forwards frames and events to the player, and clears the
// player's window state when the window goes away however it was closed (title bar, task manager,
// resolution change). (satoru)
namespace {
struct MpWindow : kui::AppHandler {
    kui::Window* self = nullptr;
    void OnCreate(kui::Window& w) override { self = &w; g_win = &w; MediaPlayerApp::win_id = 1; }
    void OnFrame(kui::Window& w, kui::Canvas& c, uint32_t now) override { MediaPlayerApp::OnFrame(w, c, now); }
    bool OnEvent(kui::Window& w, const kui::Event& e) override { return MediaPlayerApp::OnEvent(w, e); }
    void OnTick(kui::Window& w, uint32_t now) override { (void)w; (void)now; MpAudio::Pump(); }   // keep the stream fed while hidden (satoru)
    ~MpWindow() override { MediaPlayerApp::WindowGone(self); }
};
}

static kui::AppHandler* MakePlayerWindow() { return new MpWindow; }

// content size: the old 500x380 window minus the 1 px borders and the 36 px title bar (satoru)
static const kui::WindowSpec kPlayerSpec = {
    "Media Player", "mediaplayer", 498, 343, 138, 63, 200, 120, true, true, 0, -1
};

#if defined(KUI_BACKEND_WAYLAND)
// ring 3 launch with no file: list the usual audio folders so the window has tracks to play. (satoru)
static void MpScanAudio() {
    static const char* dirs[] = { "/home/user/Music", "/home/user/Downloads" };
    static kui::DirEntry kids[64];
    for (int di = 0; di < 2; di++) {
        const char* dir = dirs[di];
        int n = kui::Fs::ListDir(dir, kids, 64);
        for (int i = 0; i < n; i++) {
            if (kids[i].dir) continue;
            const char* nm = kids[i].name;
            if (!(mp_ends_with(nm, ".wav") || mp_ends_with(nm, ".mp3") || mp_ends_with(nm, ".flac") ||
                  mp_ends_with(nm, ".aac") || mp_ends_with(nm, ".ogg") || mp_ends_with(nm, ".pcm")))
                continue;
            char full[192];
            int p = 0;
            for (const char* q = dir; *q && p < 180; q++) full[p++] = *q;
            full[p++] = '/';
            for (const char* q = nm; *q && p < 190; q++) full[p++] = *q;
            full[p] = 0;
            MediaPlayerApp::AddToPlaylist(full);
        }
    }
}
int main(int argc, char** argv) {
    if (argc > 1 && argv[1] && argv[1][0]) MediaPlayerApp::Open(argv[1]);
    else MpScanAudio();
    return kui::Run(kPlayerSpec, MakePlayerWindow);
}
#else
KUI_APP(mediaplayer, kPlayerSpec, MakePlayerWindow)
#endif

static void MpCreateAudioWindow() {
    if (MediaPlayerApp::win_id >= 0) return;
    MP_APPLOG("open", nullptr);

    MediaPlayerApp::win_id = 1;    // claimed now so a second open can't race the window coming up (satoru)
#if !defined(KUI_BACKEND_WAYLAND)
    if (kui_launch_mediaplayer() != 0 && !g_win) MediaPlayerApp::win_id = -1;
#endif
    if (MediaPlayerApp::win_id < 0) return;

    // DISABLED: auto-loading + playing the preset media here breaks the global
    // audio init (the player's own mp4/wav audio path corrupts the AudioServer
    // backend state - completely broken). the file-explorer double-click path
    // (Open(file_path) below) works perfectly and is the supported way in. so the
    // bare "Media Player" launch now opens an empty player instead of a preset
    // playlist. revisit when the player's audio engine is rewritten. (satoru)
    // if (playlist_count == 0) {
    //     AddToPlaylist("/home/user/Documents/denji.mp4");
    //     AddToPlaylist("/home/user/Music/startup.wav");
    //     AddToPlaylist("/home/user/Music/notification.wav");
    // }
}

#ifndef KUI_BACKEND_WAYLAND
void MediaPlayerApp::Open() {
    TaskMem::AppScope _tms(TaskMem::APP_MEDIA);   // kurono.taskmem=1: charge to the app (satoru)
    DenjiApp::OpenLibrary();
}
#endif

void MediaPlayerApp::Open(const char* file_path) {
#ifndef KUI_BACKEND_WAYLAND
    TaskMem::AppScope _tms(TaskMem::APP_MEDIA);   // kurono.taskmem=1: charge to the app (satoru)
    // video files play in the video player, which has a real clock, seeking and
    // working audio; this window only handles audio files now. (satoru)
    if (file_path && (DetectMediaType(file_path) == MEDIA_VIDEO ||
                      mp_ends_with(file_path, ".kvid") || mp_ends_with(file_path, ".kvd"))) {
        if (DenjiApp::OpenFile(file_path)) return;
    }
#endif
    MpCreateAudioWindow();
    if (file_path) {
        MP_APPLOG("queue-file", file_path);
        // dedupe: jump to existing entry if already in playlist
        for (int i = 0; i < playlist_count; i++) {
            int j = 0; bool same = true;
            while (file_path[j] || playlist[i].path[j]) {
                if (file_path[j] != playlist[i].path[j]) { same = false; break; }
                j++;
            }
            if (same) { current_track = i; if (g_win) g_win->Invalidate(); return; }
        }
        AddToPlaylist(file_path);
        current_track = playlist_count - 1;
        if (g_win) g_win->Invalidate();
    }
}

void MediaPlayerApp::Close() {
    Stop();
    if (g_win) {
        kui::Window* w = g_win;
        g_win = nullptr;
        win_id = -1;
        w->Close();
    }
    win_id = -1;
    // reset UI/cache state so reopen starts fresh and doesn't leak stale data
    hover_button = -1;
    scroll_offset = 0;
    dragging_seek = false;
    dragging_vol = false;
    cached_track = -1;
    cached_valid = false;
    video_frame_len = 0;
    seek_position = -1;
    paused = false;
}

bool MediaPlayerApp::IsOpen() {
    return win_id >= 0;
}

//  media type detection

MediaType MediaPlayerApp::DetectMediaType(const char* path) {
    if (mp_ends_with(path, ".mp4") || mp_ends_with(path, ".avi") ||
        mp_ends_with(path, ".mkv") || mp_ends_with(path, ".webm") ||
        mp_ends_with(path, ".mov")) {
        return MEDIA_VIDEO;
    }
    if (mp_ends_with(path, ".wav") || mp_ends_with(path, ".pcm") ||
        mp_ends_with(path, ".mp3") || mp_ends_with(path, ".ogg") ||
        mp_ends_with(path, ".flac") || mp_ends_with(path, ".aac") ||
        mp_ends_with(path, ".wma") || mp_ends_with(path, ".opus")) {
        return MEDIA_AUDIO;
    }
    return MEDIA_UNKNOWN;
}

//  playlist management

void MediaPlayerApp::AddToPlaylist(const char* path) {
    if (playlist_count >= MP_MAX_PLAYLIST) return;
    if (!path) return;
    MP_APPLOG("playlist-add", path);

    PlaylistEntry* e = &playlist[playlist_count++];
    mp_scpy(e->path, path, 128);
    mp_scpy(e->name, mp_basename(path), MP_MAX_NAME);
    e->type = DetectMediaType(path);
    e->codec = CODEC_UNKNOWN;
    e->is_playing = false;
    e->file_size = 0;
    e->duration_sec = 0;
    e->video_width = 0;
    e->video_height = 0;
    e->video_codec = 0;
    e->has_video = false;
    e->has_audio = false;

    // get actual file size first
    int actual_size = kui::Fs::FileSize(path);
    if (actual_size > 0) e->file_size = actual_size;

    // read header bytes for codec detection (16kb for mp4 moov atom scanning)
    static char hdr_buf[16384];
    int rd = kui::Fs::ReadFile(path, hdr_buf, sizeof(hdr_buf) - 1);
    if (rd <= 0) return;
    hdr_buf[rd] = 0;
    if (e->file_size == 0) e->file_size = rd;

    const uint8_t* data = (const uint8_t*)hdr_buf;

    // detect codec using codecregistry (magic bytes)
    e->codec = CodecRegistry::Detect(data, rd);
    if (e->codec == CODEC_UNKNOWN)
        e->codec = CodecRegistry::DetectByExtension(path);

    // update media type from codec if unknown
    if (e->type == MEDIA_UNKNOWN) {
        if (e->codec == CODEC_MP4)
            e->type = MEDIA_VIDEO;
        else if (e->codec != CODEC_UNKNOWN)
            e->type = MEDIA_AUDIO;
    }

    // codec-specific duration detection
    switch (e->codec) {
        case CODEC_WAV:
            // wav: duration from riff header
            if (rd >= 44 && data[0]=='R' && data[1]=='I' && data[2]=='F' && data[3]=='F' &&
                data[8]=='W' && data[9]=='A' && data[10]=='V' && data[11]=='E') {
                int channels    = data[22] | (data[23] << 8);
                int sample_rate = data[24] | (data[25] << 8) | (data[26] << 16) | (data[27] << 24);
                int bits        = data[34] | (data[35] << 8);
                int data_size   = data[40] | (data[41] << 8) | (data[42] << 16) | (data[43] << 24);
                if (sample_rate > 0 && channels > 0 && bits > 0) {
                    int bps = (bits / 8) * channels;
                    // compute byte rate in 64-bit: sample_rate*bps can overflow a
                    // 32-bit int to exactly 0 (crafted wav header) and divide-by-
                    // zero #de. guard the product is nonzero before dividing. (satoru)
                    uint64_t byte_rate = (uint64_t)(uint32_t)sample_rate * (uint32_t)bps;
                    if (byte_rate > 0)
                        e->duration_sec = (int)((uint64_t)(uint32_t)data_size / byte_rate);
                }
            }
            break;

        case CODEC_MP3:
            // mp3: check for id3v2 tag, then estimate from bitrate in first frame header
            {
                int hdr_offset = 0;
                // skip id3v2 tag if present
                if (rd >= 10 && data[0]=='I' && data[1]=='D' && data[2]=='3') {
                    int id3_size = ((data[6]&0x7F)<<21)|((data[7]&0x7F)<<14)|
                                   ((data[8]&0x7F)<<7)|(data[9]&0x7F);
                    hdr_offset = 10 + id3_size;
                }
                // find first mpeg sync word (0xffe or 0xfff)
                for (int i = hdr_offset; i < rd - 4; i++) {
                    if (data[i] == 0xFF && (data[i+1] & 0xE0) == 0xE0) {
                        // parse mpeg frame header
                        int layer       = (data[i+1] >> 1) & 0x3;  // 1 = layer iii (mp3)
                        int bitr_idx    = (data[i+2] >> 4) & 0xF;
                        int freq_idx    = (data[i+2] >> 2) & 0x3;
                        (void)layer;
                        // mp3 layer 3 bitrate table (kbps)
                        static const int br_tbl[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
                        // mp3 sample rate table
                        static const int sr_tbl[4] = {44100,48000,32000,0};
                        int bitrate_kbps = br_tbl[bitr_idx];
                        int samplerate   = sr_tbl[freq_idx];
                        (void)samplerate;
                        // duration: file_size * 8 bits / bitrate_bits_per_sec
                        if (bitrate_kbps > 0 && e->file_size > 0)
                            e->duration_sec = (e->file_size * 8) / (bitrate_kbps * 1000);
                        break;
                    }
                }
            }
            break;

        case CODEC_FLAC:
            // flac: parse streaminfo metadata block (always first)
            if (rd >= 42 && data[0]=='f' && data[1]=='L' && data[2]=='a' && data[3]=='C') {
                FLACStreamInfo fsi;
                if (CodecRegistry::ParseFLACStreamInfo(data, rd, &fsi)) {
                    if (fsi.sample_rate > 0 && fsi.total_samples > 0) {
                        // total_samples is 36-bit in spec but we store as int
                        e->duration_sec = (int)(fsi.total_samples / fsi.sample_rate);
                    }
                }
            }
            break;

        case CODEC_AAC:
            // aac (adts): estimate from first frame header
            {
                AACFrameHeader aac_hdr;
                if (CodecRegistry::ParseAACHeader(data, rd, &aac_hdr)) {
                    // adts frame: 1024 samples per frame
                    // count frames: frame_length is in header
                    if (aac_hdr.frame_length > 0 && aac_hdr.sample_rate > 0) {
                        int est_frames = e->file_size / aac_hdr.frame_length;
                        e->duration_sec = (est_frames * 1024) / aac_hdr.sample_rate;
                    }
                }
            }
            break;

        case CODEC_MP4:
            // mp4: parse container
            {
                MP4TrackInfo tr;
                if (CodecRegistry::ParseMP4(data, rd, &tr)) {
                    if (tr.duration_ms > 0)
                        e->duration_sec = tr.duration_ms / 1000;
                    // store video metadata in playlist entry
                    e->has_audio = tr.has_audio;
                    e->has_video = tr.has_video;
                    e->video_width = tr.video_width;
                    e->video_height = tr.video_height;
                    e->video_codec = tr.video_codec;
                    if (tr.has_video)
                        e->type = MEDIA_VIDEO;
                    else if (tr.has_audio)
                        e->type = MEDIA_AUDIO;
                }
            }
            break;

        default:
            // raw pcm estimate for completely unknown formats
            if (e->type == MEDIA_AUDIO && e->file_size > 44) {
                e->duration_sec = (e->file_size - 44) / 22050;
                if (e->duration_sec < 1) e->duration_sec = 0;
            }
            break;
    }
}

void MediaPlayerApp::ClearPlaylist() {
    playlist_count = 0;
    current_track = -1;
    Stop();
}

//  playback control

void MediaPlayerApp::Play() {
    if (playlist_count == 0) return;
    if (current_track < 0) current_track = 0;

    if (paused) {
        MpAudio::Pause(false);
        paused = false;
        playing = true;
        g_prog_t0 = 0;   // the progress clock restarts from the resume (satoru)
        return;
    }

    PlaylistEntry* entry = (current_track >= 0 && current_track < playlist_count)
                           ? &playlist[current_track] : nullptr;

    if (entry) {
        // read audio data from kvfs - use heap for large buffer
        int read_size = entry->file_size;
        if (read_size <= 0) read_size = 131072; // 128kb default
        if (read_size > 524288) read_size = 524288; // 512kb max
        uint8_t* audio_buf = (uint8_t*)kui::Alloc(read_size);
        if (!audio_buf) read_size = 0;
        int rd = 0;
        if (audio_buf) {
            rd = kui::Fs::ReadFile(entry->path, (char*)audio_buf, read_size);
        }

        if (rd > 0) {
            // use codec registry for format detection and decoding
            CodecType fmt = CodecRegistry::Detect(audio_buf, rd);
            if (fmt == CODEC_UNKNOWN)
                fmt = CodecRegistry::DetectByExtension(entry->path);

            bool audio_started = false;

            if (fmt == CODEC_WAV || fmt == CODEC_MP3 || fmt == CODEC_AAC ||
                fmt == CODEC_FLAC) {
                // unified decode path - codec registry handles format dispatch
                AudioBuffer abuf = CodecRegistry::DecodeAudio(audio_buf, rd);
                if (abuf.valid && abuf.samples && abuf.length > 0) {
                    audio_started = PlayDecodedAudio(
                        (const uint8_t*)abuf.samples, abuf.length,
                        abuf.sample_rate, abuf.bits, abuf.channels);
                }
            }
            else if (fmt == CODEC_MP4) {
                // mp4 container - extract and decode audio track
                AudioBuffer abuf = CodecRegistry::ExtractMP4Audio(audio_buf, rd);
                if (abuf.valid && abuf.samples && abuf.length > 0) {
                    audio_started = PlayDecodedAudio(
                        (const uint8_t*)abuf.samples, abuf.length,
                        abuf.sample_rate, abuf.bits, abuf.channels);
                }
            }
            else if (rd > 0) {
                // unknown format - try raw pcm at 22050hz 8-bit mono
                audio_started = PlayDecodedAudio(audio_buf, rd, 22050, 8, 1);
            }

            (void)audio_started;
        }
        if (audio_buf) kui::Free(audio_buf);
    }

    playing = true;
    paused = false;
    playback_progress = 0;
    g_prog_t0 = 0;

    // start the decoded-frame-rate window fresh for this track. (satoru)
    fps_frame_accum = 0;
    fps_display = 0;
    fps_window_start_ms = 0;

    if (entry) {
        for (int i = 0; i < playlist_count; i++) playlist[i].is_playing = false;
        entry->is_playing = true;
    }
}

static bool PlayDecodedAudio(const uint8_t* data, int len, int rate, int bits, int channels) {
    return MpAudio::PlayPCM(data, len, rate, bits, channels);
}

void MediaPlayerApp::Pause() {
    if (!playing) return;
    // the output is a kui::Audio stream, so pause is a real per-stream cork and resume picks up
    // where it stopped (the old path halted the backend dma and could not resume). (satoru)
    MpAudio::Pause(true);
    playing = false;
    paused = true;
}

void MediaPlayerApp::Stop() {
    // stop the active backend through the router instead of poking sb16/ac97
    // by hand; this halts hardware dma for whichever backend is live. (satoru)
    MpAudio::Stop();
    playing = false;
    paused = false;
    playback_progress = 0;
    for (int i = 0; i < playlist_count; i++) playlist[i].is_playing = false;
    cached_valid = false;  // invalidate cache on stop
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

void MediaPlayerApp::SetVolume(int vol) {
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    volume_pct = vol;
    // single master-volume knob on the mixer (0..100) replaces the old
    // per-backend fan-out; it scales every stream before the active backend
    // and is what audioserver::getstatus() reports. (satoru)
    MpAudio::SetMasterVolume(vol);
}

int MediaPlayerApp::GetVolume() {
    return volume_pct;
}

bool MediaPlayerApp::NowPlayingActive() {
    return current_track >= 0 && current_track < playlist_count;
}

const char* MediaPlayerApp::NowPlayingTitle() {
    if (!NowPlayingActive()) return "";
    return playlist[current_track].name;
}

int MediaPlayerApp::NowPlayingProgress() {
    if (!NowPlayingActive()) return 0;
    int p = playback_progress;
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return p;
}

bool MediaPlayerApp::IsPlayingNow() { return playing; }
bool MediaPlayerApp::IsPausedNow() { return paused; }

void MediaPlayerApp::TogglePlayback() {
    if (!NowPlayingActive()) return;
    if (playing) Pause();
    else          Play();
}

//  rendering

void MediaPlayerApp::CacheVideoInfo(int track_idx) {
    if (track_idx < 0 || track_idx >= playlist_count) {
        cached_valid = false;
        cached_track = -1;
        return;
    }
    if (cached_track == track_idx && cached_valid) return; // already cached

    PlaylistEntry* e = &playlist[track_idx];
    cached_track = track_idx;
    cached_vwidth = e->video_width;
    cached_vheight = e->video_height;
    cached_vcodec = e->video_codec;
    cached_has_video = e->has_video;
    cached_has_audio = e->has_audio;
    cached_valid = true;
    video_anim_frame = 0;

    // load some raw data for video visualization
    video_frame_len = kui::Fs::ReadFile(e->path, (char*)video_frame_buf, (int)sizeof(video_frame_buf));
    if (video_frame_len < 0) video_frame_len = 0;
}

void MediaPlayerApp::RenderVideoPreview(kui::Canvas& c) {
    int cx = 0;
    int cy = 0;
    int cw = c.Width();
    int preview_h = 180;

    // dark cinema background
    c.FillRect(cx, cy, cw, preview_h, 0xFF0A0A15);

    // cache video info for current track (only reads file once per track switch)
    CacheVideoInfo(current_track);

    if (current_track >= 0 && current_track < playlist_count &&
        playlist[current_track].type == MEDIA_VIDEO) {

        PlaylistEntry* e = &playlist[current_track];
        int border = 8;
        int vx = cx + border, vy = cy + border;
        int vw = cw - border*2, vh = preview_h - border*2;

        // cinema-style video viewport - dark letterbox with metadata
        c.FillRect(vx, vy, vw, vh, 0xFF0E0E1C);

        // subtle gradient-style cinema bars at top & bottom
        for (int i = 0; i < 12; i++) {
            uint8_t a = (uint8_t)(200 - i * 15);
            uint32_t bar_col = ((uint32_t)a << 24) | 0x000A0A15;
            c.FillRect(vx, vy + i, vw, 1, bar_col);
            c.FillRect(vx, vy + vh - 1 - i, vw, 1, bar_col);
        }

        // track name - large, centered
        const char* name = e->name;
        int nlen = mp_slen(name);
        int name_x = vx + vw/2 - (nlen * 8) / 2;
        int name_y = vy + 20;
        // shadow
        mp_text(c, name_x + 1, name_y + 1, name, 0xFF000000);
        mp_text(c, name_x, name_y, name, 0xFFE8E8FF);

        // codec info line: "h.264 1920x1080" or "video"
        const char* vcodec_name = "Video";
        if (cached_vcodec == 1) vcodec_name = "H.264";
        else if (cached_vcodec == 2) vcodec_name = "H.265";

        char info_line[64] = "";
        mp_scpy(info_line, vcodec_name, 64);
        if (cached_vwidth > 0) {
            int il = mp_slen(info_line);
            info_line[il] = ' ';
            mp_int_str(cached_vwidth, info_line + il + 1, 8);
            int il2 = mp_slen(info_line);
            info_line[il2] = 'x';
            mp_int_str(cached_vheight, info_line + il2 + 1, 8);
        }
        int info_len = mp_slen(info_line);
        mp_text(c, vx + vw/2 - info_len*4, vy + 40, info_line, MP_ACCENT);

        // audio indicator
        if (cached_has_audio) {
            mp_text(c, vx + vw/2 - 20, vy + 56, "Audio", MP_GREEN);
        }

        if (playing) {
            video_anim_frame++;

            // real decoded-frame-rate counter: count frames produced for the
            // viewport and roll the tally over once per real second. (satoru)
            uint32_t fnow = kui::NowMs();
            if (fps_window_start_ms == 0) fps_window_start_ms = fnow;
            fps_frame_accum++;
            if (fnow - fps_window_start_ms >= 1000) {
                fps_display = fps_frame_accum;
                fps_frame_accum = 0;
                fps_window_start_ms = fnow;
            }

            // animated equalizer bars in center - represents audio playing
            int eq_cx = vx + vw/2;
            int eq_by = vy + vh - 20;
            for (int i = 0; i < 15; i++) {
                int bar_h = 6 + ((video_anim_frame * 3 + i * 17) % 50);
                int bx = eq_cx - 75 + i * 10;
                uint32_t col;
                // color gradient: blue to cyan to green
                if (i < 5) col = MP_ACCENT;
                else if (i < 10) col = 0xFF1ABC9C;
                else col = MP_GREEN;
                c.FillRect(bx, eq_by - bar_h, 7, bar_h, col);
            }

            // "now playing" badge
            const char* np = "Now Playing";
            int np_w = mp_slen(np) * 8;
            int np_x = vx + vw/2 - np_w/2;
            int np_y = vy + vh - 24 - 52;
            c.FillRoundedRect(np_x - 8, np_y - 2, np_w + 16, 16, 4, 0xAA000000);
            // blinking dot
            uint32_t now_ms = kui::NowMs();
            if ((now_ms / 600) % 2 == 0) {
                c.FillCircle(np_x - 2, np_y + 6, 3, MP_RED);
            }
            mp_text(c, np_x + 6, np_y + 1, np, 0xFFCCCCDD);

            // decoded-frame-rate overlay - top-left of the viewport. (satoru)
            char fps_str[16] = "";
            mp_int_str(fps_display, fps_str, 12);
            int fl = mp_slen(fps_str);
            fps_str[fl] = ' '; fps_str[fl+1] = 'F'; fps_str[fl+2] = 'P';
            fps_str[fl+3] = 'S'; fps_str[fl+4] = 0;
            int fps_w = mp_slen(fps_str) * 8;
            c.FillRoundedRect(vx + 6, vy + 6, fps_w + 10, 16, 4, 0xAA000000);
            mp_text(c, vx + 11, vy + 8, fps_str, MP_GREEN);

        } else {
            // playback not running - drop the fps tally so it restarts clean
            // on the next play. (satoru)
            fps_frame_accum = 0;
            fps_display = 0;
            fps_window_start_ms = 0;

            // stopped/paused - show play button overlay at center
            int pcx = vx + vw/2, pcy = vy + vh/2 + 10;
            c.FillCircle(pcx, pcy, 24, 0x66000000);
            c.DrawCircle(pcx, pcy, 24, 0xAAFFFFFF);
            // play triangle
            for (int r = 0; r < 20; r++) {
                int ww = (r < 10) ? r : (20 - r);
                c.FillRect(pcx - 5, pcy - 10 + r, ww, 1, 0xFFFFFFFF);
            }
            // "press play" text
            mp_text(c, vx + vw/2 - 36, pcy + 30, "Press Play", MP_TEXT_DIM);
        }

        // file size badge at bottom-right
        if (e->file_size > 0) {
            char sz_str[24] = "";
            if (e->file_size >= 1048576) {
                mp_int_str(e->file_size / 1048576, sz_str, 12);
                int sl = mp_slen(sz_str);
                sz_str[sl] = ' '; sz_str[sl+1] = 'M'; sz_str[sl+2] = 'B'; sz_str[sl+3] = 0;
            } else if (e->file_size >= 1024) {
                mp_int_str(e->file_size / 1024, sz_str, 12);
                int sl = mp_slen(sz_str);
                sz_str[sl] = ' '; sz_str[sl+1] = 'K'; sz_str[sl+2] = 'B'; sz_str[sl+3] = 0;
            } else {
                mp_int_str(e->file_size, sz_str, 12);
                int sl = mp_slen(sz_str);
                sz_str[sl] = ' '; sz_str[sl+1] = 'B'; sz_str[sl+2] = 0;
            }
            int szw = mp_slen(sz_str) * 8;
            c.FillRect(vx + vw - szw - 12, vy + vh - 16, szw + 8, 14, 0xAA000000);
            mp_text(c, vx + vw - szw - 8, vy + vh - 14, sz_str, MP_TEXT_DIM);
        }

    } else if (current_track >= 0 && current_track < playlist_count &&
               playlist[current_track].type == MEDIA_AUDIO) {
        // audio file - show waveform visualization
        c.FillRect(cx, cy, cw, preview_h, 0xFF0A0A18);

        // show track name
        const char* name = playlist[current_track].name;
        int nw = mp_slen(name) * 8;
        mp_text(c, cx + cw/2 - nw/2, cy + 20, name, MP_ACCENT);

        // music note icon
        int note_cx = cx + cw/2;
        int note_cy = cy + preview_h/2 - 5;
        c.FillCircle(note_cx - 8, note_cy + 12, 6, MP_ACCENT);
        c.FillCircle(note_cx + 12, note_cy + 8, 6, MP_ACCENT);
        c.FillRect(note_cx - 2, note_cy - 20, 3, 32, MP_ACCENT);
        c.FillRect(note_cx + 18, note_cy - 24, 3, 32, MP_ACCENT);
        c.FillRect(note_cx - 2, note_cy - 20, 23, 3, MP_ACCENT);

        // animated equalizer bars when playing
        if (playing) {
            static int eq_frame = 0;
            eq_frame++;
            for (int i = 0; i < 11; i++) {
                int bar_h = 8 + ((eq_frame * 3 + i * 13) % 40);
                int bx = cx + cw/2 - 50 + i * 10;
                int by = cy + preview_h - 10;
                uint32_t col = (i % 3 == 0) ? MP_ACCENT : MP_ACCENT_DIM;
                c.FillRect(bx, by - bar_h, 7, bar_h, col);
            }
        }

        // show playback status - check all audio backends
        const char* status = MpAudio::StatusText();
        mp_text(c, cx + cw/2 - mp_slen(status)*4, cy + preview_h - 28, status, MP_TEXT_DIM);
    } else {
        // no media selected
        mp_text(c, cx + cw/2 - 48, cy + preview_h/2 - 8, "No Media", MP_TEXT_DIM);
        mp_text(c, cx + cw/2 - 80, cy + preview_h/2 + 10, "Open from playlist", MP_TEXT_DIM);
    }
}

void MediaPlayerApp::RenderProgressBar(kui::Canvas& c) {
    int cx = 0;
    int cy = 0;
    int cw = c.Width();
    int bar_y = cy + 186;

    // progress bar track
    c.FillRect(cx + 10, bar_y, cw - 20, 4, MP_PROGRESS_BG);

    // filled portion
    int filled = ((cw - 20) * playback_progress) / 100;
    if (filled > 0) {
        c.FillRect(cx + 10, bar_y, filled, 4, MP_ACCENT);
    }

    // knob
    int knob_x = cx + 10 + filled;
    c.FillCircle(knob_x, bar_y + 2, 5, 0xFFFFFFFF);
    c.DrawCircle(knob_x, bar_y + 2, 5, MP_ACCENT);

    // time labels
    if (current_track >= 0 && current_track < playlist_count) {
        int dur = playlist[current_track].duration_sec;
        char cur_str[16], dur_str[16];

        if (dur > 0) {
            int cur = (dur * playback_progress) / 100;
            // format current time mm:ss
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

            // format duration time mm:ss
            mp_int_str(dur / 60, m, 4);
            mp_scpy(dur_str, m, 16);
            cl = mp_slen(dur_str);
            dur_str[cl] = ':'; dur_str[cl+1] = 0;
            if (dur % 60 < 10) { int l2=mp_slen(dur_str); dur_str[l2]='0'; dur_str[l2+1]=0; }
            mp_int_str(dur%60, s2, 4);
            l3 = mp_slen(dur_str);
            for(int i=0; s2[i] && l3<15; i++) dur_str[l3++] = s2[i];
            dur_str[l3] = 0;
        } else {
            // unknown duration - show dashes
            mp_scpy(cur_str, "0:00", 16);
            mp_scpy(dur_str, "--:--", 16);
        }

        mp_text(c, cx + 10, bar_y + 8, cur_str, MP_TEXT_DIM);
        int dw = mp_slen(dur_str) * 8;
        mp_text(c, cx + cw - 10 - dw, bar_y + 8, dur_str, MP_TEXT_DIM);
    }
}

void MediaPlayerApp::RenderControls(kui::Canvas& c) {
    int cx = 0;
    int cy = 0;
    int cw = c.Width();
    int ctrl_y = cy + 208;

    // control buttons centered
    int btn_w = 36, btn_h = 36;
    int total_w = btn_w * 4 + 12 * 3;
    int bx = cx + cw/2 - total_w/2;

    // previous
    c.FillRoundedRect(bx, ctrl_y, btn_w, btn_h, 6, MP_BG_LIGHT);
    // << arrows
    mp_text(c, bx + 8, ctrl_y + 10, "<<", MP_TEXT);
    bx += btn_w + 12;

    // play/pause
    uint32_t play_col = playing ? MP_GREEN : MP_ACCENT;
    c.FillRoundedRect(bx, ctrl_y, btn_w, btn_h, 6, play_col);
    if (playing) {
        // pause bars
        c.FillRect(bx + 12, ctrl_y + 10, 4, 16, 0xFFFFFFFF);
        c.FillRect(bx + 20, ctrl_y + 10, 4, 16, 0xFFFFFFFF);
    } else {
        // play triangle
        for (int r = 0; r < 16; r++) {
            int ww = (r < 8) ? r : (16 - r);
            c.FillRect(bx + 12, ctrl_y + 10 + r, ww, 1, 0xFFFFFFFF);
        }
    }
    bx += btn_w + 12;

    // stop
    c.FillRoundedRect(bx, ctrl_y, btn_w, btn_h, 6, MP_BG_LIGHT);
    c.FillRect(bx + 11, ctrl_y + 11, 14, 14, MP_RED);
    bx += btn_w + 12;

    // next
    c.FillRoundedRect(bx, ctrl_y, btn_w, btn_h, 6, MP_BG_LIGHT);
    mp_text(c, bx + 8, ctrl_y + 10, ">>", MP_TEXT);
}

void MediaPlayerApp::RenderPlaylist(kui::Canvas& c) {
    int cx = 0;
    int cy = 0;
    int cw = c.Width();
    int ch = c.Height();
    int list_y = cy + 300;   // pushed down to make room for codec/volume rows
    int list_h = ch - 300;

    if (list_h <= 0) return;

    // playlist header
    c.FillRect(cx, list_y, cw, 20, MP_BG_LIGHT);
    mp_text(c, cx + 8, list_y + 4, "Playlist", MP_TEXT);

    char count_str[16];
    mp_int_str(playlist_count, count_str, 16);
    int csw = mp_slen(count_str) * 8;
    mp_text(c, cx + cw - csw - 8, list_y + 4, count_str, MP_TEXT_DIM);

    // separator
    c.FillRect(cx, list_y + 20, cw, 1, 0xFF333355);

    // playlist items
    int iy = list_y + 22;
    for (int i = scroll_offset; i < playlist_count && iy < cy + ch - 4; i++) {
        PlaylistEntry* e = &playlist[i];
        bool selected = (i == current_track);

        // background
        uint32_t bg = selected ? 0xFF252540 : ((i % 2) ? MP_BG : 0xFF151525);
        c.FillRect(cx + 2, iy, cw - 4, 24, bg);

        // playing indicator
        if (e->is_playing) {
            c.FillCircle(cx + 10, iy + 12, 4, MP_GREEN);
        }

        // codec tag
        const char* ctag = "??";
        uint32_t ctag_col = MP_TEXT_DIM;
        switch(e->codec) {
            case CODEC_WAV:  ctag = "WAV"; ctag_col = 0xFFAAAAAA; break;
            case CODEC_MP3:  ctag = "MP3"; ctag_col = 0xFF7F8CFF; break;
            case CODEC_AAC:  ctag = "AAC"; ctag_col = 0xFF2ECC71; break;
            case CODEC_FLAC: ctag = "FLC"; ctag_col = 0xFFE74C3C; break;
            case CODEC_MP4:  ctag = "MP4"; ctag_col = 0xFFE67E22; break;
            default:
                ctag = (e->type == MEDIA_VIDEO) ? "VID" : " ? ";
                ctag_col = MP_TEXT_DIM;
                break;
        }
        mp_text(c, cx + 18, iy + 5, ctag, ctag_col);

        // file name (truncated to fit)
        char display_name[36];
        mp_scpy(display_name, e->name, 35);
        mp_text(c, cx + 46, iy + 5, display_name, selected ? MP_TEXT : MP_TEXT_DIM);

        // duration
        char dur[8];
        if (e->duration_sec > 0) {
            mp_int_str(e->duration_sec / 60, dur, 8);
            int dl = mp_slen(dur);
            dur[dl] = ':'; dur[dl+1] = 0;
            if (e->duration_sec % 60 < 10) { dur[mp_slen(dur)] = '0'; dur[mp_slen(dur)+1] = 0; }
            char sec[4]; mp_int_str(e->duration_sec % 60, sec, 4);
            dl = mp_slen(dur);
            for(int j=0; sec[j] && dl<7; j++) dur[dl++] = sec[j];
            dur[dl] = 0;
        } else {
            mp_scpy(dur, "--:--", 8);
        }
        int dw = mp_slen(dur) * 8;
        mp_text(c, cx + cw - dw - 8, iy + 5, dur, MP_TEXT_DIM);

        iy += 26;
    }
}

void MediaPlayerApp::RenderCodecInfo(kui::Canvas& c) {
    int cx = 0;
    int cy = 0;
    int cw = c.Width();
    int info_y = cy + 250;

    if (current_track < 0 || current_track >= playlist_count) return;
    PlaylistEntry* e = &playlist[current_track];

    // codec name
    const char* codec_name = "Unknown";
    const CodecInfo* ci = CodecRegistry::GetCodecInfo(e->codec);
    if (ci && ci->available) {
        codec_name = ci->name;
    } else {
        switch(e->codec) {
            case CODEC_WAV:  codec_name = "WAV PCM";   break;
            case CODEC_MP3:  codec_name = "MP3";       break;
            case CODEC_AAC:  codec_name = "AAC-LC";    break;
            case CODEC_FLAC: codec_name = "FLAC";      break;
            case CODEC_MP4:  codec_name = "MP4/H.264"; break;
            default:         codec_name = "PCM Raw";   break;
        }
    }

    // codec badge
    uint32_t badge_col;
    switch(e->codec) {
        case CODEC_MP3:  badge_col = 0xFF7F8CFF; break;
        case CODEC_AAC:  badge_col = 0xFF2ECC71; break;
        case CODEC_FLAC: badge_col = 0xFFE74C3C; break;
        case CODEC_MP4:  badge_col = 0xFFE67E22; break;
        case CODEC_WAV:  badge_col = 0xFF95A5A6; break;
        default:         badge_col = 0xFF555555; break;
    }

    c.FillRoundedRect(cx + 10, info_y, 60, 16, 4, badge_col);
    int cnw = mp_slen(codec_name) * 8;
    mp_text(c, cx + 10 + (60 - cnw)/2, info_y + 2, codec_name, 0xFF000000);

    // active output backend indicator - reflects the unified AudioServer's
    // chosen backend (hda/ac97/sb16/pcspk) rather than the legacy per-driver
    // probes, which the player no longer routes through. (satoru)
    const char* backend = MpAudio::BackendName();

    bool have_out = (backend[0] != 'N');  // "No Audio" begins with 'N'
    uint32_t bk_col = (playing && have_out) ? MP_GREEN : MP_TEXT_DIM;
    char bk_label[16] = "Out: ";
    int bl = mp_slen(bk_label);
    for(int i=0; backend[i]&&bl<15; i++) bk_label[bl++]=backend[i];
    bk_label[bl]=0;
    mp_text(c, cx + 80, info_y + 2, bk_label, bk_col);

    // sample rate from audioinfo if playing sb16
    if (MpAudio::SampleRateHz() > 0) {
        char freq_str[24] = "";
        mp_int_str(MpAudio::SampleRateHz(), freq_str, 12);
        int fl = mp_slen(freq_str);
        freq_str[fl]='H'; freq_str[fl+1]='z'; freq_str[fl+2]=0;
        mp_text(c, cx + cw - 60, info_y + 2, freq_str, MP_TEXT_DIM);
    }
}

void MediaPlayerApp::RenderVolumeBar(kui::Canvas& c) {
    int cx = 0;
    int cy = 0;
    int cw = c.Width();
    int vol_y = cy + 272;  // just below codec info row

    // speaker icon
    mp_text(c, cx + 10, vol_y + 2, "Vol", MP_TEXT_DIM);

    // volume track
    int bar_x = cx + 36;
    int bar_w = cw - 46;
    c.FillRect(bar_x, vol_y + 6, bar_w, 4, MP_PROGRESS_BG);

    // filled
    int filled = (bar_w * volume_pct) / 100;
    uint32_t vol_col = (volume_pct > 80) ? 0xFF2ECC71 :
                       (volume_pct > 40) ? MP_ACCENT   : MP_ACCENT_DIM;
    if (filled > 0)
        c.FillRect(bar_x, vol_y + 6, filled, 4, vol_col);

    // knob
    int knob_x = bar_x + filled;
    c.FillCircle(knob_x, vol_y + 8, 4, 0xFFFFFFFF);
    c.DrawCircle(knob_x, vol_y + 8, 4, vol_col);

    // percentage label
    char pct_str[8];
    mp_int_str(volume_pct, pct_str, 8);
    int pl = mp_slen(pct_str);
    pct_str[pl]='%'; pct_str[pl+1]=0;
    mp_text(c, bar_x + bar_w + 4, vol_y + 2, pct_str, MP_TEXT_DIM);
}

void MediaPlayerApp::RenderPlayerUI(kui::Canvas& c) {
    int cx = 0;
    int cy = 0;
    int cw = c.Width();
    int ch = c.Height();

    // background
    c.FillRect(cx, cy, cw, ch, MP_BG);

    RenderVideoPreview(c);
    RenderProgressBar(c);
    RenderControls(c);
    RenderCodecInfo(c);
    RenderVolumeBar(c);
    RenderPlaylist(c);
}

void MediaPlayerApp::OnFrame(kui::Window& w, kui::Canvas& c, uint32_t now_ms) {
    RenderPlayerUI(c);

    // advance progress if playing
    if (playing) {
        // check real audio state - if audio hardware finished, update accordingly
        if (MpAudio::Drained() && playback_progress > 0) {
            // audio finished playing - if duration known, auto-advance
            int dur = 0;
            if (current_track >= 0 && current_track < playlist_count)
                dur = playlist[current_track].duration_sec;
            if (dur > 0) {
                // use real audio info to estimate progress
                playback_progress = 100;
                Next();
            }
        }

        // clock-based progress fallback (for when we don't have exact audio position): one percent
        // per duration/100 seconds. it used to count rendered frames at an assumed 60 per second,
        // frames now only come when asked for, so it counts real milliseconds instead. (satoru)
        int dur = 60; // 1 min default
        if (current_track >= 0 && current_track < playlist_count) {
            dur = playlist[current_track].duration_sec;
            if (dur < 1) dur = 60;
        }
        uint32_t step_ms = (uint32_t)dur * 10;
        if (g_prog_t0 == 0) g_prog_t0 = now_ms;
        int guard = 0;
        while (playing && now_ms - g_prog_t0 >= step_ms && guard++ < 100) {
            g_prog_t0 += step_ms;
            playback_progress++;
            if (playback_progress > 100) {
                playback_progress = 0;
                Next();
                break;   // Next restarts the clock: do not fast-forward the new track in this pass (satoru)
            }
        }
        // the equalizer bars and the progress bar move while playing: keep frames coming (satoru)
        w.RequestAnimationFrame();
    }
}

bool MediaPlayerApp::OnEvent(kui::Window& w, const kui::Event& e) {
    int cw = w.ContentSize().w;

    switch (e.type) {
    case kui::EventType::Click: {
        // mouse click: a=local_x, b=local_y (relative to content area)
        int a = e.x, b = e.y;

        // progress bar click (local y ~= 186, height ~= 10px) - also begins a
        // drag-scrub; pointer-move updates the seek until release. (satoru)
        if (b >= 180 && b <= 200) {
            int rel = a - 10;
            int bar_w = cw - 20;
            if (rel >= 0 && rel <= bar_w) {
                Seek((rel * 100) / bar_w);
                dragging_seek = true;
            }
            return true;
        }

        // control buttons (local y ~= 208, height 36)
        if (b >= 208 && b <= 244) {
            int btn_w = 36;
            int total_w = btn_w * 4 + 12 * 3;
            int bx = cw/2 - total_w/2;

            if (a >= bx && a < bx + btn_w) { Previous(); return true; }
            bx += btn_w + 12;
            if (a >= bx && a < bx + btn_w) {
                if (playing) Pause(); else Play();
                return true;
            }
            bx += btn_w + 12;
            if (a >= bx && a < bx + btn_w) { Stop(); return true; }
            bx += btn_w + 12;
            if (a >= bx && a < bx + btn_w) { Next(); return true; }
        }

        // playlist click (local y >= 322 to account for codec/volume rows)
        if (b >= 322) {
            int idx = scroll_offset + (b - 322) / 26;
            if (idx >= 0 && idx < playlist_count) {
                bool was_playing = playing;
                Stop();
                current_track = idx;
                if (was_playing) Play();
            }
            return true;
        }

        // volume bar click (local y ~= 272-286)
        if (b >= 268 && b <= 288) {
            int bar_x = 36;
            int bar_w = cw - 46;
            int rel = a - bar_x;
            if (rel >= 0 && rel <= bar_w) {
                SetVolume((rel * 100) / bar_w);
                dragging_vol = true;
            }
            return true;
        }
        return true;
    }

    case kui::EventType::KeyDown: {
        // keyboard: space plays and pauses, s / n / p stop, next, previous
        if (e.mods & (kui::M_CTRL | kui::M_ALT | kui::M_SUPER)) return false;
        int a = e.key;
        if (e.text[0]) a = (unsigned char)e.text[0];
        if (a == ' ') { if (playing) Pause(); else Play(); return true; }
        if (a == 's' || a == 'S') { Stop(); return true; }
        if (a == 'n' || a == 'N') { Next(); return true; }
        if (a == 'p' || a == 'P') { Previous(); return true; }
        return false;
    }

    case kui::EventType::Scroll: {
        // wheel: positive = content down (the old event 3 subtracted it from the offset)
        int a = (int)e.dy;
        if (a == 0 && e.dy > 0.0f) a = 1;
        if (a == 0 && e.dy < 0.0f) a = -1;
        scroll_offset -= a;
        if (scroll_offset < 0) scroll_offset = 0;
        if (scroll_offset >= playlist_count) scroll_offset = playlist_count - 1;
        if (scroll_offset < 0) scroll_offset = 0;
        return true;
    }

    case kui::EventType::PointerMove: {
        // pointer-move: drive drag-scrub / drag-volume while a drag is active. the wm keeps
        // delivering moves to this window after the press, and PointerUp ends the drag. (satoru)
        int a = e.x;
#ifndef KUI_BACKEND_WAYLAND
        // a very fast click can reach the window as release first, then Click: the drag flag would stick.
        // the button state settles it, as it did before the port. (satoru)
        if (!Mouse::IsLeftDown()) { dragging_seek = false; dragging_vol = false; return false; }
#endif
        if (dragging_seek) {
            int bar_w = cw - 20;
            int rel = a - 10;
            if (rel < 0) rel = 0;
            if (rel > bar_w) rel = bar_w;
            if (bar_w > 0) Seek((rel * 100) / bar_w);
            return true;
        }
        if (dragging_vol) {
            int bar_x = 36;
            int bar_w = cw - 46;
            int rel = a - bar_x;
            if (rel < 0) rel = 0;
            if (rel > bar_w) rel = bar_w;
            if (bar_w > 0) SetVolume((rel * 100) / bar_w);
            return true;
        }
        return false;
    }

    case kui::EventType::PointerUp:
        // a release ends any active drag-scrub or drag-volume. (satoru)
        dragging_seek = false;
        dragging_vol = false;
        return false;

    default:
        break;
    }
    return false;
}

// the window is gone (any way of closing it): silence the output and forget the window so the
// player can be opened again. a handler from an older window must not clear a newer one. (satoru)
void MediaPlayerApp::WindowGone(kui::Window* who) {
    if (g_win != who && g_win != nullptr) return;
    // always close the kui::Audio stream, even when the state flags say the
    // player already stopped: a stream left open behind a closed window is
    // what made the mixer keep playing with no way to stop it. (satoru)
    Stop();
    g_win = nullptr;
    win_id = -1;
    hover_button = -1;
    scroll_offset = 0;
    dragging_seek = false;
    dragging_vol = false;
    cached_track = -1;
    cached_valid = false;
    video_frame_len = 0;
    seek_position = -1;
    paused = false;
}

void MediaPlayerApp::LoadFile(const char* path) {
    if (!path) return;
    AddToPlaylist(path);
    current_track = playlist_count - 1;
}
