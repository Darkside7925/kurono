#pragma once
//  kurono os - "Alpine" terminal: a vt100/xterm window on the alpine guest's
//  console (hvc0 of the background vm, see virt/vm_host.h). opening it starts
//  the vm when it is off. keys arrive as raw key events (like the pty mode of
//  the Terminal app) and are encoded as xterm sequences; the window size is
//  passed to the guest tty as a virtio console resize. (satoru)

class AlpineTerminal {
public:
    static int  Open();                                    // open or focus the window
    static void KeyEvent(int key, bool shift, bool ctrl, bool alt);   // input manager
    static bool IsOpen();
};

// end (satoru)
