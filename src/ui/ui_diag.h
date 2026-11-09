#pragma once
// per-second pointer/compositor diagnostics behind kurono.uidiag=1. header
// only so the drivers, the window manager and the gui loop can note events
// without a makefile change. counters are relaxed atomics: an input process,
// the gui process and a syscall ui pump may note from different cpus. (satoru)
#include "../kernel/types.h"
#include "../drivers/timer.h"
#include "../drivers/serial.h"

namespace UiDiag {

inline bool     g_enabled = false;
inline uint32_t g_motion = 0;          // pointer position changes applied
inline uint32_t g_cursor_damage = 0;   // cursor rects added to the damage list
inline uint32_t g_cursor_draw = 0;     // software cursor draws
inline uint32_t g_win_moves = 0;       // drag/resize geometry changes applied
inline uint32_t g_frames = 0;          // presented frames
inline uint32_t g_pending_motion_ms = 0;
inline uint32_t g_evt2pres_max_ms = 0;
inline uint32_t g_window_ms = 0;

inline void Enable(bool on) {
    g_enabled = on;
    g_window_ms = Timer::GetRealMs();
    g_motion = g_cursor_damage = g_cursor_draw = g_win_moves = g_frames = 0;
    g_pending_motion_ms = 0;
    g_evt2pres_max_ms = 0;
}

inline bool Enabled() { return g_enabled; }

inline void NoteMotion() {
    if (!g_enabled) return;
    __atomic_add_fetch(&g_motion, 1u, __ATOMIC_RELAXED);
    uint32_t now = Timer::GetRealMs();
    if (!g_pending_motion_ms) g_pending_motion_ms = now ? now : 1;
}

inline void NoteCursorDamage() {
    if (!g_enabled) return;
    __atomic_add_fetch(&g_cursor_damage, 1u, __ATOMIC_RELAXED);
}

inline void NoteCursorDraw() {
    if (!g_enabled) return;
    __atomic_add_fetch(&g_cursor_draw, 1u, __ATOMIC_RELAXED);
}

inline void NoteWindowMove() {
    if (!g_enabled) return;
    __atomic_add_fetch(&g_win_moves, 1u, __ATOMIC_RELAXED);
}

inline void NotePresent() {
    if (!g_enabled) return;
    __atomic_add_fetch(&g_frames, 1u, __ATOMIC_RELAXED);
    uint32_t start = g_pending_motion_ms;
    if (!start) return;
    uint32_t now = Timer::GetRealMs();
    uint32_t d = now >= start ? now - start : 0;
    if (d > g_evt2pres_max_ms) g_evt2pres_max_ms = d;
    g_pending_motion_ms = 0;
}

inline void MaybeLog(bool dragging) {
    if (!g_enabled) return;
    uint32_t now = Timer::GetRealMs();
    uint32_t win = g_window_ms;
    if (now - win < 1000u && now >= win) return;
    g_window_ms = now;
    uint32_t motion = __atomic_exchange_n(&g_motion, 0u, __ATOMIC_RELAXED);
    uint32_t damage = __atomic_exchange_n(&g_cursor_damage, 0u, __ATOMIC_RELAXED);
    uint32_t draw = __atomic_exchange_n(&g_cursor_draw, 0u, __ATOMIC_RELAXED);
    uint32_t moves = __atomic_exchange_n(&g_win_moves, 0u, __ATOMIC_RELAXED);
    uint32_t frames = __atomic_exchange_n(&g_frames, 0u, __ATOMIC_RELAXED);
    uint32_t late = g_evt2pres_max_ms;
    g_evt2pres_max_ms = 0;
    if (!motion && !damage && !draw && !moves && !frames) return;
    SerialLogger::Log("[uidiag] 1s motion=");
    SerialLogger::LogDec((int)motion);
    SerialLogger::Log(" cursor_damage=");
    SerialLogger::LogDec((int)damage);
    SerialLogger::Log(" cursor_draw=");
    SerialLogger::LogDec((int)draw);
    SerialLogger::Log(" win_moves=");
    SerialLogger::LogDec((int)moves);
    SerialLogger::Log(" frames=");
    SerialLogger::LogDec((int)frames);
    SerialLogger::Log(" evt2pres_max_ms=");
    SerialLogger::LogDec((int)late);
    SerialLogger::Log(dragging ? " drag=1" : " drag=0");
    SerialLogger::Log("\r\n");
}

} // namespace UiDiag

// end (satoru)
