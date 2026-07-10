#pragma once
#include "../kernel/types.h"

class LockScreen {
public:
    static void Show();   // blocks until login succeeds
    enum State {
        // Login flow
        IDLE,            // big clock, tap-to-login
        FADE_IN,         // login UI fading up
        LOGIN,           // username/password entry
        SHAKE,           // wrong password animation
        FADE_OUT,        // success transition
        // Registration wizard
        WIZ_PROFILE,     // step 1
        WIZ_SECURITY,    // step 2
        WIZ_PREFS,       // step 3
        WIZ_SUMMARY      // step 4
    };
    static State current_state;
};
