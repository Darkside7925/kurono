#ifndef KURONO_SYSTEM_DBUS_SERVER_H
#define KURONO_SYSTEM_DBUS_SERVER_H

#include "../kernel/types.h"

// DBus session bus daemon.
//
// Listens at /system/run/user/1000/bus (the canonical session bus) and
// speaks the DBus 1 wire protocol per the spec at
// https://dbus.freedesktop.org/doc/dbus-specification.html
//
// Authentication: SASL EXTERNAL (the only mechanism we support).
// Wire format: little-endian by default; header is fixed-size + variable
// header fields.
//
// Implemented interfaces:
//   org.freedesktop.DBus              Hello, AddMatch, RemoveMatch,
//                                     ListNames, RequestName, ReleaseName,
//                                     NameHasOwner, GetNameOwner
//   org.freedesktop.Notifications     Notify, CloseNotification, GetCapabilities
//   org.freedesktop.ScreenSaver       Inhibit, UnInhibit
//   org.freedesktop.portal.Desktop    OpenURI (no-op success)
//
// Method calls to unknown destinations get an error reply, but the
// connection is preserved so libdbus stays happy.

namespace DBusServer {

    static const int DBUS_MAX_CLIENTS = 32;
    static const int DBUS_MAX_NAMES   = 64;
    static const int DBUS_NAME_LEN    = 96;

    void Init();

    int  ListenSd();
    int  ClientCount();
    int  RegisteredNameCount();

    // Allow other kernel modules to broadcast a signal.
    void EmitSignal(const char* path, const char* iface, const char* member);
}

#endif
