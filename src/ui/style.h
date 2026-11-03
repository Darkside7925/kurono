#pragma once

//  kurono ui style: "modern" (smooth inter text, top bar + floating dock,
//  gradient squircle icons) or "classic" (the original 8x16 pixel font and flat
//  taskbar). chosen by ui.style in /etc/kurono/ui.conf, switchable live from
//  settings; Apply() pushes the choice into every subsystem that draws
//  differently. (satoru)

namespace UIStyle {

bool Modern();

// re-read ui.style and apply it. called at desktop init and on config reload.
void Apply();

// persist + apply a new style ("modern" / "classic").
void Set(bool modern);

// modern icon set: colourful gradients, or minimal monochrome line icons.
// ui.icons = "colorful" / "minimal". (satoru)
bool MinimalIcons();
void SetMinimalIcons(bool minimal);

}
