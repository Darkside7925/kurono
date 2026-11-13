// key_layouts: keyboard layouts. the setting is ui.conf kbd.layout (an xkb
// layout name: us, gb, de, fr, es, it). two consumers:
//   - kernel text input (Keyboard::KeyToChar): an ascii table per layout for
//     the main key block, with altgr where the layout needs it for ascii
//     symbols (@ { [ ] } \ | ~ #). letters with accents have no single byte
//     form in the kernel's char path, so they are not typed there yet.
//   - wayland clients (firefox, gtk apps): the full compiled xkb keymap for
//     the layout (src/assets/keymaps/keymaps.bin, tools/keymaps/gen_keymaps.py),
//     so linux apps get every character of the layout. clients receive the
//     keymap when they bind the keyboard: a change applies to apps started
//     after it. (satoru)
#pragma once
#include "../kernel/types.h"

namespace KeyLayouts {

struct Layout { const char* id; const char* name; };
// the list the settings page offers (shared by the kernel and ring-3 builds). (satoru)
static const Layout kLayouts[] = {
    { "us", "English (US)" },
    { "gb", "English (UK)" },
    { "de", "German" },
    { "fr", "French" },
    { "es", "Spanish" },
    { "it", "Italian" },
};
static const int kLayoutCount = (int)(sizeof(kLayouts) / sizeof(kLayouts[0]));

#ifndef KUI_BACKEND_WAYLAND
// index into kLayouts of the active layout (re-read when ui.conf changes). (satoru)
int  Current();
// the ascii char for a key under the active layout. returns -1 when the
// layout has no opinion (the caller keeps its own us mapping), 0 for a key
// with no ascii form on this layout. (satoru)
int  MapChar(int key, bool shift, bool caps, bool altgr);
// the compiled xkb keymap for wayland clients, page aligned and nul
// terminated; len includes the nul. null means: use the built-in us one. (satoru)
const uint8_t* WaylandKeymap(uint32_t* len);
#endif

}  // namespace KeyLayouts

// end (satoru)
