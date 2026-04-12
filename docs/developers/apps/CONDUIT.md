# Conduit

`src/apps/conduit.cpp`, `conduit.h`, `src/system/conduit.cpp`, and `src/system/conduit.h` implement the Conduit app and its underlying bridge services.

## 1. What Conduit is

Conduit is Kurono's integration app for the embedded Linux guest. It provides a windowed terminal that connects to the Debian guest VM over the virtual serial link, a file browser that can access the guest filesystem via v9fs, and controls to start/stop the VM.

## 2. Architecture

Conduit sits above two layers:

- **`src/virt/vserial.cpp`**  -  virtual serial port connecting Kurono and the guest. Conduit reads and writes characters through this to drive the guest terminal.
- **`src/virt/v9fs.cpp`**  -  shared 9P filesystem protocol. The guest mounts a v9fs share that maps to a KVFS directory on the Kurono side, enabling file transfer.

## 3. Terminal rendering

The Conduit terminal renders the guest serial output in a scrollable view using the same font and GUI infrastructure as the main terminal app. Input typed in Conduit is forwarded to the guest over the virtual serial link.

## 4. File sharing

Files dropped into `/mnt/conduit` on the Kurono side appear at `/mnt/host` inside the guest. The v9fs protocol handles the translation. This is similar to VirtualBox shared folders.

## 5. Starting Conduit

From the start menu → Conduit, or from the shell:
```
kurono vm start
```

Conduit opens automatically when the VM starts.

## 6. Related files

- `src/virt/vserial.cpp`  -  virtual serial link
- `src/virt/v9fs.cpp`  -  shared filesystem
- `src/virt/hypervisor.cpp`  -  VM backend
- `src/linux/dual_boot.cpp`  -  guest Linux configuration
