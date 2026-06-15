# UDF, User Driver Framework

Kurono's answer to Windows UMDF: the **ring-3** tier of the hybrid kernel. A driver
that does not need ring 0 (a WiFi control plane, USB-HID, USB-storage, a printer)
runs as an ordinary kinit-managed Linux user process and reaches hardware **only**
through a thin kernel proxy. This is the strongest isolation Kurono offers, a real
CPL-3 address space, so a wild pointer or a crash cannot touch kernel memory at all
(unlike a KDF driver, which shares the kernel page tables and is only guard-fenced).
The tradeoff is a syscall round-trip per call (~10-15 µs), so performance-critical
drivers stay in ring 0 / KDF.

Source: `src/kernel/udf.{h,cpp}`, the `SYS_UDF_CALL` dispatch added surgically to
`src/linux/linux_syscall_x64.cpp`, and the proxy-death hook in
`src/system/kinit.cpp`.

## How it works

A ring-3 driver calls `udf_call(op, a0..a3)` through the **`SYS_UDF_CALL`** syscall
(number `0x4B554446`, "KUDF" in ASCII, well outside the Linux syscall space, so it
never collides). The kernel `UDFProxy`:

1. Validates user pointers (non-null, in the canonical lower half) before any
   dereference.
2. Handles **framework ops** directly: `UDF_OP_REGISTER` (announce a device class +
   name), `UDF_OP_UNREGISTER`, `UDF_OP_PING` (liveness), `UDF_OP_POLL` (pull the
   next queued request), `UDF_OP_COMPLETE` (post a completion).
3. Routes **class ops** to the registered proxy: a WiFi control op
   (`UDF_OP_WIFI_SCAN/CONNECT/STATUS`) is queued for the ring-3 driver to poll and
   service; a HID op (`UDF_OP_HID_REPORT`) is the kernel-side action performed
   immediately on the driver's behalf.

If the ring-3 driver dies, nothing in the kernel is corrupted: kinit's crash
monitor sees the process exit and calls `UDF::NotifyProxyDied(pid)`, which marks
the proxy dead so its class ops return `UDF_EDEAD` until it restarts and
re-registers.

## Device classes

`UDF_CLASS_WIFI`, `UDF_CLASS_HID`, `UDF_CLASS_STORAGE`, `UDF_CLASS_PRINTER`. A proxy
registers the class it serves; only ops in that class dispatch to it.

## Writing a ring-3 UDF driver (sketch)

```c
// announce: class = wifi, name = "wifi_udf"
syscall(0x4B554446, UDF_OP_REGISTER, UDF_CLASS_WIFI, (long)"wifi_udf", 0, 0);
for (;;) {
    unsigned long req[5];
    long r = syscall(0x4B554446, UDF_OP_POLL, (long)req, 0, 0, 0);
    if (r == UDF_OK) {
        // req[0]=op, req[1..4]=args; do the scan/assoc in ring 3...
        syscall(0x4B554446, UDF_OP_COMPLETE, 0, 0, 0, 0);
    }
    syscall(0x4B554446, UDF_OP_PING, 0, 0, 0, 0);   // heartbeat
    nanosleep(...);
}
```

The driver is then a kinit process unit (`.kservice` or `RegisterProcess`), so it
is started at its target, crash-restarted with backoff, and its proxy is marked
dead on exit.

## Status

`hwfw` (shell) prints the live proxy table (name, class, pid, alive, call count)
alongside KDF / IRP / KExec status.

## Honest scope / limits

- The framework, the `SYS_UDF_CALL` dispatch, the proxy registry, the request
  queue, and the kinit proxy-death bridge are **real and built**. No ring-3 UDF
  *driver* ships yet, `wifi_udf` and `usb_hid_udf` are the first candidates
  (WiFi has no QEMU hardware to test against; USB-HID is the more testable one).
- Wiring a normalized `UDF_OP_HID_REPORT` event into the kernel input dispatch is a
  documented follow-up (the input manager has no public synthetic-inject API yet);
  the op currently validates + counts + logs the event, proving the ring-3 → kernel
  control path round-trips.
- A per-request completion table (so a poster blocks for a specific async result)
  is a follow-up; today `UDF_OP_COMPLETE` acknowledges and the WiFi ops are
  fire-and-queue.
- Each `udf_call` adds the syscall round-trip (~10-15 µs); this is the cost of the
  ring-3 isolation and is why the perf-critical drivers stay in ring 0 / KDF.
