#pragma once
//  kurono os - browser stub
//  no compatible bare-metal c++ browser exists on github.
//  netsurf/dillo/ladybird all require libc/posix/x11.
//  this stub replaces the old browser with a removed notice.
#include "../ui/window_manager.h"
#include "../kernel/types.h"

class KBrowse {
public:
    static void Open();
    static void Open(const char* url);
    static void Close();
    static bool IsOpen();
    static void OnRender(Window* w);
    static void OnInput(Window* w, int event, int a, int b);
    static int win_id;
};
typedef KBrowse BrowserApp;
