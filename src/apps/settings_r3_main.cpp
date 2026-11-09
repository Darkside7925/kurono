// settings_r3_main.cpp: the ring-3 entry point of Settings. the shell itself is the
// kernel's own system_settings.cpp compiled with KUI_BACKEND_WAYLAND. (satoru)
#include "system_settings.h"
#include "../kui/kui.h"
#include "../kui/ring3/kui_r3_settings.h"

int main(int, char**){
    char page[32];
    page[0] = 0;
    int pr = kui::Fs::ReadFile("/system/run/kui/settings-page", page, (int)sizeof(page) - 1);
    if(pr > 0){
        if(pr >= (int)sizeof(page)) pr = (int)sizeof(page) - 1;
        page[pr] = 0;
        int n = 0;
        while(page[n] && page[n] != '\n' && page[n] != '\r') n++;
        page[n] = 0;
        kui::Fs::Remove("/system/run/kui/settings-page");
    }
    SettingsBridge::Refresh();
    if(page[0]) SystemSettings::OpenPage(page);
    else SystemSettings::Open();
    return kui::RunLoop();
}

// end (satoru)
