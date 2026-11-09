// settings_service.h: the privileged settings service a ring-3 Settings talks to. (satoru)
// it owns the ui.conf keys, validates every write, applies live side effects on the gui
// thread, and sends privileged operations through AdminGate. (satoru)
#pragma once

namespace SettingsService {
    // lazy init: create the unix socket, publish the wallpaper thumbnails. safe to call often. (satoru)
    void Init();
    // once per gui loop pass: apply queued actions. cheap when the queue is empty. (satoru)
    void Pump();
}

// end (satoru)
