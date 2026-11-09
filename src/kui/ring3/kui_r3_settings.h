// kui_r3_settings.h: the settings service client the ring-3 Settings main uses. (satoru)
#pragma once

namespace SettingsBridge {
    // fetch the config and system info snapshots from the kernel settings service. (satoru)
    bool Refresh();
    bool Loaded();
    // the kvfs node count the storage page shows; ring 3 cannot walk the tree. (satoru)
    int NodeCount();
}

// end (satoru)
