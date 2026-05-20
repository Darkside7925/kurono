//  kurono os  -  boot-time system update screen
//
//  shown immediately after kernel boot when /var/lib/kurono/pending-update
//  exists (written by `kpkg install debian` and similar long-running
//  provisioning steps that need a reboot to take effect).
//
//  the marker file is a tiny ini-style blob:
//      action=debian-install
//      gpu=nvidia        # or amd, none, auto
//      restart_to=desktop
//
//  on completion, the marker is deleted and the system continues booting
//  to the lockscreen.
#pragma once

namespace SystemUpdate {
    // returns true if a pending-update marker exists
    bool HasPendingUpdate();

    // creates the marker file with the given action + optional gpu hint.
    // call from kpkg install debian, etc.  the next boot will find the
    // marker and run RunPendingUpdate().
    bool QueueUpdate(const char* action, const char* gpu_hint);

    // run the pending update flow (full-screen ui, blocking).  clears
    // the marker on completion.  returns true on success.
    bool RunPendingUpdate();

    // hard reboot via 8042 reset line.
    void Reboot();
}
