// kui_kernel.h: the kernel side of libkurono-ui, the hooks the desktop calls (satoru)
#pragma once
#include <stdint.h>

namespace KuiKernel {
    // once per gui loop pass (DesktopEnvironment::Update): timers, frame requests, focus and (satoru)
    // pointer-leave edges, closed-window cleanup, theme changes, the ring-3 app install. (satoru)
    void Pump();
    // raw key edges from InputManager::Poll: specials (arrows, esc, tab, f-keys) and key ups for the (satoru)
    // focused kui window. printable keys come through the wm char event as before. (satoru)
    void KeyEvent(int key, bool pressed, bool shift, bool ctrl, bool alt);
    // kurono.r3apps=<list> boot flag: "1"/"all", or names like "calculator" (comma separated). (satoru)
    void SetRing3Apps(const char* list);
    // true when `name` should run as a ring-3 program (boot flag or ui.conf apps.ring3). (satoru)
    bool Ring3Wanted(const char* name);
    // start /system/apps/<name> in ring 3 through a detached launcher, or raise its window when it is (satoru)
    // already running. false = not wanted / not built / no launcher: open the in-kernel one instead. (satoru)
    bool LaunchRing3(const char* name);
    // the same, passing one validated argument (a path) to the new process; an already running (satoru)
    // instance does not swallow the request, a second instance is started for a different file. (satoru)
    bool LaunchRing3WithArg(const char* name, const char* arg);
    // publish /system/apps/*, the fonts, the icon art and the theme file for ring-3 kui programs (satoru)
    void InstallApps();
    // open a kui app the way the dock does (ring 3 if wanted, else in-kernel) (satoru)
    void Open(const char* name);
    // the same with one optional argument, e.g. the path a text editor should load (satoru)
    void OpenArg(const char* name, const char* arg);
    // open a file with the default app for its extension (satoru)
    bool OpenFile(const char* path);
    // run a .kcl script in a terminal, with the path checked against the script rules (satoru)
    bool RunScript(const char* path);
    // ask the desktop to rescan /home/user/Desktop (ring-3 Files after a change there) (satoru)
    void RefreshDesktop();
    // kurono.kui.open=<name>: the test harness opens it once, 4 s after the desktop is up (satoru)
    void SetTestOpen(const char* name);
}

// the `kapp <name>` shell command (registered in shell.cpp): runs /system/apps/<name> as a ring-3 (satoru)
// linux process and waits for it, like the other ring-3 commands. (satoru)
class KuronoShell;
namespace ShellBuiltins { int cmd_kapp(KuronoShell* sh, int argc, const char** argv, char* out, int mx); }

// end (satoru)
