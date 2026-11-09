#pragma once
#include "../kernel/types.h"

// enhanced graphics driver with 180hz support
// integrates with displaycontroller for high refresh rate rendering
// supports double buffering, vsync, and advanced rendering features

class Graphics {
public:
    enum RenderMode {
        SINGLE_BUFFER,
        DOUBLE_BUFFER,
        TRIPLE_BUFFER
    };

    enum BlendMode {
        BLEND_NONE,
        BLEND_ALPHA,
        BLEND_ADDITIVE,
        BLEND_MULTIPLY
    };

    struct DrawStats {
        uint32_t frames_rendered;
        uint32_t frames_dropped;
        uint32_t avg_frame_time_us;
        uint32_t last_frame_time_us;
        uint32_t target_fps;
        uint32_t current_fps;
        bool vsync_enabled;
    };

    static void Init(uintptr_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp);
    static void ReinitForResolution(uintptr_t addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp);
    // resync the compositor to the live bga hardware geometry after a host
    // window resize; true if a resync happened (caller relayouts). (satoru)
    static bool SyncToHardwareGeometry();
    // multi-monitor (see outputs.h): turn the boot framebuffer into a virtual
    // desktop that spans every live output; false leaves the single-fb path alone.
    static bool EnableMultiOutput();
    static bool MultiActive();
    // resize the virtual desktop after Outputs::Layout / on-off changed. (satoru)
    static bool RelayoutOutputs();
    // true once after a runtime relayout, so the gui loop re-pitches its buffers
    // without treating it as a resolution change that would close every window.
    static bool TakeMultiRelayout();
    // ui scale in percent (100/125/150/200...). the compositor lays out and draws
    // at the LOGICAL size (physical * 100 / pct) so every ui element scales as one
    // and the present step resamples to the hardware. needs double buffering; true
    // when the scale is now in effect. (satoru)
    static bool SetUIScale(int pct);
    static int  GetUIScale();
    // settings asks for a new scale; the gui loop applies it at a safe point (top of
    // its frame) because it frees and re-sizes the back buffer. (satoru)
    static void RequestUIScale(int pct);
    static void ApplyPendingUIScale();
    // runtime output arrangement, deferred to the gui loop like the scale (it
    // re-allocates the virtual desktop). ops: 1 on(a) 2 off(a) 3 primary(a)
    // 4 layout horizontal 5 layout vertical 6 swap(a,b) 7 config (below). (satoru)
    static void RequestOutputOp(int op, int a, int b);
    // settings > displays: one op (7) for a whole staged configuration, all
    // indexed by the CURRENT slots: the new primary slot, per-slot on/off and
    // arranged position in physical px (primary at 0,0). (satoru)
    static void RequestOutputConfig(int primary, const bool* on, const int* ax, const int* ay);
    static void ApplyPendingOutputOp();
    // after an op: the old per-slot origins so windows can follow their screen.
    static bool TakeOutputReloc(int* old_x, int* old_y, int* swap_a, int* swap_b);
    static void InitAdvanced(); // initialize with display controller
    static void SetRenderMode(RenderMode mode);
    static RenderMode GetRenderMode();
    
    // frame management for high refresh rates
    static void BeginFrame();
    static void EndFrame();
    static void SetTargetFPS(uint32_t fps);
    static uint32_t GetTargetFPS();
    static uint32_t GetTargetFrameTimeMs();  // frame budget in ms (1000/hz) for adaptive gui pacing (satoru)
    static void SetVirtioPresent(bool on);   // DisplayManager enables this for the accelerated backend (satoru)
    static void PresentVirtioIfActive();     // gui loop calls after SwapBuffers; pushes the frame to the host gpu (satoru)
    // record the damage box of the frame just swapped so the virtio transfer can
    // upload only that region instead of the whole screen. set by SwapBuffers
    // before it clears the dirty list. (satoru)
    static void SetPresentDamage(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

    // force every present to transfer the WHOLE screen, as full-screen content
    // (video, games, animation) naturally does. the damage-aware path makes an
    // idle desktop's transfer tiny, which hides how expensive a real present
    // is - this reproduces the heavy case. (satoru)
    static void SetForceFullPresent(bool on);
    static uint32_t DetectRefreshRate();  // measure via vsync timing, fallback 60 hz
    static uint32_t GetMonitorHz();       // last detected monitor refresh rate
    static bool     RefreshWasMeasured(); // DetectRefreshRate timed real vsync (not the 60 default) (satoru)
    static bool IsFramebufferWC();        // true if fb is write-combining (bare-metal safe)
    static bool ShouldRender(); // frame pacing for 180hz
    static void WaitForVSync();
    
    // basic drawing
    static void Clear(uint32_t color);
    static void DrawPixel(int x, int y, uint32_t color);
    static uint32_t ReadPixel(int x, int y);
    static void BlendPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    static void FillRect(int x, int y, int w, int h, uint32_t color);
    static void FillRectRounded(int x, int y, int w, int h, int r, uint32_t color);
    static void FillRectAlpha(int x, int y, int w, int h, uint8_t a, uint32_t color);
    static void FillGradientV(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
    
    // advanced drawing
    static void DrawLine(int x0, int y0, int x1, int y1, uint32_t color, int thickness = 1);
    static void DrawCircle(int cx, int cy, int radius, uint32_t color, bool filled = false);
    static void DrawEllipse(int cx, int cy, int rx, int ry, uint32_t color, bool filled = false);
    static void DrawPolygon(const int* points, int count, uint32_t color, bool filled = false);
    
    // effect rendering
    static void BlurRect(int x, int y, int w, int h, int radius);
    static void ApplyGlow(int x, int y, int w, int h, uint32_t color, int intensity);
    static void ApplyShadow(int x, int y, int w, int h, int offset_x, int offset_y, uint8_t alpha);
    
    // texture/image operations
    static void DrawImage(const uint8_t* image_data, int src_x, int src_y, int src_w, int src_h,
                         int dst_x, int dst_y, int dst_w, int dst_h);
    static void DrawImageScaled(const uint8_t* image_data, int img_w, int img_h, 
                               int x, int y, int w, int h);
    static void DrawImageRotated(const uint8_t* image_data, int img_w, int img_h,
                                int x, int y, float angle);
    
    // buffer management
    static void SwapBuffers();
    struct Rect { int x, y, w, h; };
    // atomic present of an explicit damage list (max 32 rects, NULL/0 => full)
    static void Present(const Rect* rects, int count);
    static uint8_t* GetActiveBuffer();
    static uint8_t* GetBackBuffer();
    static void ClearBackBuffer(uint32_t color);
    static void CopyRegion(int src_x, int src_y, int dst_x, int dst_y, int w, int h);
    
    // performance optimization
    static void SetClipRect(int x, int y, int w, int h);
    static void ClearClipRect();
    // nesting clip stack: PushClipRect intersects with the current clip and saves
    // the prior state; PopClipRect restores it. use these instead of Set/Clear
    // whenever one clipped region renders inside another (e.g. a window's content
    // render that itself clips a scroll panel) - a bare ClearClipRect would disarm
    // the outer clip and let inner content bleed onto the desktop. (satoru)
    static void PushClipRect(int x, int y, int w, int h);
    static void PopClipRect();
    // the active clip, false when clipping is off (kui reads it so its own rasterizer stays inside) (satoru)
    static bool GetClip(int& x, int& y, int& w, int& h) { x = clip_x; y = clip_y; w = clip_w; h = clip_h; return clipping_enabled; }
    static void SetBlendMode(BlendMode mode);
    static BlendMode GetBlendMode();
    
    // dirty region tracking for optimized rendering
    static void MarkDirty(int x, int y, int w, int h);
    static void ClearDirtyRegions();
    static void RenderDirtyRegions();
    // active damage rects of the frame in progress. code that repaints cached
    // content (the desktop wallpaper) uses this to touch only the damaged
    // rows; 0 means nothing changed and the caller can skip entirely. (satoru)
    static int GetDirtyRects(Rect* out, int max);

    // ── wallpaper damage-only (kurono.wpdamage=1, default off) ─────────
    // the desktop asks whether it may repaint only the damaged rows of its
    // cached wallpaper. the crc self-check below clears this (and the desktop
    // repaints one full frame) if the scanned and compositor buffers ever
    // disagree. (satoru)
    static void SetWallpaperDamageOnly(bool on);
    static bool WallpaperDamageOnly();

    // ── present self-check ─────────────────────────────────────────────
    // for `frames` presents, compare a crc of the whole visible scanned buffer
    // against the compositor back buffer. a mismatch means the damage tracking
    // missed pixels (wallpaper damage-only) or the scanout alias mapped the
    // wrong memory (zero-copy); the feature is disabled and a log line names
    // the first differing offset. (satoru)
    enum SelfCheckKind { SELFCHECK_NONE = 0, SELFCHECK_WPDAMAGE = 1, SELFCHECK_ZEROCOPY = 2 };
    static void ArmSelfCheck(int kind, int frames);
    static int  SelfCheckFramesLeft();

    // ── zero-copy present (kurono.igpu.zerocopy=1, default off) ────────
    // render into a write-back ram buffer that the display engine scans
    // through a ggtt window, and publish damage with clflush instead of a
    // copy. multi output reuses the compositor virtual desktop; single output
    // allocates its own ram buffer (never the wc-mapped gop framebuffer, whose
    // uncached reads made blending unusable). refuses (returns false with a
    // reason) unless 32 bpp, ui scale 100, no virtio present and no a11y
    // filter. (satoru)
    static bool ZeroCopyEnter(char* why, int whylen);
    static void ZeroCopyLeave();
    static bool ZeroCopyActive();
    // bytes of the buffer zero-copy renders into (and the driver scans). (satoru)
    static size_t GetBufferBytes();
    // the driver registers these: suspend runs before the compositor frees or
    // reallocates its buffers (relayout/scale/mode change) and restores any
    // redirected scanout; present runs after a zero-copy frame's damage was
    // flushed, so the driver can arm a deferred plane flip on the first
    // complete frame. (satoru)
    typedef void (*ZeroCopyHook)();
    static void SetZeroCopyHooks(ZeroCopyHook suspend, ZeroCopyHook present);

    // qemu test hook (kurono.igpu.zctest=1 / =wc): run the zero-copy present
    // machinery without an intel display. mode 1 renders into a write-back ram
    // buffer and copies damage to the framebuffer (the safe design); mode 2
    // renders straight into the wc framebuffer to reproduce the old slowdown
    // (measurement only, never used by the production path). (satoru)
    static bool ZeroCopyTestStart(int mode, char* why, int whylen);
    static bool ZeroCopyTestMode();

    // ── Global UI dirty signal (damage-gated compositor) ───────────────
    // Distinct from MarkDirty(): MarkDirty feeds the partial-swap region
    // list *during* a render; this counter answers "does the GUI loop need
    // to composite a NEW frame at all?".  Any code that changes on-screen
    // state must call MarkUIDirty().  The GUI loop reads ConsumeUIDirty()
    // once per frame.  Counter (not bool) so a mark that races with a
    // consume is never silently lost. (satoru)
    static void     MarkUIDirty();
    static uint32_t UIDirtyCount();        // current value (no clear)
    static bool     ConsumeUIDirty();      // true if dirty since last call; clears
    static void*    LastUIDirtyCaller();   // return address of the latest MarkUIDirty
    // ask for one frame at real time `ms` (e.g. a cursor blink edge) without
    // marking the ui dirty every frame until then. the gui loop polls it.
    static void     RequestFrameAt(uint32_t ms);
    static bool     ConsumeFrameDue(uint32_t now_ms);

    // information
    static int GetWidth();
    static int GetHeight();
    static uint32_t GetPitch();
    static uint8_t GetBpp();
    static uint8_t* GetBuffer();
    static void SetBuffer(uint8_t* addr);
    static const DrawStats& GetDrawStats();
    
    // color utilities
    static uint32_t RGB(uint8_t r, uint8_t g, uint8_t b);
    static uint32_t RGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    static void ColorToRGB(uint32_t color, uint8_t& r, uint8_t& g, uint8_t& b);
    static uint32_t BlendColors(uint32_t src, uint32_t dst, uint8_t alpha);
    static uint32_t InterpolateColor(uint32_t c1, uint32_t c2, float t);

    static inline void FillRoundedRect(int x, int y, int w, int h, int r, uint32_t c) {
        FillRectRounded(x, y, w, h, r, c);
    }
    static inline void FillCircle(int cx, int cy, int r, uint32_t c) {
        DrawCircle(cx, cy, r, c, true);
    }
    static inline void DrawRect(int x, int y, int w, int h, uint32_t c) {
        DrawLine(x, y, x+w-1, y, c);
        DrawLine(x, y+h-1, x+w-1, y+h-1, c);
        DrawLine(x, y, x, y+h-1, c);
        DrawLine(x+w-1, y, x+w-1, y+h-1, c);
    }

    // text rendering - delegates to fontttf w/ bitmap fallback
    static void DrawString(int x, int y, const char* s, uint32_t fg, uint32_t bg);

    // accessibility post-processing applied just before SwapBuffers copies
    // the back buffer to the framebuffer.  Modes:
    //   0 = off, 1 = protanopia, 2 = deuteranopia, 3 = tritanopia, 4 = grayscale
    static void SetColorFilter(int mode);
    static int  GetColorFilter();
    static void SetHighContrast(bool on);
    static void SetBrightness(int pct);   // software dim 10..100 (%) (satoru)
    static int  GetBrightness();
    // per-output software dim, indexed like Outputs (0 = primary). the single
    // output path uses slot 0; multi output dims each panel on its own. (satoru)
    static void SetOutputBrightness(int output, int pct);
    static int  GetOutputBrightness(int output);
    // warm night-light tint 0..100 applied on the way to the panel. (satoru)
    static void SetNightLight(int pct);
    static int  GetNightLight();

private:
    static uint8_t* fb_addr;
    static uint8_t* back_buffer;
    static uint8_t* active_buffer;
    static size_t   back_buffer_size;   // track for pmm freeing
    static uint32_t fb_width;
    static uint32_t fb_height;
    static uint32_t fb_pitch;
    static uint8_t fb_bpp;
    
    static RenderMode render_mode;
    static BlendMode blend_mode;
    
    // frame timing - 64-bit μs to avoid wrap (uint32 wraps every ~71 min)
    static uint32_t target_frame_time_us;
    static uint64_t last_frame_time;
    static uint32_t frame_count;
    static uint64_t fps_sample_time;
    static DrawStats draw_stats;
    
    // bare-metal state
    static bool fb_wc_active;     // true if fb pages are write-combining
    static uint32_t monitor_hz;   // last detected monitor hz (0 = unknown)
    
    // clipping
    static int clip_x;
    static int clip_y;
    static int clip_w;
    static int clip_h;
    static bool clipping_enabled;
    // saved clip states for PushClipRect/PopClipRect (fixed depth - render nesting
    // is shallow: desktop -> window -> content -> scroll panel). (satoru)
    struct ClipSave { int x, y, w, h; bool enabled; };
    static ClipSave clip_stack[16];
    static int clip_sp;
    
    // dirty regions for optimized rendering
    struct DirtyRegion {
        int x, y, w, h;
        bool active;
    };
    // larger cap = fewer forced coalesces of unrelated small rects (satoru)
    static const int DIRTY_REGION_CAP = 32;
    static DirtyRegion dirty_regions[DIRTY_REGION_CAP];
    static int dirty_count;
    // previous frame's damage, kept so a damage-only wallpaper pass can repaint
    // under things that disappeared after they were last drawn (closing menus,
    // fading toasts, animation frames). filled by ClearDirtyRegions. (satoru)
    static DirtyRegion dirty_prev[DIRTY_REGION_CAP];
    static int dirty_prev_count;
    // once total damage crosses a sane fraction of the screen we stop tracking
    // individual rects and present the whole frame; reset by ClearDirtyRegions (satoru)
    static bool full_screen_dirty;
    // running sum of damaged pixels this frame (64-bit, overflow-safe) (satoru)
    static uint64_t dirty_area_total;
    static void MarkFullScreenDirty();
    
    // internal helpers
    static void SwapBuffersBody();          // cpu present path, wrapped by SwapBuffers (satoru)
    static void SelfCheckWpTick();          // scanned vs back crc after a cpu present (satoru)
    static void ZcFlushRects(const Rect* rects, int count);   // clflush damage (satoru)
    static void ZcMirrorRects(const Rect* rects, int count);  // keep the check reference in sync (satoru)
    static void ZcCopyRects(const Rect* rects, int count);    // qemu test present (satoru)
    static bool ZcSelfCheckTick();          // scanned vs back crc on a zero-copy frame (satoru)
    static void ZcSuspendIfActive();        // leave before a buffer/geometry change (satoru)
    static bool IsPointInBounds(int x, int y);
    static void ClipRect(int& x, int& y, int& w, int& h);
    static void DrawPixelUnsafe(int x, int y, uint32_t color);
    static void DrawPixelBlended(int x, int y, uint32_t color, uint8_t alpha);
    static uint32_t GetPixelUnsafe(int x, int y);
    
    // line drawing algorithms
    static void DrawLineBresenham(int x0, int y0, int x1, int y1, uint32_t color);
    static void DrawThickLine(int x0, int y0, int x1, int y1, uint32_t color, int thickness);
    
    // circle/ellipse helpers
    static void PlotCirclePoints(int cx, int cy, int x, int y, uint32_t color, bool filled);
    static void PlotEllipsePoints(int cx, int cy, int x, int y, uint32_t color, bool filled);
};
