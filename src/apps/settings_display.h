#pragma once
//  kurono os - settings > displays: the two hooks the rest of the system calls (satoru)
//  the page itself lives in settings_mod_display.cpp. these let the boot path
//  re-apply the saved arrangement and let the gui loop draw the identify
//  numbers and run the "keep these settings?" countdown even when the
//  settings window is not being redrawn. (satoru)

namespace DisplaySettings {
    // kernel init, after multi-output is up and before the desktop is laid out:
    // re-apply the saved arrangement / primary / on-off when the same outputs
    // are present. no-op on a single output. (satoru)
    void RestoreAtBoot();
    // gui loop, once per composed frame after the hud and before the cursor. (satoru)
    void Overlay();
}
// end (satoru)
