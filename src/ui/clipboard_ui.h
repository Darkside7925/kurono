#pragma once
//  kurono os - clipboard history popup (super+v)
//
//  a small frameless window listing the clipboard manager's recent text
//  entries. clicking a row (or enter on the selected row) puts it back on the
//  clipboard; escape closes. the popup restores the window that had focus. (satoru)

namespace ClipboardUI {
    void Toggle();                                  // super+v: open or close (satoru)
    bool IsOpen();
    void Close();
    // key while the popup has focus. returns true when it consumed the key. (satoru)
    bool KeyEvent(int key, bool shift, bool ctrl, bool alt);
}

// end (satoru)
