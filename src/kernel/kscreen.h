// kscreen: the full-screen error renderer shared by the kernel panic (red)
// and the driver error screen (blue). it draws straight into a linear
// framebuffer with its own primitives and the 8x16 vga font scaled up: no
// heap, no locks, no compositor, no gpu driver, so it works from a panic, on
// the firmware (GOP) framebuffer, or on the last scanout a dead display
// driver left behind. (satoru)
//
// look (recovery_PLAN.md 1.3): a large ":(" at the top left, one plain
// sentence, an optional short paragraph, a progress ring or a row of choice
// buttons or a countdown, and one small grey technical line with the stop
// code K-<AREA>-<NNNN>. blue = kurono is alive and a driver failed; red = the
// kernel itself failed and will restart. (satoru)
#pragma once
#include "types.h"

namespace KScreen {

struct Fb {
    uint64_t addr;     // a kernel-mapped linear framebuffer (satoru)
    uint32_t pitch, width, height;
    uint8_t  bpp;      // 16, 24 or 32 (satoru)
};

enum Tone : uint8_t { TONE_BLUE = 0, TONE_RED = 1 };

struct Screen {
    Tone        tone;
    const char* headline;        // one plain sentence (satoru)
    const char* body;            // optional, wrapped; "\n" starts a new paragraph (satoru)
    const char* code;            // "K-DRV-0104" (satoru)
    const char* tech;            // small grey details after the code (satoru)
    int         progress;        // < 0: none; else the spinner phase (satoru)
    const char* progress_text;   // "Restarting the driver, attempt 1 of 2" (satoru)
    const char* const* choices;  // button labels (satoru)
    int         nchoices;
    int         selected;        // keyboard focus among the choices (satoru)
    int         countdown;       // < 0: none; else "Restarting in N seconds" (satoru)
};

void InitScreen(Screen& s);
// draw the whole screen. cheap enough to call every 100 ms for the spinner. (satoru)
void Draw(const Fb& fb, const Screen& s);
// redraw only the spinner / countdown line (no full clear, no flicker). (satoru)
void DrawProgress(const Fb& fb, const Screen& s);

// a plain text console on the screen's colour: the last `n` lines, then the
// input line with a cursor. for the error screen's "Open terminal". (satoru)
void DrawConsole(const Fb& fb, Tone tone, const char* const* lines, int n, const char* input);

// the recovery screens: a calm dark page, a title, an optional one-line
// subtitle, and big rows (label + a grey second line); `sel` is the
// highlighted row, `hint` a small line at the bottom. (satoru)
struct MenuItem { const char* label; const char* sub; };
void DrawMenu(const Fb& fb, const char* title, const char* subtitle,
              const MenuItem* items, int n, int sel, const char* hint);

// "K-<area>-<nnnn>" with nnnn as 4 hex digits. out holds 16 bytes. (satoru)
void FormatStopCode(char* out, const char* area, uint32_t number);
// the stop code for a kernel panic code from panic.h: cpu exceptions are
// K-KRN-00<vector>, kernel subsystem codes map to MEM/SCH/DRV/KRN. (satoru)
void PanicStopCode(uint32_t stop_code, char* out);

}  // namespace KScreen

// end (satoru)
